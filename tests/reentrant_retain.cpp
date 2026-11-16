#include <meguviz/media.h>
#include <atomic>
#include <cstdio>

template<class T> T init(){T x{};x.header=MGC_HEADER_INIT(T);return x;}
struct Context { MgmEffect *effect{}; MgmEffectApi api{}; std::atomic<int> retained{0},released{0},completed{0}; MgcStatus cancel_status=MGC_INTERNAL_ERROR,drain_status=MGC_INTERNAL_ERROR,release_status=MGC_INTERNAL_ERROR,completion_status=MGC_INTERNAL_ERROR; };
void retain(void *p){auto &c=*static_cast<Context*>(p);std::puts("RETAIN_ENTER");std::fflush(stdout);++c.retained;
  c.cancel_status=c.api.cancel(c.effect,77);c.drain_status=c.api.drain(c.effect,0);c.release_status=mgm_identity_effect_release(c.effect);
  std::puts("RETAIN_RETURN");std::fflush(stdout);}
void release(void *p){++static_cast<Context*>(p)->released;}
void complete(void *p,uint64_t,MgcStatus status,MgmFrame *out){auto &c=*static_cast<Context*>(p);c.completion_status=status;++c.completed;mgm_frame_release(out);}
int main(){auto rd=init<MgcRuntimeDesc>();rd.provider=MGC_PROVIDER_CPU_REFERENCE;rd.memory_budget_bytes=4096;MgcRuntime *runtime=nullptr;
 if(mgc_runtime_create(&rd,&runtime))return 2;auto dd=init<MgcDomainDesc>();MgcDomain *domain=nullptr;if(mgc_domain_acquire(runtime,&dd,&domain))return 3;
 auto r=init<MgcResourceDesc>();r.width=2;r.height=2;r.format=MGC_FORMAT_RGB8;r.row_stride_bytes=6;r.capacity_bytes=12;MgcResource *resource=nullptr;if(mgc_resource_create(domain,&r,&resource))return 4;
 auto ld=init<MgcLeaseDesc>();ld.content_id=1;ld.access=MGC_ACCESS_READ;ld.byte_length=12;MgcLease *lease=nullptr;if(mgc_lease_create(resource,&ld,&lease))return 5;
 auto fd=init<MgmFrameDesc>();fd.content=lease;fd.epoch=1;fd.timebase={1,1000};fd.frame_kind=MGM_FRAME_ORIGINAL;MgmFrame *frame=nullptr;if(mgm_frame_create(&fd,&frame))return 6;
 Context c;c.api=init<MgmEffectApi>();if(mgm_identity_effect_create(domain,&c.effect,&c.api))return 7;
 auto q=init<MgmEffectRequest>();q.request_id=77;q.epoch=1;q.left=frame;mgm_frame_retain(frame);
 std::puts("SUBMIT_ENTER");std::fflush(stdout);auto accepted=c.api.submit(c.effect,&q,complete,&c,retain,release);
 auto drained=c.api.drain(c.effect,5000000000ULL);auto closed=mgm_identity_effect_release(c.effect);
 std::printf("RESULT submit=%d cancel=%d retain_drain=%d retain_close=%d drain=%d close=%d retain=%d release=%d callbacks=%d completion=%d\n",accepted,c.cancel_status,c.drain_status,c.release_status,drained,closed,c.retained.load(),c.released.load(),c.completed.load(),c.completion_status);
 mgm_frame_release(frame);mgc_lease_release(lease);mgc_resource_release(resource);mgc_domain_release(domain);auto rc=mgc_runtime_close(runtime);mgc_runtime_release(runtime);
 return accepted==MGC_ACCEPTED&&c.cancel_status==MGC_SUCCESS&&c.drain_status==MGC_BUSY&&c.release_status==MGC_BUSY&&drained==MGC_SUCCESS&&closed==MGC_SUCCESS&&c.retained==1&&c.released==1&&c.completed==1&&c.completion_status==MGC_CANCELLED&&rc==MGC_SUCCESS?0:8;
}
