package io.github.jichuo1.meguviz.media;

import org.junit.Test;
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
        FrameGenerationProfile sameRate = new FrameGenerationProfile(1920, 1080, 1, 1, true, 16, 16, 24);
        assertNull(sameRate.select(null, sameRate.qualify(QCOM_AHB, 12)));
        FrameGenerationProfile wide = profile(4320, 2160);
        assertNull(wide.select(null, wide.qualify(QCOM_AHB, 12_000_000)));
        FrameGenerationProfile non709 = new FrameGenerationProfile(1280, 720, 6, 6, true, 33, 16, 24);
        assertNull(non709.select(null, non709.qualify(QCOM_AHB, 12)));
        assertSame(RIFE, non709.select(non709.qualify(RIFE, 18), null).backend);
    }
}
