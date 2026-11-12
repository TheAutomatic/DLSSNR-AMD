#pragma once
// The GPU's architecture, which decides the network NR runs on it. Every architecture is a value of its own and every
// caller decides by `switch` over the ones NR has a network for (has_network); anything else - an architecture NR
// knows but has no network for (RDNA1, RDNA2), or one it cannot tell - is refused by its name. No architecture is the
// default of another: RDNA4 is never "not RDNA3", RDNA3 never "not RDNA4". One more supported architecture is one more
// value here, its rows in the tables below, and its own case wherever a network is chosen.
//
// Today: RDNA4 (gfx12) runs the network as written, FP8 through the FP8 matrix instructions; RDNA3 and RDNA3.5 (gfx11)
// the RDNA3 network, whose e4m3 values travel as FP16 through the FP16 matrix instructions, as ACO's machine code only.
//
// How it is found (identify), in this order, never guessed:
// 1. AMD's driver itself, not the Vulkan loader (OptiScaler's GPU spoofing rewrites what the loader reports): its
//    compiler makes a pipeline binary of a trivial shader on a device of its own, and the ELF header names the exact
//    machine the code is for (EF_AMDGPU_MACH, kMachines). Whatever it names is the answer, known to NR or not.
// 2. Only where the driver cannot be asked (no AMD driver module in the process, a driver without pipeline binaries):
//    the GPU's exact PCI device ID in kDevices (the driver's own report where it can be had). No ID ranges (0x74xx
//    holds RDNA2's Navi 24 and Instinct parts beside RDNA3), no names, no feature guesses: an unlisted ID is unknown.
#include "nr_gpu_arch_ids.hpp"
#include "nr_driver_identity.hpp"

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace nr::arch {

// dlssnr-amd.ini [GPU] Architecture as the program reads it (nr_pe_config.cpp registers its reader when it loads; a
// program without it - the installer's int4 weight tool - is always auto): unknown = auto; `said` its value as read.
using ManualReader = GpuArch (*)(std::string* said);
inline ManualReader& manual_reader() { static ManualReader r = nullptr; return r; }
inline GpuArch manual(std::string* said = nullptr) {
    if (const ManualReader r = manual_reader()) return r(said);
    if (said) *said = "auto";
    return GpuArch::unknown;
}

// The architecture set by hand ([GPU] Architecture) over what was found: the fallback for a GPU auto cannot tell, so it
// is used as set, whatever AMD's driver or the PCI IDs say (a driver naming another architecture is warned about in
// the log, not obeyed); auto: as found.
inline Identity with_manual(Identity r) {
    std::string said;
    const GpuArch m = manual(&said);
    if (m == GpuArch::unknown) {
        if (!has_network(r.arch))
            r.how += "; dlssnr-amd.ini [GPU] Architecture (NR Control > Network) can set it by hand: rdna4 or rdna3";
        return r;
    }
    const std::string found = r.how;
    const bool other = r.mach && r.arch != m;
    r.how = std::string("set by hand: dlssnr-amd.ini [GPU] Architecture = ") + said +
            (other ? std::string(" - WARNING: AMD's driver names ") + name(r.arch) + "; machine code for " + name(m) +
                     " runs on it as set" : "") + " (auto: " + name(r.arch) + ", " + found + ")";
    r.arch = m;
    return r;
}

namespace detail {
// layout(local_size_x = 64) in; void main() {}
inline const uint32_t kProbe[] = {
    0x07230203,0x00010600,0x0008000b,0x0000000b,0x00000000,0x00020011,0x00000001,0x0006000b,0x00000001,0x4c534c47,
    0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,0x0005000f,0x00000005,0x00000004,0x6e69616d,
    0x00000000,0x00060010,0x00000004,0x00000011,0x00000040,0x00000001,0x00000001,0x00030003,0x00000002,0x000001c2,
    0x00040005,0x00000004,0x6e69616d,0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00040015,
    0x00000006,0x00000020,0x00000000,0x00040017,0x00000007,0x00000006,0x00000003,0x0004002b,0x00000006,0x00000008,
    0x00000040,0x0004002b,0x00000006,0x00000009,0x00000001,0x0006002c,0x00000007,0x0000000a,0x00000008,0x00000009,
    0x00000009,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x000100fd,0x00010038,
};

// EF_AMDGPU_MACH of an AMD GPU code object: an ELF64, little-endian, e_machine EM_AMDGPU (224) - the whole header
// checked, not one byte of it; anything else 0 (not an answer).
inline uint32_t elf_machine(const uint8_t* d, size_t bytes) {
    if (bytes < 64 || std::memcmp(d, "\x7f" "ELF", 4) != 0 || d[4] != 2 /* ELFCLASS64 */ || d[5] != 1 /* LSB */) return 0;
    const uint16_t e_machine = uint16_t(d[18] | (d[19] << 8));
    if (e_machine != 224) return 0;
    uint32_t flags = 0;
    std::memcpy(&flags, d + 48, 4);   // e_flags (ELF64)
    return flags & 0xffu;             // EF_AMDGPU_MACH
}

#ifdef _WIN32
// The machine of the code AMD's driver module `icd` compiles for the GPU with this LUID: a pipeline binary of kProbe,
// on an instance and device of its own (so no hook in front of the loader sees them). 0 when it cannot be made.
inline uint32_t compiled_machine(HMODULE icd, const uint8_t* luid) {
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(icd, "vk_icdGetInstanceProcAddr")));
    auto create_instance = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance")) : nullptr;
    if (!create_instance) return 0;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "DLSSNR-AMD architecture check";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create_instance(&ici, nullptr, &instance) != VK_SUCCESS || !instance) return 0;
