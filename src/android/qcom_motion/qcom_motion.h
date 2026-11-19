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
    QCOM_MOTION_CANDIDATE_WIDE = 2,
    /* Compact one-byte-per-cell static classification, 1 = static pixel. */
    QCOM_MOTION_STATIC_BYTES = QCOM_MOTION_GRID_WIDTH * QCOM_MOTION_GRID_HEIGHT,
    /* Forward then backward consistency visibility, one float pair per cell. */
    QCOM_MOTION_VISIBILITY_FLOATS = QCOM_MOTION_GRID_WIDTH * QCOM_MOTION_GRID_HEIGHT * 2
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
    /* Appended for the three-level prototype; earlier offsets are unchanged. */
    uint64_t gpu_mid_ns;
    uint64_t gpu_visibility_ns;
    /* Appended for the per-stage cost audit; earlier offsets are unchanged.
     * gpu_upload_ns keeps its original meaning - everything that prepares the
     * three input levels - so for the AHB path it equals gpu_acquire_ns +
     * gpu_unpack_ns + gpu_release_ns + gpu_copy_ns, while on the tightly packed
     * R8 path it equals gpu_copy_ns and the three AHB-only terms are zero.
     * gpu_total_ns spans the whole command buffer (first timestamp to last), and
     * gpu_barrier_ns is the residue not attributable to a named segment. */
    uint64_t gpu_acquire_ns;
    uint64_t gpu_unpack_ns;
    uint64_t gpu_release_ns;
    uint64_t gpu_copy_ns;
    uint64_t gpu_total_ns;
    uint64_t gpu_barrier_ns;
    /* Appended for the wait-strategy audit; earlier offsets are unchanged.
     * queue_ns is one opaque "submit -> fence retired" interval.  These four split
     * it, which is what distinguishes a slow GPU from a slow wakeup:
     *   wait_spin_ns / wait_block_ns - how the wait was satisfied.  spin is the
     *     host polling loop (vkGetFenceStatus), block is a blocking
     *     vkWaitForFences.  A frame that spent time in block and still retired
     *     late is a scheduling/wakeup problem, not a GPU problem.
     *   poll_count - vkGetFenceStatus calls in the spin phase.
     *   retire_to_submit_ns - CPU-side turnaround: from the previous estimate's
     *     retire to this one's submit, i.e. what the caller did in between. */
    uint64_t wait_spin_ns;
    uint64_t wait_block_ns;
    uint64_t poll_count;
    uint64_t retire_to_submit_ns;
    int wait_strategy;
    /* Spin budget the poll path actually used.  With the adaptive budget this
     * follows the previous frame's retire interval, so a caller can see whether
     * the spin covered the frame or fell through to the blocking wait. */
    uint64_t spin_budget_ns;
} QcomMotionTiming;

/* Create an independent Vulkan context. Metric: 0=SAD, 1=SSD. */
QcomMotion *qcom_motion_create(int metric);
void qcom_motion_destroy(QcomMotion *motion);

/* Input is tightly packed R8, rows ordered bottom-up as read by GLES.
 * The result is a grid of four floats, forward dx/dy then backward dx/dy,
 * in input pixels.  Coordinates are sub-pixel after the fine-level parabolic
 * refinement.  Optional confidence is a separate grid of two floats.
 * A handle is used by one thread at a time. */
int qcom_motion_estimate(QcomMotion *motion, const uint8_t *previous, const uint8_t *current,
                         float *vectors, float *confidence, QcomMotionTiming *timing);

/* Legacy vectors/confidence plus [cell][direction][rank][4] native float32 records.
 * Direction 0 is previous->current, direction 1 is current->previous.
 * A record is {dx, dy, mean SAD or SSD in [0,1], valid 1 or 0}.
 * Rank 0 is the legacy winner.  Ranks 1-3 are selected by increasing cost,
 * subject to Chebyshev separation of at least two input pixels.
 * The caller provides GRID_WIDTH * GRID_HEIGHT * 2 * CANDIDATE_COUNT * 4 floats.
 * Mode LOCAL uses only the legacy fine window.  WIDE also tests a separated
 * mid-level centre's 3x3 neighbourhood and zero when not already in a window. */
