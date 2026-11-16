#ifndef MEGUVIZ_MEDIA_SCHEDULER_H
#define MEGUVIZ_MEDIA_SCHEDULER_H
#include "media.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MgmAdmission MgmAdmission;
enum { MGM_EVENT_SOURCE_AVAILABLE=1,MGM_EVENT_SOURCE_LATCHED=2,MGM_EVENT_ACCEPTED=3,
    MGM_EVENT_GPU_SUBMITTED=4,MGM_EVENT_ALGORITHM_ACCEPTED=5,MGM_EVENT_SINK_ACCEPTED=6,
    MGM_EVENT_ACTUAL_PRESENTED=7,MGM_EVENT_EXECUTION_RETIRED=8,MGM_EVENT_OUTPUT_RELEASED=9,
    MGM_EVENT_INPUT_DROPPED=10,MGM_EVENT_OUTPUT_EXPIRED=11,MGM_EVENT_GPU_DEPENDENCY_READY=12,
    MGM_EVENT_OBSERVATION_INCOMPLETE=13 };
typedef struct MgmEvent {
    MgcHeader header;
    uint64_t session_id,epoch,source_id,content_id,task_id,allocation_id;
    int64_t timestamp_ns;
    uint32_t kind,time_domain;
    uint32_t required_flags,reserved;
    uint64_t allocation_generation,domain_id,domain_generation,gpu_batch_id;
    int32_t result_status;
    uint32_t frame_kind;
} MgmEvent;
typedef void (*MgmEventFn)(void *context,const MgmEvent *event);
typedef struct MgmAdmissionConfig {
    MgcHeader header;
    uint64_t cold_cost_ns,safety_margin_ns;
    uint32_t sample_window,percentile,max_in_flight,enabled;
    uint32_t required_flags,reserved[3];
} MgmAdmissionConfig;
typedef struct MgmAdmissionDecision {
    MgcHeader header;
    uint64_t predicted_joint_ns;
    uint32_t in_flight,sample_count;
    int32_t status;
    uint32_t reserved;
} MgmAdmissionDecision;
/* Samples cover the joint admitted→sink-ready interval. Failed or cancelled
 * samples are never recorded as successful execution cost. The bounded FIFO
 * contains at most 128 observations. enabled=0 disables only deadline rejection;
 * the shallow queue bound remains enforced. reserve/release are paired even on
 * cancellation, which releases its slot only after physical retirement. */
MGM_API MgcStatus mgm_admission_create(const MgmAdmissionConfig *,MgmAdmission **);
MGM_API void mgm_admission_release(MgmAdmission *);
MGM_API MgcStatus mgm_admission_reserve(MgmAdmission *,int64_t now_ns,int64_t deadline_ns,MgmAdmissionDecision *);
MGM_API MgcStatus mgm_admission_retire(MgmAdmission *,uint64_t joint_ns,uint32_t succeeded);
#ifdef __cplusplus
}
#endif
#endif
