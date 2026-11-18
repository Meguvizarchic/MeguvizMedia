#include "qcom_candidate_projection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>

namespace {
constexpr int width = QCOM_MOTION_GRID_WIDTH;
constexpr int height = QCOM_MOTION_GRID_HEIGHT;
constexpr int cells = width * height;
constexpr int candidates = QCOM_MOTION_CANDIDATE_COUNT;
constexpr int outputCandidates = QCOM_ATLAS_OUTPUT_COUNT;
constexpr float tile = static_cast<float>(QCOM_MOTION_TILE);
constexpr float infinity = std::numeric_limits<float>::infinity();
using ValidMasks = uint8_t[2 * cells];

struct Winner {
    int rankIndex = -1;
    float score = infinity;

    void consider(int candidateRank, float candidateScore)
    {
        if (candidateScore < score) {
            rankIndex = candidateRank;
            score = candidateScore;
        }
    }
};

struct ProjectionScratch {
    float support[2 * cells * candidates];
    Winner first[2 * cells];
    Winner second[2 * cells];
};

int cellAxis(float coordinate, int dimension)
{
    // The caller has already checked the continuous endpoint bounds.
    return std::min(static_cast<int>(coordinate / tile), dimension - 1);
}

int cell(float x, float y)
{
    return cellAxis(y, height) * width + cellAxis(x, width);
}

const float *block(const float *raw, int cellIndex, int direction)
{
    return raw + (cellIndex * 2 + direction) * candidates * 4;
}

bool valid(const float *value)
{
    const float photo = value[2];
    return value[3] > 0.5f && photo >= 0.0f && photo <= 1.0f &&
           std::isfinite(value[0]) && std::isfinite(value[1]);
}

float reciprocal(const float *values, uint8_t mask, float dx, float dy, float photo)
{
    float bestSupport = 0.0f;
    for (int rank = 0; rank < candidates; ++rank) {
        if (!(mask & (1u << rank)))
            continue;
        const float *option = values + rank * 4;
        const float ex = dx + option[0], ey = dy + option[1];
        const float cycle = 1.0f / (1.0f + 0.25f * (ex * ex + ey * ey));
        const float worstCost = std::max(photo, option[2]);
        const float photoSupport = std::max(0.0f, std::min(1.0f, (0.18f - worstCost) / 0.15f));
        bestSupport = std::max(bestSupport, cycle * photoSupport);
    }
    return bestSupport;
}

void store(float *atlas, int cellIndex, int direction, int rank, const float *source, float support)
{
    float *out = atlas + ((direction * outputCandidates + rank) * cells + cellIndex) * 4;
    out[0] = source ? source[0] : 0.0f;
    out[1] = source ? source[1] : 0.0f;
    out[2] = source ? source[2] : 1.0f;
    out[3] = support;
}

template <typename Visit>
void splat(float middleX, float middleY, int direction, const Visit &visit)
{
    const float gridX = middleX / tile - 0.5f;
    const float gridY = middleY / tile - 0.5f;
    // The midpoint lies inside the image because both endpoints do. The
    // coordinates relative to the center lattice are therefore at least -0.5.
    const int x0 = gridX < 0.0f ? -1 : static_cast<int>(gridX);
    const int y0 = gridY < 0.0f ? -1 : static_cast<int>(gridY);
    const float fx = gridX - x0, fy = gridY - y0;
    const float wx[2]{1.0f - fx, fx};
    const float wy[2]{1.0f - fy, fy};
    for (int j = 0; j < 2; ++j) {
        const int y = y0 + j;
        if (y < 0 || y >= height)
            continue;
        for (int i = 0; i < 2; ++i) {
            const int x = x0 + i;
            const float coverage = wx[i] * wy[j];
            if (x >= 0 && x < width && coverage > 0.0f)
                visit((y * width + x) * 2 + direction, coverage);
        }
    }
}

void project(const float *raw, const ValidMasks &validMasks, ProjectionScratch &scratch, bool alternatives)
{
    for (int center = 0; center < cells; ++center) {
        const float sx = (center % width + 0.5f) * tile;
        const float sy = (center / width + 0.5f) * tile;
        for (int direction = 0; direction < 2; ++direction) {
            const int sourceGroup = center * 2 + direction;
            const uint8_t sourceMask = validMasks[sourceGroup];
            for (int rank = 0; rank < candidates; ++rank) {
                const int rankIndex = sourceGroup * candidates + rank;
                const float *value = raw + rankIndex * 4;
                float &support = scratch.support[rankIndex];
                if (!alternatives) {
                    support = 0.0f;
                    if (!(sourceMask & (1u << rank)))
                        continue;
                    const float tx = sx + value[0], ty = sy + value[1];
                    if (tx < 0.0f || ty < 0.0f || tx >= QCOM_MOTION_WIDTH || ty >= QCOM_MOTION_HEIGHT)
                        continue;
                    const int reverseCell = cell(tx, ty);
                    const int reverse = 1 - direction;
                    support = reciprocal(block(raw, reverseCell, reverse),
                        validMasks[reverseCell * 2 + reverse], value[0], value[1], value[2]);
                }
                if (support < 0.10f)
                    continue;
                const float baseScore = value[2] + 0.18f * (1.0f - support);
                const float mx = sx + 0.5f * value[0], my = sy + 0.5f * value[1];
                splat(mx, my, direction, [&](int destination, float coverage) {
                    const float score = baseScore + 0.02f * (1.0f - coverage);
                    if (alternatives) {
                        const int firstRank = scratch.first[destination].rankIndex;
                        if (firstRank < 0)
                            return;
                        const float *first = raw + firstRank * 4;
                        const float dx = std::abs(value[0] - first[0]);
                        const float dy = std::abs(value[1] - first[1]);
                        if (std::max(dx, dy) >= 2.0f)
                            scratch.second[destination].consider(rankIndex, score);
                    } else {
                        scratch.first[destination].consider(rankIndex, score);
                    }
                });
            }
        }
    }
}
}

