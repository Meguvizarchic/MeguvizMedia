package io.github.jichuo1.meguviz.media;

/** Decoder thread exit alone does not acknowledge native Surface producer retirement. */
final class CodecRetirement {
    private volatile boolean released;

    @FunctionalInterface private interface Release extends AutoCloseable {
        @Override void close();
    }

    void stopAndRelease(Runnable stop, Runnable release) {
        try (Release confirm = () -> {
            if (release != null) release.run();
            released = true;
        }) {
            if (stop != null) stop.run();
        }
    }

    boolean proven() { return released; }
}
