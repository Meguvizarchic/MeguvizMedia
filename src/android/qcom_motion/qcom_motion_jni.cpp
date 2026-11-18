#include "qcom_motion.h"
#include "qcom_candidate_projection.h"

#include <cstdint>
#include <jni.h>

namespace {
constexpr jlong pixels = QCOM_MOTION_WIDTH * QCOM_MOTION_HEIGHT;
constexpr jlong cells = QCOM_MOTION_GRID_WIDTH * QCOM_MOTION_GRID_HEIGHT;
constexpr jlong candidateBytes = cells * 2 * QCOM_MOTION_CANDIDATE_COUNT * 4 * sizeof(float);

void *direct(JNIEnv *env, jobject buffer, jlong bytes)
{
    if (!buffer || env->GetDirectBufferCapacity(buffer) < bytes)
        return nullptr;
    return env->GetDirectBufferAddress(buffer);
}

float *directFloats(JNIEnv *env, jobject buffer, jlong bytes)
{
    auto *result = static_cast<float *>(direct(env, buffer, bytes));
    return reinterpret_cast<uintptr_t>(result) % alignof(float) == 0 ? result : nullptr;
}
}

extern "C" JNIEXPORT jlong JNICALL
Java_io_github_jichuo1_meguviz_media_QcomMotionNative_create(JNIEnv *, jclass)
{
    return reinterpret_cast<jlong>(qcom_motion_create(0));
}

extern "C" JNIEXPORT jint JNICALL
Java_io_github_jichuo1_meguviz_media_QcomMotionNative_estimate(JNIEnv *env, jclass, jlong handle, jobject previous,
                                                              jobject current, jobject vectors, jobject confidence)
{
    auto *previousBytes = static_cast<const uint8_t *>(direct(env, previous, pixels));
    auto *currentBytes = static_cast<const uint8_t *>(direct(env, current, pixels));
    auto *vectorBytes = static_cast<float *>(direct(env, vectors, cells * 4 * sizeof(float)));
    auto *confidenceBytes = static_cast<float *>(direct(env, confidence, cells * 2 * sizeof(float)));
    if ((confidence && !confidenceBytes) ||
        (vectorBytes && reinterpret_cast<uintptr_t>(vectorBytes) % alignof(float)) ||
        (confidenceBytes && reinterpret_cast<uintptr_t>(confidenceBytes) % alignof(float)))
        return -1;
    return qcom_motion_estimate(reinterpret_cast<QcomMotion *>(handle), previousBytes, currentBytes,
                                vectorBytes, confidenceBytes, nullptr);
}

extern "C" JNIEXPORT void JNICALL
Java_io_github_jichuo1_meguviz_media_QcomMotionNative_destroy(JNIEnv *, jclass, jlong handle)
{
    qcom_motion_destroy(reinterpret_cast<QcomMotion *>(handle));
}

extern "C" JNIEXPORT jint JNICALL
Java_io_github_jichuo1_meguviz_media_QcomMotionNative_estimateCandidates(
    JNIEnv *env, jclass, jlong handle, jobject previous, jobject current, jobject vectors,
    jobject confidence, jobject candidates)
{
    const auto *previousBytes = static_cast<const uint8_t *>(direct(env, previous, pixels));
    const auto *currentBytes = static_cast<const uint8_t *>(direct(env, current, pixels));
    auto *vectorBytes = directFloats(env, vectors, cells * 4 * sizeof(float));
    auto *confidenceBytes = directFloats(env, confidence, cells * 2 * sizeof(float));
    auto *candidateOutput = directFloats(env, candidates, candidateBytes);
    if (confidence && !confidenceBytes)
        return -1;
    return qcom_motion_estimate_candidates(reinterpret_cast<QcomMotion *>(handle), previousBytes,
        currentBytes, vectorBytes, confidenceBytes, candidateOutput, nullptr, QCOM_MOTION_CANDIDATE_WIDE);
}

extern "C" JNIEXPORT jint JNICALL
Java_io_github_jichuo1_meguviz_media_QcomMotionNative_prepareCandidates(
    JNIEnv *env, jclass, jobject raw, jobject atlas)
{
    return qcom_prepare_candidates(directFloats(env, raw, QCOM_ATLAS_RAW_FLOATS * sizeof(float)),
                                   directFloats(env, atlas, QCOM_ATLAS_FLOATS * sizeof(float)));
}
