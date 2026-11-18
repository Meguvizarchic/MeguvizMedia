#include "qcom_motion.h"
#include <motion_spv.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace {
constexpr unsigned width = QCOM_MOTION_WIDTH;
constexpr unsigned height = QCOM_MOTION_HEIGHT;
constexpr unsigned tile = QCOM_MOTION_TILE;
constexpr unsigned gridSize = QCOM_MOTION_GRID_WIDTH * QCOM_MOTION_GRID_HEIGHT;
/* Pyramid geometry.  The coarse level is built at 4x reduction but with an even
 * cell count: 272 / 4 = 68 rows is not divisible by the 8-row tile, so one extra
 * row is appended (replicating the last source row) and the level becomes
 * 120x72 with exactly 15x9 cells.  Without that the bottom 4 output rows would
 * have no coarse prediction at all. */
constexpr unsigned midWidth = width / 2;
constexpr unsigned midHeight = height / 2;
constexpr unsigned coarseWidth = width / 4;
constexpr unsigned coarseHeight = (height / 4 + tile - 1) / tile * tile;
constexpr unsigned coarseWidthCells = coarseWidth / tile;
constexpr unsigned coarseHeightCells = coarseHeight / tile;
constexpr unsigned midWidthCells = midWidth / tile;
constexpr unsigned midHeightCells = midHeight / tile;
constexpr unsigned coarseCellCount = coarseWidthCells * coarseHeightCells;
constexpr unsigned midCellCount = midWidthCells * midHeightCells;
constexpr unsigned candidatesSize = 2 * gridSize * QCOM_MOTION_CANDIDATE_COUNT;
constexpr float invalidCost = 1.0e10f;
/* Timestamp slots written per estimate(); see the query pool comment. */
constexpr uint32_t timestampCount = 10;constexpr VkImageUsageFlags imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
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
    int width, height, stage, candidateMode, subpixel, staticThresholdMilli, staticNeighborMilli, reserved;
    int confidenceGateMilli;
};

static_assert(sizeof(Match) == 4 * sizeof(float));
static_assert(sizeof(Parameters) == 9 * sizeof(int));

/* Static thresholds in mean absolute luma error, as parts per thousand.  A pixel
 * is static when the block and its four 2-pixel neighbours all changed by less
 * than kStaticMilli (a little over one 8-bit code value) and no neighbour by more
 * than kStaticNeighborMilli (about four code values, i.e. compression noise).
 * Both travel as push constants, so the calibrated values below are only the
 * defaults: qcom_motion_set_static_thresholds overrides them per handle without a
 * rebuild. */
constexpr int kStaticMilli = 2;
constexpr int kStaticNeighborMilli = 10;

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

#if defined(__ANDROID__)
/* Calibration hook.  The standalone fixture's Java tree is byte-pinned, so a device
 * sweep cannot go through JNI extras.  On Android the handle creation additionally
 * honours three debug properties, which lets one binary cover the whole sweep:
 *   debug.meguviz.qcom.static.milli           centre threshold
 *   debug.meguviz.qcom.static.neighbor_milli  neighbour threshold
 *   debug.meguviz.qcom.static.gate            1 = consume the mask at warp time
 * Unset or unparsable values leave the compiled defaults in place, so production
 * behaviour is unchanged when nothing is set. */
