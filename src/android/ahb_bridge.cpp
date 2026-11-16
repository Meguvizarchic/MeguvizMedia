#include <meguviz/media_android.h>
#include "../descriptor.h"
#include "fence_probe_status.hpp"
#include "retirement_queue.hpp"
#include "thread_identity.hpp"
#include "creation_pool.hpp"
#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <thread>
#include <vector>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <cerrno>
#include <cstdint>
#include <limits>

namespace {
using CreationPool=MgmAndroidCreationPool<>;
std::atomic<CreationPool*> creation_pool{nullptr};
std::atomic_flag creation_pool_initializing=ATOMIC_FLAG_INIT;
alignas(CreationPool) unsigned char creation_pool_storage[sizeof(CreationPool)];
}

// Capture owns its GL state only between begin() and finish(). External
// callbacks execute outside that interval. GL error checks consume flags;
// pre-existing errors are rejected before changing caller state. State recovery
// is not a physical-retirement proof for the submitted GPU work.
struct CheckedGlStateReport {
    static constexpr unsigned max_errors=16;
    GLenum preexisting[max_errors]{};
    GLenum operation[max_errors]{};
    GLenum body[max_errors]{};
    GLenum restoration[max_errors]{};
    unsigned preexisting_count=0,operation_count=0,body_count=0,restoration_count=0;
    bool queue_overflow=false,context_changed=false,context_lost=false,out_of_memory=false;
    bool queue_read=false,active_switched=false,snapshot_complete=false;
    bool mutation_attempted=false,restore_attempted=false,restore_succeeded=false;
};

class CheckedGlState {
public:
    CheckedGlState(EGLDisplay display,EGLContext context):display_(display),context_(context){}
    CheckedGlState(const CheckedGlState&)=delete;
    CheckedGlState& operator=(const CheckedGlState&)=delete;
    const CheckedGlStateReport& report()const{return report_;}
    bool unsafe()const{return report_.context_lost||report_.context_changed||report_.out_of_memory||report_.queue_overflow;}

    // Reject pre-existing error flags before poll can consume them. This is
    // observably destructive to the GL error queue, not an error-preserving API.
    MgcStatus preflight(){
        if(used_||!same_context()){report_.context_changed=!same_context();return MGC_INVALID_ARGUMENT;}
        const auto status=drain(report_.preexisting,report_.preexisting_count);
        if(!same_context()){report_.context_changed=true;return MGC_INTERNAL_ERROR;}
        if(status==MGC_OUT_OF_MEMORY)return status;
        return status!=MGC_SUCCESS||report_.preexisting_count?MGC_INTERNAL_ERROR:MGC_SUCCESS;
    }

    // Called before/after client-memory operations. External callbacks must
    // run outside the transaction; a changed context makes it unusable.
    MgcStatus checkpoint(){
        if(!ready_||finished_)return MGC_INVALID_ARGUMENT;
        if(!same_context()){report_.context_changed=true;return MGC_INTERNAL_ERROR;}
        auto status=drain(report_.body,report_.body_count);
        if(!same_context()){report_.context_changed=true;return MGC_INTERNAL_ERROR;}
        return status;
    }

    MgcStatus begin(){
        if(used_)return MGC_INVALID_ARGUMENT;used_=true;
        if(!same_context()){report_.context_changed=true;return MGC_INVALID_ARGUMENT;}
        auto status=drain(report_.preexisting,report_.preexisting_count);
        if(status!=MGC_SUCCESS || report_.preexisting_count){
            // The errors belong to the caller, not to a newly executed bridge
            // command. Reject rather than attribute or silently discard them.
            return MGC_INTERNAL_ERROR;
        }
        if((status=get_object(GL_DRAW_FRAMEBUFFER_BINDING,draw_fbo_))!=MGC_SUCCESS)return status;
        if((status=get_object(GL_READ_FRAMEBUFFER_BINDING,read_fbo_))!=MGC_SUCCESS)return status;
        if((status=get_object(GL_CURRENT_PROGRAM,program_))!=MGC_SUCCESS)return status;
        if((status=get_object(GL_VERTEX_ARRAY_BINDING,vao_))!=MGC_SUCCESS)return status;
        if((status=get(GL_ACTIVE_TEXTURE,active_))!=MGC_SUCCESS)return status;
        if((status=get(GL_VIEWPORT,viewport_))!=MGC_SUCCESS)return status;
        if((status=get(GL_PACK_ALIGNMENT,pack_))!=MGC_SUCCESS)return status;
        if(pack_!=1&&pack_!=2&&pack_!=4&&pack_!=8)return MGC_INTERNAL_ERROR;
        if((status=get(GL_PACK_ROW_LENGTH,pack_row_))!=MGC_SUCCESS)return status;
        if((status=get(GL_PACK_SKIP_ROWS,pack_rows_))!=MGC_SUCCESS)return status;
        if((status=get(GL_PACK_SKIP_PIXELS,pack_pixels_))!=MGC_SUCCESS)return status;
        if((status=get_object(GL_PIXEL_PACK_BUFFER_BINDING,pack_buffer_))!=MGC_SUCCESS)return status;
        if((status=get_object(GL_PIXEL_UNPACK_BUFFER_BINDING,unpack_buffer_))!=MGC_SUCCESS)return status;
        for(auto &value:color_mask_)value=0xff;
        glGetBooleanv(GL_COLOR_WRITEMASK,color_mask_);if((status=operation_result())!=MGC_SUCCESS)return status;
        for(auto value:color_mask_)if(value!=GL_FALSE&&value!=GL_TRUE)return MGC_INTERNAL_ERROR;
        if((status=enabled(GL_BLEND,blend_))!=MGC_SUCCESS)return status;
        if((status=enabled(GL_SCISSOR_TEST,scissor_))!=MGC_SUCCESS)return status;
        if((status=enabled(GL_DEPTH_TEST,depth_))!=MGC_SUCCESS)return status;
        if((status=enabled(GL_CULL_FACE,cull_))!=MGC_SUCCESS)return status;
        if((status=enabled(GL_RASTERIZER_DISCARD,raster_))!=MGC_SUCCESS)return status;
        if((status=enabled(GL_STENCIL_TEST,stencil_))!=MGC_SUCCESS)return status;
        if((status=enabled(GL_DITHER,dither_))!=MGC_SUCCESS)return status;
        if((status=enabled(GL_SAMPLE_COVERAGE,coverage_))!=MGC_SUCCESS)return status;
        if((status=enabled(GL_SAMPLE_ALPHA_TO_COVERAGE,alpha_coverage_))!=MGC_SUCCESS)return status;
        if(active_!=GL_TEXTURE0){
            glActiveTexture(GL_TEXTURE0);status=operation_result();
            if(status!=MGC_SUCCESS)return status;
            report_.active_switched=true;
        }
        if((status=get_object(GL_TEXTURE_BINDING_2D,texture2d_))!=MGC_SUCCESS)return rollback_active(status);
        if((status=get_object(GL_TEXTURE_BINDING_EXTERNAL_OES,texture_oes_))!=MGC_SUCCESS)return rollback_active(status);
        if((status=get_object(GL_SAMPLER_BINDING,sampler_))!=MGC_SUCCESS)return rollback_active(status);
        report_.snapshot_complete=true;
        for(GLenum capability:{GL_BLEND,GL_SCISSOR_TEST,GL_DEPTH_TEST,GL_CULL_FACE,GL_RASTERIZER_DISCARD,GL_STENCIL_TEST,GL_SAMPLE_COVERAGE,GL_SAMPLE_ALPHA_TO_COVERAGE}){
            report_.mutation_attempted=true;glDisable(capability);status=operation_result();
            if(status!=MGC_SUCCESS){
                if(unsafe()){report_.context_changed|=!same_context();return status;}
                const auto restored=finish();return restored==MGC_SUCCESS?status:restored;
            }
        }
        auto changed=[&](){report_.mutation_attempted=true;auto result=operation_result();if(result!=MGC_SUCCESS&&!unsafe()&&same_context())finish();return result;};
        glBindBuffer(GL_PIXEL_PACK_BUFFER,0);if((status=changed())!=MGC_SUCCESS)return status;
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);if((status=changed())!=MGC_SUCCESS)return status;
        glPixelStorei(GL_PACK_ALIGNMENT,1);if((status=changed())!=MGC_SUCCESS)return status;
        glPixelStorei(GL_PACK_ROW_LENGTH,0);if((status=changed())!=MGC_SUCCESS)return status;
        glPixelStorei(GL_PACK_SKIP_ROWS,0);if((status=changed())!=MGC_SUCCESS)return status;
        glPixelStorei(GL_PACK_SKIP_PIXELS,0);if((status=changed())!=MGC_SUCCESS)return status;
        glBindSampler(0,0);if((status=changed())!=MGC_SUCCESS)return status;
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);if((status=changed())!=MGC_SUCCESS)return status;
        ready_=true;return MGC_SUCCESS;
    }

    MgcStatus finish(){
        if(!report_.snapshot_complete||finished_)return MGC_INVALID_ARGUMENT;
        finished_=true;ready_=false;
        if(unsafe()||!same_context()){report_.context_changed|=!same_context();return MGC_INTERNAL_ERROR;}
        // Errors raised by the body must be attributed before issuing any
        // restore command. OOM or a lost context makes the GL state unknown.
        const auto body_status=drain(report_.body,report_.body_count);
        if(!same_context()){report_.context_changed=true;return MGC_INTERNAL_ERROR;}
        if(unsafe())return body_status==MGC_SUCCESS?MGC_INTERNAL_ERROR:body_status;
        report_.restore_attempted=true;
        MgcStatus result=MGC_SUCCESS;
        auto check=[&](){auto status=restoration_result();if(status!=MGC_SUCCESS&&result==MGC_SUCCESS)result=status;return !unsafe();};
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,GLuint(draw_fbo_));if(!check())return result;
        glBindFramebuffer(GL_READ_FRAMEBUFFER,GLuint(read_fbo_));if(!check())return result;
        glUseProgram(GLuint(program_));if(!check())return result;
        glBindVertexArray(GLuint(vao_));if(!check())return result;
        glActiveTexture(GL_TEXTURE0);if(!check())return result;
        glBindTexture(GL_TEXTURE_2D,GLuint(texture2d_));if(!check())return result;
        glBindTexture(GL_TEXTURE_EXTERNAL_OES,GLuint(texture_oes_));if(!check())return result;
        glBindSampler(0,GLuint(sampler_));if(!check())return result;
        glActiveTexture(GLenum(active_));if(!check())return result;
        glViewport(viewport_[0],viewport_[1],viewport_[2],viewport_[3]);if(!check())return result;
        glPixelStorei(GL_PACK_ALIGNMENT,pack_);if(!check())return result;
        glPixelStorei(GL_PACK_ROW_LENGTH,pack_row_);if(!check())return result;
        glPixelStorei(GL_PACK_SKIP_ROWS,pack_rows_);if(!check())return result;
        glPixelStorei(GL_PACK_SKIP_PIXELS,pack_pixels_);if(!check())return result;
        glBindBuffer(GL_PIXEL_PACK_BUFFER,GLuint(pack_buffer_));if(!check())return result;
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER,GLuint(unpack_buffer_));if(!check())return result;
        glColorMask(color_mask_[0],color_mask_[1],color_mask_[2],color_mask_[3]);if(!check())return result;
        const GLenum flags[]={GL_BLEND,GL_SCISSOR_TEST,GL_DEPTH_TEST,GL_CULL_FACE,GL_RASTERIZER_DISCARD,GL_STENCIL_TEST,GL_DITHER,GL_SAMPLE_COVERAGE,GL_SAMPLE_ALPHA_TO_COVERAGE};
        const GLboolean old[]={blend_,scissor_,depth_,cull_,raster_,stencil_,dither_,coverage_,alpha_coverage_};
        for(unsigned i=0;i<9;++i){if(old[i])glEnable(flags[i]);else glDisable(flags[i]);if(!check())return result;}
        report_.restore_succeeded=result==MGC_SUCCESS&&same_context();
        if(!report_.restore_succeeded&&result==MGC_SUCCESS)result=MGC_INTERNAL_ERROR;
        return result!=MGC_SUCCESS?result:body_status;
    }

    bool ready()const{return ready_;}
