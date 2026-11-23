#pragma once
#include "nr_diag_fix.hpp"
#include "nr_device_features.hpp"
#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>

namespace nr::diagfix {
inline const VkBaseInStructure* find(const void* chain, VkStructureType type) {
    for (auto* p = static_cast<const VkBaseInStructure*>(chain); p; p = p->pNext)
        if (p->sType == type) return p;
    return nullptr;
}
inline bool listed(const std::vector<const char*>& list, const char* name) {
    return std::any_of(list.begin(), list.end(), [name](const char* p) { return !std::strcmp(p, name); });
}
using Report = std::function<void(const std::string&)>;
struct Rule {
    VkStructureType type;
    uint32_t promoted;
    const char* extension;
    const char* label;
    std::vector<size_t> bits;
    VkStructureType aggregate = VK_STRUCTURE_TYPE_MAX_ENUM;
    std::vector<size_t> aggregate_bits;
};
inline const std::vector<Rule>& rules() {
#define NR_BIT(T, f) offsetof(T, f)
    static const std::vector<Rule> r = {
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, VK_API_VERSION_1_1, VK_KHR_16BIT_STORAGE_EXTENSION_NAME,
         "VkPhysicalDevice16BitStorageFeatures", {NR_BIT(VkPhysicalDevice16BitStorageFeatures, storageBuffer16BitAccess)},
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, {NR_BIT(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess)}},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, VK_API_VERSION_1_2, VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
         "VkPhysicalDevice8BitStorageFeatures", {NR_BIT(VkPhysicalDevice8BitStorageFeatures, storageBuffer8BitAccess)},
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, {NR_BIT(VkPhysicalDeviceVulkan12Features, storageBuffer8BitAccess)}},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, VK_API_VERSION_1_2, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,
         "VkPhysicalDeviceShaderFloat16Int8Features", {NR_BIT(VkPhysicalDeviceShaderFloat16Int8Features, shaderFloat16), NR_BIT(VkPhysicalDeviceShaderFloat16Int8Features, shaderInt8)},
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, {NR_BIT(VkPhysicalDeviceVulkan12Features, shaderFloat16), NR_BIT(VkPhysicalDeviceVulkan12Features, shaderInt8)}},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, VK_API_VERSION_1_2, VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME,
         "VkPhysicalDeviceVulkanMemoryModelFeatures", {NR_BIT(VkPhysicalDeviceVulkanMemoryModelFeatures, vulkanMemoryModel)},
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, {NR_BIT(VkPhysicalDeviceVulkan12Features, vulkanMemoryModel)}},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VK_API_VERSION_1_2, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
         "VkPhysicalDeviceBufferDeviceAddressFeatures", {NR_BIT(VkPhysicalDeviceBufferDeviceAddressFeatures, bufferDeviceAddress)},
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, {NR_BIT(VkPhysicalDeviceVulkan12Features, bufferDeviceAddress)}},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, VK_API_VERSION_1_3, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
         "VkPhysicalDeviceSubgroupSizeControlFeatures", {NR_BIT(VkPhysicalDeviceSubgroupSizeControlFeatures, subgroupSizeControl)},
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, {NR_BIT(VkPhysicalDeviceVulkan13Features, subgroupSizeControl)}},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, UINT32_MAX, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
         "VkPhysicalDeviceCooperativeMatrixFeaturesKHR", {NR_BIT(VkPhysicalDeviceCooperativeMatrixFeaturesKHR, cooperativeMatrix)}},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR, UINT32_MAX, VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME,
         "VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR", {NR_BIT(VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR, workgroupMemoryExplicitLayout), NR_BIT(VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR, workgroupMemoryExplicitLayout8BitAccess), NR_BIT(VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR, workgroupMemoryExplicitLayout16BitAccess)}},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT, UINT32_MAX, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME,
         "VkPhysicalDeviceShaderFloat8FeaturesEXT", {NR_BIT(VkPhysicalDeviceShaderFloat8FeaturesEXT, shaderFloat8), NR_BIT(VkPhysicalDeviceShaderFloat8FeaturesEXT, shaderFloat8CooperativeMatrix)}}
    };
