// The first ACO micro-test on the Windows driver: dispatch g_pbtest.comp's SPIR-V through the
// binary importer (template capture -> record splice -> pipeline import; nr_pipeline_binary.hpp)
// and compare the buffer bit-for-bit with the driver's own compile of the same SPIR-V. The
// kernel is integer-exact, so any mismatch is a real ABI or code-import difference, not noise.
//
//   aco_micro_test.exe <shader.spv> [--elements N] [--case <noinput|ptr|test|const>]
//
// The ACO path is taken when NR_ACO_DIR points at a bundle directory with records/<fnv64>.nrp
// (the capture side: linux/test/g_pbtest.comp through acocap + aco_manifest + pack_records);
// without it the same binary runs as the native reference. NR_DEVICE_ID / NR_GPU_UUID select
// the GPU (Context::create). Build (cross, repo root):
//
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -DNR_ARCH_RDNA3=1 \
//       -Itoolchain/Vulkan-Headers/include -Iwindows/src -Iwindows/src/core \
//       windows/test/aco_micro_test.cpp -o aco_micro_test.exe <libvulkan-1.a> \
//       -static -static-libgcc -static-libstdc++
#include "nrvk.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <vector>

namespace {

struct Buf {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped{};
};

template <class Ctx>
Buf make_buf(const Ctx& ctx, VkDeviceSize bytes) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer b{};
    NRVK_CHECK(vkCreateBuffer(ctx.device, &bi, nullptr, &b));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(ctx.device, b, &req);
    uint32_t type = ~0u;
    for (uint32_t i = 0; i < ctx.mem.memoryTypeCount; ++i)
        if (req.memoryTypeBits & 1u << i && ctx.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
            && ctx.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) { type = i; break; }
    if (type == ~0u) throw std::runtime_error("no host-visible memory type");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size; ai.memoryTypeIndex = type;
    VkDeviceMemory m{};
    NRVK_CHECK(vkAllocateMemory(ctx.device, &ai, nullptr, &m));
    NRVK_CHECK(vkBindBufferMemory(ctx.device, b, m, 0));
    void* p{};
    NRVK_CHECK(vkMapMemory(ctx.device, m, 0, bytes, 0, &p));
    return {b, m, p};
}

// Test-only fallback (NR_MICRO_PLAIN=1): a device without the network's cooperative-matrix
// requirements, so the same plumbing can run on a software implementation (lavapipe) that
// really executes dispatches — the bisect oracle when a GPU's behaviour is the question.
struct PlainCtx {
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
    uint32_t family{}; VkPhysicalDeviceMemoryProperties mem{};
    std::string gpu_name, driver_name;
    void create() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "pb-micro"; app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ii.pApplicationInfo = &app;
        NRVK_CHECK(vkCreateInstance(&ii, nullptr, &instance));
        uint32_t n = 0; NRVK_CHECK(vkEnumeratePhysicalDevices(instance, &n, nullptr));
        std::vector<VkPhysicalDevice> pd(n);
        NRVK_CHECK(vkEnumeratePhysicalDevices(instance, &n, pd.data()));
        const uint32_t forced = [] { const char* g = std::getenv("NR_GPU"); return g ? uint32_t(std::atoi(g)) : 0u; }();
        physical = pd[forced < n ? forced : 0];
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        vkGetPhysicalDeviceProperties2(physical, &p2);
        gpu_name = p2.properties.deviceName;
        driver_name = "Plain/lavapipe";
        uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qp(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, qp.data());
        for (uint32_t i = 0; i < qn; ++i) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
        float pr = 1.f; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &pr;
        // P2 fix: enable subgroupSizeControl so pipeline stage requiredSubgroupSize=32 is valid
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.subgroupSizeControl = VK_TRUE;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.pNext = &f13;
        di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
        NRVK_CHECK(vkCreateDevice(physical, &di, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    }
    void submit(const VkSubmitInfo& si, VkFence fence) { NRVK_CHECK(vkQueueSubmit(queue, 1, &si, fence)); }
};

std::array<uint8_t, 32> sha256_calc(const void* data, size_t len) {
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    auto ror = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };
    std::vector<uint8_t> msg(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
    uint64_t total_bits = static_cast<uint64_t>(len) * 8;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>((total_bits >> (i * 8)) & 0xff));

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(msg[chunk + i * 4]) << 24) | (uint32_t(msg[chunk + i * 4 + 1]) << 16)
                 | (uint32_t(msg[chunk + i * 4 + 2]) << 8)  | uint32_t(msg[chunk + i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hp = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = hp + S1 + ch + k[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            hp = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hp;
    }
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = uint8_t(h[i] >> 24);
        out[i * 4 + 1] = uint8_t(h[i] >> 16);
        out[i * 4 + 2] = uint8_t(h[i] >> 8);
        out[i * 4 + 3] = uint8_t(h[i]);
    }
    return out;
}

