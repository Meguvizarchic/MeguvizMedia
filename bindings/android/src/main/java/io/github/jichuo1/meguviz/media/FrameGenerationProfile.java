package io.github.jichuo1.meguviz.media;

/** Source and display configuration for one playback generation on one EGL/device owner. */
public final class FrameGenerationProfile {
    public enum Backend { RIFE, QCOM_AHB }

    public final int width, height, outputWidth, outputHeight, primaries, matrix;
    public final long sourcePeriodNs, displayPeriodNs, midpointBudgetNs;
    public final boolean sdr8Bit;

    public FrameGenerationProfile(int width, int height, int primaries, int matrix,
                                  boolean sdr8Bit, long sourcePeriodNs, long displayPeriodNs,
                                  long midpointBudgetNs) {
        this(width, height, width, height, primaries, matrix, sdr8Bit, sourcePeriodNs,
            displayPeriodNs, midpointBudgetNs);
    }

    public FrameGenerationProfile(int width, int height, int outputWidth, int outputHeight,
                                  int primaries, int matrix, boolean sdr8Bit, long sourcePeriodNs,
                                  long displayPeriodNs, long midpointBudgetNs) {
        if (width <= 0 || height <= 0 || outputWidth <= 0 || outputHeight <= 0
                || sourcePeriodNs <= 0 || displayPeriodNs <= 0 || midpointBudgetNs <= 0) {
            throw new IllegalArgumentException("invalid frame generation profile");
        }
        this.width = width;
        this.height = height;
        this.outputWidth = outputWidth;
        this.outputHeight = outputHeight;
        this.primaries = primaries;
        this.matrix = matrix;
        this.sdr8Bit = sdr8Bit;
        this.sourcePeriodNs = sourcePeriodNs;
        this.displayPeriodNs = displayPeriodNs;
        this.midpointBudgetNs = midpointBudgetNs;
    }

    /** Record measured pair-to-draw cost after device preflight and visual qualification.
     * Carry this token only with the exact prepared backend and playback profile measured.
     * A format, cadence or device generation change requires a new profile and qualification. */
    public Qualification qualify(Backend backend, long pairToDrawP95Ns) {
        if (backend == null || pairToDrawP95Ns <= 0) {
            throw new IllegalArgumentException("invalid frame generation qualification");
        }
        return new Qualification(this, backend, pairToDrawP95Ns, 0, 0, 0, 0);
    }

    /** Qualifies an exact RIFE configuration at the source frame size. */
    public Qualification qualifyRife(int syncPath, int flowScale, long pairToDrawP95Ns) {
        return qualifyRife(width, height, syncPath, flowScale, pairToDrawP95Ns);
    }

    /** Qualifies RIFE at the measured processing size, which may be smaller than the source. */
    public Qualification qualifyRife(int processingWidth, int processingHeight, int syncPath,
                                      int flowScale, long pairToDrawP95Ns) {
        if (processingWidth <= 0 || processingWidth > width || processingHeight <= 0 || processingHeight > height
                || (syncPath != AndroidMediaSession.AHB_HOST_WAIT && syncPath != AndroidMediaSession.AHB_GPU_SYNC)
                || (flowScale != 4 && flowScale != 6 && flowScale != 8) || pairToDrawP95Ns <= 0) {
            throw new IllegalArgumentException("invalid RIFE qualification");
        }
        return new Qualification(this, Backend.RIFE, pairToDrawP95Ns, processingWidth, processingHeight,
            syncPath, flowScale);
    }

    /** Choose among already validated configurations within the budget. Prefer the largest processing
     * extent by pixel count, then finer flow, lower latency, and host wait at equal cost. */
    public Qualification selectRife(Qualification... variants) {
        Qualification best = null;
        for (Qualification variant : variants) {
            if (usable(variant, Backend.RIFE) && variant.rifePath != 0
                    && (best == null || prefersRife(variant, best))) best = variant;
        }
        return best;
    }

    private static boolean prefersRife(Qualification candidate, Qualification best) {
        long pixels = (long) candidate.rifeWidth * candidate.rifeHeight;
        long bestPixels = (long) best.rifeWidth * best.rifeHeight;
        if (pixels != bestPixels) return pixels > bestPixels;
        if (candidate.flowScale != best.flowScale) return candidate.flowScale < best.flowScale;
        if (candidate.pairToDrawP95Ns != best.pairToDrawP95Ns)
            return candidate.pairToDrawP95Ns < best.pairToDrawP95Ns;
        return candidate.rifePath == AndroidMediaSession.AHB_HOST_WAIT
            && best.rifePath != AndroidMediaSession.AHB_HOST_WAIT;
    }

    /** RIFE is preferred when both qualified paths meet the playback budget. */
    public Qualification select(Qualification rife, Qualification qcom) {
        if (usable(rife, Backend.RIFE)) return rife;
        if (usable(qcom, Backend.QCOM_AHB)) return qcom;
        return null;
    }

    private boolean usable(Qualification candidate, Backend backend) {
        if (candidate == null || candidate.profile != this || candidate.backend != backend
                || candidate.pairToDrawP95Ns > midpointBudgetNs || !sdr8Bit
                || displayPeriodNs >= sourcePeriodNs || outputWidth > 8192 || outputHeight > 8192) {
            return false;
        }
        if (backend == Backend.RIFE) return width <= 8192 && height <= 8192;
        // The compact luma shader and renderer have only been qualified for BT.709 SDR.
        return primaries == 1 && matrix == 1 && width <= 4096 && height <= 4096
            && (long) width * height <= 4096L * 2160L;
    }

    public static final class Qualification {
        final FrameGenerationProfile profile;
        public final Backend backend;
        public final long pairToDrawP95Ns;
        /** Exact RIFE processing geometry and options. Zero for legacy and QCOM qualifications. */
        public final int rifeWidth, rifeHeight, rifePath, flowScale;

        private Qualification(FrameGenerationProfile profile, Backend backend, long pairToDrawP95Ns,
                              int rifeWidth, int rifeHeight, int rifePath, int flowScale) {
            this.profile = profile;
            this.backend = backend;
            this.pairToDrawP95Ns = pairToDrawP95Ns;
            this.rifeWidth = rifeWidth;
            this.rifeHeight = rifeHeight;
            this.rifePath = rifePath;
            this.flowScale = flowScale;
        }

        boolean fits(long nowNs, long deadlineNs) {
            return deadlineNs - nowNs >= pairToDrawP95Ns;
        }
    }
}