#define NR_ARCH_I(f) auto p_##f = reinterpret_cast<PFN_##f>(gipa(instance, #f))
    NR_ARCH_I(vkDestroyInstance);
    NR_ARCH_I(vkEnumeratePhysicalDevices);
    NR_ARCH_I(vkGetPhysicalDeviceProperties2);
    NR_ARCH_I(vkEnumerateDeviceExtensionProperties);
    NR_ARCH_I(vkCreateDevice);
    NR_ARCH_I(vkGetDeviceProcAddr);
#undef NR_ARCH_I
    uint32_t machine = 0;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t n = 0;
    if (p_vkEnumeratePhysicalDevices && p_vkGetPhysicalDeviceProperties2 &&
        p_vkEnumeratePhysicalDevices(instance, &n, nullptr) == VK_SUCCESS && n) {
        std::vector<VkPhysicalDevice> devices(n);
        if (p_vkEnumeratePhysicalDevices(instance, &n, devices.data()) == VK_SUCCESS)
            for (VkPhysicalDevice d : devices) {
                VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
                VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
                p_vkGetPhysicalDeviceProperties2(d, &p);
                if (id.deviceLUIDValid && std::memcmp(id.deviceLUID, luid, VK_LUID_SIZE) == 0) physical = d;
            }
    }
    bool offered = false;
    if (physical && p_vkEnumerateDeviceExtensionProperties && p_vkCreateDevice && p_vkGetDeviceProcAddr) {
        uint32_t e = 0;
        p_vkEnumerateDeviceExtensionProperties(physical, nullptr, &e, nullptr);
        std::vector<VkExtensionProperties> have(e);
        p_vkEnumerateDeviceExtensionProperties(physical, nullptr, &e, have.data());
        int both = 0;
        for (const auto& x : have)
            both += !std::strcmp(x.extensionName, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) ||
                    !std::strcmp(x.extensionName, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        offered = both == 2;
    }
    VkDevice device = VK_NULL_HANDLE;
    if (offered) {
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = 0; q.queueCount = 1; q.pQueuePriorities = &priority;
        VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
        m5.maintenance5 = VK_TRUE;
        VkPhysicalDevicePipelineBinaryFeaturesKHR pb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, &m5};
        pb.pipelineBinaries = VK_TRUE;
        const char* exts[] = {VK_KHR_PIPELINE_BINARY_EXTENSION_NAME, VK_KHR_MAINTENANCE_5_EXTENSION_NAME};
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &pb};
        di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &q;
        di.enabledExtensionCount = 2; di.ppEnabledExtensionNames = exts;
        if (p_vkCreateDevice(physical, &di, nullptr, &device) != VK_SUCCESS) device = VK_NULL_HANDLE;
    }
    if (device) {
#define NR_ARCH_D(f) auto p_##f = reinterpret_cast<PFN_##f>(p_vkGetDeviceProcAddr(device, #f))
        NR_ARCH_D(vkDestroyDevice);
        NR_ARCH_D(vkCreateShaderModule);
        NR_ARCH_D(vkDestroyShaderModule);
        NR_ARCH_D(vkCreatePipelineLayout);
        NR_ARCH_D(vkDestroyPipelineLayout);
        NR_ARCH_D(vkCreateComputePipelines);
        NR_ARCH_D(vkDestroyPipeline);
        NR_ARCH_D(vkCreatePipelineBinariesKHR);
        NR_ARCH_D(vkDestroyPipelineBinaryKHR);
        NR_ARCH_D(vkGetPipelineBinaryDataKHR);
        NR_ARCH_D(vkReleaseCapturedPipelineDataKHR);
#undef NR_ARCH_D
        if (p_vkCreateShaderModule && p_vkCreatePipelineLayout && p_vkCreateComputePipelines && p_vkCreatePipelineBinariesKHR &&
            p_vkGetPipelineBinaryDataKHR) {
            VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            sm.codeSize = sizeof kProbe; sm.pCode = kProbe;
            VkShaderModule module = VK_NULL_HANDLE;
            VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            VkPipelineLayout layout = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (p_vkCreateShaderModule(device, &sm, nullptr, &module) == VK_SUCCESS &&
                p_vkCreatePipelineLayout(device, &li, nullptr, &layout) == VK_SUCCESS) {
                VkPipelineCreateFlags2CreateInfoKHR flags{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR};
                flags.flags = VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;
                VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, &flags};
                ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
                ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = module; ci.stage.pName = "main";
                ci.layout = layout;
                if (p_vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS)
                    pipeline = VK_NULL_HANDLE;
            }
            if (pipeline) {
                VkPipelineBinaryCreateInfoKHR bi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR};
                bi.pipeline = pipeline;
                VkPipelineBinaryHandlesInfoKHR hi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
                std::vector<VkPipelineBinaryKHR> binaries;
                if (p_vkCreatePipelineBinariesKHR(device, &bi, nullptr, &hi) == VK_SUCCESS && hi.pipelineBinaryCount) {
                    binaries.resize(hi.pipelineBinaryCount);
                    hi.pPipelineBinaries = binaries.data();
                    if (p_vkCreatePipelineBinariesKHR(device, &bi, nullptr, &hi) != VK_SUCCESS) binaries.clear();
                }
                for (VkPipelineBinaryKHR b : binaries) {
                    VkPipelineBinaryDataInfoKHR info{VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR};
                    info.pipelineBinary = b;
                    VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
                    size_t bytes = 0;
                    if (!machine && p_vkGetPipelineBinaryDataKHR(device, &info, &key, &bytes, nullptr) == VK_SUCCESS &&
                        bytes >= 64 && bytes <= (64u << 20)) {
                        std::vector<uint8_t> data(bytes);
                        if (p_vkGetPipelineBinaryDataKHR(device, &info, &key, &bytes, data.data()) == VK_SUCCESS)
                            machine = elf_machine(data.data(), bytes);
                    }
                    if (p_vkDestroyPipelineBinaryKHR) p_vkDestroyPipelineBinaryKHR(device, b, nullptr);
                }
                if (p_vkReleaseCapturedPipelineDataKHR) {
                    VkReleaseCapturedPipelineDataInfoKHR r{VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR};
                    r.pipeline = pipeline;
                    p_vkReleaseCapturedPipelineDataKHR(device, &r, nullptr);
                }
                if (p_vkDestroyPipeline) p_vkDestroyPipeline(device, pipeline, nullptr);
            }
            if (layout && p_vkDestroyPipelineLayout) p_vkDestroyPipelineLayout(device, layout, nullptr);
            if (module && p_vkDestroyShaderModule) p_vkDestroyShaderModule(device, module, nullptr);
        }
        if (p_vkDestroyDevice) p_vkDestroyDevice(device, nullptr);
    }
    if (p_vkDestroyInstance) p_vkDestroyInstance(instance, nullptr);
    return machine;
}
#endif
}  // namespace detail

