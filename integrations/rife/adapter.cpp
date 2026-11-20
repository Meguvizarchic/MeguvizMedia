#include <meguviz/media_rife.h>
#include "../../src/alpha.hpp"
#include "../../src/descriptor.h"
#include "../../src/frame_private.h"
#include "interpolate_pts.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>

#if defined(MGM_EXPERIMENTAL_SHARED_PLAN)
extern "C" MgcStatus mgr_private_submit_shared_ahb(MgrSession*,const MgrRequest*,MgrCompletionFn,void*,MgcContextFn,MgcContextFn,MgcTicket**);
extern "C" void mgr_private_release_shared_ahb(MgrPlan*);
#endif
namespace {
struct Job;
struct Effect {
#if defined(MGM_EXPERIMENTAL_SHARED_PLAN)
    bool shared_ahb=false;
    MgrPlan* shared_plan=nullptr;
#endif
    MgrSession *session=nullptr;
    std::mutex mutex;
    Job *active=nullptr;
    unsigned submitting=0;
    bool cancelled=false,closing=false;
    MgcTicket *ticket=nullptr;
};
struct Job {std::atomic<uint32_t> refs{1};Effect *effect;MgmEffectRequest request;MgmEffectCompletionFn callback;void *context;MgcContextFn retain_context,release_context;};
Effect *effect(MgmEffect *e){return reinterpret_cast<Effect*>(e);}
using mgm_detail::copy_desc;
using mgm_detail::write_desc;
using mgm_detail::zero;
#if defined(MGR_ENABLE_JOB_OBSERVATION)
const char rife_observation_tag=0;
struct RifeFrameObservation : mgm_detail::FrameAttachment {
    MgrJobObservation value{};
    RifeFrameObservation() {
        tag=&rife_observation_tag;
        destroy=[](mgm_detail::FrameAttachment *p) noexcept { delete static_cast<RifeFrameObservation*>(p); };
        value.header=MGC_HEADER_INIT(MgrJobObservation);
    }
};
void attach_observation(MgmFrame *frame,MgrResult *result,const MgrResultInfo &info,uint64_t request_id){
    if(!frame||!result||info.request_id!=request_id)return;
    auto snapshot=new(std::nothrow) RifeFrameObservation;
    if(!snapshot)return;
    const auto status=mgr_result_observation(result,&snapshot->value);
    if(status!=MGC_SUCCESS||snapshot->value.request_id!=request_id||snapshot->value.frame_kind!=info.frame_kind||
       (info.frame_kind==MGR_RESULT_INFERRED&&snapshot->value.effective_flow_scale!=info.effective_flow_scale)||
       (info.frame_kind==MGR_RESULT_ENDPOINT&&snapshot->value.effective_flow_scale!=0)){
        delete snapshot;return;
    }
    frame->attachment=snapshot;
}
#endif
void retain_job(void *p){auto j=static_cast<Job*>(p);++j->refs;if(j->retain_context)j->retain_context(j->context);}
void drop_job(Job *j){if(--j->refs==0){mgm_frame_release(j->request.left);mgm_frame_release(j->request.right);delete j;}}
void release_job(void *p){auto j=static_cast<Job*>(p);if(j->release_context)j->release_context(j->context);drop_job(j);}
void batch_event(void *p,const MgcBatchEvent *event){auto j=static_cast<Job*>(p);if(j->request.batch_event)j->request.batch_event(j->context,event);}
void completed(void *p,uint64_t id,MgcStatus status,MgrResult *result){auto j=static_cast<Job*>(p);MgmFrame *output=nullptr;
    if(status==MGC_SUCCESS&&result){
        MgrResultInfo info{};
        info.header=MGC_HEADER_INIT(MgrResultInfo);
        status=mgr_result_describe(result,&info);
        if(status==MGC_SUCCESS&&(!info.algorithm_accepted||!info.output))status=MGC_INTERNAL_ERROR;
        if(status==MGC_SUCCESS){
            // The job retains both frames through the completion callback.
            auto desc=j->request.left->desc;
            const auto &right=j->request.right->desc;
            desc.gpu_elapsed_ns=info.gpu_elapsed_ns;
            desc.gpu_batch_count=info.gpu_batch_count;
            desc.gpu_observed_batch_count=info.gpu_observed_batch_count;
            desc.gpu_elapsed_observed=info.gpu_elapsed_observed;
            desc.gpu_timestamp_valid_bits=info.gpu_timestamp_valid_bits;
            desc.gpu_timestamp_period_bits=info.gpu_timestamp_period_bits;
            desc.batch_event_coverage_observed=info.batch_event_coverage_observed;
            desc.worker_cpu_ns=info.worker_cpu_ns;
            desc.worker_cpu_observed=info.worker_cpu_observed;
            desc.task_id=j->request.request_id;
            desc.source_id_after=right.source_id;
            desc.content=info.output;
            desc.pts_ticks=mgm_detail::interpolate_pts(desc.pts_ticks,right.pts_ticks,j->request.alpha);
            desc.diagnostics.header=MGC_HEADER_INIT(MgmAlgorithmDiagnostics);
            desc.diagnostics.schema=MGM_DIAGNOSTICS_RIFE_LEGACY;
            desc.diagnostics.values[0]=info.unreliable_permille;
            desc.diagnostics.values[1]=info.patched_tiles;
            desc.diagnostics.values[2]=info.histogram_permille;
            desc.diagnostics.values[3]=info.effective_flow_scale;
            for(size_t stage=0;stage<3;++stage)desc.diagnostics.stage_ns[stage]=info.stage_ns[stage];
            desc.frame_kind=info.frame_kind==MGR_RESULT_INFERRED?MGM_FRAME_INFERRED:
                (info.frame_kind==MGR_RESULT_ENDPOINT?MGM_FRAME_ENDPOINT:MGM_FRAME_BYPASS);
            status=mgm_frame_create(&desc,&output);
#if defined(MGR_ENABLE_JOB_OBSERVATION)
            if(status==MGC_SUCCESS)attach_observation(output,result,info,j->request.request_id);
#endif
        }
    }
    if(result)mgr_result_release(result);j->callback(j->context,id,status,output);
    MgcTicket *ticket=nullptr;
    {
        std::lock_guard<std::mutex> lock(j->effect->mutex);
        if(j->effect->active==j){
            ticket=j->effect->ticket;
            j->effect->ticket=nullptr;
            j->effect->active=nullptr;
        }
    }
    if(ticket)mgc_ticket_release(ticket);
}
MgcStatus query(MgmEffect *e,MgcCapability *c){if(!e)return MGC_INVALID_ARGUMENT;MgcCapability value{};uint32_t size=0;auto status=copy_desc(c,offsetof(MgcCapability,reserved),value,&size);if(status)return status;if(!zero(value.reserved))return MGC_INVALID_ARGUMENT;value.feature=MGC_CAP_HOST_JOB_ASYNC;value.advertised=1;value.enabled=1;value.probe_result=MGC_SUCCESS;value.workload_qualified=0;value.reason=MGC_SUCCESS;write_desc(c,value,size);return MGC_SUCCESS;}
bool supported_color(const MgmColor &color){
    return color.transfer==MGM_TRANSFER_SDR&&color.bit_depth==8&&
        color.range==MGM_RANGE_FULL&&!color.premultiplied;
}
MgcStatus prepare(MgmEffect *e,const MgmFrameDesc *d){if(!e)return MGC_INVALID_ARGUMENT;MgmFrameDesc copied{};auto status=copy_desc(d,offsetof(MgmFrameDesc,reserved),copied);if(status)return status;d=&copied;if(d->required_flags)return MGC_UNSUPPORTED_ABI;for(auto v:d->reserved)if(v)return MGC_INVALID_ARGUMENT;MgmColor color{};status=mgm_detail::copy_color(d->color,color);if(status)return status;return supported_color(color)?MGC_SUCCESS:MGC_UNSUPPORTED_CAPABILITY;}
MgcStatus submit(MgmEffect *opaque,const MgmEffectRequest *r,MgmEffectCompletionFn callback,void *context,MgcContextFn retain,MgcContextFn release){if(!opaque||!callback)return MGC_INVALID_ARGUMENT;MgmEffectRequest copied{};uint32_t request_size=0;auto status=copy_desc(r,offsetof(MgmEffectRequest,reserved),copied,&request_size);if(status)return status;if(!mgm_detail::effect_request_prefix_complete(request_size))return MGC_INVALID_ARGUMENT;r=&copied;if(r->required_flags)return MGC_UNSUPPORTED_ABI;for(auto v:r->reserved)if(v)return MGC_INVALID_ARGUMENT;if(!r->left||!r->right||!mgm_detail::valid_alpha(r->alpha)||!!retain!=!!release)return MGC_INVALID_ARGUMENT;auto e=effect(opaque);
    // mgm_frame_create validates the ABI of these immutable descriptors.
    const auto &a=r->left->desc;
    const auto &b=r->right->desc;
    if(a.epoch!=r->epoch||b.epoch!=r->epoch||a.timebase.numerator!=b.timebase.numerator||a.timebase.denominator!=b.timebase.denominator)return MGC_INVALID_ARGUMENT;
    if(!supported_color(a.color)||!supported_color(b.color))return MGC_UNSUPPORTED_CAPABILITY;
    if(a.color.primaries!=b.color.primaries||a.color.transfer!=b.color.transfer||a.color.range!=b.color.range||a.color.matrix!=b.color.matrix||a.color.chroma_siting!=b.color.chroma_siting||a.color.bit_depth!=b.color.bit_depth||a.color.premultiplied!=b.color.premultiplied||a.rotation_degrees!=b.rotation_degrees||a.crop_left!=b.crop_left||a.crop_top!=b.crop_top||a.crop_width!=b.crop_width||a.crop_height!=b.crop_height)return MGC_UNSUPPORTED_CAPABILITY;
    MgcLeaseInfo info{};info.header=MGC_HEADER_INIT(MgcLeaseInfo);status=mgc_lease_info(a.content,&info);if(status)return status;
    auto job=new(std::nothrow)Job;if(!job)return MGC_OUT_OF_MEMORY;job->effect=e;job->request=*r;job->callback=callback;job->context=context;job->retain_context=retain;job->release_context=release;mgm_frame_retain(r->left);mgm_frame_retain(r->right);
    {
        std::lock_guard<std::mutex> lock(e->mutex);
        if(e->active||e->closing){drop_job(job);return e->closing?MGC_BUSY:MGC_BACKPRESSURE;}
        e->active=job;
        e->cancelled=false;
        ++e->submitting;
    }
    MgrRequest request{};request.header=MGC_HEADER_INIT(MgrRequest);request.request_id=r->request_id;request.batch_event=r->batch_event?batch_event:nullptr;request.left=a.content;request.right=b.content;request.alpha=r->alpha;request.output.header=MGC_HEADER_INIT(MgrOutputSpec);request.output.width=info.width;request.output.height=info.height;request.output.format=info.format;request.output.row_stride_bytes=uint64_t(info.width)*(info.format==MGC_FORMAT_RGBA8?4:3);request.flow_scale=r->algorithm_scale;
    if(info.format==MGC_FORMAT_RGBA8){request.output_storage_usage=MGC_RESOURCE_ANDROID_AHB;request.output_sync_mode=r->output_sync_mode;}
    mgc_lease_retain(a.content);mgc_lease_retain(b.content);MgcTicket *ticket=nullptr;
#if defined(MGM_EXPERIMENTAL_SHARED_PLAN)
    status=e->shared_ahb?mgr_private_submit_shared_ahb(e->session,&request,completed,job,retain_job,release_job,&ticket):
        mgr_submit(e->session,&request,completed,job,retain_job,release_job,&ticket);
#else
    status=mgr_submit(e->session,&request,completed,job,retain_job,release_job,&ticket);
#endif
    if(status==MGC_ACCEPTED){mgm_frame_release(r->left);mgm_frame_release(r->right);}
    else {mgc_lease_release(a.content);mgc_lease_release(b.content);}
    {
        std::lock_guard<std::mutex> lock(e->mutex);
        // Completion can free the slot before this submit call receives its
        // ticket. Match the exact job even if another request reused its ID.
        if(e->active==job){
            if(status==MGC_ACCEPTED){
                e->ticket=ticket;
                if(e->cancelled)mgr_cancel(e->session,ticket);
                ticket=nullptr;
            }else e->active=nullptr;
        }
        --e->submitting;
    }
    if(ticket)mgc_ticket_release(ticket);
    drop_job(job);
    return status;
}
MgcStatus cancel(MgmEffect *opaque,uint64_t id){
    if(!opaque)return MGC_INVALID_ARGUMENT;
    auto e=effect(opaque);
    std::lock_guard<std::mutex> lock(e->mutex);
    if(!e->active||e->active->request.request_id!=id)return MGC_SUCCESS;
    e->cancelled=true;
    return e->ticket?mgr_cancel(e->session,e->ticket):MGC_SUCCESS;
}
MgcStatus drain(MgmEffect *opaque,uint64_t timeout){return opaque?mgr_drain(effect(opaque)->session,timeout):MGC_INVALID_ARGUMENT;}
}
extern "C" {
MgcStatus mgm_rife_frame_observation(MgmFrame *frame,MgrJobObservation *out){
    if(!frame||!out)return MGC_INVALID_ARGUMENT;
    const uint32_t size=out->header.struct_size;
    constexpr size_t required=offsetof(MgrJobObservation,reserved);
    if(out->header.abi_major!=MGR_ABI_MAJOR||size<required||
       (size>required&&size<sizeof(MgrJobObservation)&&(size-required)%sizeof(uint32_t)))return MGC_UNSUPPORTED_ABI;
#if defined(MGR_ENABLE_JOB_OBSERVATION)
    if(!frame->attachment||frame->attachment->tag!=&rife_observation_tag)return MGC_UNSUPPORTED_CAPABILITY;
    auto value=static_cast<RifeFrameObservation*>(frame->attachment)->value;
    value.header.struct_size=size;
    std::memcpy(out,&value,size<sizeof(value)?size:sizeof(value));return MGC_SUCCESS;
#else
    return MGC_UNSUPPORTED_CAPABILITY;
#endif
}
MgcStatus mgm_rife_effect_create(MgrPlan *plan,MgmEffect **out,MgmEffectApi *api){if(out)*out=nullptr;if(!plan||!out)return MGC_INVALID_ARGUMENT;MgmEffectApi value{};uint32_t size=0;auto status=copy_desc(api,offsetof(MgmEffectApi,reserved),value,&size);if(status)return status;if(value.required_flags)return MGC_UNSUPPORTED_ABI;if(!zero(value.reserved))return MGC_INVALID_ARGUMENT;auto e=new(std::nothrow)Effect;if(!e)return MGC_OUT_OF_MEMORY;MgcSessionDesc desc{};desc.header=MGC_HEADER_INIT(MgcSessionDesc);desc.max_in_flight=1;status=mgr_session_create(plan,&desc,&e->session);if(status){delete e;return status;}value.query=query;value.prepare=prepare;value.submit=submit;value.cancel=cancel;value.drain=drain;value.estimated_cost_ns=0;write_desc(api,value,size);*out=reinterpret_cast<MgmEffect*>(e);return MGC_SUCCESS;}
#if defined(MGM_EXPERIMENTAL_SHARED_PLAN)
// Contract: the caller must have enabled the plan for shared AHB via
// mgr_private_enable_shared_ahb before creating this effect, and must pair
// release with mgr_private_release_shared_ahb. Submits on a non-shared plan
// fail with MGC_UNSUPPORTED_CAPABILITY rather than silently using RGB storage.
MgcStatus mgm_private_rife_effect_shared(MgrPlan* plan,MgmEffect** out,MgmEffectApi* api) {
    auto status=mgm_rife_effect_create(plan,out,api);
    if(!status){auto* e=effect(*out);mgr_plan_retain(plan);e->shared_plan=plan;e->shared_ahb=true;}
    return status;
}
#endif
MgcStatus mgm_rife_effect_release(MgmEffect *opaque){if(!opaque)return MGC_INVALID_ARGUMENT;auto e=effect(opaque);{std::lock_guard<std::mutex> lock(e->mutex);if(e->active||e->submitting||e->closing)return MGC_BUSY;e->closing=true;}auto status=mgr_session_close(e->session);if(status){std::lock_guard<std::mutex> lock(e->mutex);e->closing=false;return status;}
#if defined(MGM_EXPERIMENTAL_SHARED_PLAN)
    if(e->shared_plan){mgr_private_release_shared_ahb(e->shared_plan);mgr_plan_release(e->shared_plan);e->shared_plan=nullptr;}
#endif
    delete e;return MGC_SUCCESS;}
}
