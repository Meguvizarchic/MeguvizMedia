package io.github.jichuo1.meguviz.media;

/** Host policy remains with the player: decoder/Surface ownership, clock
 * translation, audio correction, hook/reflection, and restoration. */
public interface HostBridge {
    final class ClockSnapshot {
        public final long mediaUs, monotonicNs, discontinuity, uncertaintyUs;
        public final double speed;
        public final boolean paused;
        public ClockSnapshot(long mediaUs, long monotonicNs, double speed, boolean paused,
                             long discontinuity, long uncertaintyUs) {
            if (mediaUs < 0 || monotonicNs < 0 || !Double.isFinite(speed) || speed < 0 || (!paused && speed == 0) || uncertaintyUs < 0)
                throw new IllegalArgumentException("Invalid clock snapshot");
            this.mediaUs = mediaUs; this.monotonicNs = monotonicNs; this.speed = speed;
            this.paused = paused; this.discontinuity = discontinuity; this.uncertaintyUs = uncertaintyUs;
        }
    }
    ClockSnapshot clock();
    void onMediaEvent(int kind, long epoch, long sourceId, long frameId, long monotonicNs);
    /** Detailed native events are dispatched by explicit pollEvents on the GL
     * owner, after native locks have been released. Default preserves old API. */
    default void onNativeMediaEvent(MediaEvent event) {
        onMediaEvent(event.kind,event.epoch,event.sourceId,event.contentId,event.timestampNs);
    }
    /** Cumulative native loss; consumer should retain its own last value. */
    default void onNativeEventsDropped(long droppedTotal) {}
    void restoreHostPlayback(int cause);
}