#ifdef _WIN32
namespace detail {
// The module a Vulkan driver manifest names (its "library_path", relative to the manifest's folder unless absolute);
// a value naming a .dll itself is that module. UTF-8, JSON escapes.
inline std::wstring manifest_module(const std::wstring& manifest) {
    auto ends = [](const std::wstring& t, const wchar_t* e) {
        const size_t n = wcslen(e);
        return t.size() >= n && _wcsicmp(t.c_str() + t.size() - n, e) == 0;
    };
    if (ends(manifest, L".dll")) return manifest;
    std::string text;
    if (FILE* f = _wfopen(manifest.c_str(), L"rb")) {
        char buf[4096]; size_t got;
        while ((got = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (1u << 20)) text.append(buf, got);
        std::fclose(f);
    }
    const size_t key = text.find("\"library_path\"");
    const size_t colon = key == std::string::npos ? key : text.find(':', key);
    const size_t q0 = colon == std::string::npos ? colon : text.find('"', colon);
    if (q0 == std::string::npos) return {};
    std::string lib;
    for (size_t k = q0 + 1; k < text.size() && text[k] != '"'; ++k) {
        if (text[k] == '\\' && k + 1 < text.size()) {
            ++k;
            if (text[k] == 'u') { k += 4; continue; }   // (\uXXXX: not in a driver path; skipped)
        }
        lib += text[k];
    }
    if (lib.empty()) return {};
    std::wstring w(lib.size() + 1, L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, lib.c_str(), int(lib.size()), &w[0], int(w.size()));
    w.resize(n > 0 ? size_t(n) : 0);
    if (w.rfind(L".\\", 0) == 0 || w.rfind(L"./", 0) == 0) w = w.substr(2);
    const bool absolute = w.size() > 2 && (w[1] == L':' || (w[0] == L'\\' && w[1] == L'\\'));
    return absolute ? w : manifest.substr(0, manifest.find_last_of(L"\\/") + 1) + w;
}
// Every string of a REG_MULTI_SZ / REG_SZ value.
inline std::vector<std::wstring> reg_strings(HKEY key, const wchar_t* sub, const wchar_t* value) {
    std::vector<std::wstring> out;
    DWORD bytes = 0;
    if (RegGetValueW(key, sub, value, RRF_RT_REG_MULTI_SZ | RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        !bytes || bytes > (1u << 20))
        return out;
    std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(key, sub, value, RRF_RT_REG_MULTI_SZ | RRF_RT_REG_SZ, nullptr, buf.data(), &bytes) != ERROR_SUCCESS)
        return out;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) out.emplace_back(p);
    return out;
}
}  // namespace detail

