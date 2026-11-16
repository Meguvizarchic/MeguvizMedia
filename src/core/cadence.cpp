#include <meguviz/media_cadence.h>
#include "../descriptor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <new>
#include <vector>
#include <memory>

namespace {
constexpr int64_t OverdueNs=2000000;
bool add(int64_t a,int64_t b,int64_t& out){if((b>0&&a>INT64_MAX-b)||(b<0&&a<INT64_MIN-b))return false;out=a+b;return true;}
bool subtract(int64_t a,int64_t b,int64_t& out){if((b>0&&a<INT64_MIN+b)||(b<0&&a>INT64_MAX+b))return false;out=a-b;return true;}
double round_even(double value){const double base=std::floor(value),fraction=value-base;if(fraction<0.5)return base;if(fraction>0.5)return base+1;return std::fmod(base,2.0)==0?base:base+1;}
template<class T>MgcStatus output_header(T* out,uint32_t& size){MgcHeader h{};auto status=mgm_detail::read_header(out,offsetof(T,reserved),h);size=h.struct_size;return status;}
}
struct MgmCadence {
    std::mutex mutex;MgmCadenceConfig config{};std::array<int64_t,31> intervals{};uint32_t count=0;
    int64_t last=INT64_MIN;uint64_t frames=0,gaps=0;
    int64_t median()const{auto sorted=intervals;std::sort(sorted.begin(),sorted.begin()+count);return count?sorted[count/2]:0;}
};
struct MgmVsyncGrid {
    std::mutex mutex;int64_t content=0,last=0,period=0;std::array<int64_t,32>deltas{};uint32_t count=0,cursor=0;
};
struct MgmVsyncMeter {
    std::mutex mutex;uint32_t warmup=0,samples=0,count=0;uint64_t seen=0;int64_t last=INT64_MIN;bool reported=false;
    std::vector<int64_t> intervals;
};
extern "C" {
MgcStatus mgm_cadence_create(const MgmCadenceConfig* source,MgmCadence** out){
    if(!out)return MGC_INVALID_ARGUMENT;*out=nullptr;MgmCadenceConfig c{};auto status=mgm_detail::copy_desc(source,offsetof(MgmCadenceConfig,reserved),c);if(status)return status;
    if(c.required_flags)return MGC_UNSUPPORTED_ABI;if(!c.target_fps||c.interpolation_required>1||!mgm_detail::zero(c.reserved))return MGC_INVALID_ARGUMENT;
    try{auto p=new MgmCadence;p->config=c;*out=p;return MGC_SUCCESS;}catch(...){return MGC_OUT_OF_MEMORY;}
}
void mgm_cadence_release(MgmCadence* p){delete p;}
MgcStatus mgm_cadence_add(MgmCadence* p,int64_t pts){
    if(!p)return MGC_INVALID_ARGUMENT;std::lock_guard<std::mutex> lock(p->mutex);int64_t interval=0;
    if(p->last!=INT64_MIN&&pts>p->last&&!subtract(pts,p->last,interval))return MGC_INVALID_ARGUMENT;
    const auto previous=p->last;p->last=pts;p->frames++;if(previous==INT64_MIN||pts<=previous)return MGC_SUCCESS;
    const int64_t median=p->median();int64_t threshold=0;
    if(p->count&&add(median,median/2,threshold)&&interval>threshold){p->gaps++;return MGC_SUCCESS;}
    if(p->count==p->intervals.size())std::move(p->intervals.begin()+1,p->intervals.end(),p->intervals.begin());else p->count++;
    p->intervals[p->count-1]=interval;return MGC_SUCCESS;
}
MgcStatus mgm_cadence_reset(MgmCadence* p){if(!p)return MGC_INVALID_ARGUMENT;std::lock_guard<std::mutex> lock(p->mutex);p->last=INT64_MIN;return MGC_SUCCESS;}
MgcStatus mgm_cadence_snapshot(MgmCadence* p,MgmCadenceSnapshot* out){
    if(!p)return MGC_INVALID_ARGUMENT;uint32_t size=0;auto status=output_header(out,size);if(status)return status;std::lock_guard<std::mutex> lock(p->mutex);
    MgmCadenceSnapshot r{};r.header=MGC_HEADER_INIT(MgmCadenceSnapshot);r.frames=p->frames;r.gaps=p->gaps;r.sample_count=p->count;r.median_us=p->median();
    if(p->count){r.has_fps=1;r.source_fps=1000000.0/double(r.median_us);}
    if(p->count>=p->config.min_samples&&r.has_fps){const double ratio=double(p->config.target_fps)/r.source_fps,nearest=round_even(ratio);
        r.verdict=nearest>=(p->config.interpolation_required?2.0:1.0)&&std::abs(ratio-nearest)/nearest<=0.02?MGM_CADENCE_SUITABLE:MGM_CADENCE_UNSUITABLE;}
    mgm_detail::write_desc(out,r,size);return MGC_SUCCESS;
}
MgcStatus mgm_vsync_grid_create(const MgmVsyncConfig* source,MgmVsyncGrid** out){
    if(!out)return MGC_INVALID_ARGUMENT;*out=nullptr;MgmVsyncConfig c{};auto status=mgm_detail::copy_desc(source,offsetof(MgmVsyncConfig,reserved),c);if(status)return status;
    if(c.required_flags)return MGC_UNSUPPORTED_ABI;if(c.content_period_ns<=0||c.content_period_ns>INT64_MAX/32||!mgm_detail::zero(c.reserved))return MGC_INVALID_ARGUMENT;
    try{auto p=new MgmVsyncGrid;p->content=p->period=c.content_period_ns;*out=p;return MGC_SUCCESS;}catch(...){return MGC_OUT_OF_MEMORY;}
}
void mgm_vsync_grid_release(MgmVsyncGrid* p){delete p;}
MgcStatus mgm_vsync_grid_on_vsync(MgmVsyncGrid* p,int64_t vsync,uint32_t* accepted){
    if(!p||!accepted)return MGC_INVALID_ARGUMENT;*accepted=0;std::lock_guard<std::mutex> lock(p->mutex);if(vsync<=p->last)return MGC_SUCCESS;
    if(p->last){const int64_t delta=vsync-p->last;if(delta>INT64_MAX/32)return MGC_INVALID_ARGUMENT;p->deltas[p->cursor]=delta;p->cursor=(p->cursor+1)%32;
        if(p->count<32)p->count++;auto sorted=p->deltas;std::sort(sorted.begin(),sorted.begin()+p->count);p->period=sorted[p->count/4];}
    p->last=vsync;*accepted=1;return MGC_SUCCESS;
}
MgcStatus mgm_vsync_grid_snapshot(MgmVsyncGrid* p,int64_t due,int64_t now,MgmVsyncSnapshot* out){
    if(!p)return MGC_INVALID_ARGUMENT;uint32_t size=0;auto status=output_header(out,size);if(status)return status;std::lock_guard<std::mutex> lock(p->mutex);
    MgmVsyncSnapshot r{};r.header=MGC_HEADER_INIT(MgmVsyncSnapshot);r.last_ns=p->last;r.period_ns=p->period;r.sample_count=p->count;
    int64_t elapsed=0;if(!subtract(now,p->last,elapsed))return MGC_INVALID_ARGUMENT;
    r.current_ns=p->last;if(elapsed>=p->period+OverdueNs){const int64_t advance=(elapsed-OverdueNs)/p->period*p->period;if(!add(p->last,advance,r.current_ns))return MGC_INVALID_ARGUMENT;}
    int64_t remaining=0;if(!subtract(r.current_ns,now,remaining)||!add(remaining,p->period+OverdueNs,r.until_next_ns))return MGC_INVALID_ARGUMENT;
    r.stale=!p->last||elapsed>=4*p->content;
    const int64_t lead=std::max(p->content,p->period)+p->period/2,refresh=p->period+p->period/2;
    r.releasable=due<=r.current_ns||due-r.current_ns<=lead;r.same_refresh=due<=r.current_ns||due-r.current_ns<=refresh;
    r.faster_than_content=p->period*20<p->content*19;r.slower_than_content=p->period*20>p->content*21;
    mgm_detail::write_desc(out,r,size);return MGC_SUCCESS;
}
MgcStatus mgm_vsync_meter_create(const MgmVsyncMeterConfig* source,MgmVsyncMeter** out){
    if(!out)return MGC_INVALID_ARGUMENT;*out=nullptr;MgmVsyncMeterConfig c{};auto status=mgm_detail::copy_desc(source,offsetof(MgmVsyncMeterConfig,reserved),c);if(status)return status;
    if(c.required_flags)return MGC_UNSUPPORTED_ABI;if(!c.samples||c.samples>4096||c.reserved)return MGC_INVALID_ARGUMENT;
    try{auto p=std::make_unique<MgmVsyncMeter>();p->warmup=c.warmup;p->samples=c.samples;p->intervals.resize(c.samples);*out=p.release();return MGC_SUCCESS;}catch(...){return MGC_OUT_OF_MEMORY;}
}
void mgm_vsync_meter_release(MgmVsyncMeter* p){delete p;}
MgcStatus mgm_vsync_meter_add(MgmVsyncMeter* p,int64_t frame,uint32_t* reported,float* hz){
    if(!p||!reported||!hz)return MGC_INVALID_ARGUMENT;*reported=0;*hz=0;std::lock_guard<std::mutex> lock(p->mutex);int64_t delta=0;
    if(!p->reported&&p->last!=INT64_MIN&&frame>p->last&&!subtract(frame,p->last,delta))return MGC_INVALID_ARGUMENT;
    const auto previous=p->last;p->last=frame;if(p->reported||previous==INT64_MIN||frame<=previous)return MGC_SUCCESS;
    if(++p->seen<=p->warmup)return MGC_SUCCESS;p->intervals[p->count++]=delta;if(p->count<p->samples)return MGC_SUCCESS;
    std::array<int64_t,4096> sorted{};std::copy(p->intervals.begin(),p->intervals.end(),sorted.begin());
    std::sort(sorted.begin(),sorted.begin()+p->samples);p->reported=true;*hz=float(1000000000.0/double(sorted[p->samples/2]));*reported=1;return MGC_SUCCESS;
}
}
