#ifndef MEGUVIZ_MEDIA_ANDROID_H
#define MEGUVIZ_MEDIA_ANDROID_H
#include "media.h"
#include "media_scheduler.h"
#include <meguviz/compute_android.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MgmAndroidSession MgmAndroidSession;
enum { MGM_ANDROID_AHB_HOST_WAIT=1,MGM_ANDROID_AHB_GPU_SYNC=2 };
enum { MGM_ANDROID_CANONICAL_TOP_DOWN=1 };
typedef struct MgmAndroidSessionDesc {
    MgcHeader header;
    MgcDomain *domain;
    uint64_t session_id,epoch;
    uint32_t width,height,pool_capacity,path;
    MgmEventFn event;
    void *event_context;
    uint32_t required_flags,reserved[3];
    /* Optional tail. Zero source dimensions use width/height. CANONICAL_TOP_DOWN
     * applies the legacy vertical flip before the raw source transform during
     * capture, and flips canonical top-down rows when presenting to GL. */
    uint32_t source_width,source_height,transform_flags,reserved_tail;
} MgmAndroidSessionDesc;
typedef struct MgmAndroidProbe {
    MgcHeader header;
    uint32_t actual_width,actual_height,actual_stride,actual_format;
    uint64_t actual_usage;
    int32_t egl_renderable,egl_sampleable,vulkan_sampleable,vulkan_storage;
    /* These fields preserve Compute query/probe failures. UNSUPPORTED means
     * the EGL fence path or Compute capability is unavailable. HOST_WAIT does
     * not require them; GPU_SYNC requires both to succeed. An observed device
     * loss takes priority over the overall result for either path. */
    int32_t native_fence_import,native_fence_export;
    uint32_t workload_qualified,required_flags;
} MgmAndroidProbe;
typedef struct MgmAndroidCapture {
    MgcHeader header;
    uint64_t source_id,epoch;
    int64_t pts_us;
    uint32_t oes_texture,required_flags;
    /* Column-major raw SurfaceTexture/crop/rotation transform. With the session's
     * CANONICAL_TOP_DOWN flag, the platform samples T * F; callers must not add
     * another vertical flip. Flags=0 retains explicit caller orientation. */
    float texture_transform[16];
    MgmColor color;
} MgmAndroidCapture;
typedef struct MgmAndroidGuardResult {
    MgcHeader header;
    uint32_t is_static,is_cut;
    uint64_t sampled_readback_bytes;
    uint32_t is_duplicate,reserved;
} MgmAndroidGuardResult;
/* All EGL/GLES operations run on the creating current context/thread. The
 * domain is retained. Explicit probe allocates and imports the exact requested
 * RGBA8 extent/usage and checks FBO completeness, Vulkan usage and fence paths;
 * it never grants workload qualification. create does not silently downgrade.
 * Output frames own real Compute content leases. Slots are pinned by Compute's
 * final content callback, so AHB acquire/reference counts never permit rewrite.
 * Pool full returns BACKPRESSURE without taking the source's content ownership.
 * No decoder AImage/SurfaceControl or HDR profile is enabled here.
 * Event callbacks run synchronously on the creating context/thread. Reentrant
 * operations on the same session, including close, return BUSY; callers may
 * retry after the callback and enclosing operation return. Operations on a
 * different thread/context remain invalid. The callback and its context must
 * stay valid until replaced or the session is successfully closed. The event
 * pointer is borrowed and valid only for the duration of that callback. C++
 * exceptions must not cross this C callback boundary. */
/* Before synchronous creation, call prepare on a background thread while an
 * independent host/JVM loader reference keeps this library loaded through the
 * call. Preparation is idempotent while a bounded slot is ready; concurrent
 * preparations can reserve multiple slots. Creation consumes a slot with
 * a nonblocking operation before its first GL side effect; without a slot it
 * returns BUSY. Failed creations retain their resources and are progressed on
 * the original GL owner/context by service_owner and on a background thread
 * by service_background. The host must keep the original EGL context and its
 * owner thread alive until all its failed creations finish; equality of EGL
 * handle values cannot establish continuity after destruction and reuse.
 * Neither service may be used after the host releases its own independent
 * executing-image reference. */
MGM_API MgcStatus mgm_android_creation_prepare(void);
/* Background-only exclusive reservation, carrying a positive opaque token.
 * Concurrent prepare/legacy create cannot take a reserved slot. Tokens are
 * valid only within this process and this loaded Media image; never pass them
 * across simultaneous images or process lifetimes. The clock can yield BUSY
 * while establishing uniqueness or during contention. */
MGM_API MgcStatus mgm_android_creation_reserve(uint64_t *token);
/* Background-only cancellation. An already consumed or stale token is a
 * no-op; BUSY means retry. Service_background performs actual pin release. */
MGM_API MgcStatus mgm_android_creation_release_reservation(uint64_t token);
/* Cancel a reserved or initializing token from any thread. SUCCESS wins over
 * later commit; BUSY means commit already won and the live session must close
 * normally. A driver call already executing may finish before retirement. */
MGM_API MgcStatus mgm_android_creation_cancel_import(uint64_t token);
MGM_API MgcStatus mgm_android_creation_service_owner(void);
MGM_API MgcStatus mgm_android_creation_service_background(void);
/* Discard currently unused prewarmed slots on a background thread; a slot
 * concurrently claimed by creation is unaffected. Requires the same host
 * loader/execution reference as prepare and background service. */
