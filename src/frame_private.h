#ifndef MEGUVIZ_MEDIA_FRAME_PRIVATE_H
#define MEGUVIZ_MEDIA_FRAME_PRIVATE_H
#include <meguviz/media.h>
#include <atomic>

/* The frame stays opaque in the public ABI. Optional attachments are written
 * before publication and remain immutable until the last frame reference. */
#if defined(MGR_ENABLE_JOB_OBSERVATION)
namespace mgm_detail {
struct FrameAttachment {
    const void *tag;
    void (*destroy)(FrameAttachment *) noexcept;
};
}
#endif
struct MgmFrame {
    std::atomic<uint32_t> refs{1};
    MgmFrameDesc desc{};
#if defined(MGR_ENABLE_JOB_OBSERVATION)
    mgm_detail::FrameAttachment *attachment=nullptr;
#endif
};
#endif
