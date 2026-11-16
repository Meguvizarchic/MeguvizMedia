package io.github.jichuo1.meguviz.media.player;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/** Host test of bounded retries; does not simulate Android scheduling. */
public final class DeferredRetryTest {
    private static void require(boolean value, String message) { if (!value) throw new AssertionError(message); }
    public static void main(String[] args) {
        DeferredRetry retry = new DeferredRetry(); List<Runnable> posted = new ArrayList<>();
        DeferredRetry.Poster poster = (action, delay) -> { require(delay == 250, "incorrect delay"); posted.add(action); return true; };
        AtomicBoolean active = new AtomicBoolean(true); AtomicInteger calls = new AtomicInteger();
        for (int i = 0; i < 100; i++) retry.schedule(poster, active::get, calls::incrementAndGet);
        require(posted.size() == 1 && calls.get() == 0, "retry queue was not bounded");
        posted.remove(0).run(); require(calls.get() == 1, "active retry did not fire");
        retry.schedule(poster, active::get, calls::incrementAndGet); active.set(false);
        posted.remove(0).run(); require(calls.get() == 1, "inactive retry fired");
        active.set(true); retry.schedule((action, delay) -> false, active::get, calls::incrementAndGet);
        retry.schedule(poster, active::get, calls::incrementAndGet);
        require(posted.size() == 1, "failed post blocked future retry");
        posted.remove(0).run(); require(calls.get() == 2, "retry after failed post did not fire");
        System.out.println("PASS bounded delayed retry");
    }
}
