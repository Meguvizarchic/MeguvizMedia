#include "qcom_motion.h"
#include <motion_spv.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace {
constexpr unsigned width = QCOM_MOTION_WIDTH;
constexpr unsigned height = QCOM_MOTION_HEIGHT;
constexpr unsigned tile = QCOM_MOTION_TILE;
constexpr unsigned gridSize = QCOM_MOTION_GRID_WIDTH * QCOM_MOTION_GRID_HEIGHT;
constexpr unsigned coarseSize = gridSize / 4;
constexpr unsigned candidatesSize = 2 * gridSize * QCOM_MOTION_CANDIDATE_COUNT;
constexpr float invalidCost = 1.0e10f;
constexpr VkImageUsageFlags imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_SAMPLE_BLOCK_MATCH_BIT_QCOM;
using Clock = std::chrono::steady_clock;

uint64_t nanoseconds(Clock::time_point begin, Clock::time_point end)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

void checked(VkResult value, const char *operation)
{
    if (value != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(value));
}

struct Buffer {
    VkBuffer object = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize size = 0;

    void clear(VkDevice device)
    {
        if (mapped) vkUnmapMemory(device, memory);
        if (object) vkDestroyBuffer(device, object, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
    }
};

struct Image {
    VkImage object = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkExtent3D extent{};
    VkDeviceSize offset = 0;

    void clear(VkDevice device)
    {
        if (view) vkDestroyImageView(device, view, nullptr);
        if (object) vkDestroyImage(device, object, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
    }
};

struct Match {
    float x, y, best, next;
};

struct Parameters {
    int width, height, refine, candidateMode;
};

static_assert(sizeof(Match) == 4 * sizeof(float));

float confidenceFor(const Match &match)
{
    if (!std::isfinite(match.best) || !std::isfinite(match.next) || match.next >= invalidCost ||
        match.next <= 0.0f)
        return 0.0f;
    return std::clamp(1.0f - match.best / match.next, 0.0f, 1.0f);
}

bool extensionAvailable(const std::vector<VkExtensionProperties> &extensions, const char *name)
{
    return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties &extension) {
        return std::strcmp(name, extension.extensionName) == 0;
    });
}
}

#include "ahb_vk.inc"

struct QcomMotion {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkPhysicalDeviceMemoryProperties properties{};
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> sets{};
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool timestamps = VK_NULL_HANDLE;
    Buffer staging, coarse, fine, candidateBuffer;
    std::array<Image, 4> images{};
    uint32_t family = 0;
    uint32_t timestampBits = 0;
    float timestampPeriod = 0.0f;
    bool populated = false;
    bool failed = false;
    bool ahbSubmissionUnknown = false;
    std::unique_ptr<AhbInput> ahb;

