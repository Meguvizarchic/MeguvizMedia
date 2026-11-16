package io.github.jichuo1.meguviz.media;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.io.File;
import java.io.IOException;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.json.JSONTokener;
import java.util.HashSet;
import java.util.Set;

/** Stable JNI owner. The host explicitly loads and attaches the matching DSO. */
public final class MediaNative {
    private MediaNative() {}
    private static volatile boolean frameRetirementWakePending;
    private static synchronized void requestFrameRetirementSafely() {
        frameRetirementWakePending=true;
        try {
            MediaRetirementService.request();
            frameRetirementWakePending=MediaRetirementService.status()==9;
        } catch (Throwable ignored) { }
    }
    public static native int abiVersion();
    /** Version of the Android creation/receipt protocol, independent of the
     * base Media ABI. Zero means that the currently loaded DSO lacks it. */
    public static int androidCreationProtocolVersion() {
        try { return androidCreationProtocolVersionNative(); }
        catch (LinkageError missing) { return 0; }
    }
    private static native int androidCreationProtocolVersionNative();
    public static native byte[] identityRgb(byte[] rgb, int width, int height, int stride);
    public static long[] frameCreate(int backend, int gpuIndex, int threads, long maxPixels) {
        // A native failure may retain partially-created resources even when
        // JNI propagates an exception and returns no array.
        try { return frameCreateNative(backend,gpuIndex,threads,maxPixels); }
        finally { requestFrameRetirementSafely(); }
    }
    private static native long[] frameCreateNative(int backend, int gpuIndex, int threads, long maxPixels);
    /** Isolated v1 owned-creation protocol: reserve has no GPU side effects. */
    public static int frameOwnedCreationProtocolVersion() {
        try { return frameOwnedCreationProtocolVersionNative(); } catch (LinkageError missing) { return 0; }
    }
    private static native int frameOwnedCreationProtocolVersionNative();
    public static native long frameReserveNative();
    public static native int frameStartReservedNative(long receipt, int backend, int gpuIndex, int threads, long maxPixels);
    public static native int frameCancelReservationNative(long receipt);
    /** Optional additive v1 protocol; older DSOs report zero. */
    public static int frameSharedPlanProtocolVersion() {
        try { return frameSharedPlanProtocolVersionNative(); }
        catch (UnsatisfiedLinkError missing) { return 0; }
    }
    private static native int frameSharedPlanProtocolVersionNative();
    /** 0 disables, 1 prefers a shared GPU plan. Call after configure and before model load. */
    public static int frameSetSharedPlanPolicy(long handle, int policy) {
        if (policy != 0 && policy != 1) return 1;
        try { return frameSetSharedPlanPolicyNative(handle, policy); }
        catch (UnsatisfiedLinkError missing) { return 2; }
    }
    private static native int frameSetSharedPlanPolicyNative(long handle, int policy);
    public static native int frameConfigure(long handle, int flags);
    /** Raw domain status: 0 means no loss observed, -11 means device lost;
     * other negative values mean the query could not establish the state. */
    public static native int frameDeviceLossStatus(long handle);
    /** Explicit opt-in: bit 1 requests actual GPU timestamps, zero disables.
     * The domain samples flags at acceptance; this does not change NN math. */
    public static native int frameSetMeasurementFlags(long handle, int flags);
    /** {observed,elapsedNs,batches,observedBatches,validBits,floatPeriodBits,eventCoverage}.
     * Absent, failed, endpoint or partially observed work never reports elapsed.
     * These are command intervals, not shader busy time or utilization. */
    public static native long[] frameLastGpuMeasurement(long handle);
    /** Existing RIFE optimizations only: 1 tail prune, 2 direct reduced input,
     * 4 Add+LeakyReLU. Zero preserves the default legacy execution profile. */
    public static native int frameSetOptimizationFlags(long handle, int flags);
    /** Configure before model load, or on a worker after draining old work.
     * A loaded engine creates a new immutable plan and keeps the old plan if
     * validation fails. Existing AndroidMediaSession instances retain their
     * old immutable configuration; drain/close and recreate them to switch.
     * All-null clears the pack explicitly. Invalid explicit data never falls
     * back to a different pack or hides a binding failure. */
    public static int frameSetShaderpack(long handle, String rootDir, String canonicalManifestHash, String bindingsJson) {
        if (rootDir == null && canonicalManifestHash == null && bindingsJson == null)
            return frameSetShaderpackBindings(handle, null, null, null, null, null);
        if (rootDir == null || canonicalManifestHash == null || bindingsJson == null ||
            rootDir.indexOf('\0') >= 0 || bindingsJson.length() > 1024 * 1024) return 1;
        try {
            File directory = new File(rootDir);
            if (!directory.isAbsolute() || !directory.isDirectory() || !directory.getCanonicalPath().equals(rootDir)) return 1;
            byte[] root = rootDir.getBytes(StandardCharsets.UTF_8);
            if (root.length == 0 || root.length > 32768) return 1;
            JSONObject config = new BindingJson(bindingsJson).object();
            keys(config, "schema_version", "private_sdk_fingerprint", "bindings");
            if (integer(config, "schema_version") != 1) return 1;
            byte[] sdk = hash(config.getString("private_sdk_fingerprint"));
            JSONArray list = config.getJSONArray("bindings");
            if (list.length() == 0 || list.length() > 64) return 1;
            int[] kernels = new int[list.length()]; byte[] values = new byte[list.length() * 160];
            String[] fields = {"variant_key", "effective_options_hash", "target_facts_hash", "source_sha256", "spirv_sha256"};
            for (int i = 0; i < list.length(); i++) {
                JSONObject binding = list.getJSONObject(i);
                keys(binding, "kernel", "variant_key", "effective_options_hash", "target_facts_hash", "source_sha256", "spirv_sha256");
                long kernel = integer(binding, "kernel"); if (kernel < 1 || kernel > 9) return 1;
                kernels[i] = (int)kernel;
                for (int j = 0; j < fields.length; j++)
                    System.arraycopy(hash(binding.getString(fields[j])), 0, values, i * 160 + j * 32, 32);
            }
            return frameSetShaderpackBindings(handle, root, hash(canonicalManifestHash), sdk, kernels, values);
        } catch (JSONException | IOException | IllegalArgumentException failure) { return 1; }
    }
    private static void keys(JSONObject object, String... names) throws JSONException {
        if (object.length() != names.length) throw new JSONException("Unexpected binding fields");
        for (String name : names) if (!object.has(name)) throw new JSONException("Missing binding field");
    }
    private static long integer(JSONObject object, String name) throws JSONException {
        Object value = object.get(name);
        if (!(value instanceof Integer) && !(value instanceof Long)) throw new JSONException("Integer required");
        return ((Number)value).longValue();
    }
    private static byte[] hash(String text) {
        if (text == null || text.length() != 64) throw new IllegalArgumentException("SHA256 required");
        byte[] result = new byte[32];
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i); int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (value < 0) throw new IllegalArgumentException("Lower-case SHA256 required");
            result[i / 2] |= (byte)(value << ((i & 1) == 0 ? 4 : 0));
        }
        return result;
    }
    /** Small strict transport reader: Android JSONObject alone overwrites
     * duplicate keys. Keys are decoded before duplicate detection, so escaped
     * aliases cannot bypass it. Bounds match only this binding schema. */
    private static final class BindingJson {
        private final String source;
        private int at, nodes;
        BindingJson(String source) { this.source = source; }
        JSONObject object() throws JSONException {
            Object value = value(0); space();
            if (!(value instanceof JSONObject) || at != source.length()) throw error();
            return (JSONObject)value;
        }
        private JSONException error() { return new JSONException("Invalid or duplicate shader binding JSON"); }
        private void space() { while (at < source.length() && " \t\r\n".indexOf(source.charAt(at)) >= 0) at++; }
        private boolean take(char c) { space(); if (at < source.length() && source.charAt(at) == c) { at++; return true; } return false; }
        private Object value(int depth) throws JSONException {
            space(); if (depth > 32 || ++nodes > 4096 || at >= source.length()) throw error();
            char c = source.charAt(at);
            if (c == '{') {
                at++; JSONObject result = new JSONObject(); Set<String> seen = new HashSet<>();
                if (take('}')) return result;
                do {
                    space(); if (at >= source.length() || source.charAt(at) != '"') throw error();
                    String key = string(); if (!seen.add(key) || seen.size() > 32 || !take(':')) throw error();
                    result.put(key, value(depth + 1));
                    if (take('}')) return result;
                } while (take(','));
                throw error();
            }
            if (c == '[') {
                at++; JSONArray result = new JSONArray(); if (take(']')) return result;
                do {
                    if (result.length() >= 64) throw error(); result.put(value(depth + 1));
                    if (take(']')) return result;
                } while (take(','));
                throw error();
            }
            if (c == '"') return string();
            int begin = at;
            while (at < source.length() && " \t\r\n,]}:".indexOf(source.charAt(at)) < 0) at++;
            String token = source.substring(begin, at);
            if (token.equals("true")) return Boolean.TRUE;
            if (token.equals("false")) return Boolean.FALSE;
            if (token.equals("null")) return JSONObject.NULL;
            if (token.isEmpty() || token.length() > 32 || !token.matches("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?")) throw error();
            Object number = new JSONTokener(token).nextValue();
            if (!(number instanceof Number) || !Double.isFinite(((Number)number).doubleValue())) throw error();
            return number;
        }
        private String string() throws JSONException {
            int begin = at++;
            while (at < source.length()) {
                char c = source.charAt(at++);
                if (c == '"') {
                    Object decoded = new JSONTokener(source.substring(begin, at)).nextValue();
                    if (!(decoded instanceof String) || ((String)decoded).length() > 256) throw error();
                    return (String)decoded;
                }
                if (c < 32 || at - begin > 1538) throw error();
                if (c == '\\') {
                    if (at >= source.length()) throw error(); char escape = source.charAt(at++);
                    if ("\"\\/bfnrtu".indexOf(escape) < 0) throw error();
                    if (escape == 'u') for (int j = 0; j < 4; j++) {
                        if (at >= source.length()) throw error();char hex = source.charAt(at++);
                        if (!(hex >= '0' && hex <= '9') && !(hex >= 'a' && hex <= 'f') && !(hex >= 'A' && hex <= 'F')) throw error();
                    }
                }
            }
            throw error();
        }
    }
    private static native int frameSetShaderpackBindings(long handle, byte[] rootUtf8, byte[] manifestHash,
        byte[] privateSdkFingerprint, int[] kernels, byte[] bindingHashes);
    /** A failed upload may still own GPU work. Service reclaims abandoned
     * upload objects; callers must explicitly retry load after it succeeds. */
    public static int frameLoad(long handle, ByteBuffer param, ByteBuffer weights) {
        int status = frameLoadNative(handle, param, weights);
        if (status != 0 || frameRetirementWakePending) requestFrameRetirementSafely();
        return status;
    }
    private static native int frameLoadNative(long handle, ByteBuffer param, ByteBuffer weights);
    public static native int frameProcess(long handle, ByteBuffer left, ByteBuffer right, ByteBuffer output,
        int width, int height, int leftStride, int rightStride, int outputStride,
        float alpha, long leftId, long rightId, int flowScale);
    /** Requests worker-side close. BUSY keeps the native owner and starts one
     * bounded-queue retirement service; it does not mean physical completion.
     * The host keeps the matching library loaded until service succeeds. */
    public static void frameClose(long handle) {
        int status = frameTryClose(handle);
        if ((status != 0 && status != 7) || frameRetirementWakePending) requestFrameRetirementSafely();
    }
    /** Worker-only status close: zero closes, BUSY retains the native handle.
     * Cleanup/driver calls have no hard wall-clock deadline. */
    public static native int frameTryClose(long handle);
    /** Services abandoned uploads and closing engines. Zero handle selects
     * this module; nonzero selects one engine. Zero wait polls the device slot.
     * This does not close live sessions or reclaim consumer-owned outputs. */
    public static native int frameServiceUploads(long handle, long waitNs);
    /** Last background service result; zero is not a device qualification. */
    public static synchronized int frameRetirementServiceStatus() {
        try {
            if (frameRetirementWakePending || MediaRetirementService.status() == 9)
                requestFrameRetirementSafely();
            return MediaRetirementService.status();
        }
        catch (Throwable ignored) { frameRetirementWakePending=true; return 9; }
    }
    public static native void frameSetMotionGuard(long handle, boolean enabled);
    public static native int frameLastUnreliablePermille(long handle);
    public static native int frameLastPatchedTiles(long handle);
    public static native int frameLastHistogramPermille(long handle);
    public static native long[] frameLastStageNanos(long handle);
    public static native String frameExecutionInfo(long handle);
    /** Optional schema-v1 19-value per-job snapshot. Element 0 is 1 only for a
     * completed still-current request. Indices 1..5 are generation, native
     * request-ID-observed, native request ID, left source ID, right source ID;
     * 6..7 are JNI host-cache states; 8..10 are frame kind, effective scale,
     * and cache-lookup scale; 11..12 are static/motion guard dispatch states;
     * 13..16 are tensor and bridge cache states; 17..18 are tensor publication
     * stage observed and published count. Values use MGR_CACHE_OBSERVATION_*
     * and MGR_GUARD_OBSERVATION_* from the public RIFE C header. OFF, BUSY,
     * failure and dirty plans are unobserved; no per-pixel effect is implied. */
    public static native long[] frameJobObservation(long handle);
    public static native int frameLastFrameKind(long handle);
    public static native int frameLastEffectiveFlowScale(long handle);
    public static native long frameLastWorkerCpuNanos(long handle);
    public static native int isDuplicate(ByteBuffer left, ByteBuffer right, int width, int height, int stride);
    public static native int analyzeScene(ByteBuffer left, ByteBuffer right, int width, int height, int stride);
    public static native int rgbaToRgb(ByteBuffer rgba, int width, int height, ByteBuffer output, int outputStride);
    static native long[] androidCreate(long engine, int width, int height, int poolCapacity, int path, boolean admission);
    static native long[] androidCreateWithSource(long engine, int width, int height, int sourceWidth, int sourceHeight,
        int poolCapacity, int path, boolean admission);
    static native int androidPrepareCreation();
    static native int androidDiscardPreparedCreation();
    static native int androidServiceCreationOwner();
    static native int androidAbandonCreation(long receipt);
    static native int androidCreatePrepared(long engine, int width, int height, int sourceWidth, int sourceHeight,
        int poolCapacity, int path, boolean admission, long[] receipt);
    static native int androidAsyncReserve(long[] receipt);
    static native int androidAsyncPreparePlan(long receipt, long engine, int width, int height, int sourceWidth,
        int sourceHeight, int capacity, int path, boolean admission);
    static native int androidAsyncBeginGl(long receipt);
    static native int androidAsyncImport(long receipt);
    static native int androidAsyncCommitGl(long receipt);
    static native int androidAsyncCancel(long receipt);
    static native int androidAsyncRetirementStatus(long receipt);
    static native String androidExecutionInfo(long session);
    static native long[] androidFrameJobObservation(long session, long frame);
    static native long[] androidProbe(long session);
    static native long[] androidCapture(long session, int oesTexture, float[] transform, long sourceId, long epoch, long ptsUs, int primaries, int matrix);
    static native long[] androidCompare(long session, long left, long right);
    static native long[] androidInterpolate(long session, long left, long right, double alpha, long deadlineNs);
    static native long[] androidSubmit(long session, long left, long right, double alpha, long deadlineNs, int flowScale);
    static native long[] androidPollCompleted(long session);
    static native int androidCancel(long session, long jobId);
    static native int androidDraw(long session, long frame, int width, int height);
    static native int androidPoll(long session, long timeoutNs);
    static native int androidDeviceLossStatus(long session);
    static native long[] androidDeviceState(long session);
    static native int androidBeginEpoch(long session, long epoch);
    static native void androidReleaseFrame(long session, long frame);
    static native int androidReleaseFrameDeferred(long session, long frame);
    static native int androidClose(long session);
    static native int androidSetEventsEnabled(long session, boolean enabled);
    static native long[] androidPollEvents(long session);
}
