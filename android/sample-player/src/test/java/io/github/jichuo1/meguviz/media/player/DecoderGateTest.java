package io.github.jichuo1.meguviz.media.player;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.lang.ref.WeakReference;

/** Standalone host test: no Android runtime or mocked MediaCodec involved. */
public final class DecoderGateTest {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    public static void main(String[] args) throws Exception {
        DecoderGate gate = new DecoderGate();
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        AtomicBoolean cancelled = new AtomicBoolean();
        long first = gate.cancel();
        Object owner = new Object(); WeakReference<Object> weakOwner = new WeakReference<>(owner);
        require(gate.start(first, owner, token -> {
            entered.countDown();
            try { release.await(); cancelled.set(token.get()); }
            catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            return true;
        }), "first decoder did not start");
        owner = null;
        require(entered.await(5, TimeUnit.SECONDS), "first decoder did not enter");
        System.gc();
        require(weakOwner.get() != null, "surface consumer was not retained while decoder was live");
        long second = gate.cancel();
        require(!gate.awaitStopped(10), "busy decoder reported stopped");
        require(!gate.ready(second), "busy surface reported ready");
        require(!gate.start(second, new Object(), token -> true), "second decoder overlapped the first");
        release.countDown();
        require(gate.awaitStopped(5000), "first decoder did not stop");
        require(cancelled.get(), "cancellation was not delivered");
        require(!gate.start(first, new Object(), token -> true), "stale start was accepted");
        CountDownLatch secondStarted = new CountDownLatch(1);
        require(gate.start(second, new Object(), token -> { secondStarted.countDown(); return true; }), "fresh start rejected");
        require(secondStarted.await(5, TimeUnit.SECONDS), "fresh decoder did not enter");
        require(gate.awaitStopped(5000), "fresh decoder did not stop");
        long third = gate.cancel();
        long fourth = gate.cancel();
        require(!gate.ready(third) && !gate.start(third, new Object(), token -> true), "late queued start accepted");
        require(gate.ready(fourth), "latest generation not ready");
        require(gate.start(fourth, new Object(), token -> false), "failure case did not start");
        require(!gate.awaitStopped(5000), "unconfirmed release reported safe");
        long fifth = gate.cancel();
        require(!gate.ready(fifth) && !gate.start(fifth, new Object(), token -> true), "unconfirmed release allowed another decoder");
        System.out.println("PASS decoder handoff gate");
    }
}
