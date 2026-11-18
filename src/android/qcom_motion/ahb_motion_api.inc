extern "C" QcomMotion *qcom_motion_create_ahb(int metric)
{
    if (metric != 0 && metric != 1) return nullptr;
    try {
        auto motion = std::make_unique<QcomMotion>();
        applyStaticOverrides(*motion);
        motion->initialize(metric, true);
        return motion.release();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "QCOM AHB preflight: %s\n", error.what());
        return nullptr;
    }
}

extern "C" int qcom_motion_bind_luma_ahb(QcomMotion *motion, unsigned slot, AHardwareBuffer *buffer)
{
    if (!motion || !motion->ahb || motion->failed || motion->ahbSubmissionUnknown) return -1;
    try {
        motion->ahb->attach(slot, buffer);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "QCOM AHB import: %s\n", error.what());
        return -2;
    }
}

extern "C" int qcom_motion_estimate_ahb(QcomMotion *motion, QcomAhbFrame left, QcomAhbFrame right,
                                      float *vectors, float *confidence, float *candidates,
                                      QcomMotionTiming *timing, int candidateMode)
{
    if (!motion || !motion->ahb || !vectors || !candidates ||
        (candidateMode != QCOM_MOTION_CANDIDATE_LOCAL && candidateMode != QCOM_MOTION_CANDIDATE_WIDE))
        return -1;
    if (motion->failed || motion->ahbSubmissionUnknown) return -2;
    try {
        const QcomAhbFrame inputs[]{left, right};
        /* The frozen AHB entry point does not surface the prototype's static
         * mask or visibility; qcom_motion_estimate_ahb_extended does. */
        motion->estimate(nullptr, nullptr, vectors, confidence, candidates, nullptr, nullptr,
                         timing, candidateMode, &inputs);
        return 0;
    } catch (const std::exception &error) {
        motion->failed = true;
        std::fprintf(stderr, "QCOM AHB estimate: %s\n", error.what());
        return -3;
    }
}

/* AHB counterpart of qcom_motion_estimate_extended.  Same semantics and the same
 * optional outputs; staticMask and visibility are read back from the device
 * buffers the AHB path already allocates, so no extra Vulkan resource is needed. */
extern "C" int qcom_motion_estimate_ahb_extended(QcomMotion *motion, QcomAhbFrame left,
                                                QcomAhbFrame right, float *vectors,
                                                float *confidence, float *candidates,
                                                uint8_t *staticMask, float *visibility,
                                                QcomMotionTiming *timing, int candidateMode)
{
    if (!motion || !motion->ahb || !vectors ||
        (candidateMode != QCOM_MOTION_CANDIDATE_LOCAL && candidateMode != QCOM_MOTION_CANDIDATE_WIDE))
        return -1;
    if (motion->failed || motion->ahbSubmissionUnknown) return -2;
    try {
        const QcomAhbFrame inputs[]{left, right};
        motion->estimate(nullptr, nullptr, vectors, confidence, candidates, staticMask, visibility,
                         timing, candidateMode, &inputs);
        return 0;
    } catch (const std::exception &error) {
        motion->failed = true;
        std::fprintf(stderr, "QCOM AHB extended estimate: %s\n", error.what());
        return -3;
    }
}

extern "C" int qcom_motion_ahb_retired(QcomMotion *motion)
{
    if (!motion || !motion->ahb) return -1;
    if (!motion->ahbSubmissionUnknown) return 1;
    if (vkGetFenceStatus(motion->device, motion->fence) != VK_SUCCESS) return 0;
    motion->ahbSubmissionUnknown = false;
    return 1;
}

extern "C" int qcom_motion_ahb_test_pyramid(QcomMotion *motion, const uint8_t *previous, const uint8_t *current)
{
    if (!motion || !motion->ahb || motion->failed || motion->ahbSubmissionUnknown || !previous || !current) return -1;
    Buffer observed{};
    try {
        motion->makeBuffer(observed, motion->staging.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        checked(vkResetFences(motion->device, 1, &motion->fence), "reset pyramid test fence");
        checked(vkResetCommandBuffer(motion->command, 0), "reset pyramid test command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(motion->command, &begin), "begin pyramid test");
        VkBufferCopy copy{0, 0, observed.size};
        vkCmdCopyBuffer(motion->command, motion->staging.object, observed.object, 1, &copy);
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = observed.object;
        barrier.size = observed.size;
        vkCmdPipelineBarrier(motion->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, nullptr, 1, &barrier, 0, nullptr);
        checked(vkEndCommandBuffer(motion->command), "end pyramid test");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &motion->command;
        motion->ahbSubmissionUnknown = true;
        checked(vkQueueSubmit(motion->queue, 1, &submit, motion->fence), "submit pyramid test");
        checked(vkWaitForFences(motion->device, 1, &motion->fence, VK_TRUE, 2000000000ULL), "await pyramid test");
        motion->ahbSubmissionUnknown = false;
        std::vector<uint8_t> expected(observed.size);
        // Reuse the frozen CPU pyramid implementation as a byte oracle.
        motion->staging.mapped = expected.data();
        motion->buildPyramid(previous, current);
        motion->staging.mapped = nullptr;
        const bool match = std::memcmp(expected.data(), observed.mapped, expected.size()) == 0;
        observed.clear(motion->device);
        return match ? 0 : -2;
    } catch (const std::exception &error) {
        if (!motion->ahbSubmissionUnknown) observed.clear(motion->device);
        std::fprintf(stderr, "QCOM AHB pyramid diagnostic: %s\n", error.what());
        return -3;
    }
}

