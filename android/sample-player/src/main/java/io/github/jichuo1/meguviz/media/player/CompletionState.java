package io.github.jichuo1.meguviz.media.player;

/** GL-thread-owned accounting; a lost device stays failed for this renderer. */
final class CompletionState {
    static final int DEVICE_LOST = -11;
    private int pending;
    private boolean deviceLost;

    void accepted() {
        if (deviceLost || pending == Integer.MAX_VALUE) throw new IllegalStateException("invalid completion state");
        ++pending;
    }
    void completed(long jobId, int status) {
        if (jobId != 0 && pending > 0) --pending;
        failed(status);
    }
    void failed(int status) { if (status == DEVICE_LOST) deviceLost = true; }
    void newEpoch() { pending = 0; }
    boolean needsPoll() { return pending > 0 && !deviceLost; }
    boolean deviceLost() { return deviceLost; }
}
