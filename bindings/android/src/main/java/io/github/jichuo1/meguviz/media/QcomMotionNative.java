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

    /** Parallel top-two reduction. enabled=true selects mode 1 (default, reduction);
     *  enabled=false selects mode 0 (explicit rollback). modes 2 and 3 are rejected
     *  because the alternate-window gate lost its quality gate on device. Both
     *  shipped modes are byte-identical in output. Returns 0 on success, -1 on
     *  rejection. */
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
     * strategy: 1 = bounded spin then block (a default handle uses this with the
     * adaptive budget), 0 = blocking (explicit rollback). The create-time property
     * debug.meguviz.qcom.wait.strategy selects 0 = blocking, 1 = fixed spin,
     * 2 = adaptive (absent = 2).
     * This setter does not change the budget mode: a new handle keeps the adaptive
     * budget, so call setWaitSpinNs with a non-zero value for a fixed budget.
     * spinNs 0 selects the adaptive budget (previous frame's retire interval +
     * margin). */
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
