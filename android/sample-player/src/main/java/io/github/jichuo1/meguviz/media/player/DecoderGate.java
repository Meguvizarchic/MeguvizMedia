package io.github.jichuo1.meguviz.media.player;

import java.util.concurrent.atomic.AtomicBoolean;

/** Serializes codec ownership of a Surface and invalidates delayed starts. */
final class DecoderGate {
    interface Decode { boolean run(AtomicBoolean cancelled); }
    private final Object lock = new Object();
    private long generation;
    private Thread worker;
    private AtomicBoolean cancelled = new AtomicBoolean(true);
    private boolean releaseUnconfirmed;
    private Object producerOwner;
    private Object unstartedSurface;
    private Object unstartedTexture;

    private void reapStopped() {
        if (worker != null && !worker.isAlive() && !releaseUnconfirmed) {
            worker = null;
            producerOwner = null;
        }
    }

    long cancel() {
        synchronized (lock) {
            cancelled.set(true);
            return ++generation;
        }
    }

    boolean awaitStopped(long timeoutMillis) {
        Thread previous;
        synchronized (lock) { previous = worker; }
        if (previous != null && previous != Thread.currentThread()) {
            try { previous.join(timeoutMillis); }
            catch (InterruptedException e) { Thread.currentThread().interrupt(); return false; }
        }
        synchronized (lock) { reapStopped(); return !releaseUnconfirmed && worker == null; }
    }

    boolean ready(long expectedGeneration) {
        synchronized (lock) {
            reapStopped();
            return !releaseUnconfirmed && generation == expectedGeneration && worker == null;
        }
    }

    boolean current(long expectedGeneration) { synchronized (lock) { return generation == expectedGeneration; } }
    boolean releaseUnconfirmed() { synchronized (lock) { return releaseUnconfirmed; } }

    // A newly created Surface was never handed to a decoder, but its release
    // failed. Keep the actual objects and refuse subsequent producer starts.
    void retainUnstartedSurface(Object surface, Object texture) {
        synchronized (lock) {
            if (unstartedSurface != null) throw new IllegalStateException("unstarted Surface already retained");
            unstartedSurface = surface;
            unstartedTexture = texture;
            releaseUnconfirmed = true;
        }
    }

    boolean start(long expectedGeneration, Object resourcesOwner, Decode decode) {
        synchronized (lock) {
            reapStopped();
            if (resourcesOwner == null || releaseUnconfirmed || generation != expectedGeneration || worker != null) return false;
            AtomicBoolean token = new AtomicBoolean(false);
            Thread next = new Thread(() -> {
                boolean released = false;
                try { released = decode.run(token); }
                finally { if (!released) synchronized (lock) { releaseUnconfirmed = true; } }
            }, "MediaCodec-source");
            cancelled = token;
            worker = next;
            // Keep the actual producer and its weakly referenced consumer alive.
            producerOwner = resourcesOwner;
            try { next.start(); }
            catch (RuntimeException | Error e) { worker = null; producerOwner = null; token.set(true); throw e; }
            return true;
        }
    }
}