// AMD's Vulkan driver modules that may serve the adapter with `luid`, each way on its own (OptiScaler's GPU spoofing
// hooks D3DKMT - a log 11-10: even the kernel's PCI IDs were 10de:2684 - so D3DKMT is one way, not the way):
//   d3dkmt   the adapter's VulkanDriverName (KMTQAITYPE_QUERYREGISTRY, the path translated), every entry
//   class    the display adapters' registry keys, read directly (VulkanDriverName, every entry)
//   khronos  HKLM\SOFTWARE\Khronos\Vulkan\Drivers
//   umd      beside the D3D12 driver's amdxc64.dll (the same driver package)
// Only modules named amdvlk64.dll (AMD's Vulkan driver) are kept: another vendor's driver is never loaded here.
// `steps`: what each way found, for the log.
inline std::vector<std::wstring> vulkan_driver_paths(LUID luid, std::string* steps) {
    std::vector<std::wstring> manifests;
    std::string said;
    auto note = [&](const char* way, size_t before) {
        said += std::string(said.empty() ? "" : "; ") + way + " " + std::to_string(manifests.size() - before);
    };
    {   // d3dkmt
        const size_t before = manifests.size();
        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
        // D3DKMT_OPENADAPTERFROMLUID, _QUERYADAPTERINFO, _CLOSEADAPTER, D3DDDI_QUERYREGISTRY_INFO (d3dkmthk.h)
        struct OpenFromLuid { LUID AdapterLuid; UINT hAdapter; };
        struct QueryInfo { UINT hAdapter; UINT Type; void* pPrivateDriverData; UINT PrivateDriverDataSize; };
        struct CloseAdapter { UINT hAdapter; };
        struct RegistryInfo {
            UINT QueryType;     // D3DDDI_QUERYREGISTRY_ADAPTERKEY = 1
            UINT QueryFlags;    // bit 0 TranslatePath
            WCHAR ValueName[MAX_PATH];
            ULONG ValueType;
            ULONG PhysicalAdapterIndex;
            ULONG OutputValueSize;
            UINT Status;        // 0 success, 1 buffer overflow
            union { DWORD OutputDword; UINT64 OutputQword; WCHAR OutputString[1]; BYTE OutputBinary[1]; };
        };
        static_assert(offsetof(RegistryInfo, OutputDword) == 544 && sizeof(RegistryInfo) == 552, "D3DDDI_QUERYREGISTRY_INFO");
        using Open = LONG(APIENTRY*)(OpenFromLuid*);
        using Query = LONG(APIENTRY*)(const QueryInfo*);
        using Close = LONG(APIENTRY*)(const CloseAdapter*);
        const auto open = gdi ? reinterpret_cast<Open>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"))) : nullptr;
        const auto query = gdi ? reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"))) : nullptr;
        const auto close = gdi ? reinterpret_cast<Close>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTCloseAdapter"))) : nullptr;
        OpenFromLuid o{luid, 0};
        if (open && query && close && open(&o) == 0) {
            RegistryInfo base{};
            base.QueryType = 1;
            base.QueryFlags = 1;
            base.ValueType = REG_MULTI_SZ;
            wcsncpy(base.ValueName, L"VulkanDriverName", MAX_PATH - 1);
            QueryInfo q{o.hAdapter, 48u /* KMTQAITYPE_QUERYREGISTRY */, &base, UINT(sizeof base)};
            const LONG first = query(&q);
            std::vector<uint8_t> full;
            const wchar_t* strings = nullptr;
            size_t chars = 0;
            if (first == 0 && base.Status == 1 && base.OutputValueSize && base.OutputValueSize < (1u << 20)) {
                full.assign(sizeof(RegistryInfo) + base.OutputValueSize + 4, 0);
                auto* info = reinterpret_cast<RegistryInfo*>(full.data());
                *info = base;
                q.pPrivateDriverData = info;
                q.PrivateDriverDataSize = UINT(sizeof(RegistryInfo) + base.OutputValueSize);
                if (query(&q) == 0 && info->Status == 0) { strings = info->OutputString; chars = info->OutputValueSize / sizeof(wchar_t); }
            } else if (first == 0 && base.Status == 0) {
                strings = base.OutputString; chars = base.OutputValueSize / sizeof(wchar_t);
            }
            for (size_t k = 0; strings && k < chars && strings[k];) {   // every entry of the MULTI_SZ
                const std::wstring one(strings + k);
                manifests.push_back(one);
                k += one.size() + 1;
            }
            const CloseAdapter c{o.hAdapter};
            close(&c);
        }
        note("d3dkmt", before);
    }
    {   // class: HKLM\SYSTEM\CurrentControlSet\Control\Class\{display}\NNNN
        const size_t before = manifests.size();
        HKEY cls = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}",
                          0, KEY_READ, &cls) == ERROR_SUCCESS) {
            wchar_t sub[64];
            for (DWORD i = 0;; ++i) {
                DWORD n = DWORD(sizeof sub / sizeof sub[0]);
                if (RegEnumKeyExW(cls, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                for (const std::wstring& m : detail::reg_strings(cls, sub, L"VulkanDriverName")) manifests.push_back(m);
            }
            RegCloseKey(cls);
        }
        note("class", before);
    }
    {   // khronos: the value names are the manifests
        const size_t before = manifests.size();
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\Vulkan\\Drivers", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            wchar_t name[MAX_PATH * 2];
            for (DWORD i = 0;; ++i) {
                DWORD n = DWORD(sizeof name / sizeof name[0]);
                if (RegEnumValueW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                manifests.emplace_back(name, n);
            }
            RegCloseKey(k);
        }
        note("khronos", before);
    }
    std::vector<std::wstring> paths;
    auto keep = [&](const std::wstring& path) {
        const size_t slash = path.find_last_of(L"\\/");
        const std::wstring file = slash == std::wstring::npos ? path : path.substr(slash + 1);
        if (_wcsicmp(file.c_str(), L"amdvlk64.dll") != 0 || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        for (const std::wstring& p : paths)
            if (_wcsicmp(p.c_str(), path.c_str()) == 0) return;
        paths.push_back(path);
    };
    for (const std::wstring& m : manifests) keep(detail::manifest_module(m));
    if (HMODULE umd = GetModuleHandleW(L"amdxc64.dll")) {   // umd
        wchar_t buf[MAX_PATH * 2] = {};
        const DWORD n = GetModuleFileNameW(umd, buf, DWORD(sizeof buf / sizeof buf[0]));
        if (n && n < sizeof buf / sizeof buf[0]) {
            std::wstring path(buf, n);
            keep(path.substr(0, path.find_last_of(L"\\/") + 1) + L"amdvlk64.dll");
        }
        said += "; umd loaded";
    } else {
        said += "; umd not loaded";
    }
    said += "; " + std::to_string(paths.size()) + " AMD driver module(s)";
    if (steps) *steps = said;
    return paths;
}

// AMD's Vulkan driver modules in the process (one per driver package).
inline std::vector<HMODULE> loaded_icds() {
    std::vector<HMODULE> icds;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{};
        me.dwSize = sizeof me;
        for (BOOL more = Module32FirstW(snap, &me); more; more = Module32NextW(snap, &me))
            if (_wcsicmp(me.szModule, L"amdvlk64.dll") == 0) icds.push_back(me.hModule);
        CloseHandle(snap);
    }
    return icds;
}

