package io.github.jichuo1.meguviz.media;

/** One-shot refresh meter; warmup, interval median and reporting live in core. */
public final class MediaVsyncMeter implements AutoCloseable {
    private long handle;
    public MediaVsyncMeter() { this(60, 120); }
    public MediaVsyncMeter(int warmup, int samples) {
        if (!MediaRuntime.isAttached()) throw new IllegalStateException("Attach the explicit native library first");
        if (warmup < 0 || samples < 1 || samples > 4096) throw new IllegalArgumentException("Invalid refresh meter configuration");
        long[] result = nCreate(warmup, samples); check(result[0]); handle = result[1];
    }
    private static void check(long status) { if (status != 0) throw new IllegalStateException("Media meter status " + status); }
    public synchronized Float add(long frameTimeNs) {
        if (handle == 0) throw new IllegalStateException("Vsync meter is closed");
        long[] result = nAdd(handle, frameTimeNs); check(result[0]);
        return result[1] == 0 ? null : Float.intBitsToFloat((int)result[2]);
    }
    @Override public synchronized void close() { if (handle != 0) { nRelease(handle); handle = 0; } }
    private static native long[] nCreate(int warmup, int samples);
    private static native long[] nAdd(long handle, long frameTimeNs);
    private static native void nRelease(long handle);
}