MGM_API MgcStatus mgm_android_creation_discard_ready(void);
MGM_API MgcStatus mgm_android_session_create(const MgmAndroidSessionDesc *,MgmAndroidSession **);
/* GL owner consumes one exact token; BUSY leaves the caller's token unconsumed
 * unless creation has already started. Creation failures are held as orphans. */
MGM_API MgcStatus mgm_android_session_create_reserved(const MgmAndroidSessionDesc *,uint64_t token,MgmAndroidSession **);
/* Staged creation. begin runs on the GL owner, returns ACCEPTED and an owned
 * but unusable initializing session; it allocates GL/AHB objects but performs
 * no Vulkan import. import runs off the GL owner and can block; while running,
 * close returns BUSY and the pool retains the session/domain/code/AHB. commit
 * runs on the original GL owner, verifies generation and cancellation, and
 * publishes a usable session without GPU import or object destruction. On any
 * failure/cancellation after ACCEPTED the caller must retain and close session. */
MGM_API MgcStatus mgm_android_session_begin_reserved(const MgmAndroidSessionDesc *,uint64_t token,MgmAndroidSession **);
MGM_API MgcStatus mgm_android_creation_import_background(uint64_t token);
MGM_API MgcStatus mgm_android_session_commit_initialization(MgmAndroidSession *);
/* Each session admits one probe attempt; its temporary GPU resources remain
 * session-owned until close. A repeated attempt returns BUSY. */
MGM_API MgcStatus mgm_android_probe(MgmAndroidSession *,MgmAndroidProbe *);
/* Capture temporarily binds no pack/unpack PBO and uses tight client-memory
 * rows; nondefault PBO bindings and PACK_ALIGNMENT/ROW_LENGTH/SKIP_* are saved
 * and restored along with independent read/draw framebuffers and touched draw
 * state. The caller owns bound objects and keeps them valid through the call.
 * Dither enablement is preserved during drawing to match the source profile.
 * GL checks consume error flags: ordinary pre-existing errors return
 * INTERNAL_ERROR; GL_OUT_OF_MEMORY returns OUT_OF_MEMORY, before changing GL
 * state. A clean error state is required on entry and after
 * callbacks. Callbacks run outside the capture state transaction and must leave
 * the creating context current. Ordinary GL failures attempt checked recovery;
 * context loss, GL OOM or failed recovery leave state/retirement uncertain and
 * the session rejects further work or close rather than freeing unknown work.
 * This is not a device-reset or bounded GPU-retirement recovery contract. */
MGM_API MgcStatus mgm_android_capture_oes(MgmAndroidSession *,const MgmAndroidCapture *,MgmFrame **owned_frame);
MGM_API MgcStatus mgm_android_compare_inputs(MgmAndroidSession *,MgmFrame *,MgmFrame *,MgmAndroidGuardResult *);
/* Draws into caller's current framebuffer. Success is SINK_ACCEPTED only;
 * caller owns eglSwapBuffers and must never label a swap as actual present.
 * Frame stays pinned until GL's sampling fence signals. Compute's external
 * access reservation spans draw→native-fence registration; without a native fd,
 * poll keeps that reservation until the KHR fence signals and registers -1.
 * A second pending read of the same allocation is rejected, never replacing an
 * unretired dependency. Before-draw failures close an unused reservation;
 * unobservable submitted work preserves its pin/reservation. poll retires fences;
 * a timeout/failed fence preserves the frame. At most two draws are pending. */
MGM_API MgcStatus mgm_android_draw(MgmAndroidSession *,MgmFrame *,uint32_t width,uint32_t height);
MGM_API MgcStatus mgm_android_poll(MgmAndroidSession *,uint64_t timeout_ns);
/* On a lost device, release can require Vulkan/driver work and must not run on
 * the GL owner. Defer transfers one owned frame reference only on SUCCESS;
 * BUSY retains caller ownership. The non-GL service may block in a destructor;
 * the caller must keep both session and code image alive until service returns.
 * poll/close remain BUSY while deferred work or its GL-owner receipt remains. */
MGM_API MgcStatus mgm_android_defer_frame_release(MgmAndroidSession *,MgmFrame *owned_frame);
MGM_API MgcStatus mgm_android_service_retirement(MgmAndroidSession *);
/* Strictly newer epoch invalidates capture/draw of old frames. Pending GL work
 * still retires normally. Pause/seek/surface loss are host policy events. */
MGM_API MgcStatus mgm_android_begin_epoch(MgmAndroidSession *,uint64_t epoch);
MGM_API MgcStatus mgm_android_set_event_handler(MgmAndroidSession *,MgmEventFn,void *context);
/* BUSY leaves the object valid when content leases, GL reads or background
 * releases remain. Closing can first enqueue slot/resource/domain references;
 * call mgm_android_service_retirement from a non-GL thread while keeping the
 * session and code image alive, then poll/close again on the creating context.
 * Device loss also requires mgc_domain_service from a non-GL thread. Neither
 * service substitutes Vulkan retirement for an external producer/GL fence.
 * Destroy from another context/thread is rejected, preserving EGL ownership. */
MGM_API MgcStatus mgm_android_session_close(MgmAndroidSession *);
#ifdef __cplusplus
}
#endif
#endif