extern "C" int qcom_prepare_candidates(const float *raw, float *atlas)
{
    if (!raw || !atlas)
        return -1;
    const auto rawAddress = reinterpret_cast<uintptr_t>(raw);
    const auto atlasAddress = reinterpret_cast<uintptr_t>(atlas);
    if (rawAddress <= atlasAddress ? atlasAddress - rawAddress < QCOM_ATLAS_RAW_FLOATS * sizeof(float)
                                   : rawAddress - atlasAddress < QCOM_ATLAS_FLOATS * sizeof(float))
        return -1;

    static thread_local std::unique_ptr<ProjectionScratch> workspace;
    if (!workspace)
        workspace.reset(new (std::nothrow) ProjectionScratch);
    if (!workspace)
        return -1;
    auto &scratch = *workspace;
    std::fill_n(scratch.first, 2 * cells, Winner{});
    std::fill_n(scratch.second, 2 * cells, Winner{});
    ValidMasks validMasks;
    for (int group = 0; group < 2 * cells; ++group) {
        uint8_t mask = 0;
        for (int rank = 0; rank < candidates; ++rank) {
            if (valid(raw + (group * candidates + rank) * 4))
                mask |= static_cast<uint8_t>(1u << rank);
        }
        validMasks[group] = mask;
    }
    project(raw, validMasks, scratch, false);
    // Revisit only the footprint. With the final first winner fixed, no
    // duplicate displacement can crowd a distinct candidate out of rank 1.
    project(raw, validMasks, scratch, true);
    for (int center = 0; center < cells; ++center) {
        for (int direction = 0; direction < 2; ++direction) {
            const int group = center * 2 + direction;
            const int first = scratch.first[group].rankIndex;
            const int second = scratch.second[group].rankIndex;
            store(atlas, center, direction, 0, first < 0 ? nullptr : raw + 4 * first,
                  first < 0 ? 0.0f : scratch.support[first]);
            store(atlas, center, direction, 1, second < 0 ? nullptr : raw + 4 * second,
                  second < 0 ? 0.0f : scratch.support[second]);
        }
    }
    return 0;
}