// The last way, only where every other found no driver that knows the adapter: the system's Vulkan loader loads the
// drivers as for any Vulkan program (an instance and its devices enumerated); the instance is kept, so the drivers stay.
inline void load_drivers_through_the_loader() {
    static std::once_flag once;
    std::call_once(once, [] {
        wchar_t sys[MAX_PATH] = {};
        const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (!n || n >= MAX_PATH) return;
        HMODULE loader = LoadLibraryW((std::wstring(sys) + L"\\vulkan-1.dll").c_str());
        if (!loader) return;
        auto create = reinterpret_cast<PFN_vkCreateInstance>(reinterpret_cast<void*>(GetProcAddress(loader, "vkCreateInstance")));
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            reinterpret_cast<void*>(GetProcAddress(loader, "vkEnumeratePhysicalDevices")));
        if (!create || !enumerate) return;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "DLSSNR-AMD architecture check";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        VkInstance instance = VK_NULL_HANDLE;
        if (create(&ici, nullptr, &instance) != VK_SUCCESS || !instance) return;
        uint32_t count = 0;
        enumerate(instance, &count, nullptr);   // (kept: its drivers stay loaded)
    });
}

// The machine AMD's own compiler makes code for on the adapter with `luid` (EF_AMDGPU_MACH; 0: it could not be
// asked, `how` says why). AMD's Vulkan driver modules already in the process first, then those the registry names
// (vulkan_driver_paths), then whatever the system's loader loads. Modules loaded here stay loaded (a driver module
// unloaded under threads of its own is not safe).
inline uint32_t driver_machine(LUID luid, std::string* how) {
    static_assert(sizeof(LUID) == VK_LUID_SIZE, "LUID size");
    uint8_t id[VK_LUID_SIZE];
    std::memcpy(id, &luid, sizeof id);
    std::vector<HMODULE> tried;
    auto ask_loaded = [&](const char* what) -> uint32_t {
        for (HMODULE icd : loaded_icds()) {
            if (std::find(tried.begin(), tried.end(), icd) != tried.end()) continue;
            tried.push_back(icd);
            if (const uint32_t mach = detail::compiled_machine(icd, id)) { *how = std::string("AMD's Vulkan driver (") + what + ")"; return mach; }
        }
        return 0;
    };
    if (const uint32_t mach = ask_loaded("loaded")) return mach;
    std::string steps;
    for (const std::wstring& path : vulkan_driver_paths(luid, &steps)) {
        HMODULE icd = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!icd || std::find(tried.begin(), tried.end(), icd) != tried.end()) continue;
        tried.push_back(icd);
        if (const uint32_t mach = detail::compiled_machine(icd, id)) {
            char narrow[MAX_PATH * 2] = {};
            WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, narrow, int(sizeof narrow) - 1, nullptr, nullptr);
            *how = std::string("AMD's Vulkan driver ") + narrow;
            return mach;
        }
    }
    load_drivers_through_the_loader();
    if (const uint32_t mach = ask_loaded("loaded by the Vulkan loader")) return mach;
    *how = "AMD's driver could not be asked: no AMD Vulkan driver module knows this adapter (" + steps + "; " +
           std::to_string(tried.size()) + " module(s) asked)";
    return 0;
}
#endif