#undef NR_BIT
    return r;
}
inline bool bit(const void* p, size_t off) {
    VkBool32 value = 0;
    if (p) std::memcpy(&value, static_cast<const char*>(p) + off, sizeof value);
    return value != 0;
}
inline void set_bit(void* p, size_t off, bool value) {
    const VkBool32 v = value;
    std::memcpy(static_cast<char*>(p) + off, &v, sizeof v);
}
inline bool available(const Rule& r, uint32_t api, const std::vector<const char*>& have) {
    if (api < VK_API_VERSION_1_1 &&
        (r.type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES || r.type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES) &&
        !listed(have, VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME)) return false;
    if (api < VK_API_VERSION_1_1 && r.type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES &&
        !listed(have, VK_KHR_DEVICE_GROUP_EXTENSION_NAME)) return false;
    return api >= r.promoted || listed(have, r.extension);
}
inline void legalize(DeviceFeatures& features, const DeviceFeatures& support, VkPhysicalDeviceFeatures& core,
                     const VkPhysicalDeviceFeatures& supported_core, uint32_t api, arch::GpuArch gpu,
                     const std::vector<const char*>& have, std::vector<const char*>& extensions, Fix fix, const Report& report) {
    if (!device(fix)) return;
    auto add = [&](const char* extension) {
        if (!listed(extensions, extension)) { extensions.push_back(extension); report(std::string("add extension ") + extension); }
    };
    if (nobda(fix)) {
        extensions.erase(std::remove_if(extensions.begin(), extensions.end(), [&](const char* e) {
            const bool drop = !std::strcmp(e, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) || !std::strcmp(e, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
            if (drop) report(std::string("drop extension ") + e);
            return drop;
        }), extensions.end());
        if (find(features.head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT))
            report("drop struct VkPhysicalDeviceBufferDeviceAddressFeaturesEXT (forced no-BDA)");
        features.erase(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT);
        if (auto* v12 = features.find<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES))
            v12->bufferDeviceAddress = v12->bufferDeviceAddressCaptureReplay = v12->bufferDeviceAddressMultiDevice = VK_FALSE;
    }
    for (const auto& r : rules()) {
        const bool bda = r.type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        const auto* supported = find(support.head, r.type);
        bool enable = available(r, api, have) && !(bda && nobda(fix)) &&
            (r.type != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT || gpu == arch::GpuArch::rdna4);
        for (size_t off : r.bits) enable = enable && bit(supported, off);
        auto* aggregate = const_cast<VkBaseInStructure*>(find(features.head, r.aggregate));
        if (aggregate) {
            for (size_t off : r.aggregate_bits) set_bit(aggregate, off, enable);
            features.erase(r.type);
            if (enable && api < r.promoted) add(r.extension);
            report(std::string(enable ? "enable core fields for " : "drop core fields for ") + r.label);
        } else if (enable) {
            auto* p = features.get_raw(r.type);
            for (size_t off : r.bits) set_bit(p, off, true);
            if (api < r.promoted) add(r.extension);
            report(std::string("add/enable struct ") + r.label);
        } else {
            features.erase(r.type);
            report(std::string("drop struct ") + r.label + (bda && nobda(fix) ? " (forced no-BDA)" : " (unsupported or not required)"));
            if (aggregate) for (size_t off : r.aggregate_bits) set_bit(aggregate, off, false);
        }
    }
    if (api < VK_API_VERSION_1_1) {
        if ((find(features.head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES) || find(features.head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES)) &&
            listed(have, VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME)) add(VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME);
        if (find(features.head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) && listed(have, VK_KHR_DEVICE_GROUP_EXTENSION_NAME)) add(VK_KHR_DEVICE_GROUP_EXTENSION_NAME);
    }
    core.shaderInt16 = supported_core.shaderInt16;
    report(core.shaderInt16 ? "enable core shaderInt16" : "drop core shaderInt16 (unsupported)");
}
inline Caps inspect(const VkDeviceCreateInfo& info, uint32_t api, arch::GpuArch gpu) {
    std::vector<const char*> extensions;
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i) extensions.push_back(info.ppEnabledExtensionNames[i]);
    Caps caps;
    caps.required = true;
    for (const auto& r : rules()) {
        const auto* p = find(info.pNext, r.type);
        const auto* aggregate = find(info.pNext, r.aggregate);
        bool enabled = api >= r.promoted || listed(extensions, r.extension);
        for (size_t i = 0; i < r.bits.size(); ++i)
            enabled = enabled && (aggregate ? bit(aggregate, r.aggregate_bits[i]) : bit(p, r.bits[i]));
        if (r.type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) caps.bda = enabled;
        else {
            if (r.type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES) caps.subgroup = enabled;
            if (r.type != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT || gpu == arch::GpuArch::rdna4)
                caps.required = caps.required && enabled;
        }
    }
    const auto* f2 = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(find(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2));
    const auto* core = f2 ? &f2->features : info.pEnabledFeatures;
    caps.required = caps.required && core && core->shaderInt16;
    const auto* pr = reinterpret_cast<const VkPhysicalDevicePipelineRobustnessFeaturesEXT*>(find(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT));
    const auto* v14 = reinterpret_cast<const VkPhysicalDeviceVulkan14Features*>(find(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES));
    caps.robustness = (pr && pr->pipelineRobustness && (api >= VK_API_VERSION_1_4 || listed(extensions, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME))) ||
        (api >= VK_API_VERSION_1_4 && v14 && v14->pipelineRobustness);
    return caps;
}
}