std::string sha256_hex(const void* data, size_t len) {
    auto b = sha256_calc(data, len);
    char buf[65];
    for (int i = 0; i < 32; ++i) std::snprintf(buf + i * 2, 3, "%02x", b[i]);
    return std::string(buf);
}

void print_key_info(const char* label, const VkPipelineBinaryKeyKHR& key) {
    std::printf("%s (keySize: %u): ", label, key.keySize);
    for (uint32_t i = 0; i < key.keySize && i < 32; ++i)
        std::printf("%02x", key.key[i]);
    if (key.keySize > 32) std::printf("...");
    std::printf("\n");
}

}   // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <shader.spv> [--elements N] [--case <noinput|ptr|test|const>]\n", argv[0]);
        return 2;
    }
    uint32_t elements = 65536;
    std::string explicit_case;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--elements") && i + 1 < argc) elements = uint32_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--case") && i + 1 < argc) explicit_case = argv[++i];
    }
    try {
        std::ifstream in(argv[1], std::ios::binary);
        if (!in) throw std::runtime_error(std::string("cannot read ") + argv[1]);
        std::vector<char> spv((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (spv.size() % 4 || std::memcmp(spv.data(), "\x03\x02\x23\x07", 4) != 0)
            throw std::runtime_error("not a SPIR-V module");
        std::vector<uint32_t> code(spv.size() / 4);
        std::memcpy(code.data(), spv.data(), spv.size());

        // Determine case name from explicit flag or filename
        std::string spv_name = argv[1];
        std::string base_name = spv_name.substr(spv_name.find_last_of("/\\") + 1);
        std::string case_name = explicit_case;
        if (case_name.empty()) {
            if (base_name.find("noinput") != std::string::npos) case_name = "noinput";
            else if (base_name.find("const") != std::string::npos) case_name = "const";
            else if (base_name.find("scratch") != std::string::npos) case_name = "scratch";
            else if (base_name.find("ptr") != std::string::npos) case_name = "ptr";
            else if (base_name.find("test") != std::string::npos) case_name = "test";
            else case_name = "ptr"; // default network-shape kernel
        }
        if (case_name == "noinput" && (elements % 64 != 0)) {
            elements = (elements + 63) / 64 * 64;
            std::printf("note: rounded elements to %u (multiple of 64 for noinput)\n", elements);
        }

        std::string spv_sha = sha256_hex(spv.data(), spv.size());
        std::printf("input: %s (size %zu bytes, sha256: %s)\n", argv[1], spv.size(), spv_sha.c_str());
        std::printf("oracle case: %s\n", case_name.c_str());

        auto go = [&](auto& ctx) {
            ctx.create();   // NR_ACO_DIR or NR_MICRO_* activates the pipeline binary import path
            const bool aco = nr::binary::enabled(ctx.device);
            std::printf("mode: %s\n", aco ? "ACO import (VK_KHR_pipeline_binary)" : "native compile (reference)");
            std::printf("device: %s\ndriver: %s\n", ctx.gpu_name.c_str(), ctx.driver_name.c_str());

            const uint32_t mul = 0x2545F491u, add = 0x9E3779B9u, seed = 0x1234567u;
            struct Push { uint32_t w[32]; } push{};
            push.w[0] = mul; push.w[1] = add; push.w[2] = elements; push.w[3] = seed;
            for (uint32_t k = 4; k < 32; ++k) push.w[k] = 0x9E3779B1u * k + 0x13572468u;

            auto run = [&](auto& ctx2, std::vector<uint32_t>& out) {
                Buf bin_ = make_buf(ctx, elements * 4), bout = make_buf(ctx, elements * 4);
                std::vector<uint32_t> in_(elements);
                for (uint32_t i = 0; i < elements; ++i) in_[i] = i * 2654435761u + 12345u;
                std::memcpy(bin_.mapped, in_.data(), in_.size() * 4);

                // Stage A: initialize output buffer with non-zero sentinel so lack of execution is detectable
                const uint32_t sentinel = 0xbaadf00du;
                std::fill_n(static_cast<uint32_t*>(bout.mapped), elements, sentinel);

                VkDescriptorSetLayoutBinding lb[2]{};
                lb[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
                lb[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
                VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
                dl.bindingCount = 2; dl.pBindings = lb;
                VkDescriptorSetLayout set_layout{};
                NRVK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &dl, nullptr, &set_layout));
                VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push};
                VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                pli.setLayoutCount = 1; pli.pSetLayouts = &set_layout;
                pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pc;
                VkPipelineLayout layout{};
                NRVK_CHECK(vkCreatePipelineLayout(ctx.device, &pli, nullptr, &layout));

                VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                sm.codeSize = spv.size(); sm.pCode = code.data();
                VkShaderModule module{};
                NRVK_CHECK(vkCreateShaderModule(ctx.device, &sm, nullptr, &module));
                VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req{
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
                req.requiredSubgroupSize = 32;
                ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &req};
                ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                ci.stage.module = module;
                ci.stage.pName = "main";
                ci.layout = layout;

                VkPipeline pipeline{};
                if (std::getenv("NR_MICRO_SELFIMPORT")) {
                    nr::binary::Functions f(ctx.device);
                    VkPipelineCreateFlags2CreateInfoKHR flags{
                        VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR};
                    flags.flags = VkPipelineCreateFlags2KHR(ci.flags) | VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;
                    flags.pNext = ci.pNext;
                    auto capture_ci = ci;
                    capture_ci.pNext = &flags;
                    NRVK_CHECK(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &capture_ci, nullptr, &pipeline));
                    nr::binary::Binaries original(f);
                    VkPipelineBinaryCreateInfoKHR bi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR};
                    bi.pipeline = pipeline;
                    std::vector<VkPipelineBinaryKHR> bins(1);
                    VkPipelineBinaryHandlesInfoKHR hi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
                    hi.pipelineBinaryCount = 1;
                    hi.pPipelineBinaries = bins.data();
                    NRVK_CHECK(f.create(ctx.device, &bi, nullptr, &hi));
                    VkPipelineBinaryDataInfoKHR di{VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR};
                    di.pipelineBinary = hi.pPipelineBinaries[0];
                    VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
                    size_t bytes = 0;
                    NRVK_CHECK(f.data(ctx.device, &di, &key, &bytes, nullptr));
                    std::vector<unsigned char> blob(bytes);
                    NRVK_CHECK(f.data(ctx.device, &di, &key, &bytes, blob.data()));
                    vkDestroyPipeline(ctx.device, pipeline, nullptr);

                    // Stage B: Save raw blob + raw key if requested
                    if (const char* dump_pfx = std::getenv("NR_MICRO_DUMP_ORIGINAL")) {
                        std::string bin_path = std::string(dump_pfx) + ".bin";
                        std::string key_path = std::string(dump_pfx) + ".key";
                        std::ofstream bf(bin_path, std::ios::binary);
                        bf.write(reinterpret_cast<const char*>(blob.data()), blob.size());
                        std::ofstream kf(key_path, std::ios::binary);
                        kf.write(reinterpret_cast<const char*>(&key.keySize), 4);
                        kf.write(reinterpret_cast<const char*>(key.key), key.keySize);
                        std::printf("dumped original: %zu bytes -> %s, keySize %u -> %s\n",
                                    blob.size(), bin_path.c_str(), key.keySize, key_path.c_str());
                    }
                    print_key_info("captured key", key);

                    // Stage B: ELF rebuild probe (no splice, purely Elf(blob).build())
                    if (std::getenv("NR_MICRO_ELF_REBUILD")) {
                        size_t old_sz = blob.size();
                        blob = nr::pal::Elf(blob).build();
                        std::printf("ELF rebuild: %zu -> %zu bytes (no splice)\n", old_sz, blob.size());
                    }

                    pipeline = nr::binary::import_pipeline(f, ctx.device, ci, key, blob);
                    std::printf("self-import: %zu bytes round-tripped\n", blob.size());
                } else if (const char* import_file = std::getenv("NR_MICRO_IMPORT_FILE")) {
                    std::ifstream bf(import_file, std::ios::binary);
                    if (!bf) throw std::runtime_error(std::string("cannot read ") + import_file);
                    std::vector<char> raw((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
                    std::vector<unsigned char> blob(raw.begin(), raw.end());
                    nr::binary::Functions f(ctx.device);

                    // Stage B: Raw key control - read key from file if available
                    VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
                    std::string key_file;
                    if (const char* kf = std::getenv("NR_MICRO_IMPORT_KEY")) {
                        key_file = kf;
                    } else {
                        std::string cand = import_file;
                        size_t dot = cand.find_last_of('.');
                        if (dot != std::string::npos) cand = cand.substr(0, dot);
                        cand += ".key";
                        std::ifstream test_k(cand, std::ios::binary);
                        if (test_k) key_file = cand;
                    }

                    if (!key_file.empty()) {
                        std::ifstream kf(key_file, std::ios::binary);
                        if (!kf) throw std::runtime_error(std::string("cannot read key file ") + key_file);
                        uint32_t ks = 0;
                        kf.read(reinterpret_cast<char*>(&ks), 4);
                        if (ks > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR)
                            throw std::runtime_error("invalid keySize in " + key_file);
                        key.keySize = ks;
                        kf.read(reinterpret_cast<char*>(key.key), ks);
                        std::printf("loaded raw key (%u bytes) from %s\n", ks, key_file.c_str());
                    } else {
                        // Key strategy: SHA256 or FNV64
                        const char* strat = std::getenv("NR_MICRO_KEY_STRATEGY");
                        if (strat && !std::strcmp(strat, "sha256")) {
                            auto h = sha256_calc(blob.data(), blob.size());
                            key.keySize = 32;
                            std::memcpy(key.key, h.data(), 32);
                            std::printf("using content SHA256 key (32 bytes)\n");
                        } else {
                            uint64_t h = 0xcbf29ce484222325ull;
                            for (unsigned char c : blob) { h ^= c; h *= 0x100000001b3ull; }
                            key.keySize = 8;
                            std::memcpy(key.key, &h, 8);
                            std::printf("using FNV64 content key (8 bytes)\n");
                        }
                    }
                    print_key_info("import key", key);

                    // Stage B: ELF rebuild probe
                    if (std::getenv("NR_MICRO_ELF_REBUILD")) {
                        size_t old_sz = blob.size();
                        blob = nr::pal::Elf(blob).build();
                        std::printf("ELF rebuild: %zu -> %zu bytes (no splice)\n", old_sz, blob.size());
                    }

                    pipeline = nr::binary::import_pipeline(f, ctx.device, ci, key, blob);
                    std::printf("blob import: %zu bytes from %s (sha256: %s)\n",
                                blob.size(), import_file, sha256_hex(blob.data(), blob.size()).c_str());
                } else {
                    NRVK_CHECK(nr::binary::create_or_default(ctx.device, VK_NULL_HANDLE, ci, argv[1],
                                                             code.data(), code.size() * 4, &pipeline,
                                                             sizeof push));
                }

                VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
                VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
                VkDescriptorPool pool{};
                NRVK_CHECK(vkCreateDescriptorPool(ctx.device, &dpi, nullptr, &pool));
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &set_layout;
                VkDescriptorSet set{};
                NRVK_CHECK(vkAllocateDescriptorSets(ctx.device, &ai, &set));
                VkDescriptorBufferInfo wb[2]{{bin_.buffer, 0, VK_WHOLE_SIZE}, {bout.buffer, 0, VK_WHOLE_SIZE}};
                VkWriteDescriptorSet w[2]{};
                w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[0].dstSet = set; w[0].dstBinding = 0; w[0].descriptorCount = 1;
                w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &wb[0];
                w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[1].dstSet = set; w[1].dstBinding = 1; w[1].descriptorCount = 1;
                w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &wb[1];
                vkUpdateDescriptorSets(ctx.device, 2, w, 0, nullptr);

                VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                cpi.queueFamilyIndex = ctx.family;
                VkCommandPool cpool{};
                NRVK_CHECK(vkCreateCommandPool(ctx.device, &cpi, nullptr, &cpool));
                VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                cbi.commandPool = cpool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
                VkCommandBuffer cb{};
                NRVK_CHECK(vkAllocateCommandBuffers(ctx.device, &cbi, &cb));
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                NRVK_CHECK(vkBeginCommandBuffer(cb, &bi));
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
                vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);

                if (std::getenv("NR_NO_DISPATCH")) {
                    vkCmdFillBuffer(cb, bout.buffer, 0, VK_WHOLE_SIZE, 0xfeedfaceu);
                    VkBufferMemoryBarrier bar{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    bar.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bar.buffer = bout.buffer;
                    bar.offset = 0;
                    bar.size = VK_WHOLE_SIZE;
                    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                         0, 0, nullptr, 1, &bar, 0, nullptr);
                } else {
                    vkCmdDispatch(cb, (elements + 63) / 64, 1, 1);
                    VkBufferMemoryBarrier bar{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                    bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    bar.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bar.buffer = bout.buffer;
                    bar.offset = 0;
                    bar.size = VK_WHOLE_SIZE;
                    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                         0, 0, nullptr, 1, &bar, 0, nullptr);
                }
                NRVK_CHECK(vkEndCommandBuffer(cb));

                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                VkFence fence{};
                NRVK_CHECK(vkCreateFence(ctx.device, &fi, nullptr, &fence));
                ctx.submit(si, fence);
                NRVK_CHECK(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, ~0ull));

                out.resize(elements);
                std::memcpy(out.data(), bout.mapped, elements * 4);

                vkDestroyFence(ctx.device, fence, nullptr);
                vkDestroyCommandPool(ctx.device, cpool, nullptr);
                vkDestroyDescriptorPool(ctx.device, pool, nullptr);
                vkDestroyPipeline(ctx.device, pipeline, nullptr);
                vkDestroyShaderModule(ctx.device, module, nullptr);
                vkDestroyPipelineLayout(ctx.device, layout, nullptr);
                vkDestroyDescriptorSetLayout(ctx.device, set_layout, nullptr);
                vkUnmapMemory(ctx.device, bin_.memory);
                vkUnmapMemory(ctx.device, bout.memory);
                vkFreeMemory(ctx.device, bin_.memory, nullptr);
                vkFreeMemory(ctx.device, bout.memory, nullptr);
                vkDestroyBuffer(ctx.device, bin_.buffer, nullptr);
                vkDestroyBuffer(ctx.device, bout.buffer, nullptr);
            };

            std::vector<uint32_t> ref, got;
            run(ctx, got);

            // CPU reference according to case
            ref.resize(elements);
            if (std::getenv("NR_NO_DISPATCH")) {
                for (uint32_t i = 0; i < elements; ++i) ref[i] = 0xfeedfaceu;
            } else if (case_name == "noinput") {
                for (uint32_t i = 0; i < elements; ++i) ref[i] = 0xdeadbe30u + i;
            } else if (case_name == "const") {
                for (uint32_t i = 0; i < elements; ++i) ref[i] = 0xdeadbe10u + i;
            } else if (case_name == "scratch") {
                for (uint32_t idx = 0; idx < elements; ++idx) {
                    uint32_t val = (idx * 2654435761u + 12345u) ^ idx;
                    uint32_t v[300];
                    for (uint32_t i = 0; i < 300; ++i) v[i] = val + i * 0x9e3779b9u;
                    for (int iter = 0; iter < 4; ++iter) {
                        for (uint32_t i = 0; i < 300; ++i) {
                            uint32_t prev = (i == 0 ? 299 : i - 1);
                            v[i] = (v[i] ^ (v[prev] >> 3)) + 0x12345u;
                        }
                    }
                    uint32_t res = 0;
                    for (uint32_t i = 0; i < 300; ++i) res ^= v[i];
                    ref[idx] = res;
                }
            } else if (case_name == "test") {
                for (uint32_t i = 0; i < elements; ++i) {
                    const uint32_t x = (i * 2654435761u + 12345u) ^ (push.w[3] + i);
                    ref[i] = x * push.w[0] + push.w[1] + i;
                }
            } else { // "ptr"
                for (uint32_t i = 0; i < elements; ++i) {
                    uint32_t x = (i * 2654435761u + 12345u) ^ (push.w[3] + i);
                    for (uint32_t k = 4; k < 32; ++k) x ^= push.w[k] + (i << (k & 7));
                    ref[i] = x * push.w[0] + push.w[1] + i;
                }
            }

            const uint32_t sentinel = 0xbaadf00du;
            uint64_t bad = 0, first = ~0ull;
            for (uint32_t i = 0; i < elements; ++i)
                if (got[i] != ref[i]) { ++bad; if (first == ~0ull) first = i; }
            std::printf("elements: %u, mismatches: %llu", elements,
                        static_cast<unsigned long long>(bad));
            if (bad) {
                std::printf(" (first at %llu: got %08x want %08x)",
                            static_cast<unsigned long long>(first), got[first], ref[first]);
                if (got[first] == sentinel)
                    std::printf(" [OUTPUT UNTOUCHED - sentinel preserved!]");
                else if (got[first] == 0)
                    std::printf(" [OUTPUT ALL ZEROS]");
            }
            std::printf("\nresult: %s\n", bad ? "FAIL" : "PASS");
            return bad ? 1 : 0;
        };

        if (std::getenv("NR_MICRO_PLAIN")) { PlainCtx ctx; return go(ctx); }
        nrvk::Context ctx;
        return go(ctx);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
