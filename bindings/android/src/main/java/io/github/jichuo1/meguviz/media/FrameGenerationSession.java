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
    private static final int SUBMIT_IN_FLIGHT = -1, SUBMIT_DEADLINE = -2, SUBMIT_INPUT_NOT_READY = -3;

    public enum Decision { ACCEPTED, IN_FLIGHT, DEADLINE, INPUT_NOT_READY, BACKEND_REJECTED }

    public static final class Submission {
        private static final Submission IN_FLIGHT = new Submission(Decision.IN_FLIGHT, 0);
        private static final Submission DEADLINE = new Submission(Decision.DEADLINE, 0);
        private static final Submission INPUT_NOT_READY = new Submission(Decision.INPUT_NOT_READY, 0);
        private static final Submission BACKEND_REJECTED = new Submission(Decision.BACKEND_REJECTED, 0);

        public final Decision decision;
        /** Positive only for accepted work, which owns a slot until its terminal receipt. */
        public final long jobId;

        private Submission(Decision decision, long jobId) {
            this.decision = decision;
            this.jobId = jobId;
        }
    }

    private final Thread owner = Thread.currentThread();
    private final FrameGenerationProfile.Qualification qualification;
    private final WindowBackend backend;
    private final LongSupplier clock;
    private long generation = 1;
    private long jobId, jobDeadlineNs;
    private int discardStatus, ownedFrames;
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
        if (rife != null && rife.rifePath != 0)
            throw new IllegalArgumentException("use openQualified for an exact RIFE configuration");
        FrameGenerationProfile.Qualification choice = profile.select(rife, qcom);
        if (choice != null && choice.backend == FrameGenerationProfile.Backend.RIFE) {
            if (flowScale != 0 && flowScale != 4 && flowScale != 6 && flowScale != 8)
                throw new IllegalArgumentException("invalid RIFE flow scale");
            AndroidMediaSession session = preparedRife == null ? null : preparedRife.get();
            if (session != null) return new FrameGenerationSession(choice,
                new RifeWindowSession(session, profile, flowScale), System::nanoTime);
            choice = profile.select(null, qcom);
        }
        return openQcom(profile, choice);
    }

    /**
     * Opens the chosen exact RIFE configuration on the session's EGL owner. Call probe() on the
     * prepared RIFE session once before this call. The session transfers ownership only when the
     * returned backend is RIFE. With no prepared RIFE session, try qualified QCOM, then original.
     */
    public static FrameGenerationSession openQualified(FrameGenerationProfile profile,
            FrameGenerationProfile.Qualification rife, AndroidMediaSession preparedRife,
            FrameGenerationProfile.Qualification qcom) {
        if (rife != null && rife.rifePath == 0)
            throw new IllegalArgumentException("exact RIFE qualification required");
        FrameGenerationProfile.Qualification choice = profile.selectRife(rife);
        if (choice != null && preparedRife != null) {
            if (!preparedRife.matchesRifeQualification(choice))
                throw new IllegalArgumentException("prepared RIFE session does not match its qualified AHB configuration");
            return new FrameGenerationSession(choice,
                new RifeWindowSession(preparedRife, profile, choice.flowScale), System::nanoTime);
        }
        return openQcom(profile, profile.select(null, qcom));
    }

    private static FrameGenerationSession openQcom(FrameGenerationProfile profile,
                                                    FrameGenerationProfile.Qualification choice) {
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
        return Math.max(0, admit(left, right, deadlineNs, false));
    }

    /** Nonblocking admission for hosts that want to distinguish a missed budget from backend pressure.
     * A rejected pair transfers no resources. The host can keep presenting original frames. */
    public Submission trySubmit(Object left, Object right, long deadlineNs) {
        long result = admit(left, right, deadlineNs, true);
        if (result > 0) return new Submission(Decision.ACCEPTED, result);
        switch ((int) result) {
            case SUBMIT_IN_FLIGHT: return Submission.IN_FLIGHT;
            case SUBMIT_DEADLINE: return Submission.DEADLINE;
            case SUBMIT_INPUT_NOT_READY: return Submission.INPUT_NOT_READY;
            default: return Submission.BACKEND_REJECTED;
        }
    }

    private long admit(Object left, Object right, long deadlineNs, boolean explainRejection) {
        checkOpen();
        Frame first = source(left), second = source(right);
        if (first.sourceId >= second.sourceId || first.ptsUs >= second.ptsUs)
            throw new IllegalArgumentException("invalid source pair");
        if (jobId != 0) return SUBMIT_IN_FLIGHT;
        if (!qualification.fits(clock.getAsLong(), deadlineNs)) return SUBMIT_DEADLINE;
        long accepted = backend.submit(first.value, second.value, deadlineNs);
        if (accepted > 0) {
            jobId = accepted;
            jobDeadlineNs = deadlineNs;
            discardStatus = 0;
            return accepted;
        }
        return explainRejection && !backend.inputsReady(first.value, second.value) ? SUBMIT_INPUT_NOT_READY : 0;
    }

    /**
     * Call on seek or discontinuity. Existing frames remain releasable. The accepted
     * job keeps its retirement slot until the backend returns a terminal receipt.
     * Open a fresh session for resolution, color, cadence or device changes.
     */
    public void invalidate() {
        checkOpen();
        ++generation;
        if (jobId != 0) discardActive(CANCELLED);
    }

    @Override public void cancel(long id) {
        checkOpen();
        if (id > 0 && id == jobId) discardActive(CANCELLED);
    }

    private void discardActive(int status) {
        boolean notifyBackend = discardStatus == 0;
        discardStatus = status;
        if (notifyBackend) backend.cancel(jobId);
    }

    @Override public Completion poll() {
        checkOpen();
        Completion result = backend.poll();
        if (result == null) {
            if (jobId != 0 && discardStatus == 0 && jobDeadlineNs - clock.getAsLong() <= 0)
                discardActive(DEADLINE_REJECTED);
            return null;
        }
        if (jobId == 0 || result.jobId != jobId) {
            if (result.frame != null) backend.release(result.frame);
            throw new IllegalStateException("unexpected midpoint completion " + result.jobId);
        }
        jobId = 0;
        int status = result.status;
        if (discardStatus != 0 && (status == 0 || status == CANCELLED || status == DEADLINE_REJECTED))
            status = discardStatus;
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
            if (jobId != 0) discardActive(CANCELLED);
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
