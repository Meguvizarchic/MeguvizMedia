#include <meguviz/media_scheduler.h>
#include "../descriptor.h"
#include <algorithm>
#include <array>
#include <mutex>
#include <new>
struct MgmAdmission {
    MgmAdmissionConfig config{};
    std::mutex mutex;
    std::array<uint64_t,128> samples{};
    uint32_t count=0,next=0,in_flight=0;
};
extern "C" {
MgcStatus mgm_admission_create(const MgmAdmissionConfig *source,MgmAdmission **out){
    if(out)*out=nullptr;if(!out)return MGC_INVALID_ARGUMENT;
    MgmAdmissionConfig c{};auto s=mgm_detail::copy_desc(source,offsetof(MgmAdmissionConfig,reserved),c);if(s)return s;
    if(c.required_flags)return MGC_UNSUPPORTED_ABI;
    if(!mgm_detail::zero(c.reserved)||!c.sample_window||c.sample_window>128||c.percentile<50||c.percentile>100||!c.max_in_flight||c.max_in_flight>4||c.enabled>1||!c.cold_cost_ns||c.cold_cost_ns>10000000000ULL||c.safety_margin_ns>1000000000ULL)return MGC_INVALID_ARGUMENT;
    auto p=new(std::nothrow) MgmAdmission;if(!p)return MGC_OUT_OF_MEMORY;p->config=c;*out=p;return MGC_SUCCESS;
}
void mgm_admission_release(MgmAdmission *p){delete p;}
MgcStatus mgm_admission_reserve(MgmAdmission *p,int64_t now,int64_t deadline,MgmAdmissionDecision *out){
    if(!p||now<0||deadline<0)return MGC_INVALID_ARGUMENT;
    MgmAdmissionDecision d{};uint32_t size=0;auto s=mgm_detail::copy_desc(out,offsetof(MgmAdmissionDecision,reserved),d,&size);if(s)return s;
    std::lock_guard<std::mutex> lock(p->mutex);
    std::array<uint64_t,128> sorted=p->samples;std::sort(sorted.begin(),sorted.begin()+p->count);
    const uint64_t cost=p->count?sorted[(uint64_t(p->count)*p->config.percentile+99)/100-1]:p->config.cold_cost_ns;
    const uint64_t multiplier=uint64_t(p->in_flight)+1;
    d.predicted_joint_ns=cost>UINT64_MAX/multiplier?UINT64_MAX:cost*multiplier;
    if(d.predicted_joint_ns>UINT64_MAX-p->config.safety_margin_ns)d.predicted_joint_ns=UINT64_MAX;else d.predicted_joint_ns+=p->config.safety_margin_ns;
    d.in_flight=p->in_flight;d.sample_count=p->count;
    d.status=p->in_flight>=p->config.max_in_flight?MGC_BACKPRESSURE:MGC_ACCEPTED;
    if(d.status==MGC_ACCEPTED&&p->config.enabled&&(deadline<now||uint64_t(deadline-now)<d.predicted_joint_ns))d.status=MGC_DEADLINE_REJECTED;
    if(d.status==MGC_ACCEPTED)++p->in_flight;
    mgm_detail::write_desc(out,d,size);return d.status;
}
MgcStatus mgm_admission_retire(MgmAdmission *p,uint64_t joint,uint32_t success){
    if(!p||success>1||joint>60000000000ULL)return MGC_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(p->mutex);if(!p->in_flight)return MGC_INVALID_ARGUMENT;--p->in_flight;
    if(success&&joint){p->samples[p->next]=joint;p->next=(p->next+1)%p->config.sample_window;p->count=std::min(p->count+1,p->config.sample_window);}return MGC_SUCCESS;
}
}
