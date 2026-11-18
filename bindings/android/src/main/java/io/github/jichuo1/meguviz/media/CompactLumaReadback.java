package io.github.jichuo1.meguviz.media;

import android.opengl.GLES30;
import java.nio.Buffer;
import java.nio.ByteBuffer;

/** One owner-thread PBO for one capture slot. The worker never sees a mapped buffer. */
final class CompactLumaReadback implements AutoCloseable {
    private final int width, height, bytes, buffer;
    private long fence;
    private WindowBackend.CaptureTiming timing;

    CompactLumaReadback(int width, int height) {
        this.width = width;
        this.height = height;
        this.bytes = Math.multiplyExact(Math.multiplyExact(width, height), 4);
        int[] names = new int[1];
        GLES30.glGenBuffers(1, names, 0);
        buffer = names[0];
        if (buffer == 0) throw new IllegalStateException("compact pixel pack buffer allocation failed");
        try {
            GLES30.glBindBuffer(GLES30.GL_PIXEL_PACK_BUFFER, buffer);
            GLES30.glBufferData(GLES30.GL_PIXEL_PACK_BUFFER, bytes, null, GLES30.GL_STREAM_READ);
            if (GLES30.glGetError() != GLES30.GL_NO_ERROR) throw new IllegalStateException("compact pixel pack storage allocation failed");
        } catch (RuntimeException | Error failure) {
            GLES30.glDeleteBuffers(1, names, 0);
            throw failure;
        } finally {
            GLES30.glBindBuffer(GLES30.GL_PIXEL_PACK_BUFFER, 0);
        }
    }

    boolean pending() { return fence != 0L; }

    void issue(WindowBackend.CaptureTiming receipt) {
        if (pending()) throw new IllegalStateException("capture slot still has pending compact readback");
        receipt.lumaReadStartNs = System.nanoTime();
        tightPack();
        GLES30.glBindBuffer(GLES30.GL_PIXEL_PACK_BUFFER, buffer);
        try {
            GLES30.glReadPixels(0, 0, width, height, GLES30.GL_RGBA, GLES30.GL_UNSIGNED_BYTE, 0);
            fence = GLES30.glFenceSync(GLES30.GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (fence == 0L) throw new IllegalStateException("compact readback fence creation failed");
            timing = receipt;
            GLES30.glFlush();
        } finally {
            GLES30.glBindBuffer(GLES30.GL_PIXEL_PACK_BUFFER, 0);
        }
        receipt.lumaReadIssuedNs = System.nanoTime();
    }

    /** A null target retires a dropped source without copying or publishing byte readiness. */
    void poll(ByteBuffer target) {
        if (!pending()) return;
        int status = GLES30.glClientWaitSync(fence, 0, 0);
        if (status == GLES30.GL_TIMEOUT_EXPIRED) return;
        if (!signaled(status)) throw new IllegalStateException("compact fence poll status " + status);
        timing.lumaFenceObservedNs = System.nanoTime();
        if (target != null) {
            long mapStart = System.nanoTime();
            GLES30.glBindBuffer(GLES30.GL_PIXEL_PACK_BUFFER, buffer);
            try {
                Buffer mapped = GLES30.glMapBufferRange(GLES30.GL_PIXEL_PACK_BUFFER, 0, bytes, GLES30.GL_MAP_READ_BIT);
                boolean intact;
                try {
                    if (!(mapped instanceof ByteBuffer) || mapped.capacity() < bytes || target.capacity() < bytes) {
                        throw new IllegalStateException("invalid mapped compact readback");
                    }
                    ByteBuffer data = (ByteBuffer) mapped;
                    data.position(0);
                    data.limit(bytes);
                    target.clear();
                    target.put(data);
                    target.position(0);
                } finally {
                    intact = mapped != null && GLES30.glUnmapBuffer(GLES30.GL_PIXEL_PACK_BUFFER);
                }
                if (!intact) throw new IllegalStateException("compact readback contents invalidated");
            } finally {
                GLES30.glBindBuffer(GLES30.GL_PIXEL_PACK_BUFFER, 0);
            }
            timing.lumaReadyNs = System.nanoTime();
            timing.lumaMapCopyNs = timing.lumaReadyNs - mapStart;
        }
        GLES30.glDeleteSync(fence);
        fence = 0L;
        timing = null;
    }

    @Override public void close() {
        if (pending()) {
            int status = GLES30.glClientWaitSync(fence, GLES30.GL_SYNC_FLUSH_COMMANDS_BIT, 3_000_000_000L);
            if (!signaled(status)) throw new IllegalStateException("pending compact readback retirement status " + status);
            GLES30.glDeleteSync(fence);
            fence = 0L;
            timing = null;
        }
        GLES30.glDeleteBuffers(1, new int[]{buffer}, 0);
    }

    static void readSync(int width, int height, ByteBuffer target, WindowBackend.CaptureTiming timing) {
        timing.lumaReadStartNs = System.nanoTime();
        tightPack();
        GLES30.glBindBuffer(GLES30.GL_PIXEL_PACK_BUFFER, 0);
        target.clear();
        GLES30.glReadPixels(0, 0, width, height, GLES30.GL_RGBA, GLES30.GL_UNSIGNED_BYTE, target);
        timing.lumaReadIssuedNs = timing.lumaReadyNs = System.nanoTime();
    }

    private static boolean signaled(int status) {
        return status == GLES30.GL_ALREADY_SIGNALED || status == GLES30.GL_CONDITION_SATISFIED;
    }

    private static void tightPack() {
        // The experiment owns its EGL context. Normalize GL pack for both modes.
        GLES30.glPixelStorei(GLES30.GL_PACK_ALIGNMENT, 4);
        GLES30.glPixelStorei(GLES30.GL_PACK_ROW_LENGTH, 0);
        GLES30.glPixelStorei(GLES30.GL_PACK_SKIP_ROWS, 0);
        GLES30.glPixelStorei(GLES30.GL_PACK_SKIP_PIXELS, 0);
    }
}
