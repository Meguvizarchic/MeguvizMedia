package io.github.jichuo1.meguviz.media;

/** Calls are serialized on one EGL owner with the window context current. */
public interface WindowBackend extends AutoCloseable {
    /** Returns null on temporary resource pressure. */
    Object capture(int oes, float[] transform, long sourceId, long ptsUs);
    default int lastCaptureStatus() { return 0; }
    /** GPU capture is available to draw even if compact CPU input is still pending. */
    default boolean inputsReady(Object left, Object right) { return true; }
    default CaptureTiming captureTiming(Object frame) { return null; }

    /** Returns zero if this pair cannot be admitted. A positive ID has one terminal poll receipt. */
    long submit(Object left, Object right, long deadlineNs);
    /** Advisory cancellation. An accepted job still retires through poll or close. */
    default void cancel(long jobId) {}

    /** Returns null when no completion is currently deliverable. */
    Completion poll();
    void draw(Object frame, int width, int height);
    void release(Object frame);
    @Override void close();

    /** Owner clock observations. Fence observation is not a GPU duration. */
    final class CaptureTiming {
        public long lumaPublishedNs;
        public long lumaReadStartNs, lumaReadIssuedNs, lumaFenceObservedNs, lumaReadyNs, lumaMapCopyNs;
    }

    final class Completion {
        public final long jobId;
        public final int status, frameKind;
        public final Object frame;
        public final long workDurationNs;
        /** Synchronous call wall times in ns, or -1 if this backend does not report the stage. */
        public final long nativeEstimateNs, candidateProjectionNs;
        public Completion(long jobId, int status, int frameKind, Object frame, long workDurationNs) {
            this(jobId, status, frameKind, frame, workDurationNs, -1L, -1L);
        }
        public Completion(long jobId, int status, int frameKind, Object frame, long workDurationNs,
                          long nativeEstimateNs, long candidateProjectionNs) {
            this.jobId = jobId;
            this.status = status;
            this.frameKind = frameKind;
            this.frame = frame;
            this.workDurationNs = workDurationNs;
            this.nativeEstimateNs = nativeEstimateNs;
            this.candidateProjectionNs = candidateProjectionNs;
        }
    }

    final class Backpressure extends RuntimeException {
        public final int status;
        public Backpressure() { this(0); }
        public Backpressure(int status) { super("draw backpressure"); this.status = status; }
        public Backpressure(String message) { super(message); status = 0; }
    }
}
