#include <jni.h>
#include <meguviz/media_cadence.h>
#include <map>
#include <memory>
#include <mutex>
#include <cstring>

namespace {
template<class Core,void(*Release)(Core*)>struct Holder {Core* core=nullptr;~Holder(){Release(core);}};
using Cadence=Holder<MgmCadence,mgm_cadence_release>;
using Grid=Holder<MgmVsyncGrid,mgm_vsync_grid_release>;
using Meter=Holder<MgmVsyncMeter,mgm_vsync_meter_release>;
std::mutex registry_mutex;uint64_t sequence=0;
std::map<jlong,std::shared_ptr<Cadence>> cadences;
std::map<jlong,std::shared_ptr<Grid>> grids;
std::map<jlong,std::shared_ptr<Meter>> meters;
template<class T>std::shared_ptr<T> acquire(std::map<jlong,std::shared_ptr<T>>& entries,jlong handle){std::lock_guard<std::mutex> lock(registry_mutex);auto i=entries.find(handle);return i==entries.end()?nullptr:i->second;}
template<class T>void release(std::map<jlong,std::shared_ptr<T>>& entries,jlong handle){std::shared_ptr<T> removed;{std::lock_guard<std::mutex> lock(registry_mutex);auto i=entries.find(handle);if(i!=entries.end()){removed=std::move(i->second);entries.erase(i);}}}
template<class T>MgcStatus save(std::map<jlong,std::shared_ptr<T>>& entries,const std::shared_ptr<T>& owner,jlong& handle){std::lock_guard<std::mutex> lock(registry_mutex);if(entries.size()>=128||sequence==uint64_t(INT64_MAX))return MGC_BACKPRESSURE;handle=jlong(++sequence);entries.emplace(handle,owner);return MGC_SUCCESS;}
jlongArray values(JNIEnv* env,std::initializer_list<jlong> source){auto result=env->NewLongArray(jsize(source.size()));if(result)env->SetLongArrayRegion(result,0,jsize(source.size()),source.begin());return result;}
}
#define CADENCE(name) Java_io_github_jichuo1_meguviz_media_MediaCadence_##name
#define GRID(name) Java_io_github_jichuo1_meguviz_media_MediaVsyncGrid_##name
#define METER(name) Java_io_github_jichuo1_meguviz_media_MediaVsyncMeter_##name
extern "C" JNIEXPORT jlongArray JNICALL CADENCE(nCreate)(JNIEnv* env,jclass,jint fps,jint minimum,jboolean required){
    if(fps<=0||minimum<0)return values(env,{MGC_INVALID_ARGUMENT,0});
    try{auto p=std::make_shared<Cadence>();MgmCadenceConfig c{};c.header=MGC_HEADER_INIT(MgmCadenceConfig);c.target_fps=uint32_t(fps);c.min_samples=uint32_t(minimum);c.interpolation_required=required==JNI_TRUE;
        auto status=mgm_cadence_create(&c,&p->core);if(status)return values(env,{status,0});jlong handle=0;status=save(cadences,p,handle);if(status)return values(env,{status,0});
        auto result=values(env,{MGC_SUCCESS,handle});if(!result)release(cadences,handle);return result;}catch(...){return values(env,{MGC_OUT_OF_MEMORY,0});}
}
extern "C" JNIEXPORT jint JNICALL CADENCE(nAdd)(JNIEnv*,jclass,jlong handle,jlong pts){auto p=acquire(cadences,handle);return p?mgm_cadence_add(p->core,pts):MGC_INVALID_ARGUMENT;}
extern "C" JNIEXPORT jint JNICALL CADENCE(nReset)(JNIEnv*,jclass,jlong handle){auto p=acquire(cadences,handle);return p?mgm_cadence_reset(p->core):MGC_INVALID_ARGUMENT;}
extern "C" JNIEXPORT jlongArray JNICALL CADENCE(nSnapshot)(JNIEnv* env,jclass,jlong handle){auto p=acquire(cadences,handle);if(!p)return values(env,{MGC_INVALID_ARGUMENT});MgmCadenceSnapshot s{};s.header=MGC_HEADER_INIT(MgmCadenceSnapshot);auto status=mgm_cadence_snapshot(p->core,&s);if(status)return values(env,{status});jlong bits=0;static_assert(sizeof(bits)==sizeof(s.source_fps),"binary64 JNI transport");std::memcpy(&bits,&s.source_fps,sizeof(bits));return values(env,{MGC_SUCCESS,s.has_fps,bits,jlong(s.frames),jlong(s.gaps),s.sample_count,s.median_us,s.verdict});}
extern "C" JNIEXPORT void JNICALL CADENCE(nRelease)(JNIEnv*,jclass,jlong handle){release(cadences,handle);}
extern "C" JNIEXPORT jlongArray JNICALL GRID(nCreate)(JNIEnv* env,jclass,jlong period){
    try{auto p=std::make_shared<Grid>();MgmVsyncConfig c{};c.header=MGC_HEADER_INIT(MgmVsyncConfig);c.content_period_ns=period;auto status=mgm_vsync_grid_create(&c,&p->core);if(status)return values(env,{status,0});
        MgmVsyncSnapshot s{};s.header=MGC_HEADER_INIT(MgmVsyncSnapshot);status=mgm_vsync_grid_snapshot(p->core,0,0,&s);if(status)return values(env,{status,0});
        jlong handle=0;status=save(grids,p,handle);if(status)return values(env,{status,0});auto result=values(env,{MGC_SUCCESS,handle,s.last_ns,s.period_ns,s.faster_than_content,s.slower_than_content});if(!result)release(grids,handle);return result;
    }catch(...){return values(env,{MGC_OUT_OF_MEMORY,0});}
}
extern "C" JNIEXPORT jlongArray JNICALL GRID(nOnVsync)(JNIEnv* env,jclass,jlong handle,jlong vsync){auto p=acquire(grids,handle);if(!p)return values(env,{MGC_INVALID_ARGUMENT,0});uint32_t accepted=0;auto status=mgm_vsync_grid_on_vsync(p->core,vsync,&accepted);if(status||!accepted)return values(env,{status,0});MgmVsyncSnapshot s{};s.header=MGC_HEADER_INIT(MgmVsyncSnapshot);status=mgm_vsync_grid_snapshot(p->core,0,vsync,&s);return values(env,{status,accepted,s.last_ns,s.period_ns,s.faster_than_content,s.slower_than_content});}
extern "C" JNIEXPORT jlongArray JNICALL GRID(nSnapshot)(JNIEnv* env,jclass,jlong handle,jlong due,jlong now){auto p=acquire(grids,handle);if(!p)return values(env,{MGC_INVALID_ARGUMENT});MgmVsyncSnapshot s{};s.header=MGC_HEADER_INIT(MgmVsyncSnapshot);auto status=mgm_vsync_grid_snapshot(p->core,due,now,&s);return values(env,{status,s.last_ns,s.period_ns,s.current_ns,s.until_next_ns,s.stale,s.releasable,s.same_refresh,s.faster_than_content,s.slower_than_content,s.sample_count});}
extern "C" JNIEXPORT void JNICALL GRID(nRelease)(JNIEnv*,jclass,jlong handle){release(grids,handle);}
extern "C" JNIEXPORT jlongArray JNICALL METER(nCreate)(JNIEnv* env,jclass,jint warmup,jint samples){
    if(warmup<0||samples<1||samples>4096)return values(env,{MGC_INVALID_ARGUMENT,0});try{auto p=std::make_shared<Meter>();MgmVsyncMeterConfig c{};c.header=MGC_HEADER_INIT(MgmVsyncMeterConfig);c.warmup=uint32_t(warmup);c.samples=uint32_t(samples);auto status=mgm_vsync_meter_create(&c,&p->core);if(status)return values(env,{status,0});jlong handle=0;status=save(meters,p,handle);if(status)return values(env,{status,0});auto result=values(env,{MGC_SUCCESS,handle});if(!result)release(meters,handle);return result;}catch(...){return values(env,{MGC_OUT_OF_MEMORY,0});}
}
extern "C" JNIEXPORT jlongArray JNICALL METER(nAdd)(JNIEnv* env,jclass,jlong handle,jlong frame){auto p=acquire(meters,handle);if(!p)return values(env,{MGC_INVALID_ARGUMENT});uint32_t reported=0;float hz=0;auto status=mgm_vsync_meter_add(p->core,frame,&reported,&hz);uint32_t bits=0;static_assert(sizeof(bits)==sizeof(hz),"binary32 JNI transport");std::memcpy(&bits,&hz,sizeof(bits));return values(env,{status,reported,jlong(bits)});}
extern "C" JNIEXPORT void JNICALL METER(nRelease)(JNIEnv*,jclass,jlong handle){release(meters,handle);}
