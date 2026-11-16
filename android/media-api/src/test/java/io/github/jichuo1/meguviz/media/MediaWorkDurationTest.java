package io.github.jichuo1.meguviz.media;

import org.junit.Test;
import static org.junit.Assert.*;

public class MediaWorkDurationTest {
    private static long[] gpu(long observed, long elapsed, long batches, long observedBatches,
                              long validBits, long periodBits) {
        return new long[]{observed, elapsed, batches, observedBatches, validBits, periodBits, 0};
    }

    private static long[] validGpu() { return gpu(1, 300, 2, 2, 63, Float.floatToIntBits(1.0f)); }

    @Test public void completeMeasurementsKeepIndependentCpuGpuAndPeriod() {
        MediaWorkDuration value = MediaWorkDuration.create(100, 1_000, 700, validGpu(), 44);
        assertNotNull(value);
        assertEquals(100, value.periodStartUptimeNanos);
        assertEquals(1_000, value.totalDurationNanos);
        assertEquals(700, value.cpuDurationNanos);
        assertEquals(300, value.gpuDurationNanos);
        assertEquals(44, value.workerThreadId);
    }

    @Test public void overlappingCpuAndGpuAreNotSummedOrRejected() {
        long[] gpu = gpu(1, 700, 1, 1, 1, Float.floatToIntBits(1.0f));
        MediaWorkDuration value = MediaWorkDuration.create(100, 1_000, 700, gpu, 44);
        assertNotNull("700 CPU + 700 GPU may overlap inside a 1000 wall period", value);
    }

    @Test public void invalidPeriodCpuAndWorkerAreRejected() {
        assertNull(MediaWorkDuration.create(0, 1_000, 1, validGpu(), 44));
        assertNull(MediaWorkDuration.create(100, 0, 1, validGpu(), 44));
        assertNull(MediaWorkDuration.create(100, 1_000, -1, validGpu(), 44));
        assertNull(MediaWorkDuration.create(100, 1_000, 1_001, validGpu(), 44));
        assertNull(MediaWorkDuration.create(100, 1_000, 1, validGpu(), 0));
        assertNull(MediaWorkDuration.from(null));
    }

    @Test public void partialOrInvalidGpuMeasurementsAreRejected() {
        assertNull(MediaWorkDuration.create(100, 1_000, 1, null, 44));
        assertNull(MediaWorkDuration.create(100, 1_000, 1, new long[6], 44));
        assertNull(MediaWorkDuration.create(100, 1_000, 1, gpu(0, 300, 2, 2, 63, Float.floatToIntBits(1)), 44));
        assertNull(MediaWorkDuration.create(100, 1_000, 1, gpu(1, 0, 2, 2, 63, Float.floatToIntBits(1)), 44));
        assertNull(MediaWorkDuration.create(100, 1_000, 1, gpu(1, 1_001, 2, 2, 63, Float.floatToIntBits(1)), 44));
        assertNull(MediaWorkDuration.create(100, 1_000, 1, gpu(1, 300, 0, 0, 63, Float.floatToIntBits(1)), 44));
        assertNull(MediaWorkDuration.create(100, 1_000, 1, gpu(1, 300, 2, 1, 63, Float.floatToIntBits(1)), 44));
        assertNull(MediaWorkDuration.create(100, 1_000, 1, gpu(1, 300, 2, 2, 0, Float.floatToIntBits(1)), 44));
        assertNull(MediaWorkDuration.create(100, 1_000, 1, gpu(1, 300, 2, 2, 65, Float.floatToIntBits(1)), 44));
        assertNull(MediaWorkDuration.create(100, 1_000, 1, gpu(1, 300, 2, 2, 63, Float.floatToIntBits(Float.NaN)), 44));
        assertNull(MediaWorkDuration.create(100, 1_000, 1, gpu(1, 300, 2, 2, 63, 0), 44));
    }
}
