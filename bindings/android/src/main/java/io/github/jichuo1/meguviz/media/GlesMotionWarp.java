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
    static final class Unsupported extends IllegalStateException {
        Unsupported(String message) { super(message); }
    }
    private static final int DEFAULT_WIDTH = 3840, DEFAULT_HEIGHT = 2160;
    private static final int LUMA_W = 480, LUMA_H = 272, GRID_W = 60, GRID_H = 34, TILE = 8;
    private static final int GRID_CELLS = GRID_W * GRID_H;
    private static final int INFERRED = 2;

    /**
     * Opt-in render fixed-cost switches. Zero is the frozen behaviour: {@link #WARP_FRAG} is
     * compiled verbatim and no extra render target is created, so the default path stays
     * byte-identical to the shipped shader.
     * <p>This build generates the rank family, the census instrument, and the two estimator-prior
     * switches the native layer now supports: {@code FC_VISIBILITY_GATE} (clear the
     * {@code alternativeMotion()} subtree for cells the estimator's bidirectional visibility says
     * cannot need it) and {@code FC_NATIVE_STATIC_GATE} (force zero motion on cells the fine stage
     * classified static). {@code FC_STATIC_PRIOR} stays rejected: F measured it as a −3.51 dB
     * regression in the subtitle ROI because its whole-cell static classification erases strokes.
     */
    static final int FC_CENSUS = 128;
    static final int FC_RANK1_SHARE = 512;
    static final int FC_RANK1_ALU = 1024;
    /** Clears the alternative-motion subtree per cell from the estimator's visibility prior. */
    static final int FC_VISIBILITY_GATE = 8;
    /** Consumes the estimator's static classification: zero motion on static cells. */
    static final int FC_NATIVE_STATIC_GATE = 16;
    /** Bits this build can generate. */
    static final int FC_SUPPORTED = FC_CENSUS | FC_RANK1_SHARE | FC_RANK1_ALU
        | FC_VISIBILITY_GATE | FC_NATIVE_STATIC_GATE;
    /** Every bit the render-fixed-cost lineage defines, so unknown bits stay distinguishable. */
    private static final int FC_STATIC_MASK = 1, FC_SPARSE_CONFLICT = 2, FC_STATIC_PRIOR = 4,
        FC_ATLAS_DEAD = 32, FC_PROPOSAL_CUT = 64, FC_NO_ALT_PROBE = 256;
    private static final int FC_KNOWN = FC_STATIC_MASK | FC_SPARSE_CONFLICT | FC_STATIC_PRIOR
        | FC_VISIBILITY_GATE | FC_NATIVE_STATIC_GATE | FC_ATLAS_DEAD | FC_PROPOSAL_CUT
        | FC_CENSUS | FC_NO_ALT_PROBE | FC_RANK1_SHARE | FC_RANK1_ALU;
    /** Switches measured as no-gain, harmful on Adreno, diagnostic-only, or explicitly rejected. */
    private static final int FC_EXCLUDED = FC_STATIC_MASK | FC_SPARSE_CONFLICT | FC_STATIC_PRIOR
        | FC_ATLAS_DEAD | FC_PROPOSAL_CUT | FC_NO_ALT_PROBE;
    /**
     * Visibility gate cut. A cell keeps the conflict-resolution subtree when either direction's
     * visibility is below {@link #GATE_LOW} or the two directions disagree by more than
     * {@link #GATE_ASYMMETRY}. A one-sided trajectory is exactly the case the atlas exists for,
     * so asymmetry alone keeps the cell live even when both directions look individually clean.
     */
    static final float GATE_LOW = 0.25f, GATE_ASYMMETRY = 0.20f;
    /** Support thresholds the atlas consumers use; the census reads the same cuts. */
    static final float PAIR_CUT = 0.42f, SIDE_CUT = 0.58f;
    /**
     * Slack covering the RGBA16F round trip between the packed atlas and the shader. The atlas is
     * uploaded as half floats, so a value the CPU reads as {@link #PAIR_CUT} can land marginally
     * above it in the texture. Classifying only strictly below each consumer threshold keeps the
     * CPU's dead set a subset of the shader's provably-dead set.
     */
    static final float SUPPORT_SAFE = 1.0f / 512.0f;

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

    // ---- Render fixed-cost variants ------------------------------------------------
    // The frozen literal above is never edited in place. Each opt-in switch performs exactly one
    // substitution on that literal and fails construction if the anchor is missing or repeated,
    // so an "off" build provably compiles the untouched baseline source.
    /**
     * The two-rank candidate fill. Replacing it with the single-rank form keeps the array
     * declarations and every downstream index intact, so only the amount of work changes.
     */
    private static final String ANCHOR_RANK_FILL =
        "    for (int n=0; n<2; ++n) {\n"
        + "        left[n] = proposal(cell,0,n);\n"
        + "        right[n] = proposal(cell,1,n);\n"
        + "    }\n";
    private static final String RANK1_SHARED_FILL =
        "    left[0] = proposal(cell,0,0);\n"
        + "    right[0] = proposal(cell,1,0);\n"
        + "    left[1] = left[0];\n"
        + "    right[1] = right[0];\n";
    /** Rank-0-only pair and side loops; same expressions, one iteration each. */
    private static final String ANCHOR_PAIR_LOOPS =
        "    for (int a=0; a<2; ++a) {\n"
        + "        for (int b=0; b<2; ++b) {\n";
    private static final String RANK0_PAIR_LOOP =
        "    for (int a=0; a<1; ++a) {\n"
        + "        for (int b=0; b<1; ++b) {\n";
    private static final String ANCHOR_SIDE_LOOP =
        "    for (int n=0; n<2; ++n) {\n"
        + "        float vL = visibility(left[n],peakRight,originalLeft,sourceChange);\n";
    private static final String RANK0_SIDE_LOOP =
        "    for (int n=0; n<1; ++n) {\n"
        + "        float vL = visibility(left[n],peakRight,originalLeft,sourceChange);\n";
    private static final String ANCHOR_CANDIDATE_UNIFORM = "uniform highp sampler2D candidateField;\n";
    private static final String ANCHOR_VIEW_CANDIDATE = "struct ViewCandidate {\n";
    private static final String ANCHOR_ALTERNATIVE_COND =
        "    if (conflict && disagreement > 24.0/255.0 &&\n"
        + "        alternativeMotion(originalLeft,originalRight,disagreement,fallback,alternative)) {\n";
    private static final String GATED_ALTERNATIVE_COND =
        "    if (conflict && disagreement > 24.0/255.0 && cellConflict() &&\n"
        + "        alternativeMotion(originalLeft,originalRight,disagreement,fallback,alternative)) {\n";
    /**
     * The cell-level conflict predicate: one RG8 fetch, uniform across each cell's interior. A
     * set byte is a cell the estimator's visibility prior could not rule out, which is exactly
     * the {@code alternativeMotion()} subtree's live set.
     */
    private static final String CELL_CONFLICT_HELPER =
        "bool cellConflict() {\n"
        + "    ivec2 cell = clamp(ivec2(floor(uv * GRID)), ivec2(0), ivec2(59, 33));\n"
        + "    return texelFetch(cellPrior, cell, 0).g > 0.5;\n"
        + "}\n"
        + "\n";

    private static String substitute(String source, String anchor, String replacement, String label) {
        int first = source.indexOf(anchor);
        if (first < 0) throw new IllegalStateException("missing shader anchor: " + label);
        if (source.indexOf(anchor, first + 1) >= 0) throw new IllegalStateException("ambiguous shader anchor: " + label);
        return source.substring(0, first) + replacement + source.substring(first + anchor.length());
    }

    /**
     * The frozen fragment with the requested switches. With {@code options == 0} the result is
     * {@link #WARP_FRAG} itself, which is what keeps the default path byte-exact.
     */
    static String warpFragment(int width, int height, int options) {
        String fragment = WARP_FRAG;
        boolean prior = false;
        if ((options & FC_VISIBILITY_GATE) != 0) {
            fragment = substitute(fragment, ANCHOR_ALTERNATIVE_COND, GATED_ALTERNATIVE_COND,
                "alternative-motion gate");
            fragment = substitute(fragment, ANCHOR_VIEW_CANDIDATE,
                CELL_CONFLICT_HELPER + ANCHOR_VIEW_CANDIDATE, "cell conflict helper");
            prior = true;
        }
        if ((options & FC_RANK1_SHARE) != 0) {
            fragment = substitute(fragment, ANCHOR_RANK_FILL, RANK1_SHARED_FILL, "rank-1 colour share");
        }
        if ((options & FC_RANK1_ALU) != 0) {
            fragment = substitute(fragment, ANCHOR_PAIR_LOOPS, RANK0_PAIR_LOOP, "pair loop collapse");
            fragment = substitute(fragment, ANCHOR_SIDE_LOOP, RANK0_SIDE_LOOP, "side loop collapse");
        }
        if (prior) {
            fragment = substitute(fragment, ANCHOR_CANDIDATE_UNIFORM,
                ANCHOR_CANDIDATE_UNIFORM + "uniform highp sampler2D cellPrior;\n", "cell prior uniform");
        }
        return fragment;
    }

    static String warpFragment(int width, int height, int outputWidth, int outputHeight, int options) {
        String fragment = warpFragment(width, height, options);
        if (width == outputWidth && height == outputHeight) return fragment;
        // The static-neighborhood texel fetch needs source coordinates when drawing into a scaled viewport.
        return substitute(fragment, "ivec2 pixel = ivec2(gl_FragCoord.xy);",
            "ivec2 pixel = clamp(ivec2(uv * vec2(" + width + ".0, " + height
                + ".0)), ivec2(0), ivec2(" + (width - 1) + ", " + (height - 1) + "));",
            "static-neighborhood source texel");
    }

    /** Host/reference name for a shader literal so build receipts can identify exact bytes. */
    static String literal(String name) {
        switch (name) {
            case "QUAD": return QUAD;
            case "COPY": return COPY;
            case "ORIGINAL": return ORIGINAL;
            case "LUMA": return LUMA;
            case "WARP_VERT": return WARP_VERT;
            case "WARP_FRAG": return WARP_FRAG;
            default: throw new IllegalArgumentException("unknown shader " + name);
        }
    }

    /** The options this build can actually render, for callers that probe capability. */
    static int supportedRenderOptions() { return FC_SUPPORTED; }

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
        /**
         * Cell-level priors from the estimator, allocated only when a switch asked for them.
         * <p>{@code visibility} drives the gate. {@code staticMask} is read for the occupancy
         * receipt only: in ws-peak the static exemption of the gate rule exists only under
         * {@code FC_STATIC_PRIOR}, which this build rejects, so an opt8 build's gate is exactly
         * visibility-only and its R byte stays zero.
         */
        final ByteBuffer staticMask, visibility;
        /** RG8 cell-prior texture payload: R static classification, G conflict subtree enabled. */
        final ByteBuffer cellPrior;
        int staticCells = -1, conflictCells = -1;
        /** Ten-bucket histogram of min(forward, backward) visibility, in tenths. */
        final int[] visibilityHistogram = new int[10];
        /** Ten-bucket histogram of the cell's strongest atlas support, in tenths. */
        final int[] atlasSupportHistogram = new int[10];
        boolean used;

        FlowSlot(boolean staticMaskWanted, boolean gate) {
            cellPrior = gate ? buffer(GRID_CELLS * 2) : null;
            staticMask = staticMaskWanted ? buffer(GRID_CELLS) : null;
            visibility = gate ? buffer(GRID_CELLS * 2 * Float.BYTES) : null;
        }

        /** True when the extended estimate entry point is needed to fill a prior plane. */
        boolean wantsExtended() { return staticMask != null || visibility != null; }
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
        /** Snapshot of this pair's atlas census, or null when no census was requested. */
        int[] census;
        Throwable failure;

        Motion(Source left, Source right, FlowSlot flow, long id) {
            this.left = left;
            this.right = right;
            this.flow = flow;
            this.id = id;
        }
    }

    private final SourceSlot[] sources = {new SourceSlot(0), new SourceSlot(1), new SourceSlot(2)};
    private final FlowSlot[] flows;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Thread owner = Thread.currentThread();
    private Thread renderOwner;
    private boolean sharedDraw;
    private final int frameWidth, frameHeight, outputWidth, outputHeight;
    private final boolean asyncReadback, useAhb, wantsVisibilityGate, wantsNativeStaticGate;
    /** True when the RG8 cell-prior payload and texture are needed (the visibility gate). */
    private final boolean wantsCellPrior;
    private final int renderOptions;
    /** Census reads the same projected atlas the shader fetches, so it runs only when asked. */
    private final boolean wantsCensus;
    private boolean ahbGlUsed, ahbClosed;
    private long estimator, jobIds;
    private Future<Motion> pending;
    private Future<Boolean> retirementCheck;
    private boolean closingAhb;
    private int copyProgram, originalProgram, lumaProgram, warpProgram;
    private int framebuffer, lumaTexture, flowTexture, confidenceTexture, candidateTexture;
    private int cellPriorTexture;
    /** Retained only for the census and prior-occupancy receipts; holds no ownership. */
    private Motion lastDrawn;
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
        this(width, height, asyncReadback, useAhb, 0);
    }

    public GlesMotionWarp(int width, int height, boolean asyncReadback, boolean useAhb, int renderOptions) {
        this(width, height, width, height, asyncReadback, useAhb, renderOptions);
    }

    GlesMotionWarp(int width, int height, int outputWidth, int outputHeight,
                   boolean asyncReadback, boolean useAhb, int renderOptions) {
        this.useAhb = useAhb;
        this.asyncReadback = asyncReadback && !useAhb;
        if (renderOptions < 0 || (renderOptions & ~FC_KNOWN) != 0) {
            throw new IllegalArgumentException("unsupported render options " + renderOptions);
        }
        if ((renderOptions & FC_EXCLUDED) != 0) {
            // Measured as zero-gain, structurally harmful on Adreno, diagnostic-only, or rejected
            // on quality: 32/64 gave no device gain, 96 regressed to 30-36 ms, 1/2 changed the
            // sparse path, 256 deletes the correction outright, and 4 lost 3.51 dB on subtitles.
            throw new IllegalArgumentException(
                "render options " + renderOptions + " are not shipped by this backend");
        }
        this.renderOptions = renderOptions;
        wantsCensus = (renderOptions & FC_CENSUS) != 0;
        wantsVisibilityGate = (renderOptions & FC_VISIBILITY_GATE) != 0;
        wantsNativeStaticGate = (renderOptions & FC_NATIVE_STATIC_GATE) != 0;
        wantsCellPrior = wantsVisibilityGate;
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || (long) width * height > 4096L * 2160L) {
            throw new IllegalArgumentException("unsupported source size " + width + "x" + height);
        }
        frameWidth = width;
        frameHeight = height;
        if (outputWidth <= 0 || outputHeight <= 0) throw new IllegalArgumentException("invalid output size");
        this.outputWidth = outputWidth;
        this.outputHeight = outputHeight;
        // A FlowSlot allocates the prior planes only when a switch asked for them, so the default
        // path's working set is exactly the pre-existing one. The static plane is requested when
        // the native static gate is on, because the occupancy receipt then reports its cell count.
        flows = new FlowSlot[]{new FlowSlot(wantsNativeStaticGate, wantsVisibilityGate),
            new FlowSlot(wantsNativeStaticGate, wantsVisibilityGate),
            new FlowSlot(wantsNativeStaticGate, wantsVisibilityGate)};
        try {
            estimator = useAhb ? QcomMotionNative.createAhb() : QcomMotionNative.create();
            if (estimator == 0) throw new Unsupported("Vulkan QCOM block matcher unavailable");
            if (wantsNativeStaticGate) {
                // The classifier's own defaults, set explicitly so the gate cannot inherit a
                // caller's earlier thresholds on this estimator handle.
                QcomMotionNative.setStaticThresholds(estimator, 2, 10);
                QcomMotionNative.setStaticGate(estimator, true);
            }
            copyProgram = program(QUAD, COPY);
            originalProgram = program(QUAD, ORIGINAL);
            lumaProgram = program(QUAD, LUMA);
            // Options zero compiles the frozen literal verbatim; a switch rewrites only the
            // expression it needs, so "off" cannot drift away from the baseline source.
            warpProgram = program(WARP_VERT, warpFragment(frameWidth, frameHeight, outputWidth, outputHeight, renderOptions));
            int[] name = new int[1];
            GLES30.glGenFramebuffers(1, name, 0);
            framebuffer = name[0];
            for (SourceSlot source : sources) {
                source.texture = texture(frameWidth, frameHeight, GLES30.GL_RGBA8, GLES30.GL_LINEAR);
                if (useAhb) {
                    source.ahbSlot = QcomMotionNative.createLumaSlot(estimator, source.slotIndex);
                    if (source.ahbSlot == 0) throw new Unsupported("compact luma AHB preflight failed");
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
            if (wantsCellPrior) cellPriorTexture = texture(GRID_W, GRID_H, GLES30.GL_RG8, GLES30.GL_NEAREST);
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
                            if (flow.wantsExtended()) {
                                // Extended entry point: same estimate, plus the estimator's own
                                // cell-level static classification and bidirectional visibility.
                                // Confidence stays off because the shader consumes the packed byte
                                // texture prepareMidpoint() builds, so the float plane would be
                                // dead weight read back on the worker thread.
                                motion.status = QcomMotionNative.estimateAhbExtended(estimator,
                                    first.slot.slotIndex, first.slot.producerFd, second.slot.slotIndex, second.slot.producerFd,
                                    flow.raw, null, flow.nativeCandidates,
                                    flow.staticMask, flow.visibility);
                            } else {
                                motion.status = QcomMotionNative.estimateAhbCandidates(estimator,
                                    first.slot.slotIndex, first.slot.producerFd, second.slot.slotIndex, second.slot.producerFd,
                                    flow.raw, flow.nativeCandidates);
                            }
                            motion.ahbPending = !QcomMotionNative.ahbRetired(estimator);
                        } else {
                            if (flow.wantsExtended()) {
                                motion.status = QcomMotionNative.estimateExtended(estimator,
                                    first.slot.luma, second.slot.luma, flow.raw, null, flow.nativeCandidates,
                                    flow.staticMask, flow.visibility);
                            } else {
                                motion.status = QcomMotionNative.estimateCandidates(estimator,
                                    first.slot.luma, second.slot.luma, flow.raw, null, flow.nativeCandidates);
                            }
                        }
                        motion.nativeEstimateNs = System.nanoTime() - estimateStarted;
                        if (motion.status == 0) {
                            prepareMidpoint(flow);
                            long projectionStarted = System.nanoTime();
                            motion.status = QcomMotionNative.prepareCandidates(flow.nativeCandidates, flow.midpointCandidates);
                            motion.candidateProjectionNs = System.nanoTime() - projectionStarted;
                            // The census and the gate both read the projected atlas, so they run
                            // after projection: the readings are then the exact values the shader
                            // will fetch rather than the raw native candidates.
                            if (motion.status == 0 && (flow.cellPrior != null || flow.staticMask != null))
                                packCellPrior(flow);
                            if (motion.status == 0 && wantsCensus) motion.census = atlasCensus(flow);
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
        if (width != outputWidth || height != outputHeight) {
            throw new IllegalArgumentException("expected " + outputWidth + "x" + outputHeight + " output");
        }
        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, 0);
        viewport(width, height);
        if (frame instanceof Source) {
            GLES30.glUseProgram(originalProgram);
            bindSampler(originalProgram, "sourceImage", 0, GLES30.GL_TEXTURE_2D, ((Source) frame).slot.texture);
            quad();
        } else {
            Motion motion = (Motion) frame;
            lastDrawn = motion;
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
            if (cellPriorTexture != 0) {
                // glTexSubImage2D writes to the texture bound on the active unit, so each upload
                // binds its target first; the same binding serves the draw that follows.
                bindSampler(warpProgram, "cellPrior", 5, GLES30.GL_TEXTURE_2D, cellPriorTexture);
                motion.flow.cellPrior.clear();
                GLES30.glTexSubImage2D(GLES30.GL_TEXTURE_2D, 0, 0, 0, GRID_W, GRID_H,
                    GLES30.GL_RG, GLES30.GL_UNSIGNED_BYTE, motion.flow.cellPrior);
            }
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

    /** The exact render options this instance compiled, for run receipts. */
    int renderOptions() { return renderOptions; }

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
            lumaTexture, flowTexture, confidenceTexture, candidateTexture, cellPriorTexture};
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

    /**
     * Per-pair census snapshot of the projected atlas: cells that keep the conflict subtree, cells
     * the atlas cannot serve, surviving candidate colour fetches, then the strongest-support
     * buckets. This answers, per frame, whether a slow stretch crosses more cells into the atlas
     * path or makes each cell cost more. It never changes the shader or the drawn result.
     */
    private static int[] atlasCensus(FlowSlot slot) {
        FloatBuffer atlas = slot.midpointCandidates.asFloatBuffer();
        int[] histogram = new int[10];
        int dead = 0, live = 0;
        for (int cell = 0; cell < GRID_CELLS; cell++) {
            float left = 0.0f, right = 0.0f;
            for (int rank = 0; rank < QcomCandidateField.OUTPUT_COUNT; rank++) {
                float l = atlas.get((rank * GRID_CELLS + cell) * 4 + 3);
                float r = atlas.get(((QcomCandidateField.OUTPUT_COUNT + rank) * GRID_CELLS + cell) * 4 + 3);
                left = Math.max(left, l);
                right = Math.max(right, r);
                // A fetch survives the 0.42 cut only when its own support can still be observed.
                if (l > PAIR_CUT) live++;
                if (r > PAIR_CUT) live++;
            }
            float best = Math.max(left, right);
            // No pair can proceed (the loop needs min(left,right) > PAIR_CUT) and no single
            // candidate can reach the visibility support term (> SIDE_CUT).
            boolean unreachable = (left <= PAIR_CUT - SUPPORT_SAFE || right <= PAIR_CUT - SUPPORT_SAFE)
                && best <= SIDE_CUT - SUPPORT_SAFE;
            if (unreachable) dead++;
            histogram[Math.min(9, Math.max(0, (int) (best * 10.0f)))]++;
        }
        int[] result = new int[6 + 10];
        result[0] = GRID_CELLS - dead;
        result[1] = dead;
        result[4] = live;
        result[5] = GRID_CELLS * 4;
        System.arraycopy(histogram, 0, result, 6, 10);
        return result;
    }

    /**
     * Census of the most recently drawn pair, or null when {@link #FC_CENSUS} is off and before
     * any warp draw. Layout: 0 conflict cells, 1 atlas-dead cells, 4 surviving candidate fetches,
     * 5 the four-per-cell total, 6..15 the strongest-support histogram in tenths.
     */
    int[] lastCensus() {
        Motion motion = lastDrawn;
        return motion == null ? null : motion.census;
    }

    /**
     * Packs the estimator's cell-level priors into the single RG8 texel the warp reads: R is the
     * static classification, G is one when the cell must keep the conflict-resolution subtree.
     * A cell keeps it when either direction's bidirectional visibility is low or the two directions
     * disagree; see {@link #GATE_LOW} and {@link #GATE_ASYMMETRY}.
     * <p>Only the gate has a shader consumer, so an opt16-only run does no atlas scan here: it
     * counts the static cells for the occupancy receipt and stops.
     */
    private static void packCellPrior(FlowSlot slot) {
        FloatBuffer visibility = slot.visibility == null ? null : slot.visibility.asFloatBuffer();
        int staticCells = 0;
        if (slot.cellPrior == null) {
            if (slot.staticMask != null) {
                for (int cell = 0; cell < GRID_CELLS; cell++) staticCells += slot.staticMask.get(cell) != 0 ? 1 : 0;
            }
            slot.staticCells = staticCells;
            return;
        }
        slot.cellPrior.clear();
        // Per-pair census: every bucket describes this pair only. Reusing a slot three times
        // would otherwise accumulate the previous pairs into the same buckets.
        java.util.Arrays.fill(slot.visibilityHistogram, 0);
        java.util.Arrays.fill(slot.atlasSupportHistogram, 0);
        FloatBuffer atlas = slot.midpointCandidates.asFloatBuffer();
        int conflictCells = 0;
        for (int cell = 0; cell < GRID_CELLS; cell++) {
            int isStatic = slot.staticMask != null && slot.staticMask.get(cell) != 0 ? 1 : 0;
            int conflict = 1;
            if (visibility != null) {
                float forward = visibility.get(cell), backward = visibility.get(cell + GRID_CELLS);
                float weak = Math.min(forward, backward);
                conflict = weak < GATE_LOW || Math.abs(forward - backward) > GATE_ASYMMETRY ? 1 : 0;
            }
            float left = 0.0f, right = 0.0f;
            for (int rank = 0; rank < QcomCandidateField.OUTPUT_COUNT; rank++) {
                left = Math.max(left, atlas.get((rank * GRID_CELLS + cell) * 4 + 3));
                right = Math.max(right, atlas.get(((QcomCandidateField.OUTPUT_COUNT + rank) * GRID_CELLS + cell) * 4 + 3));
            }
            staticCells += isStatic;
            conflictCells += conflict;
            if (visibility != null) {
                float weak = Math.min(visibility.get(cell), visibility.get(cell + GRID_CELLS));
                slot.visibilityHistogram[Math.min(9, Math.max(0, (int) (weak * 10.0f)))]++;
            }
            slot.atlasSupportHistogram[Math.min(9, Math.max(0, (int) (Math.max(left, right) * 10.0f)))]++;
            slot.cellPrior.put((byte) (isStatic * 255)).put((byte) (conflict * 255));
        }
        slot.staticCells = staticCells;
        slot.conflictCells = conflictCells;
        slot.cellPrior.clear();
    }

    /**
     * Prior occupancy of the most recently drawn pair, for run receipts. Bit 8 (the gate) reports
     * the conflict figures; bit 16 (the native static gate) reports the static figure. Every gate
     * figure is -1 when the gate is off.
     * <p>Layout: 0 static cells, 1 conflict cells, 2 total cells, 3..12 the minimum-visibility
     * histogram in tenths, 13..22 the strongest-atlas-support histogram in tenths.
     */
    int[] lastPriorOccupancy() {
        Motion motion = lastDrawn;
        int[] result = new int[3 + 10 + 10];
        boolean live = motion != null;
        result[0] = !live || !wantsNativeStaticGate ? -1 : motion.flow.staticCells;
        result[1] = !live || !wantsCellPrior ? -1 : motion.flow.conflictCells;
        result[2] = GRID_CELLS;
        if (live) {
            System.arraycopy(motion.flow.visibilityHistogram, 0, result, 3, 10);
            System.arraycopy(motion.flow.atlasSupportHistogram, 0, result, 13, 10);
        }
        return result;
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
