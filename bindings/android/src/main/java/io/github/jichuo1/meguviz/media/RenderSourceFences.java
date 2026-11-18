package io.github.jichuo1.meguviz.media;

/** Capture owner retains these receipts until a shared texture is actually reusable. */
final class RenderSourceFences {
    interface Api {
        long fenceAndFlush();
        boolean signaled(long fence);
        void delete(long fence);
    }

    private long ready, lastRead;
    private final Api api;

    RenderSourceFences(Api api) { this.api = api; }

    void publish() {
        if (ready != 0 || lastRead != 0) throw new IllegalStateException("source fence was not retired");
        ready = api.fenceAndFlush();
        if (ready == 0) throw new IllegalStateException("missing producer fence");
    }

    boolean readyForRender() { return ready != 0 && api.signaled(ready); }

    /** Applied on the capture owner in the renderer's FIFO receipt order. */
    void acceptRead(long next) {
        if (next == 0 || ready == 0) throw new IllegalArgumentException("missing renderer or source fence");
        long previous = lastRead;
        lastRead = next;
        if (previous != 0) api.delete(previous);
    }

    /** Only call when the scheduler and Vulkan worker have returned all logical references. */
    boolean retireForReuse() {
        // Never enqueue a render dependency into the capture command stream.
        if (lastRead != 0 && !api.signaled(lastRead)) return false;
        if (ready != 0 && !api.signaled(ready)) return false;
        discardAfterRendererStopped();
        return true;
    }

    void discardAfterRendererStopped() {
        if (lastRead != 0) api.delete(lastRead);
        if (ready != 0) api.delete(ready);
        ready = lastRead = 0;
    }
}
