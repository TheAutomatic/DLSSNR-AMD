#include "../src/core/nr_device_features.hpp"
#include <cassert>
#include <cstdio>
#include <set>

static std::set<std::string> extensions(uint32_t api, bool x86) {
    const nr::DeviceFeatureRequirements required(api, x86);
    std::set<std::string> result(required.extensions.begin(), required.extensions.end());
    assert(result.size() == required.extensions.size());
    assert(required.buffer_device_address == (!x86 || api >= VK_API_VERSION_1_2));
    return result;
}

int main() {
    assert(nr::effective_device_api(0, VK_API_VERSION_1_3) == VK_API_VERSION_1_3);
    assert(nr::effective_device_api(VK_API_VERSION_1_0, VK_API_VERSION_1_3) == VK_API_VERSION_1_0);
    assert(extensions(nr::effective_device_api(0, VK_API_VERSION_1_3), true).empty());
    assert(nr::effective_device_api(VK_API_VERSION_1_1, VK_API_VERSION_1_3) == VK_API_VERSION_1_1);
    assert(nr::effective_device_api(VK_API_VERSION_1_3, VK_API_VERSION_1_2) == VK_API_VERSION_1_2);
    const std::set<std::string> v11 = {
        VK_KHR_8BIT_STORAGE_EXTENSION_NAME, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,
        VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
    };
    auto v10 = v11;
    v10.insert(VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME);
    v10.insert(VK_KHR_16BIT_STORAGE_EXTENSION_NAME);
    assert(extensions(VK_API_VERSION_1_0, true) == v10);
    assert(extensions(VK_API_VERSION_1_1, true) == v11);
    auto v10_64 = v10;
    v10_64.insert(VK_KHR_DEVICE_GROUP_EXTENSION_NAME);
    v10_64.insert(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    auto v11_64 = v11;
    v11_64.insert(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    assert(extensions(VK_API_VERSION_1_0, false) == v10_64);
    assert(extensions(VK_API_VERSION_1_1, false) == v11_64);
    for (bool x86 : {false, true}) {
        assert(extensions(VK_API_VERSION_1_2, x86) == std::set<std::string>{VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME});
        assert(extensions(VK_API_VERSION_1_3, x86).empty());
        for (uint32_t api : {VK_API_VERSION_1_0, VK_API_VERSION_1_1, VK_API_VERSION_1_2, VK_API_VERSION_1_3}) {
            nr::DeviceFeatures features(nullptr);
            const nr::DeviceFeatureRequirements required(api, x86);
            features.enable(nr::arch::GpuArch::rdna4, required.buffer_device_address);
            auto* bda = features.find<VkPhysicalDeviceBufferDeviceAddressFeatures>(
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
            assert(bool(bda) == required.buffer_device_address);
            if (bda) assert(bda->bufferDeviceAddress == VK_TRUE);
        }
    }
    VkPhysicalDeviceVulkan12Features app{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    app.shaderBufferInt64Atomics = VK_TRUE;
    nr::DeviceFeatures copied(&app);
    copied.enable(nr::arch::GpuArch::rdna3, false);
    auto* enabled = copied.find<VkPhysicalDeviceVulkan12Features>(app.sType);
    assert(enabled->bufferDeviceAddress == VK_FALSE && enabled->shaderBufferInt64Atomics == VK_TRUE);
    assert(enabled->storageBuffer8BitAccess && enabled->shaderFloat16 && enabled->shaderInt8 && enabled->vulkanMemoryModel);
    assert(app.storageBuffer8BitAccess == VK_FALSE);
    VkPhysicalDeviceBufferDeviceAddressFeatures game_bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    game_bda.bufferDeviceAddress = VK_TRUE;
    nr::DeviceFeatures preserved(&game_bda);
    preserved.enable(nr::arch::GpuArch::rdna4, false);
    assert(preserved.find<VkPhysicalDeviceBufferDeviceAddressFeatures>(game_bda.sType)->bufferDeviceAddress == VK_TRUE);
    std::puts("PASS: API 1.0/1.1/1.2/1.3 exact extension sets; storage-class/device-group dependencies; x86/x64 BDA policy; unknown API uses physical version, explicit 1.0 preserved; clamped API; no added BDA struct below 1.2 on x86; aggregate and app-feature preservation");
}
