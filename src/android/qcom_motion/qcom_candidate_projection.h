#ifndef QCOM_CANDIDATE_PROJECTION_H
#define QCOM_CANDIDATE_PROJECTION_H

#include "qcom_motion.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    QCOM_ATLAS_OUTPUT_COUNT = 2,
    QCOM_ATLAS_RAW_FLOATS = QCOM_MOTION_GRID_WIDTH * QCOM_MOTION_GRID_HEIGHT * 2 * QCOM_MOTION_CANDIDATE_COUNT * 4,
    QCOM_ATLAS_FLOATS = QCOM_MOTION_GRID_WIDTH * QCOM_MOTION_GRID_HEIGHT * 2 * QCOM_ATLAS_OUTPUT_COUNT * 4,
};

/* Input cell/direction/rank float4; output direction/rank/cell float4.
 * Input and output have the sizes above and must not overlap. */
int qcom_prepare_candidates(const float *raw, float *atlas);

#ifdef __cplusplus
}
#endif
#endif
