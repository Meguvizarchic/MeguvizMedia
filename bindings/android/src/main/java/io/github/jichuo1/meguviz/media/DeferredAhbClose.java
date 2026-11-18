package io.github.jichuo1.meguviz.media;

import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.LockSupport;

/** Failure returns to the fixture caller while this EGL owner still owns late cleanup. */
public final class DeferredAhbClose {
    static final long WORKER_WAIT_NS = TimeUnit.SECONDS.toNanos(3);
    private static final long RETRY_NS = TimeUnit.MILLISECONDS.toNanos(250);
    private static final ThreadLocal<Retention> retained = new ThreadLocal<>();

    private static final class Retention {
        final String reason;
        final Runnable retry;
        final Object[] owners;
        boolean interrupted;
        Retention(String reason, Runnable retry, Object... owners) {
            this.reason = reason;
            this.retry = retry;
            this.owners = owners;
        }
    }

    static final class Unproven extends IllegalStateException {
        Unproven(String message) { super(message); }
        Unproven(String message, Throwable cause) { super(message, cause); }
    }

    static void close(GlesMotionWarp backend, Runnable releaseFrames, Runnable closeEgl, long workerWaitNs) {
        try {
            backend.closeWithFrames(releaseFrames, System.nanoTime() + workerWaitNs);
        } catch (Unproven failure) {
            retained.set(new Retention(failure.getMessage(), () -> {
                backend.closeWithFrames(releaseFrames, System.nanoTime() + workerWaitNs);
                closeEgl.run();
            }, backend));
            throw failure;
        } catch (RuntimeException | Error failure) {
            if (backend.preservesEglOnClose()) retained.set(new Retention(failure.toString(), null, backend, closeEgl, releaseFrames));
            throw failure;
        }
    }

    @FunctionalInterface interface OwnerJoin { void run() throws InterruptedException; }

    static void rejoin(OwnerJoin join, String name) {
        try { join.run(); }
        catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new Unproven(name + " retirement interrupted", interrupted);
        } catch (IllegalStateException waiting) { throw new Unproven(name + " retirement was not proven", waiting); }
    }

    private static final class OwnerSequence implements Runnable {
        final Runnable[] steps;
        int cursor;
        OwnerSequence(Runnable[] steps) { this.steps = steps; }
        @Override public void run() {
            while (cursor < steps.length) {
                steps[cursor].run();
                cursor++;
            }
        }
    }

    static void resumeUnprovenOwners(String reason, Object[] owners, Runnable... steps) {
        retained.set(new Retention(reason, new OwnerSequence(steps), owners));
    }

    /** Allow NIO to persist the existing failure without interrupting that write. */
    public static void prepareReceiptOnCurrentOwner() {
        Retention claim = retained.get();
        if (claim != null) claim.interrupted |= Thread.interrupted();
    }

    public static String reasonOnCurrentOwner() {
        Retention claim = retained.get();
        return claim == null ? null : claim.reason;
    }

    /** Called after the terminal failure receipt has been published by the Activity. */
    public static void reapOnCurrentOwner() {
        Retention claim = retained.get();
        if (claim == null) return;
        boolean restoreInterrupt = claim.interrupted | Thread.interrupted();
        boolean retry = claim.retry != null;
        try {
            for (;;) {
                if (retry) {
                    try {
                        claim.retry.run();
                        retained.remove();
                        return;
                    } catch (Unproven pending) {
                        // Keep the original owner and all of its resources for the next proof.
                    } catch (RuntimeException | Error failed) {
                        // A partially attempted native teardown is not retried blindly.
                        retry = false;
                        System.err.println("AHB retirement remains unproven: " + failed);
                    }
                }
                restoreInterrupt |= Thread.interrupted();
                if (retry) LockSupport.parkNanos(RETRY_NS);
                else LockSupport.park();
            }
        } finally {
            if (restoreInterrupt) Thread.currentThread().interrupt();
        }
    }

    private DeferredAhbClose() {}
}
