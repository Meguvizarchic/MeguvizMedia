#ifndef MEGUVIZ_MEDIA_RIFE_H
#define MEGUVIZ_MEDIA_RIFE_H
#include "media.h"
#include <meguviz/rife.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional adapter only; Media core never includes the RIFE header. */
MGM_API MgcStatus mgm_rife_effect_create(MgrPlan *plan,MgmEffect **effect,MgmEffectApi *api);
MGM_API MgcStatus mgm_rife_effect_release(MgmEffect *effect);
/* Reads the optional RIFE snapshot bound to this exact output frame. The
 * versioned output follows mgr_result_observation's prefix contract. Original,
 * bypass, disabled or otherwise unobserved frames return UNSUPPORTED. */
MGM_API MgcStatus mgm_rife_frame_observation(MgmFrame *frame,MgrJobObservation *observation);
#ifdef __cplusplus
}
#endif
#endif
