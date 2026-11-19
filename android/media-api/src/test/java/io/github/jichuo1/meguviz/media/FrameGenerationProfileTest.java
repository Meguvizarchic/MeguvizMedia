package io.github.jichuo1.meguviz.media;

import org.junit.Test;
import static io.github.jichuo1.meguviz.media.AndroidMediaSession.AHB_GPU_SYNC;
import static io.github.jichuo1.meguviz.media.AndroidMediaSession.AHB_HOST_WAIT;
import static io.github.jichuo1.meguviz.media.FrameGenerationProfile.Backend.*;
import static org.junit.Assert.*;

public class FrameGenerationProfileTest {
    private static FrameGenerationProfile profile(int width, int height) {
        return new FrameGenerationProfile(width, height, 1, 1, true, 33_333_333, 16_666_667, 24_000_000);
    }

    @Test public void samePolicyUsesMeasuredCostAtEveryResolution() {
        for (int[] size : new int[][]{{1280, 720}, {1920, 1080}, {3840, 2160}}) {
            FrameGenerationProfile p = profile(size[0], size[1]);
            FrameGenerationProfile.Qualification qcom = p.qualify(QCOM_AHB, 21_000_000);
            assertSame(qcom, p.select(p.qualify(RIFE, 36_000_000), qcom));
            FrameGenerationProfile.Qualification rife = p.qualify(RIFE, 24_000_000);
            assertSame(rife, p.select(rife, qcom));
            assertNull(p.select(p.qualify(RIFE, 24_000_001), null));
        }
    }

    @Test public void rejectsEvidenceFromAnotherPlaybackGeneration() {
        FrameGenerationProfile p = profile(1280, 720);
        FrameGenerationProfile sameSize = profile(1280, 720);
        FrameGenerationProfile newSize = profile(1920, 1080);
        assertNull(sameSize.select(null, p.qualify(QCOM_AHB, 15_000_000)));
        assertNull(newSize.select(p.qualify(RIFE, 15_000_000), null));
        assertNull(p.select(p.qualify(QCOM_AHB, 15_000_000), p.qualify(RIFE, 15_000_000)));
        FrameGenerationProfile scaled = new FrameGenerationProfile(1280, 720, 1920, 1080,
            1, 1, true, 33, 16, 24);
        assertNull(scaled.select(null, p.qualify(QCOM_AHB, 15_000_000)));
        assertNotNull(scaled.select(null, scaled.qualify(QCOM_AHB, 12)));
    }

    @Test public void respectsColorCadenceAndBackendLimits() {
        FrameGenerationProfile hdr = new FrameGenerationProfile(1920, 1080, 9, 9, false, 33, 16, 24);
        assertNull(hdr.select(hdr.qualify(RIFE, 18), hdr.qualify(QCOM_AHB, 12)));
        assertNull(hdr.selectRife(hdr.qualifyRife(AHB_HOST_WAIT, 4, 18)));
        FrameGenerationProfile sameRate = new FrameGenerationProfile(1920, 1080, 1, 1, true, 16, 16, 24);
        assertNull(sameRate.select(null, sameRate.qualify(QCOM_AHB, 12)));
        assertNull(sameRate.selectRife(sameRate.qualifyRife(AHB_GPU_SYNC, 4, 18)));
        FrameGenerationProfile wide = profile(4320, 2160);
        assertNull(wide.select(null, wide.qualify(QCOM_AHB, 12_000_000)));
        FrameGenerationProfile non709 = new FrameGenerationProfile(1280, 720, 6, 6, true, 33, 16, 24);
        assertNull(non709.select(null, non709.qualify(QCOM_AHB, 12)));
        assertSame(RIFE, non709.select(non709.qualify(RIFE, 18), null).backend);
    }

    @Test public void selectsFinestQualifiedRifeScaleWithinBudget() {
        FrameGenerationProfile profile = profile(1920, 1080);
        FrameGenerationProfile.Qualification fine =
            profile.qualifyRife(AHB_HOST_WAIT, 4, 24_000_000);
        FrameGenerationProfile.Qualification medium =
            profile.qualifyRife(AHB_GPU_SYNC, 6, 19_000_000);
        FrameGenerationProfile.Qualification coarse =
            profile.qualifyRife(AHB_HOST_WAIT, 8, 12_000_000);

        assertSame(fine, profile.selectRife(coarse, medium, fine));
        assertSame(medium, profile.selectRife(coarse, medium));
        assertEquals(AHB_GPU_SYNC, medium.rifePath);
        assertEquals(6, medium.flowScale);
    }

