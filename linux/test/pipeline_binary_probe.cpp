// The T1 capture probe: on the selected Vulkan device, compile one compute SPIR-V into a
// pipeline created with capture-data and take its pipeline binary (VK_KHR_pipeline_binary)
// apart. The producer's capture stage in miniature — it proves the whole chain (RADV on the
// drm-shim phoenix device, gfx1103, hands back the compiled binary) before any record format
// exists. AMD's pipeline binaries are PAL AMDGPU ELF containers, so the ELF magic and
// e_machine are checked and printed (gfx1103 is 0x44).
//
//   pipeline_binary_probe <shader.spv> <out.bin>
//
// The device: NR_GPU selects an enumeration index; the default is the first AMD device with
// the cooperative-matrix and pipeline-binary extensions. The shader gets one storage buffer
// at binding 0 and nothing else — the probe compiles and captures, it never dispatches.
//
// Build (repo root): g++ -std=c++17 -O2 -Itoolchain/Vulkan-Headers/include \
//     linux/test/pipeline_binary_probe.cpp -o build/pipeline_binary_probe -lvulkan
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

static void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "%s: VkResult=%d\n", what, int(r));
        std::exit(1);
    }
}

static bool has_ext(VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> e(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, e.data());
    for (const auto& x : e)
        if (std::strcmp(x.extensionName, name) == 0) return true;
    return false;
}

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: %s <shader.spv> <out.bin>\n", argv[0]); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    std::vector<char> spv((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (spv.size() < 20 || std::memcmp(spv.data(), "\x03\x02\x23\x07", 4) != 0) {
        std::fprintf(stderr, "%s is not a SPIR-V module\n", argv[1]);
        return 2;
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "pb-probe";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    VkInstance instance{};
    check(vkCreateInstance(&ii, nullptr, &instance), "vkCreateInstance");

    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
    const uint32_t forced = [] { const char* g = std::getenv("NR_GPU"); return g ? uint32_t(std::atoi(g)) : ~0u; }();
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties picked{};
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        if (forced != ~0u ? i != forced
                          : (p.vendorID != 0x1002 || !has_ext(devices[i], VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME)
                             || !has_ext(devices[i], VK_KHR_PIPELINE_BINARY_EXTENSION_NAME)))
            continue;
        physical = devices[i];
        picked = p;
        break;
    }
    if (!physical) { std::fprintf(stderr, "no candidate GPU\n"); return 1; }

    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDevicePipelineBinaryFeaturesKHR pb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR};
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    q.pNext = &coop; coop.pNext = &pb; pb.pNext = &m5; m5.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(physical, &q);
    if (!coop.cooperativeMatrix || !pb.pipelineBinaries || !m5.maintenance5 || !f12.shaderFloat16) {
        std::fprintf(stderr, "a needed feature is missing (coopMat=%d pipelineBinaries=%d m5=%d f16=%d)\n",
                     int(coop.cooperativeMatrix), int(pb.pipelineBinaries), int(m5.maintenance5),
                     int(f12.shaderFloat16));
        return 1;
    }
    coop.pNext = &pb; pb.pNext = &m5; m5.pNext = &f12; f12.pNext = nullptr;
    coop.cooperativeMatrix = pb.pipelineBinaries = m5.maintenance5 = f12.shaderFloat16 = VK_TRUE;

    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> queues(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qcount, queues.data());
    uint32_t family = qcount;
    for (uint32_t i = 0; i < qcount; ++i)
        if (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    if (family == qcount) { std::fprintf(stderr, "no compute queue family\n"); return 1; }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
    const char* exts[] = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
                          VK_KHR_PIPELINE_BINARY_EXTENSION_NAME,
                          VK_KHR_MAINTENANCE_5_EXTENSION_NAME};
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &coop; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 3; di.ppEnabledExtensionNames = exts;
    VkDevice device{};
    check(vkCreateDevice(physical, &di, nullptr, &device), "vkCreateDevice");

    auto create_binaries = reinterpret_cast<PFN_vkCreatePipelineBinariesKHR>(
        vkGetInstanceProcAddr(instance, "vkCreatePipelineBinariesKHR"));
    auto get_data = reinterpret_cast<PFN_vkGetPipelineBinaryDataKHR>(
        vkGetInstanceProcAddr(instance, "vkGetPipelineBinaryDataKHR"));
    if (!create_binaries || !get_data) { std::fprintf(stderr, "pipeline binary functions absent\n"); return 1; }

    VkDescriptorSetLayoutBinding bind{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1; dl.pBindings = &bind;
    VkDescriptorSetLayout set_layout{};
    check(vkCreateDescriptorSetLayout(device, &dl, nullptr, &set_layout), "vkCreateDescriptorSetLayout");
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1; pl.pSetLayouts = &set_layout;
    VkPipelineLayout layout{};
    check(vkCreatePipelineLayout(device, &pl, nullptr, &layout), "vkCreatePipelineLayout");

    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = spv.size(); sm.pCode = reinterpret_cast<const uint32_t*>(spv.data());
    VkShaderModule module{};
    check(vkCreateShaderModule(device, &sm, nullptr, &module), "vkCreateShaderModule");
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    // VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR's bit, in the old create info's 32-bit field:
    // with the flag the compiled binary can be taken off the pipeline before it is bound.
    cp.flags = VkPipelineCreateFlags(0x80000000);
    cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = module;
    cp.stage.pName = "main";
    // wave32: the network's fragment shapes are the wave32 ones.
    cp.stage.pNext = [&] {
        static VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        req.requiredSubgroupSize = 32;
        return &req;
    }();
    cp.layout = layout;
    VkPipeline pipe{};
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &pipe), "vkCreateComputePipelines");

    VkPipelineBinaryCreateInfoKHR bc{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR};
    bc.pipeline = pipe;
    std::vector<VkPipelineBinaryKHR> bins(1);
    VkPipelineBinaryHandlesInfoKHR handles{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
    handles.pipelineBinaryCount = 1;
    handles.pPipelineBinaries = bins.data();
    check(create_binaries(device, &bc, nullptr, &handles), "vkCreatePipelineBinariesKHR");
    if (handles.pipelineBinaryCount != 1) {
        std::fprintf(stderr, "expected 1 pipeline binary, got %u\n", handles.pipelineBinaryCount);
        return 1;
    }
    VkPipelineBinaryDataInfoKHR di2{VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR};
    di2.pipelineBinary = handles.pPipelineBinaries[0];
    // This RADV dereferences the key even on the size query; a real one also lets us print it.
    VkPipelineBinaryKeyKHR key{};
    size_t bytes = 0;
    check(get_data(device, &di2, &key, &bytes, nullptr), "vkGetPipelineBinaryDataKHR(size)");
    std::vector<unsigned char> data(bytes);
    check(get_data(device, &di2, &key, &bytes, data.data()), "vkGetPipelineBinaryDataKHR(data)");

    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
    vkGetPhysicalDeviceProperties2(physical, &p2);
    std::printf("device: %s (%04x:%04x)\n", p2.properties.deviceName, p2.properties.vendorID,
                p2.properties.deviceID);
    std::printf("driver: %s / %s\n", drv.driverName, drv.driverInfo);
    std::printf("pipeline binary: %zu bytes\n", bytes);
    if (bytes >= 20 && std::memcmp(data.data(), "\x7f""ELF", 4) == 0) {
        const unsigned machine = unsigned(data[18]) | unsigned(data[19]) << 8;
        std::printf("PAL AMDGPU ELF: e_machine=0x%02x (%s)\n", machine,
                    machine == 0x44 ? "gfx1103" : machine == 0x48 ? "gfx1200" : "other");
    } else {
        std::printf("container: not an ELF (first bytes %02x %02x %02x %02x)\n", data[0], data[1],
                    data[2], data[3]);
    }
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(bytes));
    if (!out) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    std::printf("dumped: %s\n", argv[2]);
    return 0;
}
