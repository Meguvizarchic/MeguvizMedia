package io.github.jichuo1.meguviz.media.player;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.ArrayList;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/** Host test of request and codec ownership ordering, not Android lifecycle proof. */
public final class DeferredStartTest {
    private static void require(boolean value, String message) { if (!value) throw new AssertionError(message); }
    public static void main(String[] args) throws Exception {
        DecoderGate gate = new DecoderGate(); DeferredStart<String> pending = new DeferredStart<>();
        long generation = gate.cancel(); pending.offer("selection-before-resume", generation);
        require(pending.takeIfReady(false, gate) == null && pending.peek() != null, "lost selection without current context");
        DeferredStart.Request<String> ready = pending.takeIfReady(true, gate);
        require(ready != null && ready.source.equals("selection-before-resume") && pending.peek() == null, "resumed selection missing");
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        require(gate.start(generation, new Object(), token -> {
            entered.countDown(); try { release.await(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            return true;
        }), "decoder not started");
        require(entered.await(5, TimeUnit.SECONDS), "decoder not entered");
        long next = gate.cancel(); pending.offer("wait-for-old-decoder", next);
        require(pending.takeIfReady(true, gate) == null && pending.peek() != null, "busy source was consumed");
        DeferredRetry retireRetry = new DeferredRetry(); ArrayList<Runnable> posted = new ArrayList<>();
        DeferredRetry.Poster poster = (action, delay) -> { posted.add(action); return true; };
        AtomicBoolean resumed = new AtomicBoolean(true); AtomicInteger retirementPollRequests = new AtomicInteger();
        retireRetry.schedule(poster, resumed::get, retirementPollRequests::incrementAndGet);
        resumed.set(false); posted.remove(0).run();
        require(retirementPollRequests.get() == 0 && pending.peek() != null, "paused retry altered state");
        resumed.set(true); retireRetry.schedule(poster, resumed::get, retirementPollRequests::incrementAndGet);
        posted.remove(0).run();
        require(retirementPollRequests.get() == 1 && pending.peek() != null, "retirement retry lost during busy start");
        release.countDown(); require(gate.awaitStopped(5000), "old decoder failed to stop");
        require(pending.takeIfReady(true, gate).source.equals("wait-for-old-decoder"), "pending source not delivered");
        long stale = gate.cancel(); pending.offer("stale", stale); gate.cancel();
        require(pending.takeIfReady(true, gate) == null && pending.peek() == null, "stale selection delivered");
        long current = gate.cancel(); pending.offer("old", current); pending.offer("new", current);
        require(pending.takeIfReady(true, gate).source.equals("new"), "latest selection not delivered");
        System.out.println("PASS deferred source selection");
    }
}
