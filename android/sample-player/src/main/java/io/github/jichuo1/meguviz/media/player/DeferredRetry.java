package io.github.jichuo1.meguviz.media.player;

import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;

/** Allows one delayed retry; inactive clients do not run the action. */
final class DeferredRetry {
    interface Poster { boolean post(Runnable action, long delayMillis); }
    private final AtomicBoolean scheduled = new AtomicBoolean();

    void schedule(Poster poster, BooleanSupplier active, Runnable action) {
        if (!scheduled.compareAndSet(false, true)) return;
        try {
            if (!poster.post(() -> {
                scheduled.set(false);
                if (active.getAsBoolean()) action.run();
            }, 250)) scheduled.set(false);
        } catch (RuntimeException | Error e) { scheduled.set(false); throw e; }
    }
}
