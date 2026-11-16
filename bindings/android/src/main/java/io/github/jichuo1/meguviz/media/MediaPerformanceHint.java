package io.github.jichuo1.meguviz.media;

import android.content.Context;
import android.os.Build;

/** Owns the optional hint session independently of the GL owner. No session exists by default. */
final class MediaPerformanceHint implements AutoCloseable {
    private final Context context;
    private final long targetWorkDurationNanos;
    private MediaPerformanceHintApi35 session;
    private int workerThreadId;

    private MediaPerformanceHint(Context context, long targetWorkDurationNanos) {
        this.context = context;
        this.targetWorkDurationNanos = targetWorkDurationNanos;
    }

    static MediaPerformanceHint create(Context context, long targetWorkDurationNanos) {
        if (context == null) throw new IllegalArgumentException("Context is required");
        if (targetWorkDurationNanos <= 0) throw new IllegalArgumentException("Target must be positive");
        Context application = context.getApplicationContext();
        return new MediaPerformanceHint(application != null ? application : context, targetWorkDurationNanos);
    }

    void report(AndroidMediaSession.Completed completed) {
        if (Build.VERSION.SDK_INT < 35) return;
        MediaWorkDuration measurement = MediaWorkDuration.from(completed);
        if (measurement == null) return;
        if (session != null && workerThreadId != measurement.workerThreadId) closeSession();
        if (session == null) {
            session = MediaPerformanceHintApi35.create(context, targetWorkDurationNanos, measurement.workerThreadId);
            workerThreadId = session != null ? measurement.workerThreadId : 0;
        }
        if (session != null && !session.report(measurement)) closeSession();
    }

    private void closeSession() {
        MediaPerformanceHintApi35 old = session;
        session = null;
        workerThreadId = 0;
        if (old != null) old.close();
    }

    @Override public void close() { closeSession(); }
}
