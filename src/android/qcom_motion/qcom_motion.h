#ifndef QCOM_MOTION_H
#define QCOM_MOTION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    QCOM_MOTION_WIDTH = 480,
    QCOM_MOTION_HEIGHT = 272,
    QCOM_MOTION_TILE = 8,
    QCOM_MOTION_GRID_WIDTH = QCOM_MOTION_WIDTH / QCOM_MOTION_TILE,
    QCOM_MOTION_GRID_HEIGHT = QCOM_MOTION_HEIGHT / QCOM_MOTION_TILE,
    QCOM_MOTION_CANDIDATE_COUNT = 4,
    QCOM_MOTION_CANDIDATE_LOCAL = 1,
    QCOM_MOTION_CANDIDATE_WIDE = 2
};

typedef struct QcomMotion QcomMotion;

typedef struct QcomMotionTiming {
    uint64_t preparation_ns;
    uint64_t queue_ns;
    uint64_t cpu_readback_ns;
    uint64_t gpu_upload_ns;
    uint64_t gpu_coarse_ns;
    uint64_t gpu_fine_ns;
    int gpu_observed;
    int rgba_expanded;
} QcomMotionTiming;

/* Create an independent Vulkan context. Metric: 0=SAD, 1=SSD. */
QcomMotion *qcom_motion_create(int metric);
void qcom_motion_destroy(QcomMotion *motion);

/* Input is tightly packed R8, rows ordered bottom-up as read by GLES.
 * The result is a grid of four floats, forward dx/dy then backward dx/dy,
 * in input pixels. Optional confidence is a separate grid of two floats.
 * A handle is used by one thread at a time. */
int qcom_motion_estimate(QcomMotion *motion, const uint8_t *previous, const uint8_t *current,
                         float *vectors, float *confidence, QcomMotionTiming *timing);

/* Legacy vectors/confidence plus [cell][direction][rank][4] native float32 records.
 * Direction 0 is previous->current, direction 1 is current->previous.
 * A record is {dx, dy, mean SAD or SSD in [0,1], valid 1 or 0}.
 * Rank 0 is the legacy winner. Ranks 1-3 are selected by increasing cost,
 * subject to Chebyshev separation of at least two input pixels.
 * The caller provides GRID_WIDTH * GRID_HEIGHT * 2 * CANDIDATE_COUNT * 4 floats.
 * Mode LOCAL uses only the legacy fine window. WIDE also tests a separated
 * coarse center's 3x3 neighborhood and zero when not already in a window. */
int qcom_motion_estimate_candidates(QcomMotion *motion, const uint8_t *previous, const uint8_t *current,
                                    float *vectors, float *confidence, float *candidates,
                                    QcomMotionTiming *timing, int candidate_mode);

#ifdef __cplusplus
}
#endif
#endif
