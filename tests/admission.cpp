#include <meguviz/media_scheduler.h>
#define REQUIRE(expression) do { if(!(expression)) return __LINE__; } while(false)
int main(){
    MgmAdmissionConfig c{};c.header=MGC_HEADER_INIT(MgmAdmissionConfig);c.cold_cost_ns=30;c.safety_margin_ns=2;c.sample_window=4;c.percentile=95;c.max_in_flight=1;c.enabled=1;
    MgmAdmission *p=nullptr;REQUIRE(mgm_admission_create(&c,&p)==MGC_SUCCESS);
    MgmAdmissionDecision d{};d.header=MGC_HEADER_INIT(MgmAdmissionDecision);
    REQUIRE(mgm_admission_reserve(p,100,131,&d)==MGC_DEADLINE_REJECTED);
    REQUIRE(mgm_admission_reserve(p,100,132,&d)==MGC_ACCEPTED);
    REQUIRE(mgm_admission_reserve(p,100,200,&d)==MGC_BACKPRESSURE);
    // A cancelled/failed 1ns operation cannot poison the successful distribution.
    REQUIRE(mgm_admission_retire(p,1,0)==MGC_SUCCESS);
    REQUIRE(mgm_admission_reserve(p,100,131,&d)==MGC_DEADLINE_REJECTED);
    REQUIRE(mgm_admission_reserve(p,100,200,&d)==MGC_ACCEPTED);
    REQUIRE(mgm_admission_retire(p,45,1)==MGC_SUCCESS);
    REQUIRE(mgm_admission_reserve(p,100,146,&d)==MGC_DEADLINE_REJECTED);
    REQUIRE(mgm_admission_reserve(p,100,147,&d)==MGC_ACCEPTED);
    REQUIRE(mgm_admission_retire(p,44,1)==MGC_SUCCESS);
    REQUIRE(mgm_admission_retire(p,1,1)==MGC_INVALID_ARGUMENT);
    mgm_admission_release(p);
    return 0;
}
