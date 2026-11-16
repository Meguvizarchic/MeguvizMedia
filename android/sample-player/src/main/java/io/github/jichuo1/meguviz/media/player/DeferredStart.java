package io.github.jichuo1.meguviz.media.player;

import java.util.concurrent.atomic.AtomicReference;

/** One pending source, consumed only by a current-context render callback. */
final class DeferredStart<T> {
    static final class Request<T> {
        final T source; final long generation;
        Request(T source, long generation) { this.source = source; this.generation = generation; }
    }
    private final AtomicReference<Request<T>> pending = new AtomicReference<>();

    void offer(T source, long generation) { pending.set(new Request<>(source, generation)); }
    void clear() { pending.set(null); }
    Request<T> peek() { return pending.get(); }
    boolean discard(Request<T> request) { return pending.compareAndSet(request, null); }
    Request<T> takeIfReady(boolean contextCurrent, DecoderGate gate) {
        Request<T> request = pending.get();
        if (request == null) return null;
        if (!gate.current(request.generation)) { discard(request); return null; }
        if (!contextCurrent || !gate.ready(request.generation)) return null;
        return pending.compareAndSet(request, null) ? request : null;
    }
}