int qcom_motion_estimate_candidates(QcomMotion *motion, const uint8_t *previous, const uint8_t *current,
                                    float *vectors, float *confidence, float *candidates,
                                    QcomMotionTiming *timing, int candidate_mode);

/* Prototype entry point.  Behaves exactly like qcom_motion_estimate_candidates
 * with candidate_mode QCOM_MOTION_CANDIDATE_WIDE, and additionally reports the
 * static classification (QCOM_MOTION_STATIC_BYTES bytes, may be null) and the
 * bidirectional consistency visibility (QCOM_MOTION_VISIBILITY_FLOATS floats,
 * may be null).  candidates may be null when only the new outputs are wanted. */
int qcom_motion_estimate_extended(QcomMotion *motion, const uint8_t *previous, const uint8_t *current,
                                  float *vectors, float *confidence, float *candidates,
                                  uint8_t *static_mask, float *visibility,
                                  QcomMotionTiming *timing, int candidate_mode);

/* AHB counterpart of qcom_motion_estimate_extended.  Identical semantics, but the
 * two inputs are imported hardware buffers and the same optional static_mask /
 * visibility outputs are filled from the device buffers the AHB path already
 * keeps.  Both outputs may be null.  Defined in ahb_motion_api.inc and only
 * present in the AHB-capable library. */
struct QcomAhbFrame;
int qcom_motion_estimate_ahb_extended(QcomMotion *motion, QcomAhbFrame left, QcomAhbFrame right,
                                      float *vectors, float *confidence, float *candidates,
                                      uint8_t *static_mask, float *visibility,
                                      QcomMotionTiming *timing, int candidate_mode);

/* Enable or disable the fine-level parabolic sub-pixel refinement.  Enabled by
 * default; disabling it reproduces integer vectors for A/B measurement. */
void qcom_motion_set_subpixel(QcomMotion *motion, int enabled);

/* Static-classification thresholds in thousandths of a normalized luma code
 * value.  A cell is static when its own 8x8 temporal cost and the worst of its
 * four 2-pixel neighbour probes are both below the corresponding threshold.
 * Defaults are STATIC_THRESHOLD_MILLI / STATIC_NEIGHBOR_MILLI.  Returns 0 on
 * success and -1 when the values are rejected (non-positive, or a neighbour
 * threshold below the centre threshold). */
enum {
    QCOM_MOTION_STATIC_THRESHOLD_MILLI = 2,
    QCOM_MOTION_STATIC_NEIGHBOR_MILLI = 10
};
int qcom_motion_set_static_thresholds(QcomMotion *motion, int threshold_milli,
                                      int neighbor_milli);

/* Opt-in cost reducer.  Mode 1 is the default and selects a second pipeline
 * built from the same shader module with the parallel top-two reduction
 * specialization constant set, so its vectors, confidence, candidates, static
 * mask and visibility are byte-identical to the unreduced estimator.
 *
 * Mode 0 keeps a pipeline with the reducer compiled out - also byte-identical -
 * and is the explicit rollback via debug.meguviz.qcom.adaptive.mode=0 or this
 * setter.  The reduction replaces the 128-iteration serial fold on one lane with
 * an order-keyed tree fold.  The order key is (cost, weight, slot index), a
 * strict total order, so it reproduces
 * the serial left fold exactly - vectors, confidence, candidates (all four ranks),
 * static mask and visibility are byte-identical.  Worth ~2.4 ms of nativeEstimate
 * p50 on Adreno 740; 12 locked 4K pairs measured 0 differing pixels.
 *
 * Modes 2 and 3 are REJECTED with -1 by this build.  They would enable the
 * alternate-hypothesis gate, which device quality evidence rejected: 12 locked
 * 4K pairs showed fast_motion crossing 23.416 -> 23.316 dB, occlusion reveal
 * -0.091 dB and occlusion-13 -0.352 dB, for only ~0.14 ms on real content.  The
 * gate's specialization constant is also hard-wired false in both pipelines, so
 * it cannot be reached by any call path.
 *
 * Returns 0 on success, -1 on a null handle, a mode outside 0..1, or a cut outside
 * 0..1000.  confidence_gate_milli is accepted for signature compatibility with the
 * validated prototype and has no effect while the gate is absent.
 *
 * On Android, debug.meguviz.qcom.adaptive.mode is read at handle creation:
 * 0 selects mode 0 (explicit rollback), 1 selects mode 1 (also the default when
 * the property is absent or unparsable), and values above 1 are clamped to 1.
 * motion.comp still carries "default=false" on the reducer specialization
 * constants; that is the compile-time value of the unreduced pipeline variant
 * and is superseded at runtime by the host-side adaptiveMode=1 default.  The
 * shader bytes are unchanged.
 *
 * Returning to the pre-default-flip behaviour takes both switches:
 * adaptive.mode=0 (or qcom_motion_set_adaptive(..., 0, ...)) AND wait.strategy=0
 * (or qcom_motion_set_wait_strategy(..., QCOM_MOTION_WAIT_BLOCK)).  Setting only
 * the wait strategy disables the spin but leaves the reduction enabled. */
