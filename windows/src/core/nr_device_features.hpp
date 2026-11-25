#pragma once
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "nr_device_chain_sizes.hpp"
#include "nr_gpu_arch_ids.hpp"

namespace nr {
struct InstanceApiInfo {
    VkInstance instance = VK_NULL_HANDLE;
    uint32_t requested_api = 0;
    bool has_get_physical_device_properties2 = false;
};

inline InstanceApiInfo instance_api_info(VkInstance instance, const VkInstanceCreateInfo* info) {
    InstanceApiInfo out{};
    out.instance = instance;
    out.requested_api = info && info->pApplicationInfo && info->pApplicationInfo->apiVersion
        ? info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    if (info && info->ppEnabledExtensionNames) {
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
            if (info->ppEnabledExtensionNames[i] &&
                std::strcmp(info->ppEnabledExtensionNames[i], VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0) {
                out.has_get_physical_device_properties2 = true;
                break;
            }
        }
    }
    return out;
}

class InstanceApiTracker {
    std::map<VkInstance, InstanceApiInfo> instances_;
    std::map<VkPhysicalDevice, VkInstance> physicals_;
public:
    void note_instance(VkInstance instance, const VkInstanceCreateInfo* info) {
        if (!instance) return;
        instances_[instance] = instance_api_info(instance, info);
    }
    void note_physical_devices(VkInstance instance, uint32_t count, const VkPhysicalDevice* physical) {
        if (!instance || !physical) return;
        for (uint32_t i = 0; i < count; ++i)
            if (physical[i] != VK_NULL_HANDLE) physicals_[physical[i]] = instance;
    }
    void forget_instance(VkInstance instance) {
        if (!instance) return;
        instances_.erase(instance);
        for (auto it = physicals_.begin(); it != physicals_.end();)
            if (it->second == instance) it = physicals_.erase(it);
            else ++it;
    }
    InstanceApiInfo lookup(VkPhysicalDevice physical) const {
        if (!physical) return {};
        const auto p = physicals_.find(physical);
        if (p == physicals_.end()) return {};
        const auto v = instances_.find(p->second);
        if (v == instances_.end()) return InstanceApiInfo{p->second, 0, false};
        return v->second;
    }
};

inline uint32_t infer_min_api_from_pnext(const void* chain) {
    uint32_t api = 0;
    size_t count = 0;
    auto bump = [&api](uint32_t v) { if (v > api) api = v; };
    for (auto* p = static_cast<const VkBaseInStructure*>(chain); p && count++ < 512; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES) bump(VK_API_VERSION_1_4);
        else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) bump(VK_API_VERSION_1_3);
        else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) bump(VK_API_VERSION_1_2);
        else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES ||
                 p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES) bump(VK_API_VERSION_1_1);
    }
    return api;
}

inline uint32_t infer_min_api_from_device_info(const VkDeviceCreateInfo* info) {
    if (!info) return 0;
    uint32_t api = infer_min_api_from_pnext(info->pNext);
    if (info->ppEnabledExtensionNames) {
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
            if (info->ppEnabledExtensionNames[i] &&
                std::strcmp(info->ppEnabledExtensionNames[i], VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME) == 0) {
                if (VK_API_VERSION_1_1 > api) api = VK_API_VERSION_1_1;
                break;
            }
        }
    }
    return api;
}

inline uint32_t effective_device_api(uint32_t requested, uint32_t supported, const VkDeviceCreateInfo* device_info = nullptr) {
    if (requested != 0) return requested < supported ? requested : supported;
    const uint32_t inferred = infer_min_api_from_device_info(device_info);
    if (inferred == 0) return 0;
    return inferred < supported ? inferred : supported;
}

inline bool can_query_physical_device_features2(uint32_t api, bool has_get_physical_device_properties2) {
    return api >= VK_API_VERSION_1_1 || (api >= VK_API_VERSION_1_0 && has_get_physical_device_properties2);
}

struct PipelineRobustnessRequirements {
    bool can_query;
    bool enabled;
    bool extension;
    PipelineRobustnessRequirements(uint32_t api, bool has_get_physical_device_properties2, bool offered, bool feature)
        : can_query(can_query_physical_device_features2(api, has_get_physical_device_properties2)),
          enabled(can_query && feature && (api >= VK_API_VERSION_1_4 || offered)),
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
    bool known_api;
    bool buffer_device_address;
    std::vector<const char*> extensions;

    DeviceFeatureRequirements(uint32_t api, bool x86, nr::arch::GpuArch arch = nr::arch::GpuArch::rdna4)
        : known_api(api >= VK_API_VERSION_1_0),
          buffer_device_address(known_api && (!x86 || api >= VK_API_VERSION_1_2)) {
        if (!known_api) return;
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
            if (arch == nr::arch::GpuArch::rdna2) {
                extensions.push_back(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
                extensions.push_back(VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME);
            }
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
    void enable_integer_dot_product() {
        if (auto* f = find<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES))
            f->shaderIntegerDotProduct = VK_TRUE;
        else get<VkPhysicalDeviceShaderIntegerDotProductFeatures>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES)->shaderIntegerDotProduct = VK_TRUE;
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
        if (arch != nr::arch::GpuArch::rdna2)
            get<VkPhysicalDeviceCooperativeMatrixFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR)->cooperativeMatrix = true;
        // The FP8 features are RDNA4's network's; RDNA3 and RDNA2 multiply FP16/INT8 operands and enable nothing here.
        switch (arch) {
        case nr::arch::GpuArch::rdna4: {
            auto* f = get<VkPhysicalDeviceShaderFloat8FeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT);
            f->shaderFloat8 = f->shaderFloat8CooperativeMatrix = true;
            break;
        }
        case nr::arch::GpuArch::rdna3:
        case nr::arch::GpuArch::rdna2:
        case nr::arch::GpuArch::unknown: case nr::arch::GpuArch::rdna1:
            break;
        }
    }
};
} // namespace nr