    @Test public void selectsLowerCostAtSameRifeScaleAndBreaksTiesWithHostWait() {
        FrameGenerationProfile profile = profile(1280, 720);
        FrameGenerationProfile.Qualification host =
            profile.qualifyRife(AHB_HOST_WAIT, 4, 22_000_000);
        FrameGenerationProfile.Qualification fasterGpu =
            profile.qualifyRife(AHB_GPU_SYNC, 4, 21_000_000);
        FrameGenerationProfile.Qualification tiedGpu =
            profile.qualifyRife(AHB_GPU_SYNC, 4, 22_000_000);

        assertSame(fasterGpu, profile.selectRife(host, fasterGpu));
        assertSame(host, profile.selectRife(tiedGpu, host));
        assertSame(host, profile.selectRife(host, tiedGpu));
    }

    @Test public void selectsQualifiedDownsampleWhenNativeProcessingMissesBudget() {
        FrameGenerationProfile profile = profile(3840, 2160);
        FrameGenerationProfile.Qualification nativeGpu =
            profile.qualifyRife(AHB_GPU_SYNC, 4, 36_000_000);
        FrameGenerationProfile.Qualification nativeHost =
            profile.qualifyRife(AHB_HOST_WAIT, 8, 27_000_000);
        FrameGenerationProfile.Qualification downsampled =
            profile.qualifyRife(1920, 1080, AHB_HOST_WAIT, 4, 19_000_000);

        assertSame(downsampled, profile.selectRife(nativeGpu, downsampled, nativeHost));
        assertEquals(1920, downsampled.rifeWidth);
        assertEquals(1080, downsampled.rifeHeight);
    }

    @Test public void prefersNativeProcessingWhenBothSizesFitBudget() {
        FrameGenerationProfile profile = profile(3840, 2160);
        FrameGenerationProfile.Qualification nativeSize =
            profile.qualifyRife(AHB_HOST_WAIT, 8, 22_000_000);
        FrameGenerationProfile.Qualification downsampled =
            profile.qualifyRife(1920, 1080, AHB_GPU_SYNC, 4, 14_000_000);

        assertSame(nativeSize, profile.selectRife(downsampled, nativeSize));
        assertSame(nativeSize, profile.selectRife(nativeSize, downsampled));
        assertEquals(3840, nativeSize.rifeWidth);
        assertEquals(2160, nativeSize.rifeHeight);
    }

    @Test public void ignoresStaleLegacyAndOutOfBudgetRifeQualifications() {
        FrameGenerationProfile profile = profile(1920, 1080);
        FrameGenerationProfile.Qualification stale =
            profile(1920, 1080).qualifyRife(AHB_GPU_SYNC, 4, 12_000_000);
        FrameGenerationProfile.Qualification legacy = profile.qualify(RIFE, 12_000_000);
        FrameGenerationProfile.Qualification qcom = profile.qualify(QCOM_AHB, 12_000_000);
        FrameGenerationProfile.Qualification overBudget =
            profile.qualifyRife(AHB_GPU_SYNC, 4, 24_000_001);
        FrameGenerationProfile.Qualification fallback =
            profile.qualifyRife(AHB_HOST_WAIT, 8, 18_000_000);

        assertEquals(0, legacy.rifePath);
        assertEquals(0, legacy.flowScale);
        assertSame(legacy, profile.select(legacy, qcom));
        assertNull(profile.selectRife(stale, legacy, qcom, overBudget, null));
        assertSame(fallback, profile.selectRife(stale, legacy, qcom, overBudget, fallback));
    }

