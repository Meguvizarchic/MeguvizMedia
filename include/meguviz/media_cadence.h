#ifndef MEGUVIZ_MEDIA_CADENCE_H
#define MEGUVIZ_MEDIA_CADENCE_H
#include "media.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MgmCadence MgmCadence;
typedef struct MgmVsyncGrid MgmVsyncGrid;
typedef struct MgmVsyncMeter MgmVsyncMeter;
enum {MGM_CADENCE_PENDING=0,MGM_CADENCE_SUITABLE=1,MGM_CADENCE_UNSUITABLE=2};
typedef struct MgmCadenceConfig {
    MgcHeader header;
    uint32_t target_fps,min_samples,interpolation_required,required_flags;
    uint32_t reserved[4];
} MgmCadenceConfig;
typedef struct MgmCadenceSnapshot {
    MgcHeader header;
    uint64_t frames,gaps;
    int64_t median_us;
    double source_fps;
    uint32_t sample_count,verdict,has_fps,reserved;
} MgmCadenceSnapshot;
/* Pure bounded state, no GPU/model/clock acquisition. All timestamps are passed
 * by the caller. reset matches the original implementation: only last PTS is
 * invalidated; the 31-entry interval history and diagnostic counters survive. */
MGM_API MgcStatus mgm_cadence_create(const MgmCadenceConfig*,MgmCadence**);
MGM_API void mgm_cadence_release(MgmCadence*);
MGM_API MgcStatus mgm_cadence_add(MgmCadence*,int64_t pts_us);
MGM_API MgcStatus mgm_cadence_reset(MgmCadence*);
MGM_API MgcStatus mgm_cadence_snapshot(MgmCadence*,MgmCadenceSnapshot*);
typedef struct MgmVsyncConfig {
    MgcHeader header;
    int64_t content_period_ns;
    uint32_t required_flags,reserved[3];
} MgmVsyncConfig;
typedef struct MgmVsyncSnapshot {
    MgcHeader header;
    int64_t last_ns,period_ns,current_ns,until_next_ns;
    uint32_t stale,releasable,same_refresh,faster_than_content,slower_than_content,sample_count;
    uint32_t reserved[2];
} MgmVsyncSnapshot;
/* Fixed 32-entry lower-quartile display-period estimator; 2ms extrapolation
 * grace and four content-period stale threshold preserve the original policy.
 * Impossible arithmetic ranges reject instead of overflowing signed integers. */
MGM_API MgcStatus mgm_vsync_grid_create(const MgmVsyncConfig*,MgmVsyncGrid**);
MGM_API void mgm_vsync_grid_release(MgmVsyncGrid*);
MGM_API MgcStatus mgm_vsync_grid_on_vsync(MgmVsyncGrid*,int64_t vsync_ns,uint32_t*accepted);
MGM_API MgcStatus mgm_vsync_grid_snapshot(MgmVsyncGrid*,int64_t due_ns,int64_t now_ns,MgmVsyncSnapshot*);
typedef struct MgmVsyncMeterConfig {
    MgcHeader header;
    uint32_t warmup,samples,required_flags,reserved;
} MgmVsyncMeterConfig;
/* Reports the upper-median refresh rate exactly once after warmup. samples is
 * bounded to 1..4096; the default Java facade uses the original 60/120 profile. */
MGM_API MgcStatus mgm_vsync_meter_create(const MgmVsyncMeterConfig*,MgmVsyncMeter**);
MGM_API void mgm_vsync_meter_release(MgmVsyncMeter*);
MGM_API MgcStatus mgm_vsync_meter_add(MgmVsyncMeter*,int64_t frame_ns,uint32_t*reported,float*hz);
#ifdef __cplusplus
}
#endif
#endif
