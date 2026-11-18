package io.github.jichuo1.meguviz.media;

import java.nio.ByteBuffer;

/** JNI for the isolated Vulkan low-resolution motion estimator. */
final class QcomMotionNative {
    static { System.loadLibrary("meguviz_qcom_motion"); }

    private QcomMotionNative() {}

    static native long create();
    static native int estimate(long handle, ByteBuffer previousR8, ByteBuffer currentR8,
                               ByteBuffer vectors, ByteBuffer confidence);
    /** Float4 [cell][direction][rank]: dx, dy, normalized mean box SAD, valid. */
    static native int estimateCandidates(long handle, ByteBuffer previousR8, ByteBuffer currentR8,
                                        ByteBuffer vectors, ByteBuffer confidence, ByteBuffer candidates);
    /** Same as estimateCandidates plus the static classification and visibility fields. */
    static native int estimateExtended(long handle, ByteBuffer previousR8, ByteBuffer currentR8,
                                      ByteBuffer vectors, ByteBuffer confidence, ByteBuffer candidates,
                                      ByteBuffer staticMask, ByteBuffer visibility);
    static native int prepareCandidates(ByteBuffer candidates, ByteBuffer atlas);
    static native void destroy(long handle);

    /** Opt-in parallel top-two reduction. mode is 0 (default) or 1 (reduction);
     *  modes 2 and 3 are rejected because the alternate-window gate lost its
     *  quality gate on device. Returns 0 on success, -1 on rejection. */
    static native int setAdaptive(long handle, boolean enabled, int confidenceMilli);
    /** Disables the fine sub-pixel refinement when enabled is false. */
    static native void setSubpixel(long handle, boolean enabled);
    /** Static-classification thresholds in thousandths of a normalized luma code value. */
    static native int setStaticThresholds(long handle, int thresholdMilli, int neighborMilli);
    /** Forces zero motion on cells the fine stage classified static. */
    static native void setStaticGate(long handle, boolean enabled);

    static native long createAhb();
    static native long createLumaSlot(long estimator, int index);
    static native int lumaTexture(long slot);
    static native boolean destroyLumaSlot(long slot);
    static native int publishLumaFence(long slot);
    static native int lumaFenceStatus(int fd);
    static native void closeLumaFence(int fd);
    static native boolean drainLumaEgl(long slot);
    static native boolean ahbRetired(long estimator);
    /** Composite-driver wait policy for the submit->retire fence.  Only how the CPU
     * observes the existing fence changes; estimator outputs are unaffected.
     * strategy: 0 = blocking (default), 1 = bounded spin then block.  spinNs 0
     * selects the adaptive budget (previous frame's retire interval + margin). */
    static native int setWaitStrategy(long estimator, int strategy);
    /** Fixed spin budget in ns, clamped to one 60 Hz frame; 0 = adaptive budget. */
    static native int setWaitSpinNs(long estimator, long spinNs);
    static native int estimateAhbCandidates(long estimator, int leftSlot, int leftFd, int rightSlot, int rightFd,
                                            ByteBuffer vectors, ByteBuffer candidates);
    /** AHB estimate that also surfaces the static mask and bidirectional visibility. */
    static native int estimateAhbExtended(long estimator, int leftSlot, int leftFd, int rightSlot, int rightFd,
                                          ByteBuffer vectors, ByteBuffer confidence, ByteBuffer candidates,
                                          ByteBuffer staticMask, ByteBuffer visibility);
}
