// Capture shim for build_d3d12_outer_aco.py: LD_PRELOAD into a Vulkan program on RADV (here outer_aco_capture under
// the amdgpu noop drm-shim, nothing reaches a GPU). With CAPTURE_DUMP=<dir> set:
// - vkCreateDevice adds VK_KHR_pipeline_binary + VK_KHR_maintenance5 (features on) when the device has them;
// - vkCreateComputePipelines captures each pipeline's binary through VK_KHR_pipeline_binary and writes, keyed by the
//   FNV-1a 64 of the SPIR-V (== the .spv file's): <dir>/<key>.blob (RADV's serialized shader), .code (its machine
//   code section) and .info (where the parts sit). The pipeline handed back is created from that captured binary.
// RADV blob: radv_shader_binary_legacy {base {type, config, info, total_size}, code_size, exec_size, ir_size,
// disasm_size, stats_size, debug_info_size} data: stats, code, ir, disasm, debug [+ optional debug trailer]. The header
// is found by consistency (total_size + the six sizes), not by one Mesa version's struct offsets.
// Build: g++ -std=c++17 -O2 -shared -fPIC capture_dump.cpp -ldl -o capture_dump.so
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {
std::mutex mu;
template <class F> F next(const char* n) { return reinterpret_cast<F>(dlsym(RTLD_NEXT, n)); }
uint64_t fnv(const void* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
std::string hx(uint64_t v) { char t[20]; std::snprintf(t, sizeof t, "%016llx", (unsigned long long)v); return t; }
std::map<VkShaderModule, uint64_t> g_mod;
bool g_pb = false;
void say(const char* what, const std::string& a, long b = 0) { std::fprintf(stderr, "[capture] %s %s %ld\n", what, a.c_str(), b); }
uint32_t rd32(const std::vector<uint8_t>& v, size_t o) { uint32_t x; std::memcpy(&x, v.data() + o, 4); return x; }

struct Layout { size_t tot_at = 0, sizes_at = 0, data_at = 0; uint32_t code, exec, ir, dis, stats, dbg, total; };
// total_size: a u32 no larger than the blob, followed (after 0 or 4 bytes of padding) by the six sizes whose sum plus
// the header's end is total_size; exactly one place in the blob may fit.
bool locate(const std::vector<uint8_t>& b, Layout& L) {
    int found = 0;
    for (size_t h = 4; h + 32 <= b.size() && h < 65536; h += 4) {
        const uint32_t total = rd32(b, h);
        if (total > b.size() || total < h + 28) continue;
        for (size_t pad : {size_t(0), size_t(4)}) {
            const size_t s = h + 4 + pad;
            if (s + 24 > b.size()) continue;
            const uint32_t code = rd32(b, s), exec = rd32(b, s + 4), ir = rd32(b, s + 8), dis = rd32(b, s + 12),
                           st = rd32(b, s + 16), dbg = rd32(b, s + 20);
            if (code == 0 || code % 4 || exec > code) continue;
            if (uint64_t(s) + 24 + st + code + ir + dis + dbg != total) continue;
            if (!found) { L = {h, s, s + 24, code, exec, ir, dis, st, dbg, total}; }
            ++found;
        }
    }
    return found == 1;
}

PFN_vkCreatePipelineBinariesKHR p_cpb; PFN_vkGetPipelineBinaryDataKHR p_gpbd; PFN_vkDestroyPipelineBinaryKHR p_dpb;
PFN_vkReleaseCapturedPipelineDataKHR p_rel;

bool capture(VkDevice d, VkComputePipelineCreateInfo ci, std::vector<VkPipelineBinaryKeyKHR>& keys,
             std::vector<std::vector<uint8_t>>& data) {
    static auto real = next<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
    VkPipelineCreateFlags2CreateInfoKHR fl{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR};
    fl.flags = VkPipelineCreateFlags2KHR(ci.flags) | VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;
    fl.pNext = ci.pNext; ci.pNext = &fl;
    VkPipeline cap{};
    if (real(d, VK_NULL_HANDLE, 1, &ci, nullptr, &cap) != VK_SUCCESS) return false;
    VkPipelineBinaryCreateInfoKHR bci{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR}; bci.pipeline = cap;
    VkPipelineBinaryHandlesInfoKHR hi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
    bool ok = p_cpb(d, &bci, nullptr, &hi) == VK_SUCCESS;
    std::vector<VkPipelineBinaryKHR> bins(hi.pipelineBinaryCount);
    hi.pPipelineBinaries = bins.data();
    ok = ok && p_cpb(d, &bci, nullptr, &hi) == VK_SUCCESS;
    VkReleaseCapturedPipelineDataInfoKHR rel{VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR}; rel.pipeline = cap;
    p_rel(d, &rel, nullptr);
    vkDestroyPipeline(d, cap, nullptr);
    keys.assign(bins.size(), {VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR});
    data.assign(bins.size(), {});
    for (size_t i = 0; ok && i < bins.size(); ++i) {
        VkPipelineBinaryDataInfoKHR gi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR}; gi.pipelineBinary = bins[i];
        size_t sz = 0;
        ok = p_gpbd(d, &gi, &keys[i], &sz, nullptr) == VK_SUCCESS;
        data[i].resize(sz);
        ok = ok && p_gpbd(d, &gi, &keys[i], &sz, data[i].data()) == VK_SUCCESS;
    }
    for (auto b : bins) if (b) p_dpb(d, b, nullptr);
    return ok;
}
VkResult import(VkDevice d, const VkComputePipelineCreateInfo& ci, std::vector<VkPipelineBinaryKeyKHR>& keys,
                std::vector<std::vector<uint8_t>>& data, VkPipeline* out) {
    static auto real = next<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
    std::vector<VkPipelineBinaryDataKHR> pd(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        keys[i].key[0] ^= 0x5a; keys[i].key[1] ^= 0xa5;   // a fresh key: never the driver's cache entry of the original
        pd[i] = {data[i].size(), data[i].data()};
    }
    VkPipelineBinaryKeysAndDataKHR kd{uint32_t(data.size()), keys.data(), pd.data()};
    VkPipelineBinaryCreateInfoKHR bci{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR}; bci.pKeysAndDataInfo = &kd;
    std::vector<VkPipelineBinaryKHR> bins(data.size());
    VkPipelineBinaryHandlesInfoKHR hi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
    hi.pipelineBinaryCount = uint32_t(bins.size()); hi.pPipelineBinaries = bins.data();
    VkResult r = p_cpb(d, &bci, nullptr, &hi);
    if (r != VK_SUCCESS) return r;
    VkPipelineBinaryInfoKHR bi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR};
    bi.binaryCount = uint32_t(bins.size()); bi.pPipelineBinaries = bins.data();
    VkComputePipelineCreateInfo c2 = ci; bi.pNext = c2.pNext; c2.pNext = &bi;
    r = real(d, VK_NULL_HANDLE, 1, &c2, nullptr, out);
    for (auto b : bins) if (b) p_dpb(d, b, nullptr);
    return r;
}
void spit(const std::string& p, const void* d, size_t n) { std::ofstream f(p, std::ios::binary); f.write((const char*)d, std::streamsize(n)); }
}  // namespace

extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* d) {
    static auto real = next<PFN_vkCreateDevice>("vkCreateDevice");
    if (!std::getenv("CAPTURE_DUMP")) return real(pd, ci, a, d);
    VkPhysicalDevicePipelineBinaryFeaturesKHR pbf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR};
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    pbf.pNext = &m5;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &pbf};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!pbf.pipelineBinaries || !m5.maintenance5) { say("device lacks pipeline_binary / maintenance5", ""); return real(pd, ci, a, d); }
    std::vector<const char*> ex(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    bool has_pb = false, has_m5 = false;
    for (auto e : ex) { has_pb |= !std::strcmp(e, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME); has_m5 |= !std::strcmp(e, VK_KHR_MAINTENANCE_5_EXTENSION_NAME); }
    if (!has_pb) ex.push_back(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME);
    if (!has_m5) ex.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    // the feature structs: ours unless the chain has them already (then switched on there)
    bool chain_pb = false, chain_m5 = false;
    for (auto* s = (VkBaseOutStructure*)ci->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR) { ((VkPhysicalDevicePipelineBinaryFeaturesKHR*)s)->pipelineBinaries = VK_TRUE; chain_pb = true; }
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR) { ((VkPhysicalDeviceMaintenance5FeaturesKHR*)s)->maintenance5 = VK_TRUE; chain_m5 = true; }
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES) { ((VkPhysicalDeviceVulkan14Features*)s)->maintenance5 = VK_TRUE; chain_m5 = true; }
    }
    VkPhysicalDevicePipelineBinaryFeaturesKHR pb2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR};
    VkPhysicalDeviceMaintenance5FeaturesKHR m52{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    pb2.pipelineBinaries = VK_TRUE; m52.maintenance5 = VK_TRUE;
    VkDeviceCreateInfo c = *ci;
    const void* head = c.pNext;
    if (!chain_m5) { m52.pNext = const_cast<void*>(head); head = &m52; }
    if (!chain_pb) { pb2.pNext = const_cast<void*>(head); head = &pb2; }
    c.pNext = head;
    c.enabledExtensionCount = uint32_t(ex.size()); c.ppEnabledExtensionNames = ex.data();
    VkResult r = real(pd, &c, a, d);
    if (r == VK_SUCCESS) {
        auto gdpa = next<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        p_cpb = (PFN_vkCreatePipelineBinariesKHR)gdpa(*d, "vkCreatePipelineBinariesKHR");
        p_gpbd = (PFN_vkGetPipelineBinaryDataKHR)gdpa(*d, "vkGetPipelineBinaryDataKHR");
        p_dpb = (PFN_vkDestroyPipelineBinaryKHR)gdpa(*d, "vkDestroyPipelineBinaryKHR");
        p_rel = (PFN_vkReleaseCapturedPipelineDataKHR)gdpa(*d, "vkReleaseCapturedPipelineDataKHR");
        g_pb = p_cpb && p_gpbd && p_dpb && p_rel;
        if (!g_pb) say("pipeline_binary entry points missing", "");
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* a, VkShaderModule* m) {
    static auto real = next<PFN_vkCreateShaderModule>("vkCreateShaderModule");
    const VkResult r = real(d, ci, a, m);
    if (r == VK_SUCCESS) { std::lock_guard<std::mutex> l(mu); g_mod[*m] = fnv(ci->pCode, ci->codeSize); }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo* ci,
                                                        const VkAllocationCallbacks* a, VkPipeline* p) {
    static auto real = next<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
    const char* dump = std::getenv("CAPTURE_DUMP");
    if (!dump) return real(d, c, n, ci, a, p);
    if (!g_pb) { say("no pipeline_binary: nothing captured", ""); return VK_ERROR_UNKNOWN; }
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t key = 0;
        { std::lock_guard<std::mutex> l(mu); key = g_mod.count(ci[i].stage.module) ? g_mod[ci[i].stage.module] : 0; }
        const std::string k = hx(key);
        std::vector<VkPipelineBinaryKeyKHR> keys; std::vector<std::vector<uint8_t>> data;
        if (!capture(d, ci[i], keys, data) || data.size() != 1) { say("capture failed", k, long(data.size())); return VK_ERROR_UNKNOWN; }
        const auto& b = data[0];
        Layout L;
        if (!locate(b, L)) { say("blob header not located", k, long(b.size())); return VK_ERROR_UNKNOWN; }
        const size_t code_at = L.data_at + L.stats;
        spit(std::string(dump) + "/" + k + ".blob", b.data(), b.size());
        spit(std::string(dump) + "/" + k + ".code", b.data() + code_at, L.code);
        std::ofstream inf(std::string(dump) + "/" + k + ".info");
        inf << "blob " << b.size() << " total_at " << L.tot_at << " sizes_at " << L.sizes_at << " data_at " << L.data_at
            << " code " << L.code << " exec " << L.exec << " ir " << L.ir << " disasm " << L.dis << " stats " << L.stats
            << " dbg " << L.dbg << " total " << L.total << " code_at " << code_at << "\n";
        const VkResult r = import(d, ci[i], keys, data, &p[i]);
        if (r != VK_SUCCESS) { say("import failed", k, long(r)); return r; }
    }
    return VK_SUCCESS;
}
}
