package io.github.jichuo1.meguviz.media;

/** Trusted submit-to-native-completion measurements for the explicit ADPF experiment.
 * CPU and GPU may overlap; no waiting estimate is derived by adding or subtracting them. */
public final class MediaWorkDuration {
    public final long periodStartUptimeNanos, totalDurationNanos, cpuDurationNanos, gpuDurationNanos;
    public final int workerThreadId;

    private MediaWorkDuration(long start, long total, long cpu, long gpu, int workerThreadId) {
        periodStartUptimeNanos = start;
        totalDurationNanos = total;
        cpuDurationNanos = cpu;
        gpuDurationNanos = gpu;
        this.workerThreadId = workerThreadId;
    }

    static MediaWorkDuration from(AndroidMediaSession.Completed completed) {
        if (completed == null || completed.status != 0 || completed.frame == null || completed.frameKind != 2 ||
            !completed.workerCpuObserved) return null;
        return create(completed.workPeriodStartUptimeNanos, completed.workPeriodDurationNanos,
            completed.workerCpuNanos, completed.gpuMeasurement, completed.workerThreadId);
    }

    static MediaWorkDuration create(long start, long total, long cpu, long[] gpu, int workerThreadId) {
        if (start <= 0 || total <= 0 || cpu < 0 || cpu > total || workerThreadId <= 0 ||
            gpu == null || gpu.length != 7 || gpu[0] != 1) return null;
        long gpuDuration = gpu[1], batches = gpu[2], observedBatches = gpu[3], validBits = gpu[4];
        float timestampPeriod = Float.intBitsToFloat((int)gpu[5]);
        if (gpuDuration <= 0 || gpuDuration > total || batches <= 0 || observedBatches != batches ||
            validBits <= 0 || validBits > 64 || !Float.isFinite(timestampPeriod) || timestampPeriod <= 0) return null;
        return new MediaWorkDuration(start, total, cpu, gpuDuration, workerThreadId);
    }
}
