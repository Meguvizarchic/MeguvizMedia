package io.github.jichuo1.meguviz.media;

import java.util.Objects;
import java.util.function.LongSupplier;
import java.util.function.Supplier;

/**
 * Optional midpoint generation on the host's EGL owner. The host retains its playback
 * clock, original-frame presentation path and ownership of the EGL context.
 */
public final class FrameGenerationSession implements WindowBackend {
    static final int DEADLINE_REJECTED = -9, CANCELLED = -13;
    private static final int INFERRED = 2;

    private final Thread owner = Thread.currentThread();
    private final FrameGenerationProfile.Qualification qualification;
    private final WindowBackend backend;
    private final LongSupplier clock;
    private long generation = 1;
    private long jobId, jobGeneration, jobDeadlineNs;
    private int ownedFrames;
    private boolean closing, closed;

    /**
     * Opens the highest-priority qualified route. A null result leaves original playback
     * to the host. The RIFE supplier transfers a ready, exclusive session only if invoked;
     * it may return null when that route is unavailable. QCOM uses compact AHB luma.
     * Call outside the playback hot path on the EGL owner.
     */
    public static FrameGenerationSession open(FrameGenerationProfile profile,
            FrameGenerationProfile.Qualification rife, Supplier<AndroidMediaSession> preparedRife,
            int flowScale, FrameGenerationProfile.Qualification qcom) {
        FrameGenerationProfile.Qualification choice = profile.select(rife, qcom);
        if (choice != null && choice.backend == FrameGenerationProfile.Backend.RIFE) {
            if (flowScale != 0 && flowScale != 4 && flowScale != 6 && flowScale != 8)
                throw new IllegalArgumentException("invalid RIFE flow scale");
            AndroidMediaSession session = preparedRife == null ? null : preparedRife.get();
            if (session != null) return new FrameGenerationSession(choice,
                new RifeWindowSession(session, profile, flowScale), System::nanoTime);
            choice = profile.select(null, qcom);
        }
        if (choice == null) return null;
        try {
            QcomMotionNative.ensureLoaded();
        } catch (UnsatisfiedLinkError | NoClassDefFoundError unavailable) {
            return null;
        }
        try {
            return new FrameGenerationSession(choice,
                new GlesMotionWarp(profile.width, profile.height, profile.outputWidth,
                    profile.outputHeight, false, true, 0), System::nanoTime);
        } catch (GlesMotionWarp.Unsupported unavailable) {
            return null;
        }
    }

    FrameGenerationSession(FrameGenerationProfile.Qualification qualification, WindowBackend backend,
                           LongSupplier clock) {
        this.qualification = Objects.requireNonNull(qualification);
        this.backend = Objects.requireNonNull(backend);
        this.clock = Objects.requireNonNull(clock);
    }

    public FrameGenerationProfile.Backend backend() { return qualification.backend; }

    @Override public Object capture(int oes, float[] transform, long sourceId, long ptsUs) {
        checkOpen();
        Object captured = backend.capture(oes, transform, sourceId, ptsUs);
        return captured == null ? null : own(captured, sourceId, ptsUs, false, 0);
    }

    @Override public int lastCaptureStatus() { checkOpen(); return backend.lastCaptureStatus(); }

    @Override public boolean inputsReady(Object left, Object right) {
        checkOpen();
        return backend.inputsReady(source(left).value, source(right).value);
    }

    @Override public CaptureTiming captureTiming(Object frame) {
        checkOpen();
        return backend.captureTiming(source(frame).value);
    }

    @Override public long submit(Object left, Object right, long deadlineNs) {
        checkOpen();
        Frame first = source(left), second = source(right);
        if (first.sourceId >= second.sourceId || first.ptsUs >= second.ptsUs)
            throw new IllegalArgumentException("invalid source pair");
        if (jobId != 0 || !qualification.fits(clock.getAsLong(), deadlineNs)) return 0;
        long accepted = backend.submit(first.value, second.value, deadlineNs);
        if (accepted > 0) {
            jobId = accepted;
            jobGeneration = generation;
            jobDeadlineNs = deadlineNs;
        }
        return accepted;
    }

