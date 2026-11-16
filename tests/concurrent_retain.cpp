#include <meguviz/media.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

template<class T> T init(){T x{};x.header=MGC_HEADER_INIT(T);return x;}
struct Context {std::mutex mutex;std::condition_variable cv;bool entered=false,proceed=false;std::atomic<int> retain{0},release{0},complete{0};MgcStatus result=MGC_INTERNAL_ERROR;};
void retain(void *p){auto &c=*static_cast<Context*>(p);++c.retain;std::unique_lock<std::mutex> l(c.mutex);c.entered=true;c.cv.notify_all();c.cv.wait(l,[&]{return c.proceed;});}
void release(void *p){++static_cast<Context*>(p)->release;}
void complete(void *p,uint64_t,MgcStatus s,MgmFrame *out){auto &c=*static_cast<Context*>(p);c.result=s;++c.complete;mgm_frame_release(out);}
int main(){auto rd=init<MgcRuntimeDesc>();rd.provider=MGC_PROVIDER_CPU_REFERENCE;rd.memory_budget_bytes=4096;MgcRuntime *runtime=nullptr;if(mgc_runtime_create(&rd,&runtime))return 2;
 auto dd=init<MgcDomainDesc>();MgcDomain *domain=nullptr;if(mgc_domain_acquire(runtime,&dd,&domain))return 3;auto r=init<MgcResourceDesc>();r.width=2;r.height=2;r.format=MGC_FORMAT_RGB8;r.row_stride_bytes=6;r.capacity_bytes=12;MgcResource *resource=nullptr;if(mgc_resource_create(domain,&r,&resource))return 4;
 auto ld=init<MgcLeaseDesc>();ld.content_id=1;ld.access=MGC_ACCESS_READ;ld.byte_length=12;MgcLease *lease=nullptr;if(mgc_lease_create(resource,&ld,&lease))return 5;auto fd=init<MgmFrameDesc>();fd.content=lease;fd.epoch=1;fd.timebase={1,1000};fd.frame_kind=MGM_FRAME_ORIGINAL;MgmFrame *frame=nullptr;if(mgm_frame_create(&fd,&frame))return 6;
 MgmEffect *effect=nullptr;auto api=init<MgmEffectApi>();if(mgm_identity_effect_create(domain,&effect,&api))return 7;Context first,rejected;
 auto q=init<MgmEffectRequest>();q.request_id=77;q.epoch=1;q.left=frame;mgm_frame_retain(frame);MgcStatus accepted=MGC_INTERNAL_ERROR;
 std::thread worker([&]{accepted=api.submit(effect,&q,complete,&first,retain,release);});
 {std::unique_lock<std::mutex> l(first.mutex);if(!first.cv.wait_for(l,std::chrono::seconds(2),[&]{return first.entered;}))return 8;}
 auto during_drain=api.drain(effect,0);auto during_close=mgm_identity_effect_release(effect);auto cancellation=api.cancel(effect,77);
 auto q2=q;q2.request_id=78;mgm_frame_retain(frame);auto denied=api.submit(effect,&q2,complete,&rejected,retain,release);if(denied!=MGC_ACCEPTED)mgm_frame_release(frame);
 {std::lock_guard<std::mutex> l(first.mutex);first.proceed=true;}first.cv.notify_all();worker.join();auto drained=api.drain(effect,5000000000ULL);auto closed=mgm_identity_effect_release(effect);
 std::printf("RESULT submit=%d denied=%d drain_during=%d close_during=%d cancel=%d drain=%d close=%d retain=%d release=%d completion=%d completion_status=%d rejected_retain=%d rejected_release=%d\n",accepted,denied,during_drain,during_close,cancellation,drained,closed,first.retain.load(),first.release.load(),first.complete.load(),first.result,rejected.retain.load(),rejected.release.load());
 mgm_frame_release(frame);mgc_lease_release(lease);mgc_resource_release(resource);mgc_domain_release(domain);auto rc=mgc_runtime_close(runtime);mgc_runtime_release(runtime);
 return accepted==MGC_ACCEPTED&&denied==MGC_BACKPRESSURE&&during_drain==MGC_TIMEOUT&&during_close==MGC_BUSY&&cancellation==MGC_SUCCESS&&drained==MGC_SUCCESS&&closed==MGC_SUCCESS&&first.retain==1&&first.release==1&&first.complete==1&&first.result==MGC_CANCELLED&&rejected.retain==0&&rejected.release==0&&rc==MGC_SUCCESS?0:9;
}