    ~QcomMotion()
    {
        if (device) {
            if (failed && !ahb) vkDeviceWaitIdle(device);
            ahb.reset();
            if (timestamps) vkDestroyQueryPool(device, timestamps, nullptr);
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (shader) vkDestroyShaderModule(device, shader, nullptr);
            if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (descriptors) vkDestroyDescriptorPool(device, descriptors, nullptr);
            if (layout) vkDestroyDescriptorSetLayout(device, layout, nullptr);
            if (sampler) vkDestroySampler(device, sampler, nullptr);
            for (auto &image : images) image.clear(device);
            candidateBuffer.clear(device);
            fine.clear(device);
            coarse.clear(device);
            staging.clear(device);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    uint32_t memoryType(uint32_t mask, VkMemoryPropertyFlags flags) const
    {
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((mask & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        }
        throw std::runtime_error("Required Vulkan memory type unavailable");
    }

    void makeBuffer(Buffer &buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool host)
    {
        buffer.size = size;
        VkBufferCreateInfo config{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        config.size = size;
        config.usage = usage;
        checked(vkCreateBuffer(device, &config, nullptr, &buffer.object), "vkCreateBuffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, buffer.object, &req);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = req.size;
        allocate.memoryTypeIndex = memoryType(req.memoryTypeBits, host
            ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checked(vkAllocateMemory(device, &allocate, nullptr, &buffer.memory), "vkAllocateMemory(buffer)");
        checked(vkBindBufferMemory(device, buffer.object, buffer.memory, 0), "vkBindBufferMemory");
        if (host)
            checked(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped), "vkMapMemory");
    }

    void makeImage(Image &image)
    {
        VkImageCreateInfo config{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        config.imageType = VK_IMAGE_TYPE_2D;
        config.format = format;
        config.extent = image.extent;
        config.mipLevels = 1;
        config.arrayLayers = 1;
        config.samples = VK_SAMPLE_COUNT_1_BIT;
        config.tiling = VK_IMAGE_TILING_OPTIMAL;
        config.usage = imageUsage;
        checked(vkCreateImage(device, &config, nullptr, &image.object), "vkCreateImage");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, image.object, &req);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = req.size;
        allocate.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checked(vkAllocateMemory(device, &allocate, nullptr, &image.memory), "vkAllocateMemory(image)");
        checked(vkBindImageMemory(device, image.object, image.memory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.object;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        checked(vkCreateImageView(device, &view, nullptr, &image.view), "vkCreateImageView");
    }

    void initializeDevice(bool ahbInput)
    {
        uint32_t api = VK_API_VERSION_1_0;
        auto loaderVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        if (loaderVersion) checked(loaderVersion(&api), "vkEnumerateInstanceVersion");
        if (api < VK_API_VERSION_1_1)
            throw std::runtime_error("Vulkan 1.1 required for device feature queries");
        api = std::min(api, uint32_t(VK_API_VERSION_1_3));
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Meguviz native QCOM candidate";
        app.apiVersion = api;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app;
        checked(vkCreateInstance(&create, nullptr, &instance), "vkCreateInstance");
        const auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
        const auto getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
        const auto getFormatProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFormatProperties2"));
        if (!getFeatures2 || !getProperties2 || !getFormatProperties2)
            throw std::runtime_error("Vulkan 1.1 physical-device query entry points unavailable");
        uint32_t count = 0;
        checked(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(count);
        checked(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");

        for (VkPhysicalDevice candidate : devices) {
            uint32_t extensionCount = 0;
            checked(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr),
                    "vkEnumerateDeviceExtensionProperties");
            std::vector<VkExtensionProperties> extensions(extensionCount);
            checked(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, extensions.data()),
                    "vkEnumerateDeviceExtensionProperties");
            if (!extensionAvailable(extensions, VK_QCOM_IMAGE_PROCESSING_EXTENSION_NAME) ||
                (ahbInput && !supportsAhb(instance, candidate, extensions)))
                continue;
            VkPhysicalDeviceImageProcessingFeaturesQCOM features{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_FEATURES_QCOM};
            VkPhysicalDeviceFeatures2 queried{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            queried.pNext = &features;
            getFeatures2(candidate, &queried);
            if (!features.textureBlockMatch)
                continue;
            VkPhysicalDeviceImageProcessingPropertiesQCOM imageProperties{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_PROPERTIES_QCOM};
            VkPhysicalDeviceProperties2 deviceProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            deviceProperties.pNext = &imageProperties;
            getProperties2(candidate, &deviceProperties);
            if (imageProperties.maxBlockMatchRegion.width < tile || imageProperties.maxBlockMatchRegion.height < tile)
                continue;
            const bool coreSpirv14 = api >= VK_API_VERSION_1_2 &&
                                     deviceProperties.properties.apiVersion >= VK_API_VERSION_1_2;
            if (!coreSpirv14 &&
                (!extensionAvailable(extensions, VK_KHR_SPIRV_1_4_EXTENSION_NAME) ||
                 !extensionAvailable(extensions, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME)))
                continue;
            const bool coreFlags2 = api >= VK_API_VERSION_1_3 &&
                                   deviceProperties.properties.apiVersion >= VK_API_VERSION_1_3;
            const bool extensionFlags2 = extensionAvailable(extensions, VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME);
            for (VkFormat choice : {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
                if (coreFlags2 || extensionFlags2) {
                    VkFormatProperties3 flags{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
                    VkFormatProperties2 formatProperties{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
                    formatProperties.pNext = &flags;
                    getFormatProperties2(candidate, choice, &formatProperties);
                    if (!(flags.optimalTilingFeatures & VK_FORMAT_FEATURE_2_BLOCK_MATCHING_BIT_QCOM))
                        continue;
                }
                VkImageFormatProperties limits{};
                if (vkGetPhysicalDeviceImageFormatProperties(candidate, choice, VK_IMAGE_TYPE_2D,
                    VK_IMAGE_TILING_OPTIMAL, imageUsage, 0, &limits) == VK_SUCCESS &&
                    limits.maxExtent.width >= width && limits.maxExtent.height >= height) {
                    format = choice;
                    break;
                }
            }
            if (format == VK_FORMAT_UNDEFINED)
                continue;
            uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, queues.data());
            for (uint32_t i = 0; i < families; ++i) {
                if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                    physical = candidate;
                    family = i;
                    timestampBits = queues[i].timestampValidBits;
                    timestampPeriod = deviceProperties.properties.limits.timestampPeriod;
                    break;
                }
            }
            if (!physical) {
                format = VK_FORMAT_UNDEFINED;
                continue;
            }
            float priority = 1.0f;
            VkDeviceQueueCreateInfo queueCreate{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queueCreate.queueFamilyIndex = family;
            queueCreate.queueCount = 1;
            queueCreate.pQueuePriorities = &priority;
            VkPhysicalDeviceImageProcessingFeaturesQCOM enable{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_FEATURES_QCOM};
            enable.textureBlockMatch = VK_TRUE;
            std::vector<const char *> names{VK_QCOM_IMAGE_PROCESSING_EXTENSION_NAME};
            if (ahbInput) {
                names.push_back(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
                names.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
                names.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
            }
            if (!coreSpirv14) {
                names.push_back(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
                names.push_back(VK_KHR_SPIRV_1_4_EXTENSION_NAME);
            }
            if (!coreFlags2 && extensionFlags2)
                names.push_back(VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME);
            VkDeviceCreateInfo deviceCreate{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            deviceCreate.pNext = &enable;
            deviceCreate.queueCreateInfoCount = 1;
            deviceCreate.pQueueCreateInfos = &queueCreate;
            deviceCreate.enabledExtensionCount = static_cast<uint32_t>(names.size());
            deviceCreate.ppEnabledExtensionNames = names.data();
            checked(vkCreateDevice(physical, &deviceCreate, nullptr, &device), "vkCreateDevice");
            vkGetDeviceQueue(device, family, 0, &queue);
            vkGetPhysicalDeviceMemoryProperties(physical, &properties);
            std::fprintf(stderr, "QCOM device=%s format=%d max_block=%ux%u timestamp_bits=%u spirv14=%s\n",
                deviceProperties.properties.deviceName, format, imageProperties.maxBlockMatchRegion.width,
                imageProperties.maxBlockMatchRegion.height, timestampBits, coreSpirv14 ? "core" : "KHR");
            return;
        }
        throw std::runtime_error("No suitable VK_QCOM_image_processing textureBlockMatch device, SPIR-V 1.4 and format");
    }

    void initialize(int metric, bool ahbInput = false)
    {
        initializeDevice(ahbInput);
        const unsigned bytesPerPixel = format == VK_FORMAT_R8_UNORM ? 1 : 4;
        VkDeviceSize stagingSize = 0;
        for (size_t i = 0; i < images.size(); ++i) {
            const unsigned scale = i < 2 ? 2 : 1;
            auto &image = images[i];
            image.extent = {width / scale, height / scale, 1};
            image.offset = stagingSize;
            makeImage(image);
            stagingSize += image.extent.width * image.extent.height * bytesPerPixel;
        }
        const auto storage = ahbInput ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0;
        makeBuffer(staging, stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | storage, !ahbInput);
        if (ahbInput) {
            ahb = std::make_unique<AhbInput>(device, family);
            ahb->create();
        }
        makeBuffer(coarse, 4 * coarseSize * sizeof(Match), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        makeBuffer(fine, 2 * gridSize * sizeof(Match), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        makeBuffer(candidateBuffer, candidatesSize * sizeof(Match), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);

        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.flags = VK_SAMPLER_CREATE_IMAGE_PROCESSING_BIT_QCOM;
        samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.unnormalizedCoordinates = VK_TRUE;
        checked(vkCreateSampler(device, &samplerInfo, nullptr, &sampler), "vkCreateSampler");
        const VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &sampler},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &sampler},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 5;
        layoutInfo.pBindings = bindings;
        checked(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout), "vkCreateDescriptorSetLayout");
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6}};
        VkDescriptorPoolCreateInfo descriptorsInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        descriptorsInfo.maxSets = 2;
        descriptorsInfo.poolSizeCount = 2;
        descriptorsInfo.pPoolSizes = sizes;
        checked(vkCreateDescriptorPool(device, &descriptorsInfo, nullptr, &descriptors), "vkCreateDescriptorPool");
        const VkDescriptorSetLayout layouts[] = {layout, layout};
        VkDescriptorSetAllocateInfo allocSets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocSets.descriptorPool = descriptors;
        allocSets.descriptorSetCount = 2;
        allocSets.pSetLayouts = layouts;
        checked(vkAllocateDescriptorSets(device, &allocSets, sets.data()), "vkAllocateDescriptorSets");
        for (size_t pass = 0; pass < sets.size(); ++pass) {
            const VkDescriptorImageInfo sampled[] = {
                {sampler, images[2 * pass].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {sampler, images[2 * pass + 1].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            const VkDescriptorBufferInfo buffers[] = {{coarse.object, 0, coarse.size}, {fine.object, 0, fine.size},
                                                       {candidateBuffer.object, 0, candidateBuffer.size}};
            VkWriteDescriptorSet writes[5]{};
            for (size_t i = 0; i < 5; ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = sets[pass];
                writes[i].dstBinding = static_cast<uint32_t>(i);
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = bindings[i].descriptorType;
                if (i < 2) writes[i].pImageInfo = &sampled[i];
                else writes[i].pBufferInfo = &buffers[i - 2];
            }
            vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
        }

        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters)};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &layout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &push;
        checked(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout), "vkCreatePipelineLayout");
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize = sizeof(qcom_motion_spv);
        shaderInfo.pCode = qcom_motion_spv;
        checked(vkCreateShaderModule(device, &shaderInfo, nullptr, &shader), "vkCreateShaderModule");
        const VkBool32 squared = metric != 0;
        const VkSpecializationMapEntry metricEntry{0, 0, sizeof(squared)};
        const VkSpecializationInfo specialization{1, &metricEntry, sizeof(squared), &squared};
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                              VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", &specialization};
        pipelineInfo.layout = pipelineLayout;
        checked(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateComputePipelines");
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        checked(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocCommand{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocCommand.commandPool = pool;
        allocCommand.commandBufferCount = 1;
        checked(vkAllocateCommandBuffers(device, &allocCommand, &command), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        checked(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");
        if (timestampBits && timestampPeriod > 0.0f) {
            VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = 4;
            checked(vkCreateQueryPool(device, &queryInfo, nullptr, &timestamps), "vkCreateQueryPool");
        }
    }

    void buildPyramid(const uint8_t *previous, const uint8_t *current)
    {
        const bool rgba = format != VK_FORMAT_R8_UNORM;
        const uint8_t *inputs[] = {previous, current};
        for (size_t i = 0; i < images.size(); ++i) {
            uint8_t *dest = static_cast<uint8_t *>(staging.mapped) + images[i].offset;
            const uint8_t *source = inputs[i % 2];
            if (i >= 2 && !rgba) {
                std::memcpy(dest, source, width * height);
                continue;
            }
            for (unsigned y = 0; y < images[i].extent.height; ++y) {
                for (unsigned x = 0; x < images[i].extent.width; ++x) {
                    unsigned value = source[y * width + x];
                    if (i < 2) {
                        const uint8_t *pair = source + 2 * y * width + 2 * x;
                        value = (unsigned(pair[0]) + pair[1] + pair[width] + pair[width + 1] + 2) / 4;
                    }
                    *dest++ = static_cast<uint8_t>(value);
                    if (rgba) {
                        *dest++ = static_cast<uint8_t>(value);
                        *dest++ = static_cast<uint8_t>(value);
                        *dest++ = 255;
                    }
                }
            }
        }
    }

    void upload()
    {
        std::array<VkImageMemoryBarrier, 4> barriers{};
        for (size_t i = 0; i < images.size(); ++i) {
            auto &barrier = barriers[i];
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = populated ? VK_ACCESS_SHADER_READ_BIT : 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = populated ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images[i].object;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vkCmdPipelineBarrier(command, populated ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 4, barriers.data());
        for (size_t i = 0; i < images.size(); ++i) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = images[i].offset;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = images[i].extent;
            vkCmdCopyBufferToImage(command, staging.object, images[i].object, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            auto &barrier = barriers[i];
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 4, barriers.data());
    }

    void bufferBarrier(const Buffer &buffer, VkAccessFlags access, VkPipelineStageFlags stage)
    {
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = access;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer.object;
        barrier.size = buffer.size;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, stage, 0,
            0, nullptr, 1, &barrier, 0, nullptr);
    }

    void dispatch(unsigned level, int candidateMode)
    {
        const unsigned scale = level == 0 ? 2 : 1;
        const Parameters params{int(width / scale), int(height / scale), int(level), candidateMode};
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &sets[level], 0, nullptr);
        vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
        vkCmdDispatch(command, width / scale / tile, height / scale / tile, 2);
    }

    void readTimestamps(QcomMotionTiming &timing)
    {
        if (!timestamps)
            return;
        uint64_t values[4]{};
        if (vkGetQueryPoolResults(device, timestamps, 0, 4, sizeof(values), values, sizeof(values[0]),
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return;
        const uint64_t mask = timestampBits == 64 ? UINT64_MAX : ((1ULL << timestampBits) - 1);
        auto delta = [mask, this](uint64_t a, uint64_t b) {
            return static_cast<uint64_t>(double((b - a) & mask) * timestampPeriod);
        };
        timing.gpu_upload_ns = delta(values[0], values[1]);
        timing.gpu_coarse_ns = delta(values[1], values[2]);
        timing.gpu_fine_ns = delta(values[2], values[3]);
        timing.gpu_observed = 1;
    }

    void estimate(const uint8_t *previous, const uint8_t *current, float *vectors, float *confidence,
                  float *candidates, QcomMotionTiming *timing, int candidateMode, const QcomAhbFrame (*inputs)[2] = nullptr)
    {
        auto start = Clock::now();
        if (inputs) ahb->waitOn(*inputs);
        else buildPyramid(previous, current);
        checked(vkResetFences(device, 1, &fence), "vkResetFences");
        checked(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        if (timestamps) {
            vkCmdResetQueryPool(command, timestamps, 0, 4);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 0);
        }
        if (inputs)
            ahb->record(command, *inputs, images, staging, format != VK_FORMAT_R8_UNORM);
        upload();
        if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TRANSFER_BIT, timestamps, 1);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        dispatch(0, candidateMode);
        bufferBarrier(coarse, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timestamps, 2);
        dispatch(1, candidateMode);
        bufferBarrier(fine, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);
        if (candidateMode != 0)
            bufferBarrier(candidateBuffer, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);
        if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 3);
        checked(vkEndCommandBuffer(command), "vkEndCommandBuffer");

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        const VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
        if (inputs) {
            submit.waitSemaphoreCount = 2;
            submit.pWaitSemaphores = ahb->semaphores.data();
            submit.pWaitDstStageMask = waitStages;
            ahbSubmissionUnknown = true;
        }
        auto queued = Clock::now();
        checked(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
        checked(vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ULL), "vkWaitForFences");
        if (inputs) ahbSubmissionUnknown = false;
        auto retired = Clock::now();
        populated = true;
        const auto *matches = static_cast<const Match *>(fine.mapped);
        for (size_t i = 0; i < gridSize; ++i) {
            const auto &forward = matches[i];
            const auto &backward = matches[i + gridSize];
            const float values[] = {forward.x, forward.y, backward.x, backward.y};
            std::memcpy(vectors + 4 * i, values, sizeof(values));
            if (confidence) {
                const float levels[] = {confidenceFor(forward), confidenceFor(backward)};
                std::memcpy(confidence + 2 * i, levels, sizeof(levels));
            }
        }
        if (candidates)
            std::memcpy(candidates, candidateBuffer.mapped, candidateBuffer.size);
        if (timing) {
            *timing = {};
            timing->preparation_ns = nanoseconds(start, queued);
            timing->queue_ns = nanoseconds(queued, retired);
            timing->cpu_readback_ns = nanoseconds(retired, Clock::now());
            timing->rgba_expanded = format != VK_FORMAT_R8_UNORM;
            readTimestamps(*timing);
        }
    }
};

extern "C" QcomMotion *qcom_motion_create(int metric)
{
    if (metric != 0 && metric != 1) return nullptr;
    try {
        auto motion = std::make_unique<QcomMotion>();
        motion->initialize(metric);
        return motion.release();
    } catch (const std::exception &failure) {
        std::fprintf(stderr, "QCOM create: %s\n", failure.what());
        return nullptr;
    }
}

extern "C" void qcom_motion_destroy(QcomMotion *motion)
{
    if (motion && motion->ahbSubmissionUnknown) return;
    delete motion;
}

static int estimateChecked(QcomMotion *motion, const uint8_t *previous, const uint8_t *current,
                           float *vectors, float *confidence, float *candidates,
                           QcomMotionTiming *timing, int candidateMode)
{
    if (!motion || motion->ahb || !previous || !current || !vectors) return -1;
    if (motion->failed) return -2;
    try {
        motion->estimate(previous, current, vectors, confidence, candidates, timing, candidateMode);
        return 0;
    } catch (const std::exception &failure) {
        motion->failed = true;
        std::fprintf(stderr, "QCOM estimate: %s\n", failure.what());
        return -3;
    }
}

extern "C" int qcom_motion_estimate(QcomMotion *motion, const uint8_t *previous, const uint8_t *current,
                                    float *vectors, float *confidence, QcomMotionTiming *timing)
{
    return estimateChecked(motion, previous, current, vectors, confidence, nullptr, timing, 0);
}

extern "C" int qcom_motion_estimate_candidates(QcomMotion *motion, const uint8_t *previous,
                                               const uint8_t *current, float *vectors, float *confidence,
                                               float *candidates, QcomMotionTiming *timing, int candidateMode)
{
    if (!candidates ||
        (candidateMode != QCOM_MOTION_CANDIDATE_LOCAL && candidateMode != QCOM_MOTION_CANDIDATE_WIDE))
        return -1;
    return estimateChecked(motion, previous, current, vectors, confidence, candidates, timing, candidateMode);
}

#include "ahb_motion_api.inc"