    /**
     * Call on seek or discontinuity. Existing frames remain releasable. The accepted
     * job keeps its retirement slot until the backend returns a terminal receipt.
     * Open a fresh session for resolution, color, cadence or device changes.
     */
    public void invalidate() {
        checkOpen();
        ++generation;
        if (jobId != 0) backend.cancel(jobId);
    }

    @Override public void cancel(long id) {
        checkOpen();
        if (id > 0 && id == jobId) {
            jobGeneration = 0;
            backend.cancel(id);
        }
    }

    @Override public Completion poll() {
        checkOpen();
        Completion result = backend.poll();
        if (result == null) return null;
        if (jobId == 0 || result.jobId != jobId) {
            if (result.frame != null) backend.release(result.frame);
            throw new IllegalStateException("unexpected midpoint completion " + result.jobId);
        }
        jobId = 0;
        int status = result.status;
        if (status == 0 && jobGeneration != generation) status = CANCELLED;
        if (status == 0 && jobDeadlineNs - clock.getAsLong() <= 0) status = DEADLINE_REJECTED;
        Object output = null;
        if (result.frame != null) {
            if (status == 0 && result.frameKind == INFERRED) output = own(result.frame, 0, 0, true, jobDeadlineNs);
            else backend.release(result.frame);
        }
        return new Completion(result.jobId, status, output == null ? 0 : INFERRED, output,
            result.workDurationNs, result.nativeEstimateNs, result.candidateProjectionNs);
    }

    @Override public void draw(Object frame, int width, int height) {
        checkOpen();
        Frame owned = current(frame);
        if (width != qualification.profile.outputWidth || height != qualification.profile.outputHeight)
            throw new IllegalArgumentException("output size differs from the prepared profile");
        if (owned.inferred && owned.deadlineNs - clock.getAsLong() <= 0) throw new Backpressure(DEADLINE_REJECTED);
        backend.draw(owned.value, width, height);
    }

    @Override public void release(Object frame) {
        checkOwner();
        Frame owned = owned(frame);
        backend.release(owned.value);
        owned.value = null;
        --ownedFrames;
    }

    /** Release all frames first. A close failure may leave EGL resources owned;
     * keep the owner/context and retry retirement before destroying them. */
    @Override public void close() {
        checkOwner();
        if (closed) return;
        if (ownedFrames != 0) throw new IllegalStateException("release frame references before close");
        if (!closing) {
            closing = true;
            if (jobId != 0) backend.cancel(jobId);
        }
        backend.close();
        closed = true;
    }

    private Frame own(Object value, long sourceId, long ptsUs, boolean inferred, long deadlineNs) {
        try {
            Frame frame = new Frame(value, sourceId, ptsUs, inferred, deadlineNs);
            ++ownedFrames;
            return frame;
        } catch (OutOfMemoryError failure) {
            backend.release(value);
            throw failure;
        }
    }

    private Frame source(Object frame) {
        Frame owned = current(frame);
        if (owned.inferred) throw new IllegalArgumentException("expected an original frame");
        return owned;
    }

    private Frame current(Object frame) {
        Frame owned = owned(frame);
        if (owned.generation != generation) throw new IllegalArgumentException("stale frame generation");
        return owned;
    }

    private Frame owned(Object frame) {
        if (!(frame instanceof Frame)) throw new IllegalArgumentException("expected a frame");
        Frame owned = (Frame) frame;
        if (owned.session != this || owned.value == null) throw new IllegalArgumentException("foreign or released frame");
        return owned;
    }

    private void checkOwner() {
        if (Thread.currentThread() != owner) throw new IllegalStateException("use the creating EGL owner");
    }

    private void checkOpen() {
        checkOwner();
        if (closing) throw new IllegalStateException("frame generation is closing");
    }

    private final class Frame {
        final FrameGenerationSession session = FrameGenerationSession.this;
        final long generation = FrameGenerationSession.this.generation;
        final long sourceId, ptsUs, deadlineNs;
        final boolean inferred;
        Object value;

        Frame(Object value, long sourceId, long ptsUs, boolean inferred, long deadlineNs) {
            this.value = value;
            this.sourceId = sourceId;
            this.ptsUs = ptsUs;
            this.inferred = inferred;
            this.deadlineNs = deadlineNs;
        }
    }
}
