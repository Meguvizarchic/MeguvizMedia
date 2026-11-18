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
    static native int prepareCandidates(ByteBuffer candidates, ByteBuffer atlas);
    static native void destroy(long handle);
    static native long createAhb();
    static native long createLumaSlot(long estimator, int index);
    static native int lumaTexture(long slot);
    static native boolean destroyLumaSlot(long slot);
    static native int publishLumaFence(long slot);
    static native int lumaFenceStatus(int fd);
    static native void closeLumaFence(int fd);
    static native boolean drainLumaEgl(long slot);
    static native boolean ahbRetired(long estimator);
    static native int estimateAhbCandidates(long estimator, int leftSlot, int leftFd, int rightSlot, int rightFd,
                                            ByteBuffer vectors, ByteBuffer candidates);
}
