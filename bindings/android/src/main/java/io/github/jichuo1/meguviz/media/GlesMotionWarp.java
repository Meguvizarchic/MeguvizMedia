package io.github.jichuo1.meguviz.media;

import android.opengl.GLES11Ext;
import android.opengl.GLES30;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/** Offloads compact luma estimation to Vulkan and retains full-resolution original pixels on the GPU. */
public final class GlesMotionWarp implements WindowBackend {
    private static final int DEFAULT_WIDTH = 3840, DEFAULT_HEIGHT = 2160;
    private static final int LUMA_W = 480, LUMA_H = 272, GRID_W = 60, GRID_H = 34, TILE = 8;
    private static final int GRID_CELLS = GRID_W * GRID_H;
    private static final int INFERRED = 2;

    private static final String QUAD = "#version 300 es\n"
        + "out vec2 uv;\n"
        + "void main() {\n"
        + "  uv = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
        + "  gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);\n"
        + "}\n";
    private static final String COPY = "#version 300 es\n"
        + "#extension GL_OES_EGL_image_external_essl3 : require\n"
        + "precision highp float;\n"
        + "in vec2 uv; out vec4 color;\n"
        + "uniform samplerExternalOES sourceImage; uniform mat4 sourceTransform;\n"
        + "void main() { color = texture(sourceImage, (sourceTransform * vec4(uv, 0, 1)).xy); }\n";
    private static final String ORIGINAL = "#version 300 es\n"
        + "precision highp float;\n"
        + "in vec2 uv; out vec4 color; uniform sampler2D sourceImage;\n"
        + "void main() { color = texture(sourceImage, uv); }\n";
    private static final String LUMA = "#version 300 es\n"
        + "precision highp float;\n"
        + "out vec4 color; uniform sampler2D sourceImage;\n"
        + "float luma(vec2 uv) {\n"
        + "  vec2 d = 0.25 / vec2(480.0, 272.0);\n"
        + "  vec3 v = texture(sourceImage, uv + vec2(-d.x,-d.y)).rgb\n"
        + "         + texture(sourceImage, uv + vec2( d.x,-d.y)).rgb\n"
        + "         + texture(sourceImage, uv + vec2(-d.x, d.y)).rgb\n"
        + "         + texture(sourceImage, uv + vec2( d.x, d.y)).rgb;\n"
        + "  return dot(v * 0.25, vec3(0.2126,0.7152,0.0722));\n"
        + "}\n"
        + "void main() {\n"
        + "  float x = float(int(gl_FragCoord.x) * 4) + 0.5;\n"
        + "  float y = gl_FragCoord.y / 272.0;\n"
        + "  color = vec4(luma(vec2(x/480.0,y)), luma(vec2((x+1.0)/480.0,y)),\n"
        + "               luma(vec2((x+2.0)/480.0,y)), luma(vec2((x+3.0)/480.0,y)));\n"
        + "}\n";
    // Fetch tile motion once per mesh vertex. The rasterizer interpolates
    // displacement and confidence, leaving only two full-size reads per output pixel.
    private static final String WARP_VERT = "#version 300 es\n"
        + "precision highp float;\n"
        + "out vec2 uv; out vec4 displacement; out vec2 reliability;\n"
        + "uniform highp sampler2D motionField; uniform highp sampler2D confidenceField;\n"
        + "void main() {\n"
        + "  ivec2 corner = ivec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
        + "  ivec2 vertex = ivec2(gl_InstanceID % 61, gl_InstanceID / 61) + corner;\n"
        + "  ivec2 cell = clamp(vertex - ivec2(1), ivec2(0), ivec2(59,33));\n"
        + "  uv = clamp((vec2(vertex) - 0.5) / vec2(60,34), 0.0, 1.0);\n"
        + "  displacement = texelFetch(motionField, cell, 0) * vec4(0.5/480.0,0.5/272.0,0.5/480.0,0.5/272.0);\n"
        + "  reliability = texelFetch(confidenceField, cell, 0).rg;\n"
        + "  gl_Position = vec4(uv * 2.0 - 1.0, 0, 1);\n"
        + "}\n";
    private static final String WARP_FRAG = "#version 300 es\n"
        + "precision highp float;\n"
        + "precision highp int;\n"
        + "in vec2 uv;\n"
        + "in vec4 displacement;\n"
        + "in vec2 reliability;\n"
        + "out vec4 color;\n"
        + "uniform sampler2D previousImage;\n"
        + "uniform sampler2D currentImage;\n"
        + "uniform highp sampler2D motionField;\n"
        + "uniform highp sampler2D confidenceField;\n"
        + "uniform highp sampler2D candidateField;\n"
        + "\n"
        + "const vec2 GRID = vec2(60.0, 34.0);\n"
        + "const vec4 HALF_COMPACT_PIXEL = vec4(0.5 / 480.0, 0.5 / 272.0, 0.5 / 480.0, 0.5 / 272.0);\n"
        + "\n"
        + "float insideImage(vec2 p) {\n"
        + "    return float(all(greaterThanEqual(p, vec2(0))) && all(lessThanEqual(p, vec2(1))));\n"
        + "}\n"
        + "\n"
        + "float rgbDistance(vec3 a, vec3 b) {\n"
        + "    vec3 delta = abs(a - b);\n"
        + "    return max(delta.r, max(delta.g, delta.b));\n"
        + "}\n"
        + "\n"
        + "float sourceDifference(ivec2 pixel) {\n"
        + "    return rgbDistance(texelFetch(previousImage, pixel, 0).rgb, texelFetch(currentImage, pixel, 0).rgb);\n"
        + "}\n"
        + "\n"
        + "bool staticNeighborhood(ivec2 pixel) {\n"
        + "    ivec2 limit = textureSize(previousImage, 0) - ivec2(1);\n"
        + "    vec2 direction = (abs(displacement.xy) + abs(displacement.zw)) * vec2(limit + ivec2(1));\n"
        + "    ivec2 offset = (direction.x >= direction.y) ? ivec2(2, 0) : ivec2(0, 2);\n"
        + "    ivec2 before = clamp(pixel - offset, ivec2(0), limit);\n"
        + "    ivec2 after = clamp(pixel + offset, ivec2(0), limit);\n"
        + "    return sourceDifference(before) <= 5.5 / 255.0 && sourceDifference(after) <= 5.5 / 255.0;\n"
        + "}\n"
        + "\n"
        + "struct ViewCandidate {\n"
        + "    vec2 shift;\n"
        + "    vec3 rgb;\n"
        + "    float cost;\n"
        + "    float support;\n"
        + "};\n"
        + "\n"
        + "ViewCandidate proposal(ivec2 cell, int direction, int rank) {\n"
        + "    ivec2 at = cell + ivec2(0, (direction * 2 + rank) * 34);\n"
        + "    vec4 value = texelFetch(candidateField, at, 0);\n"
        + "    ViewCandidate item = ViewCandidate(value.xy, vec3(0.0), value.z, value.w);\n"
        + "    if (item.support < 0.16) {\n"
        + "        item.support = 0.0;\n"
        + "        return item;\n"
        + "    }\n"
        + "    vec2 sampleUv = uv - item.shift * HALF_COMPACT_PIXEL.xy;\n"
        + "    if (insideImage(sampleUv) < 0.5) {\n"
        + "        item.support = 0.0;\n"
        + "        return item;\n"
        + "    }\n"
        + "    item.rgb = direction == 0 ? texture(previousImage, sampleUv).rgb : texture(currentImage, sampleUv).rgb;\n"
        + "    return item;\n"
        + "}\n"
        + "\n"
        + "float maxAbs(vec2 a) {\n"
        + "    a = abs(a);\n"
        + "    return max(a.x, a.y);\n"
        + "}\n"
        + "\n"
        + "float visibility(ViewCandidate item, float opposingSupport, vec3 observed, float sourceChange) {\n"
        + "    float support = smoothstep(0.58,0.80,item.support);\n"
        + "    float dominance = smoothstep(0.40,0.69,item.support-opposingSupport);\n"
        + "    float photo = 1.0-smoothstep(0.04,0.075,item.cost);\n"
        + "    float matching = 1.0-smoothstep(16.0/255.0,32.0/255.0,rgbDistance(item.rgb,observed));\n"
        + "    float transition = smoothstep(16.0/255.0,32.0/255.0,sourceChange);\n"
        + "    return support * dominance * photo * matching * transition;\n"
        + "}\n"
        + "\n"
        + "bool alternativeMotion(vec3 originalLeft, vec3 originalRight, float originalDisagreement,\n"
        + "                       vec3 fallback, out vec3 result) {\n"
        + "    ivec2 cell = clamp(ivec2(floor(uv * GRID)), ivec2(0), ivec2(59,33));\n"
        + "    ViewCandidate left[2];\n"
        + "    ViewCandidate right[2];\n"
        + "    for (int n=0; n<2; ++n) {\n"
        + "        left[n] = proposal(cell,0,n);\n"
        + "        right[n] = proposal(cell,1,n);\n"
        + "    }\n"
        + "    float pairMass = 0.0;\n"
        + "    float pairPeak = 0.0;\n"
        + "    vec3 pairSum = vec3(0.0);\n"
        + "    vec3 pairSquare = vec3(0.0);\n"
        + "    for (int a=0; a<2; ++a) {\n"
        + "        for (int b=0; b<2; ++b) {\n"
        + "            float support = min(left[a].support, right[b].support);\n"
        + "            if (support <= 0.42) continue;\n"
        + "\n"
        + "            // The two shifts must describe the same boundary trajectory across frames.\n"
        + "            float cycle = maxAbs(left[a].shift + right[b].shift);\n"
        + "            vec4 shift = vec4(left[a].shift,right[b].shift);\n"
        + "            vec4 change = abs(shift - displacement / HALF_COMPACT_PIXEL);\n"
        + "            float innovation = max(max(change.x,change.y),max(change.z,change.w));\n"
        + "            float mismatch = rgbDistance(left[a].rgb,right[b].rgb);\n"
        + "            float gain = originalDisagreement - mismatch;\n"
        + "            float score = mismatch + 0.6 * (left[a].cost + right[b].cost) + 0.012 * cycle;\n"
        + "            float evidence = smoothstep(8.0/255.0,32.0/255.0,gain) * smoothstep(0.42,0.75,support);\n"
        + "            evidence *= 1.0-smoothstep(2.0,3.75,cycle);\n"
        + "            evidence *= smoothstep(1.5,3.0,innovation);\n"
        + "            evidence *= 1.0-smoothstep(20.0/255.0,28.0/255.0,mismatch);\n"
        + "            evidence *= smoothstep(6.0/255.0,16.0/255.0,originalDisagreement-score);\n"
        + "            if (evidence == 0.0) continue;\n"
        + "            float w = left[a].support / (left[a].support + right[b].support);\n"
        + "            vec3 rgb = mix(right[b].rgb,left[a].rgb,w);\n"
        + "            float vote = evidence*evidence;\n"
        + "            pairSum += rgb*vote;\n"
        + "            pairSquare += rgb*rgb*vote;\n"
        + "            pairMass += vote;\n"
        + "            pairPeak = max(pairPeak,evidence);\n"
        + "        }\n"
        + "    }\n"
        + "\n"
        + "    // Continuous per-side visibility. Equal or weak support does not override the original blend.\n"
        + "    float peakLeft = max(left[0].support,left[1].support);\n"
        + "    float peakRight = max(right[0].support,right[1].support);\n"
        + "    float sourceChange = rgbDistance(originalLeft,originalRight);\n"
        + "    float sideMass = 0.0;\n"
        + "    float sidePeak = 0.0;\n"
        + "    vec3 sideSum = vec3(0.0);\n"
        + "    for (int n=0; n<2; ++n) {\n"
        + "        float vL = visibility(left[n],peakRight,originalLeft,sourceChange);\n"
        + "        float vR = visibility(right[n],peakLeft,originalRight,sourceChange);\n"
        + "        float wl = vL*vL;\n"
        + "        float wr = vR*vR;\n"
        + "        sideSum += left[n].rgb*wl + right[n].rgb*wr;\n"
        + "        sideMass += wl+wr;\n"
        + "        sidePeak = max(sidePeak,max(vL,vR));\n"
        + "    }\n"
        + "    if (pairMass == 0.0 && sideMass == 0.0) return false;\n"
        + "\n"
        + "    vec3 changed = fallback;\n"
        + "    if (sideMass > 0.0) changed = mix(changed,sideSum/sideMass,0.75*sidePeak);\n"
        + "    if (pairMass > 0.0) {\n"
        + "        vec3 average = pairSum/pairMass;\n"
        + "        vec3 variance = max(pairSquare/pairMass-average*average,vec3(0.0));\n"
        + "        float spread = max(max(variance.r,variance.g),variance.b);\n"
        + "        float consistency = 1.0-smoothstep(0.006,0.030,spread);\n"
        + "        changed = mix(changed,average,pairPeak*consistency);\n"
        + "    }\n"
        + "\n"
        + "    // Compact candidates change by grid cell. Fade narrowly across that discrete boundary.\n"
        + "    vec2 phase = fract(uv * GRID);\n"
        + "    vec2 margin = min(phase,1.0-phase);\n"
        + "    float edgeFade = smoothstep(0.0,0.065,min(margin.x,margin.y));\n"
        + "    float conflictFade = smoothstep(24.0/255.0,44.0/255.0,originalDisagreement);\n"
        + "    result = mix(fallback,changed,edgeFade*conflictFade);\n"
        + "    return true;\n"
        + "}\n"
        + "\n"
        + "\n"
        + "void main() {\n"
        + "    vec2 a = uv - displacement.xy;\n"
        + "    vec2 b = uv - displacement.zw;\n"
        + "    vec2 weight = reliability * vec2(insideImage(a), insideImage(b));\n"
        + "    float sum = weight.x + weight.y;\n"
        + "    if (sum < 0.0001) {\n"
        + "        color = vec4(texture(previousImage, uv).rgb * 0.5 + texture(currentImage, uv).rgb * 0.5, 1.0);\n"
        + "        return;\n"
        + "    }\n"
        + "\n"
        + "    vec3 left = texture(previousImage, a).rgb;\n"
        + "    vec3 right = texture(currentImage, b).rgb;\n"
        + "    float disagreement = rgbDistance(left, right);\n"
        + "    bool conflict = min(weight.x, weight.y) > 0.0 && disagreement > 16.0 / 255.0;\n"
        + "    bool weak = max(weight.x, weight.y) < 0.5;\n"
        + "    vec3 originalLeft = vec3(0.0), originalRight = vec3(0.0);\n"
        + "    if (conflict || weak) {\n"
        + "        ivec2 pixel = ivec2(gl_FragCoord.xy);\n"
        + "        originalLeft = texelFetch(previousImage, pixel, 0).rgb;\n"
        + "        originalRight = texelFetch(currentImage, pixel, 0).rgb;\n"
        + "        if (rgbDistance(originalLeft, originalRight) <= 2.5 / 255.0 && staticNeighborhood(pixel)) {\n"
        + "            color = vec4(0.5 * (originalLeft + originalRight), 1.0);\n"
        + "            return;\n"
        + "        }\n"
        + "    }\n"
        + "\n"
        + "    if (conflict) {\n"
        + "        ivec2 cell = clamp(ivec2(floor(uv * GRID)), ivec2(0), ivec2(59, 33));\n"
        + "        vec4 nearestMotion = texelFetch(motionField, cell, 0);\n"
        + "        vec4 difference = abs(nearestMotion - displacement / HALF_COMPACT_PIXEL);\n"
        + "        float jump = max(max(difference.x, difference.y), max(difference.z, difference.w));\n"
        + "        if (jump >= 3.0) {\n"
        + "            vec4 nearest = nearestMotion * HALF_COMPACT_PIXEL;\n"
        + "            vec2 u = uv - nearest.xy;\n"
        + "            vec2 v = uv - nearest.zw;\n"
        + "            vec2 nearestWeight = texelFetch(confidenceField, cell, 0).rg * vec2(insideImage(u), insideImage(v));\n"
        + "            if (min(nearestWeight.x, nearestWeight.y) >= 0.35) {\n"
        + "                vec3 nearestLeft = texture(previousImage, u).rgb;\n"
        + "                vec3 nearestRight = texture(currentImage, v).rgb;\n"
        + "                float nearestError = rgbDistance(nearestLeft, nearestRight);\n"
        + "                if (nearestError <= 20.0 / 255.0 && nearestError + 8.0 / 255.0 <= disagreement) {\n"
        + "                    nearestWeight /= nearestWeight.x + nearestWeight.y;\n"
        + "                    color = vec4(nearestLeft * nearestWeight.x + nearestRight * nearestWeight.y, 1.0);\n"
        + "                    return;\n"
        + "                }\n"
        + "            }\n"
        + "        }\n"
        + "    }\n"
        + "\n"
        + "    float strongest = max(weight.x, weight.y);\n"
        + "    weight /= sum;\n"
        + "    if (conflict && disagreement > 24.0 / 255.0) {\n"
        + "        float colorGate = smoothstep(24.0 / 255.0, 64.0 / 255.0, disagreement);\n"
        + "        float dominanceGate = smoothstep(0.10, 0.55, abs(weight.x - weight.y));\n"
        + "        float confidenceGate = smoothstep(0.35, 0.70, strongest);\n"
        + "        vec2 sharper = weight * weight;\n"
        + "        sharper /= sharper.x + sharper.y;\n"
        + "        weight = mix(weight, sharper, colorGate * dominanceGate * confidenceGate);\n"
        + "    }\n"
        + "    vec3 fallback = left * weight.x + right * weight.y;\n"
        + "    vec3 alternative;\n"
        + "    if (conflict && disagreement > 24.0/255.0 &&\n"
        + "        alternativeMotion(originalLeft,originalRight,disagreement,fallback,alternative)) {\n"
        + "        color = vec4(alternative,1.0);\n"
        + "        return;\n"
        + "    }\n"
        + "    color = vec4(fallback,1.0);\n"
        + "}\n";

