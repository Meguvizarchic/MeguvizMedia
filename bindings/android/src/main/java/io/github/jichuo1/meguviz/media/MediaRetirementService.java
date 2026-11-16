package io.github.jichuo1.meguviz.media;

import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** One daemon and at most one queued retry. Native owners remain retained if
 * scheduling fails or the driver stays busy. The idle worker may time out. */
final class MediaRetirementService {
    private static ScheduledThreadPoolExecutor executor;
    // A scheduling exception may occur after a task enters the executor queue.
    // Keep that exact executor until it has actually terminated; its stale
    // runnable must not acquire authority to enter JNI after invalidation.
    private static ScheduledThreadPoolExecutor uncertainExecutor;
    private static Object executorGeneration;
    private static Object taskTicket;
    private static ScheduledFuture<?> pending;
    private static boolean scheduled, dirty, immediateDirty, running, prepareCreation, discardCreation;
    private static boolean nativeOutcomeUncertain;
    // Identity, not an incrementing counter: a stale BUSY completion cannot
    // resurrect a request superseded by a newer discard (or prepare).
    private static Object creationRequest;
    private static long delayMs;
    private static volatile int lastStatus;

    private MediaRetirementService() {}

    static synchronized void request() {
        requestLocked(false);
    }
    static synchronized void prepareCreation() {
        creationRequest = new Object();
        prepareCreation = true; discardCreation = false;
        requestLocked(true);
    }
    static synchronized void discardCreation() {
        creationRequest = new Object();
        discardCreation = true; prepareCreation = false;
        requestLocked(true);
    }

    /** Queue promptly without creating one worker per frame. */
    static synchronized void requestImmediate() {
        requestLocked(true);
    }

    private static void requestLocked(boolean immediate) {
        dirty = true;
        if (immediate) immediateDirty = true;
        if (nativeOutcomeUncertain) { lastStatus = 9; return; }
        if (uncertainExecutor != null) {
            // isTerminated is true only after all workers and queued tasks of
            // the old executor have exited, not merely after shutdown().
            try {
                if (!uncertainExecutor.isTerminated()) { lastStatus = 9; return; }
            } catch (Throwable failure) { lastStatus = 9; return; }
            uncertainExecutor = null;
        }
        lastStatus = 8;
        try {
            if (executor == null || executor.isShutdown()) {
                final Object generation = new Object();
                executor = new ScheduledThreadPoolExecutor(1, task -> {
                    Thread thread = new Thread(task, "Meguviz-retirement");
                    thread.setDaemon(true);
                    return thread;
                });
                executorGeneration = generation;
                executor.setRemoveOnCancelPolicy(true);
                executor.setKeepAliveTime(2, TimeUnit.SECONDS);
                executor.allowCoreThreadTimeOut(true);
            }
            if (scheduled) {
                if (immediate && !running && pending != null && pending.cancel(false))
                    scheduleLocked(0);
                return;
            }
            delayMs = 64;
            scheduled = true;
            scheduleLocked(immediate ? 0 : delayMs);
        } catch (Throwable failure) {
            failed();
        }
    }

    private static void scheduleLocked(long delay) {
        final Object generation = executorGeneration;
        final Object ticket = new Object();
        taskTicket = ticket;
        pending = executor.schedule(() -> run(generation, ticket), delay, TimeUnit.MILLISECONDS);
    }

    private static void run(Object generation, Object ticket) {
        boolean prepare, discard; Object request;
        synchronized (MediaRetirementService.class) {
            if (generation != executorGeneration || ticket != taskTicket || executor == null || nativeOutcomeUncertain) return;
            taskTicket = null;
            dirty = false; immediateDirty = false; running = true; pending = null;
            prepare = prepareCreation; discard = discardCreation; request = creationRequest;
            prepareCreation = discardCreation = false;
        }
        int result;
        boolean unknown = false;
        try {
            result = MediaNative.frameServiceUploads(0, 0);
            if (prepare) {
                int status = MediaNative.androidPrepareCreation();
                if (status == -7 || status == -8) {
                    synchronized (MediaRetirementService.class) { if (creationRequest == request) prepareCreation = true; }
                    result = 8;
                } else if (status != 0) result = 9;
            }
            if (discard) {
                int status = MediaNative.androidDiscardPreparedCreation();
                if (status == -7 || status == -8) {
                    synchronized (MediaRetirementService.class) { if (creationRequest == request) discardCreation = true; }
                    result = 8;
                } else if (status != 0) result = 9;
            }
        }
        catch (Throwable failure) { result = 9; unknown = true; }
        synchronized (MediaRetirementService.class) {
            if (generation != executorGeneration || executor == null) return;
            running = false;
            lastStatus = result;
            // A JNI exception can follow an unknown partial operation. No
            // subsequent request may silently replay it without a receipt.
            if (unknown) {
                nativeOutcomeUncertain = true;
                scheduled = false;
                pending = null;
                return;
            }
            if (dirty || result == 8) {
                delayMs = Math.min(1000, delayMs * 2);
                try {
                    // New work gets a prompt follow-up. A BUSY result still
                    // yields briefly, so repeated requests cannot spin at 0ms.
                    long nextDelay = immediateDirty ? (result == 8 ? 8 : 0) : delayMs;
                    scheduleLocked(nextDelay);
                } catch (Throwable failure) {
                    failed();
                }
            } else {
                scheduled = false;
                pending = null;
            }
        }
    }

    private static void failed() {
        scheduled = false;
        running = false;
        pending = null;
        lastStatus = 9;
        ScheduledThreadPoolExecutor old = executor;
        executor = null;
        executorGeneration = null;
        taskTicket = null;
        if (old != null) {
            uncertainExecutor = old;
            // shutdown is nonblocking and does not interrupt any JNI worker.
            // If it throws, retain the executor and keep future work blocked.
            try { old.shutdown(); } catch (Throwable ignored) { }
        }
    }

    static int status() { return lastStatus; }
}
