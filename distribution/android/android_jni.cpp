#include <jni.h>
#include <meguviz/media_android.h>
#include <meguviz/media_rife.h>
#include "../../src/alpha.hpp"
#include "../../src/android/thread_identity.hpp"
#include "../../src/android/creation_guard.hpp"
#include <map>
#include <memory>
#include <string>
#include <mutex>
#include <vector>
#include <cmath>
#include <thread>
#include <array>
#include <atomic>
#include <algorithm>
#include <android/hardware_buffer.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>
MgcStatus mgm_jni_create_ahb_effect(jlong,uint32_t,uint32_t,MgcDomain **,MgmEffect **,MgmEffectApi *,std::string *);
namespace {
using JniGuards=MgmCreationGuards<>;
using JniGuard=JniGuards::Handle;
std::mutex creation_guards_mutex;
std::atomic<JniGuards*> creation_guards{nullptr};
struct ReadyGuard {JniGuard guard;uint64_t reservation=0;bool preparing=false,discard=false;};
std::array<ReadyGuard,2> ready_guards{};
// Loader work is always performed by the background caller. The caller owns
// a separate JVM/host execution reference through preparation and collection.
MgcStatus prepare_jni_guard(){
    size_t slot=ready_guards.size();JniGuards* pool=nullptr;JniGuard guard;
    {std::lock_guard<std::mutex> lock(creation_guards_mutex);
        for(const auto& ready:ready_guards)if(ready.guard&&ready.reservation&&!ready.preparing)return MGC_SUCCESS;
        pool=creation_guards.load(std::memory_order_relaxed);
        if(!pool){pool=JniGuards::create();if(!pool)return MGC_OUT_OF_MEMORY;creation_guards.store(pool,std::memory_order_release);}
        for(size_t i=0;i<ready_guards.size();++i)if(!ready_guards[i].preparing&&!ready_guards[i].reservation){slot=i;ready_guards[i].preparing=true;guard=std::move(ready_guards[i].guard);break;}
    }
    if(slot==ready_guards.size())return MGC_BACKPRESSURE;
    MgcStatus status=MGC_SUCCESS;uint64_t reservation=0;
    if(!guard)status=pool->prepare(reinterpret_cast<const void*>(&prepare_jni_guard),"meguviz_code_image_anchor_v1",guard);
    if(!status){status=mgm_android_creation_reserve(&reservation);if(!status&&!reservation)status=MGC_INTERNAL_ERROR;}
    bool discard=false;
    {std::lock_guard<std::mutex> lock(creation_guards_mutex);auto& ready=ready_guards[slot];discard=ready.discard;ready.discard=false;
        if(!discard){ready.guard=std::move(guard);ready.reservation=reservation;ready.preparing=false;}}
    if(discard){
        const auto released=reservation?mgm_android_creation_release_reservation(reservation):MGC_SUCCESS;
        {std::lock_guard<std::mutex> lock(creation_guards_mutex);auto& ready=ready_guards[slot];
            if(released){ready.guard=std::move(guard);ready.reservation=reservation;}ready.discard=false;ready.preparing=false;}
        if(released)return released;
        guard.reset();pool->collect();return MGC_BUSY;
    }
    return status;
}
MgcStatus take_jni_guard(JniGuard& out,uint64_t& reservation){
    std::unique_lock<std::mutex> lock(creation_guards_mutex,std::try_to_lock);if(!lock)return MGC_BUSY;
    for(auto& slot:ready_guards)if(slot.guard&&slot.reservation&&!slot.preparing){out=std::move(slot.guard);reservation=slot.reservation;slot.reservation=0;return MGC_SUCCESS;}
    return MGC_BUSY;
}
MgcStatus collect_jni_guards(){auto* pool=creation_guards.load(std::memory_order_acquire);if(!pool)return MGC_SUCCESS;
    const auto status=pool->collect();return status==MGC_BUSY?MGC_SUCCESS:status;}
int64_t now(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return int64_t(t.tv_sec)*1000000000+t.tv_nsec;}
uint64_t identity_budget(uint32_t width,uint32_t height,uint32_t capacity){
    AHardwareBuffer_Desc request{};request.width=width;request.height=height;request.layers=1;request.format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;request.usage=AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE|AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;AHardwareBuffer *buffer=nullptr;
    uint64_t stride=width;if(!AHardwareBuffer_allocate(&request,&buffer)){AHardwareBuffer_Desc actual{};AHardwareBuffer_describe(buffer,&actual);stride=actual.stride;AHardwareBuffer_release(buffer);}
    return std::max<uint64_t>(64ULL*1024*1024,stride*height*4*(capacity+3)*2);
}
enum AsyncStage { ASYNC_RESERVED=0,ASYNC_PLANNING=1,ASYNC_PLAN_READY=2,ASYNC_GL_BEGIN=3,
    ASYNC_GL_READY=4,ASYNC_IMPORTING=5,ASYNC_IMPORTED=6,ASYNC_LIVE=7,ASYNC_CANCELLED=8,ASYNC_FAILED=9,ASYNC_COMMITTING=10,ASYNC_CLOSING=11 };
struct Session {
    std::thread::id thread=std::this_thread::get_id();
    uint64_t thread_token=mgm_thread_token();
    std::atomic<MgmAndroidSession*> platform{nullptr};MgcRuntime *runtime=nullptr;MgcDomain *domain=nullptr;MgmEffect *effect=nullptr;MgmEffectApi api{};bool identity=false;std::string execution_info;JniGuard code_guard;std::atomic<uint64_t> platform_reservation{0};
    struct Job {uint64_t epoch;int64_t start,deadline;bool cancelled;uint64_t retired_ns=0;MgmEvent input_identity{};};
    struct Ready {
        uint64_t job=0,epoch=0;MgcStatus status=MGC_INTERNAL_ERROR;MgmFrame *output=nullptr;
        // CLOCK_MONOTONIC matches SystemClock.uptimeNanos; completion runs on the measured session worker.
        int64_t work_start_ns=0,work_duration_ns=0;int32_t worker_tid=0;
    };
    uint32_t sync_mode=0;MgmAdmission *admission=nullptr;std::atomic<uint64_t> epoch{1};std::atomic<bool> device_lost{false},closing{false},creating{false},unreported{false};std::atomic<MgcStatus> final_domain_status{MGC_BUSY};uint64_t sequence=0;std::map<jlong,MgmFrame*> frames;
    bool async=false;std::atomic<int> async_stage{0};std::atomic<bool> async_cancel{false};std::atomic<MgcStatus> async_error{MGC_SUCCESS};
    int async_width=0,async_height=0,async_source_width=0,async_source_height=0,async_capacity=0,async_path=0;bool async_admission=false;
    std::mutex completion_mutex,platform_mutex,domain_mutex;std::map<uint64_t,Job> jobs;std::array<Ready,3> ready{};uint32_t ready_head=0,ready_count=0;
    std::array<jlong,32> requested_frame_releases{};std::array<MgmFrame*,64> pending_frame_releases{};
    std::atomic<bool> retirement_backpressure{false},platform_closed{false},finalizing{false},finalized{false},finalization_uncertain{false};
    std::atomic<uint32_t> platform_services{0};std::atomic<MgcStatus> finalization_status{MGC_BUSY};
    struct Observation {uint64_t sequence;MgmEvent value;};
    std::atomic<bool> event_enabled{false};std::mutex event_mutex;std::array<Observation,512> events{};
    uint64_t event_sequence=0,event_dropped=0,session_id=0,domain_id=0,domain_generation=0;uint32_t event_head=0,event_count=0;
    void observe(const MgmEvent &event){if(!event_enabled.load())return;std::lock_guard<std::mutex> lock(event_mutex);uint64_t sequence=++event_sequence;if(event_count==events.size()){++event_dropped;return;}events[(event_head+event_count)%events.size()]={sequence,event};++event_count;}
    ~Session(){for(auto &p:frames)mgm_frame_release(p.second);for(auto &r:ready)mgm_frame_release(r.output);if(platform)mgm_android_session_close(platform);if(effect){api.drain(effect,UINT64_MAX);if(identity)mgm_identity_effect_release(effect);else mgm_rife_effect_release(effect);}mgc_domain_release(domain);if(runtime){mgc_runtime_close(runtime);mgc_runtime_release(runtime);}mgm_admission_release(admission);}
};
std::mutex mutex;std::map<jlong,std::shared_ptr<Session>> sessions;jlong sequence=0;
std::shared_ptr<Session> acquire(jlong id,bool allow_closing=false){std::lock_guard<std::mutex> lock(mutex);auto i=sessions.find(id);return i==sessions.end()||i->second->creating.load(std::memory_order_acquire)||!i->second->thread_token||i->second->thread_token!=mgm_thread_token()||i->second->thread!=std::this_thread::get_id()||(!allow_closing&&i->second->closing.load())?nullptr:i->second;}
std::shared_ptr<Session> acquire_close(jlong id,bool& busy){std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);if(!lock){busy=true;return nullptr;}
    auto i=sessions.find(id);if(i==sessions.end())return nullptr;auto s=i->second;
    if(s->creating.load(std::memory_order_acquire)){busy=true;return nullptr;}
    int claimed=-1;
    if(s->async){int stage=s->async_stage.load(std::memory_order_acquire);
        for(;;){if(stage==ASYNC_CLOSING)break;
            if(stage==ASYNC_PLANNING||stage==ASYNC_GL_BEGIN||stage==ASYNC_IMPORTING||stage==ASYNC_COMMITTING){busy=true;return nullptr;}
            const int previous=stage;if(s->async_stage.compare_exchange_weak(stage,ASYNC_CLOSING,std::memory_order_acq_rel)){claimed=previous;break;}}}
    if(!s->thread_token||s->thread_token!=mgm_thread_token()||s->thread!=std::this_thread::get_id()){
        if(claimed>=0)s->async_stage.store(claimed,std::memory_order_release);return nullptr;}
    return s;}