    private static ByteBuffer buffer(int size) {
        return ByteBuffer.allocateDirect(size).order(ByteOrder.nativeOrder());
    }

    private static final RenderSourceFences.Api SHARED_FENCES = new RenderSourceFences.Api() {
        @Override public long fenceAndFlush() {
            long fence = createSharedFence();
            try {
                GLES30.glFlush();
                checkGl("flush capture fence");
                return fence;
            } catch (RuntimeException | Error failure) {
                GLES30.glDeleteSync(fence);
                throw failure;
            }
        }

        @Override public boolean signaled(long fence) {
            int result = GLES30.glClientWaitSync(fence, 0, 0L);
            if (result == GLES30.GL_ALREADY_SIGNALED || result == GLES30.GL_CONDITION_SATISFIED) return true;
            if (result == GLES30.GL_TIMEOUT_EXPIRED) return false;
            throw new IllegalStateException("shared fence wait failed: " + result);
        }

        @Override public void delete(long fence) { GLES30.glDeleteSync(fence); }
    };

    private static long createSharedFence() {
        long fence = GLES30.glFenceSync(GLES30.GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (fence == 0) throw new IllegalStateException("missing shared GL fence");
        return fence;
    }

    static final class DrawRelease {
        private final Source left, right;
        private final long leftFence, rightFence;

        private DrawRelease(Source left, Source right, long leftFence, long rightFence) {
            this.left = left;
            this.right = right;
            this.leftFence = leftFence;
            this.rightFence = rightFence;
        }
    }

    private static final class SourceSlot {
        RenderSourceFences fences;
        final ByteBuffer luma = buffer(LUMA_W * LUMA_H);
        CompactLumaReadback compactReadback;
        int texture;
        long ahbSlot;
        int producerFd = -1;
        boolean unprovenProducer;
        final int slotIndex;
        Source active;

        SourceSlot(int slotIndex) { this.slotIndex = slotIndex; }

        boolean allowAhbWrite() {
            if (unprovenProducer) return false;
            if (producerFd < 0) return true;
            int status = QcomMotionNative.lumaFenceStatus(producerFd);
            if (status < 0) throw new IllegalStateException("could not prove compact luma GL retirement");
            if (status == 0) return false;
            QcomMotionNative.closeLumaFence(producerFd);
            producerFd = -1;
            return true;
        }
    }

    private static final class Source {
        final SourceSlot slot;
        final long id;
        final long ptsUs;
        final CaptureTiming timing = new CaptureTiming();
        int refs = 1;

        Source(SourceSlot slot, long id, long ptsUs) {
            this.slot = slot;
            this.id = id;
            this.ptsUs = ptsUs;
        }
    }

    private static final class FlowSlot {
        final ByteBuffer raw = buffer(GRID_CELLS * 4 * Float.BYTES);
        final ByteBuffer positions = buffer(GRID_CELLS * 4 * Float.BYTES);
        final ByteBuffer confidence = buffer(GRID_CELLS * 2);
        final ByteBuffer nativeCandidates = buffer(QcomCandidateField.RAW_BYTES);
        final ByteBuffer midpointCandidates = buffer(QcomCandidateField.ATLAS_BYTES);
        boolean used;
    }

    private static final class Motion {
        final Source left;
        final Source right;
        final FlowSlot flow;
        final long id;
        int status;
        long workNs;
        boolean ahbPending;
        long nativeEstimateNs = -1L, candidateProjectionNs = -1L;
        Throwable failure;

        Motion(Source left, Source right, FlowSlot flow, long id) {
            this.left = left;
            this.right = right;
            this.flow = flow;
            this.id = id;
        }
    }

    private final SourceSlot[] sources = {new SourceSlot(0), new SourceSlot(1), new SourceSlot(2)};
    private final FlowSlot[] flows = {new FlowSlot(), new FlowSlot(), new FlowSlot()};
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Thread owner = Thread.currentThread();
    private Thread renderOwner;
    private boolean sharedDraw;
    private final int frameWidth, frameHeight;
    private final boolean asyncReadback, useAhb;
    private boolean ahbGlUsed, ahbClosed;
    private long estimator, jobIds;
    private Future<Motion> pending;
    private Future<Boolean> retirementCheck;
    private boolean closingAhb;
    private int copyProgram, originalProgram, lumaProgram, warpProgram;
    private int framebuffer, lumaTexture, flowTexture, confidenceTexture, candidateTexture;
    private boolean closed;

    public GlesMotionWarp() {
        this(DEFAULT_WIDTH, DEFAULT_HEIGHT);
    }

    public GlesMotionWarp(int width, int height) {
        this(width, height, false);
    }

    public GlesMotionWarp(int width, int height, boolean asyncReadback) {
        this(width, height, asyncReadback, false);
    }

    public GlesMotionWarp(int width, int height, boolean asyncReadback, boolean useAhb) {
        this.useAhb = useAhb;
        this.asyncReadback = asyncReadback && !useAhb;
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || (long) width * height > 4096L * 2160L) {
            throw new IllegalArgumentException("unsupported source size " + width + "x" + height);
        }
        frameWidth = width;
        frameHeight = height;
        try {
            estimator = useAhb ? QcomMotionNative.createAhb() : QcomMotionNative.create();
            if (estimator == 0) throw new IllegalStateException("Vulkan QCOM block matcher unavailable");
            copyProgram = program(QUAD, COPY);
            originalProgram = program(QUAD, ORIGINAL);
            lumaProgram = program(QUAD, LUMA);
            warpProgram = program(WARP_VERT, WARP_FRAG);
            int[] name = new int[1];
            GLES30.glGenFramebuffers(1, name, 0);
            framebuffer = name[0];
            for (SourceSlot source : sources) {
                source.texture = texture(frameWidth, frameHeight, GLES30.GL_RGBA8, GLES30.GL_LINEAR);
                if (useAhb) {
                    source.ahbSlot = QcomMotionNative.createLumaSlot(estimator, source.slotIndex);
                    if (source.ahbSlot == 0) throw new IllegalStateException("compact luma AHB preflight failed");
                } else if (this.asyncReadback) {
                    source.compactReadback = new CompactLumaReadback(LUMA_W / 4, LUMA_H);
                }
            }
            // Pack four consecutive luma samples into RGBA8 so the guaranteed
            // GL_RGBA/GL_UNSIGNED_BYTE readback is already the exact R8 byte layout.
            if (!useAhb) lumaTexture = texture(LUMA_W / 4, LUMA_H, GLES30.GL_RGBA8, GLES30.GL_NEAREST);
            flowTexture = texture(GRID_W, GRID_H, GLES30.GL_RGBA16F, GLES30.GL_NEAREST);
            confidenceTexture = texture(GRID_W, GRID_H, GLES30.GL_RG8, GLES30.GL_NEAREST);
            candidateTexture = texture(GRID_W, QcomCandidateField.ROWS, GLES30.GL_RGBA16F, GLES30.GL_NEAREST);
            int lumaTarget = useAhb ? QcomMotionNative.lumaTexture(sources[0].ahbSlot) : lumaTexture;
            if (lumaTarget == 0) throw new IllegalStateException("invalid luma attachment");
            bindTarget(lumaTarget, LUMA_W / 4, LUMA_H);
            GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, 0);
            checkGl("warp setup");
        } catch (RuntimeException | Error failure) {
            close();
            throw failure;
        }
    }

    void enableSharedDraw() {
        requireOwner();
        for (SourceSlot slot : sources) slot.fences = new RenderSourceFences(SHARED_FENCES);
        sharedDraw = true;
    }

    boolean readyForSharedDraw(Object frame) {
        requireOwner();
        if (!sharedDraw) throw new IllegalStateException("shared draw is not configured");
        if (frame instanceof Source) return valid(frame).slot.fences.readyForRender();
        Motion motion = (Motion) frame;
        return valid(motion.left).slot.fences.readyForRender()
            && valid(motion.right).slot.fences.readyForRender();
    }

    DrawRelease drawShared(Object frame, int width, int height) {
        if (renderOwner == null) renderOwner = Thread.currentThread();
        if (!sharedDraw || Thread.currentThread() == owner || Thread.currentThread() != renderOwner)
            throw new IllegalStateException("render owner required");
        drawCurrent(frame, width, height);
        Source left = frame instanceof Source ? (Source) frame : ((Motion) frame).left;
        Source right = frame instanceof Source ? null : ((Motion) frame).right;
        long leftFence = 0, rightFence = 0;
        try {
            leftFence = createSharedFence();
            if (right != null) rightFence = createSharedFence();
            GLES30.glFlush();
            checkGl("flush renderer read fences");
            return new DrawRelease(left, right, leftFence, rightFence);
        } catch (RuntimeException | Error failure) {
            // Scheduler keeps both logical holds until positive renderer shutdown.
            if (leftFence != 0) GLES30.glDeleteSync(leftFence);
            if (rightFence != 0) GLES30.glDeleteSync(rightFence);
            throw failure;
        }
    }

    void acceptSharedRelease(DrawRelease release) {
        requireOwner();
        release.left.slot.fences.acceptRead(release.leftFence);
        if (release.right != null) release.right.slot.fences.acceptRead(release.rightFence);
    }

    @Override public Object capture(int oes, float[] transform, long sourceId, long ptsUs) {
        requireOwner();
        if (closingAhb) throw new IllegalStateException("AHB capture is closing");
        for (SourceSlot slot : sources) {
            if (slot.active != null || (slot.compactReadback != null && slot.compactReadback.pending())) continue;
            if (sharedDraw && !slot.fences.retireForReuse()) continue;
            if (useAhb && !slot.allowAhbWrite()) continue;
            if (useAhb) {
                ahbGlUsed = true;
                slot.unprovenProducer = true;
            }
            try {
                bindTarget(slot.texture, frameWidth, frameHeight);
                GLES30.glUseProgram(copyProgram);
                bindSampler(copyProgram, "sourceImage", 0, GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oes);
                GLES30.glUniformMatrix4fv(GLES30.glGetUniformLocation(copyProgram, "sourceTransform"),
                    1, false, transform, 0);
                quad();
                int compactTarget = useAhb ? QcomMotionNative.lumaTexture(slot.ahbSlot) : lumaTexture;
                if (compactTarget == 0) throw new IllegalStateException("lost compact luma attachment");
                bindTarget(compactTarget, LUMA_W / 4, LUMA_H);
                GLES30.glUseProgram(lumaProgram);
                bindSampler(lumaProgram, "sourceImage", 0, GLES30.GL_TEXTURE_2D, slot.texture);
                quad();
                Source captured = new Source(slot, sourceId, ptsUs);
                if (useAhb) {
                    slot.producerFd = QcomMotionNative.publishLumaFence(slot.ahbSlot);
                    if (slot.producerFd < 0) throw new IllegalStateException("compact luma producer fence failed");
                    slot.unprovenProducer = false;
                    captured.timing.lumaPublishedNs = System.nanoTime();
                } else if (asyncReadback) slot.compactReadback.issue(captured.timing);
                else CompactLumaReadback.readSync(LUMA_W / 4, LUMA_H, slot.luma, captured.timing);
                checkGl("capture and compact luma readback");
                if (sharedDraw) slot.fences.publish();
                slot.active = captured;
                return slot.active;
            } finally {
                GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, 0);
            }
        }
        return null;
    }

    @Override public boolean inputsReady(Object left, Object right) {
        requireOwner();
        return compactReady(valid(left)) && compactReady(valid(right));
    }

    private boolean compactReady(Source source) {
        return useAhb ? source.timing.lumaPublishedNs != 0L && source.slot.producerFd >= 0
                      : source.timing.lumaReadyNs != 0L;
    }

    @Override public CaptureTiming captureTiming(Object frame) {
        requireOwner();
        return valid(frame).timing;
    }

    @Override public long submit(Object left, Object right, long deadlineNs) {
        requireOwner();
        if (closingAhb) throw new IllegalStateException("AHB capture is closing");
        if (pending != null || System.nanoTime() >= deadlineNs) return 0L;
        Source first = valid(left), second = valid(right);
        if (!compactReady(first) || !compactReady(second)) return 0L;
        if (first.id >= second.id || first.ptsUs >= second.ptsUs) {
            throw new IllegalArgumentException("invalid source pair");
        }
        for (FlowSlot flow : flows) {
            if (flow.used) continue;
            Motion motion = new Motion(first, second, flow, ++jobIds);
            flow.used = true;
            first.refs++;
            second.refs++;
            try {
                pending = worker.submit(() -> {
                    long started = System.nanoTime();
                    try {
                        // The estimate itself only touches compact buffers and its Vulkan queue.
                        long estimateStarted = System.nanoTime();
                        if (useAhb) {
                            // The two Source references held by Motion keep these exact generations
                            // and their original descriptors alive until positive Vulkan retirement.
                            motion.ahbPending = true;
                            motion.status = QcomMotionNative.estimateAhbCandidates(estimator,
                                first.slot.slotIndex, first.slot.producerFd, second.slot.slotIndex, second.slot.producerFd,
                                flow.raw, flow.nativeCandidates);
                            motion.ahbPending = !QcomMotionNative.ahbRetired(estimator);
                        } else {
                            motion.status = QcomMotionNative.estimateCandidates(estimator, first.slot.luma, second.slot.luma,
                                flow.raw, null, flow.nativeCandidates);
                        }
                        motion.nativeEstimateNs = System.nanoTime() - estimateStarted;
                        if (motion.status == 0) {
                            prepareMidpoint(flow);
                            long projectionStarted = System.nanoTime();
                            motion.status = QcomMotionNative.prepareCandidates(flow.nativeCandidates, flow.midpointCandidates);
                            motion.candidateProjectionNs = System.nanoTime() - projectionStarted;
                        }
                    } catch (RuntimeException | Error failure) {
                        motion.failure = failure;
                    } finally {
                        motion.workNs = System.nanoTime() - started;
                    }
                    return motion;
                });
                return motion.id;
            } catch (RuntimeException | Error failure) {
                releaseMotion(motion);
                throw failure;
            }
        }
        return 0L;
    }

    @Override public Completion poll() {
        requireOwner();
        if (closingAhb) throw new IllegalStateException("AHB capture is closing");
        if (asyncReadback) {
            for (SourceSlot slot : sources) {
                if (!slot.compactReadback.pending()) continue;
                slot.compactReadback.poll(slot.active == null ? null : slot.luma);
            }
        }
        if (pending == null || !pending.isDone()) return null;
        Motion motion = await(pending);
        if (motion.ahbPending) throw new DeferredAhbClose.Unproven("compact luma Vulkan retirement was not proven", motion.failure);
        pending = null;
        if (motion.failure != null) {
            releaseMotion(motion);
            throw new IllegalStateException("Vulkan motion estimation failed", motion.failure);
        }
        if (motion.status != 0) {
            releaseMotion(motion);
            return new Completion(motion.id, motion.status, 0, null, motion.workNs, motion.nativeEstimateNs, motion.candidateProjectionNs);
        }
        return new Completion(motion.id, 0, INFERRED, motion, motion.workNs, motion.nativeEstimateNs, motion.candidateProjectionNs);
    }

    @Override public void draw(Object frame, int width, int height) {
        requireOwner();
        if (frame instanceof Source) valid(frame);
        else {
            Motion motion = (Motion) frame;
            valid(motion.left);
            valid(motion.right);
        }
        drawCurrent(frame, width, height);
    }

    private void drawCurrent(Object frame, int width, int height) {
        if (width != frameWidth || height != frameHeight) {
            throw new IllegalArgumentException("expected native " + frameWidth + "x" + frameHeight + " output");
        }
        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, 0);
        viewport(width, height);
        if (frame instanceof Source) {
            GLES30.glUseProgram(originalProgram);
            bindSampler(originalProgram, "sourceImage", 0, GLES30.GL_TEXTURE_2D, ((Source) frame).slot.texture);
            quad();
        } else {
            Motion motion = (Motion) frame;
            GLES30.glUseProgram(warpProgram);
            bindSampler(warpProgram, "previousImage", 0, GLES30.GL_TEXTURE_2D, motion.left.slot.texture);
            bindSampler(warpProgram, "currentImage", 1, GLES30.GL_TEXTURE_2D, motion.right.slot.texture);
            bindSampler(warpProgram, "motionField", 2, GLES30.GL_TEXTURE_2D, flowTexture);
            motion.flow.positions.clear();
            GLES30.glTexSubImage2D(GLES30.GL_TEXTURE_2D, 0, 0, 0, GRID_W, GRID_H, GLES30.GL_RGBA,
                GLES30.GL_FLOAT, motion.flow.positions);
            bindSampler(warpProgram, "confidenceField", 3, GLES30.GL_TEXTURE_2D, confidenceTexture);
            motion.flow.confidence.clear();
            GLES30.glTexSubImage2D(GLES30.GL_TEXTURE_2D, 0, 0, 0, GRID_W, GRID_H, GLES30.GL_RG,
                GLES30.GL_UNSIGNED_BYTE, motion.flow.confidence);
            bindSampler(warpProgram, "candidateField", 4, GLES30.GL_TEXTURE_2D, candidateTexture);
            motion.flow.midpointCandidates.clear();
            GLES30.glTexSubImage2D(GLES30.GL_TEXTURE_2D, 0, 0, 0, GRID_W, QcomCandidateField.ROWS,
                GLES30.GL_RGBA, GLES30.GL_FLOAT, motion.flow.midpointCandidates);
            GLES30.glDrawArraysInstanced(GLES30.GL_TRIANGLE_STRIP, 0, 4, (GRID_W + 1) * (GRID_H + 1));
        }
        checkGl("full-resolution output warp");
    }

    @Override public void release(Object frame) {
        requireOwner();
        if (frame instanceof Source) releaseSource(valid(frame));
        else releaseMotion((Motion) frame);
    }

    @Override public void close() {
        closeWithFrames(null, System.nanoTime() + DeferredAhbClose.WORKER_WAIT_NS);
    }

    boolean usesAhb() { return useAhb; }

    void closeWithFrames(Runnable releaseFrames, long deadlineNs) {
        if (closed) return;
        requireOwner();
        if (useAhb) closingAhb = true;
        Motion motion = pending == null ? null : useAhb ? awaitAhbRetirement(deadlineNs) : await(pending);
        if (useAhb && ahbGlUsed) {
            if (!QcomMotionNative.drainLumaEgl(sources[0].ahbSlot))
                throw new DeferredAhbClose.Unproven("compact luma GL retirement was not proven");
            ahbGlUsed = false;
        }
        if (releaseFrames != null) releaseFrames.run();
        if (motion != null) {
            releaseMotion(motion);
            pending = null;
        }
        worker.shutdown();
        RuntimeException readbackFailure = null;
        for (SourceSlot slot : sources) {
            if (slot.fences != null) slot.fences.discardAfterRendererStopped();
            if (slot.compactReadback == null) continue;
            try { slot.compactReadback.close(); }
            catch (RuntimeException failure) {
                if (readbackFailure == null) readbackFailure = failure;
                else readbackFailure.addSuppressed(failure);
            }
        }
        if (estimator != 0) {
            QcomMotionNative.destroy(estimator);
            estimator = 0;
        }
        if (useAhb) {
            for (SourceSlot source : sources) {
                if (source.producerFd >= 0) {
                    QcomMotionNative.closeLumaFence(source.producerFd);
                    source.producerFd = -1;
                }
                if (source.ahbSlot != 0 && !QcomMotionNative.destroyLumaSlot(source.ahbSlot))
                    throw new IllegalStateException("compact luma EGL owner changed before retirement");
                source.ahbSlot = 0;
            }
            ahbClosed = true;
        }
        int[] textures = {sources[0].texture, sources[1].texture, sources[2].texture,
            lumaTexture, flowTexture, confidenceTexture, candidateTexture};
        GLES30.glDeleteTextures(textures.length, textures, 0);
        GLES30.glDeleteFramebuffers(1, new int[]{framebuffer}, 0);
        for (int handle : new int[]{copyProgram, originalProgram, lumaProgram, warpProgram}) {
            if (handle != 0) GLES30.glDeleteProgram(handle);
        }
        closed = true;
        if (readbackFailure != null) throw readbackFailure;
    }

    boolean preservesEglOnClose() { return useAhb && !ahbClosed; }

    private Motion awaitAhbRetirement(long deadlineNs) {
        Motion motion = awaitBy(pending, deadlineNs);
        if (motion.ahbPending) {
            if (retirementCheck == null) retirementCheck = worker.submit(() -> QcomMotionNative.ahbRetired(estimator));
            boolean retired;
            try {
                retired = awaitBy(retirementCheck, deadlineNs);
            } catch (DeferredAhbClose.Unproven failure) {
                // Failure from a terminated check can be retried. A timeout or
                // caller interruption leaves its exact Future in flight.
                if (failure.getCause() instanceof ExecutionException) retirementCheck = null;
                throw failure;
            }
            retirementCheck = null;
            if (!retired) throw new DeferredAhbClose.Unproven("compact luma Vulkan retirement was not proven", motion.failure);
            motion.ahbPending = false;
        }
        return motion;
    }

    private static <T> T awaitBy(Future<T> task, long deadlineNs) {
        try {
            return task.get(Math.max(0L, deadlineNs - System.nanoTime()), TimeUnit.NANOSECONDS);
        } catch (TimeoutException timeout) {
            throw new DeferredAhbClose.Unproven("compact luma Vulkan retirement timed out", timeout);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new DeferredAhbClose.Unproven("compact luma Vulkan retirement interrupted", interrupted);
        } catch (ExecutionException failed) {
            throw new DeferredAhbClose.Unproven("compact luma Vulkan retirement was not proven", failed);
        }
    }

    private void requireOwner() {
        if (Thread.currentThread() != owner || closed) throw new IllegalStateException("GL owner required");
    }

    private static Motion await(Future<Motion> job) {
        try {
            return job.get();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("motion worker interrupted", interrupted);
        } catch (ExecutionException failed) {
            throw new IllegalStateException("motion worker failed", failed.getCause());
        }
    }

    private static Source valid(Object alleged) {
        Source source = (Source) alleged;
        if (source.refs <= 0 || source.slot.active != source) throw new IllegalStateException("expired source");
        return source;
    }

    private static void releaseSource(Source source) {
        if (--source.refs == 0) source.slot.active = null;
    }

    private static void releaseMotion(Motion motion) {
        if (!motion.flow.used) throw new IllegalStateException("expired motion result");
        releaseSource(motion.left);
        releaseSource(motion.right);
        motion.flow.used = false;
    }

    private static void prepareMidpoint(FlowSlot slot) {
        FloatBuffer field = slot.raw.asFloatBuffer();
        FloatBuffer out = slot.positions.asFloatBuffer();
        for (int y = 0; y < GRID_H; y++) {
            for (int x = 0; x < GRID_W; x++) {
                final int cell = y * GRID_W + x;
                final float centerX = (x + 0.5f) * TILE, centerY = (y + 0.5f) * TILE;
                for (int direction = 0; direction < 2; direction++) {
                    final int component = direction * 2;
                    final int reverse = 2 - component;
                    float x0 = field.get(cell * 4 + component);
                    float y0 = field.get(cell * 4 + component + 1);
                    int source = nearestCell(centerX - x0 * 0.5f, centerY - y0 * 0.5f);
                    float dx = field.get(source * 4 + component);
                    float dy = field.get(source * 4 + component + 1);
                    int other = nearestCell(centerX + dx * 0.5f, centerY + dy * 0.5f);
                    float ex = dx + field.get(other * 4 + reverse);
                    float ey = dy + field.get(other * 4 + reverse + 1);
                    out.put(cell * 4 + component, dx);
                    out.put(cell * 4 + component + 1, dy);
                    byte weight = (byte) Math.round(255.0f / (1.0f + 0.25f * (ex * ex + ey * ey)));
                    slot.confidence.put(cell * 2 + direction, weight);
                }
            }
        }
    }

    private static int nearestCell(float x, float y) {
        int c = Math.min(Math.max((int) Math.floor(x / TILE), 0), GRID_W - 1);
        int r = Math.min(Math.max((int) Math.floor(y / TILE), 0), GRID_H - 1);
        return r * GRID_W + c;
    }

    private void bindTarget(int image, int width, int height) {
        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, framebuffer);
        GLES30.glFramebufferTexture2D(GLES30.GL_FRAMEBUFFER, GLES30.GL_COLOR_ATTACHMENT0,
            GLES30.GL_TEXTURE_2D, image, 0);
        if (GLES30.glCheckFramebufferStatus(GLES30.GL_FRAMEBUFFER) != GLES30.GL_FRAMEBUFFER_COMPLETE) {
            throw new IllegalStateException("incomplete capture target");
        }
        viewport(width, height);
    }

    private static void viewport(int width, int height) {
        GLES30.glDisable(GLES30.GL_DEPTH_TEST);
        GLES30.glDisable(GLES30.GL_BLEND);
        GLES30.glDisable(GLES30.GL_SCISSOR_TEST);
        GLES30.glViewport(0, 0, width, height);
    }

    private static int texture(int width, int height, int format, int filter) {
        int[] ids = new int[1];
        GLES30.glGenTextures(1, ids, 0);
        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, ids[0]);
        GLES30.glTexStorage2D(GLES30.GL_TEXTURE_2D, 1, format, width, height);
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MIN_FILTER, filter);
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MAG_FILTER, filter);
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE);
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE);
        return ids[0];
    }

    private static void bindSampler(int program, String uniform, int unit, int target, int texture) {
        GLES30.glActiveTexture(GLES30.GL_TEXTURE0 + unit);
        GLES30.glBindTexture(target, texture);
        GLES30.glUniform1i(GLES30.glGetUniformLocation(program, uniform), unit);
    }

    private static void quad() { GLES30.glDrawArrays(GLES30.GL_TRIANGLE_STRIP, 0, 4); }

    private static int program(String vert, String frag) {
        int vs = shader(GLES30.GL_VERTEX_SHADER, vert), fs = 0;
        int program = 0;
        try {
            fs = shader(GLES30.GL_FRAGMENT_SHADER, frag);
            program = GLES30.glCreateProgram();
            GLES30.glAttachShader(program, vs);
            GLES30.glAttachShader(program, fs);
            GLES30.glLinkProgram(program);
            int[] status = new int[1];
            GLES30.glGetProgramiv(program, GLES30.GL_LINK_STATUS, status, 0);
            if (status[0] == 0) throw new IllegalStateException(GLES30.glGetProgramInfoLog(program));
            return program;
        } catch (RuntimeException | Error failure) {
            if (program != 0) GLES30.glDeleteProgram(program);
            throw failure;
        } finally {
            GLES30.glDeleteShader(vs);
            if (fs != 0) GLES30.glDeleteShader(fs);
        }
    }

    private static int shader(int kind, String source) {
        int shader = GLES30.glCreateShader(kind);
        GLES30.glShaderSource(shader, source);
        GLES30.glCompileShader(shader);
        int[] status = new int[1];
        GLES30.glGetShaderiv(shader, GLES30.GL_COMPILE_STATUS, status, 0);
        if (status[0] != 0) return shader;
        String error = GLES30.glGetShaderInfoLog(shader);
        GLES30.glDeleteShader(shader);
        throw new IllegalStateException(error);
    }

    private static void checkGl(String step) {
        int error = GLES30.glGetError();
        if (error != GLES30.GL_NO_ERROR) throw new IllegalStateException(step + " GL error " + error);
    }
}