private:
    EGLDisplay display_;EGLContext context_;
    GLuint draw_fbo_=0,read_fbo_=0,program_=0,vao_=0,texture2d_=0,texture_oes_=0,pack_buffer_=0,unpack_buffer_=0,sampler_=0;
    GLint active_=0,viewport_[4]{},pack_=0;
    GLint pack_row_=0,pack_rows_=0,pack_pixels_=0;
    GLboolean color_mask_[4]{};
    GLboolean blend_=GL_FALSE,scissor_=GL_FALSE,depth_=GL_FALSE,cull_=GL_FALSE,raster_=GL_FALSE,stencil_=GL_FALSE,dither_=GL_FALSE,coverage_=GL_FALSE,alpha_coverage_=GL_FALSE;
    bool used_=false,ready_=false,finished_=false;
    CheckedGlStateReport report_{};
    bool same_context()const{return display_!=EGL_NO_DISPLAY&&context_!=EGL_NO_CONTEXT&&eglGetCurrentDisplay()==display_&&eglGetCurrentContext()==context_;}
    MgcStatus drain(GLenum (&buffer)[CheckedGlStateReport::max_errors],unsigned& count){
        report_.queue_read=true;bool saw_oom=false,saw_error=false;
        for(unsigned i=0;i<CheckedGlStateReport::max_errors;++i){
            const auto error=glGetError();if(error==GL_NO_ERROR)return saw_oom?MGC_OUT_OF_MEMORY:saw_error?MGC_INTERNAL_ERROR:MGC_SUCCESS;
            saw_error=true;if(count<CheckedGlStateReport::max_errors)buffer[count++]=error;else report_.queue_overflow=true;
            if(error==GL_OUT_OF_MEMORY){report_.out_of_memory=true;saw_oom=true;}
            if(error==GLenum(0x0507))report_.context_lost=true;
        }
        report_.queue_overflow=true;return saw_oom?MGC_OUT_OF_MEMORY:MGC_INTERNAL_ERROR;
    }
    MgcStatus operation_result(){
        auto result=drain(report_.operation,report_.operation_count);
        if(!same_context()){report_.context_changed=true;return MGC_INTERNAL_ERROR;}return result;
    }
    MgcStatus restoration_result(){
        auto result=drain(report_.restoration,report_.restoration_count);
        if(!same_context()){report_.context_changed=true;return MGC_INTERNAL_ERROR;}return result;
    }
    MgcStatus get_object(GLenum name,GLuint& value){GLint64 wide=-1;glGetInteger64v(name,&wide);auto status=operation_result();if(status!=MGC_SUCCESS)return status;if(wide<0||uint64_t(wide)>std::numeric_limits<GLuint>::max())return MGC_INTERNAL_ERROR;value=GLuint(wide);return MGC_SUCCESS;}
    MgcStatus get(GLenum name,GLint& value){value=-1;glGetIntegerv(name,&value);auto status=operation_result();return status==MGC_SUCCESS&&value<0?MGC_INTERNAL_ERROR:status;}
    MgcStatus get(GLenum name,GLint (&values)[4]){values[2]=values[3]=-1;glGetIntegerv(name,values);auto status=operation_result();return status==MGC_SUCCESS&&(values[2]<0||values[3]<0)?MGC_INTERNAL_ERROR:status;}
    MgcStatus enabled(GLenum name,GLboolean& value){value=glIsEnabled(name);auto status=operation_result();return status==MGC_SUCCESS&&value!=GL_FALSE&&value!=GL_TRUE?MGC_INTERNAL_ERROR:status;}
    MgcStatus rollback_active(MgcStatus cause){
        if(!report_.active_switched)return cause;
        if(unsafe()||!same_context()){report_.context_changed|=!same_context();return cause;}
        report_.restore_attempted=true;glActiveTexture(GLenum(active_));const auto status=restoration_result();
        report_.restore_succeeded=status==MGC_SUCCESS&&same_context();return status==MGC_SUCCESS?cause:status;
    }
};


