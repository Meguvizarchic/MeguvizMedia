package io.github.jichuo1.meguviz.media;

import org.junit.Test;
import static org.junit.Assert.*;

public class FrameGenerationSessionTest {
    private static final class Backend implements WindowBackend {
        long submitted, cancelled;
        int draws, releases, closeCalls;
        boolean busyOnClose;
        Completion completion;

        @Override public Object capture(int oes, float[] transform, long sourceId, long ptsUs) { return new Object(); }
        @Override public long submit(Object left, Object right, long deadlineNs) { return ++submitted; }
        @Override public void cancel(long jobId) { cancelled = jobId; }
        @Override public Completion poll() { Completion result = completion; completion = null; return result; }
        @Override public void draw(Object frame, int width, int height) { draws++; }
        @Override public void release(Object frame) { releases++; }
        @Override public void close() {
            closeCalls++;
            if (busyOnClose) throw new Backpressure(-7);
        }
        void complete(int kind) { completion = new Completion(submitted, 0, kind, new Object(), 3); }
    }

    private final FrameGenerationProfile profile = new FrameGenerationProfile(1280, 720, 1, 1, true, 33, 16, 24);
    private final Backend backend = new Backend();
    private long now = 1000;
    private final FrameGenerationSession session = new FrameGenerationSession(
        profile.qualify(FrameGenerationProfile.Backend.QCOM_AHB, 20), backend, () -> now);

    @Test public void lateWorkDoesNotPreventDrawingAnOriginal() {
        Object left = session.capture(1, null, 1, 0);
        Object right = session.capture(1, null, 2, 33);
        assertEquals(0, session.submit(left, right, now + 19));
        long deadline = now + 20;
        assertEquals(1, session.submit(left, right, deadline));
        assertEquals(0, session.submit(left, right, deadline));
        backend.complete(2);
        now = deadline;
        WindowBackend.Completion output = session.poll();
        assertNull(output.frame);
        assertEquals(FrameGenerationSession.DEADLINE_REJECTED, output.status);
        assertThrows(IllegalArgumentException.class, () -> session.draw(right, 1920, 1080));
        session.draw(right, 1280, 720);
        assertEquals(1, backend.draws);
        session.release(left);
        session.release(right);
        assertEquals(3, backend.releases);
        session.close();
    }

    @Test public void scaledQcomWarpUsesSourceTexelsAndKeepsNativeShader() {
        String nativeSize = GlesMotionWarp.warpFragment(1280, 720, 0);
        assertSame(nativeSize, GlesMotionWarp.warpFragment(1280, 720, 1280, 720, 0));
        String scaled = GlesMotionWarp.warpFragment(1280, 720, 1920, 1080, 0);
        assertTrue(scaled.contains("uv * vec2(1280.0, 720.0)"));
        assertTrue(scaled.contains("ivec2(1279, 719)"));
        assertFalse(scaled.contains("ivec2 pixel = ivec2(gl_FragCoord.xy);"));
    }

    @Test public void seekSuppressesOldOutputAndAllowsNewFramesAfterRetirement() {
        Object left = session.capture(1, null, 4, 90);
        Object right = session.capture(1, null, 5, 123);
        assertEquals(1, session.submit(left, right, now + 24));
        session.invalidate();
        assertEquals(1, backend.cancelled);
        assertThrows(IllegalArgumentException.class, () -> session.draw(right, 1280, 720));
        session.release(left);
        session.release(right);

        Object nextLeft = session.capture(1, null, 6, 0);
        Object nextRight = session.capture(1, null, 7, 33);
        assertEquals(0, session.submit(nextLeft, nextRight, now + 24));
        backend.complete(2);
        assertEquals(FrameGenerationSession.CANCELLED, session.poll().status);
        assertEquals(2, session.submit(nextLeft, nextRight, now + 24));
        backend.complete(2);
        Object output = session.poll().frame;
        assertNotNull(output);
        session.draw(output, 1280, 720);
        now += 24;
        assertThrows(WindowBackend.Backpressure.class, () -> session.draw(output, 1280, 720));
        session.release(output);
        session.release(nextLeft);
        session.release(nextRight);
        assertEquals(6, backend.releases);
        session.close();
    }

    @Test public void bypassAndRetirementKeepBackendOwnershipExplicit() {
        Object left = session.capture(1, null, 1, 0);
        Object right = session.capture(1, null, 2, 33);
        assertEquals(1, session.submit(left, right, now + 24));
        backend.complete(4);
        assertNull(session.poll().frame);
        assertThrows(IllegalStateException.class, session::close);
        assertEquals(0, backend.closeCalls);
        session.release(left);
        session.release(right);
        backend.busyOnClose = true;
        assertThrows(WindowBackend.Backpressure.class, session::close);
        assertThrows(IllegalStateException.class, () -> session.capture(1, null, 3, 66));
        backend.busyOnClose = false;
        session.close();
        session.close();
        assertEquals(2, backend.closeCalls);
    }
}
