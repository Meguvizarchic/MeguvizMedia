#include "qcom_motion.h"
#include "ahb_motion_api.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <jni.h>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
constexpr unsigned width = QCOM_MOTION_WIDTH / 4;
constexpr unsigned height = QCOM_MOTION_HEIGHT;
constexpr jlong cells = QCOM_MOTION_GRID_WIDTH * QCOM_MOTION_GRID_HEIGHT;

bool extension(const char *set, const char *key)
{
    return set && (std::string(" ") + set + " ").find(std::string(" ") + key + " ") != std::string::npos;
}

template<class T> T entry(const char *name)
{
    auto value = reinterpret_cast<T>(eglGetProcAddress(name));
    if (!value) throw std::runtime_error(name);
    return value;
}

struct GlFns {
    PFNEGLCREATESYNCKHRPROC fence = nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroyFence = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC duplicate = nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC clientWait = nullptr;

    void resolve(EGLDisplay display)
    {
        if (display == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT)
            throw std::runtime_error("no current capture context");
        const char *ext = eglQueryString(display, EGL_EXTENSIONS);
        if (!extension(ext, "EGL_ANDROID_native_fence_sync") || !extension(ext, "EGL_KHR_fence_sync"))
            throw std::runtime_error("no EGL native fence capability");
        fence = entry<decltype(fence)>("eglCreateSyncKHR");
        destroyFence = entry<decltype(destroyFence)>("eglDestroySyncKHR");
        duplicate = entry<decltype(duplicate)>("eglDupNativeFenceFDANDROID");
        clientWait = entry<decltype(clientWait)>("eglClientWaitSyncKHR");
    }
};

struct GlSlot {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    AHardwareBuffer *buffer = nullptr;
    GLuint texture = 0;
    GlFns methods;

    bool isOwner() const
    {
        return display != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT &&
               display == eglGetCurrentDisplay() && context == eglGetCurrentContext();
    }

    void create(QcomMotion *motion, unsigned slot)
    {
        display = eglGetCurrentDisplay();
        context = eglGetCurrentContext();
        methods.resolve(display);
        const char *ext = eglQueryString(display, EGL_EXTENSIONS);
        if (!extension(ext, "EGL_KHR_image_base") ||
            !extension(ext, "EGL_ANDROID_image_native_buffer") ||
            !extension(ext, "EGL_ANDROID_get_native_client_buffer") ||
            !extension(reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS)), "GL_OES_EGL_image"))
            throw std::runtime_error("cannot bind Android buffer as a GLES 2D attachment");
        const auto client = entry<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>("eglGetNativeClientBufferANDROID");
        const auto createImage = entry<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
        destroyImage = entry<decltype(destroyImage)>("eglDestroyImageKHR");
        const auto bindImage = entry<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>("glEGLImageTargetTexture2DOES");
        AHardwareBuffer_Desc wanted{};
        wanted.width = width;
        wanted.height = height;
        wanted.layers = 1;
        wanted.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        wanted.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
        if (AHardwareBuffer_allocate(&wanted, &buffer) || !buffer)
            throw std::runtime_error("AHardwareBuffer_allocate");
        const auto native = client(buffer);
        const EGLint attributes[]{EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        if (!native || (image = createImage(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, native, attributes)) == EGL_NO_IMAGE_KHR)
            throw std::runtime_error("EGL attachment image import");
        GLint prevTexture = 0, drawFbo = 0, readFbo = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        bindImage(GL_TEXTURE_2D, image);
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        const GLenum complete = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
        glDeleteFramebuffers(1, &fbo);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTexture));
        const GLenum glError = glGetError();
        if (!texture || !fbo || complete != GL_FRAMEBUFFER_COMPLETE || glError != GL_NO_ERROR ||
            qcom_motion_bind_luma_ahb(motion, slot, buffer))
            throw std::runtime_error("AHB framebuffer or Vulkan import failed");
    }

    ~GlSlot()
    {
        if (texture) glDeleteTextures(1, &texture);
        if (image != EGL_NO_IMAGE_KHR) destroyImage(display, image);
        if (buffer) AHardwareBuffer_release(buffer);
    }
};

float *direct(JNIEnv *env, jobject buffer, jlong bytes)
{
    if (!buffer || env->GetDirectBufferCapacity(buffer) < bytes) return nullptr;
    auto *out = env->GetDirectBufferAddress(buffer);
    return reinterpret_cast<uintptr_t>(out) % alignof(float) == 0 ? static_cast<float *>(out) : nullptr;
}

