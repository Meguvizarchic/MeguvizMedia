package io.github.jichuo1.meguviz.media;

import android.content.Context;
import android.os.PerformanceHintManager;
import android.os.WorkDuration;
import android.annotation.TargetApi;

/** API 35 isolation: older runtimes never load WorkDuration or report a scalar fallback. */
@TargetApi(35)
final class MediaPerformanceHintApi35 implements AutoCloseable {
    private PerformanceHintManager.Session session;

    private MediaPerformanceHintApi35(PerformanceHintManager.Session session) { this.session = session; }

    static MediaPerformanceHintApi35 create(Context context, long targetNs, int workerThreadId) {
        try {
            PerformanceHintManager manager = context.getSystemService(PerformanceHintManager.class);
            if (manager == null) return null;
            return new MediaPerformanceHintApi35(manager.createHintSession(new int[]{workerThreadId}, targetNs));
        } catch (RuntimeException unavailable) {
            return null; // Optional power hints must never take down media playback.
        }
    }

    boolean report(MediaWorkDuration measurement) {
        PerformanceHintManager.Session current = session;
        if (current == null) return false;
        try {
            WorkDuration duration = new WorkDuration();
            duration.setWorkPeriodStartTimestampNanos(measurement.periodStartUptimeNanos);
            duration.setActualTotalDurationNanos(measurement.totalDurationNanos);
            duration.setActualCpuDurationNanos(measurement.cpuDurationNanos);
            duration.setActualGpuDurationNanos(measurement.gpuDurationNanos);
            current.reportActualWorkDuration(duration);
            return true;
        } catch (RuntimeException unavailable) {
            return false;
        }
    }

    @Override public void close() {
        PerformanceHintManager.Session old = session;
        session = null;
        if (old != null) old.close();
    }
}