void readProperty(const char *name, int &target)
{
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get(name, value) <= 0) return;
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0 || parsed > 100000) return;
    target = static_cast<int>(parsed);
}
#endif
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
    std::array<VkDescriptorSet, 3> sets{};
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    /* Pipelines built from the same module with a reducers subset compiled in:
     *   pipeline        every reducer compiled out (the default, byte-identical)
     *   reducePipeline  parallel top-two reduction only (output-preserving)
     * Selected per estimate() by `adaptiveMode`.  The alternate-hypothesis gate
     * (reducer bit 1) is deliberately absent from this module: device quality
     * evidence rejected it (crossing 23.416->23.316, occlusion-13 -0.352 dB) for
     * only ~0.14 ms on real 4K content, so no pipeline enables it and
     * qcom_motion_set_adaptive rejects every mode above 1. */
    VkPipeline reducePipeline = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool timestamps = VK_NULL_HANDLE;
    Buffer staging, coarse0, coarse1, fine, candidateBuffer, staticBuffer, visibilityBuffer;
    /* images[0..1] coarse pair, [2..3] mid pair, [4..5] fine pair. */
    std::array<Image, 6> images{};
    uint32_t family = 0;
    uint32_t timestampBits = 0;
    float timestampPeriod = 0.0f;
    int subpixel = 1;
    /* Cost-reducer mode.  0 keeps the default pipeline; 1 enables the parallel
     * top-two reduction, which is output-preserving (strict total order key
     * (cost, weight, slot index) reproduces the serial left fold exactly).
     * Modes 2 and 3 (alternate-window gate) are rejected by
     * qcom_motion_set_adaptive: the gate lost its quality gate on device. */
    int adaptiveMode = 0;
    /* Parent-confidence cut in thousandths.  Retained only so the validated
     * call signature still works; unused while the gate is absent. */
    int confidenceGateMilli = 250;
    /* Runtime-calibrated static classification thresholds, in thousandths of a
     * normalized luma code value. */
    int staticMilli = kStaticMilli;
    int staticNeighbourMilli = kStaticNeighborMilli;
    /* Opt-in: force zero motion on classified-static cells so the warp blends the
     * two endpoint frames at the same coordinate. */
    bool staticGate = false;
    /* How the submit->retire fence is awaited.  0 keeps the historical blocking
     * vkWaitForFences; 1 spins on vkGetFenceStatus first.  The fence covers only
     * this handle's own single-frame submission, so the interval is dominated by
     * wakeup latency, and the two strategies retire on the same fence. */
    int waitStrategy = QCOM_MOTION_WAIT_BLOCK;
    uint64_t waitSpinNs = QCOM_MOTION_WAIT_SPIN_NS_DEFAULT;
    /* True when the spin budget should follow the previous frame's retire
     * interval instead of the fixed waitSpinNs.  See awaitRetire(). */
    bool waitSpinNsAdaptive = false;
    /* The instant the previous estimate retired, and whether there is one.  Used
     * only for the timing report's caller-turnaround figure. */
    Clock::time_point lastRetired{};
    bool hasLastRetired = false;
    /* Filled by awaitRetire() for the timing report of the current estimate. */
    uint64_t waitSpunNs = 0;
    uint64_t waitBlockedNs = 0;
    uint64_t waitPolls = 0;
    uint64_t spinBudgetNs = 0;
    /* Previous frame's submit->retire interval, the adaptive spin predictor. */
    uint64_t lastQueueNs = 0;
    /* Periodic wait statistics.  The window trace records only JNI durations, so
     * without this there is no way to tell whether the poll path engaged in the
     * integrated app or how often the spin fell through to the blocking wait.
     * One line per kStatsWindow estimates goes to stderr, which logcat carries. */
    bool statsEnabled = false;
    uint64_t statsCount = 0, statsQueueSum = 0, statsQueueMax = 0;
    uint64_t statsSpinSum = 0, statsBlockSum = 0, statsBlockFallbacks = 0;
    uint64_t statsGpuSum = 0, statsGpuMax = 0;
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
            if (reducePipeline) vkDestroyPipeline(device, reducePipeline, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (shader) vkDestroyShaderModule(device, shader, nullptr);
            if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (descriptors) vkDestroyDescriptorPool(device, descriptors, nullptr);
            if (layout) vkDestroyDescriptorSetLayout(device, layout, nullptr);
            if (sampler) vkDestroySampler(device, sampler, nullptr);
            for (auto &image : images) image.clear(device);
            visibilityBuffer.clear(device);
            staticBuffer.clear(device);
            candidateBuffer.clear(device);
            fine.clear(device);
            coarse1.clear(device);
            coarse0.clear(device);
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

    void makeImage(Image &image, VkExtent3D extent)
    {
        image.extent = extent;
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
        app.pApplicationName = "Meguviz native QCOM motion estimator";
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
            /* The block size is fixed at 8x8 in the shader, so a device that
             * cannot match an 8x8 region cannot run this estimator.  Adreno 740
             * reports 64x64 here, leaving plenty of headroom. */
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
            std::fprintf(stderr,
                "QCOM device=%s format=%d max_block=%ux%u timestamp_bits=%u spirv14=%s "
                "pyramid=%ux%u/%ux%u/%ux%u\n",
                deviceProperties.properties.deviceName, format, imageProperties.maxBlockMatchRegion.width,
                imageProperties.maxBlockMatchRegion.height, timestampBits, coreSpirv14 ? "core" : "KHR",
                coarseWidth, coarseHeight, midWidth, midHeight, width, height);
            return;
        }
        throw std::runtime_error("No suitable VK_QCOM_image_processing textureBlockMatch device, SPIR-V 1.4 and format");
    }

    void initialize(int metric, bool ahbInput = false)
    {
        initializeDevice(ahbInput);
        /* One line per handle creation recording the reducer mode that is actually
         * in effect.  The reducers are chosen at create time from a debug property,
         * so without this a capture cannot prove which variant produced its frames -
         * and "I thought I switched it" is exactly the failure a quality A/B cannot
         * afford.  The line goes to stderr, which logcat carries. */
        std::fprintf(stderr, "QCOM adaptive_mode=%d confidence_milli=%d\n",
                     adaptiveMode, confidenceGateMilli);
        const unsigned bytesPerPixel = format == VK_FORMAT_R8_UNORM ? 1 : 4;
        VkDeviceSize stagingSize = 0;
        const VkExtent3D levels[3] = {
            {coarseWidth, coarseHeight, 1}, {midWidth, midHeight, 1}, {width, height, 1}};
        for (size_t level = 0; level < 3; ++level) {
            for (size_t pair = 0; pair < 2; ++pair) {
                auto &image = images[2 * level + pair];
                image.offset = stagingSize;
                makeImage(image, levels[level]);
                stagingSize += levels[level].width * levels[level].height * bytesPerPixel;
            }
        }
        const auto storage = ahbInput ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0;
        makeBuffer(staging, stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | storage, !ahbInput);
        if (ahbInput) {
            ahb = std::make_unique<AhbInput>(device, family);
            ahb->create();
        }
        makeBuffer(coarse0, 4 * coarseCellCount * sizeof(Match), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        makeBuffer(coarse1, 4 * midCellCount * sizeof(Match), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        makeBuffer(fine, 2 * gridSize * sizeof(Match), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        makeBuffer(candidateBuffer, candidatesSize * sizeof(Match), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        makeBuffer(staticBuffer, gridSize * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        makeBuffer(visibilityBuffer, 2 * gridSize * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);

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
            {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 8;
        layoutInfo.pBindings = bindings;
        checked(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout), "vkCreateDescriptorSetLayout");
        /* Three sets: each needs 2 combined image samplers and 6 storage buffers,
         * so the pool must hold 6 of the former and 18 of the latter. */
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 18}};
        VkDescriptorPoolCreateInfo descriptorsInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        descriptorsInfo.maxSets = 3;
        descriptorsInfo.poolSizeCount = 2;
        descriptorsInfo.pPoolSizes = sizes;
        checked(vkCreateDescriptorPool(device, &descriptorsInfo, nullptr, &descriptors), "vkCreateDescriptorPool");
        const VkDescriptorSetLayout layouts[] = {layout, layout, layout};
        VkDescriptorSetAllocateInfo allocSets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocSets.descriptorPool = descriptors;
        allocSets.descriptorSetCount = 3;
        allocSets.pSetLayouts = layouts;
        checked(vkAllocateDescriptorSets(device, &allocSets, sets.data()), "vkAllocateDescriptorSets");
        for (size_t level = 0; level < sets.size(); ++level) {
            const VkDescriptorImageInfo sampled[] = {
                {sampler, images[2 * level].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {sampler, images[2 * level + 1].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            const VkDescriptorBufferInfo buffers[] = {
                {coarse0.object, 0, coarse0.size}, {coarse1.object, 0, coarse1.size},
                {fine.object, 0, fine.size}, {candidateBuffer.object, 0, candidateBuffer.size},
                {staticBuffer.object, 0, staticBuffer.size}, {visibilityBuffer.object, 0, visibilityBuffer.size}};
            VkWriteDescriptorSet writes[8]{};
            for (size_t i = 0; i < 8; ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = sets[level];
                writes[i].dstBinding = static_cast<uint32_t>(i);
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = bindings[i].descriptorType;
                if (i < 2) writes[i].pImageInfo = &sampled[i];
                else writes[i].pBufferInfo = &buffers[i - 2];
            }
            vkUpdateDescriptorSets(device, 8, writes, 0, nullptr);
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
        /* Two pipelines from one module.  Every cost reducer is a specialization
         * constant, so the reduce pipeline is compiled with the parallel reduction
         * present and the default one is compiled with it eliminated - no
         * push-constant branch remains in the default path, which is what keeps a
         * default handle both byte-identical in output and unchanged in occupancy.
         * A push constant branch was measured to cost ~0.3 ms of fine-stage time
         * even when never taken, so this is not a stylistic choice.  Both variants
         * set constant_id 2 (gateAlternate) false: the gate is not shipped, and
         * leaving it false here means even a mis-set mode cannot enable it. */
        const VkBool32 metricValues[3] = {metric != 0 ? VK_TRUE : VK_FALSE, VK_FALSE, VK_FALSE};
        VkSpecializationMapEntry entries[3];
        for (uint32_t i = 0; i < 3; ++i) {
            entries[i].constantID = i;
            entries[i].offset = i * sizeof(VkBool32);
            entries[i].size = sizeof(VkBool32);
        }
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        {
            VkSpecializationInfo defaultInfo{3, entries, sizeof(metricValues), metricValues};
            pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                  VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", &defaultInfo};
            pipelineInfo.layout = pipelineLayout;
            checked(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
                    "vkCreateComputePipelines");
        }
        /* Variant with the reduction only: provably output-preserving. */
        {
            const VkBool32 reduceValues[3] = {metric != 0 ? VK_TRUE : VK_FALSE, VK_TRUE, VK_FALSE};
            VkSpecializationInfo reduceInfo{3, entries, sizeof(reduceValues), reduceValues};
            VkComputePipelineCreateInfo reduceCreate{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            reduceCreate.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                  VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", &reduceInfo};
            reduceCreate.layout = pipelineLayout;
            checked(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &reduceCreate, nullptr,
                                             &reducePipeline), "vkCreateComputePipelines(reduce)");
        }
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
            /* Ten points, so the stages the single gpu_upload_ns bucket used to
             * merge become separately observable.  On the AHB path the fixed
             * per-frame cost of filling the three input levels has four parts -
             * the foreign-queue acquire, the unpack dispatch, the release, and the
             * six buffer->image copies - and only splitting them shows which one
             * dominates:
             *   0 top of pipe
             *   1 after the foreign-queue acquire barrier
             *   2 after the two unpack dispatches
             *   3 after the foreign-queue release barrier
             *   4 after the six buffer->image copies
             *   5 coarse, 6 mid, 7 fine, 8 visibility
             *   9 bottom of pipe (after the four host-read barriers)
             * Whatever remains between the sum of those segments and 0 -> 9 is the
             * barrier/transition residue, reported as gpu_barrier_ns.
             * The R8 path has no AHB input, so points 1-3 collapse onto point 0. */
            queryInfo.queryCount = timestampCount;
            checked(vkCreateQueryPool(device, &queryInfo, nullptr, &timestamps), "vkCreateQueryPool");
        }
    }

    void buildPyramid(const uint8_t *previous, const uint8_t *current)
    {
        const bool rgba = format != VK_FORMAT_R8_UNORM;
        const uint8_t *inputs[] = {previous, current};
        for (size_t level = 0; level < 3; ++level) {
            const unsigned scale = 1u << (2 - level);
            for (size_t pair = 0; pair < 2; ++pair) {
                auto &image = images[2 * level + pair];
                uint8_t *dest = static_cast<uint8_t *>(staging.mapped) + image.offset;
                const uint8_t *source = inputs[pair];
                for (unsigned y = 0; y < image.extent.height; ++y) {
                    for (unsigned x = 0; x < image.extent.width; ++x) {
                        unsigned value = 0;
                        if (scale == 1) {
                            value = source[y * width + x];
                        } else {
                            /* Box average of the scale x scale footprint, clamped to
                             * the source so the replicated coarse row is legal. */
                            unsigned sum = 0;
                            for (unsigned dy = 0; dy < scale; ++dy) {
                                const unsigned sy = std::min(y * scale + dy, height - 1);
                                for (unsigned dx = 0; dx < scale; ++dx) {
                                    const unsigned sx = std::min(x * scale + dx, width - 1);
                                    sum += source[sy * width + sx];
                                }
                            }
                            value = (sum + scale * scale / 2) / (scale * scale);
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
    }

    void upload()
    {
        std::array<VkImageMemoryBarrier, 6> barriers{};
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
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 6, barriers.data());
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
            0, 0, nullptr, 0, nullptr, 6, barriers.data());
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

    VkPipeline activePipeline() const
    {
        return adaptiveMode == 1 ? reducePipeline : pipeline;
    }

    void dispatch(int level, int stage, int candidateMode)
    {
        const VkExtent3D extents[3] = {
            {coarseWidth, coarseHeight, 1}, {midWidth, midHeight, 1}, {width, height, 1}};
        const Parameters params{int(extents[level].width), int(extents[level].height), stage,
                                candidateMode, subpixel, staticMilli, staticNeighbourMilli, 0,
                                confidenceGateMilli};
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, activePipeline());
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &sets[level], 0, nullptr);
        vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
        vkCmdDispatch(command, extents[level].width / tile, extents[level].height / tile, 2);
    }

    void dispatchVisibility()
    {
        const Parameters params{int(width), int(height), 3, 0, subpixel, staticMilli,
                                staticNeighbourMilli, 0, confidenceGateMilli};
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &sets[2], 0, nullptr);
        vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
        vkCmdDispatch(command, (gridSize + 63) / 64, 1, 1);
    }

    void readTimestamps(QcomMotionTiming &timing)
    {
        if (!timestamps) {
            /* Loud on purpose: a silent zero here would look like a free stage in
             * the cost table, which is the worst possible failure mode for a
             * measurement whose whole purpose is deciding what to optimize. */
            std::fprintf(stderr, "QCOM timestamps unavailable: bits=%u period=%.6f\n",
                         timestampBits, double(timestampPeriod));
            return;
        }
        uint64_t values[timestampCount]{};
        const VkResult status = vkGetQueryPoolResults(device, timestamps, 0, timestampCount,
                                                      sizeof(values), values, sizeof(values[0]),
                                                      VK_QUERY_RESULT_64_BIT);
        if (status != VK_SUCCESS) {
            std::fprintf(stderr, "QCOM timestamp readback failed: %d\n", int(status));
            return;
        }
        const uint64_t mask = timestampBits == 64 ? UINT64_MAX : ((1ULL << timestampBits) - 1);
        auto delta = [mask, this](uint64_t a, uint64_t b) {
            return static_cast<uint64_t>(double((b - a) & mask) * timestampPeriod);
        };
        /* Slots: 0 top of pipe, 1 after AHB acquire, 2 after unpack, 3 after AHB
         * release, 4 after the six buffer->image copies, 5 coarse, 6 mid, 7 fine,
         * 8 visibility, 9 bottom of pipe.  Slots 1-3 are always written: the AHB
         * path marks the acquire, the unpack dispatches and the release, while the
         * R8 path aliases them onto slot 0's stage.  Aliasing keeps the pool
         * contiguous, which vkGetQueryPoolResults requires, and makes the three
         * AHB-only terms collapse to ~0 there without a special case. */
        timing.gpu_acquire_ns = delta(values[0], values[1]);
        timing.gpu_unpack_ns = delta(values[1], values[2]);
        timing.gpu_release_ns = delta(values[2], values[3]);
        timing.gpu_copy_ns = delta(values[3], values[4]);
        timing.gpu_upload_ns = delta(values[0], values[4]);
        timing.gpu_coarse_ns = delta(values[4], values[5]);
        timing.gpu_mid_ns = delta(values[5], values[6]);
        timing.gpu_fine_ns = delta(values[6], values[7]);
        timing.gpu_visibility_ns = delta(values[7], values[8]);
        timing.gpu_total_ns = delta(values[0], values[9]);
        const uint64_t segments = timing.gpu_acquire_ns + timing.gpu_unpack_ns +
            timing.gpu_release_ns + timing.gpu_copy_ns + timing.gpu_coarse_ns +
            timing.gpu_mid_ns + timing.gpu_fine_ns + timing.gpu_visibility_ns;
        timing.gpu_barrier_ns = timing.gpu_total_ns > segments ? timing.gpu_total_ns - segments : 0;
        timing.gpu_observed = 1;
    }

    /* Awaits this handle's submit fence, reporting how the wait was satisfied.
     * Returns the CPU instant the fence was observed signalled.
     *
     * The only work in flight on this fence is this handle's own command buffer -
     * one AHB acquire, the pyramid unpack, three block-match levels, visibility,
     * and the host-read barriers.  Nothing else is ever submitted to this queue,
     * so there is no queueing behind other work to wait out.  That makes the
     * interval a pure "how fast does the CPU learn the GPU finished" question,
     * which is why polling is worth measuring: vkWaitForFences parks the thread
     * and pays a scheduler wakeup, and that wakeup is the jitter source the window
     * trace shows as a 21 ms nativeWork tail with a 6.1 ms flat GPU.
     *
     * The spin budget is bounded and a blocking wait always follows, so a fence
     * that is genuinely late still completes rather than timing out. */
    Clock::time_point awaitRetire()
    {
        auto spinBegin = Clock::now();
        if (waitStrategy == QCOM_MOTION_WAIT_POLL) {
            /* The budget has to cover the frame's own GPU time, otherwise the spin
             * expires first and the frame pays the spin *and* the blocking wakeup -
             * measured as a worse tail than blocking alone.  The previous frame's
             * retire interval is the best available predictor of this frame's, and
             * it adapts by itself when the caller switches between the default and
             * the reducer pipeline (~8.1 ms vs ~6.1 ms of GPU) or when content
             * makes the fine stage cheaper.  The margin covers run-to-run drift;
             * the blocking fallback still catches a genuinely late fence. */
            uint64_t budget = waitSpinNs;
            if (waitSpinNsAdaptive && lastQueueNs) {
                constexpr uint64_t margin = 1500000;
                budget = lastQueueNs + margin;
                if (budget < 2000000) budget = 2000000;
            }
            if (budget > 20000000) budget = 20000000;
            const auto deadline = spinBegin + std::chrono::nanoseconds(budget);
            uint64_t polls = 0;
            for (;;) {
                ++polls;
                if (vkGetFenceStatus(device, fence) == VK_SUCCESS) {
                    auto observed = Clock::now();
                    spinBudgetNs = budget;
                    waitSpunNs = nanoseconds(spinBegin, observed);
                    waitBlockedNs = 0;
                    waitPolls = polls;
                    return observed;
                }
                if (Clock::now() >= deadline) break;
            }
            spinBudgetNs = budget;
            waitSpunNs = nanoseconds(spinBegin, Clock::now());
            waitPolls = polls;
            auto blockBegin = Clock::now();
            checked(vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ULL), "vkWaitForFences");
            auto observed = Clock::now();
            waitBlockedNs = nanoseconds(blockBegin, observed);
            return observed;
        }
        checked(vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ULL), "vkWaitForFences");
        auto observed = Clock::now();
        spinBudgetNs = 0;
        waitSpunNs = 0;
        waitBlockedNs = nanoseconds(spinBegin, observed);
        waitPolls = 1;
        return observed;
    }

    void estimate(const uint8_t *previous, const uint8_t *current, float *vectors, float *confidence,
                  float *candidates, uint8_t *staticMask, float *visibility,
                  QcomMotionTiming *timing, int candidateMode,
                  const QcomAhbFrame (*inputs)[2] = nullptr)
    {
        auto start = Clock::now();
        const uint64_t retiredToSubmit = hasLastRetired ? nanoseconds(lastRetired, start) : 0;
        waitSpunNs = waitBlockedNs = waitPolls = 0;
        if (inputs) ahb->waitOn(*inputs);
        else buildPyramid(previous, current);
        checked(vkResetFences(device, 1, &fence), "vkResetFences");
        checked(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        if (timestamps) {
            vkCmdResetQueryPool(command, timestamps, 0, timestampCount);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 0);
        }
        if (inputs) {
            ahb->record(command, *inputs, images, staging, format != VK_FORMAT_R8_UNORM, timestamps);
        } else if (timestamps) {
            /* The R8 path has no AHB input, so slots 1-3 have nothing to mark.  They
             * are still written, as aliases of slot 0, because a single
             * vkGetQueryPoolResults over the pool returns VK_NOT_READY unless every
             * query in the range was written.  Leaving them unwritten silently
             * zeroed the whole per-stage table, which is exactly the failure this
             * measurement cannot afford. */
            for (uint32_t slot = 1; slot <= 3; ++slot)
                vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, slot);
        }
        upload();
        if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TRANSFER_BIT, timestamps, 4);
        dispatch(0, 0, candidateMode);
        bufferBarrier(coarse0, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timestamps, 5);
        dispatch(1, 1, candidateMode);
        bufferBarrier(coarse1, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timestamps, 6);
        dispatch(2, 2, candidateMode);
        bufferBarrier(fine, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timestamps, 7);
        dispatchVisibility();
        if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timestamps, 8);
        bufferBarrier(fine, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);
        if (candidateMode != 0)
            bufferBarrier(candidateBuffer, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);
        bufferBarrier(staticBuffer, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);
        bufferBarrier(visibilityBuffer, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);
        if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 9);
        checked(vkEndCommandBuffer(command), "vkEndCommandBuffer");

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        const VkPipelineStageFlags waitStages[] = {
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
        if (inputs) {
            submit.waitSemaphoreCount = 2;
            submit.pWaitSemaphores = ahb->semaphores.data();
            submit.pWaitDstStageMask = waitStages;
            ahbSubmissionUnknown = true;
        }
        auto queued = Clock::now();
        checked(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
        auto retired = awaitRetire();
        if (inputs) ahbSubmissionUnknown = false;
        lastRetired = retired;
        hasLastRetired = true;
        lastQueueNs = nanoseconds(queued, retired);
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
        if (staticMask) {
            const auto *words = static_cast<const uint32_t *>(staticBuffer.mapped);
            for (size_t i = 0; i < gridSize; ++i)
                staticMask[i] = words[i] ? 1u : 0u;
        }
        if (staticGate) {
            /* Stationary content must reproduce the same coordinate in both
             * frames.  Zeroing both directions is exactly the zero-displacement
             * blend the static classifier is asserting, and it removes the
             * sub-pixel jitter a block matcher finds inside flat or repetitive
             * static regions. */
            const auto *words = static_cast<const uint32_t *>(staticBuffer.mapped);
            for (size_t i = 0; i < gridSize; ++i) {
                if (!words[i])
                    continue;
                std::memset(vectors + 4 * i, 0, 4 * sizeof(float));
                if (confidence)
                    std::memset(confidence + 2 * i, 0, 2 * sizeof(float));
            }
        }
        if (visibility)
            std::memcpy(visibility, visibilityBuffer.mapped, visibilityBuffer.size);
        const uint64_t queueNs = nanoseconds(queued, retired);
        if (timing) {
            *timing = {};
            timing->preparation_ns = nanoseconds(start, queued);
            timing->queue_ns = queueNs;
            timing->cpu_readback_ns = nanoseconds(retired, Clock::now());
            timing->wait_spin_ns = waitSpunNs;
            timing->wait_block_ns = waitBlockedNs;
            timing->poll_count = waitPolls;
            timing->retire_to_submit_ns = retiredToSubmit;
            timing->wait_strategy = waitStrategy;
            timing->spin_budget_ns = spinBudgetNs;
            timing->rgba_expanded = format != VK_FORMAT_R8_UNORM;
            readTimestamps(*timing);
        }
        if (statsEnabled) {
            /* gpu_total only exists once the timestamp pool has been read, so this
             * block runs after readTimestamps rather than beside the memcpys. */
            /* gpu_total only exists once the timestamp pool has been read.  The JNI
             * entry points pass a null timing, so the stats path reads the pool
             * itself into a local struct rather than reporting a spurious zero. */
            QcomMotionTiming local{};
            if (!timing) readTimestamps(local);
            const uint64_t gpuNs = timing ? timing->gpu_total_ns : local.gpu_total_ns;
            ++statsCount;
            statsQueueSum += queueNs;
            if (queueNs > statsQueueMax) statsQueueMax = queueNs;
            statsSpinSum += waitSpunNs;
            statsBlockSum += waitBlockedNs;
            if (waitBlockedNs) ++statsBlockFallbacks;
            statsGpuSum += gpuNs;
            if (gpuNs > statsGpuMax) statsGpuMax = gpuNs;
            constexpr uint64_t kStatsWindow = 30;
            if (statsCount % kStatsWindow == 0) {
                /* Window stats: sums and counts are reset below, so the divisor is
                 * the window length, not the running total. */
                const double n = double(kStatsWindow);
                const double queueMean = double(statsQueueSum) / n / 1e6;
                const double spinMean = double(statsSpinSum) / n / 1e6;
                const double blockMean = double(statsBlockSum) / n / 1e6;
                const double gpuMean = double(statsGpuSum) / n / 1e6;
                std::fprintf(stderr,
                    "QCOM wait stats n=%llu strategy=%d queue_mean_ms=%.3f queue_max_ms=%.3f "
                    "spin_mean_ms=%.3f block_mean_ms=%.3f block_fallbacks=%llu "
                    "gpu_mean_ms=%.3f gpu_max_ms=%.3f\n",
                    static_cast<unsigned long long>(statsCount), waitStrategy,
                    queueMean, double(statsQueueMax) / 1e6, spinMean, blockMean,
                    static_cast<unsigned long long>(statsBlockFallbacks),
                    gpuMean, double(statsGpuMax) / 1e6);
                std::fflush(stderr);
                /* stderr does not reach logcat from a release APK, and the app
                 * sandbox cannot write /data/local/tmp, so the same line goes to
                 * the app's own files directory, which `run-as` can read while the
                 * run is still going.  Writing on this cadence costs a few
                 * microseconds per frame, far below the measurement resolution. */
                if (FILE *log = std::fopen("/data/user/0/io.github.jichuo1.meguviz.qcom4kedge/files/qcom-jitter-stats.txt", "a")) {
                    std::fprintf(log, "n=%llu strategy=%d queue_mean_ms=%.3f queue_max_ms=%.3f "
                        "spin_mean_ms=%.3f block_mean_ms=%.3f block_fallbacks=%llu "
                        "gpu_mean_ms=%.3f gpu_max_ms=%.3f\n",
                        static_cast<unsigned long long>(statsCount), waitStrategy,
                        queueMean, double(statsQueueMax) / 1e6, spinMean, blockMean,
                        static_cast<unsigned long long>(statsBlockFallbacks),
                        gpuMean, double(statsGpuMax) / 1e6);
                    std::fclose(log);
                }
                statsQueueSum = statsQueueMax = statsSpinSum = statsBlockSum = 0;
                statsBlockFallbacks = 0;
                statsGpuSum = statsGpuMax = 0;
            }
        }
    }
};

/* Applies the debug property overrides described next to readProperty().  Called
 * before initialize() so the shader sees the calibrated thresholds from the very
 * first dispatch. */
static void applyStaticOverrides(QcomMotion &motion)
{
#if defined(__ANDROID__)
    readProperty("debug.meguviz.qcom.static.milli", motion.staticMilli);
    readProperty("debug.meguviz.qcom.static.neighbor_milli", motion.staticNeighbourMilli);
    int gate = motion.staticGate ? 1 : 0;
    readProperty("debug.meguviz.qcom.static.gate", gate);
    motion.staticGate = gate != 0;
    readProperty("debug.meguviz.qcom.adaptive.mode", motion.adaptiveMode);
    readProperty("debug.meguviz.qcom.adaptive.confidence", motion.confidenceGateMilli);
    /* Wait policy is a latency-only switch, so it is exposed as one property:
     *   0 or absent  blocking vkWaitForFences (historical)
     *   1            poll with the fixed default budget, then block
     *   2            poll with the adaptive budget, then block
     * 2 is what the device A/B picked; 0 keeps an unconfigured handle identical
     * to the pre-existing library. */
    int waitMode = 0;
    readProperty("debug.meguviz.qcom.wait.strategy", waitMode);
    if (waitMode == 2) {
        motion.waitStrategy = QCOM_MOTION_WAIT_POLL;
        motion.waitSpinNsAdaptive = true;
    } else if (waitMode == 1) {
        motion.waitStrategy = QCOM_MOTION_WAIT_POLL;
        motion.waitSpinNsAdaptive = false;
        motion.waitSpinNs = QCOM_MOTION_WAIT_SPIN_NS_DEFAULT;
    } else {
        motion.waitStrategy = QCOM_MOTION_WAIT_BLOCK;
        motion.waitSpinNsAdaptive = false;
    }
    int stats = 0;
    readProperty("debug.meguviz.qcom.wait.stats", stats);
    motion.statsEnabled = stats != 0;
    /* The alternate-window gate (modes 2 and 3) is not shipped, so a debug property
     * asking for it is clamped to the reduction-only mode rather than silently
     * running a variant this build cannot reproduce from source. */
    if (motion.adaptiveMode < 0) motion.adaptiveMode = 0;
    if (motion.adaptiveMode > 1) motion.adaptiveMode = 1;
    if (motion.staticMilli <= 0 || motion.staticNeighbourMilli < motion.staticMilli) {
        motion.staticMilli = kStaticMilli;
        motion.staticNeighbourMilli = kStaticNeighborMilli;
    }
#else
    (void)motion;
#endif
}

extern "C" QcomMotion *qcom_motion_create(int metric)
{
    if (metric != 0 && metric != 1) return nullptr;
    try {
        auto motion = std::make_unique<QcomMotion>();
        applyStaticOverrides(*motion);
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


extern "C" void qcom_motion_set_subpixel(QcomMotion *motion, int enabled)
{
    if (motion) motion->subpixel = enabled != 0;
}

extern "C" int qcom_motion_set_static_thresholds(QcomMotion *motion, int threshold_milli,
                                                int neighbor_milli)
{
    if (!motion || threshold_milli <= 0 || neighbor_milli < threshold_milli) return -1;
    motion->staticMilli = threshold_milli;
    motion->staticNeighbourMilli = neighbor_milli;
    return 0;
}

extern "C" void qcom_motion_set_static_gate(QcomMotion *motion, int enabled)
{
    if (motion) motion->staticGate = enabled != 0;
}

extern "C" int qcom_motion_set_adaptive(QcomMotion *motion, int mode, int confidence_gate_milli)
{
    if (!motion) return -1;
    /* Only modes 0 (default pipeline) and 1 (parallel reduction) exist in this
     * build.  Modes 2 and 3 would mean the alternate-hypothesis gate, which was
     * rejected on device quality evidence, so they are refused rather than
     * silently downgraded - a caller must not believe it enabled something that
     * is not there. */
    if (mode < 0 || mode > 1) return -1;
    if (confidence_gate_milli < 0 || confidence_gate_milli > 1000) return -1;
    motion->adaptiveMode = mode;
    motion->confidenceGateMilli = confidence_gate_milli;
    return 0;
}

extern "C" int qcom_motion_set_wait_strategy(QcomMotion *motion, int strategy)
{
    if (!motion) return -1;
    if (strategy != QCOM_MOTION_WAIT_BLOCK && strategy != QCOM_MOTION_WAIT_POLL) return -1;
    motion->waitStrategy = strategy;
    return 0;
}

extern "C" int qcom_motion_set_wait_spin_ns(QcomMotion *motion, uint64_t spin_ns)
{
    if (!motion) return -1;
    /* A spin budget beyond one 60 Hz frame cannot help: the blocking fallback
     * would already have been reached, so clamp instead of rejecting.  Passing 0
     * selects the adaptive budget, which follows the previous frame's retire
     * interval instead of a fixed value; the fixed budget is left at the default
     * so the one frame before the predictor has data still spins usefully. */
    constexpr uint64_t ceiling = 20ull * 1000 * 1000;
    motion->waitSpinNsAdaptive = spin_ns == 0;
    motion->waitSpinNs = spin_ns == 0 ? QCOM_MOTION_WAIT_SPIN_NS_DEFAULT
                                      : (spin_ns > ceiling ? ceiling : spin_ns);
    return 0;
}

static int estimateChecked(QcomMotion *motion, const uint8_t *previous, const uint8_t *current,
                           float *vectors, float *confidence, float *candidates,
                           uint8_t *staticMask, float *visibility,
                           QcomMotionTiming *timing, int candidateMode)
{
    if (!motion || motion->ahb || !previous || !current || !vectors) return -1;
    if (motion->failed) return -2;
    try {
        motion->estimate(previous, current, vectors, confidence, candidates, staticMask, visibility, timing, candidateMode);
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
    return estimateChecked(motion, previous, current, vectors, confidence, nullptr, nullptr, nullptr, timing, 0);
}

extern "C" int qcom_motion_estimate_candidates(QcomMotion *motion, const uint8_t *previous,
                                               const uint8_t *current, float *vectors, float *confidence,
                                               float *candidates, QcomMotionTiming *timing, int candidateMode)
{
    if (!candidates ||
        (candidateMode != QCOM_MOTION_CANDIDATE_LOCAL && candidateMode != QCOM_MOTION_CANDIDATE_WIDE))
        return -1;
    return estimateChecked(motion, previous, current, vectors, confidence, candidates, nullptr, nullptr, timing, candidateMode);
}

extern "C" int qcom_motion_estimate_extended(QcomMotion *motion, const uint8_t *previous, const uint8_t *current,
                                             float *vectors, float *confidence, float *candidates,
                                             uint8_t *static_mask, float *visibility,
                                             QcomMotionTiming *timing, int candidate_mode)
{
    /* Mode 0 means "no candidate list", which is the plain-estimate behaviour plus
     * the new outputs; 1 and 2 are the frozen candidate modes. */
    if (candidate_mode < 0 || candidate_mode > QCOM_MOTION_CANDIDATE_WIDE)
        return -1;
    if (candidate_mode == 0 && candidates)
        return -1;
    return estimateChecked(motion, previous, current, vectors, confidence, candidates,
                          static_mask, visibility, timing, candidate_mode);
}

#include "ahb_motion_api.inc"
