package io.github.jichuo1.meguviz.media.player;

import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Host test for cleanup ordering and the gate's actual retained owner. */
public final class DecoderResourcesTest {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static Object retainedOwner(DecoderGate gate) throws Exception {
        Field field = DecoderGate.class.getDeclaredField("producerOwner");
        field.setAccessible(true);
        return field.get(gate);
    }
    private static Object gateField(DecoderGate gate, String name) throws Exception {
        Field field = DecoderGate.class.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(gate);
    }

    private static DecoderResources<Object, Object, Object, Object> owner() {
        DecoderResources<Object, Object, Object, Object> result = new DecoderResources<>(new Object(), new Object());
        result.codec = new Object(); result.extractor = new Object();
        return result;
    }

    public static void main(String[] args) throws Exception {
        DecoderGate failed = new DecoderGate();
        DecoderResources<Object, Object, Object, Object> resources = owner();
        Object codec = resources.codec, extractor = resources.extractor, surface = resources.surface, texture = resources.texture;
        List<String> calls = new ArrayList<>();
        long generation = failed.cancel();
        require(failed.start(generation, resources, token -> resources.release(
            value -> calls.add("stop"), value -> { calls.add("codec"); throw new IllegalStateException("failed"); },
            value -> calls.add("extractor"), value -> calls.add("surface"))), "failure job not accepted");
        require(!failed.awaitStopped(5000), "failed release was marked stopped");
        require(failed.releaseUnconfirmed() && retainedOwner(failed) == resources, "actual failed owner was not retained");
        require(resources.codec == codec && resources.extractor == extractor && resources.surface == surface && resources.texture == texture,
            "failed owner lost producer or consumer objects");
        require(calls.equals(Arrays.asList("stop", "codec", "extractor")), "Surface released after codec failure: " + calls);
        long next = failed.cancel();
        require(!failed.ready(next) && !failed.start(next, owner(), token -> true), "failure admitted another producer");

        DecoderGate fatal = new DecoderGate();
        DecoderResources<Object, Object, Object, Object> fatalOwner = owner();
        List<String> fatalCalls = new ArrayList<>();
        Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {});
        try {
            generation = fatal.cancel();
            require(fatal.start(generation, fatalOwner, token -> fatalOwner.release(
                value -> fatalCalls.add("stop"), value -> { fatalCalls.add("codec"); throw new AssertionError("fatal release"); },
                value -> fatalCalls.add("extractor"), value -> fatalCalls.add("surface"))), "fatal job not accepted");
            require(!fatal.awaitStopped(5000) && fatal.releaseUnconfirmed() && retainedOwner(fatal) == fatalOwner,
                "non-runtime release error did not retain owner");
            require(fatalCalls.equals(Arrays.asList("stop", "codec")), "surface touched after fatal codec release: " + fatalCalls);
        } finally { Thread.setDefaultUncaughtExceptionHandler(previous); }

        DecoderGate unstarted = new DecoderGate();
        Object unstartedSurface = new Object(), unstartedTexture = new Object();
        unstarted.retainUnstartedSurface(unstartedSurface, unstartedTexture);
        generation = unstarted.cancel();
        require(unstarted.releaseUnconfirmed() && !unstarted.ready(generation) &&
            gateField(unstarted, "unstartedSurface") == unstartedSurface && gateField(unstarted, "unstartedTexture") == unstartedTexture,
            "failed unstarted Surface cleanup lost its owner");
        require(!unstarted.start(generation, owner(), token -> true), "unconfirmed unstarted Surface allowed a producer");

        DecoderGate normal = new DecoderGate();
        DecoderResources<Object, Object, Object, Object> success = owner();
        List<String> normalCalls = new ArrayList<>();
        generation = normal.cancel();
        require(normal.start(generation, success, token -> success.release(
            value -> { normalCalls.add("stop"); throw new IllegalStateException("stop"); },
            value -> normalCalls.add("codec"), value -> normalCalls.add("extractor"), value -> normalCalls.add("surface"))),
            "normal job not accepted");
        require(normal.awaitStopped(5000), "successful cleanup did not finish");
        require(normalCalls.equals(Arrays.asList("stop", "codec", "extractor", "surface")), "unexpected cleanup order: " + normalCalls);
        require(retainedOwner(normal) == null, "confirmed owner was not reaped");

        DecoderResources<Object, Object, Object, Object> noCodec = new DecoderResources<>(new Object(), new Object());
        List<String> emptyCalls = new ArrayList<>();
        require(noCodec.release(v -> emptyCalls.add("stop"), v -> emptyCalls.add("codec"),
            v -> emptyCalls.add("extractor"), v -> emptyCalls.add("surface")), "pre-codec cleanup failed");
        require(emptyCalls.equals(Arrays.asList("surface")), "unexpected pre-codec cleanup: " + emptyCalls);
        System.out.println("PASS producer cleanup ordering and gate ownership");
    }
}
