#include <jni.h>
#include <meguviz/media.h>
#include <meguviz/rife.h>
#include <meguviz/media_rife.h>
#include "../../src/alpha.hpp"
#include "model_catalog.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <set>
#if defined(MGM_EXPERIMENTAL_SHARED_PLAN)
extern "C" MgcStatus mgr_private_create_share_capable_plan(MgrModel*,const MgrPlanDesc*,MgrPlan**);
extern "C" MgcStatus mgr_private_enable_shared_ahb(MgrPlan*);
extern "C" void mgr_private_release_shared_ahb(MgrPlan*);
extern "C" MgcStatus mgm_private_rife_effect_shared(MgrPlan*,MgmEffect**,MgmEffectApi*);
#endif
#if defined(__ANDROID__)
MgcStatus mgm_jni_service_android_retirement();
#endif

#if defined(MGR_ENABLE_JOB_OBSERVATION)
#define MGR_JNI_OBSERVE(...) __VA_ARGS__
#define MGR_JNI_OBSERVE_ARG(value) , value
#else
#define MGR_JNI_OBSERVE(...)
#define MGR_JNI_OBSERVE_ARG(value)
#endif

namespace {
constexpr uint64_t max_frame_pixels=4096ULL*2160;
enum Code {OK=0,INVALID=1,UNSUPPORTED=2,GPU_UNAVAILABLE=3,MODEL_TOO_LARGE=4,INVALID_MODEL=5,MODEL_LOAD_FAILED=6,NOT_LOADED=7,BUSY=8,ALLOCATION_FAILED=9,INFERENCE_FAILED=10,INVALID_OUTPUT=11};
int code(MgcStatus s){switch(s){case MGC_SUCCESS:return OK;case MGC_INVALID_ARGUMENT:case MGC_UNSUPPORTED_ABI:return INVALID;case MGC_UNSUPPORTED_CAPABILITY:return UNSUPPORTED;case MGC_DEVICE_LOST:return GPU_UNAVAILABLE;case MGC_MODEL_MISMATCH:case MGC_ARTIFACT_MISMATCH:return INVALID_MODEL;case MGC_BUSY:case MGC_BACKPRESSURE:return BUSY;case MGC_OUT_OF_MEMORY:return ALLOCATION_FAILED;default:return INFERENCE_FAILED;}}
bool direct(JNIEnv *env,jobject b,void **p,uint64_t &size,bool writable=false){if(!b)return false;jlong n=env->GetDirectBufferCapacity(b);if(n<=0||uint64_t(n)>SIZE_MAX)return false;*p=env->GetDirectBufferAddress(b);size=uint64_t(n);if(!*p)return false;if(writable){jclass cls=env->GetObjectClass(b);auto method=env->GetMethodID(cls,"isReadOnly","()Z");bool ro=!method||env->CallBooleanMethod(b,method);env->DeleteLocalRef(cls);if(env->ExceptionCheck()||ro)return false;}return true;}
bool shape(int w,int h,int stride,uint64_t capacity,uint64_t channels=3){return w>0&&h>0&&uint64_t(w)*h<=max_frame_pixels&&stride>=int64_t(w)*channels&&(uint64_t(h)-1)*uint64_t(stride)+uint64_t(w)*channels<=capacity;}
MgcHash digest(const char *hex){MgcHash h{};auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};for(size_t i=0;i<32;++i)h.bytes[i]=uint8_t(digit(hex[i*2])*16+digit(hex[i*2+1]));return h;}
struct CachedInput;
struct ShaderConfiguration {
    MgcShaderpack* pack=nullptr;
    std::vector<MgrShaderBinding> bindings;
    ~ShaderConfiguration(){mgc_shaderpack_release(pack);}
};
struct Engine {
    MgcRuntime *runtime=nullptr;MgcDomain *domain=nullptr;MgrModel *model=nullptr;MgrPlan *plan=nullptr;MgrSession *session=nullptr;
    // Independent reference for status reads while frameProcess owns mutex.
    // Access after publication only through atomic shared_ptr operations.
    std::shared_ptr<MgcDomain> device_status_domain;
    std::mutex mutex;uint64_t max_pixels=0,sequence=0;int backend=0,threads=1,flags=0;bool motion=true,plan_dirty=false;
    std::atomic<bool> closing{false},creating{false};
    // 0 legacy, 1 reserved, 2 starting, 3 started or canceled, 4 failed start.
    std::atomic<int> reservation_state{0};
    std::chrono::steady_clock::time_point reserved_at{};
    std::atomic<uint32_t> unreliable{0},patched{0},histogram{0};std::array<std::atomic<int64_t>,3> stages{};
    std::atomic<uint32_t> last_kind{0},last_scale{0};std::atomic<int64_t> last_worker_cpu{-1};
    std::array<jlong,7> gpu_measurement{};
    void capture_gpu_measurement(const MgrResultInfo& result) {
        gpu_measurement={};
        const auto bound=uint64_t(std::numeric_limits<jlong>::max());
        if(result.gpu_elapsed_ns>bound||result.gpu_batch_count>bound||result.gpu_observed_batch_count>bound)return;
        float period=0;std::memcpy(&period,&result.gpu_timestamp_period_bits,sizeof(period));
        const bool observed=result.frame_kind==MGR_RESULT_INFERRED&&result.gpu_elapsed_observed&&
            result.gpu_batch_count>0&&result.gpu_batch_count==result.gpu_observed_batch_count&&
            result.gpu_timestamp_valid_bits>0&&result.gpu_timestamp_valid_bits<=64&&std::isfinite(period)&&period>0;
        gpu_measurement={observed?1:0,observed?jlong(result.gpu_elapsed_ns):0,jlong(result.gpu_batch_count),
            jlong(result.gpu_observed_batch_count),jlong(result.gpu_timestamp_valid_bits),
            jlong(result.gpu_timestamp_period_bits),result.batch_event_coverage_observed?1:0};
    }
    std::vector<std::shared_ptr<CachedInput>> input_cache;
#if defined(MGR_ENABLE_JOB_OBSERVATION)
    std::array<jlong,2> observation_host{};
    MgrJobObservation observation{};
    std::atomic<uint64_t> observation_state{0};
    int64_t observation_left=-1,observation_right=-1;
    uint64_t begin_observation() noexcept {
        auto old=observation_state.load(std::memory_order_relaxed);
        for(;;){const auto generation=old&~uint64_t(1);
            const auto next=old==UINT64_MAX||generation>=UINT64_MAX-2?UINT64_MAX:generation+2;
            if(observation_state.compare_exchange_weak(old,next,std::memory_order_acq_rel))return next;}
    }
    // Called with mutex held. A later completed job may have acquired the lock
    // first; claim its published generation only with a CAS. An invalid state
    // represents a failed/BUSY attempt and must never be revived.
    uint64_t admit_observation(uint64_t ticket) noexcept {
        auto state=observation_state.load(std::memory_order_acquire);
        if(state==ticket)return ticket;
        if(state==UINT64_MAX||!(state&1))return UINT64_MAX;
        const auto generation=state&~uint64_t(1);
        const auto next=generation>=UINT64_MAX-2?UINT64_MAX:generation+2;
        return observation_state.compare_exchange_strong(state,next,std::memory_order_acq_rel)?next:UINT64_MAX;
    }
    void publish_observation(uint64_t generation) noexcept {
        if(generation==UINT64_MAX)return;auto expected=generation;
        observation_state.compare_exchange_strong(expected,generation|1,std::memory_order_release,std::memory_order_relaxed);
    }
#endif
#if defined(MGM_EXPERIMENTAL_SHARED_PLAN)
    // Immutable for a loaded generation; configure before model load.
    bool shared_plan_requested=false;
#endif
    void clear_shared_policy() {
#if defined(MGM_EXPERIMENTAL_SHARED_PLAN)
        shared_plan_requested=false;
#endif
    }
    uint32_t optimization_flags=0;
    std::shared_ptr<ShaderConfiguration> shaders;
    void clear_plan(){if(session){mgr_drain(session,UINT64_MAX);mgr_session_close(session);session=nullptr;}mgr_plan_release(plan);plan=nullptr;input_cache.clear();last_kind=0;last_scale=0;last_worker_cpu=-1;gpu_measurement={};for(auto &stage:stages)stage=0;}
    MgcStatus try_close(){
        if(session){auto status=mgr_drain(session,0);if(status)return status;status=mgr_session_close(session);if(status)return status;session=nullptr;}
        mgr_plan_release(plan);plan=nullptr;input_cache.clear();
        if(backend&&domain){auto status=mgr_uploads_poll(domain,0);if(status)return status;}
        shaders.reset();mgr_model_release(model);model=nullptr;
        std::atomic_store(&device_status_domain,std::shared_ptr<MgcDomain>{});
        if(domain){mgc_domain_release(domain);domain=nullptr;}
        if(runtime){auto status=mgc_runtime_close(runtime);if(status)return status;mgc_runtime_release(runtime);runtime=nullptr;}
        return MGC_SUCCESS;
    }
    ~Engine(){clear_plan();shaders.reset();mgr_model_release(model);mgc_domain_release(domain);if(runtime){mgc_runtime_close(runtime);mgc_runtime_release(runtime);}}
    MgrPlanDesc describe_plan(uint32_t format,uint32_t width,uint32_t height,bool dynamic,
                              uint32_t optimizations,const std::shared_ptr<ShaderConfiguration>& shader_config)const {
        MgrPlanDesc d{};d.header=MGC_HEADER_INIT(MgrPlanDesc);d.width=width;d.height=height;d.input_format=format;
        d.precision_profile=!backend?(flags&1?MGR_PRECISION_CPU_FP16_LEGACY:MGR_PRECISION_FP32_REFERENCE):(flags&1?MGR_PRECISION_LEGACY_FP16:MGR_PRECISION_VULKAN_FP32_LEGACY);
        d.flow_scale=(flags&6)==6?8:(flags&4?4:(flags&2?2:1));d.guard_flags=(flags&16?MGR_GUARD_STATIC_DETAIL:0)|(motion&&backend?MGR_GUARD_MOTION:0);
        d.execution_flags=(dynamic?MGR_EXECUTION_DYNAMIC_SHAPE:0)|(flags&128?MGR_EXECUTION_ADAPTIVE_FLOW:0)|
            (!(flags&32)&&d.flow_scale>1?MGR_EXECUTION_SEPARATE_WARP:0)|(flags&64?MGR_EXECUTION_RAW_GPU_INPUT:0);
        if(backend&&!(flags&8))d.execution_flags|=MGR_EXECUTION_GPU_IO_DISABLED;
        d.max_pixels=max_pixels;d.num_threads=threads;d.optimization_flags=optimizations;
        if(shader_config){d.shaderpack=shader_config->pack;d.shader_bindings=shader_config->bindings.data();d.shader_binding_count=uint32_t(shader_config->bindings.size());}
        return d;
    }
    MgcStatus reconfigure(uint32_t optimizations,const std::shared_ptr<ShaderConfiguration>& shader_config) {
        if(!model){optimization_flags=optimizations;shaders=shader_config;return MGC_SUCCESS;}
        auto status=session?mgr_drain(session,UINT64_MAX):MGC_SUCCESS;if(status)return status;
        const auto d=describe_plan(MGC_FORMAT_RGB8,0,0,true,optimizations,shader_config);
        MgrPlan* fresh_plan=nullptr;MgrSession* fresh_session=nullptr;
#if defined(MGM_EXPERIMENTAL_SHARED_PLAN)
        status=shared_plan_requested?mgr_private_create_share_capable_plan(model,&d,&fresh_plan):mgr_plan_create(model,&d,&fresh_plan);
#else
        status=mgr_plan_create(model,&d,&fresh_plan);
#endif
        if(status)return status;
        MgcSessionDesc sd{};sd.header=MGC_HEADER_INIT(MgcSessionDesc);sd.max_in_flight=1;
        status=mgr_session_create(fresh_plan,&sd,&fresh_session);
        if(status){mgr_plan_release(fresh_plan);return status;}
        if(session){status=mgr_session_close(session);if(status){mgr_session_close(fresh_session);mgr_plan_release(fresh_plan);return status;}}
        mgr_plan_release(plan);plan=fresh_plan;session=fresh_session;optimization_flags=optimizations;shaders=shader_config;
        input_cache.clear();last_kind=0;last_scale=0;last_worker_cpu=-1;gpu_measurement={};for(auto& stage:stages)stage=0;plan_dirty=false;return MGC_SUCCESS;
    }
    MgcStatus prepare(){if(!model)return MGC_MODEL_MISMATCH;return reconfigure(optimization_flags,shaders);}
};
std::mutex registry_mutex;std::map<jlong,std::shared_ptr<Engine>> registry;uint64_t next_handle=0;
std::shared_ptr<Engine> acquire(jlong id,bool include_closing=false){std::lock_guard<std::mutex> lock(registry_mutex);auto i=registry.find(id);return i==registry.end()||i->second->creating.load(std::memory_order_acquire)||(!include_closing&&(i->second->closing.load()||i->second->reservation_state.load()==4))?nullptr:i->second;}
int request_close(jlong id){
    auto engine=acquire(id,true);if(!engine)return NOT_LOADED;
    engine->closing.store(true);
    std::unique_lock<std::mutex> lock(engine->mutex,std::try_to_lock);if(!lock)return BUSY;
    const auto status=engine->try_close();
    // Drain/upload polling can time out without retiring the owner. Only this
    // close protocol treats that as retryable; normal frame status is unchanged.
    if(status==MGC_TIMEOUT||status==MGC_BACKPRESSURE)return BUSY;
    if(status)return code(status);
    lock.unlock();
    {std::lock_guard<std::mutex> guard(registry_mutex);auto found=registry.find(id);if(found!=registry.end()&&found->second==engine)registry.erase(found);}
    return OK;
}
jlongArray creation(JNIEnv *env,jlongArray out,int status,jlong id){jlong v[2]={status,id};env->SetLongArrayRegion(out,0,2,v);return env->ExceptionCheck()?nullptr:out;}
struct Lease {MgcResource *resource=nullptr;MgcLease *lease=nullptr;~Lease(){mgc_lease_release(lease);mgc_resource_release(resource);}};
struct CachedInput {Lease storage;int64_t source_id=-1;int width=0,height=0,stride=0;uint64_t domain_id=0,domain_generation=0,stamp=0;};
MgcStatus input(Engine &engine,const void *bytes,int width,int height,int stride,Lease &out){MgcResourceDesc r{};r.header=MGC_HEADER_INIT(MgcResourceDesc);r.width=uint32_t(width);r.height=uint32_t(height);r.format=MGC_FORMAT_RGB8;r.row_stride_bytes=uint64_t(width)*3;r.capacity_bytes=r.row_stride_bytes*height;auto s=mgc_resource_create(engine.domain,&r,&out.resource);if(s)return s;MgcLeaseDesc d{};d.header=MGC_HEADER_INIT(MgcLeaseDesc);d.content_id=++engine.sequence;d.byte_length=r.capacity_bytes;d.access=MGC_ACCESS_WRITE;s=mgc_lease_create(out.resource,&d,&out.lease);if(s)return s;void *data=nullptr;uint64_t n=0;s=mgc_lease_map(out.lease,&data,&n);if(s)return s;for(int y=0;y<height;++y)std::memcpy(static_cast<uint8_t*>(data)+y*r.row_stride_bytes,static_cast<const uint8_t*>(bytes)+uint64_t(y)*stride,r.row_stride_bytes);mgc_lease_unmap(out.lease);mgc_lease_release(out.lease);out.lease=nullptr;d.access=MGC_ACCESS_READ;return mgc_lease_create(out.resource,&d,&out.lease);}
// A cached READ is evicted before touching its backing allocation. A new
// exclusive WRITE remains the sole authority to recycle; shared_ptr counts
// alone never prove that Compute or a GPU has retired the old content.
MgcStatus reuse_input(Engine &engine,const void *bytes,int width,int height,int stride,Lease &out) {
    MgcLeaseInfo old{};old.header=MGC_HEADER_INIT(MgcLeaseInfo);
    auto status=mgc_lease_info(out.lease,&old);if(status)return status;
    const uint64_t row=uint64_t(width)*3, length=row*uint64_t(height);
    if(old.byte_offset || old.width!=uint32_t(width) || old.height!=uint32_t(height) ||
       old.format!=MGC_FORMAT_RGB8 || old.row_stride_bytes!=row || old.byte_length!=length ||
       engine.sequence==UINT64_MAX) return MGC_INVALID_ARGUMENT;
    mgc_lease_release(out.lease);out.lease=nullptr;
    MgcLeaseDesc d{};d.header=MGC_HEADER_INIT(MgcLeaseDesc);d.content_id=++engine.sequence;
    d.byte_length=length;d.access=MGC_ACCESS_WRITE;
    status=mgc_lease_create(out.resource,&d,&out.lease);if(status)return status;
    void* data=nullptr;uint64_t capacity=0;status=mgc_lease_map(out.lease,&data,&capacity);if(status)return status;
    if(!data||capacity<length) { auto unmap=mgc_lease_unmap(out.lease);return unmap?unmap:MGC_INTERNAL_ERROR; }
    for(int y=0;y<height;++y)std::memcpy(static_cast<uint8_t*>(data)+uint64_t(y)*row,
        static_cast<const uint8_t*>(bytes)+uint64_t(y)*stride,size_t(row));
    status=mgc_lease_unmap(out.lease);if(status)return status;
    mgc_lease_release(out.lease);out.lease=nullptr;d.access=MGC_ACCESS_READ;
    return mgc_lease_create(out.resource,&d,&out.lease);
}
MgcStatus cached_input(Engine &e,const void *bytes,int width,int height,int stride,int64_t source_id,std::shared_ptr<CachedInput> &out MGR_JNI_OBSERVE_ARG(jlong &observation)){
    uint64_t domain=0,generation=0;auto status=mgc_domain_identity(e.domain,&domain,&generation);if(status)return status;
    if(source_id>=0)for(auto &entry:e.input_cache)if(entry->source_id==source_id&&entry->width==width&&entry->height==height&&entry->stride==stride&&entry->domain_id==domain&&entry->domain_generation==generation){entry->stamp=++e.sequence;out=entry;MGR_JNI_OBSERVE(observation=MGR_CACHE_OBSERVATION_HIT;)return MGC_SUCCESS;}
    MGR_JNI_OBSERVE(observation=source_id<0?MGR_CACHE_OBSERVATION_DISABLED:MGR_CACHE_OBSERVATION_MISS;)
    std::shared_ptr<CachedInput> fresh;
    if(source_id>=0 && e.input_cache.size()==3 && e.sequence<UINT64_MAX-1) {
        auto oldest=std::min_element(e.input_cache.begin(),e.input_cache.end(),[](const auto &a,const auto &b){return a->stamp<b->stamp;});
        const auto& victim=*oldest;
        if(victim.use_count()==1 && victim->width==width && victim->height==height &&
           victim->domain_id==domain && victim->domain_generation==generation && victim->storage.resource && victim->storage.lease) {
            fresh=std::move(*oldest);e.input_cache.erase(oldest);
            fresh->source_id=-1;fresh->stamp=++e.sequence;
            status=reuse_input(e,bytes,width,height,stride,fresh->storage);
            // An unavailable recycled slot must not replace the existing fresh-allocation path.
            // A failed WRITE/map/unmap/READ never restores the old cache identity.
            if(status)fresh.reset();
        }
    }
    if(!fresh) {
        fresh=std::make_shared<CachedInput>();fresh->stamp=++e.sequence;
        status=input(e,bytes,width,height,stride,fresh->storage);if(status)return status;
    }
    fresh->source_id=source_id;fresh->width=width;fresh->height=height;fresh->stride=stride;fresh->domain_id=domain;fresh->domain_generation=generation;
    if(source_id>=0){if(e.input_cache.size()==3){auto oldest=std::min_element(e.input_cache.begin(),e.input_cache.end(),[](const auto &a,const auto &b){return a->stamp<b->stamp;});e.input_cache.erase(oldest);}e.input_cache.push_back(fresh);}out=std::move(fresh);return MGC_SUCCESS;
}
struct Completion {MgcStatus status=MGC_INTERNAL_ERROR;MgrResult *result=nullptr;~Completion(){mgr_result_release(result);}};
void complete(void *p,uint64_t,MgcStatus s,MgrResult *result){auto c=static_cast<Completion*>(p);c->status=s;c->result=result;}
}
#define JNI(name) Java_io_github_jichuo1_meguviz_media_MediaNative_##name
extern "C" JNIEXPORT jlongArray JNICALL JNI(frameCreateNative)(JNIEnv *env,jclass,jint backend,jint gpu,jint threads,jlong pixels){
    // An allocation failure here happens before a runtime, device or registry
    // owner exists. Never allocate the result after publishing device work.
    auto result=env->NewLongArray(2);if(!result)return nullptr;
    if(backend==1){mgc_domain_service(nullptr,0);mgr_uploads_poll(nullptr,0);}
    if((backend!=0&&backend!=1)||gpu<0||gpu>15||threads<1||threads>4||pixels<=0||uint64_t(pixels)>max_frame_pixels)return creation(env,result,INVALID,0);
    int status=ALLOCATION_FAILED;jlong id=0;std::shared_ptr<Engine> e;
    try{
        e=std::make_shared<Engine>();e->creating.store(true,std::memory_order_relaxed);e->max_pixels=pixels;e->backend=backend;e->threads=threads;
        // Publish a bounded, inaccessible owner before any runtime side effect.
        // In particular, registry allocation can no longer discard a live GPU.
        {std::lock_guard<std::mutex> lock(registry_mutex);
            if(registry.size()>=2||next_handle==uint64_t(INT64_MAX))return creation(env,result,BUSY,0);
            id=jlong(++next_handle);registry.emplace(id,e);}
        MgcRuntimeDesc r{};r.header=MGC_HEADER_INIT(MgcRuntimeDesc);r.provider=backend?MGC_PROVIDER_NCNN_VULKAN:MGC_PROVIDER_NCNN_CPU;r.memory_budget_bytes=std::max<uint64_t>(64ULL*1024*1024,uint64_t(pixels)*128);
        auto s=mgc_runtime_create(&r,&e->runtime);
        if(!s){MgcDomainDesc d{};d.header=MGC_HEADER_INIT(MgcDomainDesc);d.device_index=gpu;s=mgc_domain_acquire(e->runtime,&d,&e->domain);}
        if(!s){mgc_domain_retain(e->domain);e->device_status_domain=std::shared_ptr<MgcDomain>(e->domain,mgc_domain_release);}
        status=code(s);if(s==MGC_UNSUPPORTED_CAPABILITY&&backend)status=GPU_UNAVAILABLE;
    }catch(...){status=ALLOCATION_FAILED;}
    auto out=creation(env,result,status,status==OK?id:0);
    if(e&&id){
        if(status!=OK||!out)e->closing.store(true,std::memory_order_release);
        // Background service cannot observe an incomplete Engine, including
        // failure/exception paths; Java requests it in a finally block.
        e->creating.store(false,std::memory_order_release);
    }
    return out;
}
extern "C" JNIEXPORT jlongArray JNICALL JNI(frameCreate)(JNIEnv* env,jclass cls,jint backend,jint gpu,jint threads,jlong pixels){return JNI(frameCreateNative)(env,cls,backend,gpu,threads,pixels);}
// A reserved Engine has no runtime/domain or GPU side effects. Its positive
// primitive receipt is retained by Java before start can touch the device.
extern "C" JNIEXPORT jint JNICALL JNI(frameOwnedCreationProtocolVersionNative)(JNIEnv*,jclass) { return 1; }
extern "C" JNIEXPORT jlong JNICALL JNI(frameReserveNative)(JNIEnv*,jclass) {
    try {
        auto e=std::make_shared<Engine>();e->creating.store(true);e->reservation_state.store(1);e->reserved_at=std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(registry_mutex);
        // Reclaim only empty reservations which never entered start. A delayed
        // owner sees the dedicated expired code and no GPU work was performed.
        const auto now=std::chrono::steady_clock::now();
        for(auto i=registry.begin();i!=registry.end();) {
            auto& old=i->second;int expected=1;
            if(now-old->reserved_at>std::chrono::seconds(60)&&old->reservation_state.compare_exchange_strong(expected,3))i=registry.erase(i);
            else ++i;
        }
        if(registry.size()>=2||next_handle==uint64_t(INT64_MAX))return 0;
        const jlong id=jlong(++next_handle);registry.emplace(id,e);return id;
    } catch (...) { return 0; }
}
extern "C" JNIEXPORT jint JNICALL JNI(frameStartReservedNative)(JNIEnv*,jclass,jlong id,jint backend,jint gpu,jint threads,jlong pixels) {
    std::shared_ptr<Engine> e;
    {std::lock_guard<std::mutex> lock(registry_mutex);auto i=registry.find(id);if(i==registry.end())return 12;e=i->second;
        int expected=1;if(!e->reservation_state.compare_exchange_strong(expected,2))return INVALID;}
    int result=ALLOCATION_FAILED;
    try {
        if((backend!=0&&backend!=1)||gpu<0||gpu>15||threads<1||threads>4||pixels<=0||uint64_t(pixels)>max_frame_pixels)result=INVALID;
        else {
            if(backend==1){mgc_domain_service(nullptr,0);mgr_uploads_poll(nullptr,0);}
            e->max_pixels=pixels;e->backend=backend;e->threads=threads;
            MgcRuntimeDesc r{};r.header=MGC_HEADER_INIT(MgcRuntimeDesc);r.provider=backend?MGC_PROVIDER_NCNN_VULKAN:MGC_PROVIDER_NCNN_CPU;r.memory_budget_bytes=std::max<uint64_t>(64ULL*1024*1024,uint64_t(pixels)*128);
            auto status=mgc_runtime_create(&r,&e->runtime);
            if(!status){MgcDomainDesc d{};d.header=MGC_HEADER_INIT(MgcDomainDesc);d.device_index=gpu;status=mgc_domain_acquire(e->runtime,&d,&e->domain);}
            if(!status){mgc_domain_retain(e->domain);e->device_status_domain=std::shared_ptr<MgcDomain>(e->domain,mgc_domain_release);}
            result=code(status);if(status==MGC_UNSUPPORTED_CAPABILITY&&backend)result=GPU_UNAVAILABLE;
        }
    } catch (...) { result=ALLOCATION_FAILED; }
    // Failure remains explicitly owned and is not placed on the global closing
    // queue until the Java owner starts the exact-handle retirement protocol.
    e->reservation_state.store(result==OK?3:4,std::memory_order_release);e->creating.store(false,std::memory_order_release);return result;
}
extern "C" JNIEXPORT jint JNICALL JNI(frameCancelReservationNative)(JNIEnv*,jclass,jlong id) {
    std::shared_ptr<Engine> e;{std::lock_guard<std::mutex> lock(registry_mutex);auto i=registry.find(id);if(i==registry.end())return 12;e=i->second;}
    int expected=1;
    if(e->reservation_state.compare_exchange_strong(expected,3)){e->creating.store(false,std::memory_order_release);return OK;}
    if(expected==2||e->creating.load(std::memory_order_acquire))return BUSY;
    return expected==3||expected==4?OK:INVALID;
}
// Raw Compute status, separate from the legacy positive frameProcess codes.
// The atomic snapshot keeps the domain alive without waiting for inference.
extern "C" JNIEXPORT jint JNICALL JNI(frameDeviceLossStatus)(JNIEnv*,jclass,jlong id){
    auto e=acquire(id);if(!e)return MGC_INVALID_ARGUMENT;
    auto domain=std::atomic_load(&e->device_status_domain);
    if(!domain)return MGC_BUSY;
    MgcDomainStateInfo state{};state.header=MGC_HEADER_INIT(MgcDomainStateInfo);
    const auto status=mgc_domain_state(domain.get(),&state);
    return status?status:state.lost?MGC_DEVICE_LOST:MGC_SUCCESS;
}
extern "C" JNIEXPORT jint JNICALL JNI(frameConfigure)(JNIEnv*,jclass,jlong id,jint flags){auto e=acquire(id);if(!e)return NOT_LOADED;MGR_JNI_OBSERVE(e->begin_observation();)if(flags&~255)return INVALID;std::unique_lock<std::mutex> lock(e->mutex,std::try_to_lock);if(!lock||e->closing.load())return BUSY;MGR_JNI_OBSERVE(e->begin_observation();)const int flow=(flags&6)==6?8:(flags&4?4:(flags&2?2:1));if(!e->backend&&(flow!=1||(flags&8)||(flags&128)))return UNSUPPORTED;if((flags&128)&&flow!=4)return INVALID;e->clear_plan();mgr_model_release(e->model);e->model=nullptr;e->flags=flags;e->clear_shared_policy();return OK;}
// Additive optional protocol: older DSOs do not export these names.
extern "C" JNIEXPORT jint JNICALL JNI(frameSharedPlanProtocolVersionNative)(JNIEnv*,jclass){
#if defined(MGM_EXPERIMENTAL_SHARED_PLAN)
    return 1;
#else
    return 0;
#endif
}
extern "C" JNIEXPORT jint JNICALL JNI(frameSetSharedPlanPolicyNative)(JNIEnv*,jclass,jlong id,jint policy){
    if(policy!=0 && policy!=1)return INVALID;
    auto e=acquire(id);if(!e)return NOT_LOADED;
    std::unique_lock<std::mutex> lock(e->mutex,std::try_to_lock);if(!lock||e->closing.load())return BUSY;
#if defined(MGM_EXPERIMENTAL_SHARED_PLAN)
    if(e->model||e->plan||e->session)return BUSY;
    if(policy&&(!e->backend||!(e->flags&8)))return UNSUPPORTED;
    e->shared_plan_requested=policy!=0;return OK;
#else
    return policy?UNSUPPORTED:OK;
#endif
}
extern "C" JNIEXPORT jint JNICALL JNI(frameSetMeasurementFlags)(JNIEnv*,jclass,jlong id,jint flags){
    auto e=acquire(id);if(!e)return NOT_LOADED;MGR_JNI_OBSERVE(e->begin_observation();)if(flags<0||(flags&~MGC_MEASURE_GPU_TIMESTAMPS))return INVALID;
    std::unique_lock<std::mutex> lock(e->mutex,std::try_to_lock);if(!lock||e->closing.load())return BUSY;MGR_JNI_OBSERVE(e->begin_observation();)
    const auto status=mgc_domain_set_measurement_flags(e->domain,uint32_t(flags));
    if(!status)e->gpu_measurement={};return code(status);
}
extern "C" JNIEXPORT jlongArray JNICALL JNI(frameLastGpuMeasurement)(JNIEnv* env,jclass,jlong id){
    std::array<jlong,7> value{};auto e=acquire(id);
    if(e){std::unique_lock<std::mutex> lock(e->mutex,std::try_to_lock);if(lock)value=e->gpu_measurement;}
    auto result=env->NewLongArray(jsize(value.size()));
    if(result)env->SetLongArrayRegion(result,0,jsize(value.size()),value.data());return result;
}
extern "C" JNIEXPORT jint JNICALL JNI(frameSetOptimizationFlags)(JNIEnv*,jclass,jlong id,jint flags){
    auto e=acquire(id);if(!e)return NOT_LOADED;MGR_JNI_OBSERVE(e->begin_observation();)if(flags<0||(flags&~7))return INVALID;
    std::unique_lock<std::mutex> lock(e->mutex,std::try_to_lock);if(!lock||e->closing.load())return BUSY;MGR_JNI_OBSERVE(e->begin_observation();)if(flags&&!e->backend)return UNSUPPORTED;
    try{return code(e->reconfigure(uint32_t(flags),e->shaders));}catch(...){return ALLOCATION_FAILED;}
}
extern "C" JNIEXPORT jint JNICALL JNI(frameSetShaderpackBindings)(JNIEnv* env,jclass,jlong id,jbyteArray root,jbyteArray manifest,jbyteArray sdk,jintArray kernels,jbyteArray hashes){
    auto e=acquire(id);if(!e)return NOT_LOADED;MGR_JNI_OBSERVE(e->begin_observation();)std::unique_lock<std::mutex> lock(e->mutex,std::try_to_lock);if(!lock||e->closing.load())return BUSY;MGR_JNI_OBSERVE(e->begin_observation();)
    if(!root&&!manifest&&!sdk&&!kernels&&!hashes){try{return code(e->reconfigure(e->optimization_flags,nullptr));}catch(...){return ALLOCATION_FAILED;}}
    if(!root||!manifest||!sdk||!kernels||!hashes)return INVALID;if(!e->backend)return UNSUPPORTED;
    const jsize path_size=env->GetArrayLength(root),count=env->GetArrayLength(kernels);
    if(path_size<=0||path_size>32768||count<=0||count>64||env->GetArrayLength(manifest)!=32||env->GetArrayLength(sdk)!=32||env->GetArrayLength(hashes)!=count*160)return INVALID;
    try{
        std::string path(size_t(path_size),'\0');MgcHash manifest_hash{},sdk_hash{};std::vector<jint> ids(size_t(count),0);std::vector<uint8_t> data(size_t(count)*160);
        env->GetByteArrayRegion(root,0,path_size,reinterpret_cast<jbyte*>(path.data()));env->GetByteArrayRegion(manifest,0,32,reinterpret_cast<jbyte*>(manifest_hash.bytes));
        env->GetByteArrayRegion(sdk,0,32,reinterpret_cast<jbyte*>(sdk_hash.bytes));env->GetIntArrayRegion(kernels,0,count,ids.data());
        env->GetByteArrayRegion(hashes,0,count*160,reinterpret_cast<jbyte*>(data.data()));if(env->ExceptionCheck())return INVALID;
        if(path[0]!='/'||path.find('\0')!=std::string::npos)return INVALID;
        auto nonzero=[](const MgcHash& h){for(auto byte:h.bytes)if(byte)return true;return false;};
        MgcBuildInfo build{};build.header=MGC_HEADER_INIT(MgcBuildInfo);auto status=mgc_query_build_info(&build);if(status)return code(status);
        if(!nonzero(manifest_hash)||std::memcmp(sdk_hash.bytes,build.private_sdk_fingerprint.bytes,32))return INVALID_MODEL;
        auto config=std::make_shared<ShaderConfiguration>();std::set<jint> unique;
        for(jsize i=0;i<count;i++){
            if(ids[size_t(i)]<1||ids[size_t(i)]>9||!unique.insert(ids[size_t(i)]).second)return INVALID;
            MgrShaderBinding binding{};binding.header=MGC_HEADER_INIT(MgrShaderBinding);binding.kernel=uint32_t(ids[size_t(i)]);binding.private_sdk_fingerprint=sdk_hash;
            const auto* source=data.data()+size_t(i)*160;std::memcpy(binding.variant_key.bytes,source,32);std::memcpy(binding.effective_options_hash.bytes,source+32,32);
            std::memcpy(binding.target_facts_hash.bytes,source+64,32);std::memcpy(binding.source_sha256.bytes,source+96,32);std::memcpy(binding.spirv_sha256.bytes,source+128,32);
            if(!nonzero(binding.variant_key)||!nonzero(binding.effective_options_hash)||!nonzero(binding.target_facts_hash)||!nonzero(binding.source_sha256)||!nonzero(binding.spirv_sha256))return INVALID_MODEL;
            config->bindings.push_back(binding);
        }
        MgcShaderpackDesc desc{};desc.header=MGC_HEADER_INIT(MgcShaderpackDesc);desc.root_path_utf8=path.data();desc.root_path_length=uint32_t(path.size());desc.profile=MGC_SHADER_PROFILE_NCNN_COMPAT;desc.manifest_hash=manifest_hash;
        status=mgc_shaderpack_open(e->domain,&desc,&config->pack);if(status)return code(status);
        return code(e->reconfigure(e->optimization_flags,config));
    }catch(...){return ALLOCATION_FAILED;}
}
extern "C" JNIEXPORT jint JNICALL JNI(frameLoadNative)(JNIEnv *env,jclass,jlong id,jobject param,jobject weights){auto e=acquire(id);if(!e)return NOT_LOADED;MGR_JNI_OBSERVE(e->begin_observation();)std::unique_lock<std::mutex> lock(e->mutex,std::try_to_lock);if(!lock||e->closing.load())return BUSY;MGR_JNI_OBSERVE(e->begin_observation();)void *p=nullptr,*w=nullptr;uint64_t ps=0,ws=0;if(!direct(env,param,&p,ps)||!direct(env,weights,&w,ws))return INVALID;if(ps>1024*1024||ws>32*1024*1024)return MODEL_TOO_LARGE;try{e->clear_plan();mgr_model_release(e->model);e->model=nullptr;MgrModelDesc d{};d.header=MGC_HEADER_INIT(MgrModelDesc);d.manifest_utf8=reinterpret_cast<const uint8_t*>(mgm_model_catalog);d.manifest_size=std::strlen(mgm_model_catalog);d.graph_data=static_cast<const uint8_t*>(p);d.graph_size=ps;d.weights_data=static_cast<const uint8_t*>(w);d.weights_size=ws;d.graph_sha256=digest("724569596bcd1e7b9fa50455c604777ebed99746d2ef40aa86e31b5725f1053c");d.weights_sha256=digest("f334ed2260149ce0188a6dcf049844e8b0cdd912e01cbcfb63553157d2508958");MgcBuildInfo b{};b.header=MGC_HEADER_INIT(MgcBuildInfo);auto s=mgc_query_build_info(&b);if(s)return code(s);d.private_sdk_fingerprint=b.private_sdk_fingerprint;s=mgr_model_create(e->domain,&d,&e->model);if(s)return code(s);s=e->prepare();return s==MGC_MODEL_MISMATCH?MODEL_LOAD_FAILED:code(s);}catch(...){return ALLOCATION_FAILED;}}
extern "C" JNIEXPORT jint JNICALL JNI(frameLoad)(JNIEnv *env,jclass cls,jlong id,jobject param,jobject weights){return JNI(frameLoadNative)(env,cls,id,param,weights);}
extern "C" JNIEXPORT jint JNICALL JNI(frameProcess)(JNIEnv *env,jclass,jlong id,jobject left,jobject right,jobject output,jint width,jint height,jint ls,jint rs,jint os,jfloat alpha,jlong left_id,jlong right_id,jint flow){auto e=acquire(id);if(!e)return NOT_LOADED;MGR_JNI_OBSERVE(const uint64_t observationTicket=e->begin_observation();)std::unique_lock<std::mutex> lock(e->mutex,std::try_to_lock);if(!lock||e->closing.load())return BUSY;MGR_JNI_OBSERVE(const uint64_t observationGeneration=e->admit_observation(observationTicket);)e->last_kind=0;e->last_scale=0;e->last_worker_cpu=-1;e->gpu_measurement={};MGR_JNI_OBSERVE(e->observation_host={};e->observation={};e->observation.header=MGC_HEADER_INIT(MgrJobObservation);e->observation_left=left_id;e->observation_right=right_id;)for(auto &stage:e->stages)stage=0;if(!e->model)return NOT_LOADED;void *l=nullptr,*r=nullptr,*o=nullptr;uint64_t ln=0,rn=0,on=0;if(!direct(env,left,&l,ln)||!direct(env,right,&r,rn)||!direct(env,output,&o,on,true)||!shape(width,height,ls,ln)||!shape(width,height,rs,rn)||!shape(width,height,os,on)||uint64_t(width)*height>e->max_pixels||!mgm_detail::valid_alpha_float(alpha)||flow<0)return INVALID;
    try{if(e->plan_dirty){auto s=e->prepare();if(s)return code(s);}if(!e->session)return NOT_LOADED;std::shared_ptr<CachedInput> a,b;auto s=cached_input(*e,l,width,height,ls,left_id,a MGR_JNI_OBSERVE_ARG(e->observation_host[0]));if(s)return code(s);s=cached_input(*e,r,width,height,rs,right_id,b MGR_JNI_OBSERVE_ARG(e->observation_host[1]));if(s)return code(s);MgrRequest request{};request.header=MGC_HEADER_INIT(MgrRequest);request.request_id=++e->sequence;request.left=a->storage.lease;request.right=b->storage.lease;request.alpha=mgm_detail::alpha_from_float(alpha);request.flow_scale=flow;request.output.header=MGC_HEADER_INIT(MgrOutputSpec);request.output.width=width;request.output.height=height;request.output.format=MGC_FORMAT_RGB8;request.output.row_stride_bytes=uint64_t(width)*3;Completion c;MgcTicket *ticket=nullptr;mgc_lease_retain(a->storage.lease);mgc_lease_retain(b->storage.lease);s=mgr_submit(e->session,&request,complete,&c,nullptr,nullptr,&ticket);if(s!=MGC_ACCEPTED){mgc_lease_release(a->storage.lease);mgc_lease_release(b->storage.lease);return code(s);}s=mgr_drain(e->session,UINT64_MAX);mgc_ticket_release(ticket);if(s)return code(s);if(c.status)return code(c.status);MgrResultInfo result{};result.header=MGC_HEADER_INIT(MgrResultInfo);s=mgr_result_describe(c.result,&result);if(s||!result.algorithm_accepted||!result.output)return INVALID_OUTPUT;MGR_JNI_OBSERVE(const bool observationReady=mgr_result_observation(c.result,&e->observation)==MGC_SUCCESS;)void *data=nullptr;uint64_t capacity=0;s=mgc_lease_map(result.output,&data,&capacity);if(s)return code(s);MgcLeaseInfo info{};info.header=MGC_HEADER_INIT(MgcLeaseInfo);s=mgc_lease_info(result.output,&info);if(!s&&shape(width,height,int(info.row_stride_bytes),capacity)){for(int y=0;y<height;++y)std::memcpy(static_cast<uint8_t*>(o)+uint64_t(y)*os,static_cast<const uint8_t*>(data)+uint64_t(y)*info.row_stride_bytes,uint64_t(width)*3);}else s=MGC_INTERNAL_ERROR;mgc_lease_unmap(result.output);if(s)return INVALID_OUTPUT;e->last_worker_cpu=result.worker_cpu_observed?result.worker_cpu_ns:-1;e->last_kind=result.frame_kind==MGR_RESULT_INFERRED?1:(result.frame_kind==MGR_RESULT_ENDPOINT?2:0);e->last_scale=e->last_kind.load()==1?result.effective_flow_scale:0;e->capture_gpu_measurement(result);e->unreliable=result.unreliable_permille;e->patched=result.patched_tiles;e->histogram=result.histogram_permille;for(size_t i=0;i<3;++i)e->stages[i]=e->last_kind.load()==1?result.stage_ns[i]:0;MGR_JNI_OBSERVE(if(observationReady&&e->observation.request_id==request.request_id&&e->observation.frame_kind==result.frame_kind&&e->observation.request_id<=uint64_t(INT64_MAX)&&((result.frame_kind==MGR_RESULT_ENDPOINT&&e->observation.effective_flow_scale==0)||(result.frame_kind==MGR_RESULT_INFERRED&&e->observation.effective_flow_scale==result.effective_flow_scale)))e->publish_observation(observationGeneration);)return OK;}catch(...){return ALLOCATION_FAILED;}
}
extern "C" JNIEXPORT jint JNICALL JNI(frameTryClose)(JNIEnv*,jclass,jlong id){
    try{return request_close(id);}catch(...){return BUSY;}
}
// Kept for an older matching Java facade. New facades use frameTryClose and
// explicitly service BUSY closures rather than discarding native ownership.
extern "C" JNIEXPORT void JNICALL JNI(frameClose)(JNIEnv*,jclass,jlong id){try{request_close(id);}catch(...){}}
extern "C" JNIEXPORT jint JNICALL JNI(frameServiceUploads)(JNIEnv*,jclass,jlong id,jlong wait_ns){
    if(wait_ns<0||wait_ns>100000000)return INVALID;
    try{
        if(id){
            auto engine=acquire(id,true);if(!engine)return NOT_LOADED;
            if(engine->closing.load())return request_close(id);
            std::unique_lock<std::mutex> lock(engine->mutex,std::try_to_lock);if(!lock)return BUSY;
            if(!engine->backend||!engine->domain)return OK;
            const auto core=mgc_domain_service(engine->domain,uint64_t(wait_ns));
            return core?code(core):code(mgr_uploads_poll(engine->domain,uint64_t(wait_ns)));
        }
        bool waiting=false;
#if defined(__ANDROID__)
        waiting|=mgm_jni_service_android_retirement()!=MGC_SUCCESS;
#endif
        waiting|=mgc_domain_service(nullptr,uint64_t(wait_ns))!=MGC_SUCCESS;
        waiting|=mgr_uploads_poll(nullptr,uint64_t(wait_ns))!=MGC_SUCCESS;
        std::array<jlong,2> closing{};size_t count=0;
        {std::lock_guard<std::mutex> lock(registry_mutex);
            for(const auto& item:registry){if(item.second->creating.load(std::memory_order_acquire))waiting=true;
                else if(item.second->closing.load())closing[count++]=item.first;}}
        for(size_t i=0;i<count;++i){const auto status=request_close(closing[i]);waiting|=status!=OK&&status!=NOT_LOADED;}
        return waiting?BUSY:OK;
    }catch(...){return BUSY;}
}
extern "C" JNIEXPORT void JNICALL JNI(frameSetMotionGuard)(JNIEnv*,jclass,jlong id,jboolean enabled){auto e=acquire(id);if(!e)return;std::lock_guard<std::mutex> lock(e->mutex);if(e->motion!=(enabled==JNI_TRUE)){MGR_JNI_OBSERVE(e->begin_observation();)e->motion=enabled==JNI_TRUE;e->plan_dirty=true;}}
extern "C" JNIEXPORT jint JNICALL JNI(frameLastUnreliablePermille)(JNIEnv*,jclass,jlong id){auto e=acquire(id);return e?e->unreliable.load():0;}
extern "C" JNIEXPORT jint JNICALL JNI(frameLastPatchedTiles)(JNIEnv*,jclass,jlong id){auto e=acquire(id);return e?e->patched.load():0;}
extern "C" JNIEXPORT jint JNICALL JNI(frameLastHistogramPermille)(JNIEnv*,jclass,jlong id){auto e=acquire(id);return e?e->histogram.load():0;}
extern "C" JNIEXPORT jlongArray JNICALL JNI(frameLastStageNanos)(JNIEnv *env,jclass,jlong id){auto e=acquire(id);jlong v[3]{};if(e)for(size_t i=0;i<3;++i)v[i]=e->stages[i].load();auto out=env->NewLongArray(3);if(out)env->SetLongArrayRegion(out,0,3,v);return out;}
extern "C" JNIEXPORT jint JNICALL JNI(frameLastFrameKind)(JNIEnv*,jclass,jlong id){auto e=acquire(id);return e?e->last_kind.load():0;}
extern "C" JNIEXPORT jint JNICALL JNI(frameLastEffectiveFlowScale)(JNIEnv*,jclass,jlong id){auto e=acquire(id);return e?e->last_scale.load():0;}
extern "C" JNIEXPORT jlong JNICALL JNI(frameLastWorkerCpuNanos)(JNIEnv*,jclass,jlong id){auto e=acquire(id);return e?e->last_worker_cpu.load():-1;}
extern "C" JNIEXPORT jlongArray JNICALL JNI(frameJobObservation)(JNIEnv *env,jclass,jlong id){
    jlong fields[19]{};
#if defined(MGR_ENABLE_JOB_OBSERVATION)
    auto e=acquire(id);
    if(e){
        std::unique_lock<std::mutex> lock(e->mutex,std::try_to_lock);
        if(lock){
            const auto state=e->observation_state.load(std::memory_order_acquire);
            const auto generation=state&~uint64_t(1);
            if(state!=UINT64_MAX && generation<=uint64_t(INT64_MAX)){
                fields[1]=jlong(generation);
                if((state&1)&&!e->plan_dirty){
                    const auto& o=e->observation;
                    fields[0]=1;fields[2]=1;fields[3]=jlong(o.request_id);
                    fields[4]=jlong(e->observation_left);fields[5]=jlong(e->observation_right);
                    fields[6]=e->observation_host[0];fields[7]=e->observation_host[1];
                    fields[8]=o.frame_kind;fields[9]=o.effective_flow_scale;fields[10]=o.cache_lookup_scale;
                    fields[11]=o.static_guard_dispatch;fields[12]=o.motion_guard_dispatch;
                    fields[13]=o.tensor_cache_left;fields[14]=o.tensor_cache_right;
                    fields[15]=o.bridge_cache_left;fields[16]=o.bridge_cache_right;
                    fields[17]=o.tensor_cache_publish_stage_completed;fields[18]=o.tensor_cache_published_count;
                    if(e->observation_state.load(std::memory_order_acquire)!=state)std::fill(std::begin(fields),std::end(fields),jlong(0));
                }
            }
        }
    }
#else
    (void)id;
#endif
    auto result=env->NewLongArray(19);
    if(result)env->SetLongArrayRegion(result,0,19,fields);
    return result;
}
extern "C" JNIEXPORT jstring JNICALL JNI(frameExecutionInfo)(JNIEnv *env,jclass,jlong id){
    auto e=acquire(id);if(!e)return env->NewStringUTF("{\"schema_version\":1,\"effective_options_observed\":false,\"frame_kind\":0}");
    std::unique_lock<std::mutex> lock(e->mutex,std::try_to_lock);if(!lock||!e->plan||e->plan_dirty)return env->NewStringUTF("{\"schema_version\":1,\"effective_options_observed\":false,\"frame_kind\":0}");
    try{const char *bytes=nullptr;uint64_t size=0;auto status=mgr_plan_execution_info(e->plan,&bytes,&size);if(status||!bytes||size>1024*1024)return env->NewStringUTF("{\"schema_version\":1,\"effective_options_observed\":false,\"frame_kind\":0}");
        // This short public JSON contains only fixed option keys, booleans and
        // integers. Java's own JSON implementation overlays per-job metadata;
        // Media never reaches into RIFE's private parser or ncnn objects.
        std::string json(bytes,size);const jint kind=e->last_kind.load(),scale=e->last_scale.load();const jlong worker_cpu=e->last_worker_cpu.load();std::array<jlong,3> observed_stages{};for(size_t i=0;i<3;++i)observed_stages[i]=e->stages[i].load();lock.unlock();if(env->PushLocalFrame(24)<0)return nullptr;
        jclass cls=env->FindClass("org/json/JSONObject");if(!cls)return static_cast<jstring>(env->PopLocalFrame(nullptr));
        auto method=[&](jclass type,const char *name,const char *signature)->jmethodID {return env->ExceptionCheck()?nullptr:env->GetMethodID(type,name,signature);};auto constructor=method(cls,"<init>","(Ljava/lang/String;)V");auto put_int=method(cls,"put","(Ljava/lang/String;I)Lorg/json/JSONObject;");auto put_object=method(cls,"put","(Ljava/lang/String;Ljava/lang/Object;)Lorg/json/JSONObject;");auto text=method(cls,"toString","()Ljava/lang/String;");
        if(!constructor||!put_int||!put_object||!text)return static_cast<jstring>(env->PopLocalFrame(nullptr));
        auto json_text=env->NewStringUTF(json.c_str());if(!json_text)return static_cast<jstring>(env->PopLocalFrame(nullptr));auto owner=env->NewObject(cls,constructor,json_text);if(!owner)return static_cast<jstring>(env->PopLocalFrame(nullptr));
        auto kind_key=env->NewStringUTF("frame_kind");if(!kind_key)return static_cast<jstring>(env->PopLocalFrame(nullptr));env->CallObjectMethod(owner,put_int,kind_key,kind);if(env->ExceptionCheck())return static_cast<jstring>(env->PopLocalFrame(nullptr));auto scale_key=env->NewStringUTF("effective_flow_scale");if(!scale_key)return static_cast<jstring>(env->PopLocalFrame(nullptr));env->CallObjectMethod(owner,put_int,scale_key,scale);if(env->ExceptionCheck())return static_cast<jstring>(env->PopLocalFrame(nullptr));
        jclass array=env->FindClass("org/json/JSONArray");if(!array)return static_cast<jstring>(env->PopLocalFrame(nullptr));auto array_constructor=method(array,"<init>","()V");auto put_long=method(array,"put","(J)Lorg/json/JSONArray;");if(!array_constructor||!put_long)return static_cast<jstring>(env->PopLocalFrame(nullptr));auto stages=env->NewObject(array,array_constructor);if(!stages)return static_cast<jstring>(env->PopLocalFrame(nullptr));for(size_t i=0;i<3;++i){env->CallObjectMethod(stages,put_long,observed_stages[i]);if(env->ExceptionCheck())return static_cast<jstring>(env->PopLocalFrame(nullptr));}auto stages_key=env->NewStringUTF("stage_ns");if(!stages_key)return static_cast<jstring>(env->PopLocalFrame(nullptr));env->CallObjectMethod(owner,put_object,stages_key,stages);if(env->ExceptionCheck())return static_cast<jstring>(env->PopLocalFrame(nullptr));auto cpu_key=env->NewStringUTF("worker_cpu_ns");if(!cpu_key)return static_cast<jstring>(env->PopLocalFrame(nullptr));auto put_long_value=method(cls,"put","(Ljava/lang/String;J)Lorg/json/JSONObject;");if(!put_long_value)return static_cast<jstring>(env->PopLocalFrame(nullptr));env->CallObjectMethod(owner,put_long_value,cpu_key,worker_cpu);if(env->ExceptionCheck())return static_cast<jstring>(env->PopLocalFrame(nullptr));auto cpu_observed_key=env->NewStringUTF("worker_cpu_observed");if(!cpu_observed_key)return static_cast<jstring>(env->PopLocalFrame(nullptr));auto put_bool=method(cls,"put","(Ljava/lang/String;Z)Lorg/json/JSONObject;");if(!put_bool)return static_cast<jstring>(env->PopLocalFrame(nullptr));env->CallObjectMethod(owner,put_bool,cpu_observed_key,jboolean(worker_cpu>=0));if(env->ExceptionCheck())return static_cast<jstring>(env->PopLocalFrame(nullptr));auto result=static_cast<jstring>(env->CallObjectMethod(owner,text));return static_cast<jstring>(env->PopLocalFrame(result));
    }catch(...){return env->NewStringUTF("{\"schema_version\":1,\"effective_options_observed\":false,\"frame_kind\":0}");}
}