namespace detail {
// One identification per GPU (LUID) for the process - only one the driver's compiler gave. One without it (PCI IDs
// only, or nothing) is never kept: the next caller asks again (the D3D12 passthrough asks before anything loads AMD's
// Vulkan driver; a log 11-10: OptiScaler's spoofing made even the kernel's PCI IDs 10de:2684, and the kept "unknown"
// refused the bridge on an RX 9070 too).
struct Known { uint8_t luid[VK_LUID_SIZE]; Identity id; };
inline std::mutex& known_lock() { static std::mutex m; return m; }
inline std::vector<Known>& known() { static std::vector<Known> k; return k; }
inline bool find_known(const uint8_t* luid, Identity* out) {
    for (const Known& k : known())
        if (!std::memcmp(k.luid, luid, VK_LUID_SIZE)) { *out = k.id; return true; }
    return false;
}
inline void add_known(const uint8_t* luid, const Identity& id) {
    if (!id.mach) return;
    for (const Known& k : known())
        if (!std::memcmp(k.luid, luid, VK_LUID_SIZE)) return;
    Known k{};
    std::memcpy(k.luid, luid, VK_LUID_SIZE);
    k.id = id;
    known().push_back(k);
}
}  // namespace detail

// The architecture of `physical` (a Vulkan physical device, possibly the loader's under OptiScaler's spoofing).
// `manual`: [GPU] Architecture may apply (the GPU the game runs on); false where a GPU is looked for among several - the
// hand setting names the game's GPU, not every GPU (an RDNA2 integrated GPU beside an RDNA4 must not become RDNA4).
inline Identity identify(VkPhysicalDevice physical, bool manual = true) {
    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 asked{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp};
    vkGetPhysicalDeviceProperties2(physical, &asked);
    std::lock_guard<std::mutex> guard(detail::known_lock());
    Identity r;
    if (idp.deviceLUIDValid && detail::find_known(idp.deviceLUID, &r)) return manual ? with_manual(r) : r;
    uint32_t mach = 0;
    std::string why = "the device has no LUID";
#ifdef _WIN32
    if (idp.deviceLUIDValid) {
        LUID luid;
        std::memcpy(&luid, idp.deviceLUID, sizeof luid);
        mach = driver_machine(luid, &why);
    }
#else
    why = "not on Windows";
#endif
    VkPhysicalDeviceProperties props = asked.properties;
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    const bool driver = nr::driver::amd_reported(physical, &props, &drv);
    r = decide(mach, why, driver ? "AMD's driver reports them" : "the Vulkan loader reports them", props.vendorID,
               props.deviceID);
    if (idp.deviceLUIDValid) detail::add_known(idp.deviceLUID, r);   // (the driver's answer only; the hand setting never)
    if (manual) r = with_manual(r);
    return r;
}

#ifdef _WIN32
// The architecture of the adapter with `luid` (a D3D adapter), its PCI IDs as `ids` says (null: none).
inline Identity identify(LUID luid, const char* ids, uint32_t vendor, uint32_t device) {
    uint8_t key[VK_LUID_SIZE];
    std::memcpy(key, &luid, sizeof key);
    std::lock_guard<std::mutex> guard(detail::known_lock());
    Identity r;
    if (detail::find_known(key, &r)) return with_manual(r);
    std::string why;
    const uint32_t mach = driver_machine(luid, &why);
    r = decide(mach, why, ids, vendor, device);
    detail::add_known(key, r);   // (the driver's answer only; the hand setting never)
    r = with_manual(r);
    return r;
}
#endif

// The architecture alone (and the log line).
inline GpuArch of(VkPhysicalDevice physical, std::string* how = nullptr, bool manual = true) {
    const Identity id = identify(physical, manual);
    if (how) *how = id.how;
    return id.arch;
}

}  // namespace nr::arch