int qcom_motion_set_adaptive(QcomMotion *motion, int mode, int confidence_gate_milli);

/* Opt-in consumption of the static classification.  When enabled, every cell the
 * fine stage classified static has both motion vectors forced to zero, so the warp
 * samples both endpoint frames at the same coordinate and reproduces the
 * zero-displacement blend instead of a spurious sub-pixel shift inside stationary
 * content.  Disabled by default, which reproduces the pre-existing vectors exactly
 * and keeps staticMask a pure observation. */
void qcom_motion_set_static_gate(QcomMotion *motion, int enabled);

/* Wait strategy for the submit->retire fence.  The estimator is latency-bound and
 * the fence only ever covers one frame of its own work, so the retire interval is
 * dominated by how quickly the CPU notices a signalled fence rather than by GPU
 * execution.  Both strategies retire on the same fence, so vectors, confidence,
 * candidates, static mask and visibility are byte-identical; only the timing
 * differs.
 *
 *   QCOM_MOTION_WAIT_BLOCK  historical blocking vkWaitForFences.  Parks the
 *                           thread, so the retire instant includes a scheduler
 *                           wakeup.  Selected by
 *                           debug.meguviz.qcom.wait.strategy=0 (rollback).
 *   QCOM_MOTION_WAIT_POLL   polls vkGetFenceStatus on the calling thread within
 *                           a bounded budget, then falls back to the blocking
 *                           wait.  Default: an unconfigured handle spins with
 *                           the adaptive budget.  The fallback means a genuinely
 *                           late fence
 *                           still completes instead of timing out; the spin only
 *                           ever replaces a wakeup that would otherwise have
 *                           happened.
 *
 * The budget: qcom_motion_set_wait_spin_ns sets a fixed value (clamped to one
 * 60 Hz frame); passing 0 selects the adaptive budget, which follows the previous
 * frame's submit->retire interval plus a margin, clamped 2..20 ms.  The device
 * A/B picked the adaptive budget (QCOM_MOTION_WAIT_POLL with spin_ns 0).
 * qcom_motion_set_wait_strategy does NOT reset the budget mode: a handle created
 * with the new defaults keeps waitSpinNsAdaptive=true, so POLL still follows the
 * previous frame's interval rather than the fixed 4 ms.  Call
 * qcom_motion_set_wait_spin_ns with a non-zero value to pin the fixed budget.
 * On Android, debug.meguviz.qcom.wait.strategy selects 0/1/2 (block, fixed,
 * adaptive; absent = adaptive) at handle creation and
 * debug.meguviz.qcom.wait.stats=1 appends a
 * per-30-estimate window statistic to the app's private files directory. */
enum {
    QCOM_MOTION_WAIT_BLOCK = 0,
    QCOM_MOTION_WAIT_POLL = 1,
    QCOM_MOTION_WAIT_SPIN_NS_DEFAULT = 4000000
};
int qcom_motion_set_wait_strategy(QcomMotion *motion, int strategy);
int qcom_motion_set_wait_spin_ns(QcomMotion *motion, uint64_t spin_ns);

#ifdef __cplusplus
}
#endif
#endif
