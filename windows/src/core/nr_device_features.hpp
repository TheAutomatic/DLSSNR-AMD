#pragma once
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "nr_device_chain_sizes.hpp"
#include "nr_gpu_arch_ids.hpp"

namespace nr {
inline uint32_t effective_device_api(uint32_t requested, uint32_t supported) {
    if (!requested) return supported;
    return requested < supported ? requested : supported;
}

struct PipelineRobustnessRequirements {
    bool enabled;
    bool extension;
    PipelineRobustnessRequirements(uint32_t api, bool offered, bool feature)
        : enabled(feature && (api >= VK_API_VERSION_1_4 || offered)),
          extension(enabled && api < VK_API_VERSION_1_4) {}
};

inline bool pipeline_robustness_enabled(const void* chain) {
    size_t count = 0;
    for (auto* p = static_cast<const VkBaseInStructure*>(chain); p && count++ < 512; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES)
            return reinterpret_cast<const VkPhysicalDeviceVulkan14Features*>(p)->pipelineRobustness != 0;
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT)
            return reinterpret_cast<const VkPhysicalDevicePipelineRobustnessFeaturesEXT*>(p)->pipelineRobustness != 0;
    }
    return false;
}

struct DeviceFeatureRequirements {
    bool buffer_device_address;
    std::vector<const char*> extensions;

    DeviceFeatureRequirements(uint32_t api, bool x86)
        : buffer_device_address(!x86 || api >= VK_API_VERSION_1_2) {
        if (api < VK_API_VERSION_1_1) {
            extensions.push_back(VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME);
            extensions.push_back(VK_KHR_16BIT_STORAGE_EXTENSION_NAME);
            if (buffer_device_address) extensions.push_back(VK_KHR_DEVICE_GROUP_EXTENSION_NAME);
        }
        if (api < VK_API_VERSION_1_2) {
            extensions.push_back(VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
            extensions.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
            extensions.push_back(VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME);
            if (buffer_device_address) extensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        }
        if (api < VK_API_VERSION_1_3)
            extensions.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    }
};

// Preserve app feature fields and all known extension structures. Never write
// through the application's const pNext pointers or prepend duplicate features.
class DeviceFeatures {
    using Node = std::unique_ptr<void, decltype(&std::free)>;
    std::vector<Node> nodes;
public:
    VkBaseOutStructure* head{};
    explicit DeviceFeatures(const void* chain) {
        VkBaseOutStructure** tail = &head;
        for (auto p = static_cast<const VkBaseInStructure*>(chain); p; p = p->pNext) {
            if (nodes.size() >= 512) throw std::runtime_error("cyclic or oversized device pNext");
            const size_t bytes = nr_device_chain_size(p->sType);
            if (!bytes) throw std::runtime_error("unknown device pNext sType " + std::to_string(p->sType));
            auto* copy = static_cast<VkBaseOutStructure*>(std::malloc(bytes));
            if (!copy) throw std::bad_alloc();
            nodes.emplace_back(copy, std::free);
            std::memcpy(copy, p, bytes); copy->pNext = nullptr;
            *tail = copy; tail = &copy->pNext;
        }
    }
    template<class T> T* find(VkStructureType type) {
        for (auto* p = head; p; p = p->pNext)
            if (p->sType == type) return reinterpret_cast<T*>(p);
        return nullptr;
    }
    template<class T> T* get(VkStructureType type) {
        if (auto* p = find<T>(type)) return p;
        auto* p = static_cast<T*>(std::calloc(1, sizeof(T)));
        if (!p) throw std::bad_alloc();
        nodes.emplace_back(p, std::free);
        p->sType = type; p->pNext = head;
        head = reinterpret_cast<VkBaseOutStructure*>(p);
        return p;
    }
    void enable_pipeline_robustness() {
        if (auto* f = find<VkPhysicalDeviceVulkan14Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES))
            f->pipelineRobustness = VK_TRUE;
        else get<VkPhysicalDevicePipelineRobustnessFeaturesEXT>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT)->pipelineRobustness = VK_TRUE;
    }
    // The features the network of `arch` (nr_gpu_arch.hpp; one NR has a network for, checked by the caller) needs.
    void enable(nr::arch::GpuArch arch, bool buffer_device_address = true) {
        if (auto* f = find<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES))
            f->storageBuffer16BitAccess = true;
        else get<VkPhysicalDevice16BitStorageFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES)->storageBuffer16BitAccess = true;
        if (auto* f = find<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)) {
            f->storageBuffer8BitAccess = f->shaderFloat16 = f->shaderInt8 = f->vulkanMemoryModel = true;
            if (buffer_device_address) f->bufferDeviceAddress = true;
        } else {
            get<VkPhysicalDevice8BitStorageFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES)->storageBuffer8BitAccess = true;
            auto* half = get<VkPhysicalDeviceShaderFloat16Int8Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES);
            half->shaderFloat16 = half->shaderInt8 = true;
            get<VkPhysicalDeviceVulkanMemoryModelFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES)->vulkanMemoryModel = true;
            // The network reads the activation arena through a buffer_reference on Windows
            // (vit_attn.comp NR_VBDA). vkd3d-proton enables it anyway; DXVK does not.
            if (buffer_device_address)
                get<VkPhysicalDeviceBufferDeviceAddressFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)->bufferDeviceAddress = true;
        }
        if (auto* f = find<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES))
            f->subgroupSizeControl = true;
        else get<VkPhysicalDeviceSubgroupSizeControlFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES)->subgroupSizeControl = true;
        get<VkPhysicalDeviceCooperativeMatrixFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR)->cooperativeMatrix = true;
        // The FP8 features are RDNA4's network's; RDNA3's multiplies FP16 operands and enables nothing here.
        switch (arch) {
        case nr::arch::GpuArch::rdna4: {
            auto* f = get<VkPhysicalDeviceShaderFloat8FeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT);
            f->shaderFloat8 = f->shaderFloat8CooperativeMatrix = true;
            break;
        }
        case nr::arch::GpuArch::rdna3:
        case nr::arch::GpuArch::unknown: case nr::arch::GpuArch::rdna1: case nr::arch::GpuArch::rdna2:
            break;
        }
    }
};
} // namespace nr
