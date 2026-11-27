// Compile a single compute SPIR-V under RADV with ACO capture metadata for D3D12 outer pipelines.
// Reflects set=0 descriptor bindings directly from SPIR-V so the layout matches each shader's exact bindings.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void vk_check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed (" + std::to_string(int(r)) + ")");
    }
}

struct TypeInfo {
    uint32_t op = 0;
    uint32_t storage = 0;      // OpTypePointer storage class
    uint32_t sub = 0;          // pointee / array element / SampledImage image type
    uint32_t length_id = 0;    // OpTypeArray length constant id
    uint32_t sampled = 0;      // OpTypeImage Sampled operand (1=sampled, 2=storage)
    bool buffer_block = false; // OpDecorate BufferBlock (pre-SPV 1.3 SSBO)
    bool block = false;
};

struct VarInfo {
    uint32_t ptr_type = 0;
    uint32_t storage = 0;
    uint32_t set = UINT32_MAX;
    uint32_t binding = UINT32_MAX;
};

struct Reflected {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    uint32_t push_bytes = 0;
    bool has_spec0 = false;
};

Reflected reflect_spv(const std::vector<uint32_t>& words, uint32_t push_override) {
    if (words.size() < 5 || words[0] != 0x07230203u) {
        throw std::runtime_error("invalid SPIR-V header");
    }
    std::map<uint32_t, TypeInfo> types;
    std::map<uint32_t, VarInfo> vars;
    std::map<uint32_t, uint32_t> const_u32;
    std::map<uint32_t, uint32_t> dec_set, dec_bind;
    std::map<uint32_t, uint32_t> struct_decs;                   // bit0=Block, bit1=BufferBlock
    bool has_spec0 = false;

    size_t p = 5;
    while (p < words.size()) {
        const uint32_t wc = words[p] >> 16;
        const uint32_t op = words[p] & 0xffffu;
        if (wc == 0 || p + wc > words.size()) throw std::runtime_error("truncated SPIR-V instruction");
        const uint32_t* a = words.data() + p + 1;
        switch (op) {
            case 43: // OpConstant
            case 50: // OpSpecConstant
                if (wc >= 4) const_u32[a[1]] = a[2];
                break;
            case 71: // OpDecorate
                if (wc >= 3) {
                    if (a[1] == 34 && wc >= 4) dec_set[a[0]] = a[2];          // DescriptorSet
                    else if (a[1] == 33 && wc >= 4) dec_bind[a[0]] = a[2];    // Binding
                    else if (a[1] == 2) struct_decs[a[0]] |= 1u;              // Block
                    else if (a[1] == 3) struct_decs[a[0]] |= 2u;              // BufferBlock
                    else if (a[1] == 1 && wc >= 4 && a[2] == 0u) has_spec0 = true;// SpecId 0
                }
                break;
            case 25: // OpTypeImage
                if (wc >= 9) {
                    TypeInfo& t = types[a[0]];
                    t.op = op;
                    t.sampled = a[6]; // Sampled operand (1=sampled, 2=storage)
                }
                break;
            case 26: // OpTypeSampler
                if (wc >= 2) types[a[0]].op = op;
                break;
            case 27: // OpTypeSampledImage
                if (wc >= 3) {
                    TypeInfo& t = types[a[0]];
                    t.op = op;
                    t.sub = a[1];
                }
                break;
            case 28: // OpTypeArray
                if (wc >= 4) {
                    TypeInfo& t = types[a[0]];
                    t.op = op;
                    t.sub = a[1];
                    t.length_id = a[2];
                }
                break;
            case 30: // OpTypeStruct
                if (wc >= 2) types[a[0]].op = op;
                break;
            case 32: // OpTypePointer
                if (wc >= 4) {
                    TypeInfo& t = types[a[0]];
                    t.op = op;
                    t.storage = a[1];
                    t.sub = a[2];
                }
                break;
            case 59: // OpVariable
                if (wc >= 4) {
                    VarInfo& v = vars[a[1]];
                    v.ptr_type = a[0];
                    v.storage = a[2];
                }
                break;
            default: break;
        }
        p += wc;
    }

    Reflected out;
    out.has_spec0 = has_spec0;
    bool has_push = false;
    for (const auto& [vid, v] : vars) {
        if (v.storage == 9u) { // StorageClassPushConstant
            has_push = true;
            continue;
        }
        if (!dec_bind.count(vid)) continue;
        const uint32_t set_num = dec_set.count(vid) ? dec_set[vid] : 0u;
        if (set_num != 0u) throw std::runtime_error("unexpected descriptor set != 0");
        const uint32_t binding = dec_bind[vid];

        const TypeInfo& pt = types.at(v.ptr_type);
        uint32_t base_id = pt.sub;
        uint32_t count = 1;
        while (types.count(base_id) && types[base_id].op == 28u) {
            const TypeInfo& arr = types[base_id];
            if (!const_u32.count(arr.length_id)) throw std::runtime_error("non-constant descriptor array length");
            count *= const_u32[arr.length_id];
            base_id = arr.sub;
        }
        const TypeInfo& bt = types.at(base_id);
        VkDescriptorType dtype = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        if (bt.op == 27u) {
            dtype = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        } else if (bt.op == 26u) {
            dtype = VK_DESCRIPTOR_TYPE_SAMPLER;
        } else if (bt.op == 25u) {
            dtype = (bt.sampled == 2u) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        } else if (bt.op == 30u) {
            const uint32_t flags = struct_decs[base_id];
            if (v.storage == 12u || (v.storage == 2u && (flags & 2u))) dtype = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            else dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        } else {
            throw std::runtime_error("unsupported descriptor base Op " + std::to_string(bt.op));
        }
        VkDescriptorSetLayoutBinding b{};
        b.binding = binding;
        b.descriptorType = dtype;
        b.descriptorCount = count;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        out.bindings.push_back(b);
    }
    std::sort(out.bindings.begin(), out.bindings.end(),
              [](const auto& a, const auto& b) { return a.binding < b.binding; });
    out.push_bytes = has_push ? (push_override ? push_override : 256u) : 0u;
    return out;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::fprintf(stderr, "usage: outer_aco_capture <shader.spv> [--push bytes] [--spec0 value]\n");
            return 2;
        }
        const char* spv_path = argv[1];
        uint32_t push_override = 0;
        bool use_spec0 = false;
        uint32_t spec0_val = 0;
        bool mip_linear = false;
        for (int i = 2; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--push") && i + 1 < argc) push_override = uint32_t(std::strtoul(argv[++i], nullptr, 0));
            else if (!std::strcmp(argv[i], "--spec0") && i + 1 < argc) {
                use_spec0 = true;
                spec0_val = uint32_t(std::strtoul(argv[++i], nullptr, 0));
            } else if (!std::strcmp(argv[i], "--mip-linear")) {
                mip_linear = true;
            }
        }

        std::ifstream f(spv_path, std::ios::binary);
        if (!f) throw std::runtime_error(std::string("cannot open ") + spv_path);
        std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), {});
        if (raw.size() % 4 != 0) throw std::runtime_error("SPIR-V size not a multiple of 4");
        std::vector<uint32_t> words(raw.size() / 4);
        std::memcpy(words.data(), raw.data(), raw.size());
        const Reflected refl = reflect_spv(words, push_override);

        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "dlssnr_outer_capture";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        VkInstance inst = VK_NULL_HANDLE;
        vk_check(vkCreateInstance(&ici, nullptr, &inst), "vkCreateInstance");

        uint32_t ndev = 0;
        vk_check(vkEnumeratePhysicalDevices(inst, &ndev, nullptr), "vkEnumeratePhysicalDevices");
        if (!ndev) throw std::runtime_error("no Vulkan physical device");
        std::vector<VkPhysicalDevice> pds(ndev);
        vk_check(vkEnumeratePhysicalDevices(inst, &ndev, pds.data()), "vkEnumeratePhysicalDevices");
        VkPhysicalDevice phys = pds[0];

        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13};
        VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11};
        vkGetPhysicalDeviceFeatures2(phys, &f2);

        uint32_t nqf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(nqf);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qfs.data());
        uint32_t qfam = UINT32_MAX;
        for (uint32_t i = 0; i < nqf; ++i) {
            if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfam = i; break; }
        }
        if (qfam == UINT32_MAX) throw std::runtime_error("no compute queue family");

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qfam;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;

        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        VkDevice dev = VK_NULL_HANDLE;
        vk_check(vkCreateDevice(phys, &dci, nullptr, &dev), "vkCreateDevice");

        // Immutable linear-clamp sampler matching D3D12 MIN_MAG_LINEAR_MIP_POINT + CLAMP (or MIP_LINEAR with --mip-linear).
        VkSamplerCreateInfo smpi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        smpi.magFilter = VK_FILTER_LINEAR;
        smpi.minFilter = VK_FILTER_LINEAR;
        smpi.mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        smpi.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        smpi.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        smpi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        smpi.maxLod = mip_linear ? VK_LOD_CLAMP_NONE : 0.0f;
        VkSampler linear_sampler = VK_NULL_HANDLE;
        vk_check(vkCreateSampler(dev, &smpi, nullptr, &linear_sampler), "vkCreateSampler");

        std::vector<VkDescriptorSetLayoutBinding> bindings = refl.bindings;
        for (auto& b : bindings) {
            if (b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) {
                b.pImmutableSamplers = &linear_sampler;
            }
        }
        std::vector<VkDescriptorBindingFlags> bflags(bindings.size(), VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
        VkDescriptorSetLayoutBindingFlagsCreateInfo dfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        dfi.bindingCount = uint32_t(bflags.size());
        dfi.pBindingFlags = bflags.data();
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            bindings.empty() ? nullptr : &dfi};
        dli.bindingCount = uint32_t(bindings.size());
        dli.pBindings = bindings.data();
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        vk_check(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &dsl), "vkCreateDescriptorSetLayout");

        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, refl.push_bytes};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &dsl;
        if (refl.push_bytes) {
            plci.pushConstantRangeCount = 1;
            plci.pPushConstantRanges = &pcr;
        }
        VkPipelineLayout pl = VK_NULL_HANDLE;
        vk_check(vkCreatePipelineLayout(dev, &plci, nullptr, &pl), "vkCreatePipelineLayout");

        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = raw.size();
        smci.pCode = words.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        vk_check(vkCreateShaderModule(dev, &smci, nullptr, &mod), "vkCreateShaderModule");

        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        rss.requiredSubgroupSize = 32;

        VkSpecializationMapEntry spec_entry{0u, 0u, sizeof(uint32_t)};
        VkSpecializationInfo spec_info{1u, &spec_entry, sizeof(uint32_t), &spec0_val};

        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = mod;
        cpi.stage.pName = "main";
        cpi.stage.pNext = &rss;
        cpi.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
        if (use_spec0 || refl.has_spec0) cpi.stage.pSpecializationInfo = &spec_info;
        cpi.layout = pl;
        VkPipeline pipe = VK_NULL_HANDLE;
        vk_check(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe), "vkCreateComputePipelines");

        vkDestroyPipeline(dev, pipe, nullptr);
        vkDestroyShaderModule(dev, mod, nullptr);
        vkDestroyPipelineLayout(dev, pl, nullptr);
        vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
        vkDestroySampler(dev, linear_sampler, nullptr);
        vkDestroyDevice(dev, nullptr);
        vkDestroyInstance(inst, nullptr);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "outer_aco_capture error: %s\n", e.what());
        return 1;
    }
}