    @Test public void rejectsInvalidRifeQualificationParameters() {
        FrameGenerationProfile profile = profile(1280, 720);
        assertThrows(IllegalArgumentException.class, () -> profile.qualifyRife(0, 4, 20_000_000));
        assertThrows(IllegalArgumentException.class, () -> profile.qualifyRife(AHB_GPU_SYNC, 0, 20_000_000));
        assertThrows(IllegalArgumentException.class, () -> profile.qualifyRife(AHB_HOST_WAIT, 5, 20_000_000));
        assertThrows(IllegalArgumentException.class, () -> profile.qualifyRife(AHB_HOST_WAIT, 8, 0));
        assertThrows(IllegalArgumentException.class,
            () -> profile.qualifyRife(1281, 720, AHB_HOST_WAIT, 4, 20_000_000));
        assertThrows(IllegalArgumentException.class,
            () -> profile.qualifyRife(1280, 721, AHB_GPU_SYNC, 4, 20_000_000));
        assertThrows(IllegalArgumentException.class,
            () -> profile.qualifyRife(1280, 0, AHB_HOST_WAIT, 4, 20_000_000));
    }

    @Test public void openingKeepsLegacyAndExactRifeQualificationsSeparate() {
        FrameGenerationProfile profile = profile(1280, 720);
        FrameGenerationProfile.Qualification legacy = profile.qualify(RIFE, 20_000_000);
        FrameGenerationProfile.Qualification exact =
            profile.qualifyRife(AHB_HOST_WAIT, 4, 20_000_000);

        assertNull(FrameGenerationSession.open(profile, legacy, null, 4, null));
        assertThrows(IllegalArgumentException.class,
            () -> FrameGenerationSession.open(profile, exact, null, 4, null));
        assertThrows(IllegalArgumentException.class,
            () -> FrameGenerationSession.openQualified(profile, legacy, null, null));
        assertNull(FrameGenerationSession.openQualified(profile, exact, null, null));
    }

    private static long[] supportedAhbProbe() {
        return new long[]{0, 1280, 720, 1344, 1, 0x300, 0, 0, 0, 0, 0, 0, 0};
    }

    @Test public void failsClosedForIncompleteOrMismatchedAhbProbe() {
        assertFalse(AndroidMediaSession.validAhbProbe(null, 1280, 720, AHB_HOST_WAIT));
        assertFalse(AndroidMediaSession.validAhbProbe(new long[12], 1280, 720, AHB_GPU_SYNC));
        assertFalse(AndroidMediaSession.validAhbProbe(new long[13], 1280, 720, AHB_GPU_SYNC));
        assertFalse(AndroidMediaSession.validAhbProbe(supportedAhbProbe(), 1920, 1080, AHB_HOST_WAIT));
        assertFalse(AndroidMediaSession.validAhbProbe(supportedAhbProbe(), 1280, 360, AHB_HOST_WAIT));
        assertFalse(AndroidMediaSession.validAhbProbe(supportedAhbProbe(), 1280, 720, 0));

        long[] badStride = supportedAhbProbe();
        badStride[3] = 1279;
        assertFalse(AndroidMediaSession.validAhbProbe(badStride, 1280, 720, AHB_HOST_WAIT));
        for (int index : new int[]{0, 6, 7, 8, 9}) {
            long[] failed = supportedAhbProbe();
            failed[index] = -3;
            assertFalse(AndroidMediaSession.validAhbProbe(failed, 1280, 720, AHB_HOST_WAIT));
            assertFalse(AndroidMediaSession.validAhbProbe(failed, 1280, 720, AHB_GPU_SYNC));
        }
    }

    @Test public void gpuSyncRequiresBothFenceDirectionsButHostWaitRemainsUsable() {
        assertTrue(AndroidMediaSession.validAhbProbe(supportedAhbProbe(), 1280, 720, AHB_HOST_WAIT));
        assertTrue(AndroidMediaSession.validAhbProbe(supportedAhbProbe(), 1280, 720, AHB_GPU_SYNC));

        for (int index : new int[]{10, 11}) {
            long[] missingFence = supportedAhbProbe();
            missingFence[index] = -3;
            assertTrue(AndroidMediaSession.validAhbProbe(missingFence, 1280, 720, AHB_HOST_WAIT));
            assertFalse(AndroidMediaSession.validAhbProbe(missingFence, 1280, 720, AHB_GPU_SYNC));
        }
    }
}
