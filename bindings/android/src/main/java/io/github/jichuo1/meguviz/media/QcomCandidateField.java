package io.github.jichuo1.meguviz.media;

import java.nio.ByteBuffer;
import java.nio.FloatBuffer;

/** Compact-only midpoint candidate preparation for the V6 experimental renderer. */
final class QcomCandidateField {
    static final int GRID_W = 60, GRID_H = 34;
    static final int NATIVE_COUNT = 4, OUTPUT_COUNT = 2;
    static final int ROWS = GRID_H * 2 * OUTPUT_COUNT;
    static final int RAW_BYTES = GRID_W * GRID_H * 2 * NATIVE_COUNT * 4 * Float.BYTES;
    static final int ATLAS_BYTES = GRID_W * ROWS * 4 * Float.BYTES;
    private static final int CELLS = GRID_W * GRID_H;
    private static final int TILE = 8;

    private QcomCandidateField() {}

    private static int rawIndex(int cell, int direction, int rank) {
        return ((cell * 2 + direction) * NATIVE_COUNT + rank) * 4;
    }

    private static int cell(float x, float y) {
        int cx = Math.min(Math.max((int) Math.floor(x / TILE), 0), GRID_W - 1);
        int cy = Math.min(Math.max((int) Math.floor(y / TILE), 0), GRID_H - 1);
        return cy * GRID_W + cx;
    }

    private static boolean valid(FloatBuffer raw, int i) {
        float photo = raw.get(i + 2);
        return raw.get(i + 3) > .5f && photo >= 0.0f && photo <= 1.0f &&
                Float.isFinite(raw.get(i)) && Float.isFinite(raw.get(i + 1));
    }

    private static int selectSource(FloatBuffer raw, int cell, int direction, float seedX, float seedY) {
        int winner = -1;
        float bestScore = Float.POSITIVE_INFINITY;
        for (int rank = 0; rank < NATIVE_COUNT; rank++) {
            int index = rawIndex(cell, direction, rank);
            if (!valid(raw,index)) continue;
            float ax = Math.abs(raw.get(index) - seedX);
            float ay = Math.abs(raw.get(index + 1) - seedY);
            if (Math.max(ax, ay) > 3.0f) continue;
            float score = raw.get(index + 2) + .012f * (ax + ay);
            if (score < bestScore) {
                winner = index;
                bestScore = score;
            }
        }
        return winner;
    }

    private static float reciprocal(FloatBuffer raw, int cell, int direction, float dx, float dy, float photo) {
        float bestSupport = 0.0f;
        for (int rank = 0; rank < NATIVE_COUNT; rank++) {
            int index = rawIndex(cell, 1 - direction, rank);
            if (!valid(raw,index)) continue;
            float ex = dx + raw.get(index), ey = dy + raw.get(index + 1);
            float cycle = 1.0f / (1.0f + .25f * (ex*ex + ey*ey));
            // Cost is mean absolute luminance error, normalized by the native matcher.
            float worstCost = Math.max(photo, raw.get(index + 2));
            float photoSupport = Math.max(0.0f, Math.min(1.0f, (.18f - worstCost) / .15f));
            bestSupport = Math.max(bestSupport, cycle * photoSupport);
        }
        return bestSupport;
    }

    private static void store(FloatBuffer atlas, int cell, int direction, int rank,
                              FloatBuffer raw, int index, float support) {
        int out = ((direction * OUTPUT_COUNT + rank) * CELLS + cell) * 4;
        if (index < 0) {
            atlas.put(out,0.0f);
            atlas.put(out+1,0.0f);
            atlas.put(out+2,1.0f);
            atlas.put(out+3,0.0f);
            return;
        }
        atlas.put(out,raw.get(index));
        atlas.put(out+1,raw.get(index+1));
        atlas.put(out+2,raw.get(index+2));
        atlas.put(out+3,support);
    }

    static void prepare(ByteBuffer rawBytes, ByteBuffer atlasBytes) {
        rawBytes.clear();
        atlasBytes.clear();
        FloatBuffer raw = rawBytes.asFloatBuffer();
        FloatBuffer atlas = atlasBytes.asFloatBuffer();
        int[] sources = new int[NATIVE_COUNT];
        float[] supports = new float[NATIVE_COUNT];
        float[] scores = new float[NATIVE_COUNT];
        for (int center = 0; center < CELLS; center++) {
            float cx = (center % GRID_W + .5f) * TILE, cy = (center / GRID_W + .5f) * TILE;
            for (int direction = 0; direction < 2; direction++) {
                int top = -1;
                float topScore = Float.POSITIVE_INFINITY;
                for (int rank = 0; rank < NATIVE_COUNT; rank++) {
                    sources[rank] = -1;
                    int seed = rawIndex(center,direction,rank);
                    if (!valid(raw,seed)) continue;
                    float seedX = raw.get(seed), seedY = raw.get(seed+1);
                    int sourceCell = cell(cx - .5f*seedX, cy - .5f*seedY);
                    int source = selectSource(raw,sourceCell,direction,seedX,seedY);
                    if (source < 0) continue;
                    float dx = raw.get(source), dy = raw.get(source+1), cost = raw.get(source+2);
                    int reverseCell = cell(cx + .5f*dx, cy + .5f*dy);
                    float support = reciprocal(raw,reverseCell,direction,dx,dy,cost);
                    if (support < .10f) continue;
                    float score = cost + .18f * (1.0f-support);
                    sources[rank] = source;
                    supports[rank] = support;
                    scores[rank] = score;
                    if (score < topScore) {
                        top = rank;
                        topScore = score;
                    }
                }
                int second = -1;
                float secondScore = Float.POSITIVE_INFINITY;
                if (top >= 0) {
                    int first = sources[top];
                    for (int rank = 0; rank < NATIVE_COUNT; rank++) {
                        int source = sources[rank];
                        if (source < 0 || rank == top) continue;
                        float ax = Math.abs(raw.get(source)-raw.get(first));
                        float ay = Math.abs(raw.get(source+1)-raw.get(first+1));
                        if (Math.max(ax,ay) < 2.0f) continue;
                        if (scores[rank] < secondScore) {
                            second = rank;
                            secondScore = scores[rank];
                        }
                    }
                }
                store(atlas,center,direction,0,raw,top < 0 ? -1 : sources[top], top < 0 ? 0 : supports[top]);
                store(atlas,center,direction,1,raw,second < 0 ? -1 : sources[second], second < 0 ? 0 : supports[second]);
            }
        }
    }
}
