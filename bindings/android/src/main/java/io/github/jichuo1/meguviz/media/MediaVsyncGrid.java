package io.github.jichuo1.meguviz.media;

/** Caller-clocked display grid. Estimation, extrapolation and release policy
 * execute in Media core; Java owns only the opaque handle and copied values. */
public final class MediaVsyncGrid implements AutoCloseable {
    private long handle;
    private long lastNs, periodNs;
    private boolean faster, slower;
    public MediaVsyncGrid(long contentPeriodNs) {
        if (!MediaRuntime.isAttached()) throw new IllegalStateException("Attach the explicit native library first");
        if (contentPeriodNs <= 0) throw new IllegalArgumentException("Positive content period required");
        long[] result = nCreate(contentPeriodNs); check(result[0]); handle = result[1]; copyState(result, 2);
    }
    private static void check(long status) { if (status != 0) throw new IllegalStateException("Media vsync status " + status); }
    private void open() { if (handle == 0) throw new IllegalStateException("Vsync grid is closed"); }
    private long[] snapshot(long dueNs, long nowNs) { open(); long[] data = nSnapshot(handle, dueNs, nowNs); check(data[0]); return data; }
    private void copyState(long[] data, int offset) { lastNs = data[offset]; periodNs = data[offset + 1]; faster = data[offset + 2] != 0; slower = data[offset + 3] != 0; }
    public synchronized long getLastNs() { open(); return lastNs; }
    public synchronized long getPeriodNs() { open(); return periodNs; }
    public synchronized boolean onVsync(long vsyncNs) { open(); long[] data = nOnVsync(handle, vsyncNs); check(data[0]); if (data[1] == 0) return false; copyState(data, 2); return true; }
    public synchronized boolean stale(long nowNs) { return snapshot(0, nowNs)[5] != 0; }
    public synchronized long current(long nowNs) { return snapshot(0, nowNs)[3]; }
    public synchronized boolean releasable(long dueNs, long nowNs) { return snapshot(dueNs, nowNs)[6] != 0; }
    public synchronized boolean sameRefresh(long dueNs, long nowNs) { return snapshot(dueNs, nowNs)[7] != 0; }
    public synchronized boolean fasterThanContent() { open(); return faster; }
    public synchronized boolean slowerThanContent() { open(); return slower; }
    public synchronized long untilNextNs(long nowNs) { return snapshot(0, nowNs)[4]; }
    @Override public synchronized void close() { if (handle != 0) { nRelease(handle); handle = 0; } }
    private static native long[] nCreate(long contentPeriodNs);
    private static native long[] nOnVsync(long handle, long vsyncNs);
    private static native long[] nSnapshot(long handle, long dueNs, long nowNs);
    private static native void nRelease(long handle);
}
