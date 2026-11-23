#include "../src/core/nr_device_features.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <set>

static std::set<std::string> extensions(uint32_t api, bool x86) {
    const nr::DeviceFeatureRequirements required(api, x86);
    assert(required.known_api == (api >= VK_API_VERSION_1_0));
    std::set<std::string> result(required.extensions.begin(), required.extensions.end());
    assert(result.size() == required.extensions.size());
    assert(required.buffer_device_address == (required.known_api && (!x86 || api >= VK_API_VERSION_1_2)));
    return result;
}

static VkInstance fake_instance(std::uintptr_t id) {
    return reinterpret_cast<VkInstance>(id);
}

static VkPhysicalDevice fake_physical(std::uintptr_t id) {
    return reinterpret_cast<VkPhysicalDevice>(id);
}

int main() {
    // Unknown API (e.g. missed vkCreateInstance): never guess physical/1.4 and never blindly force 1.0 extensions.
    assert(nr::effective_device_api(0, VK_API_VERSION_1_4, nullptr) == 0);
    VkDeviceCreateInfo empty_dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    assert(nr::effective_device_api(0, VK_API_VERSION_1_4, &empty_dci) == 0);
    for (bool x86 : {false, true}) {
        const nr::DeviceFeatureRequirements unknown_req(0, x86);
        assert(!unknown_req.known_api);
        assert(!unknown_req.buffer_device_address);
        assert(unknown_req.extensions.empty());
    }

    // Provable lower bounds inferred from VkDeviceCreateInfo when instance creation was missed (ReShade late-load).
    VkPhysicalDeviceVulkan11Features dci_v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkDeviceCreateInfo dci_with_v11{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &dci_v11};
    assert(nr::effective_device_api(0, VK_API_VERSION_1_4, &dci_with_v11) == VK_API_VERSION_1_1);

    VkPhysicalDeviceVulkan12Features dci_v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkDeviceCreateInfo dci_with_v12{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &dci_v12};
    assert(nr::effective_device_api(0, VK_API_VERSION_1_4, &dci_with_v12) == VK_API_VERSION_1_2);

    VkPhysicalDeviceVulkan13Features dci_v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkDeviceCreateInfo dci_with_v13{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &dci_v13};
    assert(nr::effective_device_api(0, VK_API_VERSION_1_4, &dci_with_v13) == VK_API_VERSION_1_3);

    VkPhysicalDeviceVulkan14Features dci_v14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkDeviceCreateInfo dci_with_v14{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &dci_v14};
    assert(nr::effective_device_api(0, VK_API_VERSION_1_4, &dci_with_v14) == VK_API_VERSION_1_4);
    assert(nr::effective_device_api(0, VK_API_VERSION_1_3, &dci_with_v14) == VK_API_VERSION_1_3);

    VkPhysicalDeviceSubgroupSizeControlFeatures dci_sg{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
    VkDeviceCreateInfo dci_with_sg{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &dci_sg};
    assert(nr::effective_device_api(0, VK_API_VERSION_1_4, &dci_with_sg) == VK_API_VERSION_1_1);

    const char* sg_ext = VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME;
    VkDeviceCreateInfo dci_with_sg_ext{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci_with_sg_ext.enabledExtensionCount = 1;
    dci_with_sg_ext.ppEnabledExtensionNames = &sg_ext;
    assert(nr::effective_device_api(0, VK_API_VERSION_1_4, &dci_with_sg_ext) == VK_API_VERSION_1_1);

    VkBaseInStructure cycle_dci{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr};
    cycle_dci.pNext = &cycle_dci;
    VkDeviceCreateInfo dci_cycle{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &cycle_dci};
    assert(nr::effective_device_api(0, VK_API_VERSION_1_4, &dci_cycle) == 0);

    // Explicit known instance versions & clamping.
    assert(nr::effective_device_api(VK_API_VERSION_1_0, VK_API_VERSION_1_3) == VK_API_VERSION_1_0);
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

    // Features2 / pipeline robustness prerequisites across unknown, 1.0 (with/without KHR_get_physical_device_properties2), and 1.1..1.4.
    for (uint32_t api : {0u, VK_API_VERSION_1_0, VK_API_VERSION_1_1, VK_API_VERSION_1_2, VK_API_VERSION_1_3, VK_API_VERSION_1_4}) {
        for (bool has_khr_props2 : {false, true}) {
            const bool expect_can_query = api >= VK_API_VERSION_1_1 || (api == VK_API_VERSION_1_0 && has_khr_props2);
            assert(nr::can_query_physical_device_features2(api, has_khr_props2) == expect_can_query);
            for (bool offered : {false, true}) {
                for (bool feature : {false, true}) {
                    const nr::PipelineRobustnessRequirements required(api, has_khr_props2, offered, feature);
                    assert(required.can_query == expect_can_query);
                    assert(required.enabled == (expect_can_query && feature && (offered || api >= VK_API_VERSION_1_4)));
                    assert(required.extension == (required.enabled && api < VK_API_VERSION_1_4));
                    nr::DeviceFeatures chain(nullptr);
                    if (required.enabled) chain.enable_pipeline_robustness();
                    assert(nr::pipeline_robustness_enabled(chain.head) == required.enabled);
                }
            }
        }
    }

    assert(!nr::pipeline_robustness_enabled(nullptr));
    VkPhysicalDevicePipelineRobustnessFeaturesEXT app_robustness{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
    nr::DeviceFeatures robustness(&app_robustness);
    robustness.enable_pipeline_robustness();
    robustness.enable_pipeline_robustness();
    assert(nr::pipeline_robustness_enabled(robustness.head));
    assert(robustness.head->pNext == nullptr);
    assert(app_robustness.pipelineRobustness == VK_FALSE);
    // A failed augmentation retries this original, unmodified chain.
    assert(!nr::pipeline_robustness_enabled(&app_robustness));
    app_robustness.pipelineRobustness = VK_TRUE;
    nr::DeviceFeatures game_robustness(&app_robustness);
    assert(nr::pipeline_robustness_enabled(game_robustness.head));
    VkPhysicalDeviceVulkan14Features app14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    app14.shaderSubgroupRotate = VK_TRUE;
    nr::DeviceFeatures core_robustness(&app14);
    core_robustness.enable_pipeline_robustness();
    assert(nr::pipeline_robustness_enabled(core_robustness.head));
    assert(core_robustness.head->pNext == nullptr);
    assert(core_robustness.find<VkPhysicalDeviceVulkan14Features>(app14.sType)->shaderSubgroupRotate == VK_TRUE);
    assert(app14.pipelineRobustness == VK_FALSE);
    app14.pipelineRobustness = VK_TRUE;
    assert(nr::pipeline_robustness_enabled(&app14));
    VkPhysicalDeviceFeatures2 root{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &app14};
    assert(nr::pipeline_robustness_enabled(&root));
    VkBaseInStructure cycle{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr};
    cycle.pNext = &cycle;
    assert(!nr::pipeline_robustness_enabled(&cycle));

    // InstanceApiTracker: ASI normal tracking, explicit 1.0 + KHR_get_physical_device_properties2,
    // ReShade late-load, vkDestroyInstance cleanup & handle-reuse regression.
    nr::InstanceApiTracker tracker;
    const VkInstance inst1 = fake_instance(0x1001);
    const VkInstance inst2 = fake_instance(0x1002);
    const VkPhysicalDevice phys1 = fake_physical(0x2001);
    const VkPhysicalDevice phys2 = fake_physical(0x2002);

    // 1. Normal ASI tracking: 1.1 instance on 1.4 GPU.
    VkApplicationInfo app_1_1{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_1_1.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici_1_1{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici_1_1.pApplicationInfo = &app_1_1;
    tracker.note_instance(inst1, &ici_1_1);
    const VkPhysicalDevice list_with_null[2] = {VK_NULL_HANDLE, phys1};
    tracker.note_physical_devices(inst1, 2, list_with_null);
    assert(tracker.lookup(VK_NULL_HANDLE).requested_api == 0);
    const nr::InstanceApiInfo seen_1_1 = tracker.lookup(phys1);
    assert(seen_1_1.instance == inst1);
    assert(seen_1_1.requested_api == VK_API_VERSION_1_1);
    assert(!seen_1_1.has_get_physical_device_properties2);
    const uint32_t eff_1_1 = nr::effective_device_api(seen_1_1.requested_api, VK_API_VERSION_1_4, &empty_dci);
    assert(eff_1_1 == VK_API_VERSION_1_1);
    assert(!nr::DeviceFeatureRequirements(eff_1_1, true).buffer_device_address);
    assert(nr::DeviceFeatureRequirements(eff_1_1, false).buffer_device_address);
    assert(nr::PipelineRobustnessRequirements(eff_1_1, seen_1_1.has_get_physical_device_properties2, true, true).extension);

    // 2. Explicit 1.0 (nullptr pApplicationInfo and apiVersion == 0), with and without KHR props2 ext.
    VkInstanceCreateInfo ici_null_app{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    tracker.note_instance(inst2, &ici_null_app);
    tracker.note_physical_devices(inst2, 1, &phys2);
    const nr::InstanceApiInfo seen_1_0_no_ext = tracker.lookup(phys2);
    assert(seen_1_0_no_ext.requested_api == VK_API_VERSION_1_0);
    assert(!seen_1_0_no_ext.has_get_physical_device_properties2);
    assert(!nr::PipelineRobustnessRequirements(
        seen_1_0_no_ext.requested_api, seen_1_0_no_ext.has_get_physical_device_properties2, true, true).enabled);

    const char* khr_props2_ext = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkApplicationInfo app_zero{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_zero.apiVersion = 0;
    VkInstanceCreateInfo ici_1_0_ext{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici_1_0_ext.pApplicationInfo = &app_zero;
    ici_1_0_ext.enabledExtensionCount = 1;
    ici_1_0_ext.ppEnabledExtensionNames = &khr_props2_ext;
    tracker.note_instance(inst2, &ici_1_0_ext);
    const nr::InstanceApiInfo seen_1_0_ext = tracker.lookup(phys2);
    assert(seen_1_0_ext.requested_api == VK_API_VERSION_1_0);
    assert(seen_1_0_ext.has_get_physical_device_properties2);
    const nr::PipelineRobustnessRequirements pr_1_0_ext(
        seen_1_0_ext.requested_api, seen_1_0_ext.has_get_physical_device_properties2, true, true);
    assert(pr_1_0_ext.can_query && pr_1_0_ext.enabled && pr_1_0_ext.extension);

    // 3. ReShade late-load (only vkEnumeratePhysicalDevices seen): never guesses 1.4/1.0, uses pNext lower bound only.
    const VkInstance late_inst = fake_instance(0x1003);
    const VkPhysicalDevice late_phys = fake_physical(0x2003);
    tracker.note_physical_devices(late_inst, 1, &late_phys);
    const nr::InstanceApiInfo late_info = tracker.lookup(late_phys);
    assert(late_info.instance == late_inst);
    assert(late_info.requested_api == 0);
    assert(!late_info.has_get_physical_device_properties2);
    assert(nr::effective_device_api(late_info.requested_api, VK_API_VERSION_1_4, &empty_dci) == 0);
    const uint32_t late_inferred_12 = nr::effective_device_api(late_info.requested_api, VK_API_VERSION_1_4, &dci_with_v12);
    assert(late_inferred_12 == VK_API_VERSION_1_2);
    assert(nr::DeviceFeatureRequirements(late_inferred_12, true).buffer_device_address);
    assert(nr::PipelineRobustnessRequirements(
        late_inferred_12, late_info.has_get_physical_device_properties2, true, true).extension);

    // 4. vkDestroyInstance cleanup + handle reuse regression: reused inst1/phys1 must not inherit stale 1.4 version.
    VkApplicationInfo app_1_4{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_1_4.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo ici_1_4{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici_1_4.pApplicationInfo = &app_1_4;
    tracker.note_instance(inst1, &ici_1_4);
    tracker.note_physical_devices(inst1, 1, &phys1);
    assert(tracker.lookup(phys1).requested_api == VK_API_VERSION_1_4);
    tracker.forget_instance(inst1);
    assert(tracker.lookup(phys1).instance == VK_NULL_HANDLE);
    assert(tracker.lookup(phys1).requested_api == 0);
    // Simulate handle reuse when vkCreateInstance is later missed (late-load): must remain unknown (0), not stale 1.4.
    tracker.note_physical_devices(inst1, 1, &phys1);
    assert(tracker.lookup(phys1).instance == inst1);
    assert(tracker.lookup(phys1).requested_api == 0);
    assert(tracker.lookup(phys2).requested_api == VK_API_VERSION_1_0);

    std::puts("PASS: unknown API safety & pNext lower-bound inference; explicit 1.0/1.1/1.2/1.3/1.4 extension/BDA/features2 policy");
    std::puts("PASS: ASI/ReShade instance tracking, vkDestroyInstance cleanup, and handle-reuse safety; pipeline robustness chain preservation");
}