extern "C" JNIEXPORT jint JNICALL JNI(isDuplicate)(JNIEnv *env,jclass,jobject left,jobject right,jint w,jint h,jint stride){void *a=nullptr,*b=nullptr;uint64_t an=0,bn=0;if(!direct(env,left,&a,an)||!direct(env,right,&b,bn)||!shape(w,h,stride,an)||!shape(w,h,stride,bn))return -1;auto p=static_cast<const uint8_t*>(a),q=static_cast<const uint8_t*>(b);const int sx=std::max(1,w/320),sy=std::max(1,h/180);uint64_t sum=0,count=0,noisy=0;for(int y=0;y<h;y+=sy)for(int x=0;x<w;x+=sx)for(int c=0;c<3;++c){int d=std::abs(int(p[uint64_t(y)*stride+x*3+c])-int(q[uint64_t(y)*stride+x*3+c]));if(d>16)return 0;if(d>4)++noisy;sum+=d;++count;}return count&&sum*10<=count*3&&noisy*200<=count?1:0;}
extern "C" JNIEXPORT jint JNICALL JNI(analyzeScene)(JNIEnv *env,jclass,jobject left,jobject right,jint w,jint h,jint stride){void *a=nullptr,*b=nullptr;uint64_t an=0,bn=0;if(!direct(env,left,&a,an)||!direct(env,right,&b,bn)||!shape(w,h,stride,an)||!shape(w,h,stride,bn))return -1;auto p=static_cast<const uint8_t*>(a),q=static_cast<const uint8_t*>(b);uint64_t sum=0,count=0;bool still=true;int step=std::max(4,w/320);for(int y=0;y<h&&still;y+=step)for(int x=0;x<w&&still;x+=step)for(int c=0;c<3;++c){int d=std::abs(int(p[uint64_t(y)*stride+x*3+c])-int(q[uint64_t(y)*stride+x*3+c]));if(d>6){still=false;break;}sum+=d;++count;}if(still&&count&&sum*2<=count)return 1;int bins[48]{};const int cols=std::min(w,64),rows=std::min(h,36);uint64_t total=0,changed=0;for(int gy=0;gy<rows;++gy)for(int gx=0;gx<cols;++gx){int y=int((gy*2LL+1)*h/(rows*2LL)),x=int((gx*2LL+1)*w/(cols*2LL));uint64_t i=uint64_t(y)*stride+x*3;int d=0;for(int c=0;c<3;++c){++bins[c*16+(p[i+c]>>4)];--bins[c*16+(q[i+c]>>4)];d+=std::abs(int(p[i+c])-int(q[i+c]));}total+=d;if(d>=120)++changed;}uint64_t hist=0;for(int v:bins)hist+=std::abs(v);uint64_t samples=uint64_t(cols)*rows;return changed*10>=samples*7&&total>=samples*135&&hist*20>=samples*6*9?2:0;}
extern "C" JNIEXPORT jint JNICALL JNI(rgbaToRgb)(JNIEnv *env,jclass,jobject rgba,jint w,jint h,jobject output,jint stride){void *a=nullptr,*b=nullptr;uint64_t an=0,bn=0;if(!direct(env,rgba,&a,an)||!direct(env,output,&b,bn,true)||w<=0||h<=0||uint64_t(w)*h>max_frame_pixels||uint64_t(w)*h*4>an||!shape(w,h,stride,bn))return INVALID;auto p=static_cast<const uint8_t*>(a);auto q=static_cast<uint8_t*>(b);for(int y=0;y<h;++y)for(int x=0;x<w;++x)std::memcpy(q+uint64_t(y)*stride+x*3,p+(uint64_t(y)*w+x)*4,3);return OK;}

