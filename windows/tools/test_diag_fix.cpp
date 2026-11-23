#include "../src/core/nr_diag_device.hpp"
#include <cassert>
#include <cstdio>
#include <set>

using namespace nr::diagfix;
using nr::DeviceFeatures;
constexpr auto gpu = nr::arch::GpuArch::rdna4;
static int address_lookup = 0;
static VKAPI_ATTR VkDeviceAddress VKAPI_CALL fake_address(VkDevice, const VkBufferDeviceAddressInfo*) { return 123; }
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char* name) {
    if ((address_lookup == 1 && !std::strcmp(name, "vkGetBufferDeviceAddress")) ||
        (address_lookup == 2 && !std::strcmp(name, "vkGetBufferDeviceAddressKHR")))
        return reinterpret_cast<PFN_vkVoidFunction>(fake_address);
    return nullptr;
}

int main() {
    for (const auto& p : std::vector<std::pair<const char*, Fix>>{
            {nullptr, Fix::none}, {"", Fix::none}, {"device", Fix::device}, {"device-nobda", Fix::device_nobda},
            {"robust", Fix::robust}, {"all", Fix::all}, {"baseline", Fix::none}, {"ALL", Fix::none},
            {" device", Fix::none}, {"device ", Fix::none}, {"device-nobdaX", Fix::none}, {"0", Fix::none}})
        assert(parse(p.first) == p.second);
    std::vector<const char*> have;
    DeviceFeatures support(nullptr);
    for (const auto& r : rules()) {
        have.push_back(r.extension);
        auto* p = support.get_raw(r.type);
        for (size_t off : r.bits) set_bit(p, off, true);
    }
    have.push_back(VK_KHR_DEVICE_GROUP_EXTENSION_NAME);
    have.push_back(VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME);
    VkPhysicalDeviceFeatures supported_core{}; supported_core.shaderInt16 = VK_TRUE;
    std::vector<std::string> messages;
    const Report report = [&](const std::string& s) { messages.push_back(s); };
    auto inspect_features = [](DeviceFeatures& f, VkPhysicalDeviceFeatures& core, std::vector<const char*>& ext, uint32_t api) {
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.pNext = f.head; info.pEnabledFeatures = &core;
        info.enabledExtensionCount = uint32_t(ext.size()); info.ppEnabledExtensionNames = ext.data();
        return inspect(info, api, gpu);
    };
    for (uint32_t api : {VK_API_VERSION_1_0, VK_API_VERSION_1_1, VK_API_VERSION_1_2, VK_API_VERSION_1_3}) {
        for (Fix mode : {Fix::device, Fix::device_nobda, Fix::all}) {
            DeviceFeatures f(nullptr);
            std::vector<const char*> extensions;
            VkPhysicalDeviceFeatures core{}; core.shaderInt64 = VK_TRUE;
            legalize(f, support, core, supported_core, api, gpu, have, extensions, mode, report);
            assert(core.shaderInt16 && core.shaderInt64);
            for (const auto& r : rules()) {
                const bool dropped = nobda(mode) && r.type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
                assert(bool(find(f.head, r.type)) == !dropped);
                assert(listed(extensions, r.extension) == (!dropped && api < r.promoted));
            }
            const auto caps = inspect_features(f, core, extensions, api);
            assert(caps.required && caps.subgroup && caps.bda == !nobda(mode));
            const size_t count = extensions.size();
            legalize(f, support, core, supported_core, api, gpu, have, extensions, mode, report);
            assert(extensions.size() == count);
            std::set<VkStructureType> types;
            for (auto* p = f.head; p; p = p->pNext) assert(types.insert(p->sType).second);
        }
    }
    for (const auto& missing : rules()) {
        auto available = have;
        available.erase(std::remove_if(available.begin(), available.end(), [&](const char* p) { return !std::strcmp(p, missing.extension); }), available.end());
        DeviceFeatures f(nullptr); VkPhysicalDeviceFeatures core{}; std::vector<const char*> ext;
        legalize(f, support, core, supported_core, VK_API_VERSION_1_0, gpu, available, ext, Fix::device, report);
        assert(!find(f.head, missing.type) && !listed(ext, missing.extension));
    }
    {
        DeviceFeatures f(nullptr), unsupported(nullptr);
        VkPhysicalDeviceFeatures core{}, none{}; std::vector<const char*> ext;
        legalize(f, unsupported, core, none, VK_API_VERSION_1_3, gpu, have, ext, Fix::device, report);
        assert(!f.head && !core.shaderInt16 && !inspect_features(f, core, ext, VK_API_VERSION_1_3).required);
    }
    {
        DeviceFeatures f(nullptr); VkPhysicalDeviceFeatures core{}; std::vector<const char*> ext;
        auto available = have;
        available.erase(std::remove_if(available.begin(), available.end(), [](const char* p) {
            return !std::strcmp(p, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        }), available.end());
        legalize(f, support, core, supported_core, VK_API_VERSION_1_2, gpu, available, ext, Fix::device, report);
        assert(find(f.head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES));
        assert(!listed(ext, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME));
        assert(inspect_features(f, core, ext, VK_API_VERSION_1_2).bda);
    }
    {
        DeviceFeatures f(nullptr);
        auto* v11 = f.get<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
        auto* v12 = f.get<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        auto* v13 = f.get<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
        v12->descriptorIndexing = VK_TRUE; v12->bufferDeviceAddressCaptureReplay = VK_TRUE;
        f.get<VkPhysicalDeviceBufferDeviceAddressFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT)->bufferDeviceAddress = VK_TRUE;
        std::vector<const char*> ext{VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME};
        VkPhysicalDeviceFeatures core{};
        legalize(f, support, core, supported_core, VK_API_VERSION_1_3, gpu, have, ext, Fix::device_nobda, report);
        assert(v11->storageBuffer16BitAccess && v12->shaderFloat16 && v13->subgroupSizeControl && v12->descriptorIndexing);
        assert(!v12->bufferDeviceAddress && !v12->bufferDeviceAddressCaptureReplay);
        assert(!find(f.head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT));
        assert(!find(f.head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES));
        assert(!listed(ext, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) && !listed(ext, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME));
        assert(inspect_features(f, core, ext, VK_API_VERSION_1_3).required);
        legalize(f, support, core, supported_core, VK_API_VERSION_1_0, gpu, have, ext, Fix::device_nobda, report);
        assert(v12->shaderFloat16 && v12->descriptorIndexing && v13->subgroupSizeControl);
        assert(!find(f.head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES));
        assert(!find(f.head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES));
        assert(inspect_features(f, core, ext, VK_API_VERSION_1_0).required);
    }
    for (Fix mode : {Fix::none, Fix::robust}) {
        DeviceFeatures f(nullptr); f.enable(gpu);
        const auto* head = f.head; VkPhysicalDeviceFeatures core{}; std::vector<const char*> ext;
        legalize(f, support, core, supported_core, VK_API_VERSION_1_1, gpu, have, ext, mode, report);
        assert(f.head == head && ext.empty() && !core.shaderInt16);
    }
    {
        DeviceFeatures f(nullptr); VkPhysicalDeviceFeatures core{}; std::vector<const char*> ext;
        legalize(f, support, core, supported_core, VK_API_VERSION_1_1, nr::arch::GpuArch::rdna3, have, ext, Fix::device, report);
        assert(!find(f.head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT));
    }
    VkPhysicalDevicePipelineRobustnessFeaturesEXT pr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
    pr.pipelineRobustness = VK_TRUE;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; info.pNext = &pr;
    assert(!inspect(info, VK_API_VERSION_1_1, gpu).robustness);
    const char* ext = VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME;
    info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = &ext;
    assert(inspect(info, VK_API_VERSION_1_1, gpu).robustness);
    info.enabledExtensionCount = 0;
    assert(inspect(info, VK_API_VERSION_1_4, gpu).robustness);
    VkPhysicalDeviceVulkan14Features v14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    v14.pipelineRobustness = VK_TRUE;
    info.pNext = &v14;
    assert(inspect(info, VK_API_VERSION_1_4, gpu).robustness);
    assert(!inspect(info, VK_API_VERSION_1_3, gpu).robustness);
    info.pNext = &pr; info.enabledExtensionCount = 1;
    const auto device_handle = reinterpret_cast<VkDevice>(uintptr_t(123));
    setenv("NR_DIAG_FIX", "robust", 1);
    assert(!pipeline_robustness(true, device_handle));
    remember(device_handle, inspect(info, VK_API_VERSION_1_1, gpu));
    assert(pipeline_robustness(true, device_handle));
    forget(device_handle); assert(!pipeline_robustness(true, device_handle));
    unsetenv("NR_DIAG_FIX"); assert(pipeline_robustness(true, device_handle));
    for (address_lookup = 0; address_lookup < 3; ++address_lookup) {
        const auto fn = address_function(device_handle);
        assert(bool(fn) == (address_lookup != 0));
        if (fn) assert(fn(device_handle, nullptr) == 123);
    }
    assert(!messages.empty());
    std::puts("PASS: fix parser valid/unset/empty/invalid; API 1.0/1.1/1.2/1.3 legalization; supported/unsupported extensions/features; promoted core and aggregate preservation; no-BDA removal; shaderInt16; baseline isolation; logical robustness; core/KHR BDA lookup; device retirement");
}
