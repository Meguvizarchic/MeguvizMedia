package io.github.jichuo1.meguviz.media.player;

/** Holds the actual producer objects until the decoder gate confirms cleanup. */
final class DecoderResources<C, E, S, T> {
    interface Action<V> { void run(V value); }

    final T texture;
    final S surface;
    C codec;
    E extractor;

    DecoderResources(T texture, S surface) {
        this.texture = texture;
        this.surface = surface;
    }

    boolean release(Action<C> stopCodec, Action<C> releaseCodec,
                    Action<E> releaseExtractor, Action<S> releaseSurface) {
        boolean released = true;
        boolean codecReleaseReturned = codec == null;
        if (codec != null) {
            try { stopCodec.run(codec); } catch (RuntimeException ignored) {}
            try { releaseCodec.run(codec); codecReleaseReturned = true; }
            catch (RuntimeException e) { released = false; }
        }
        if (extractor != null) {
            try { releaseExtractor.run(extractor); } catch (RuntimeException e) { released = false; }
        }
        // A throwing codec release leaves ownership unknown. Keep the Surface
        // and its consumer reachable through this owner instead of releasing it.
        if (codecReleaseReturned) {
            try { releaseSurface.run(surface); } catch (RuntimeException e) { released = false; }
        }
        return released && codecReleaseReturned;
    }
}
