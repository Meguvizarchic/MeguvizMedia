#include "ahb_contract.hpp"
#include "ahb_motion_api.h"
#include <unpack_spv.h>
#include <fcntl.h>
#include <unistd.h>

namespace {
constexpr unsigned ahbSlots = 3;
constexpr VkFormat lumaAhbFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageUsageFlags lumaAhbUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
constexpr auto externalImageType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
constexpr auto externalSyncType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
constexpr VkImageSubresourceRange colorSubresources{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

bool supportsAhb(VkInstance instance, VkPhysicalDevice physical,
                 const std::vector<VkExtensionProperties> &extensions)
{
    if (!extensionAvailable(extensions, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) ||
        !extensionAvailable(extensions, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) ||
        !extensionAvailable(extensions, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME))
        return false;
    auto imageCaps = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceImageFormatProperties2"));
    auto semaphoreCaps = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceExternalSemaphoreProperties"));
    if (!imageCaps || !semaphoreCaps)
        return false;
    VkPhysicalDeviceExternalSemaphoreInfo sem{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    sem.handleType = externalSyncType;
    VkExternalSemaphoreProperties semProps{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    semaphoreCaps(physical, &sem, &semProps);
    if (!(semProps.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) ||
        !(semProps.compatibleHandleTypes & externalSyncType))
        return false;
    VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    external.handleType = externalImageType;
    VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    query.pNext = &external;
    query.format = lumaAhbFormat;
    query.type = VK_IMAGE_TYPE_2D;
    query.tiling = VK_IMAGE_TILING_OPTIMAL;
    query.usage = lumaAhbUsage;
    VkExternalImageFormatProperties externalProps{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    props.pNext = &externalProps;
    return imageCaps(physical, &query, &props) == VK_SUCCESS &&
           (externalProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) &&
           (externalProps.externalMemoryProperties.compatibleHandleTypes & externalImageType) &&
           props.imageFormatProperties.maxExtent.width >= qcom::ahb::packedWidth &&
           props.imageFormatProperties.maxExtent.height >= height;
}

struct NativeFd {
    static void close(int fd) { ::close(fd); }
};

struct AhbInput {
    VkDevice device;
    uint32_t family;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID properties = nullptr;
    PFN_vkImportSemaphoreFdKHR importFd = nullptr;
    std::array<Image, ahbSlots> imported{};
    std::array<VkSemaphore, 2> semaphores{};
    std::array<VkDescriptorSet, 2> sets{};
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;

    AhbInput(VkDevice device, uint32_t family) : device(device), family(family) {}
    ~AhbInput()
    {
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (shader) vkDestroyShaderModule(device, shader, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (descriptors) vkDestroyDescriptorPool(device, descriptors, nullptr);
        if (layout) vkDestroyDescriptorSetLayout(device, layout, nullptr);
        if (sampler) vkDestroySampler(device, sampler, nullptr);
        for (auto &image : imported) image.clear(device);
        for (auto semaphore : semaphores)
            if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
    }

    void create()
    {
        properties = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
            vkGetDeviceProcAddr(device, "vkGetAndroidHardwareBufferPropertiesANDROID"));
        importFd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
            vkGetDeviceProcAddr(device, "vkImportSemaphoreFdKHR"));
        if (!properties || !importFd)
            throw std::runtime_error("missing Android AHB Vulkan device entry");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto &value : semaphores)
            checked(vkCreateSemaphore(device, &semaphore, nullptr, &value), "create input SYNC_FD semaphore");

        VkSamplerCreateInfo sampling{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampling.magFilter = sampling.minFilter = VK_FILTER_NEAREST;
        sampling.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampling.addressModeU = sampling.addressModeV = sampling.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        checked(vkCreateSampler(device, &sampling, nullptr, &sampler), "create AHB sampler");
        const VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &sampler},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings = bindings;
        checked(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout), "create AHB prep set layout");
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
        };
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 2;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes;
        checked(vkCreateDescriptorPool(device, &pool, nullptr, &descriptors), "create AHB prep pool");
        const VkDescriptorSetLayout layouts[] = {layout, layout};
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptors;
        allocate.descriptorSetCount = 2;
        allocate.pSetLayouts = layouts;
        checked(vkAllocateDescriptorSets(device, &allocate, sets.data()), "allocate AHB prep sets");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 3 * sizeof(uint32_t)};
        VkPipelineLayoutCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineInfo.setLayoutCount = 1;
        pipelineInfo.pSetLayouts = &layout;
        pipelineInfo.pushConstantRangeCount = 1;
        pipelineInfo.pPushConstantRanges = &push;
        checked(vkCreatePipelineLayout(device, &pipelineInfo, nullptr, &pipelineLayout), "create AHB prep pipeline layout");
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = sizeof(qcom_unpack_spv);
        module.pCode = qcom_unpack_spv;
        checked(vkCreateShaderModule(device, &module, nullptr, &shader), "create AHB unpack module");
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.layout = pipelineLayout;
        compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                         VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
        checked(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute, nullptr, &pipeline),
                "create AHB unpack pipeline");
    }

    void attach(unsigned slot, AHardwareBuffer *hardwareBuffer)
    {
        if (!hardwareBuffer || slot >= imported.size() || imported[slot].object)
            throw std::runtime_error("invalid or occupied AHB input slot");
        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(hardwareBuffer, &desc);
        if (desc.width != qcom::ahb::packedWidth || desc.height != height || desc.layers != 1 ||
            desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ||
            !(desc.usage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE) ||
            !(desc.usage & AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER))
            throw std::runtime_error("unsupported luma AHB geometry, format or usage");
        VkAndroidHardwareBufferFormatPropertiesANDROID formatProperties{
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
        VkAndroidHardwareBufferPropertiesANDROID memory{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
        memory.pNext = &formatProperties;
        checked(properties(device, hardwareBuffer, &memory), "AHB memory properties");
        if (formatProperties.format != lumaAhbFormat ||
            !(formatProperties.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
            !(formatProperties.formatFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ||
            !memory.allocationSize || !memory.memoryTypeBits)
            throw std::runtime_error("AHB Vulkan format is not sampleable and renderable RGBA8");

        Image image{};
        try {
            image.extent = {desc.width, desc.height, 1};
            VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
            external.handleTypes = externalImageType;
            VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            create.pNext = &external;
            create.imageType = VK_IMAGE_TYPE_2D;
            create.format = lumaAhbFormat;
            create.extent = image.extent;
            create.mipLevels = create.arrayLayers = 1;
            create.samples = VK_SAMPLE_COUNT_1_BIT;
            create.tiling = VK_IMAGE_TILING_OPTIMAL;
            create.usage = lumaAhbUsage;
            checked(vkCreateImage(device, &create, nullptr, &image.object), "create luma import image");
            VkImportAndroidHardwareBufferInfoANDROID externalMemory{
                VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
            externalMemory.buffer = hardwareBuffer;
            VkMemoryDedicatedAllocateInfo dedicate{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
            dedicate.pNext = &externalMemory;
            dedicate.image = image.object;
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.pNext = &dedicate;
            allocate.allocationSize = memory.allocationSize;
            allocate.memoryTypeIndex = static_cast<uint32_t>(__builtin_ctz(memory.memoryTypeBits));
            checked(vkAllocateMemory(device, &allocate, nullptr, &image.memory), "allocate luma AHB import");
            checked(vkBindImageMemory(device, image.object, image.memory, 0), "bind luma AHB image");
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image.object;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = lumaAhbFormat;
            view.subresourceRange = colorSubresources;
            checked(vkCreateImageView(device, &view, nullptr, &image.view), "create luma AHB view");
            imported[slot] = image;
        } catch (...) {
            image.clear(device);
            throw;
        }
    }

    void waitOn(const QcomAhbFrame (&inputs)[2])
    {
        if (inputs[0].slot == inputs[1].slot)
            throw std::runtime_error("duplicate AHB slot");
        for (unsigned index = 0; index < 2; ++index) {
            const auto &input = inputs[index];
            if (input.slot >= imported.size() || !imported[input.slot].object || input.acquireFd < 0)
                throw std::runtime_error("missing AHB input or producer fence");
            qcom::ahb::FenceFd<NativeFd> duplicate(fcntl(input.acquireFd, F_DUPFD_CLOEXEC, 0));
            if (!duplicate.transferToVulkan([&](int fd) {
                VkImportSemaphoreFdInfoKHR fence{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
                fence.semaphore = semaphores[index];
                fence.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
                fence.handleType = externalSyncType;
                fence.fd = fd;
                return importFd(device, &fence) == VK_SUCCESS;
            }))
                throw std::runtime_error("duplicate/import input SYNC_FD failed");
        }
    }

    void record(VkCommandBuffer command, const QcomAhbFrame (&inputs)[2], const std::array<Image, 4> &images,
                const Buffer &staging, bool rgba)
    {
        VkImageMemoryBarrier access[2]{};
        for (unsigned i = 0; i < 2; ++i) {
            const Image &image = imported[inputs[i].slot];
            VkDescriptorImageInfo sample{sampler, image.view, VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorBufferInfo output{staging.object, 0, staging.size};
            VkWriteDescriptorSet writes[2]{};
            for (unsigned j = 0; j < 2; ++j) {
                writes[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[j].dstSet = sets[i];
                writes[j].dstBinding = j;
                writes[j].descriptorCount = 1;
            }
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &sample;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[1].pBufferInfo = &output;
            vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
            access[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            access[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            access[i].oldLayout = access[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            access[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
            access[i].dstQueueFamilyIndex = family;
            access[i].image = image.object;
            access[i].subresourceRange = colorSubresources;
        }
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 2, access);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        for (unsigned i = 0; i < 2; ++i) {
            const uint32_t args[]{static_cast<uint32_t>(images[2+i].offset / sizeof(uint32_t)),
                                  static_cast<uint32_t>(images[i].offset / sizeof(uint32_t)), rgba ? 1u : 0u};
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &sets[i], 0, nullptr);
            vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), args);
            vkCmdDispatch(command, qcom::ahb::packedWidth / 8, height / 8, 1);
            access[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            access[i].dstAccessMask = 0;
            std::swap(access[i].srcQueueFamilyIndex, access[i].dstQueueFamilyIndex);
        }
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 2, access);
        VkBufferMemoryBarrier output{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        output.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        output.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        output.srcQueueFamilyIndex = output.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        output.buffer = staging.object;
        output.size = staging.size;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 1, &output, 0, nullptr);
    }
};
}