/* One byte per cell: the static classification has no alignment requirement, but
 * returning null for a short buffer keeps the caller's error signal identical to
 * the float outputs. */
uint8_t *directBytes(JNIEnv *env, jobject buffer, jlong bytes)
{
    if (!buffer || env->GetDirectBufferCapacity(buffer) < bytes) return nullptr;
    return static_cast<uint8_t *>(env->GetDirectBufferAddress(buffer));
}

QcomMotion *motion(jlong pointer) { return reinterpret_cast<QcomMotion *>(pointer); }
}

#define QCOMJNI(name) Java_io_github_jichuo1_meguviz_media_QcomMotionNative_##name

extern "C" JNIEXPORT jlong JNICALL QCOMJNI(createAhb)(JNIEnv *, jclass)
{
    return reinterpret_cast<jlong>(qcom_motion_create_ahb(0));
}

extern "C" JNIEXPORT jlong JNICALL QCOMJNI(createLumaSlot)(JNIEnv *, jclass, jlong estimator, jint index)
{
    try {
        auto slot = std::make_unique<GlSlot>();
        slot->create(motion(estimator), static_cast<unsigned>(index));
        return reinterpret_cast<jlong>(slot.release());
    } catch (const std::exception &error) {
        std::fprintf(stderr, "QCOM AHB GLES slot: %s\n", error.what());
        return 0;
    }
}

extern "C" JNIEXPORT jint JNICALL QCOMJNI(lumaTexture)(JNIEnv *, jclass, jlong pointer)
{
    const auto *slot = reinterpret_cast<GlSlot *>(pointer);
    return slot && slot->isOwner() ? static_cast<jint>(slot->texture) : 0;
}

extern "C" JNIEXPORT jboolean JNICALL QCOMJNI(destroyLumaSlot)(JNIEnv *, jclass, jlong pointer)
{
    auto *slot = reinterpret_cast<GlSlot *>(pointer);
    if (!slot || !slot->isOwner()) return JNI_FALSE;
    delete slot;
    return JNI_TRUE;
}