namespace {
int64_t now_ns(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return int64_t(t.tv_sec)*1000000000+t.tv_nsec;}
bool extension(const char *all,const char *name){if(!all)return false;size_t n=std::strlen(name);for(auto p=std::strstr(all,name);p;p=std::strstr(p+n,name))if((p==all||p[-1]==' ')&&(p[n]==0||p[n]==' '))return true;return false;}
std::atomic<uint64_t> next_allocation{uint64_t(1)<<48},next_content{1};
struct Slot {
    std::atomic<uint32_t> pins{0};AHardwareBuffer *buffer=nullptr;EGLImageKHR image=EGL_NO_IMAGE_KHR;GLuint texture=0,fbo=0;
    uint64_t allocation=0,generation=0,content=0;std::vector<uint8_t> static_grid,scene_grid,duplicate_grid;
    MgcResource *resource=nullptr;MgcLease *pending_producer=nullptr;uint64_t capacity=0;
    int producer_retirement=-1,resource_retirement=-1;
};
void retain_slot(void *p){static_cast<Slot*>(p)->pins.fetch_add(1);}
void release_slot(void *p){static_cast<Slot*>(p)->pins.fetch_sub(1);}
struct Read {MgmFrame *frame;EGLSyncKHR fence;GLuint texture;EGLImageKHR image;bool reservation_open=true,submitted=false;MgcLease *aborted_write=nullptr;bool cleanup_uncertain=false;int owned_fd=-1;bool reservation_unknown=false;int retirement=-1;MgmEvent retired_event{};bool retired_event_present=false,gl_retired=false;MgcLeaseInfo retired_identity{};bool retired_identity_valid=false;};
/* This adapter is intended for a dedicated shared GL context. Restore touched
 * state as well, because source adapters often issue their own OES operations. */
struct GlState {
    GLint drawFbo=0,readFbo=0,program=0,vao=0,active=0,texture2d=0,textureOes=0,viewport[4]{},pack=0;GLboolean blend=0,scissor=0,depth=0,cull=0;
    bool* uncertain;EGLDisplay display;EGLContext context;bool finished=false,snapshot_complete=false;
    explicit GlState(bool* outcome=nullptr):uncertain(outcome),display(eglGetCurrentDisplay()),context(eglGetCurrentContext()){glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&drawFbo);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&readFbo);glGetIntegerv(GL_CURRENT_PROGRAM,&program);glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glActiveTexture(GL_TEXTURE0);glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture2d);glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES,&textureOes);glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_PACK_ALIGNMENT,&pack);blend=glIsEnabled(GL_BLEND);scissor=glIsEnabled(GL_SCISSOR_TEST);depth=glIsEnabled(GL_DEPTH_TEST);cull=glIsEnabled(GL_CULL_FACE);if(uncertain&&glGetError()!=GL_NO_ERROR){*uncertain=true;return;}snapshot_complete=true;glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);if(uncertain&&glGetError()!=GL_NO_ERROR)*uncertain=true;}
    bool finish(){if(finished)return !uncertain||!*uncertain;finished=true;if(uncertain&&*uncertain)return false;if(!snapshot_complete||eglGetCurrentDisplay()!=display||eglGetCurrentContext()!=context){if(uncertain)*uncertain=true;return false;}glBindFramebuffer(GL_DRAW_FRAMEBUFFER,drawFbo);glBindFramebuffer(GL_READ_FRAMEBUFFER,readFbo);glUseProgram(program);glBindVertexArray(vao);glBindTexture(GL_TEXTURE_2D,texture2d);glBindTexture(GL_TEXTURE_EXTERNAL_OES,textureOes);glActiveTexture(active);glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);glPixelStorei(GL_PACK_ALIGNMENT,pack);if(blend)glEnable(GL_BLEND);if(scissor)glEnable(GL_SCISSOR_TEST);if(depth)glEnable(GL_DEPTH_TEST);if(cull)glEnable(GL_CULL_FACE);if(uncertain&&glGetError()!=GL_NO_ERROR)*uncertain=true;return !uncertain||!*uncertain;}
    ~GlState(){(void)finish();}
};
bool shader(GLenum type,const char *source,GLuint& id,bool& uncertain){
    id=glCreateShader(type);if(!id){if(glGetError()!=GL_NO_ERROR)uncertain=true;return false;}
    glShaderSource(id,1,&source,nullptr);glCompileShader(id);GLint ok=0;glGetShaderiv(id,GL_COMPILE_STATUS,&ok);
    if(glGetError()!=GL_NO_ERROR){uncertain=true;return false;}return ok!=0;
}
bool program(const char *fragment,int mode,GLuint& vertex_shader,GLuint& fragment_shader,GLuint& output,bool& uncertain){
    const char *vertex=mode==1?
        "#version 300 es\nprecision highp float;out highp vec2 uv;out highp vec2 transformed;uniform mat4 transform;uniform int flipY;"
        "void main(){vec2 p=vec2(float(gl_VertexID&1),float(gl_VertexID>>1));uv=vec2(p.x,flipY!=0?1.0-p.y:p.y);"
        "transformed=(transform*vec4(uv,0.0,1.0)).xy;gl_Position=vec4(p*2.0-1.0,0,1);}":mode==2?
        "#version 300 es\nprecision highp float;out vec2 uv;uniform int flipY;void main(){vec2 p=vec2(float(gl_VertexID&1),float(gl_VertexID>>1));uv=vec2(p.x,flipY!=0?1.0-p.y:p.y);gl_Position=vec4(p*2.0-1.0,0,1);}":
        "#version 300 es\nprecision highp float;out vec2 uv;void main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));uv=p;gl_Position=vec4(p*2.0-1.0,0,1);}";
    if(!shader(GL_VERTEX_SHADER,vertex,vertex_shader,uncertain)||!shader(GL_FRAGMENT_SHADER,fragment,fragment_shader,uncertain))return false;
    output=glCreateProgram();if(!output){if(glGetError()!=GL_NO_ERROR)uncertain=true;return false;}
    glAttachShader(output,vertex_shader);glAttachShader(output,fragment_shader);glLinkProgram(output);
    GLint ok=0;glGetProgramiv(output,GL_LINK_STATUS,&ok);
    if(glGetError()!=GL_NO_ERROR){uncertain=true;return false;}if(!ok)return false;
    glDeleteShader(vertex_shader);if(glGetError()!=GL_NO_ERROR){uncertain=true;return false;}vertex_shader=0;
    glDeleteShader(fragment_shader);if(glGetError()!=GL_NO_ERROR){uncertain=true;return false;}fragment_shader=0;
    return true;
}
}
struct MgmAndroidSession {
    enum class ProbePhase:uint8_t { SampleImport,SampleRelease,SampleWait,SampleAcquire,StorageAllocate,StorageRelease,StorageWait,StorageAcquire,Capabilities,Done,Lost,Stuck };
    MgmAndroidSessionDesc desc{};EGLDisplay display=EGL_NO_DISPLAY;EGLContext context=EGL_NO_CONTEXT;std::thread::id thread;uint64_t thread_token=0;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC client=nullptr;PFNEGLCREATEIMAGEKHRPROC create_image=nullptr;PFNEGLDESTROYIMAGEKHRPROC destroy_image=nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target_image=nullptr;PFNEGLCREATESYNCKHRPROC create_sync=nullptr;PFNEGLDESTROYSYNCKHRPROC destroy_sync=nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC client_wait=nullptr;PFNEGLWAITSYNCKHRPROC wait_sync=nullptr;PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence=nullptr;
    GLuint capture=0,display_program=0,sample=0,vao=0,sample_texture=0,sample_fbo=0,vertex_shader=0,fragment_shader=0;bool native_fence=false,gl_cleanup_uncertain=false,poll_active=false;EGLSyncKHR uncertain_sync=EGL_NO_SYNC_KHR;
    bool usable()const{return !gl_cleanup_uncertain;}
    bool operation_active=false,closing=false,domain_released=false;
    int domain_retirement=-1;
    CreationPool::Entry* creation_entry=nullptr;
    bool initializing=false;MgcStatus initialization_status=MGC_BUSY;uint64_t initialization_token=0;
    MgcDomainStateInfo initialization_domain{};
    MgmRetirementQueue retirement;
    std::vector<std::unique_ptr<Slot>> slots;std::vector<Read> pending;std::vector<MgcLease*> unused_reservations;
    // Probe resources must outlive failures and incomplete Vulkan work. The
    // fixed slot remains owned by this session until its final retirement.
    Slot probe_slot;bool probe_attempted=false,staged_probe_ready=false,probe_delivered=false;
    MgmAndroidProbe staged_probe{};MgcStatus staged_probe_status=MGC_BUSY;
    ProbePhase staged_probe_phase=ProbePhase::SampleImport;MgcLease* staged_probe_lease=nullptr;
    int staged_probe_fd=-1;bool staged_probe_external=false;
    bool current()const{return thread_token&&thread_token==mgm_thread_token()&&thread==std::this_thread::get_id()&&display==eglGetCurrentDisplay()&&context==eglGetCurrentContext();}
    MgmEvent make_event(uint32_t kind,MgmFrame *frame,uint64_t source=0){MgmEvent e{};e.header=MGC_HEADER_INIT(MgmEvent);e.session_id=desc.session_id;e.epoch=desc.epoch;e.source_id=source;e.kind=kind;e.timestamp_ns=now_ns();e.time_domain=MGM_CLOCK_CPU_MONOTONIC;if(frame){MgmFrameDesc f{};f.header=MGC_HEADER_INIT(MgmFrameDesc);MgcLeaseInfo l{};l.header=MGC_HEADER_INIT(MgcLeaseInfo);if(!mgm_frame_describe(frame,&f)&&!mgc_lease_info(f.content,&l)){e.epoch=f.epoch;e.task_id=f.task_id;if(!e.source_id)e.source_id=f.source_id;e.content_id=l.content_id;e.allocation_id=l.allocation_id;e.allocation_generation=l.allocation_generation;e.domain_id=l.domain_id;e.domain_generation=l.domain_generation;e.frame_kind=f.frame_kind;}}return e;}
    void emit_event(MgmEvent e){if(!desc.event)return;e.timestamp_ns=now_ns();try{desc.event(desc.event_context,&e);}catch(...){/* C callbacks must not unwind into the bridge. */}}
    void event(uint32_t kind,MgmFrame *frame,uint64_t source=0){if(desc.event)emit_event(make_event(kind,frame,source));}
    // Final close runs only after real GL fences and background resource
    // releases. If deletion's effect is unknown, retain all remaining handles
    // and never repeat it as if failure proved no side effect.
    MgcStatus cleanup_slot_final(Slot& slot){
        if(slot.resource)return MGC_INTERNAL_ERROR;
        if(slot.fbo){glDeleteFramebuffers(1,&slot.fbo);if(glGetError()!=GL_NO_ERROR){gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}slot.fbo=0;}
        if(slot.texture){glDeleteTextures(1,&slot.texture);if(glGetError()!=GL_NO_ERROR){gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}slot.texture=0;}
        if(slot.image!=EGL_NO_IMAGE_KHR){if(!destroy_image(display,slot.image)){gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}slot.image=EGL_NO_IMAGE_KHR;}
        if(slot.buffer){AHardwareBuffer_release(slot.buffer);slot.buffer=nullptr;}return MGC_SUCCESS;
    }
    MgcStatus cleanup_objects_final(){
        for(auto* program:{&capture,&display_program,&sample})if(*program){glDeleteProgram(*program);if(glGetError()!=GL_NO_ERROR){gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}*program=0;}
        for(auto* shader:{&vertex_shader,&fragment_shader})if(*shader){glDeleteShader(*shader);if(glGetError()!=GL_NO_ERROR){gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}*shader=0;}
        if(vao){glDeleteVertexArrays(1,&vao);if(glGetError()!=GL_NO_ERROR){gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}vao=0;}
        if(sample_texture){glDeleteTextures(1,&sample_texture);if(glGetError()!=GL_NO_ERROR){gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}sample_texture=0;}
        if(sample_fbo){glDeleteFramebuffers(1,&sample_fbo);if(glGetError()!=GL_NO_ERROR){gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}sample_fbo=0;}
        return MGC_SUCCESS;
    }
    MgcStatus cleanup_read(Read &r){
        if(r.cleanup_uncertain)return MGC_INTERNAL_ERROR;
        if(r.fence!=EGL_NO_SYNC_KHR){if(!destroy_sync(display,r.fence)){r.cleanup_uncertain=gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}r.fence=EGL_NO_SYNC_KHR;}
        if(r.texture){glDeleteTextures(1,&r.texture);if(glGetError()!=GL_NO_ERROR){r.cleanup_uncertain=gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}r.texture=0;}
        if(r.image!=EGL_NO_IMAGE_KHR){if(!destroy_image(display,r.image)){r.cleanup_uncertain=gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}r.image=EGL_NO_IMAGE_KHR;}
        return MGC_SUCCESS;
    }
    MgcStatus image(AHardwareBuffer *buffer,EGLImageKHR &out,GLuint &texture,bool retain_on_error=false){const EGLint attributes[]={EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};out=create_image(display,EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,client(buffer),attributes);if(out==EGL_NO_IMAGE_KHR)return MGC_UNSUPPORTED_CAPABILITY;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);target_image(GL_TEXTURE_2D,out);const auto error=glGetError();if(error!=GL_NO_ERROR){if(retain_on_error||error==GL_OUT_OF_MEMORY||error==GLenum(0x0507)){gl_cleanup_uncertain=true;return error==GL_OUT_OF_MEMORY?MGC_OUT_OF_MEMORY:MGC_INTERNAL_ERROR;}return MGC_UNSUPPORTED_CAPABILITY;}return MGC_SUCCESS;}
    MgcStatus allocate(Slot &s){AHardwareBuffer_Desc d{};d.width=desc.width;d.height=desc.height;d.layers=1;d.format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;d.usage=AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE|AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
        // Partial GL/EGL objects stay attached to the session. In particular,
        // never release the backing AHB after an unverified image deletion.
        if(AHardwareBuffer_allocate(&d,&s.buffer))return MGC_OUT_OF_MEMORY;auto status=image(s.buffer,s.image,s.texture,true);if(status)return status;glGenFramebuffers(1,&s.fbo);glBindFramebuffer(GL_FRAMEBUFFER,s.fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s.texture,0);const auto completeness=glCheckFramebufferStatus(GL_FRAMEBUFFER);if(glGetError()!=GL_NO_ERROR){gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}if(completeness!=GL_FRAMEBUFFER_COMPLETE)return MGC_UNSUPPORTED_CAPABILITY;s.allocation=next_allocation.fetch_add(1);s.generation=1;return MGC_SUCCESS;}
    MgcStatus register_slot(Slot &slot){
        if(slot.resource)return MGC_SUCCESS;auto status=slot.buffer?MGC_SUCCESS:allocate(slot);if(status)return status;
        MgcAndroidImportDesc imported{};imported.header=MGC_HEADER_INIT(MgcAndroidImportDesc);imported.buffer=slot.buffer;imported.acquire_fence_fd=-1;imported.allocation_id=0;imported.allocation_generation=slot.generation;imported.content_id=next_content.fetch_add(1);imported.access=MGC_ACCESS_WRITE;imported.sync_mode=desc.path==MGM_ANDROID_AHB_GPU_SYNC?MGC_ANDROID_GPU_SYNC_FD:MGC_ANDROID_HOST_WAIT_COMPAT;
        MgcLease *bootstrap=nullptr;status=mgc_android_import(desc.domain,&imported,&bootstrap);if(status)return status;MgcLeaseInfo info{};info.header=MGC_HEADER_INIT(MgcLeaseInfo);status=mgc_lease_info(bootstrap,&info);if(!status)status=mgc_lease_resource(bootstrap,&slot.resource);if(!status){slot.capacity=info.capacity_bytes;slot.allocation=info.allocation_id;slot.generation=info.allocation_generation;}mgc_lease_release(bootstrap);return status;
    }
    MgcStatus poll_producers(bool defer=false){MgcStatus waiting=MGC_SUCCESS;for(auto &slot:slots){
        if(slot->producer_retirement>=0){if(!retirement.completed(slot->producer_retirement)){waiting=MGC_BUSY;continue;}
            retirement.consume(slot->producer_retirement);slot->producer_retirement=-1;}
        if(slot->pending_producer){auto status=mgc_android_wait_ready(slot->pending_producer,0);if(!current()){gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}
            if(status==MGC_TIMEOUT||status==MGC_BUSY){waiting=status;continue;}if(status)return status;
            if(defer){const auto index=retirement.enqueue(nullptr,slot->pending_producer,nullptr,true);if(index<0){waiting=MGC_BUSY;continue;}
                slot->pending_producer=nullptr;slot->producer_retirement=index;waiting=MGC_BUSY;}
            else{mgc_lease_release(slot->pending_producer);slot->pending_producer=nullptr;if(!current()){gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}}}}
        return waiting;}
    MgcStatus sample_grid(Slot &slot,uint32_t w,uint32_t h,int mode,std::vector<uint8_t> &bytes,CheckedGlState &state){
        if(!state.ready())return MGC_INTERNAL_ERROR;
        glBindTexture(GL_TEXTURE_2D,sample_texture);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER,sample_fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,sample_texture,0);
        const auto framebuffer=glCheckFramebufferStatus(GL_FRAMEBUFFER);auto status=state.checkpoint();if(status)return status;
        if(framebuffer!=GL_FRAMEBUFFER_COMPLETE)return MGC_UNSUPPORTED_CAPABILITY;
        glViewport(0,0,w,h);glUseProgram(sample);glUniform1i(glGetUniformLocation(sample,"mode"),mode);
        glUniform2i(glGetUniformLocation(sample,"extent"),desc.width,desc.height);glUniform2i(glGetUniformLocation(sample,"grid"),w,h);
        glBindTexture(GL_TEXTURE_2D,slot.texture);glDrawArrays(GL_TRIANGLES,0,3);
        status=state.checkpoint();if(status)return status;
        bytes.resize(size_t(w)*h*4);
        // PACK PBO/row/skip and UNPACK PBO are normalized by the transaction;
        // no external callback runs between its begin and this client read.
        glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,bytes.data());return state.checkpoint();
    }
};
struct SessionOperation {
    MgmAndroidSession* session;
    explicit SessionOperation(MgmAndroidSession* s):session(s->operation_active?nullptr:s){if(session)session->operation_active=true;}
    ~SessionOperation(){if(session)session->operation_active=false;}
    explicit operator bool()const{return session!=nullptr;}
    void disarm(){session=nullptr;}
};
// The GL owner has already created and checked temporary's AHB/EGL/FBO. The
// Vulkan portion may block and must execute on the background worker for a
// staged session. Failed external reservations remain owned by the session.
static MgcStatus android_probe_compute(MgmAndroidSession* s,CreationPool::Entry& entry,uint64_t token){
    using Phase=MgmAndroidSession::ProbePhase;auto& phase=s->staged_probe_phase;auto& p=s->staged_probe;
    const auto cancelled=[&]{return entry.gate.load(std::memory_order_acquire)!=token;};
    const auto error=[&](MgcStatus status)->MgcStatus{
        if(status==MGC_BUSY||status==MGC_TIMEOUT)return status;
        if(status==MGC_DEVICE_LOST&&s->staged_probe_lease){phase=Phase::Lost;return MGC_BUSY;}
        if(s->staged_probe_lease){phase=Phase::Stuck;s->gl_cleanup_uncertain=true;}
        return status;
    };
    for(unsigned steps=0;steps<16;++steps){
        if(phase==Phase::Stuck)return MGC_INTERNAL_ERROR;
        if(phase==Phase::Lost){MgcDomainStateInfo state{};state.header=MGC_HEADER_INIT(MgcDomainStateInfo);auto status=mgc_domain_state(s->desc.domain,&state);
            if(status)return status;if(!state.lost||!state.device_retired)return MGC_BUSY;
            if(s->staged_probe_external){status=mgc_android_set_acquire_fence(s->staged_probe_lease,-1);if(status){if(status!=MGC_BUSY&&status!=MGC_TIMEOUT){phase=Phase::Stuck;s->gl_cleanup_uncertain=true;}return status;}s->staged_probe_external=false;}
            if(s->staged_probe_fd>=0){close(s->staged_probe_fd);s->staged_probe_fd=-1;}
            mgc_lease_release(s->staged_probe_lease);s->staged_probe_lease=nullptr;return MGC_DEVICE_LOST;}
        if(phase==Phase::SampleImport){if(cancelled())return MGC_CANCELLED;
            AHardwareBuffer_Desc d{};AHardwareBuffer_describe(s->probe_slot.buffer,&d);p.actual_width=d.width;p.actual_height=d.height;p.actual_stride=d.stride;p.actual_format=d.format;p.actual_usage=d.usage;
            MgcAndroidImportDesc imp{};imp.header=MGC_HEADER_INIT(MgcAndroidImportDesc);imp.buffer=s->probe_slot.buffer;imp.acquire_fence_fd=-1;imp.content_id=next_content.fetch_add(1);imp.access=MGC_ACCESS_READ;imp.sync_mode=s->desc.path==MGM_ANDROID_AHB_GPU_SYNC?MGC_ANDROID_GPU_SYNC_FD:MGC_ANDROID_HOST_WAIT_COMPAT;
            auto status=mgc_android_import(s->desc.domain,&imp,&s->staged_probe_lease);p.vulkan_sampleable=status;if(status)return error(status);if(!s->staged_probe_lease){phase=Phase::Stuck;s->gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}phase=Phase::SampleRelease;continue;}
        if(phase==Phase::StorageAllocate){if(cancelled())return MGC_CANCELLED;
            MgcAndroidAllocateDesc alloc{};alloc.header=MGC_HEADER_INIT(MgcAndroidAllocateDesc);alloc.width=s->desc.width;alloc.height=s->desc.height;alloc.content_id=next_content.fetch_add(1);alloc.access=MGC_ACCESS_WRITE;alloc.sync_mode=s->desc.path==MGM_ANDROID_AHB_GPU_SYNC?MGC_ANDROID_GPU_SYNC_FD:MGC_ANDROID_HOST_WAIT_COMPAT;
            auto status=mgc_android_allocate(s->desc.domain,&alloc,&s->staged_probe_lease);p.vulkan_storage=status;if(status)return error(status);if(!s->staged_probe_lease){phase=Phase::Stuck;s->gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}phase=Phase::StorageRelease;continue;}
        if(phase==Phase::SampleRelease||phase==Phase::StorageRelease){int32_t fd=-1;auto status=mgc_android_release_to_external(s->staged_probe_lease,&fd);
            if(status){if(fd>=0){s->staged_probe_fd=fd;phase=Phase::Stuck;s->gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}return error(status);}s->staged_probe_fd=fd;s->staged_probe_external=true;
            phase=phase==Phase::SampleRelease?Phase::SampleWait:Phase::StorageWait;continue;}
        if(phase==Phase::SampleWait||phase==Phase::StorageWait){if(s->staged_probe_fd>=0){pollfd fd{s->staged_probe_fd,POLLIN,0};const auto ready=::poll(&fd,1,0);
                if(ready==0||(ready<0&&errno==EINTR))return MGC_BUSY;if(ready<0||!(fd.revents&POLLIN)||(fd.revents&(POLLERR|POLLNVAL))){phase=Phase::Stuck;s->gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}
                close(s->staged_probe_fd);s->staged_probe_fd=-1;}
            phase=phase==Phase::SampleWait?Phase::SampleAcquire:Phase::StorageAcquire;continue;}
        if(phase==Phase::SampleAcquire||phase==Phase::StorageAcquire){auto status=mgc_android_set_acquire_fence(s->staged_probe_lease,-1);if(status)return error(status);
            s->staged_probe_external=false;mgc_lease_release(s->staged_probe_lease);s->staged_probe_lease=nullptr;
            if(cancelled())return MGC_CANCELLED;phase=phase==Phase::SampleAcquire?Phase::StorageAllocate:Phase::Capabilities;continue;}
        if(phase==Phase::Capabilities){if(cancelled())return MGC_CANCELLED;
            for(uint32_t feature:{MGC_CAP_SYNC_FD_IMPORT,MGC_CAP_SYNC_FD_EXPORT}){MgcCapability c{};c.header=MGC_HEADER_INIT(MgcCapability);auto result=mgc_domain_capability(s->desc.domain,feature,&c);auto v=mgm_fence_probe_status(result,c,s->native_fence);if(feature==MGC_CAP_SYNC_FD_IMPORT)p.native_fence_import=v;else p.native_fence_export=v;}
            auto status=mgm_fence_overall(MGC_SUCCESS,s->desc.path,p.native_fence_import,p.native_fence_export);if(status)return status;p.workload_qualified=0;phase=Phase::Done;return MGC_SUCCESS;}
        if(phase==Phase::Done)return MGC_SUCCESS;
    }
    return MGC_BUSY;
}
extern "C" {
static MgcStatus android_poll_impl(MgmAndroidSession *s,uint64_t timeout);
static MgcStatus android_session_close_impl(MgmAndroidSession *s);
MgcStatus mgm_android_creation_prepare(void){
    auto* pool=creation_pool.load(std::memory_order_acquire);
    if(!pool){
        if(creation_pool_initializing.test_and_set(std::memory_order_acquire))return MGC_BUSY;
        pool=creation_pool.load(std::memory_order_acquire);
        if(!pool){pool=new(static_cast<void*>(creation_pool_storage)) CreationPool;
            creation_pool.store(pool,std::memory_order_release);}
        creation_pool_initializing.clear(std::memory_order_release);
    }
    if(pool->hasReady())return MGC_SUCCESS;
    return pool->prepare(reinterpret_cast<const void*>(&mgm_android_session_create),"meguviz_code_image_anchor_v1");
}
MgcStatus mgm_android_creation_reserve(uint64_t* token){
    if(!token)return MGC_INVALID_ARGUMENT;*token=0;
    const auto status=mgm_android_creation_prepare();if(status)return status;
    auto* pool=creation_pool.load(std::memory_order_acquire);return pool?pool->reserve(token):MGC_BUSY;
}
MgcStatus mgm_android_creation_release_reservation(uint64_t token){
    if(!token)return MGC_INVALID_ARGUMENT;auto* pool=creation_pool.load(std::memory_order_acquire);
    return pool?pool->releaseReservation(token):MGC_SUCCESS;
}
MgcStatus mgm_android_creation_cancel_import(uint64_t token){
    if(!token||token>=CreationPool::COMMITTED)return MGC_INVALID_ARGUMENT;
    auto* pool=creation_pool.load(std::memory_order_acquire);return pool?pool->cancel(token):MGC_SUCCESS;
}
MgcStatus mgm_android_creation_service_background(void){
    auto* pool=creation_pool.load(std::memory_order_acquire);if(!pool)return MGC_SUCCESS;
    MgcStatus result=MGC_SUCCESS;
    for(auto& entry:pool->entries){
        if(!CreationPool::claim(entry,CreationPool::State::Orphan,CreationPool::State::BackendBusy))continue;
        auto* session=static_cast<MgmAndroidSession*>(entry.owner);
        const auto status=session?mgm_android_service_retirement(session):MGC_INTERNAL_ERROR;
        CreationPool::finish(entry,CreationPool::State::Orphan);
        if(status!=MGC_SUCCESS&&status!=MGC_BUSY&&status!=MGC_TIMEOUT)result=status;
    }
    const auto collected=pool->collect();
    return result!=MGC_SUCCESS?result:collected;
}
MgcStatus mgm_android_creation_discard_ready(void){
    auto* pool=creation_pool.load(std::memory_order_acquire);if(!pool)return MGC_SUCCESS;
    pool->discardReady();return pool->collect();
}
MgcStatus mgm_android_creation_service_owner(void){
    auto* pool=creation_pool.load(std::memory_order_acquire);if(!pool)return MGC_SUCCESS;
    const auto token=mgm_thread_token();if(!token)return MGC_BUSY;
    MgcStatus result=MGC_SUCCESS;
    for(auto& entry:pool->entries){
        if(!CreationPool::claim(entry,CreationPool::State::Orphan,CreationPool::State::OwnerBusy)){
            const auto state=entry.state.load(std::memory_order_acquire);
            if((state==CreationPool::State::Orphan||state==CreationPool::State::BackendBusy||state==CreationPool::State::OwnerBusy)&&
               entry.owner_token.load(std::memory_order_relaxed)==token&&result==MGC_SUCCESS)result=MGC_BUSY;
            continue;
        }
        auto* session=static_cast<MgmAndroidSession*>(entry.owner);
        if(!session||!session->current()) {
            if(entry.owner_token.load(std::memory_order_relaxed)==token&&result==MGC_SUCCESS)result=MGC_BUSY;
            CreationPool::finish(entry,CreationPool::State::Orphan);continue;
        }
        const auto polled=mgm_android_poll(session,0);
        const auto closed=android_session_close_impl(session);
        CreationPool::finish(entry,closed==MGC_SUCCESS?CreationPool::State::Retired:CreationPool::State::Orphan);
        if(closed!=MGC_SUCCESS&&closed!=MGC_BUSY&&closed!=MGC_TIMEOUT)result=closed;
        else if(closed!=MGC_SUCCESS&&result==MGC_SUCCESS)result=polled==MGC_TIMEOUT?MGC_TIMEOUT:MGC_BUSY;
    }
    return result;
}
static MgcStatus android_session_create_impl(const MgmAndroidSessionDesc *source,uint64_t reservation,bool deferred,MgmAndroidSession **out){
    if(out)*out=nullptr;if(!out)return MGC_INVALID_ARGUMENT;MgmAndroidSessionDesc d{};auto status=mgm_detail::copy_desc(source,offsetof(MgmAndroidSessionDesc,reserved),d);if(status)return status;if(d.required_flags)return MGC_UNSUPPORTED_ABI;
    if(!d.domain||!d.epoch||!d.width||!d.height||d.width>8192||d.height>8192||d.pool_capacity<3||d.pool_capacity>12||(d.path!=MGM_ANDROID_AHB_HOST_WAIT&&d.path!=MGM_ANDROID_AHB_GPU_SYNC)||!mgm_detail::zero(d.reserved))return MGC_INVALID_ARGUMENT;
    if(d.transform_flags&~MGM_ANDROID_CANONICAL_TOP_DOWN)return MGC_UNSUPPORTED_ABI;
    if(!d.source_width&&!d.source_height){d.source_width=d.width;d.source_height=d.height;}
    if(!d.source_width||!d.source_height||d.source_width>8192||d.source_height>8192||d.reserved_tail)return MGC_INVALID_ARGUMENT;
    auto s=std::unique_ptr<MgmAndroidSession>(new(std::nothrow)MgmAndroidSession);if(!s)return MGC_OUT_OF_MEMORY;s->desc=d;s->display=eglGetCurrentDisplay();s->context=eglGetCurrentContext();s->thread=std::this_thread::get_id();s->thread_token=mgm_thread_token();if(!s->thread_token)return MGC_BACKPRESSURE;if(s->display==EGL_NO_DISPLAY||s->context==EGL_NO_CONTEXT)return MGC_INVALID_ARGUMENT;
#define MGM_LOAD(member,type,name) s->member=reinterpret_cast<type>(eglGetProcAddress(name))
    MGM_LOAD(client,PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC,"eglGetNativeClientBufferANDROID");MGM_LOAD(create_image,PFNEGLCREATEIMAGEKHRPROC,"eglCreateImageKHR");MGM_LOAD(destroy_image,PFNEGLDESTROYIMAGEKHRPROC,"eglDestroyImageKHR");MGM_LOAD(target_image,PFNGLEGLIMAGETARGETTEXTURE2DOESPROC,"glEGLImageTargetTexture2DOES");MGM_LOAD(create_sync,PFNEGLCREATESYNCKHRPROC,"eglCreateSyncKHR");MGM_LOAD(destroy_sync,PFNEGLDESTROYSYNCKHRPROC,"eglDestroySyncKHR");MGM_LOAD(client_wait,PFNEGLCLIENTWAITSYNCKHRPROC,"eglClientWaitSyncKHR");MGM_LOAD(wait_sync,PFNEGLWAITSYNCKHRPROC,"eglWaitSyncKHR");MGM_LOAD(dup_fence,PFNEGLDUPNATIVEFENCEFDANDROIDPROC,"eglDupNativeFenceFDANDROID");
#undef MGM_LOAD
    const char *ext=eglQueryString(s->display,EGL_EXTENSIONS);if(!extension(ext,"EGL_ANDROID_image_native_buffer")||!extension(ext,"EGL_KHR_fence_sync")||!s->client||!s->create_image||!s->destroy_image||!s->target_image||!s->create_sync||!s->destroy_sync||!s->client_wait)return MGC_UNSUPPORTED_CAPABILITY;
    s->native_fence=extension(ext,"EGL_ANDROID_native_fence_sync")&&s->dup_fence&&s->wait_sync;
    if(d.path==MGM_ANDROID_AHB_GPU_SYNC&&!s->native_fence)return MGC_UNSUPPORTED_CAPABILITY;
    auto* pool=creation_pool.load(std::memory_order_acquire);
    if(!pool)return MGC_BUSY;
    s->creation_entry=reservation?pool->acquireReserved(s.get(),s->thread_token,
        reinterpret_cast<uintptr_t>(s->display),reinterpret_cast<uintptr_t>(s->context),reservation):
        pool->acquire(s.get(),s->thread_token,reinterpret_cast<uintptr_t>(s->display),reinterpret_cast<uintptr_t>(s->context));
    if(!s->creation_entry)return MGC_BUSY;
    // From this point even partial shader/slot creation is reachable from the
    // bounded pool and the domain and library image remain independently held.
    mgc_domain_retain(d.domain);
    auto abandon=[&](MgcStatus error){s->desc.event=nullptr;s->desc.event_context=nullptr;
        CreationPool::orphan(*s->creation_entry);s.release();return error;};
    if(deferred){s->initializing=true;s->initialization_token=reservation;s->initialization_domain.header=MGC_HEADER_INIT(MgcDomainStateInfo);
        const auto observed=mgc_domain_state(d.domain,&s->initialization_domain);if(observed)return abandon(observed);
        if(s->initialization_domain.lost)return abandon(MGC_DEVICE_LOST);}
    const auto preexisting=glGetError();if(preexisting!=GL_NO_ERROR){if(preexisting==GLenum(0x0507))s->gl_cleanup_uncertain=true;return abandon(MGC_INTERNAL_ERROR);}
    GlState state(&s->gl_cleanup_uncertain);if(s->gl_cleanup_uncertain)return abandon(MGC_INTERNAL_ERROR);glGenVertexArrays(1,&s->vao);glBindVertexArray(s->vao);
    if(glGetError()!=GL_NO_ERROR){s->gl_cleanup_uncertain=true;return abandon(MGC_INTERNAL_ERROR);}
    if(!program("#version 300 es\n#extension GL_OES_EGL_image_external_essl3 : require\n"
        "precision mediump float;in highp vec2 uv;in highp vec2 transformed;uniform samplerExternalOES tex;uniform highp mat4 transform;"
        "uniform highp vec2 offset;uniform int downscale;out vec4 color;"
        "highp vec2 at(highp vec2 c){return (transform*vec4(c,0.0,1.0)).xy;}"
        "void main(){highp vec2 base=uv;"
        "if(downscale!=0)color=0.25*(texture(tex,at(base+vec2(-offset.x,-offset.y)))"
        "+texture(tex,at(base+vec2(offset.x,-offset.y)))+texture(tex,at(base+vec2(-offset.x,offset.y)))"
        "+texture(tex,at(base+offset)));else color=texture(tex,transformed);}",1,s->vertex_shader,s->fragment_shader,s->capture,s->gl_cleanup_uncertain))return abandon(s->gl_cleanup_uncertain?MGC_INTERNAL_ERROR:MGC_UNSUPPORTED_CAPABILITY);
    if(!program("#version 300 es\nprecision highp float;in vec2 uv;uniform sampler2D tex;out vec4 color;void main(){color=texture(tex,uv);}",2,s->vertex_shader,s->fragment_shader,s->display_program,s->gl_cleanup_uncertain))return abandon(s->gl_cleanup_uncertain?MGC_INTERNAL_ERROR:MGC_UNSUPPORTED_CAPABILITY);
    if(!program("#version 300 es\nprecision highp float;precision highp int;uniform sampler2D tex;uniform int mode;uniform ivec2 extent,grid;out vec4 color;void main(){ivec2 g=ivec2(gl_FragCoord.xy);ivec2 p=mode==0?g*max(4,extent.x/320):(mode==1?((g*2+1)*extent)/(grid*2):g*ivec2(max(1,extent.x/320),max(1,extent.y/180)));color=texelFetch(tex,min(p,extent-1),0);}",0,s->vertex_shader,s->fragment_shader,s->sample,s->gl_cleanup_uncertain))return abandon(s->gl_cleanup_uncertain?MGC_INTERNAL_ERROR:MGC_UNSUPPORTED_CAPABILITY);
    glGenTextures(1,&s->sample_texture);glGenFramebuffers(1,&s->sample_fbo);
    if(glGetError()!=GL_NO_ERROR){s->gl_cleanup_uncertain=true;return abandon(MGC_INTERNAL_ERROR);}
    try{s->slots.reserve(d.pool_capacity);s->pending.reserve(2);s->unused_reservations.reserve(2);for(uint32_t i=0;i<d.pool_capacity;++i){
        if(deferred&&s->creation_entry->gate.load(std::memory_order_acquire)!=reservation)return abandon(MGC_CANCELLED);
        s->slots.emplace_back(new Slot);status=deferred?s->allocate(*s->slots.back()):s->register_slot(*s->slots.back());if(status)return abandon(status);}
        if(deferred){
            if(s->creation_entry->gate.load(std::memory_order_acquire)!=reservation)return abandon(MGC_CANCELLED);
            s->probe_attempted=true;s->staged_probe.header=MGC_HEADER_INIT(MgmAndroidProbe);
            s->staged_probe.egl_renderable=s->staged_probe.egl_sampleable=s->staged_probe.vulkan_sampleable=
                s->staged_probe.vulkan_storage=s->staged_probe.native_fence_import=s->staged_probe.native_fence_export=MGC_UNSUPPORTED_CAPABILITY;
            status=s->allocate(s->probe_slot);s->staged_probe.egl_renderable=s->staged_probe.egl_sampleable=status;
            if(status)return abandon(status);
        }
    }catch(...){return abandon(MGC_OUT_OF_MEMORY);}
    if(!state.finish())return abandon(MGC_INTERNAL_ERROR);
    if(!deferred&&reservation){status=CreationPool::commit(*s->creation_entry,reservation);if(status)return abandon(status);}
    *out=s.release();return deferred?MGC_ACCEPTED:MGC_SUCCESS;
}
MgcStatus mgm_android_session_create(const MgmAndroidSessionDesc* source,MgmAndroidSession** out){return android_session_create_impl(source,0,false,out);}
MgcStatus mgm_android_session_create_reserved(const MgmAndroidSessionDesc* source,uint64_t reservation,MgmAndroidSession** out){
    if(!reservation){if(out)*out=nullptr;return MGC_INVALID_ARGUMENT;}return android_session_create_impl(source,reservation,false,out);
}
MgcStatus mgm_android_session_begin_reserved(const MgmAndroidSessionDesc* source,uint64_t reservation,MgmAndroidSession** out){
    if(!reservation){if(out)*out=nullptr;return MGC_INVALID_ARGUMENT;}return android_session_create_impl(source,reservation,true,out);
}
MgcStatus mgm_android_creation_import_background(uint64_t reservation){
    if(!reservation||reservation>=CreationPool::COMMITTED)return MGC_INVALID_ARGUMENT;
    auto* pool=creation_pool.load(std::memory_order_acquire);if(!pool)return MGC_CANCELLED;
    for(auto& entry:pool->entries){
        if(entry.reservation.load(std::memory_order_acquire)!=reservation)continue;
        if(!CreationPool::claim(entry,CreationPool::State::Active,CreationPool::State::BackendBusy))return MGC_BUSY;
        if(entry.reservation.load(std::memory_order_relaxed)!=reservation){CreationPool::finish(entry,CreationPool::State::Active);continue;}
        auto* s=static_cast<MgmAndroidSession*>(entry.owner);MgcStatus status=MGC_INTERNAL_ERROR;
        const auto thread=mgm_thread_token();
        if(!s||!thread)status=MGC_BUSY;
        else if(s->thread_token==thread)status=MGC_INCOMPATIBLE_DOMAIN;
        else if(!s->initializing)status=MGC_INVALID_ARGUMENT;
        else if(s->initialization_status!=MGC_BUSY)status=s->initialization_status;
        else {
            auto observed=[&]()->MgcStatus{MgcDomainStateInfo current{};current.header=MGC_HEADER_INIT(MgcDomainStateInfo);
                auto result=mgc_domain_state(s->desc.domain,&current);if(result)return result;
                if(current.lost)return MGC_DEVICE_LOST;
                if(current.domain_id!=s->initialization_domain.domain_id||current.domain_generation!=s->initialization_domain.domain_generation||
                   current.device_epoch!=s->initialization_domain.device_epoch)return MGC_INCOMPATIBLE_DOMAIN;
                return MGC_SUCCESS;};
            status=observed();
            try{
                if(status==MGC_DEVICE_LOST&&s->staged_probe_lease){s->staged_probe_phase=MgmAndroidSession::ProbePhase::Lost;status=android_probe_compute(s,entry,reservation);}
                else if(status&&s->staged_probe_lease){s->gl_cleanup_uncertain=true;s->staged_probe_phase=MgmAndroidSession::ProbePhase::Stuck;}
                else if(!status&&s->staged_probe_lease)status=android_probe_compute(s,entry,reservation);
                else if(!status){
                    for(auto& slot:s->slots){
                        if(entry.gate.load(std::memory_order_acquire)!=reservation){status=MGC_CANCELLED;break;}
                        status=s->register_slot(*slot);if(status)break;status=observed();if(status)break;
                    }
                    if(!status)status=android_probe_compute(s,entry,reservation);
                }
                s->staged_probe_status=status;
                if(!status){status=observed();if(!status)s->staged_probe_ready=true;}
            }catch(...){s->gl_cleanup_uncertain=true;status=MGC_INTERNAL_ERROR;}
            if(!status&&entry.gate.load(std::memory_order_acquire)!=reservation)status=MGC_CANCELLED;
            if(status!=MGC_BUSY&&status!=MGC_TIMEOUT)s->initialization_status=status;
        }
        CreationPool::finish(entry,CreationPool::State::Active);return status;
    }
    return MGC_CANCELLED;
}
MgcStatus mgm_android_session_commit_initialization(MgmAndroidSession* s){
    if(!s||!s->current()||!s->creation_entry)return MGC_INVALID_ARGUMENT;
    auto* entry=s->creation_entry;
    if(!CreationPool::claim(*entry,CreationPool::State::Active,CreationPool::State::OwnerBusy))return MGC_BUSY;
    SessionOperation operation(s);if(!operation){CreationPool::finish(*entry,CreationPool::State::Active);return MGC_BUSY;}
    MgcStatus result=MGC_SUCCESS;
    if(!s->initializing)result=MGC_INVALID_ARGUMENT;
    else if(s->closing)result=MGC_BUSY;
    else if(s->initialization_status!=MGC_SUCCESS)result=s->initialization_status;
    else if(!s->staged_probe_ready)result=MGC_BUSY;
    else if(!s->usable())result=MGC_INTERNAL_ERROR;
    else{
        MgcDomainStateInfo current{};current.header=MGC_HEADER_INIT(MgcDomainStateInfo);result=mgc_domain_state(s->desc.domain,&current);
        if(!result&&current.lost)result=MGC_DEVICE_LOST;
        if(!result&&(current.domain_id!=s->initialization_domain.domain_id||current.domain_generation!=s->initialization_domain.domain_generation||
                     current.device_epoch!=s->initialization_domain.device_epoch))result=MGC_INCOMPATIBLE_DOMAIN;
        if(!result)for(const auto& slot:s->slots)if(!slot->resource){result=MGC_INTERNAL_ERROR;break;}
        if(!result){result=CreationPool::commit(*entry,s->initialization_token);if(!result)s->initializing=false;}
    }
    CreationPool::finish(*entry,CreationPool::State::Active);return result;
}
MgcStatus mgm_android_probe(MgmAndroidSession *s,MgmAndroidProbe *out){
    if(!s||!s->current())return MGC_INVALID_ARGUMENT;SessionOperation operation(s);if(!operation||s->closing||s->initializing)return MGC_BUSY;if(!s->usable())return MGC_INTERNAL_ERROR;if(!s->unused_reservations.empty())return MGC_BUSY;MgmAndroidProbe p{};uint32_t size=0;auto status=mgm_detail::copy_desc(out,offsetof(MgmAndroidProbe,required_flags)+4,p,&size);if(status)return status;if(p.required_flags)return MGC_UNSUPPORTED_ABI;
    if(s->staged_probe_ready){
        if(s->probe_delivered)return MGC_BUSY;
        MgcDomainStateInfo current{};current.header=MGC_HEADER_INIT(MgcDomainStateInfo);status=mgc_domain_state(s->desc.domain,&current);
        if(status)return status;if(current.lost)return MGC_DEVICE_LOST;
        if(current.domain_id!=s->initialization_domain.domain_id||current.domain_generation!=s->initialization_domain.domain_generation||
           current.device_epoch!=s->initialization_domain.device_epoch)return MGC_INCOMPATIBLE_DOMAIN;
        s->probe_delivered=true;mgm_detail::write_desc(out,s->staged_probe,size);return s->staged_probe_status;
    }
    p.egl_renderable=p.egl_sampleable=p.vulkan_sampleable=p.vulkan_storage=p.native_fence_import=p.native_fence_export=MGC_UNSUPPORTED_CAPABILITY;
    // One bounded probe slot per session. Repeating after an attempt requires a
    // fresh session; the old probe allocation cannot safely be recycled before
    // the domain, GL commands and any exported fence have all retired.
    if(s->probe_attempted)return MGC_BUSY;s->probe_attempted=true;
    GlState state(&s->gl_cleanup_uncertain);if(s->gl_cleanup_uncertain)return MGC_INTERNAL_ERROR;Slot &temporary=s->probe_slot;status=s->allocate(temporary);p.egl_renderable=p.egl_sampleable=status;AHardwareBuffer_Desc d{};
    if(!status){AHardwareBuffer_describe(temporary.buffer,&d);p.actual_width=d.width;p.actual_height=d.height;p.actual_stride=d.stride;p.actual_format=d.format;p.actual_usage=d.usage;MgcAndroidImportDesc imp{};imp.header=MGC_HEADER_INIT(MgcAndroidImportDesc);imp.buffer=temporary.buffer;imp.acquire_fence_fd=-1;imp.content_id=next_content.fetch_add(1);imp.access=MGC_ACCESS_READ;imp.sync_mode=s->desc.path==MGM_ANDROID_AHB_GPU_SYNC?MGC_ANDROID_GPU_SYNC_FD:MGC_ANDROID_HOST_WAIT_COMPAT;MgcLease *lease=nullptr;status=mgc_android_import(s->desc.domain,&imp,&lease);p.vulkan_sampleable=status;int32_t fd=-1;if(!status){status=mgc_android_release_to_external(lease,&fd);if(fd>=0)close(fd);if(!status){status=mgc_android_set_acquire_fence(lease,-1);if(status){s->unused_reservations.push_back(lease);lease=nullptr;}}mgc_lease_release(lease);}p.vulkan_storage=MGC_UNSUPPORTED_CAPABILITY;MgcAndroidAllocateDesc alloc{};alloc.header=MGC_HEADER_INIT(MgcAndroidAllocateDesc);alloc.width=d.width;alloc.height=d.height;alloc.content_id=next_content.fetch_add(1);alloc.access=MGC_ACCESS_WRITE;alloc.sync_mode=imp.sync_mode;lease=nullptr;auto storage=mgc_android_allocate(s->desc.domain,&alloc,&lease);p.vulkan_storage=storage;if(!storage){int32_t release=-1;storage=mgc_android_release_to_external(lease,&release);if(release>=0)close(release);if(!storage){storage=mgc_android_set_acquire_fence(lease,-1);if(storage){s->unused_reservations.push_back(lease);lease=nullptr;}}mgc_lease_release(lease);p.vulkan_storage=storage;}if(!status)status=storage;}
    for(uint32_t feature:{MGC_CAP_SYNC_FD_IMPORT,MGC_CAP_SYNC_FD_EXPORT}){MgcCapability c{};c.header=MGC_HEADER_INIT(MgcCapability);auto result=mgc_domain_capability(s->desc.domain,feature,&c);auto v=mgm_fence_probe_status(result,c,s->native_fence);if(feature==MGC_CAP_SYNC_FD_IMPORT)p.native_fence_import=v;else p.native_fence_export=v;}
    status=mgm_fence_overall(status,s->desc.path,p.native_fence_import,p.native_fence_export);
    if(!state.finish())status=MGC_INTERNAL_ERROR;
    p.workload_qualified=0;mgm_detail::write_desc(out,p,size);return status;
}
MgcStatus mgm_android_capture_oes(MgmAndroidSession *s,const MgmAndroidCapture *source,MgmFrame **out){
    if(out)*out=nullptr;if(!s||!out||!s->current())return MGC_INVALID_ARGUMENT;SessionOperation operation(s);if(!operation||s->closing||s->initializing)return MGC_BUSY;if(!s->usable())return MGC_INTERNAL_ERROR;MgmAndroidCapture c{};auto status=mgm_detail::copy_desc(source,sizeof(MgmAndroidCapture),c);if(status)return status;if(c.required_flags)return MGC_UNSUPPORTED_ABI;
    if(c.epoch!=s->desc.epoch)return MGC_CANCELLED;if(!c.oes_texture||!c.source_id||c.pts_us<0)return MGC_INVALID_ARGUMENT;for(float f:c.texture_transform)if(!std::isfinite(f))return MGC_INVALID_ARGUMENT;
    MgmColor color{};status=mgm_detail::copy_desc(&c.color,offsetof(MgmColor,reserved),color);if(status)return status;if(color.reserved||color.premultiplied>1)return MGC_INVALID_ARGUMENT;if(color.premultiplied||!color.primaries||!color.matrix||color.transfer!=MGM_TRANSFER_SDR||color.bit_depth!=8||color.range!=MGM_RANGE_FULL)return MGC_UNSUPPORTED_CAPABILITY;
    CheckedGlState preflight(s->display,s->context);status=preflight.preflight();
    if(status){if(preflight.unsafe())s->gl_cleanup_uncertain=true;return status;}
    status=android_poll_impl(s,0);if(status!=MGC_SUCCESS&&status!=MGC_BUSY&&status!=MGC_TIMEOUT)return status;
    // Poll may invoke external callbacks which can change the current context.
    if(!s->current()){s->gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}
    if(s->pending.size()>=2||!s->unused_reservations.empty())return MGC_BACKPRESSURE;
    // The ownership record must not allocate after a potential submission.
    if(s->pending.capacity()<2)return MGC_INTERNAL_ERROR;
    Slot *slot=nullptr;MgcLease *writing=nullptr;int32_t before_fd=-1;
    auto hold_unknown=[&](MgcStatus cause,EGLSyncKHR fence=EGL_NO_SYNC_KHR,bool reservation_open=true,bool reservation_known=false){
        s->gl_cleanup_uncertain=true;
        if(writing){Read held{nullptr,fence,0,EGL_NO_IMAGE_KHR,reservation_open,true,writing,true,before_fd,!reservation_known};
            s->pending.push_back(held);writing=nullptr;before_fd=-1;}
        return cause?cause:MGC_INTERNAL_ERROR;
    };
    for(auto &candidate:s->slots){
        if(candidate->pins.load()!=0)continue;
        MgcLeaseDesc lease{};lease.header=MGC_HEADER_INIT(MgcLeaseDesc);lease.content_id=next_content.fetch_add(1);lease.byte_length=candidate->capacity;lease.access=MGC_ACCESS_WRITE;lease.content_context=candidate.get();lease.retain_content=retain_slot;lease.release_content=release_slot;
        status=mgc_lease_create(candidate->resource,&lease,&writing);
        if(!s->current())return hold_unknown(MGC_INTERNAL_ERROR);
        if(status==MGC_BUSY||status==MGC_BACKPRESSURE)continue;if(status)return status;
        status=mgc_android_release_to_external(writing,&before_fd);
        if(!s->current())return hold_unknown(MGC_INTERNAL_ERROR);
        if(status){
            if(status==MGC_BUSY||status==MGC_BACKPRESSURE){
                mgc_lease_release(writing);writing=nullptr;
                if(!s->current()){s->gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}
                continue;
            }
            // No GL command or external reservation has been submitted here.
            // Compute retains its own lease if an ownership command is still
            // retiring; the slot pin therefore outlives this caller reference.
            // A lost domain is not itself evidence that GL work was submitted.
            if(status==MGC_DEVICE_LOST&&before_fd<0){
                mgc_lease_release(writing);writing=nullptr;
                if(!s->current()){s->gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}
                return MGC_DEVICE_LOST;
            }
            // Compute may have submitted before returning a hard error. Its
            // current implementation does not expose physical retirement.
            return hold_unknown(status);
        }
        candidate->content=lease.content_id;slot=candidate.get();break;
    }
    if(!slot){s->event(MGM_EVENT_INPUT_DROPPED,nullptr,c.source_id);if(!s->current()){s->gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}return MGC_BACKPRESSURE;}
    // Snapshot only after poll and lease calls: either can invoke user code
    // which changes context or arbitrary GL state despite SessionOperation.
    CheckedGlState state(s->display,s->context);status=state.begin();
    if(status)return hold_unknown(status);
    auto transaction_failure=[&](MgcStatus cause,EGLSyncKHR fence=EGL_NO_SYNC_KHR){
        if(state.ready()&&!state.unsafe()){
            auto restored=state.finish();if(restored)cause=restored;
        }
        return hold_unknown(cause,fence);
    };
    glBindVertexArray(s->vao);status=state.checkpoint();if(status)return transaction_failure(status);
    if(before_fd>=0){
        if(!s->native_fence)return transaction_failure(MGC_UNSUPPORTED_CAPABILITY);
        EGLint attributes[]={EGL_SYNC_NATIVE_FENCE_FD_ANDROID,before_fd,EGL_NONE};auto ready=s->create_sync(s->display,EGL_SYNC_NATIVE_FENCE_ANDROID,attributes);
        if(ready==EGL_NO_SYNC_KHR)return transaction_failure(MGC_INTERNAL_ERROR);
        before_fd=-1;auto waited=s->wait_sync(s->display,ready,0);
        if(!waited){s->uncertain_sync=ready;return transaction_failure(MGC_INTERNAL_ERROR);}
        if(!s->destroy_sync(s->display,ready)){s->uncertain_sync=ready;return transaction_failure(MGC_INTERNAL_ERROR);}
        status=state.checkpoint();if(status)return transaction_failure(status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER,slot->fbo);glViewport(0,0,s->desc.width,s->desc.height);glUseProgram(s->capture);glUniformMatrix4fv(glGetUniformLocation(s->capture,"transform"),1,GL_FALSE,c.texture_transform);
    // Match FrameGlCapture exactly: four bilinear samples at the canonical
    // base coordinate +/- 1/4 output pixel, transformed only after the offset.
    const bool downscale=s->desc.source_width>=s->desc.width*3/2||s->desc.source_height>=s->desc.height*3/2;
    glUniform1i(glGetUniformLocation(s->capture,"downscale"),downscale?1:0);
    glUniform1i(glGetUniformLocation(s->capture,"flipY"),(s->desc.transform_flags&MGM_ANDROID_CANONICAL_TOP_DOWN)?1:0);
    glUniform2f(glGetUniformLocation(s->capture,"offset"),0.25f/float(s->desc.width),0.25f/float(s->desc.height));
    auto failed_after_draw=[&](MgcStatus cause){
        // A recoverable ordinary error still needs a real fence for any prior
        // capture commands. Do not create/flush/restore on unknown context.
        if(state.unsafe()||!s->current())return hold_unknown(cause);
        EGLSyncKHR failed_fence=s->create_sync(s->display,s->native_fence?EGL_SYNC_NATIVE_FENCE_ANDROID:EGL_SYNC_FENCE_KHR,nullptr);
        if(failed_fence==EGL_NO_SYNC_KHR)return transaction_failure(cause);
        glFlush();auto check=state.checkpoint();if(check)return transaction_failure(check,failed_fence);
        auto restored=state.finish();if(restored)return hold_unknown(restored,failed_fence);
        Read pending{nullptr,failed_fence,0,EGL_NO_IMAGE_KHR,true,true,writing};
        s->pending.push_back(pending);writing=nullptr;return cause;
    };
    glBindTexture(GL_TEXTURE_EXTERNAL_OES,c.oes_texture);glDrawArrays(GL_TRIANGLE_STRIP,0,4);status=state.checkpoint();
    if(status)return failed_after_draw(status);
    const uint32_t step=std::max(4u,s->desc.width/320),sx=std::max(1u,s->desc.width/320),sy=std::max(1u,s->desc.height/180);
    try{if(!status)status=s->sample_grid(*slot,(s->desc.width+step-1)/step,(s->desc.height+step-1)/step,0,slot->static_grid,state);if(!status)status=s->sample_grid(*slot,std::min(s->desc.width,64u),std::min(s->desc.height,36u),1,slot->scene_grid,state);if(!status)status=s->sample_grid(*slot,(s->desc.width+sx-1)/sx,(s->desc.height+sy-1)/sy,2,slot->duplicate_grid,state);}catch(...){status=MGC_OUT_OF_MEMORY;}
    if(status)return failed_after_draw(status);
    EGLSyncKHR fence=s->create_sync(s->display,s->native_fence?EGL_SYNC_NATIVE_FENCE_ANDROID:EGL_SYNC_FENCE_KHR,nullptr);
    if(fence==EGL_NO_SYNC_KHR)return transaction_failure(MGC_INTERNAL_ERROR);
    glFlush();status=state.checkpoint();if(status)return transaction_failure(status,fence);
    status=state.finish();if(status)return hold_unknown(status,fence);
    auto submitted_failure=[&](MgcStatus cause,bool reservation_open){Read pending{nullptr,fence,0,EGL_NO_IMAGE_KHR,reservation_open,true,writing};s->pending.push_back(pending);writing=nullptr;return cause;};
    int32_t fd=-1;
    if(s->desc.path==MGM_ANDROID_AHB_GPU_SYNC){fd=s->dup_fence(s->display,fence);if(fd<0)return submitted_failure(MGC_INTERNAL_ERROR,true);}
    else if(s->client_wait(s->display,fence,0,EGL_FOREVER_KHR)!=EGL_CONDITION_SATISFIED_KHR)return submitted_failure(MGC_INTERNAL_ERROR,true);
    status=mgc_android_set_acquire_fence(writing,fd);if(fd>=0)close(fd);if(!s->current())return hold_unknown(MGC_INTERNAL_ERROR,fence,status!=MGC_SUCCESS,status==MGC_SUCCESS);if(status)return submitted_failure(status,true);
    MgcLease *read=nullptr;status=mgc_lease_publish_read(writing,&read);if(status)return submitted_failure(status,false);
    if(!s->current()){mgc_lease_release(read);return hold_unknown(MGC_INTERNAL_ERROR,fence,false,true);}
    if(!s->destroy_sync(s->display,fence)){s->gl_cleanup_uncertain=true;mgc_lease_release(read);return submitted_failure(MGC_INTERNAL_ERROR,false);}
    slot->pending_producer=writing;writing=nullptr;
    MgmFrameDesc f{};f.header=MGC_HEADER_INIT(MgmFrameDesc);f.content=read;f.epoch=c.epoch;f.source_id=c.source_id;f.pts_ticks=c.pts_us;f.timebase={1,1000000};f.color=color;f.crop_width=s->desc.width;f.crop_height=s->desc.height;f.frame_kind=MGM_FRAME_ORIGINAL;MgmFrame *captured=nullptr;status=mgm_frame_create(&f,&captured);mgc_lease_release(read);
    const auto producer_status=s->poll_producers(true);
    if(producer_status!=MGC_SUCCESS&&producer_status!=MGC_BUSY&&producer_status!=MGC_TIMEOUT){if(captured)mgm_frame_release(captured);return producer_status;}
    if(!s->current()){s->gl_cleanup_uncertain=true;if(captured)mgm_frame_release(captured);return MGC_INTERNAL_ERROR;}
    if(!status){
        s->event(MGM_EVENT_SOURCE_LATCHED,captured,c.source_id);
        if(!s->current()){s->gl_cleanup_uncertain=true;mgm_frame_release(captured);return MGC_INTERNAL_ERROR;}
        *out=captured;
    }
    return status;
}
MgcStatus mgm_android_compare_inputs(MgmAndroidSession *s,MgmFrame *a,MgmFrame *b,MgmAndroidGuardResult *out){
    if(!s||!s->current())return MGC_INVALID_ARGUMENT;SessionOperation operation(s);if(!operation||s->closing||s->initializing)return MGC_BUSY;if(!s->usable())return MGC_INTERNAL_ERROR;MgmAndroidGuardResult result{};uint32_t size=0;auto status=mgm_detail::copy_desc(out,sizeof(result),result,&size);if(status)return status;Slot *found[2]{};MgmFrame *frames[2]={a,b};
    for(int i=0;i<2;++i){MgmFrameDesc d{};d.header=MGC_HEADER_INIT(MgmFrameDesc);status=mgm_frame_describe(frames[i],&d);if(status)return status;if(d.epoch!=s->desc.epoch)return MGC_CANCELLED;MgcLeaseInfo l{};l.header=MGC_HEADER_INIT(MgcLeaseInfo);status=mgc_lease_info(d.content,&l);if(status)return status;for(auto &slot:s->slots)if(slot->allocation==l.allocation_id&&slot->generation==l.allocation_generation&&slot->content==l.content_id)found[i]=slot.get();if(!found[i])return MGC_INVALID_ARGUMENT;}
    const auto &x=found[0]->static_grid,&y=found[1]->static_grid;uint64_t sum=0,count=0;bool bounded=true;for(size_t i=0;i<x.size();i+=4)for(int c=0;c<3;++c){int d=std::abs(int(x[i+c])-int(y[i+c]));sum+=d;++count;if(d>6)bounded=false;}result.is_static=bounded&&count&&sum*2<=count;
    const auto &p=found[0]->scene_grid,&q=found[1]->scene_grid;int bins[48]{};uint64_t difference=0,changed=0;for(size_t i=0;i<p.size();i+=4){int pixel=0;for(int c=0;c<3;++c){int av=p[i+c],bv=q[i+c];++bins[c*16+(av>>4)];--bins[c*16+(bv>>4)];pixel+=std::abs(av-bv);}difference+=pixel;if(pixel>=120)++changed;}uint64_t hist=0;for(int v:bins)hist+=std::abs(v);uint64_t samples=p.size()/4;result.is_cut=!result.is_static&&samples&&changed*10>=samples*7&&difference>=samples*135&&hist*100>=samples*6*45;
    const auto &u=found[0]->duplicate_grid,&v=found[1]->duplicate_grid;uint64_t dup_sum=0,dup_count=0,noisy=0;bool dup_max=true;for(size_t i=0;i<u.size();i+=4)for(int c=0;c<3;++c){int d=std::abs(int(u[i+c])-int(v[i+c]));if(d>16)dup_max=false;if(d>4)++noisy;dup_sum+=d;++dup_count;}result.is_duplicate=dup_max&&dup_count&&dup_sum*10<=dup_count*3&&noisy*200<=dup_count;
    result.sampled_readback_bytes=x.size()+y.size()+p.size()+q.size()+u.size()+v.size();mgm_detail::write_desc(out,result,size);return MGC_SUCCESS;
}
MgcStatus mgm_android_draw(MgmAndroidSession *s,MgmFrame *frame,uint32_t width,uint32_t height){
    if(!s||!s->current()||!frame||!width||!height)return MGC_INVALID_ARGUMENT;
    SessionOperation operation(s);if(!operation||s->closing||s->initializing)return MGC_BUSY;if(!s->usable())return MGC_INTERNAL_ERROR;
    CheckedGlState preflight(s->display,s->context);auto status=preflight.preflight();
    if(status){if(preflight.unsafe())s->gl_cleanup_uncertain=true;return status;}
    MgmFrameDesc d{};d.header=MGC_HEADER_INIT(MgmFrameDesc);status=mgm_frame_describe(frame,&d);if(status)return status;if(d.epoch!=s->desc.epoch)return MGC_CANCELLED;
    status=android_poll_impl(s,0);if(status!=MGC_SUCCESS&&status!=MGC_BUSY&&status!=MGC_TIMEOUT)return status;
    if(s->pending.capacity()<2||s->pending.size()>=2)return MGC_BACKPRESSURE;
    MgcLeaseInfo target{};target.header=MGC_HEADER_INIT(MgcLeaseInfo);status=mgc_lease_info(d.content,&target);if(status)return status;
    for(auto &pending:s->pending){MgmFrameDesc previous{};previous.header=MGC_HEADER_INIT(MgmFrameDesc);MgcLeaseInfo identity{};identity.header=MGC_HEADER_INIT(MgcLeaseInfo);if(pending.retirement>=0){if(!pending.retired_identity_valid)return MGC_BUSY;identity=pending.retired_identity;}else if(pending.aborted_write){if(mgc_lease_info(pending.aborted_write,&identity))return MGC_INTERNAL_ERROR;}else if(mgm_frame_describe(pending.frame,&previous)||mgc_lease_info(previous.content,&identity))return MGC_INTERNAL_ERROR;if(identity.domain_id==target.domain_id&&identity.domain_generation==target.domain_generation&&identity.allocation_id==target.allocation_id&&identity.allocation_generation==target.allocation_generation)return MGC_BACKPRESSURE;}
    int32_t fd=-1;status=mgc_android_release_to_external(d.content,&fd);
    if(status){
        if(fd>=0){Read unknown{frame,EGL_NO_SYNC_KHR,0,EGL_NO_IMAGE_KHR};unknown.owned_fd=fd;unknown.reservation_unknown=true;unknown.cleanup_uncertain=true;
            s->gl_cleanup_uncertain=true;mgm_frame_retain(frame);s->pending.push_back(unknown);return MGC_INTERNAL_ERROR;}
        if(!s->current()){s->gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}return status;
    }
    Read read{frame,EGL_NO_SYNC_KHR,0,EGL_NO_IMAGE_KHR};CheckedGlState* state=nullptr;
    auto hold=[&](MgcStatus cause){mgm_frame_retain(frame);s->pending.push_back(read);return cause;};
    auto before_draw_failure=[&](MgcStatus cause){
        if(!s->current()||s->gl_cleanup_uncertain){if(fd>=0){read.owned_fd=fd;fd=-1;}s->gl_cleanup_uncertain=true;read.cleanup_uncertain=true;return hold(cause==MGC_OUT_OF_MEMORY?MGC_OUT_OF_MEMORY:MGC_INTERNAL_ERROR);}
        if(fd>=0){close(fd);fd=-1;}
        auto end=mgc_android_set_acquire_fence(d.content,-1);if(!end)read.reservation_open=false;
        if(!s->current()){s->gl_cleanup_uncertain=true;read.cleanup_uncertain=true;return hold(MGC_INTERNAL_ERROR);}
        if(end){if(state&&state->ready()&&state->finish()!=MGC_SUCCESS){s->gl_cleanup_uncertain=true;read.cleanup_uncertain=true;}return hold(end);}
        const auto cleaned=s->cleanup_read(read);if(cleaned)return hold(cleaned);
        if(state&&state->ready()&&state->finish()!=MGC_SUCCESS){s->gl_cleanup_uncertain=true;read.cleanup_uncertain=true;return hold(MGC_INTERNAL_ERROR);}
        return cause;
    };
    if(!s->current()){s->gl_cleanup_uncertain=true;return before_draw_failure(MGC_INTERNAL_ERROR);}
    AHardwareBuffer *buffer=nullptr;status=mgc_android_get_buffer(d.content,&buffer);if(status)return before_draw_failure(status);
    if(!s->current()){s->gl_cleanup_uncertain=true;return before_draw_failure(MGC_INTERNAL_ERROR);}
    CheckedGlState transaction(s->display,s->context);state=&transaction;status=transaction.begin();
    if(status){const auto& report=transaction.report();if(transaction.unsafe()||
        (report.mutation_attempted&&!report.restore_succeeded)||(report.active_switched&&report.restore_attempted&&!report.restore_succeeded))s->gl_cleanup_uncertain=true;
        return before_draw_failure(status);}
    glBindVertexArray(s->vao);
    if(fd>=0){
        if(!s->native_fence)return before_draw_failure(MGC_UNSUPPORTED_CAPABILITY);
        EGLint attr[]={EGL_SYNC_NATIVE_FENCE_FD_ANDROID,fd,EGL_NONE};auto ready=s->create_sync(s->display,EGL_SYNC_NATIVE_FENCE_ANDROID,attr);
        if(ready==EGL_NO_SYNC_KHR)return before_draw_failure(MGC_INTERNAL_ERROR);fd=-1;read.fence=ready;
        const auto waited=s->wait_sync(s->display,ready,0);
        if(!s->current()){s->gl_cleanup_uncertain=true;read.cleanup_uncertain=true;return hold(MGC_INTERNAL_ERROR);}
        if(!s->destroy_sync(s->display,ready)){s->gl_cleanup_uncertain=true;read.cleanup_uncertain=true;return hold(MGC_INTERNAL_ERROR);}read.fence=EGL_NO_SYNC_KHR;
        if(!s->current()){s->gl_cleanup_uncertain=true;read.cleanup_uncertain=true;return hold(MGC_INTERNAL_ERROR);}
        if(!waited)return before_draw_failure(MGC_INTERNAL_ERROR);
    }
    status=s->image(buffer,read.image,read.texture);if(!s->current()){s->gl_cleanup_uncertain=true;return before_draw_failure(MGC_INTERNAL_ERROR);}if(status)return before_draw_failure(status);
    glViewport(0,0,width,height);glUseProgram(s->display_program);glUniform1i(glGetUniformLocation(s->display_program,"flipY"),(s->desc.transform_flags&MGM_ANDROID_CANONICAL_TOP_DOWN)?1:0);
    mgm_frame_retain(frame);read.submitted=true;glDrawArrays(GL_TRIANGLE_STRIP,0,4);const auto draw_status=transaction.checkpoint();
    if(transaction.unsafe()||!s->current()){s->gl_cleanup_uncertain=true;read.cleanup_uncertain=true;s->pending.push_back(read);return draw_status?draw_status:MGC_INTERNAL_ERROR;}
    read.fence=s->create_sync(s->display,s->native_fence?EGL_SYNC_NATIVE_FENCE_ANDROID:EGL_SYNC_FENCE_KHR,nullptr);
    if(read.fence==EGL_NO_SYNC_KHR){s->gl_cleanup_uncertain=true;read.cleanup_uncertain=true;s->pending.push_back(read);return MGC_INTERNAL_ERROR;}
    if(!s->current()){s->gl_cleanup_uncertain=true;read.cleanup_uncertain=true;s->pending.push_back(read);return MGC_INTERNAL_ERROR;}
    glFlush();if(transaction.checkpoint()!=MGC_SUCCESS||!s->current()){s->gl_cleanup_uncertain=true;read.cleanup_uncertain=true;s->pending.push_back(read);return MGC_INTERNAL_ERROR;}
    s->pending.push_back(read);auto &retiring=s->pending.back();
    if(transaction.finish()!=MGC_SUCCESS){s->gl_cleanup_uncertain=true;retiring.cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}
    if(s->native_fence){int32_t sampled=s->dup_fence(s->display,read.fence);
        if(!s->current()){if(sampled>=0)retiring.owned_fd=sampled;s->gl_cleanup_uncertain=true;retiring.cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}
        if(sampled<0)return MGC_INTERNAL_ERROR;status=mgc_android_set_acquire_fence(d.content,sampled);close(sampled);if(!status)retiring.reservation_open=false;if(!s->current()){s->gl_cleanup_uncertain=true;retiring.cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}if(status)return status;}
    // Without an exportable native fence, poll closes the reservation only after
    // the KHR sampling fence is actually signaled. Never publish satisfied -1 early.
    if(draw_status!=MGC_SUCCESS)return draw_status;s->event(MGM_EVENT_SINK_ACCEPTED,frame);
    if(!s->current()){s->gl_cleanup_uncertain=true;retiring.cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}return MGC_SUCCESS;
}
static MgcStatus android_poll_impl(MgmAndroidSession *s,uint64_t timeout){
    if(!s->usable())return MGC_INTERNAL_ERROR;
    s->poll_active=true;struct PollGuard{MgmAndroidSession *session;~PollGuard(){session->poll_active=false;}} guard{s};
    auto context_ok=[&](){if(s->current())return true;s->gl_cleanup_uncertain=true;return false;};
    if(s->domain_released)return s->retirement.empty()?MGC_SUCCESS:MGC_BUSY;
    if(s->domain_retirement>=0)return s->retirement.completed(s->domain_retirement)?MGC_SUCCESS:MGC_BUSY;
    // Close queues resource references and returns BUSY. Consume finished close
    // receipts here as well, because Java retries close only after poll==0.
    if(s->closing)for(auto& slot:s->slots)if(slot->resource_retirement>=0&&s->retirement.completed(slot->resource_retirement)){
        s->retirement.consume(slot->resource_retirement);slot->resource_retirement=-1;}
    // A GL owner never performs potentially unbounded Vulkan device-idle waits.
    // A separate service thread advances the domain and this retirement queue.
    MgcDomainStateInfo device_state{};device_state.header=MGC_HEADER_INIT(MgcDomainStateInfo);
    auto domain_status=mgc_domain_state(s->desc.domain,&device_state);if(domain_status)return domain_status;
    const auto device_service=device_state.lost&&!device_state.device_retired?MGC_BUSY:MGC_SUCCESS;
    auto producer_status=s->poll_producers(true);if(!context_ok())return MGC_INTERNAL_ERROR;
    if(producer_status==MGC_DEVICE_LOST)producer_status=MGC_BUSY;
    if(producer_status!=MGC_SUCCESS&&producer_status!=MGC_TIMEOUT&&producer_status!=MGC_BUSY){s->gl_cleanup_uncertain=true;return producer_status;}
    for(auto i=s->unused_reservations.begin();i!=s->unused_reservations.end();){
        const int index=s->retirement.reserve();if(index<0)return MGC_BUSY;
        auto status=mgc_android_set_acquire_fence(*i,-1);if(!context_ok()){if(index>=0)s->retirement.cancel(index);return MGC_INTERNAL_ERROR;}
        if(status){if(index>=0)s->retirement.cancel(index);if(status==MGC_DEVICE_LOST)return MGC_BUSY;if(status!=MGC_BUSY&&status!=MGC_TIMEOUT)s->gl_cleanup_uncertain=true;return status;}
        s->retirement.publish(index,nullptr,*i,nullptr,false);
        i=s->unused_reservations.erase(i);if(!context_ok())return MGC_INTERNAL_ERROR;
    }
    for(auto i=s->pending.begin();i!=s->pending.end();){
        if(i->retirement>=0){if(!s->retirement.completed(i->retirement))return MGC_BUSY;
            if(i->retired_event_present){i->retired_event_present=false;s->emit_event(i->retired_event);if(!context_ok())return MGC_INTERNAL_ERROR;}
            s->retirement.consume(i->retirement);i=s->pending.erase(i);continue;}
        if(!i->gl_retired){
        // A saved fd or unknown reservation cannot be converted into a
        // satisfied (-1) dependency by the ordinary poll path.
        if(i->reservation_unknown||i->owned_fd>=0){s->gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}
        if(i->submitted){if(i->fence==EGL_NO_SYNC_KHR){s->gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}auto result=s->client_wait(s->display,i->fence,0,timeout);if(result==EGL_TIMEOUT_EXPIRED_KHR)return MGC_TIMEOUT;if(result!=EGL_CONDITION_SATISFIED_KHR){s->gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}}
        MgcLease *lease=i->aborted_write;MgmFrameDesc frame{};if(!lease){frame.header=MGC_HEADER_INIT(MgmFrameDesc);auto rc=mgm_frame_describe(i->frame,&frame);if(!context_ok())return MGC_INTERNAL_ERROR;if(rc){s->gl_cleanup_uncertain=true;return rc;}lease=frame.content;}
        if(i->reservation_open){auto rc=mgc_android_set_acquire_fence(lease,-1);if(rc==MGC_SUCCESS)i->reservation_open=false;if(!context_ok())return MGC_INTERNAL_ERROR;if(rc){if(rc==MGC_DEVICE_LOST)return MGC_BUSY;if(rc!=MGC_BUSY&&rc!=MGC_TIMEOUT)s->gl_cleanup_uncertain=true;return rc;}}
        else {auto rc=mgc_android_wait_ready(lease,0);if(!context_ok())return MGC_INTERNAL_ERROR;if(rc){if(rc==MGC_DEVICE_LOST)return MGC_BUSY;if(rc!=MGC_BUSY&&rc!=MGC_TIMEOUT)s->gl_cleanup_uncertain=true;return rc;}}
        auto cleanup=s->cleanup_read(*i);if(cleanup)return cleanup;
        if(i->submitted&&i->frame){i->retired_event=s->make_event(MGM_EVENT_OUTPUT_RELEASED,i->frame);i->retired_event_present=true;}
        i->gl_retired=true;
        }
        if(!i->retired_identity_valid){
            MgcLease *held=i->aborted_write;MgmFrameDesc snapshot{};snapshot.header=MGC_HEADER_INIT(MgmFrameDesc);
            if(!held){auto rc=mgm_frame_describe(i->frame,&snapshot);if(rc)return rc;held=snapshot.content;}
            MgcLeaseInfo identity{};identity.header=MGC_HEADER_INIT(MgcLeaseInfo);auto rc=mgc_lease_info(held,&identity);if(rc)return rc;
            i->retired_identity=identity;i->retired_identity_valid=true;
        }
        const int index=s->retirement.enqueue(i->frame,i->aborted_write,nullptr,true);if(index<0)return MGC_BUSY;
        i->frame=nullptr;i->aborted_write=nullptr;i->retirement=index;return MGC_BUSY;
    }
    if(!s->retirement.empty())return MGC_BUSY;
    return device_service!=MGC_SUCCESS?device_service:producer_status;
}
MgcStatus mgm_android_poll(MgmAndroidSession *s,uint64_t timeout){
    if(!s||!s->current())return MGC_INVALID_ARGUMENT;
    if(s->initializing){auto* entry=s->creation_entry;if(!entry||!CreationPool::claim(*entry,CreationPool::State::Active,CreationPool::State::OwnerBusy))return MGC_BUSY;
        const auto result=[&]()->MgcStatus{SessionOperation operation(s);if(!operation)return MGC_BUSY;if(!s->usable())return MGC_INTERNAL_ERROR;return android_poll_impl(s,timeout);}();
        CreationPool::finish(*entry,CreationPool::State::Active);return result;
    }
    SessionOperation operation(s);if(!operation)return MGC_BUSY;if(!s->usable())return MGC_INTERNAL_ERROR;return android_poll_impl(s,timeout);
}
MgcStatus mgm_android_defer_frame_release(MgmAndroidSession *s,MgmFrame *frame){
    if(!s||!frame||!s->thread_token||s->thread_token!=mgm_thread_token()||s->thread!=std::this_thread::get_id())return MGC_INVALID_ARGUMENT;
    if(s->operation_active||s->initializing||s->domain_retirement>=0||s->domain_released)return MGC_BUSY;
    return s->retirement.enqueue(frame,nullptr,nullptr,false)>=0?MGC_SUCCESS:MGC_BUSY;
}
MgcStatus mgm_android_service_retirement(MgmAndroidSession *s){return s?s->retirement.service():MGC_INVALID_ARGUMENT;}
MgcStatus mgm_android_begin_epoch(MgmAndroidSession *s,uint64_t epoch){if(!s||!s->current())return MGC_INVALID_ARGUMENT;SessionOperation operation(s);if(!operation||s->closing||s->initializing)return MGC_BUSY;if(!s->usable())return MGC_INTERNAL_ERROR;if(epoch<=s->desc.epoch)return MGC_INVALID_ARGUMENT;s->desc.epoch=epoch;return MGC_SUCCESS;}
MgcStatus mgm_android_set_event_handler(MgmAndroidSession *s,MgmEventFn event,void *context){if(!s||!s->current())return MGC_INVALID_ARGUMENT;SessionOperation operation(s);if(!operation||s->initializing)return MGC_BUSY;if(!s->usable())return MGC_INTERNAL_ERROR;s->desc.event=event;s->desc.event_context=context;return MGC_SUCCESS;}
static MgcStatus android_session_close_impl(MgmAndroidSession *s){
    if(!s||!s->current())return MGC_INVALID_ARGUMENT;SessionOperation operation(s);if(!operation)return MGC_BUSY;s->closing=true;
    if(!s->usable())return MGC_INTERNAL_ERROR;if(s->staged_probe_lease||s->staged_probe_fd>=0||!s->pending.empty()||!s->unused_reservations.empty())return MGC_BUSY;
    for(auto &slot:s->slots)if(slot->pins.load()||slot->pending_producer||slot->producer_retirement>=0)return MGC_BUSY;
    bool resource_receipts=false;for(const auto& slot:s->slots)if(slot->resource_retirement>=0)resource_receipts=true;
    if(!resource_receipts&&s->domain_retirement<0&&!s->retirement.empty())return MGC_BUSY;
    MgcDomainStateInfo state{};state.header=MGC_HEADER_INIT(MgcDomainStateInfo);
    if(s->domain_retirement>=0){if(!s->retirement.completed(s->domain_retirement))return MGC_BUSY;
        s->retirement.consume(s->domain_retirement);s->domain_retirement=-1;s->domain_released=true;}
    else if(!s->domain_released) {
        auto status=mgc_domain_state(s->desc.domain,&state);if(status)return status;
        if(state.lost&&!state.device_retired)return MGC_BUSY;
        bool waiting=false;
        for(auto &slot:s->slots){
            if(slot->resource_retirement>=0){if(!s->retirement.completed(slot->resource_retirement)){waiting=true;continue;}
                s->retirement.consume(slot->resource_retirement);slot->resource_retirement=-1;}
            if(slot->resource){const int index=s->retirement.enqueue(nullptr,nullptr,slot->resource,true);
                if(index<0){waiting=true;continue;}slot->resource=nullptr;slot->resource_retirement=index;waiting=true;}
        }
        if(waiting||!s->retirement.empty())return MGC_BUSY;
        const int index=s->retirement.enqueue_domain(s->desc.domain);if(index<0)return MGC_BUSY;
        s->desc.domain=nullptr;s->domain_retirement=index;return MGC_BUSY;
    }
    if(!s->retirement.empty())return MGC_BUSY;
    if(s->desc.domain)return MGC_INTERNAL_ERROR;
    const auto prior_error=glGetError();if(prior_error!=GL_NO_ERROR){if(prior_error==GLenum(0x0507))s->gl_cleanup_uncertain=true;return MGC_INTERNAL_ERROR;}
    for(auto &slot:s->slots){const auto result=s->cleanup_slot_final(*slot);if(result)return result;}
    {const auto result=s->cleanup_slot_final(s->probe_slot);if(result)return result;}
    const auto objects=s->cleanup_objects_final();if(objects)return objects;
    operation.disarm();delete s;return MGC_SUCCESS;
}
MgcStatus mgm_android_session_close(MgmAndroidSession *s){
    if(!s||!s->current()||!s->creation_entry)return MGC_INVALID_ARGUMENT;
    if(s->initializing&&s->initialization_token){auto* pool=creation_pool.load(std::memory_order_acquire);if(pool)(void)pool->cancel(s->initialization_token);}
    auto* entry=s->creation_entry;
    if(!CreationPool::claim(*entry,CreationPool::State::Active,CreationPool::State::OwnerBusy))return MGC_BUSY;
    const auto result=android_session_close_impl(s);
    CreationPool::finish(*entry,result==MGC_SUCCESS?CreationPool::State::Retired:CreationPool::State::Active);
    return result;
}
}
