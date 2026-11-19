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
        return new Qualification(this, backend, pairToDrawP95Ns);
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

        private Qualification(FrameGenerationProfile profile, Backend backend, long pairToDrawP95Ns) {
            this.profile = profile;
            this.backend = backend;
            this.pairToDrawP95Ns = pairToDrawP95Ns;
        }

        boolean fits(long nowNs, long deadlineNs) {
            return deadlineNs - nowNs >= pairToDrawP95Ns;
        }
    }
}
