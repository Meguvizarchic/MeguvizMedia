#include <meguviz/timeline.h>
#include <cstdlib>
#include <iostream>
#include <limits>
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<" "<<#x<<std::endl;std::exit(1);}}while(0)
static void clock_result_reuse(){
    MgmPlannerConfig c{};c.header=MGC_HEADER_INIT(MgmPlannerConfig);c.target_fps=60;c.frame_window=4;c.max_clock_age_ns=250000000;c.max_clock_uncertainty_us=2000;
    MgmPlanner *p=nullptr;CHECK(mgm_planner_create(&c,&p)==MGC_SUCCESS);
    MgmEpoch epoch{1,1,1,1,1};MgmOfferResult result{};result.header=MGC_HEADER_INIT(MgmOfferResult);uint64_t generation=0;
    CHECK(mgm_planner_begin(p,&epoch,&generation,&result)==MGC_SUCCESS);
    CHECK(mgm_planner_offer(p,generation,10,1,&result)==MGC_SUCCESS&&result.accepted);
    MgmClockSnapshot clock{};clock.header=MGC_HEADER_INIT(MgmClockSnapshot);clock.speed=1;clock.discontinuity=1;clock.monotonic_ns=1;
    uint32_t state=0;CHECK(mgm_planner_clock(p,generation,&clock,&state,&result)==MGC_SUCCESS&&state==MGM_CLOCK_READY&&!result.accepted&&!result.discarded_count);
    result.accepted=1;clock.monotonic_ns=0;
    CHECK(mgm_planner_clock(p,generation,&clock,&state,&result)==MGC_SUCCESS&&state==MGM_CLOCK_STALE_SNAPSHOT&&!result.accepted&&!result.discarded_count);
    result.accepted=1;result.discarded_count=2;state=99;
    CHECK(mgm_planner_clock(p,0,&clock,&state,&result)==MGC_CANCELLED&&state==99&&!result.accepted&&!result.discarded_count);
    result.accepted=1;
    CHECK(mgm_planner_clock(p,generation,nullptr,&state,&result)==MGC_SUCCESS&&state==MGM_CLOCK_RESTART_REQUIRED&&!result.accepted&&result.discarded_count==1&&result.discarded_ids[0]==10);
    CHECK(mgm_planner_begin(p,&epoch,&generation,&result)==MGC_SUCCESS);
    CHECK(mgm_planner_offer(p,generation,20,2,&result)==MGC_SUCCESS&&result.accepted);
    clock.paused=1;CHECK(mgm_planner_clock(p,generation,&clock,&state,&result)==MGC_SUCCESS&&state==MGM_CLOCK_PAUSED_STATE&&!result.accepted&&!result.discarded_count);
    result.accepted=1;clock.paused=0;
    CHECK(mgm_planner_clock(p,generation,&clock,&state,&result)==MGC_SUCCESS&&state==MGM_CLOCK_RESTART_REQUIRED&&!result.accepted&&result.discarded_count==1&&result.discarded_ids[0]==20);
    result.accepted=1;result.discarded_count=2;clock.required_flags=1;
    CHECK(mgm_planner_clock(p,generation,&clock,&state,&result)==MGC_UNSUPPORTED_ABI&&result.accepted==1&&result.discarded_count==2);
    mgm_planner_release(p);
}
int main(){clock_result_reuse();MgmPlannerConfig c{};c.header=MGC_HEADER_INIT(MgmPlannerConfig);c.target_fps=60;c.frame_window=4;c.max_clock_age_ns=250000000;c.max_clock_uncertainty_us=2000;MgmPlanner *p=nullptr;CHECK(mgm_planner_create(&c,&p)==MGC_SUCCESS);MgmEpoch epoch{1,1,1,1,1};MgmOfferResult o{};o.header=MGC_HEADER_INIT(MgmOfferResult);uint64_t generation;CHECK(mgm_planner_begin(p,&epoch,&generation,&o)==MGC_SUCCESS);MgmClockSnapshot clock{};clock.header=MGC_HEADER_INIT(MgmClockSnapshot);clock.speed=1;clock.discontinuity=1;uint32_t state;CHECK(mgm_planner_clock(p,generation,&clock,&state,&o)==MGC_SUCCESS&&state==MGM_CLOCK_READY);CHECK(mgm_planner_offer(p,generation,1,0,&o)==MGC_SUCCESS&&o.accepted);CHECK(mgm_planner_offer(p,generation,2,33333,&o)==MGC_SUCCESS&&o.accepted);MgmProposal proposal{};proposal.header=MGC_HEADER_INIT(MgmProposal);CHECK(mgm_planner_plan(p,generation,0,&proposal)==MGC_SUCCESS&&proposal.mode==MGM_FRAME_ORIGINAL);CHECK(mgm_planner_commit(p,&proposal,0)==MGC_SUCCESS);CHECK(mgm_planner_plan(p,generation,16666666,&proposal)==MGC_SUCCESS&&proposal.mode==MGM_FRAME_INFERRED);CHECK(proposal.before_id==1&&proposal.after_id==2&&proposal.media_us==16667);auto old=proposal;CHECK(mgm_planner_begin(p,&epoch,&generation,&o)==MGC_SUCCESS&&o.discarded_count==2);CHECK(mgm_planner_valid(p,&old,16666666)==MGC_CANCELLED);
    clock.monotonic_ns=20000000;CHECK(mgm_planner_clock(p,generation,&clock,&state,&o)==MGC_SUCCESS);clock.monotonic_ns=10000000;CHECK(mgm_planner_clock(p,generation,&clock,&state,&o)==MGC_SUCCESS&&state==MGM_CLOCK_STALE_SNAPSHOT);clock.monotonic_ns=30000000;clock.speed=2;CHECK(mgm_planner_clock(p,generation,&clock,&state,&o)==MGC_SUCCESS&&state==MGM_CLOCK_RESTART_REQUIRED);CHECK(mgm_planner_plan(p,generation,30000000,&proposal)==MGC_SUCCESS&&proposal.waiting_reason==MGM_WAIT_STALE_LEASE);mgm_planner_release(p);
    int64_t ns;CHECK(mgm_ticks_to_ns(45000,{1,90000},&ns)==MGC_SUCCESS&&ns==500000000);
    CHECK(mgm_ticks_to_ns(10,{1000000000,1000000001},&ns)==MGC_SUCCESS&&ns==9999999990LL);
    CHECK(mgm_ticks_to_ns(2147483646,{2147483647,2147483647},&ns)==MGC_SUCCESS&&ns==2147483646000000000LL);
    CHECK(mgm_ticks_to_ns(9223372036LL,{1,1},&ns)==MGC_SUCCESS&&ns==9223372036000000000LL);
    CHECK(mgm_ticks_to_ns(INT64_MAX,{1,1000000000},&ns)==MGC_SUCCESS&&ns==INT64_MAX);
    ns=123;CHECK(mgm_ticks_to_ns(9223372037LL,{1,1},&ns)==MGC_INVALID_ARGUMENT&&ns==123);
    CHECK(mgm_ticks_to_ns(INT64_MAX,{1,1},&ns)==MGC_INVALID_ARGUMENT);CHECK(mgm_ticks_to_ns(-1,{1,1},&ns)==MGC_INVALID_ARGUMENT);CHECK(mgm_ticks_to_ns(1,{1,0},&ns)==MGC_INVALID_ARGUMENT);std::cout<<"{\"result\":\"PASS\",\"epoch_stale_snapshot_speed_overflow\":true}"<<std::endl;}