jlongArray values(JNIEnv *env,std::initializer_list<jlong> v){auto out=env->NewLongArray(v.size());if(!out)return nullptr;
    env->SetLongArrayRegion(out,0,v.size(),v.begin());return env->ExceptionCheck()?nullptr:out;}
MgmFrame *frame(Session &s,jlong id){auto p=s.frames.find(id);return p==s.frames.end()?nullptr:p->second;}
struct Result {MgcStatus status=MGC_INTERNAL_ERROR;MgmFrame *output=nullptr;};
void completed(void *p,uint64_t,MgcStatus status,MgmFrame *output){auto r=static_cast<Result*>(p);r->status=status;r->output=output;}
void native_observation(void *p,const MgmEvent *event){if(p&&event)static_cast<Session*>(p)->observe(*event);}
MgmEvent observation(Session &session,uint32_t kind,uint64_t task,uint64_t epoch,MgcStatus status,MgmFrame *frame=nullptr){
    MgmEvent event{};event.header=MGC_HEADER_INIT(MgmEvent);event.kind=kind;event.time_domain=MGM_CLOCK_CPU_MONOTONIC;event.timestamp_ns=now();event.session_id=session.session_id;event.epoch=epoch;event.task_id=task;event.result_status=status;event.domain_id=session.domain_id;event.domain_generation=session.domain_generation;
    if(frame){MgmFrameDesc d{};d.header=MGC_HEADER_INIT(MgmFrameDesc);MgcLeaseInfo info{};info.header=MGC_HEADER_INIT(MgcLeaseInfo);if(!mgm_frame_describe(frame,&d)&&!mgc_lease_info(d.content,&info)){event.source_id=d.source_id;event.content_id=info.content_id;event.allocation_id=info.allocation_id;event.allocation_generation=info.allocation_generation;event.domain_id=info.domain_id;event.domain_generation=info.domain_generation;event.frame_kind=d.frame_kind;}}
    return event;
}
bool inference_sample(MgmFrame *frame){if(!frame)return false;MgmFrameDesc d{};d.header=MGC_HEADER_INIT(MgmFrameDesc);return !mgm_frame_describe(frame,&d)&&d.frame_kind==MGM_FRAME_INFERRED;}
// This is a device-generation signal, not a frame completion. Never clear it
// on epoch changes, cancellation, stale output or an uncertain GL retirement.
MgcStatus session_device_loss(Session& session){
    if(session.device_lost.load())return MGC_DEVICE_LOST;
    std::unique_lock<std::mutex> lock(session.domain_mutex,std::try_to_lock);if(!lock)return MGC_BUSY;
    if(!session.domain)return session.closing.load()?session.final_domain_status.load():MGC_INVALID_ARGUMENT;
    MgcDomainStateInfo state{};state.header=MGC_HEADER_INIT(MgcDomainStateInfo);
    const auto status=mgc_domain_state(session.domain,&state);
    if(status)return status;
    if(state.lost){session.device_lost=true;return MGC_DEVICE_LOST;}
    return MGC_SUCCESS;
}
struct AsyncContext {std::atomic<uint32_t> refs{1};std::shared_ptr<Session> session;};
void retain_async(void *p){++static_cast<AsyncContext*>(p)->refs;}
void release_async(void *p){auto c=static_cast<AsyncContext*>(p);if(--c->refs==0)delete c;}
void batch_observation(void *p,const MgcBatchEvent *batch){
    auto s=static_cast<AsyncContext*>(p)->session;if(!s->event_enabled.load()||!batch)return;MgmEvent event{};
    {std::lock_guard<std::mutex> lock(s->completion_mutex);auto job=s->jobs.find(batch->request_id);if(job==s->jobs.end())return;if(batch->kind==MGC_EVENT_EXECUTION_RETIRED){job->second.retired_ns=batch->monotonic_ns;return;}event=job->second.input_identity;}
    if(batch->kind==MGC_EVENT_ACCEPTED){event.kind=MGM_EVENT_ACCEPTED;event.result_status=MGC_ACCEPTED;}
    else if(batch->kind==MGC_EVENT_GPU_SUBMITTED)event.kind=MGM_EVENT_GPU_SUBMITTED;
    else if(batch->kind==MGC_EVENT_GPU_DEPENDENCY_READY)event.kind=MGM_EVENT_GPU_DEPENDENCY_READY;
    else return;
    event.timestamp_ns=int64_t(batch->monotonic_ns);event.gpu_batch_id=batch->batch_id;event.task_id=batch->request_id;s->observe(event);
}
// Admission accepts only positive samples up to 60 seconds. A completed job
// outside that range still releases its slot, without recording a fake sample.
uint64_t admission_elapsed(int64_t start,int64_t end){
    if(start<0||end<start)return 0;
    const uint64_t elapsed=uint64_t(end)-uint64_t(start);
    return elapsed<=60000000000ULL?elapsed:0;
}
// Cancellation and expiry discard outputs, but must not hide device loss. Old
// epochs still discard the completion instead of publishing a stale frame.
void async_completed(void *p,uint64_t id,MgcStatus status,MgmFrame *output){auto s=static_cast<AsyncContext*>(p)->session;bool publish=false,matched=false;Session::Job job{};const int64_t end=now();const int32_t worker_tid=static_cast<int32_t>(syscall(SYS_gettid));const bool inferred=inference_sample(output);if(status==MGC_SUCCESS&&!output)status=MGC_INTERNAL_ERROR;
    if(status==MGC_DEVICE_LOST)s->device_lost=true;
    {std::lock_guard<std::mutex> lock(s->completion_mutex);auto i=s->jobs.find(id);if(i!=s->jobs.end()){matched=true;job=i->second;s->jobs.erase(i);if(s->event_enabled.load()){auto retired=observation(*s,MGM_EVENT_EXECUTION_RETIRED,id,job.epoch,status,output);if(job.retired_ns)retired.timestamp_ns=int64_t(job.retired_ns);s->observe(retired);if(status==MGC_SUCCESS){s->observe(observation(*s,MGM_EVENT_ALGORITHM_ACCEPTED,id,job.epoch,status,output));MgmFrameDesc info{};info.header=MGC_HEADER_INIT(MgmFrameDesc);if(!s->identity&&!mgm_frame_describe(output,&info)&&!info.batch_event_coverage_observed)s->observe(observation(*s,MGM_EVENT_OBSERVATION_INCOMPLETE,id,job.epoch,MGC_UNSUPPORTED_CAPABILITY,output));}}if(job.epoch==s->epoch.load()){if(status!=MGC_DEVICE_LOST){if(job.cancelled)status=MGC_CANCELLED;else if(end>job.deadline)status=MGC_DEADLINE_REJECTED;}if(status==MGC_DEADLINE_REJECTED&&s->event_enabled.load())s->observe(observation(*s,MGM_EVENT_OUTPUT_EXPIRED,id,job.epoch,status,output));auto &r=s->ready[(s->ready_head+s->ready_count)%s->ready.size()];r={id,job.epoch,status,status==MGC_SUCCESS?output:nullptr,job.start,(job.start>=0&&end>=job.start?end-job.start:0),worker_tid};++s->ready_count;publish=status==MGC_SUCCESS;}}}
    // Output ownership has already moved to the queue when published. The GL
    // owner may consume/discard it immediately, so only local scalar metadata
    // may be used below; never dereference the published frame here.
    if(matched){const uint64_t elapsed=admission_elapsed(job.start,end);
        mgm_admission_retire(s->admission,elapsed,elapsed&&status==MGC_SUCCESS&&job.epoch==s->epoch.load()&&inferred);}
    if(!publish)mgm_frame_release(output);
}
}
#define JNI(name) Java_io_github_jichuo1_meguviz_media_MediaNative_##name
extern "C" JNIEXPORT jint JNICALL JNI(androidCreationProtocolVersionNative)(JNIEnv*,jclass){return 2;}
extern "C" JNIEXPORT jint JNICALL JNI(androidPrepareCreation)(JNIEnv*,jclass){
    return prepare_jni_guard();
}
extern "C" JNIEXPORT jint JNICALL JNI(androidDiscardPreparedCreation)(JNIEnv*,jclass){
    std::array<JniGuard,2> guards{};std::array<uint64_t,2> reservations{};bool pending=false;
    {std::lock_guard<std::mutex> lock(creation_guards_mutex);for(size_t i=0;i<ready_guards.size();++i){auto& ready=ready_guards[i];
        if(ready.preparing){ready.discard=true;pending=true;continue;}
        if(ready.guard||ready.reservation){ready.preparing=true;guards[i]=std::move(ready.guard);reservations[i]=ready.reservation;ready.reservation=0;}}}
    MgcStatus result=MGC_SUCCESS;
    for(size_t i=0;i<ready_guards.size();++i){if(!guards[i]&&!reservations[i])continue;
        const auto status=reservations[i]?mgm_android_creation_release_reservation(reservations[i]):MGC_SUCCESS;
        {std::lock_guard<std::mutex> lock(creation_guards_mutex);auto& ready=ready_guards[i];
            if(status){ready.guard=std::move(guards[i]);ready.reservation=reservations[i];}ready.discard=false;ready.preparing=false;}
        if(status){if(!result)result=status;}else guards[i].reset();
    }
    const auto jni=collect_jni_guards();const auto platform=mgm_android_creation_discard_ready();
    if(result)return result;if(jni)return jni;if(platform)return platform;return pending?MGC_BUSY:MGC_SUCCESS;
}
extern "C" JNIEXPORT jint JNICALL JNI(androidServiceCreationOwner)(JNIEnv*,jclass){return mgm_android_creation_service_owner();}
// Compatibility: old Java Frame.close() was void and cannot request the new
// daemon itself. Failure leaves every native reference queued/retained.
static bool request_java_retirement(JNIEnv* env){
    // Preserve exceptions already pending on entry. A notification failure of
    // our own must not replace the result of a completed native operation.
    if(!env||env->ExceptionCheck())return false;
    jclass cls=env->FindClass("io/github/jichuo1/meguviz/media/MediaRetirementService");
    if(!cls||env->ExceptionCheck()){
        if(env->ExceptionCheck())env->ExceptionClear();
        if(cls)env->DeleteLocalRef(cls);
        return false;
    }
    auto method=env->GetStaticMethodID(cls,"requestImmediate","()V");
    if(method&&!env->ExceptionCheck())env->CallStaticVoidMethod(cls,method);
    const bool ok=method&&!env->ExceptionCheck();
    if(env->ExceptionCheck())env->ExceptionClear();
    env->DeleteLocalRef(cls);return ok;
}
// Frame.close is void in older facades. On queue saturation keep the exact
// token in a fixed native retry list even after the Java token is forgotten.
static MgcStatus request_frame_release(Session& s,jlong id){
    auto found=s.frames.find(id);
    if(found==s.frames.end()){
        for(auto& value:s.requested_frame_releases)if(value==id)value=0;
        return MGC_SUCCESS;
    }
    if(s.platform&&mgm_android_defer_frame_release(s.platform,found->second)==MGC_SUCCESS){
        s.frames.erase(found);for(auto& value:s.requested_frame_releases)if(value==id)value=0;return MGC_ACCEPTED;
    }
    // Epoch changes or a repeated close can have removed frames while their
    // old retry IDs remain. Reclaim those slots before admitting another ID.
    for(auto& value:s.requested_frame_releases){
        if(value==id)return MGC_BUSY;
        if(value&&s.frames.find(value)==s.frames.end())value=0;
    }
    for(auto& value:s.requested_frame_releases)if(!value){value=id;s.retirement_backpressure=true;return MGC_BUSY;}
    return MGC_BACKPRESSURE;
}
static MgmFrame* reserved_frame_slot(){return reinterpret_cast<MgmFrame*>(uintptr_t(1));}
struct RawFrameReservation {
    Session& session;size_t index;
    explicit RawFrameReservation(Session& s):session(s),index(s.pending_frame_releases.size()){
        for(size_t i=0;i<s.pending_frame_releases.size();++i)if(!s.pending_frame_releases[i]){index=i;s.pending_frame_releases[i]=reserved_frame_slot();break;}
    }
    ~RawFrameReservation(){if(index<session.pending_frame_releases.size()&&session.pending_frame_releases[index]==reserved_frame_slot())session.pending_frame_releases[index]=nullptr;}
    explicit operator bool()const{return index<session.pending_frame_releases.size();}
    void retain(MgmFrame*& frame){if(!frame)return;
        if(session.platform&&mgm_android_defer_frame_release(session.platform,frame)==MGC_SUCCESS){frame=nullptr;return;}
        session.pending_frame_releases[index]=frame;frame=nullptr;session.retirement_backpressure=true;
    }
};
// A failed Java result publication has no remaining caller-owned token. If
// even the fixed token retry list is saturated, the raw slot reserved before
// capture/completion still retains the exact frame until owner-thread retry.
static void retain_unpublished_frame(Session& s,RawFrameReservation& reservation,jlong id){
    if(request_frame_release(s,id)!=MGC_BACKPRESSURE)return;
    auto found=s.frames.find(id);if(found==s.frames.end())return;
    MgmFrame* owned=found->second;s.frames.erase(found);reservation.retain(owned);
}
struct RawFrameBatch {
    Session& session;std::array<size_t,35> slots{};size_t count=0,used=0;bool valid=true;
    RawFrameBatch(Session& s,size_t needed):session(s){
        if(needed>slots.size()){valid=false;return;}
        for(size_t i=0;i<s.pending_frame_releases.size()&&count<needed;++i)if(!s.pending_frame_releases[i]){
            s.pending_frame_releases[i]=reserved_frame_slot();slots[count++]=i;}
        valid=count==needed;
    }
    ~RawFrameBatch(){for(size_t n=0;n<count;++n)if(session.pending_frame_releases[slots[n]]==reserved_frame_slot())session.pending_frame_releases[slots[n]]=nullptr;}
    explicit operator bool()const{return valid;}
    void retain(MgmFrame*& frame){if(!frame)return;const auto index=slots[used++];
        if(session.platform&&mgm_android_defer_frame_release(session.platform,frame)==MGC_SUCCESS){frame=nullptr;return;}
        session.pending_frame_releases[index]=frame;frame=nullptr;session.retirement_backpressure=true;}
};
static void retry_frame_releases(Session& s){
    bool waiting=false;
    for(auto& frame:s.pending_frame_releases)if(frame&&frame!=reserved_frame_slot()){
        if(s.platform&&mgm_android_defer_frame_release(s.platform,frame)==MGC_SUCCESS)frame=nullptr;else waiting=true;
    }
    for(auto& id:s.requested_frame_releases)if(id){auto found=s.frames.find(id);
        if(found==s.frames.end()){id=0;continue;}
        if(s.platform&&mgm_android_defer_frame_release(s.platform,found->second)==MGC_SUCCESS){s.frames.erase(found);id=0;}
        else waiting=true;
    }
    s.retirement_backpressure=waiting;
}
// Called only by a background service with a strong Session reference. No GL
// or registry mutex is held while frame/lease/domain destructors can block.
MgcStatus mgm_jni_service_android_retirement(){
    std::vector<std::shared_ptr<Session>> snapshot;
    try{{std::lock_guard<std::mutex> lock(mutex);for(const auto& item:sessions)snapshot.push_back(item.second);}}
    catch(...){return MGC_OUT_OF_MEMORY;}
    bool waiting=false;
    for(auto& s:snapshot){
        if(s->creating.load(std::memory_order_acquire)){waiting=true;continue;}
        MgmAndroidSession* platform=nullptr;
        {std::unique_lock<std::mutex> lock(s->platform_mutex,std::try_to_lock);if(!lock){waiting=true;continue;}
            platform=s->platform;if(platform){if(s->platform_services.load()){waiting=true;continue;}++s->platform_services;}}
        if(platform){const auto status=mgm_android_service_retirement(platform);--s->platform_services;if(status!=MGC_SUCCESS)waiting=true;}
        if(!s->platform_closed.load()||s->finalized.load())continue;
        if(s->finalization_uncertain.load()){waiting=true;continue;}
        bool expected=false;if(!s->finalizing.compare_exchange_strong(expected,true)){waiting=true;continue;}
        MgcStatus result=MGC_SUCCESS;
        try{
            if(s->effect){result=s->identity?mgm_identity_effect_release(s->effect):mgm_rife_effect_release(s->effect);if(!result)s->effect=nullptr;}
            if(!result){MgcDomain* old=nullptr;
                {std::unique_lock<std::mutex> lock(s->domain_mutex);if(s->domain){
                    MgcDomainStateInfo state{};state.header=MGC_HEADER_INIT(MgcDomainStateInfo);
                    result=s->device_lost.load()?MGC_DEVICE_LOST:mgc_domain_state(s->domain,&state);
                    if(result==MGC_SUCCESS&&state.lost)result=MGC_DEVICE_LOST;
                    if(result==MGC_SUCCESS||result==MGC_DEVICE_LOST){s->final_domain_status=result;old=s->domain;s->domain=nullptr;result=MGC_SUCCESS;}}}
                if(old)mgc_domain_release(old);
            }
            if(!result&&s->runtime){result=mgc_runtime_close(s->runtime);if(!result){mgc_runtime_release(s->runtime);s->runtime=nullptr;}}
            // If platform creation consumed this token the C operation is a
            // safe no-op; otherwise it returns the unused reservation on this
            // background worker. A BUSY claim remains reachable for retry.
            if(!result&&s->platform_reservation){result=mgm_android_creation_release_reservation(s->platform_reservation);if(!result)s->platform_reservation=0;}
        }catch(...){s->finalization_uncertain=true;result=MGC_INTERNAL_ERROR;}
        if(!result)s->code_guard.reset(); // always background, pool retains its reference
        s->finalization_status=result;
        if(!result)s->finalized=true;else waiting=true;
        s->finalizing=false;
        if(!result&&s->unreported.load(std::memory_order_acquire)){
            std::lock_guard<std::mutex> lock(mutex);auto found=sessions.find(jlong(s->session_id));
            if(found!=sessions.end()&&found->second==s)sessions.erase(found);
        }
    }
    const auto guards=collect_jni_guards();if(guards!=MGC_SUCCESS)waiting=true;
    const auto platform=mgm_android_creation_service_background();if(platform!=MGC_SUCCESS)waiting=true;
    return waiting?MGC_BUSY:MGC_SUCCESS;
}
static jint create_android_prepared(JNIEnv *env,jlong engine,jint width,jint height,jint source_width,jint source_height,jint capacity,jint path,jboolean admission,jlongArray receipt){
    if(!receipt||env->GetArrayLength(receipt)<2||env->ExceptionCheck())return MGC_INVALID_ARGUMENT;
    if(width<=0||height<=0||width>8192||height>8192||source_width<=0||source_height<=0||source_width>8192||source_height>8192||capacity<3||capacity>12||(path!=MGM_ANDROID_AHB_HOST_WAIT&&path!=MGM_ANDROID_AHB_GPU_SYNC))return MGC_INVALID_ARGUMENT;
    std::shared_ptr<Session> s;jlong id=0;MgcStatus status=MGC_OUT_OF_MEMORY;bool receipt_written=false;
    try{
        s=std::make_shared<Session>();if(!s->thread_token)return MGC_BACKPRESSURE;s->creating.store(true,std::memory_order_relaxed);
        s->sync_mode=path==MGM_ANDROID_AHB_GPU_SYNC?1:0;s->api.header=MGC_HEADER_INIT(MgmEffectApi);
        // Registry capacity and the caller's receipt are secured before any
        // runtime, effect, AHB or GL operation. No array allocation follows.
        {std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);if(!lock)return MGC_BUSY;if(sessions.size()>=2||sequence==INT64_MAX)return MGC_BACKPRESSURE;
            id=++sequence;s->session_id=id;sessions.emplace(id,s);}
        uint64_t reservation=0;status=take_jni_guard(s->code_guard,reservation);s->platform_reservation.store(reservation,std::memory_order_release);
        if(status){s->closing=true;s->platform_closed=true;s->unreported=true;s->creating.store(false,std::memory_order_release);return status;}
        const jlong value[2]={MGC_BUSY,id};env->SetLongArrayRegion(receipt,0,2,value);
        if(env->ExceptionCheck()){
            // No GL work occurred, but the unpublished owner still owns both
            // pins. Background finalization reclaims even an unseen receipt.
            status=MGC_OUT_OF_MEMORY;
        }else{
        receipt_written=true;
        if(engine){status=mgm_jni_create_ahb_effect(engine,width,height,&s->domain,&s->effect,&s->api,&s->execution_info);}
        else{s->identity=true;MgcRuntimeDesc rd{};rd.header=MGC_HEADER_INIT(MgcRuntimeDesc);rd.provider=MGC_PROVIDER_NCNN_VULKAN;rd.memory_budget_bytes=identity_budget(width,height,capacity);
            status=mgc_runtime_create(&rd,&s->runtime);if(!status){MgcDomainDesc dd{};dd.header=MGC_HEADER_INIT(MgcDomainDesc);status=mgc_domain_acquire(s->runtime,&dd,&s->domain);}
            if(!status)status=mgm_identity_effect_create(s->domain,&s->effect,&s->api);}
        if(!status){MgmAndroidSessionDesc d{};d.header=MGC_HEADER_INIT(MgmAndroidSessionDesc);d.domain=s->domain;d.epoch=1;d.width=width;d.height=height;d.source_width=source_width;d.source_height=source_height;d.transform_flags=MGM_ANDROID_CANONICAL_TOP_DOWN;d.pool_capacity=capacity;d.path=path;d.session_id=id;
            mgc_domain_identity(s->domain,&s->domain_id,&s->domain_generation);MgmAndroidSession* platform=nullptr;
            status=mgm_android_session_create_reserved(&d,s->platform_reservation.load(),&platform);s->platform.store(platform,std::memory_order_release);}
        if(!status){MgmAdmissionConfig c{};c.header=MGC_HEADER_INIT(MgmAdmissionConfig);c.cold_cost_ns=33333333;c.safety_margin_ns=1000000;c.sample_window=64;c.percentile=95;c.max_in_flight=1;c.enabled=admission;status=mgm_admission_create(&c,&s->admission);}
        }
    }catch(...){status=MGC_OUT_OF_MEMORY;}
    if(s&&id){
        if(status){s->closing.store(true,std::memory_order_release);if(!s->platform)s->platform_closed.store(true,std::memory_order_release);
            if(!receipt_written)s->unreported.store(true,std::memory_order_release);}
        s->creating.store(false,std::memory_order_release);
    }
    return status;
}
extern "C" JNIEXPORT jint JNICALL JNI(androidCreatePrepared)(JNIEnv *env,jclass,jlong engine,jint width,jint height,jint source_width,jint source_height,jint capacity,jint path,jboolean admission,jlongArray receipt){return create_android_prepared(env,engine,width,height,source_width,source_height,capacity,path,admission,receipt);}
static std::shared_ptr<Session> async_session(jlong id){std::lock_guard<std::mutex> lock(mutex);auto i=sessions.find(id);return i!=sessions.end()&&i->second->async?i->second:nullptr;}
static std::shared_ptr<Session> async_session_try(jlong id,bool& busy){
    std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);if(!lock){busy=true;return nullptr;}
    auto i=sessions.find(id);return i!=sessions.end()&&i->second->async?i->second:nullptr;
}
static bool cancel_unbound_async(const std::shared_ptr<Session>& s){
    int stage=s->async_stage.load(std::memory_order_acquire);
    while(stage==ASYNC_RESERVED||stage==ASYNC_PLAN_READY){
        if(s->async_stage.compare_exchange_weak(stage,ASYNC_CANCELLED,std::memory_order_acq_rel)){
            s->closing.store(true,std::memory_order_release);s->platform_closed.store(true,std::memory_order_release);
            s->unreported.store(true,std::memory_order_release);return true;}
    }
    return false;
}
// Reserve the Java-visible identity and both code guards before a worker can
// enter model planning. This call itself does not invoke a GPU operation.
extern "C" JNIEXPORT jint JNICALL JNI(androidAsyncReserve)(JNIEnv* env,jclass,jlongArray receipt){
    if(!receipt||env->GetArrayLength(receipt)<2||env->ExceptionCheck())return MGC_INVALID_ARGUMENT;
    std::shared_ptr<Session> s;jlong id=0;MgcStatus status=MGC_OUT_OF_MEMORY;bool written=false;
    try{
        s=std::make_shared<Session>();s->async=true;s->thread={};s->thread_token=0;s->creating=true;
        {std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);if(!lock)return MGC_BUSY;if(sessions.size()>=2||sequence==INT64_MAX)return MGC_BACKPRESSURE;
            id=++sequence;s->session_id=id;sessions.emplace(id,s);}
        uint64_t reservation=0;status=take_jni_guard(s->code_guard,reservation);s->platform_reservation.store(reservation,std::memory_order_release);
        if(status){s->closing=true;s->platform_closed=true;s->unreported=true;s->creating.store(false,std::memory_order_release);return status;}
        const jlong value[2]={MGC_BUSY,id};env->SetLongArrayRegion(receipt,0,2,value);
        if(env->ExceptionCheck())status=MGC_OUT_OF_MEMORY;else{written=true;status=MGC_SUCCESS;}
    }catch(...){status=MGC_OUT_OF_MEMORY;}
    if(s&&id){if(status){s->closing=true;s->platform_closed=true;if(!written)s->unreported=true;}s->creating.store(false,std::memory_order_release);}
    return status;
}
// Potentially unbounded planning remains on the caller's dedicated worker.
extern "C" JNIEXPORT jint JNICALL JNI(androidAsyncPreparePlan)(JNIEnv* env,jclass,jlong id,jlong engine,jint width,jint height,jint source_width,jint source_height,jint capacity,jint path,jboolean admission){
    auto s=async_session(id);if(!s)return MGC_INVALID_ARGUMENT;
    int expected=ASYNC_RESERVED;if(!s->async_stage.compare_exchange_strong(expected,ASYNC_PLANNING,std::memory_order_acq_rel))return MGC_BUSY;
    s->creating.store(true,std::memory_order_release);MgcStatus status=MGC_SUCCESS;
    try{
        const bool scheduled=request_java_retirement(env);
        if(!scheduled)status=MGC_BUSY;
        else if(width<=0||height<=0||width>8192||height>8192||source_width<=0||source_height<=0||source_width>8192||source_height>8192||capacity<3||capacity>12||(path!=MGM_ANDROID_AHB_HOST_WAIT&&path!=MGM_ANDROID_AHB_GPU_SYNC))status=MGC_INVALID_ARGUMENT;
        else if(s->async_cancel.load(std::memory_order_acquire))status=MGC_CANCELLED;
        else{s->sync_mode=path==MGM_ANDROID_AHB_GPU_SYNC?1:0;s->api.header=MGC_HEADER_INIT(MgmEffectApi);
            s->async_width=width;s->async_height=height;s->async_source_width=source_width;s->async_source_height=source_height;s->async_capacity=capacity;s->async_path=path;s->async_admission=admission;
            if(engine)status=mgm_jni_create_ahb_effect(engine,width,height,&s->domain,&s->effect,&s->api,&s->execution_info);
            else{s->identity=true;MgcRuntimeDesc rd{};rd.header=MGC_HEADER_INIT(MgcRuntimeDesc);rd.provider=MGC_PROVIDER_NCNN_VULKAN;rd.memory_budget_bytes=identity_budget(width,height,capacity);
                status=mgc_runtime_create(&rd,&s->runtime);if(!status){MgcDomainDesc dd{};dd.header=MGC_HEADER_INIT(MgcDomainDesc);status=mgc_domain_acquire(s->runtime,&dd,&s->domain);}
                if(!status)status=mgm_identity_effect_create(s->domain,&s->effect,&s->api);}}
    }catch(...){status=MGC_OUT_OF_MEMORY;}
    if(status){s->async_error=status;s->async_stage.store(status==MGC_CANCELLED?ASYNC_CANCELLED:ASYNC_FAILED,std::memory_order_release);
        s->closing=true;s->platform_closed=true;s->unreported=true;}
    else s->async_stage.store(ASYNC_PLAN_READY,std::memory_order_release);
    s->creating.store(false,std::memory_order_release);
    if(s->async_cancel.load(std::memory_order_acquire)&&cancel_unbound_async(s))status=MGC_CANCELLED;
    if(status)request_java_retirement(env);
    return status;
}
// Bind only after the independent planning worker has actually returned.
extern "C" JNIEXPORT jint JNICALL JNI(androidAsyncBeginGl)(JNIEnv*,jclass,jlong id){
    bool busy=false;auto s=async_session_try(id,busy);if(!s)return busy?MGC_BUSY:MGC_INVALID_ARGUMENT;const auto token=mgm_thread_token();if(!token)return MGC_BACKPRESSURE;
    int expected=ASYNC_PLAN_READY;if(!s->async_stage.compare_exchange_strong(expected,ASYNC_GL_BEGIN,std::memory_order_acq_rel))return MGC_BUSY;
    s->creating=true;s->thread=std::this_thread::get_id();s->thread_token=token;MgcStatus status=MGC_SUCCESS;
    try{
        if(s->async_cancel.load(std::memory_order_acquire))status=MGC_CANCELLED;
        else{MgmAndroidSessionDesc d{};d.header=MGC_HEADER_INIT(MgmAndroidSessionDesc);d.domain=s->domain;d.epoch=1;
            d.width=s->async_width;d.height=s->async_height;d.source_width=s->async_source_width;d.source_height=s->async_source_height;
            d.transform_flags=MGM_ANDROID_CANONICAL_TOP_DOWN;d.pool_capacity=s->async_capacity;d.path=s->async_path;d.session_id=id;
            mgc_domain_identity(s->domain,&s->domain_id,&s->domain_generation);
            MgmAndroidSession* platform=nullptr;status=mgm_android_session_begin_reserved(&d,s->platform_reservation.load(),&platform);
            s->platform.store(platform,std::memory_order_release);
            if(status==MGC_ACCEPTED&&platform){MgmAdmissionConfig c{};c.header=MGC_HEADER_INIT(MgmAdmissionConfig);
                c.cold_cost_ns=33333333;c.safety_margin_ns=1000000;c.sample_window=64;c.percentile=95;c.max_in_flight=1;c.enabled=s->async_admission;
                status=mgm_admission_create(&c,&s->admission);}
            else if(status==MGC_ACCEPTED)status=MGC_INTERNAL_ERROR;}
    }catch(...){status=MGC_OUT_OF_MEMORY;}
    if(s->async_cancel.load(std::memory_order_acquire)){if(s->platform)mgm_android_creation_cancel_import(s->platform_reservation);status=MGC_CANCELLED;}
    if(status){s->async_error=status;s->async_stage.store(ASYNC_FAILED,std::memory_order_release);s->closing=true;if(!s->platform)s->platform_closed=true;}
    else s->async_stage.store(ASYNC_GL_READY,std::memory_order_release);
    s->creating.store(false,std::memory_order_release);return status;
}
extern "C" JNIEXPORT jint JNICALL JNI(androidAsyncImport)(JNIEnv* env,jclass,jlong id){
    auto s=async_session(id);if(!s)return MGC_INVALID_ARGUMENT;int expected=ASYNC_GL_READY;
    if(!s->async_stage.compare_exchange_strong(expected,ASYNC_IMPORTING,std::memory_order_acq_rel))return MGC_BUSY;
    s->creating=true;
    if(!request_java_retirement(env)){s->async_error=MGC_BUSY;s->async_stage.store(ASYNC_FAILED,std::memory_order_release);s->creating=false;return MGC_BUSY;}
    MgcStatus status=MGC_BUSY;long delay_ns=8000000L;
    do{
        if(s->async_cancel.load(std::memory_order_acquire))mgm_android_creation_cancel_import(s->platform_reservation);
        status=mgm_android_creation_import_background(s->platform_reservation);
        if(status==MGC_BUSY||status==MGC_TIMEOUT){
            // The backend has returned and released its pool claim. Device
            // loss may require a separate worker to drive domain retirement.
            request_java_retirement(env);
            timespec delay{delay_ns/1000000000L,delay_ns%1000000000L};nanosleep(&delay,nullptr);delay_ns=std::min(1000000000L,delay_ns*2);
        }
    }while(status==MGC_BUSY||status==MGC_TIMEOUT);
    if(s->async_cancel.load(std::memory_order_acquire)&&(status==MGC_SUCCESS||status==MGC_CANCELLED))status=MGC_CANCELLED;
    if(status){s->async_error=status;s->async_stage.store(ASYNC_FAILED,std::memory_order_release);}
    else s->async_stage.store(ASYNC_IMPORTED,std::memory_order_release);
    s->creating.store(false,std::memory_order_release);return status;
}
extern "C" JNIEXPORT jint JNICALL JNI(androidAsyncCommitGl)(JNIEnv*,jclass,jlong id){
    bool busy=false;auto s=async_session_try(id,busy);if(!s)return busy?MGC_BUSY:MGC_INVALID_ARGUMENT;
    int expected=ASYNC_IMPORTED;if(!s->async_stage.compare_exchange_strong(expected,ASYNC_COMMITTING,std::memory_order_acq_rel))return MGC_BUSY;
    if(!s->thread_token||s->thread_token!=mgm_thread_token()||s->thread!=std::this_thread::get_id()){
        s->async_stage.store(ASYNC_IMPORTED,std::memory_order_release);return MGC_INCOMPATIBLE_DOMAIN;}
    s->creating=true;MgcStatus status=s->async_cancel.load(std::memory_order_acquire)?MGC_CANCELLED:mgm_android_session_commit_initialization(s->platform);
    if(!status)s->async_stage.store(ASYNC_LIVE,std::memory_order_release);
    else if(status==MGC_BUSY||status==MGC_TIMEOUT)s->async_stage.store(ASYNC_IMPORTED,std::memory_order_release);
    else{s->async_error=status;s->async_stage.store(ASYNC_FAILED,std::memory_order_release);}
    s->creating.store(false,std::memory_order_release);return status;
}
// Cancellation is advisory until every in-flight JNI call and backend owner
// physically retires. The GL owner still closes any bound platform session.
extern "C" JNIEXPORT jint JNICALL JNI(androidAsyncCancel)(JNIEnv* env,jclass,jlong id){
    bool busy=false;auto s=async_session_try(id,busy);if(!s)return busy?MGC_BUSY:MGC_SUCCESS;
    const auto initial=s->async_stage.load(std::memory_order_acquire);
    if(initial==ASYNC_LIVE||initial==ASYNC_CLOSING)return MGC_BUSY;
    s->async_cancel.store(true,std::memory_order_release);
    if(cancel_unbound_async(s)){request_java_retirement(env);return MGC_SUCCESS;}
    const auto stage=s->async_stage.load(std::memory_order_acquire);
    if(stage==ASYNC_GL_READY||stage==ASYNC_IMPORTING||stage==ASYNC_IMPORTED||stage==ASYNC_COMMITTING)
        return mgm_android_creation_cancel_import(s->platform_reservation);
    return MGC_BUSY;
}
extern "C" JNIEXPORT jint JNICALL JNI(androidAsyncRetirementStatus)(JNIEnv*,jclass,jlong id){bool busy=false;auto s=async_session_try(id,busy);return (busy||s)?MGC_BUSY:MGC_SUCCESS;}
// Older facades do not preserve a failure receipt and cannot safely service a
// partially-created owner. Refuse before performing any device side effect.
extern "C" JNIEXPORT jlongArray JNICALL JNI(androidCreate)(JNIEnv *env,jclass,jlong,jint,jint,jint,jint,jboolean){return values(env,{MGC_UNSUPPORTED_ABI,0});}
extern "C" JNIEXPORT jlongArray JNICALL JNI(androidCreateWithSource)(JNIEnv *env,jclass,jlong,jint,jint,jint,jint,jint,jint,jboolean){return values(env,{MGC_UNSUPPORTED_ABI,0});}
extern "C" JNIEXPORT jstring JNICALL JNI(androidExecutionInfo)(JNIEnv *env,jclass,jlong id){auto s=acquire(id);return env->NewStringUTF(s?s->execution_info.c_str():"");}
extern "C" JNIEXPORT jlongArray JNICALL JNI(androidFrameJobObservation)(JNIEnv *env,jclass,jlong id,jlong frame_id){
    auto s=acquire(id);if(!s)return values(env,{MGC_INVALID_ARGUMENT});
    auto f=frame(*s,frame_id);if(!f)return values(env,{MGC_INVALID_ARGUMENT});
    MgmFrameDesc d{};d.header=MGC_HEADER_INIT(MgmFrameDesc);auto status=mgm_frame_describe(f,&d);
    if(status)return values(env,{status});
    MgrJobObservation o{};o.header=MGC_HEADER_INIT(MgrJobObservation);status=mgm_rife_frame_observation(f,&o);
    if(status)return values(env,{status});
    if(d.task_id!=o.request_id)return values(env,{MGC_INTERNAL_ERROR});
    return values(env,{MGC_SUCCESS,jlong(o.request_id),o.frame_kind,o.effective_flow_scale,o.cache_lookup_scale,
        o.static_guard_dispatch,o.motion_guard_dispatch,o.tensor_cache_left,o.tensor_cache_right,
        o.bridge_cache_left,o.bridge_cache_right,o.tensor_cache_publish_stage_completed,o.tensor_cache_published_count,
        jlong(d.source_id),jlong(d.source_id_after),jlong(d.epoch)});
}
extern "C" JNIEXPORT jlongArray JNICALL JNI(androidProbe)(JNIEnv *env,jclass,jlong id){auto s=acquire(id);if(!s)return values(env,{MGC_INVALID_ARGUMENT});MgmAndroidProbe p{};p.header=MGC_HEADER_INIT(MgmAndroidProbe);auto status=mgm_android_probe(s->platform,&p);return values(env,{status,p.actual_width,p.actual_height,p.actual_stride,p.actual_format,jlong(p.actual_usage),p.egl_renderable,p.egl_sampleable,p.vulkan_sampleable,p.vulkan_storage,p.native_fence_import,p.native_fence_export,p.workload_qualified});}
extern "C" JNIEXPORT jlongArray JNICALL JNI(androidCapture)(JNIEnv *env,jclass,jlong id,jint texture,jfloatArray matrix,jlong source,jlong epoch,jlong pts,jint primaries,jint color_matrix){
    auto s=acquire(id);if(!s||!matrix||env->GetArrayLength(matrix)!=16)return values(env,{MGC_INVALID_ARGUMENT,0});
    if(s->retirement_backpressure.load()||s->frames.size()>=32)return values(env,{MGC_BACKPRESSURE,0});
    RawFrameReservation reservation(*s);if(!reservation)return values(env,{MGC_BACKPRESSURE,0});
    MgmAndroidCapture c{};c.header=MGC_HEADER_INIT(MgmAndroidCapture);c.source_id=source;c.epoch=epoch;c.pts_us=pts;c.oes_texture=texture;
    if(s->event_enabled.load()){auto event=observation(*s,MGM_EVENT_SOURCE_AVAILABLE,0,uint64_t(epoch),MGC_SUCCESS);event.source_id=uint64_t(source);s->observe(event);}
    env->GetFloatArrayRegion(matrix,0,16,c.texture_transform);if(env->ExceptionCheck())return nullptr;
    c.color.header=MGC_HEADER_INIT(MgmColor);c.color.primaries=primaries;c.color.matrix=color_matrix;c.color.transfer=MGM_TRANSFER_SDR;c.color.range=MGM_RANGE_FULL;c.color.bit_depth=8;
    MgmFrame *f=nullptr;jlong handle=0;bool registered=false;
    try{
        auto status=mgm_android_capture_oes(s->platform,&c,&f);if(status){reservation.retain(f);if(s->retirement_backpressure.load())request_java_retirement(env);return values(env,{status,0});}
        handle=++s->sequence;s->frames.emplace(handle,f);registered=true;
        auto result=values(env,{MGC_SUCCESS,handle});if(!result){retain_unpublished_frame(*s,reservation,handle);request_java_retirement(env);}return result;
    }catch(...){if(registered)retain_unpublished_frame(*s,reservation,handle);else reservation.retain(f);request_java_retirement(env);return values(env,{MGC_OUT_OF_MEMORY,0});}
}
extern "C" JNIEXPORT jlongArray JNICALL JNI(androidCompare)(JNIEnv *env,jclass,jlong id,jlong a,jlong b){auto s=acquire(id);if(!s)return values(env,{MGC_INVALID_ARGUMENT});if(s->retirement_backpressure.load())return values(env,{MGC_BACKPRESSURE});MgmAndroidGuardResult r{};r.header=MGC_HEADER_INIT(MgmAndroidGuardResult);auto status=mgm_android_compare_inputs(s->platform,frame(*s,a),frame(*s,b),&r);return values(env,{status,r.is_static,r.is_cut,jlong(r.sampled_readback_bytes),r.is_duplicate});}
extern "C" JNIEXPORT jlongArray JNICALL JNI(androidInterpolate)(JNIEnv *env,jclass,jlong id,jlong a,jlong b,jdouble alpha,jlong deadline){
    auto s=acquire(id);if(!s)return values(env,{MGC_INVALID_ARGUMENT,0});if(s->retirement_backpressure.load())return values(env,{MGC_BACKPRESSURE,0});
    auto left=frame(*s,a),right=frame(*s,b);if(!left||!right||!mgm_detail::valid_alpha(alpha))return values(env,{MGC_INVALID_ARGUMENT,0});
    if(s->frames.size()>=32)return values(env,{MGC_BACKPRESSURE,0});RawFrameReservation reservation(*s);if(!reservation)return values(env,{MGC_BACKPRESSURE,0});
    const int64_t start=now();MgmAdmissionDecision decision{};decision.header=MGC_HEADER_INIT(MgmAdmissionDecision);auto status=mgm_admission_reserve(s->admission,start,deadline,&decision);if(status!=MGC_ACCEPTED)return values(env,{status<0?status:MGC_INTERNAL_ERROR,0});
    MgmAndroidGuardResult guard{};guard.header=MGC_HEADER_INIT(MgmAndroidGuardResult);status=mgm_android_compare_inputs(s->platform,left,right,&guard);MgmFrame *output=nullptr;
    if(!status&&(guard.is_static||guard.is_cut)){output=left;mgm_frame_retain(output);}
    else if(!status){MgmEffectRequest r{};r.header=MGC_HEADER_INIT(MgmEffectRequest);r.request_id=++s->sequence;r.output_sync_mode=s->sync_mode;r.epoch=s->epoch;r.left=left;r.right=right;r.alpha=alpha;r.deadline_cpu_ns=deadline;Result result;mgm_frame_retain(left);mgm_frame_retain(right);status=s->api.submit(s->effect,&r,completed,&result,nullptr,nullptr);if(status!=MGC_ACCEPTED){mgm_frame_release(left);mgm_frame_release(right);if(status>=0)status=MGC_INTERNAL_ERROR;}else{status=s->api.drain(s->effect,UINT64_MAX);if(!status)status=result.status;output=result.output;}}
    if(status==MGC_SUCCESS&&!output)status=MGC_INTERNAL_ERROR;int64_t end=now();const uint64_t elapsed=admission_elapsed(start,end);mgm_admission_retire(s->admission,elapsed,elapsed&&status==MGC_SUCCESS&&inference_sample(output));
    if(status||end>deadline){reservation.retain(output);request_java_retirement(env);return values(env,{status?status:MGC_DEADLINE_REJECTED,0});}
    jlong handle=0;bool registered=false;
    try{handle=++s->sequence;s->frames.emplace(handle,output);registered=true;auto result=values(env,{MGC_SUCCESS,handle});if(!result){retain_unpublished_frame(*s,reservation,handle);request_java_retirement(env);}return result;}
    catch(...){if(registered)retain_unpublished_frame(*s,reservation,handle);else reservation.retain(output);request_java_retirement(env);return values(env,{MGC_OUT_OF_MEMORY,0});}
}
extern "C" JNIEXPORT jlongArray JNICALL JNI(androidSubmit)(JNIEnv *env,jclass,jlong id,jlong a,jlong b,jdouble alpha,jlong deadline,jint flow){auto s=acquire(id);if(!s)return values(env,{MGC_INVALID_ARGUMENT,0});if(s->retirement_backpressure.load())return values(env,{MGC_BACKPRESSURE,0});auto left=frame(*s,a),right=frame(*s,b);if(!left||!right||!mgm_detail::valid_alpha(alpha)||deadline<0||flow<0)return values(env,{MGC_INVALID_ARGUMENT,0});{std::lock_guard<std::mutex> lock(s->completion_mutex);if(s->jobs.size()+s->ready_count>=s->ready.size())return values(env,{MGC_BACKPRESSURE,0});}
    const int64_t start=now();MgmAdmissionDecision decision{};decision.header=MGC_HEADER_INIT(MgmAdmissionDecision);auto status=mgm_admission_reserve(s->admission,start,deadline,&decision);if(status!=MGC_ACCEPTED)return values(env,{status<0?status:MGC_INTERNAL_ERROR,0});
    uint64_t job=++s->sequence;auto reply=values(env,{MGC_ACCEPTED,jlong(job)});if(!reply){mgm_admission_retire(s->admission,0,0);return nullptr;}
    AsyncContext *context=nullptr;try{context=new AsyncContext;context->session=s;{std::lock_guard<std::mutex> lock(s->completion_mutex);Session::Job pending{s->epoch.load(),start,deadline,false};pending.input_identity=observation(*s,MGM_EVENT_ACCEPTED,job,pending.epoch,MGC_SUCCESS,left);s->jobs.emplace(job,pending);}}catch(...){delete context;mgm_admission_retire(s->admission,0,0);return values(env,{MGC_OUT_OF_MEMORY,0});}
    MgmEffectRequest r{};r.header=MGC_HEADER_INIT(MgmEffectRequest);r.request_id=job;r.batch_event=s->event_enabled.load()?batch_observation:nullptr;r.algorithm_scale=flow;r.output_sync_mode=s->sync_mode;r.epoch=s->epoch.load();r.left=left;r.right=right;r.alpha=alpha;r.deadline_cpu_ns=deadline;mgm_frame_retain(left);mgm_frame_retain(right);status=s->api.submit(s->effect,&r,async_completed,context,retain_async,release_async);release_async(context);if(status!=MGC_ACCEPTED){mgm_frame_release(left);mgm_frame_release(right);{std::lock_guard<std::mutex> lock(s->completion_mutex);s->jobs.erase(job);}mgm_admission_retire(s->admission,0,0);return values(env,{status<0?status:MGC_INTERNAL_ERROR,0});}if(s->identity&&s->event_enabled.load())s->observe(observation(*s,MGM_EVENT_ACCEPTED,job,s->epoch.load(),MGC_ACCEPTED,left));return reply;
}
extern "C" JNIEXPORT jlongArray JNICALL JNI(androidPollCompleted)(JNIEnv *env,jclass,jlong id){
    auto s=acquire(id,true);if(!s)return values(env,{MGC_INVALID_ARGUMENT,0,0});
    if(s->frames.size()>=32)return values(env,{MGC_BACKPRESSURE,0,0});
    RawFrameReservation reservation(*s);if(!reservation)return values(env,{MGC_BUSY,0,0});
    Session::Ready r;MgcStatus discard=MGC_SUCCESS;
    {
        std::lock_guard<std::mutex> lock(s->completion_mutex);
        if(!s->ready_count)return values(env,{MGC_BUSY,0,0});
        auto& front=s->ready[s->ready_head];
        if(front.epoch!=s->epoch.load())discard=MGC_CANCELLED;
        else if(front.status==MGC_SUCCESS&&front.output){const auto device=session_device_loss(*s);if(device!=MGC_SUCCESS)discard=device;}
        if(discard!=MGC_SUCCESS&&front.output){
            // Queue transfer is lock-free and invokes no callbacks; a
            // pre-reserved slot preserves the reference even under saturation.
            reservation.retain(front.output);
        }
        r=front;front={};s->ready_head=(s->ready_head+1)%s->ready.size();--s->ready_count;
    }
    if(discard!=MGC_SUCCESS){request_java_retirement(env);return values(env,{discard,jlong(r.job),0});}
    if(r.status||!r.output)return values(env,{r.status,jlong(r.job),0});
    jlong token=0;bool registered=false;
    try{
        token=++s->sequence;s->frames.emplace(token,r.output);registered=true;
        MgmFrameDesc desc{};desc.header=MGC_HEADER_INIT(MgmFrameDesc);mgm_frame_describe(r.output,&desc);auto &d=desc.diagnostics;
        auto out=values(env,{r.status,jlong(r.job),token,d.values[0],d.values[2],d.values[1],d.values[3],d.stage_ns[0],d.stage_ns[1],d.stage_ns[2],jlong(desc.epoch),desc.frame_kind,desc.worker_cpu_ns,desc.worker_cpu_observed,desc.gpu_elapsed_observed,jlong(desc.gpu_elapsed_ns),jlong(desc.gpu_batch_count),jlong(desc.gpu_observed_batch_count),desc.gpu_timestamp_valid_bits,desc.gpu_timestamp_period_bits,desc.batch_event_coverage_observed,jlong(r.work_start_ns),jlong(r.work_duration_ns),jlong(r.worker_tid)});
        if(!out){retain_unpublished_frame(*s,reservation,token);request_java_retirement(env);}return out;
    }catch(...){
        // At most three ready outputs exist, and new work is backpressured as
        // soon as any fixed pending slot is used.
        if(registered)retain_unpublished_frame(*s,reservation,token);else reservation.retain(r.output);
        request_java_retirement(env);return values(env,{MGC_OUT_OF_MEMORY,jlong(r.job),0});
    }
}
extern "C" JNIEXPORT jint JNICALL JNI(androidCancel)(JNIEnv*,jclass,jlong id,jlong job){auto s=acquire(id,true);if(!s)return MGC_INVALID_ARGUMENT;{std::lock_guard<std::mutex> lock(s->completion_mutex);auto i=s->jobs.find(job);if(i==s->jobs.end())return MGC_SUCCESS;i->second.cancelled=true;}return s->effect?s->api.cancel(s->effect,job):MGC_INVALID_ARGUMENT;}
extern "C" JNIEXPORT jint JNICALL JNI(androidDraw)(JNIEnv*,jclass,jlong id,jlong f,jint width,jint height){auto s=acquire(id);if(!s)return MGC_INVALID_ARGUMENT;if(s->retirement_backpressure.load())return MGC_BACKPRESSURE;return mgm_android_draw(s->platform,frame(*s,f),width,height);}
extern "C" JNIEXPORT jint JNICALL JNI(androidPoll)(JNIEnv*,jclass,jlong id,jlong timeout){auto s=acquire(id,true);if(!s||timeout<0)return MGC_INVALID_ARGUMENT;if(!s->platform)return s->closing.load()?MGC_SUCCESS:MGC_INTERNAL_ERROR;retry_frame_releases(*s);const auto status=mgm_android_poll(s->platform,uint64_t(timeout));if(status==MGC_DEVICE_LOST)s->device_lost=true;return status==MGC_SUCCESS&&s->retirement_backpressure.load()?MGC_BUSY:status;}
extern "C" JNIEXPORT jint JNICALL JNI(androidDeviceLossStatus)(JNIEnv*,jclass,jlong id){auto s=acquire(id,true);return s?session_device_loss(*s):MGC_INVALID_ARGUMENT;}
extern "C" JNIEXPORT jlongArray JNICALL JNI(androidDeviceState)(JNIEnv* env,jclass,jlong id){
    auto s=acquire(id,true);if(!s)return values(env,{MGC_INVALID_ARGUMENT,0,0,0,0,0});
    std::unique_lock<std::mutex> lock(s->domain_mutex,std::try_to_lock);if(!lock)return values(env,{MGC_BUSY,0,0,0,0,0});
    if(!s->domain)return values(env,{MGC_INVALID_ARGUMENT,0,0,0,0,0});
    MgcDomainStateInfo state{};state.header=MGC_HEADER_INIT(MgcDomainStateInfo);
    const auto status=mgc_domain_state(s->domain,&state);if(status)return values(env,{status,0,0,0,0,0});
    if(state.lost)s->device_lost=true;
    return values(env,{MGC_SUCCESS,jlong(state.domain_id),jlong(state.domain_generation),jlong(state.device_epoch),s->device_lost.load()?1:0,state.device_retired?1:0});
}
extern "C" JNIEXPORT jint JNICALL JNI(androidBeginEpoch)(JNIEnv* env,jclass,jlong id,jlong epoch){auto s=acquire(id);if(!s||epoch<=0)return MGC_INVALID_ARGUMENT;if(s->retirement_backpressure.load())return MGC_BACKPRESSURE;RawFrameBatch reservation(*s,s->frames.size()+s->ready.size());if(!reservation)return MGC_BACKPRESSURE;auto status=mgm_android_begin_epoch(s->platform,epoch);if(!status){s->epoch=epoch;std::map<jlong,MgmFrame*> old_frames;old_frames.swap(s->frames);std::array<MgmFrame*,3> discard{};std::array<uint64_t,4> cancel{};size_t cancel_count=0;{std::lock_guard<std::mutex> lock(s->completion_mutex);for(auto &j:s->jobs){j.second.cancelled=true;cancel[cancel_count++]=j.first;}for(size_t i=0;i<s->ready.size();++i){discard[i]=s->ready[i].output;s->ready[i]={};}s->ready_count=s->ready_head=0;}for(size_t i=0;i<cancel_count;++i)s->api.cancel(s->effect,cancel[i]);for(auto& f:discard)reservation.retain(f);for(auto &item:old_frames)reservation.retain(item.second);request_java_retirement(env);}return status;}
extern "C" JNIEXPORT jint JNICALL JNI(androidReleaseFrameDeferred)(JNIEnv*,jclass,jlong id,jlong f){auto s=acquire(id,true);return s?request_frame_release(*s,f):MGC_INVALID_ARGUMENT;}
extern "C" JNIEXPORT void JNICALL JNI(androidReleaseFrame)(JNIEnv* env,jclass cls,jlong id,jlong f){const auto status=JNI(androidReleaseFrameDeferred)(env,cls,id,f);if(status==MGC_ACCEPTED||status==MGC_BUSY)request_java_retirement(env);}
extern "C" JNIEXPORT jint JNICALL JNI(androidClose)(JNIEnv* env,jclass,jlong id){
    bool busy=false;auto s=acquire_close(id,busy);if(!s)return busy?MGC_BUSY:MGC_INVALID_ARGUMENT;s->closing=true;
    const auto retry=[env](MgcStatus status){if(status!=MGC_SUCCESS)request_java_retirement(env);return status;};
    retry_frame_releases(*s);
    bool queued=false;{std::lock_guard<std::mutex> lock(s->completion_mutex);queued=!s->jobs.empty()||s->ready_count;}
    if(queued)return retry(MGC_BUSY);
    if(!s->platform_closed.load()&&s->effect){const auto status=s->api.drain(s->effect,0);if(status)return retry(status);}
    if(!s->frames.empty()||s->retirement_backpressure.load())return retry(MGC_BUSY);
    if(s->platform){std::unique_lock<std::mutex> lock(s->platform_mutex,std::try_to_lock);if(!lock)return retry(MGC_BUSY);
        if(s->platform_services.load()){lock.unlock();return retry(MGC_BUSY);}
        auto status=mgm_android_poll(s->platform,0);if(status){lock.unlock();return retry(status);}
        status=mgm_android_session_close(s->platform);if(status){lock.unlock();return retry(status);}s->platform=nullptr;s->platform_closed=true;}
    if(!s->finalized.load())return retry(MGC_BUSY);
    {std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);if(!lock)return retry(MGC_BUSY);sessions.erase(id);}
    // A successful platform close has retired its pool slot, but the loader
    // reference itself must be released later by the background worker.
    request_java_retirement(env);return MGC_SUCCESS;
}
// A receipt can be visible to Java even if publishing it raised an exception
// before side effects and the empty registry entry was removed. Distinguish
// that harmless absence from a receipt owned by another thread/context.
extern "C" JNIEXPORT jint JNICALL JNI(androidAbandonCreation)(JNIEnv* env,jclass cls,jlong id){
    if(id<=0)return MGC_SUCCESS;
    {std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);if(!lock)return MGC_BUSY;if(sessions.find(id)==sessions.end())return MGC_SUCCESS;}
    bool busy=false;auto s=acquire_close(id,busy);if(!s)return busy?MGC_BUSY:MGC_INCOMPATIBLE_DOMAIN;
    return JNI(androidClose)(env,cls,id);
}
extern "C" JNIEXPORT jint JNICALL JNI(androidSetEventsEnabled)(JNIEnv*,jclass,jlong id,jboolean enabled){auto s=acquire(id);if(!s)return MGC_INVALID_ARGUMENT;auto status=mgm_android_set_event_handler(s->platform,enabled?native_observation:nullptr,enabled?s.get():nullptr);if(!status)s->event_enabled=enabled==JNI_TRUE;return status;}
extern "C" JNIEXPORT jlongArray JNICALL JNI(androidPollEvents)(JNIEnv *env,jclass,jlong id){
    auto s=acquire(id,true);if(!s)return values(env,{0,0,0,0});std::array<jlong,4+512*16> data{};std::lock_guard<std::mutex> lock(s->event_mutex);data[0]=s->event_enabled.load();data[1]=jlong(s->event_dropped);data[2]=jlong(s->event_sequence);data[3]=s->event_count;
    for(uint32_t i=0;i<s->event_count;++i){auto &row=s->events[(s->event_head+i)%s->events.size()];const auto &e=row.value;size_t p=4+i*16;data[p++]=jlong(row.sequence);data[p++]=e.kind;data[p++]=e.time_domain;data[p++]=jlong(e.session_id);data[p++]=jlong(e.epoch);data[p++]=jlong(e.source_id);data[p++]=jlong(e.content_id);data[p++]=jlong(e.task_id);data[p++]=jlong(e.allocation_id);data[p++]=jlong(e.allocation_generation);data[p++]=jlong(e.domain_id);data[p++]=jlong(e.domain_generation);data[p++]=e.timestamp_ns;data[p++]=e.result_status;data[p++]=e.frame_kind;data[p++]=jlong(e.gpu_batch_id);}
    jsize count=jsize(4+s->event_count*16);auto result=env->NewLongArray(count);if(!result)return nullptr;env->SetLongArrayRegion(result,0,count,data.data());if(env->ExceptionCheck())return nullptr;s->event_head=(s->event_head+s->event_count)%s->events.size();s->event_count=0;return result;
}