extern "C" JNIEXPORT jint JNICALL QCOMJNI(publishLumaFence)(JNIEnv *, jclass, jlong pointer)
{
    const auto *slot = reinterpret_cast<GlSlot *>(pointer);
    if (!slot || !slot->isOwner()) return -1;
    try {
        const auto &methods = slot->methods;
        EGLSyncKHR fence = methods.fence(slot->display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
        if (fence == EGL_NO_SYNC_KHR) return -1;
        glFlush();
        int fd = methods.duplicate(slot->display, fence);
        const bool destroyed = methods.destroyFence(slot->display, fence) == EGL_TRUE;
        if (glGetError() != GL_NO_ERROR || !destroyed) {
            if (fd >= 0) close(fd);
            return -1;
        }
        return fd;
    } catch (const std::exception &) { return -1; }
}

extern "C" JNIEXPORT jint JNICALL QCOMJNI(lumaFenceStatus)(JNIEnv *, jclass, jint fd)
{
    if (fd < 0) return -1;
    pollfd ready{fd, POLLIN, 0};
    const int status = poll(&ready, 1, 0);
    if (status == 0) return 0;
    return status == 1 && ready.revents == POLLIN ? 1 : -1;
}

extern "C" JNIEXPORT void JNICALL QCOMJNI(closeLumaFence)(JNIEnv *, jclass, jint fd)
{
    if (fd >= 0) close(fd);
}

extern "C" JNIEXPORT jboolean JNICALL QCOMJNI(drainLumaEgl)(JNIEnv *, jclass, jlong pointer)
{
    const auto *slot = reinterpret_cast<GlSlot *>(pointer);
    if (!slot || !slot->isOwner()) return JNI_FALSE;
    try {
        const auto &methods = slot->methods;
        EGLSyncKHR fence = methods.fence(slot->display, EGL_SYNC_FENCE_KHR, nullptr);
        if (fence == EGL_NO_SYNC_KHR) return JNI_FALSE;
        const auto wait = methods.clientWait(slot->display, fence, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 2000000000ULL);
        const bool destroyed = methods.destroyFence(slot->display, fence) == EGL_TRUE;
        return wait == EGL_CONDITION_SATISFIED_KHR && destroyed && glGetError() == GL_NO_ERROR;
    } catch (const std::exception &) { return JNI_FALSE; }
}

extern "C" JNIEXPORT jboolean JNICALL QCOMJNI(ahbRetired)(JNIEnv *, jclass, jlong pointer)
{
    return qcom_motion_ahb_retired(motion(pointer)) == 1;
}

/* Composite-driver wait policy for the submit->retire fence.  Both settings only
 * change how the CPU observes the existing fence, so neither can alter any
 * estimator output.  Exposed so the renderer can select the measured-best
 * strategy without a rebuild. */
extern "C" JNIEXPORT jint JNICALL QCOMJNI(setWaitStrategy)(JNIEnv *, jclass, jlong pointer,
                                                           jint strategy)
{
    return qcom_motion_set_wait_strategy(motion(pointer), strategy);
}

extern "C" JNIEXPORT jint JNICALL QCOMJNI(setWaitSpinNs)(JNIEnv *, jclass, jlong pointer,
                                                        jlong spinNs)
{
    if (spinNs < 0) return -1;
    return qcom_motion_set_wait_spin_ns(motion(pointer), static_cast<uint64_t>(spinNs));
}

extern "C" JNIEXPORT jint JNICALL QCOMJNI(estimateAhbCandidates)(JNIEnv *env, jclass, jlong pointer,
    jint leftSlot, jint leftFd, jint rightSlot, jint rightFd, jobject vectors, jobject candidates)
{
    return qcom_motion_estimate_ahb(motion(pointer), {static_cast<unsigned>(leftSlot), leftFd},
        {static_cast<unsigned>(rightSlot), rightFd}, direct(env, vectors, cells * 4 * sizeof(float)), nullptr,
        direct(env, candidates, cells * 2 * QCOM_MOTION_CANDIDATE_COUNT * 4 * sizeof(float)),
        nullptr, QCOM_MOTION_CANDIDATE_WIDE);
}

/* AHB estimate that also surfaces the estimator's static classification and
 * bidirectional visibility to the Java caller.  Both optional outputs are
 * host-visible direct buffers the caller already owns, so no extra Vulkan
 * allocation or synchronisation is involved.  Buffers, in order:
 *   vectors     cells * 4 floats   forward dx,dy then backward dx,dy
 *   confidence  cells * 2 floats   optional forward, backward confidence
 *   candidates  cells * 2 * 4 * 4 floats   [cell][direction][rank][4], optional
 *   staticMask  cells bytes        1 = classified static, optional
 *   visibility  cells * 2 floats   [cell][direction] consistency * photometric
 * A null optional buffer is skipped; a non-null buffer that is too small yields
 * -1 before any submission. */
extern "C" JNIEXPORT jint JNICALL QCOMJNI(estimateAhbExtended)(JNIEnv *env, jclass, jlong pointer,
    jint leftSlot, jint leftFd, jint rightSlot, jint rightFd, jobject vectors, jobject confidence,
    jobject candidates, jobject staticMask, jobject visibility)
{
    constexpr jlong candidateBytes = cells * 2 * QCOM_MOTION_CANDIDATE_COUNT * 4 * sizeof(float);
    auto *vectorOutput = direct(env, vectors, cells * 4 * sizeof(float));
    auto *confidenceOutput = confidence ? direct(env, confidence, cells * 2 * sizeof(float)) : nullptr;
    auto *candidateOutput = candidates ? direct(env, candidates, candidateBytes) : nullptr;
    auto *maskOutput = staticMask ? directBytes(env, staticMask, QCOM_MOTION_STATIC_BYTES) : nullptr;
    auto *visibilityOutput = visibility
        ? direct(env, visibility, QCOM_MOTION_VISIBILITY_FLOATS * sizeof(float)) : nullptr;
    if (!vectorOutput || (confidence && !confidenceOutput) || (candidates && !candidateOutput) ||
        (staticMask && !maskOutput) || (visibility && !visibilityOutput))
        return -1;
    return qcom_motion_estimate_ahb_extended(motion(pointer), {static_cast<unsigned>(leftSlot), leftFd},
        {static_cast<unsigned>(rightSlot), rightFd}, vectorOutput, confidenceOutput, candidateOutput,
        maskOutput, visibilityOutput, nullptr, QCOM_MOTION_CANDIDATE_WIDE);
}

