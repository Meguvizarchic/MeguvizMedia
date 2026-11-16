#include <meguviz/media_rife.h>
#include "descriptor_pages.h"
#include <cstring>
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "mxcsr_daz.h"
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<" "<<#x<<std::endl;std::exit(1);}}while(0)
static std::vector<uint8_t> read(const std::string &path){std::ifstream f(path,std::ios::binary);CHECK(f.good());return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
static MgcHash hash(const char *s){MgcHash h{};for(int i=0;i<32;++i){auto v=[](char c){return c<='9'?c-'0':c-'a'+10;};h.bytes[i]=uint8_t(v(s[i*2])*16+v(s[i*2+1]));}return h;}
struct Context {int callbacks=0,refs=0;MgmFrame *output=nullptr;};
static void retain(void *p){++static_cast<Context*>(p)->refs;}
static void release(void *p){--static_cast<Context*>(p)->refs;}
static void done(void *p,uint64_t id,MgcStatus s,MgmFrame *output){auto c=static_cast<Context*>(p);if(s!=MGC_SUCCESS||!output||c->refs!=1)std::cerr<<"callback request="<<id<<" status="<<s<<" output="<<output<<" context_refs="<<c->refs<<std::endl;CHECK(s==MGC_SUCCESS&&output&&c->refs==1);c->output=output;++c->callbacks;}
static void large_pts_contract(MgmEffect *effect,const MgmEffectApi &api,MgmFrame *left,MgmFrame *right){
    struct Case {int64_t left,right;double alpha;int64_t expected;};
    const Case cases[]={
        {INT64_C(9007199254740993),INT64_C(9007199254740994),0.0,INT64_C(9007199254740993)},
        {INT64_C(9007199254740993),INT64_C(9007199254740994),0.5,INT64_C(9007199254740994)},
        {INT64_C(9007199254740994),INT64_C(9007199254740993),0.5,INT64_C(9007199254740994)},
        {INT64_MAX-1,INT64_MAX,1.0,INT64_MAX},
        {INT64_MAX,0,0.0,INT64_MAX}};
    for(const auto &item:cases){
        MgmFrameDesc a{},b{};a.header=MGC_HEADER_INIT(MgmFrameDesc);b.header=MGC_HEADER_INIT(MgmFrameDesc);
        CHECK(mgm_frame_describe(left,&a)==MGC_SUCCESS&&mgm_frame_describe(right,&b)==MGC_SUCCESS);
        a.pts_ticks=item.left;b.pts_ticks=item.right;MgmFrame *l=nullptr,*r=nullptr;
        CHECK(mgm_frame_create(&a,&l)==MGC_SUCCESS&&mgm_frame_create(&b,&r)==MGC_SUCCESS);
        Context context;MgmEffectRequest request{};request.header=MGC_HEADER_INIT(MgmEffectRequest);
        request.request_id=138;request.epoch=9;request.left=l;request.right=r;request.alpha=item.alpha;
        mgm_frame_retain(l);mgm_frame_retain(r);
        CHECK(api.submit(effect,&request,done,&context,retain,release)==MGC_ACCEPTED);
        CHECK(api.drain(effect,30000000000ULL)==MGC_SUCCESS&&context.callbacks==1&&context.refs==0);
        MgmFrameDesc out{};out.header=MGC_HEADER_INIT(MgmFrameDesc);
        CHECK(mgm_frame_describe(context.output,&out)==MGC_SUCCESS&&out.pts_ticks==item.expected);
        mgm_frame_release(context.output);mgm_frame_release(l);mgm_frame_release(r);
    }
}
#if defined(MGM_TEST_X86_DAZ)
static void alpha_daz_contract(MgmEffect *effect,const MgmEffectApi &api,MgmFrame *left,MgmFrame *right){
    if(!mgm_test_has_daz_ftz()){std::cerr<<"DAZ/FTZ unavailable; adapter FP mode test skipped\n";return;}
    struct Case{uint64_t bits;bool accepted;uint32_t kind;};
    const Case cases[]={
        {UINT64_C(0x8000000000000001),false,0},
        {UINT64_C(0x0000000000000001),true,MGM_FRAME_INFERRED},
        {UINT64_C(0x8000000000000000),true,MGM_FRAME_ENDPOINT},
        {UINT64_C(0x3ff0000000000000),true,MGM_FRAME_ENDPOINT},
        {UINT64_C(0x3fefffffffffffff),true,MGM_FRAME_INFERRED},
        {UINT64_C(0x7ff8000000000001),false,0},
        {UINT64_C(0x7ff0000000000000),false,0},
        {UINT64_C(0x3ff0000000000001),false,0}};
    for(const auto &item:cases){
        Context context;MgmEffectRequest request{};request.header=MGC_HEADER_INIT(MgmEffectRequest);
        request.request_id=137;request.epoch=9;request.left=left;request.right=right;
        std::memcpy(&request.alpha,&item.bits,sizeof(item.bits));mgm_frame_retain(left);mgm_frame_retain(right);
        const unsigned previous=_mm_getcsr();_mm_setcsr(previous|0x8040u);
        const auto status=api.submit(effect,&request,done,&context,retain,release);_mm_setcsr(previous);
        if(!item.accepted){mgm_frame_release(left);mgm_frame_release(right);CHECK(status==MGC_INVALID_ARGUMENT&&context.refs==0&&context.callbacks==0);}
        else {CHECK(status==MGC_ACCEPTED);CHECK(api.drain(effect,30000000000ULL)==MGC_SUCCESS&&context.refs==0&&context.callbacks==1&&context.output);
            MgmFrameDesc output{};output.header=MGC_HEADER_INIT(MgmFrameDesc);CHECK(mgm_frame_describe(context.output,&output)==MGC_SUCCESS&&output.frame_kind==item.kind);
            mgm_frame_release(context.output);}
    }
}
#endif
static void partial_requests_rejected(MgmEffect *effect,const MgmEffectApi &api,const MgmEffectRequest &request){
    const std::pair<size_t,size_t> fields[]={
        {offsetof(MgmEffectRequest,reserved),sizeof(uint32_t)},
        {offsetof(MgmEffectRequest,reserved)+sizeof(uint32_t),sizeof(uint32_t)},
        {offsetof(MgmEffectRequest,reserved)+2*sizeof(uint32_t),sizeof(uint32_t)},
        {offsetof(MgmEffectRequest,algorithm_scale),sizeof(request.algorithm_scale)},
        {offsetof(MgmEffectRequest,output_sync_mode),sizeof(request.output_sync_mode)},
        {offsetof(MgmEffectRequest,batch_event),sizeof(request.batch_event)}};
    Context context;
    for(auto field:fields)for(size_t cut=1;cut<field.second;++cut){
        DescriptorPages partial(field.first+cut);partial.store(request);
        CHECK(api.submit(effect,partial.as<MgmEffectRequest>(),done,&context,retain,release)==MGC_INVALID_ARGUMENT);
        CHECK(context.refs==0&&context.callbacks==0);
    }
}
static MgmFrame *held_observation_frame=nullptr;
static void check_observation(MgmFrame *frame,uint64_t id,uint32_t kind){
    MgrJobObservation observed{};observed.header=MGC_HEADER_INIT(MgrJobObservation);
    CHECK(mgm_rife_frame_observation(nullptr,&observed)==MGC_INVALID_ARGUMENT);
    CHECK(mgm_rife_frame_observation(frame,nullptr)==MGC_INVALID_ARGUMENT);
    auto too_short=observed;too_short.header.struct_size=uint32_t(offsetof(MgrJobObservation,reserved)-1);
    CHECK(mgm_rife_frame_observation(frame,&too_short)==MGC_UNSUPPORTED_ABI);
    auto bad_major=observed;bad_major.header.abi_major=MGR_ABI_MAJOR+1;
    CHECK(mgm_rife_frame_observation(frame,&bad_major)==MGC_UNSUPPORTED_ABI);
    DescriptorPages short_page(sizeof(MgcHeader));
    CHECK(mgm_rife_frame_observation(frame,short_page.as<MgrJobObservation>())==MGC_UNSUPPORTED_ABI);
    DescriptorPages prefix_page(offsetof(MgrJobObservation,reserved));
#if defined(MGR_ENABLE_JOB_OBSERVATION)
    CHECK(mgm_rife_frame_observation(frame,&observed)==MGC_SUCCESS);
    CHECK(observed.request_id==id&&observed.frame_kind==kind);
    CHECK(observed.tensor_cache_left==MGR_CACHE_OBSERVATION_NOT_APPLICABLE&&observed.tensor_cache_right==MGR_CACHE_OBSERVATION_NOT_APPLICABLE);
    CHECK(observed.static_guard_dispatch==MGR_GUARD_OBSERVATION_NOT_APPLICABLE&&observed.motion_guard_dispatch==MGR_GUARD_OBSERVATION_NOT_APPLICABLE);
    MgrJobObservation prefix{};prefix.header=MGC_HEADER_INIT(MgrJobObservation);prefix.header.struct_size=uint32_t(offsetof(MgrJobObservation,reserved));
    CHECK(mgm_rife_frame_observation(frame,&prefix)==MGC_SUCCESS&&prefix.request_id==id);
    CHECK(mgm_rife_frame_observation(frame,prefix_page.as<MgrJobObservation>())==MGC_SUCCESS);
    CHECK(prefix_page.read<MgrJobObservation>().request_id==id);
#else
    (void)id;(void)kind;
    CHECK(mgm_rife_frame_observation(frame,&observed)==MGC_UNSUPPORTED_CAPABILITY);
    CHECK(mgm_rife_frame_observation(frame,prefix_page.as<MgrJobObservation>())==MGC_UNSUPPORTED_CAPABILITY);
#endif
}
static void check_parallel_observation(MgmFrame *frame){
    std::atomic<bool> valid{true};std::vector<std::thread> readers;
    for(int i=0;i<4;++i)readers.emplace_back([&]{
        for(int j=0;j<1000;++j){MgrJobObservation out{};out.header=MGC_HEADER_INIT(MgrJobObservation);
            const auto status=mgm_rife_frame_observation(frame,&out);
#if defined(MGR_ENABLE_JOB_OBSERVATION)
            if(status!=MGC_SUCCESS||out.request_id!=1||out.frame_kind!=MGR_RESULT_ENDPOINT)valid=false;
#else
            if(status!=MGC_UNSUPPORTED_CAPABILITY)valid=false;
#endif
        }
    });
    for(auto &reader:readers)reader.join();CHECK(valid.load());
}
int main(int argc,char **argv){CHECK(argc==3);auto graph=read(std::string(argv[1])+"/flownet.param"),weights=read(std::string(argv[1])+"/flownet.bin"),manifest=read(argv[2]);MgcRuntimeDesc rd{};rd.header=MGC_HEADER_INIT(MgcRuntimeDesc);rd.provider=MGC_PROVIDER_NCNN_CPU;rd.memory_budget_bytes=64*1024*1024;MgcRuntime *runtime=nullptr;CHECK(mgc_runtime_create(&rd,&runtime)==MGC_SUCCESS);MgcDomainDesc dd{};dd.header=MGC_HEADER_INIT(MgcDomainDesc);MgcDomain *domain=nullptr;CHECK(mgc_domain_acquire(runtime,&dd,&domain)==MGC_SUCCESS);MgcBuildInfo info{};info.header=MGC_HEADER_INIT(MgcBuildInfo);CHECK(mgc_query_build_info(&info)==MGC_SUCCESS);MgrModelDesc md{};md.header=MGC_HEADER_INIT(MgrModelDesc);md.manifest_utf8=manifest.data();md.manifest_size=manifest.size();md.graph_data=graph.data();md.graph_size=graph.size();md.weights_data=weights.data();md.weights_size=weights.size();md.graph_sha256=hash("724569596bcd1e7b9fa50455c604777ebed99746d2ef40aa86e31b5725f1053c");md.weights_sha256=hash("f334ed2260149ce0188a6dcf049844e8b0cdd912e01cbcfb63553157d2508958");md.private_sdk_fingerprint=info.private_sdk_fingerprint;MgrModel *model=nullptr;CHECK(mgr_model_create(domain,&md,&model)==MGC_SUCCESS);MgrPlanDesc pd{};pd.header=MGC_HEADER_INIT(MgrPlanDesc);pd.width=32;pd.height=32;pd.input_format=MGC_FORMAT_RGB8;pd.precision_profile=MGR_PRECISION_FP32_REFERENCE;pd.flow_scale=1;MgrPlan *plan=nullptr;CHECK(mgr_plan_create(model,&pd,&plan)==MGC_SUCCESS);
    MgmFrame *frames[2]{};std::vector<uint8_t> first(32*32*3);for(uint32_t side=0;side<2;++side){MgcResourceDesc resource_desc{};resource_desc.header=MGC_HEADER_INIT(MgcResourceDesc);resource_desc.allocation_id=side+1;resource_desc.allocation_generation=1;resource_desc.capacity_bytes=first.size();resource_desc.width=32;resource_desc.height=32;resource_desc.row_stride_bytes=96;resource_desc.format=MGC_FORMAT_RGB8;MgcResource *resource=nullptr;CHECK(mgc_resource_create(domain,&resource_desc,&resource)==MGC_SUCCESS);MgcLeaseDesc ld{};ld.header=MGC_HEADER_INIT(MgcLeaseDesc);ld.content_id=1;ld.byte_length=first.size();ld.access=MGC_ACCESS_WRITE;MgcLease *lease=nullptr;CHECK(mgc_lease_create(resource,&ld,&lease)==MGC_SUCCESS);void *bytes=nullptr;uint64_t size=0;CHECK(mgc_lease_map(lease,&bytes,&size)==MGC_SUCCESS);for(size_t i=0;i<first.size();++i)static_cast<uint8_t*>(bytes)[i]=uint8_t((i+side*13)%256);if(!side)std::memcpy(first.data(),bytes,first.size());CHECK(mgc_lease_unmap(lease)==MGC_SUCCESS);mgc_lease_release(lease);ld.access=MGC_ACCESS_READ;CHECK(mgc_lease_create(resource,&ld,&lease)==MGC_SUCCESS);MgmFrameDesc frame{};frame.header=MGC_HEADER_INIT(MgmFrameDesc);frame.content=lease;frame.epoch=9;frame.pts_ticks=side*40;frame.timebase={1,1000};frame.crop_width=32;frame.crop_height=32;frame.frame_kind=MGM_FRAME_ORIGINAL;frame.color.header=MGC_HEADER_INIT(MgmColor);frame.color.bit_depth=8;frame.color.transfer=MGM_TRANSFER_SDR;frame.color.range=MGM_RANGE_FULL;CHECK(mgm_frame_create(&frame,&frames[side])==MGC_SUCCESS);mgc_lease_release(lease);mgc_resource_release(resource);}
    MgmEffect *effect=nullptr;MgmEffectApi api{};api.header=MGC_HEADER_INIT(MgmEffectApi);
    DescriptorPages short_value;CHECK(mgm_rife_effect_create(plan,&effect,short_value.as<MgmEffectApi>())==MGC_INVALID_ARGUMENT&&!effect);
    MgmEffectApi invalid_api=api;invalid_api.required_flags=1;CHECK(mgm_rife_effect_create(plan,&effect,&invalid_api)==MGC_UNSUPPORTED_ABI&&!effect);invalid_api.required_flags=0;invalid_api.reserved[0]=1;CHECK(mgm_rife_effect_create(plan,&effect,&invalid_api)==MGC_INVALID_ARGUMENT&&!effect);
    DescriptorPages api_prefix(offsetof(MgmEffectApi,reserved));api_prefix.store(api);CHECK(mgm_rife_effect_create(plan,&effect,api_prefix.as<MgmEffectApi>())==MGC_SUCCESS);api=api_prefix.read<MgmEffectApi>();
    large_pts_contract(effect,api,frames[0],frames[1]);
    CHECK(api.prepare(effect,short_value.as<MgmFrameDesc>())==MGC_INVALID_ARGUMENT);Context rejected_context;CHECK(api.submit(effect,short_value.as<MgmEffectRequest>(),done,&rejected_context,retain,release)==MGC_INVALID_ARGUMENT&&rejected_context.callbacks==0&&rejected_context.refs==0);
    DescriptorPages capability_prefix(offsetof(MgcCapability,reserved));CHECK(api.query(effect,capability_prefix.as<MgcCapability>())==MGC_SUCCESS);MgcCapability bad_cap{};bad_cap.header=MGC_HEADER_INIT(MgcCapability);bad_cap.header.abi_major=1;capability_prefix.store(bad_cap);CHECK(api.query(effect,capability_prefix.as<MgcCapability>())==MGC_UNSUPPORTED_ABI);
    MgmFrameDesc prepare_frame{};prepare_frame.header=MGC_HEADER_INIT(MgmFrameDesc);CHECK(mgm_frame_describe(frames[0],&prepare_frame)==MGC_SUCCESS);DescriptorPages prepare_prefix(offsetof(MgmFrameDesc,reserved));prepare_prefix.store(prepare_frame);CHECK(api.prepare(effect,prepare_prefix.as<MgmFrameDesc>())==MGC_SUCCESS);prepare_frame.required_flags=1;prepare_prefix.store(prepare_frame);CHECK(api.prepare(effect,prepare_prefix.as<MgmFrameDesc>())==MGC_UNSUPPORTED_ABI);
    MgrJobObservation original{};original.header=MGC_HEADER_INIT(MgrJobObservation);CHECK(mgm_rife_frame_observation(frames[0],&original)==MGC_UNSUPPORTED_CAPABILITY);
#if defined(MGM_TEST_X86_DAZ)
    alpha_daz_contract(effect,api,frames[0],frames[1]);
#endif
    {Context full;MgmEffectRequest request{};request.header=MGC_HEADER_INIT(MgmEffectRequest);request.request_id=99;request.epoch=9;request.left=frames[0];request.right=frames[1];request.alpha=0;
     mgm_frame_retain(frames[0]);mgm_frame_retain(frames[1]);CHECK(api.submit(effect,&request,done,&full,retain,release)==MGC_ACCEPTED);
     CHECK(api.drain(effect,30000000000ULL)==MGC_SUCCESS&&full.callbacks==1&&full.refs==0&&full.output);mgm_frame_release(full.output);
     Context old;DescriptorPages legacy(offsetof(MgmEffectRequest,algorithm_scale));legacy.store(request);
     mgm_frame_retain(frames[0]);mgm_frame_retain(frames[1]);CHECK(api.submit(effect,legacy.as<MgmEffectRequest>(),done,&old,retain,release)==MGC_ACCEPTED);
     CHECK(api.drain(effect,30000000000ULL)==MGC_SUCCESS&&old.callbacks==1&&old.refs==0&&old.output);mgm_frame_release(old.output);}
    for(double alpha:{0.0,0.5}){Context context;MgmEffectRequest request{};request.header=MGC_HEADER_INIT(MgmEffectRequest);request.request_id=alpha==0?1:2;request.epoch=9;request.left=frames[0];request.right=frames[1];request.alpha=alpha;partial_requests_rejected(effect,api,request);DescriptorPages request_prefix(offsetof(MgmEffectRequest,reserved));request_prefix.store(request);request.required_flags=1;CHECK(api.submit(effect,&request,done,&context,retain,release)==MGC_UNSUPPORTED_ABI&&context.refs==0);request.required_flags=0;request.reserved[0]=1;CHECK(api.submit(effect,&request,done,&context,retain,release)==MGC_INVALID_ARGUMENT&&context.refs==0);mgm_frame_retain(frames[0]);mgm_frame_retain(frames[1]);CHECK(api.submit(effect,request_prefix.as<MgmEffectRequest>(),done,&context,retain,release)==MGC_ACCEPTED);CHECK(api.drain(effect,30000000000ULL)==MGC_SUCCESS);CHECK(context.callbacks==1&&context.refs==0);MgmFrameDesc output{};output.header=MGC_HEADER_INIT(MgmFrameDesc);CHECK(mgm_frame_describe(context.output,&output)==MGC_SUCCESS);check_observation(context.output,request.request_id,alpha==0?MGR_RESULT_ENDPOINT:MGR_RESULT_INFERRED);if(!held_observation_frame){held_observation_frame=context.output;mgm_frame_retain(held_observation_frame);}else {check_observation(held_observation_frame,1,MGR_RESULT_ENDPOINT);check_parallel_observation(held_observation_frame);}CHECK(output.pts_ticks==(alpha==0?0:20));CHECK(output.frame_kind==(alpha==0?MGM_FRAME_ENDPOINT:MGM_FRAME_INFERRED));if(alpha==0){void *bytes;uint64_t size;CHECK(mgc_lease_map(output.content,&bytes,&size)==MGC_SUCCESS);CHECK(size==first.size()&&std::memcmp(bytes,first.data(),first.size())==0);CHECK(mgc_lease_unmap(output.content)==MGC_SUCCESS);}mgm_frame_release(context.output);}
    CHECK(mgm_rife_effect_release(effect)==MGC_SUCCESS);check_observation(held_observation_frame,1,MGR_RESULT_ENDPOINT);mgm_frame_release(held_observation_frame);for(auto frame:frames)mgm_frame_release(frame);mgr_plan_release(plan);mgr_model_release(model);mgc_domain_release(domain);CHECK(mgc_runtime_close(runtime)==MGC_SUCCESS);mgc_runtime_release(runtime);std::cout<<"{\"result\":\"PASS\",\"scope\":\"cpu_fp32_full_graph_media_adapter\",\"endpoint_bitwise\":true,\"inferred_alpha\":0.5,\"gpu_qualified\":false,\"bil_qualified\":false}"<<std::endl;}