/* Internal composition glue: domain/model still originate from the one public
 * RIFE model owner. No ncnn object or header crosses into this JNI adapter. */
MgcStatus mgm_jni_create_ahb_effect(jlong id,uint32_t width,uint32_t height,MgcDomain **domain,MgmEffect **effect,MgmEffectApi *api,std::string *execution_info){
    *domain=nullptr;*effect=nullptr;auto e=acquire(id);if(!e)return MGC_INVALID_ARGUMENT;std::unique_lock<std::mutex> lock(e->mutex,std::try_to_lock);if(!lock||e->closing.load())return MGC_BUSY;if(!e->model||!e->backend||!(e->flags&8))return MGC_UNSUPPORTED_CAPABILITY;
    const MgrPlanDesc d=e->describe_plan(MGC_FORMAT_RGBA8,width,height,false,e->optimization_flags,e->shaders);
    MgrPlan *plan=nullptr;MgcStatus s=MGC_UNSUPPORTED_CAPABILITY;
#if defined(MGM_EXPERIMENTAL_SHARED_PLAN)
    if(e->shared_plan_requested && e->plan && e->session && !e->plan_dirty &&
       width>0 && height>0 && width<=4096 && height<=4096 && uint64_t(width)*height<=e->max_pixels) {
        s=mgr_private_enable_shared_ahb(e->plan);
        if(s==MGC_SUCCESS) {
            s=mgm_private_rife_effect_shared(e->plan,effect,api);
            if(s){mgr_private_release_shared_ahb(e->plan);return s;}
            plan=e->plan;mgr_plan_retain(plan);
        } else if(s!=MGC_BUSY && s!=MGC_UNSUPPORTED_CAPABILITY) return s;
    }
#endif
    if(!plan){s=mgr_plan_create(e->model,&d,&plan);if(s)return s;s=mgm_rife_effect_create(plan,effect,api);

    }

    // Copy the active plan's public snapshot while the plan is alive. In shared
    // mode this is the shared RGB plan the AHB effect runs on; otherwise the new
    // RGBA plan. Missing diagnostics must never prevent creation of a valid effect.
    if(!s&&execution_info){try{const char *bytes=nullptr;uint64_t size=0;if(!mgr_plan_execution_info(plan,&bytes,&size)&&bytes&&size&&size<=1024*1024&&!std::memchr(bytes,0,size))execution_info->assign(bytes,size);}catch(...){execution_info->clear();}}
    mgr_plan_release(plan);if(!s){*domain=e->domain;mgc_domain_retain(*domain);}return s;
}
