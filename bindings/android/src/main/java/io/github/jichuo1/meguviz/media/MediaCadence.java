package io.github.jichuo1.meguviz.media;

/** Bounded source-cadence state. All estimates and verdicts are owned by Media
 * core; this facade only transports values and owns the native handle. */
public final class MediaCadence implements AutoCloseable {
    public enum Verdict { PENDING, SUITABLE, UNSUITABLE }
    public static final class Snapshot {
        public final long frames, gaps, medianUs;
        public final int sampleCount;
        public final Double sourceFps;
        public final Verdict verdict;
        private Snapshot(long[] data) {
            sourceFps = data[1] == 0 ? null : Double.longBitsToDouble(data[2]);
            frames = data[3]; gaps = data[4]; sampleCount = (int)data[5]; medianUs = data[6];
            verdict = Verdict.values()[(int)data[7]];
        }
    }
    private long handle;
    public MediaCadence() { this(60, 12, false); }
    public MediaCadence(int targetFps) { this(targetFps, 12, false); }
    public MediaCadence(int targetFps, int minSamples, boolean interpolationRequired) {
        if (!MediaRuntime.isAttached()) throw new IllegalStateException("Attach the explicit native library first");
        if (targetFps <= 0 || minSamples < 0) throw new IllegalArgumentException("Invalid cadence configuration");
        long[] result = nCreate(targetFps, minSamples, interpolationRequired); check(result[0]); handle = result[1];
    }
    private static void check(long status) { if (status != 0) throw new IllegalStateException("Media cadence status " + status); }
    private void open() { if (handle == 0) throw new IllegalStateException("Cadence is closed"); }
    public synchronized void add(long ptsUs) { open(); check(nAdd(handle, ptsUs)); }
    /** Matches the original implementation: resets only the previous PTS. */
    public synchronized void reset() { open(); check(nReset(handle)); }
    public synchronized Snapshot snapshot() { open(); long[] data = nSnapshot(handle); check(data[0]); return new Snapshot(data); }
    public synchronized Double sourceFps() { return snapshot().sourceFps; }
    public synchronized Verdict verdict() { return snapshot().verdict; }
    public synchronized String describe() {
        Snapshot state = snapshot(); return String.format("%.2ffps frames=%d gaps=%d", state.sourceFps == null ? 0.0 : state.sourceFps, state.frames, state.gaps);
    }
    @Override public synchronized void close() { if (handle != 0) { nRelease(handle); handle = 0; } }
    private static native long[] nCreate(int targetFps, int minSamples, boolean required);
    private static native int nAdd(long handle, long ptsUs);
    private static native int nReset(long handle);
    private static native long[] nSnapshot(long handle);
    private static native void nRelease(long handle);
}
