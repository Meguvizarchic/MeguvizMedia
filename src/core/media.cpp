#include <meguviz/media.h>
#include "../descriptor.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

struct MgmFrame { std::atomic<uint32_t> refs{1};MgmFrameDesc desc{}; };
struct MgmTimeline { std::mutex mutex;MgmClockMapping mapping{}; };
struct Job { MgmEffectRequest request{};MgmEffectCompletionFn callback=nullptr;void *context=nullptr;MgcContextFn release_context=nullptr;bool cancelled=false; };
struct MgmEffect {
    MgcDomain *domain=nullptr;std::mutex mutex;std::condition_variable changed;
    std::unique_ptr<Job> preparing,pending;std::thread::id preparing_thread;Job *active=nullptr;bool stopping=false;
    std::thread worker;
};
namespace {
using mgm_detail::copy_desc;
using mgm_detail::write_desc;
using mgm_detail::zero;
bool valid_frame(MgmFrame *f){return f&&f->desc.content;}
void worker(MgmEffect *effect) {
    for(;;){
        std::unique_ptr<Job> job;
        {std::unique_lock<std::mutex> lock(effect->mutex);effect->changed.wait(lock,[&]{return effect->stopping||effect->pending;});if(effect->stopping&&!effect->pending)break;job=std::move(effect->pending);effect->active=job.get();}
        MgmFrame *output=nullptr;MgcStatus result=MGC_CANCELLED;
        {std::lock_guard<std::mutex> lock(effect->mutex);if(!job->cancelled){auto source=job->request.alpha>=0.5&&job->request.right?job->request.right:job->request.left;MgmFrameDesc desc=source->desc;desc.frame_kind=MGM_FRAME_ORIGINAL;result=mgm_frame_create(&desc,&output);}}
        // The callback sees physically retired identity work. No GPU work was
        // submitted; cancellation after this commit point may be too late.
        job->callback(job->context,job->request.request_id,result,output);
        if(job->release_context)job->release_context(job->context);
        mgm_frame_release(job->request.left);mgm_frame_release(job->request.right);
        {std::lock_guard<std::mutex> lock(effect->mutex);effect->active=nullptr;effect->changed.notify_all();}
    }
}
MgcStatus query(MgmEffect *effect,MgcCapability *cap) {
    if(!effect)return MGC_INVALID_ARGUMENT;MgcCapability value{};uint32_t size=0;auto s=copy_desc(cap,offsetof(MgcCapability,reserved),value,&size);if(s)return s;if(!zero(value.reserved))return MGC_INVALID_ARGUMENT;
    value.feature=MGC_CAP_CPU_REFERENCE;value.advertised=1;value.enabled=1;value.probe_result=MGC_SUCCESS;value.workload_qualified=1;value.reason=MGC_SUCCESS;write_desc(cap,value,size);return MGC_SUCCESS;
}
MgcStatus prepare(MgmEffect *effect,const MgmFrameDesc *format) {
    if(!effect)return MGC_INVALID_ARGUMENT;MgmFrameDesc value{};auto status=copy_desc(format,offsetof(MgmFrameDesc,reserved),value);if(status)return status;return value.required_flags?MGC_UNSUPPORTED_ABI:(!zero(value.reserved)?MGC_INVALID_ARGUMENT:MGC_SUCCESS);
}
MgcStatus submit(MgmEffect *e,const MgmEffectRequest *r,MgmEffectCompletionFn cb,void *ctx,MgcContextFn retain,MgcContextFn release) {
    if(!e||!cb)return MGC_INVALID_ARGUMENT;MgmEffectRequest copied{};auto status=copy_desc(r,offsetof(MgmEffectRequest,reserved),copied);if(status)return status;r=&copied;if(r->required_flags)return MGC_UNSUPPORTED_ABI;if(!zero(r->reserved))return MGC_INVALID_ARGUMENT;
    if(!valid_frame(r->left)||!std::isfinite(r->alpha)||r->alpha<0||r->alpha>1||(r->right&&!valid_frame(r->right))||r->left->desc.epoch!=r->epoch||(r->right&&r->right->desc.epoch!=r->epoch)||!!retain!=!!release)return MGC_INVALID_ARGUMENT;
    uint64_t domain_id=0,generation=0;if(mgc_domain_identity(e->domain,&domain_id,&generation))return MGC_INVALID_ARGUMENT;
    MgcLeaseInfo info{};info.header=MGC_HEADER_INIT(MgcLeaseInfo);if(mgc_lease_info(r->left->desc.content,&info))return MGC_INVALID_ARGUMENT;
    if(info.domain_id!=domain_id||info.domain_generation!=generation)return MGC_INCOMPATIBLE_DOMAIN;
    if(r->right){if(mgc_lease_info(r->right->desc.content,&info))return MGC_INVALID_ARGUMENT;if(info.domain_id!=domain_id||info.domain_generation!=generation)return MGC_INCOMPATIBLE_DOMAIN;}
    MgcCapability cap{};cap.header=MGC_HEADER_INIT(MgcCapability);if(mgc_domain_capability(e->domain,MGC_CAP_CPU_REFERENCE,&cap)||!cap.enabled)return MGC_UNSUPPORTED_CAPABILITY;
    try {
        auto job=std::make_unique<Job>();job->request=*r;job->callback=cb;job->context=ctx;job->release_context=release;
        // Reserve lifetime and capacity before calling user code; do not call
        // context callbacks while holding the effect mutex.
        {std::lock_guard<std::mutex> lock(e->mutex);if(e->stopping)return MGC_BUSY;if(e->preparing||e->pending||e->active)return MGC_BACKPRESSURE;
         e->preparing=std::move(job);e->preparing_thread=std::this_thread::get_id();}
        try {if(retain)retain(ctx);} catch(...) {
            std::lock_guard<std::mutex> lock(e->mutex);e->preparing.reset();e->preparing_thread={};e->changed.notify_all();return MGC_INTERNAL_ERROR;
        }
        {std::lock_guard<std::mutex> lock(e->mutex);e->pending=std::move(e->preparing);e->preparing_thread={};e->changed.notify_all();}
        return MGC_ACCEPTED;
    }catch(const std::bad_alloc&){return MGC_OUT_OF_MEMORY;}catch(...){return MGC_INTERNAL_ERROR;}
}
MgcStatus cancel(MgmEffect *e,uint64_t request_id) {
    if(!e)return MGC_INVALID_ARGUMENT;std::lock_guard<std::mutex> lock(e->mutex);
    if(e->preparing&&e->preparing->request.request_id==request_id)e->preparing->cancelled=true;
    if(e->pending&&e->pending->request.request_id==request_id)e->pending->cancelled=true;
    if(e->active&&e->active->request.request_id==request_id)e->active->cancelled=true;
    return MGC_SUCCESS;
}
MgcStatus drain(MgmEffect *e,uint64_t timeout_ns) {
    if(!e)return MGC_INVALID_ARGUMENT;if(std::this_thread::get_id()==e->worker.get_id())return MGC_BUSY;
    std::unique_lock<std::mutex> lock(e->mutex);if(e->preparing&&e->preparing_thread==std::this_thread::get_id())return MGC_BUSY;auto done=[&]{return !e->preparing&&!e->pending&&!e->active;};if(done())return MGC_SUCCESS;
    if(!timeout_ns)return MGC_TIMEOUT;
    if(timeout_ns==UINT64_MAX){e->changed.wait(lock,done);return MGC_SUCCESS;}
    // Avoid overflow in libc++'s predicate wait_for when a finite deadline is
    // larger than the representable steady-clock time point.
    constexpr uint64_t slice_ns=3600ULL*1000000000ULL;uint64_t remaining=timeout_ns;
    for(;;){const auto start=std::chrono::steady_clock::now();
        const auto slice=std::chrono::nanoseconds(static_cast<int64_t>(remaining<slice_ns?remaining:slice_ns));
        if(e->changed.wait_for(lock,slice,done))return MGC_SUCCESS;
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        if(elapsed>0&&static_cast<uint64_t>(elapsed)>=remaining)return MGC_TIMEOUT;
        if(elapsed>0)remaining-=static_cast<uint64_t>(elapsed);
    }
}
}
extern "C" {
MgcStatus mgm_frame_create(const MgmFrameDesc *d,MgmFrame **out) {
    if(out)*out=nullptr;if(!out)return MGC_INVALID_ARGUMENT;MgmFrameDesc copied{};auto s=copy_desc(d,offsetof(MgmFrameDesc,reserved),copied);if(s)return s;d=&copied;if(d->required_flags)return MGC_UNSUPPORTED_ABI;if(!zero(d->reserved))return MGC_INVALID_ARGUMENT;
    if(!d->content||d->timebase.denominator<=0||d->timebase.numerator<=0||d->pts_ticks<0||d->rotation_degrees%90||d->rotation_degrees>=360)return MGC_INVALID_ARGUMENT;
    MgcLeaseInfo info{};info.header=MGC_HEADER_INIT(MgcLeaseInfo);s=mgc_lease_info(d->content,&info);if(s)return s;
    if(info.width&&(d->crop_left>info.width||d->crop_width>info.width-d->crop_left||d->crop_top>info.height||d->crop_height>info.height-d->crop_top))return MGC_INVALID_ARGUMENT;
    auto f=new(std::nothrow) MgmFrame;if(!f)return MGC_OUT_OF_MEMORY;f->desc=*d;mgc_lease_retain(d->content);*out=f;return MGC_SUCCESS;
}
void mgm_frame_retain(MgmFrame *f){if(f)++f->refs;}
void mgm_frame_release(MgmFrame *f){if(f&&--f->refs==0){mgc_lease_release(f->desc.content);delete f;}}
MgcStatus mgm_frame_describe(MgmFrame *f,MgmFrameDesc *d){if(!f)return MGC_INVALID_ARGUMENT;MgcHeader h{};auto s=mgm_detail::read_header(d,sizeof(MgcHeader),h);if(s)return s;write_desc(d,f->desc,h.struct_size);return MGC_SUCCESS;}
MgcStatus mgm_timeline_create(const MgmClockMapping *m,MgmTimeline **out){if(out)*out=nullptr;if(!out)return MGC_INVALID_ARGUMENT;MgmClockMapping copied{};auto s=copy_desc(m,offsetof(MgmClockMapping,reserved),copied);if(s)return s;m=&copied;if(!m->valid||m->timebase.numerator<=0||m->timebase.denominator<=0||!zero(m->reserved))return MGC_INVALID_ARGUMENT;auto t=new(std::nothrow) MgmTimeline;if(!t)return MGC_OUT_OF_MEMORY;t->mapping=*m;*out=t;return MGC_SUCCESS;}
void mgm_timeline_release(MgmTimeline *t){delete t;}
MgcStatus mgm_timeline_reset(MgmTimeline *t,const MgmClockMapping *m){if(!t)return MGC_INVALID_ARGUMENT;MgmClockMapping copied{};auto s=copy_desc(m,offsetof(MgmClockMapping,reserved),copied);if(s)return s;m=&copied;std::lock_guard<std::mutex> lock(t->mutex);if(!m->valid||m->epoch<=t->mapping.epoch||m->timebase.numerator<=0||m->timebase.denominator<=0||!zero(m->reserved))return MGC_INVALID_ARGUMENT;t->mapping=*m;return MGC_SUCCESS;}
MgcStatus mgm_timeline_may_present(MgmTimeline *t,MgmFrame *f){if(!t||!f)return MGC_INVALID_ARGUMENT;std::lock_guard<std::mutex> lock(t->mutex);return t->mapping.epoch==f->desc.epoch?MGC_SUCCESS:MGC_CANCELLED;}
MgcStatus mgm_identity_effect_create(MgcDomain *domain,MgmEffect **out,MgmEffectApi *api){if(out)*out=nullptr;if(!domain||!out)return MGC_INVALID_ARGUMENT;MgmEffectApi value{};uint32_t size=0;auto s=copy_desc(api,offsetof(MgmEffectApi,reserved),value,&size);if(s)return s;if(value.required_flags)return MGC_UNSUPPORTED_ABI;if(!zero(value.reserved))return MGC_INVALID_ARGUMENT;
    try{auto e=std::make_unique<MgmEffect>();e->domain=domain;e->worker=std::thread(worker,e.get());mgc_domain_retain(domain);*out=e.release();value.query=query;value.prepare=prepare;value.submit=submit;value.cancel=cancel;value.drain=drain;value.estimated_cost_ns=0;write_desc(api,value,size);return MGC_SUCCESS;}catch(const std::bad_alloc&){return MGC_OUT_OF_MEMORY;}catch(...){return MGC_INTERNAL_ERROR;}}
MgcStatus mgm_identity_effect_release(MgmEffect *e){if(!e)return MGC_INVALID_ARGUMENT;if(std::this_thread::get_id()==e->worker.get_id())return MGC_BUSY;{std::lock_guard<std::mutex> lock(e->mutex);if(e->preparing||e->pending||e->active)return MGC_BUSY;e->stopping=true;e->changed.notify_one();}e->worker.join();mgc_domain_release(e->domain);delete e;return MGC_SUCCESS;}
}
