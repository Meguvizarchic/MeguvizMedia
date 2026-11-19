package io.github.jichuo1.meguviz.media;

/** Adapts an already prepared Media/RIFE session to the midpoint window contract. */
final class RifeWindowSession implements WindowBackend {
    private final AndroidMediaSession session;
    private final int primaries, matrix, flowScale;
    private int captureStatus;

    RifeWindowSession(AndroidMediaSession session, FrameGenerationProfile profile, int flowScale) {
        this.session = session;
        primaries = profile.primaries;
        matrix = profile.matrix;
        this.flowScale = flowScale;
    }

    @Override public Object capture(int oes, float[] transform, long sourceId, long ptsUs) {
        captureStatus = 0;
        try {
            return session.capture(oes, transform, sourceId, ptsUs, primaries, matrix);
        } catch (AndroidMediaSession.MediaException failure) {
            if (!temporary(failure.status)) throw failure;
            captureStatus = failure.status;
            return null;
        }
    }

    @Override public int lastCaptureStatus() { return captureStatus; }

    @Override public long submit(Object left, Object right, long deadlineNs) {
        try {
            return session.submit((AndroidMediaSession.Frame) left, (AndroidMediaSession.Frame) right,
                0.5, deadlineNs, flowScale);
        } catch (AndroidMediaSession.MediaException failure) {
            if (temporary(failure.status) || failure.status == FrameGenerationSession.DEADLINE_REJECTED) return 0;
            throw failure;
        }
    }

    @Override public void cancel(long jobId) { session.cancel(jobId); }

    @Override public Completion poll() {
        int status = session.poll(0);
        AndroidMediaSession.Completed result = session.pollCompleted();
        if (result != null && (result.jobId != 0 || !temporary(result.status))) {
            return new Completion(result.jobId, result.status, result.frameKind, result.frame,
                result.workPeriodDurationNanos);
        }
        if (status != 0 && !temporary(status)) throw new IllegalStateException("Media poll status " + status);
        return null;
    }

    @Override public void draw(Object frame, int width, int height) {
        try {
            session.draw((AndroidMediaSession.Frame) frame, width, height);
        } catch (AndroidMediaSession.MediaException failure) {
            if (temporary(failure.status)) throw new Backpressure(failure.status);
            throw failure;
        }
    }

    @Override public void release(Object frame) { ((AndroidMediaSession.Frame) frame).close(); }

    @Override public void close() {
        session.poll(0);
        AndroidMediaSession.Completed result = session.pollCompleted();
        if (result != null && result.frame != null) result.frame.close();
        try {
            session.close();
        } catch (AndroidMediaSession.MediaException failure) {
            if (temporary(failure.status)) throw new Backpressure(failure.status);
            throw failure;
        }
    }

    private static boolean temporary(int status) { return status == -7 || status == -8 || status == -12; }
}
