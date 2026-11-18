#pragma once

#include <android/hardware_buffer.h>

struct QcomAhbFrame {
    unsigned slot;
    int acquireFd;  // Borrowed (the caller may reuse it for the adjacent frame pair).
};

extern "C" {
QcomMotion *qcom_motion_create_ahb(int metric);
int qcom_motion_bind_luma_ahb(QcomMotion *motion, unsigned slot, AHardwareBuffer *buffer);
/* Success implies GPU release of both imports. A nonzero return must be
 * paired with a retirement check before either source allocation is reused. */
int qcom_motion_estimate_ahb(QcomMotion *motion, QcomAhbFrame left, QcomAhbFrame right,
                            float *vectors, float *confidence, float *candidates,
                            QcomMotionTiming *timing, int candidateMode);
/* 1 = safe retirement, 0 = pending or uncertain, negative = invalid handle.
 * Device loss and failed submit are conservatively retained. */
int qcom_motion_ahb_retired(QcomMotion *motion);
int qcom_motion_ahb_test_pyramid(QcomMotion *motion, const uint8_t *previous, const uint8_t *current);
}

