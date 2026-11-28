#include "nr_pe_vkdevice.hpp"

#include "nr_pe_bridge.hpp"
#include "nr_pe_interop.hpp"
#include "nr_pe_log.hpp"
#include "nr_device_features.hpp"
#include "nr_pipeline_binary.hpp"
#include "nr_gpu_arch.hpp"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nr::pe::vkdevice {
namespace {

using nr::pe::log;

// OpenRR's own Vulkan instances (openrr-amd.asi beside this module; application name "openrr...") and what they make:
// not the game's - nothing recorded, no features added. Nothing changes while OpenRR makes none.
bool is_internal_vk_app(const VkInstanceCreateInfo* info) {
    if (!info || !info->pApplicationInfo || !info->pApplicationInfo->pApplicationName) return false;
    const char* n = info->pApplicationInfo->pApplicationName;
    return _strnicmp(n, "openrr", 6) == 0 || _strnicmp(n, "dlssnr-amd", 10) == 0;
}

std::mutex lock;
DeviceHandles seen{};
nr::InstanceApiTracker instance_tracker;
std::unordered_set<VkInstance> internal_instances;
std::unordered_map<VkPhysicalDevice, VkInstance> internal_physicals;
std::unordered_set<VkDevice> internal_devices;

nr::InstanceApiInfo lookup_instance_api(VkPhysicalDevice physical) {
    std::lock_guard<std::mutex> guard(lock);
    return instance_tracker.lookup(physical);
}

struct Queue { VkDevice device; uint32_t family; VkQueue queue; };
std::vector<Queue> queues;
// The queue families the game asked for in its last vkCreateDevice.
std::vector<uint32_t> requested_families;
// Devices created with the network's features added (network_features_added).
std::vector<VkDevice> augmented_devices;
std::map<VkDevice, bool> device_robustness;
void (*device_callback)(VkDevice) = nullptr;

using PFN_CreateInstance = VkResult(VKAPI_PTR*)(const VkInstanceCreateInfo*,
                                                const VkAllocationCallbacks*, VkInstance*);
using PFN_DestroyInstance = void(VKAPI_PTR*)(VkInstance, const VkAllocationCallbacks*);
using PFN_CreateDevice = VkResult(VKAPI_PTR*)(VkPhysicalDevice, const VkDeviceCreateInfo*,
                                              const VkAllocationCallbacks*, VkDevice*);
using PFN_DestroyDevice = void(VKAPI_PTR*)(VkDevice, const VkAllocationCallbacks*);
using PFN_GetDeviceQueue = void(VKAPI_PTR*)(VkDevice, uint32_t, uint32_t, VkQueue*);
// The Vulkan 1.1 form. A game that asks for its queues this way left every one
// of them unrecorded, and the present queue's family unknown - which is exactly
// what stood the fallback down in a real game.
using PFN_GetDeviceQueue2 = void(VKAPI_PTR*)(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);
using PFN_EnumeratePhysicalDevices = VkResult(VKAPI_PTR*)(VkInstance, uint32_t*, VkPhysicalDevice*);

PFN_CreateInstance real_create_instance = nullptr;
PFN_DestroyInstance real_destroy_instance = nullptr;
PFN_CreateDevice real_create_device = nullptr;
PFN_DestroyDevice real_destroy_device = nullptr;
PFN_GetDeviceQueue real_get_device_queue = nullptr;
PFN_GetDeviceQueue2 real_get_device_queue2 = nullptr;
PFN_EnumeratePhysicalDevices real_enumerate_physical_devices = nullptr;

// What install() hooked, for uninstall().
std::vector<void*> hooked_targets;

VkResult VKAPI_PTR hooked_create_instance(const VkInstanceCreateInfo* info,
                                          const VkAllocationCallbacks* allocator,
                                          VkInstance* out) {
    const VkResult r = real_create_instance(info, allocator, out);
    if (r == VK_SUCCESS && out && *out && !nr::pe::bridge::creating_device()) {
        if (is_internal_vk_app(info)) {
            std::lock_guard<std::mutex> guard(lock);
            internal_instances.insert(*out);
        } else {
            {
                std::lock_guard<std::mutex> guard(lock);
                internal_instances.erase(*out);
            }
            note_instance(*out, info);
            std::lock_guard<std::mutex> guard(lock);
            seen.instance = *out;
        }
    }
    return r;
}

void VKAPI_PTR hooked_destroy_instance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    if (instance) {
        bool was_internal = false;
        {
            std::lock_guard<std::mutex> guard(lock);
            was_internal = internal_instances.erase(instance) != 0;
            if (was_internal) {
                for (auto it = internal_physicals.begin(); it != internal_physicals.end();) {
                    if (it->second == instance) it = internal_physicals.erase(it);
                    else ++it;
                }
            }
        }
        if (!was_internal && !nr::pe::bridge::creating_device()) forget_instance(instance);
    }
    real_destroy_instance(instance, allocator);
}

// A hook that is installed while the game's vkCreateInstance is still on the
// stack - which is when ReShade's Vulkan layer loads its add-ons - never sees
// that instance. Every app enumerates physical devices right after, and the
// instance is the first argument.
VkResult VKAPI_PTR hooked_enumerate_physical_devices(VkInstance instance, uint32_t* count,
                                                     VkPhysicalDevice* out) {
    const VkResult r = real_enumerate_physical_devices(instance, count, out);
    if (instance && !nr::pe::bridge::creating_device()) {
        {
            std::lock_guard<std::mutex> guard(lock);
            if (internal_instances.count(instance) != 0) {   // OpenRR's: its physical devices kept apart
                if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && out && count)
                    for (uint32_t i = 0; i < *count; ++i)
                        if (out[i] != VK_NULL_HANDLE) internal_physicals[out[i]] = instance;
                return r;
            }
        }
        if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && out && count)
            note_physical_devices(instance, *count, out);
        std::lock_guard<std::mutex> guard(lock);
        if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && out && count && !internal_physicals.empty())
            for (uint32_t i = 0; i < *count; ++i) internal_physicals.erase(out[i]);   // (handles OpenRR's had)
        if (!seen.instance) log("[nr] Vulkan instance learned from vkEnumeratePhysicalDevices");
        seen.instance = instance;
    }
    return r;
}

// On Windows the game's device is DXVK's or vkd3d-proton's, created with the features they need.
// The network also needs cooperative matrices, FP8 and the explicit workgroup memory layout;
// vkd3d-proton enables the first two, DXVK none. They are added on the way in when the GPU has all
// three extensions. Anything unexpected in the game's chain (a structure the size table does not
// know) leaves the call exactly as the game made it, and so does a device that then fails to create.
// FP8 is RDNA4's network's alone (nr_gpu_arch.hpp: RDNA3's travels as FP16).
const char* const kNetworkExtensions[] = {
    VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
    VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME,
};

struct Augmented {
    nr::arch::GpuArch arch = nr::arch::GpuArch::unknown;   // the network the features were added for
    std::unique_ptr<nr::DeviceFeatures> features;
    std::vector<const char*> extensions;
    VkDeviceCreateInfo info{};
    nr::binary::Features binary;
};

// Straight from vulkan-1.dll: the DLL's own vk* entry points are resolved later than this runs.
PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions = nullptr;

bool augment(VkPhysicalDevice physical, const VkDeviceCreateInfo* info, Augmented* a) {
    // The DLL's vk* entry points before the GPU's architecture is asked through them (both callers: the .asi's
    // create_with_network and the ReShade add-on's device watch): the game is making its device through vulkan-1.dll,
    // so the loader is in, possibly for the first time (a Vulkan game's first device).
    nr::pe::vulkan_entry_points();
    uint32_t n = 0;
    if (!enumerate_extensions || enumerate_extensions(physical, nullptr, &n, nullptr) != VK_SUCCESS) {
        log("[nr] device features left as the game asked: cannot list the GPU's extensions");
        return false;
    }
    std::vector<VkExtensionProperties> have(n);
    if (enumerate_extensions(physical, nullptr, &n, have.data()) != VK_SUCCESS) return false;
    have.resize(n);
    auto listed = [](const char* name, const char* const* list, size_t count) {
        for (size_t i = 0; i < count; ++i)
            if (list[i] && std::strcmp(list[i], name) == 0) return true;
        return false;
    };
    const nr::arch::Identity id = nr::arch::identify(physical);
    log("[nr] the game's GPU is %s", nr::arch::describe(id).c_str());
    std::vector<const char*> network(std::begin(kNetworkExtensions), std::end(kNetworkExtensions));
    switch (id.arch) {
    case nr::arch::GpuArch::rdna4:
        network.push_back(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
        break;
    case nr::arch::GpuArch::rdna3: {
        std::string why;
        if (!nr::binary::use_rdna3(&why)) {
            log("[nr] device features left as the game asked: RDNA3 GPUs run NR as ACO's machine code only, and %s",
                why.c_str());
            return false;
        }
        break;
    }
    case nr::arch::GpuArch::unknown: case nr::arch::GpuArch::rdna1: case nr::arch::GpuArch::rdna2:
        log("[nr] device features left as the game asked: NR has no network for this GPU (%s)",
            nr::arch::name(id.arch));
        return false;
    }
    a->arch = id.arch;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    const nr::InstanceApiInfo inst_info = lookup_instance_api(physical);
    uint32_t api = nr::effective_device_api(inst_info.requested_api, properties.apiVersion, info);
    if (!api) {
        // Neither the instance's version nor a hint in the device's create info: the GPU's own version, as before the
        // version was tracked (leaving the features out would leave NR unable to run on this device).
        api = properties.apiVersion;
        static std::once_flag once_unknown;
        std::call_once(once_unknown, [] {
            log("[nr] instance API version unknown and not inferable from the device's create info; using the GPU's API version for device features");
        });
    } else if (!inst_info.requested_api) {
        static std::once_flag once_inferred;
        std::call_once(once_inferred, [api] {
            log("[nr] instance API version unknown; using inferred lower bound %u.%u from device create info",
                VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api));
        });
    }
    // The tested 32-bit route uses storage buffers, not device addresses.
    const nr::DeviceFeatureRequirements requirements(api, sizeof(void*) == 4);
    network.insert(network.end(), requirements.extensions.begin(), requirements.extensions.end());
    for (const char* e : network) {
        bool found = false;
        for (const auto& h : have) found = found || std::strcmp(h.extensionName, e) == 0;
        if (!found) {
            log("[nr] device features left as the game asked: the GPU has no %s", e);
            return false;
        }
    }
    a->extensions.assign(info->ppEnabledExtensionNames,
                         info->ppEnabledExtensionNames + info->enabledExtensionCount);
    for (const char* e : network)
        if (!listed(e, a->extensions.data(), a->extensions.size())) a->extensions.push_back(e);
    a->features = std::make_unique<nr::DeviceFeatures>(info->pNext);
    a->features->enable(a->arch, requirements.buffer_device_address);
    bool robustness_ext = false;
    for (const auto& h : have)
        robustness_ext = robustness_ext || std::strcmp(h.extensionName, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME) == 0;
    const bool can_query_features2 = nr::can_query_physical_device_features2(
        api, inst_info.has_get_physical_device_properties2);
    VkPhysicalDevicePipelineRobustnessFeaturesEXT prf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
    if (can_query_features2 && (robustness_ext || api >= VK_API_VERSION_1_4)) {
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &prf};
        if (api >= VK_API_VERSION_1_1) {
            vkGetPhysicalDeviceFeatures2(physical, &f2);
        } else if (inst_info.instance) {
            const auto get_features2_khr = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2KHR>(
                vkGetInstanceProcAddr(inst_info.instance, "vkGetPhysicalDeviceFeatures2KHR"));
            if (get_features2_khr) get_features2_khr(physical, &f2);
        }
    }
    const nr::PipelineRobustnessRequirements robustness(
        api, inst_info.has_get_physical_device_properties2, robustness_ext, prf.pipelineRobustness != 0);
    if (robustness.enabled) {
        a->features->enable_pipeline_robustness();
        if (robustness.extension && !listed(VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME,
                                           a->extensions.data(), a->extensions.size()))
            a->extensions.push_back(VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
    }
    auto* w = a->features->get<VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR);
    w->workgroupMemoryExplicitLayout = w->workgroupMemoryExplicitLayout8BitAccess =
        w->workgroupMemoryExplicitLayout16BitAccess = VK_TRUE;
    a->info = *info;
    a->info.pNext = a->features->head;
    a->info.enabledExtensionCount = uint32_t(a->extensions.size());
    a->info.ppEnabledExtensionNames = a->extensions.data();
    a->binary.prepare(physical, a->info, a->extensions);
    return true;
}

VkResult VKAPI_PTR hooked_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                        const VkAllocationCallbacks* allocator, VkDevice* out) {
    // The bridge's own device (nr_pe_bridge.hpp) is not the game's: passed straight through, unrecorded; nor OpenRR's.
    if (nr::pe::bridge::creating_device()) return real_create_device(physical, info, allocator, out);
    // The physical device is known before the call; ReShade's Vulkan layer
    // fires its device events from inside this very call, so anything that
    // runs then must already find it.
    bool internal = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        internal = internal_physicals.count(physical) != 0;
        if (!internal) {
            seen.physical = physical;
            requested_families.clear();
            if (info)
                for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i)
                    requested_families.push_back(info->pQueueCreateInfos[i].queueFamilyIndex);
        }
    }
    if (internal) {
        const VkResult r = real_create_device(physical, info, allocator, out);
        if (r == VK_SUCCESS && out && *out) {
            std::lock_guard<std::mutex> guard(lock);
            internal_devices.insert(*out);
        }
        return r;
    }
    Augmented augmented;
    bool added = false;
    if (info) {
        try {
            added = augment(physical, info, &augmented);
        } catch (const std::exception& e) {
            log("[nr] device features left as the game asked: %s", e.what());
        }
    }
    VkResult r = real_create_device(physical, added ? &augmented.info : info, allocator, out);
    if (added && r != VK_SUCCESS) {
        log("[nr] device creation with the network's features failed (%d); retrying as the game asked", int(r));
        added = false;
        r = real_create_device(physical, info, allocator, out);
    }
    if (added && r == VK_SUCCESS && augmented.binary.active) nr::binary::mark(*out);
    if (added && r == VK_SUCCESS) log("[nr] network features added to the game's device");
    if (r == VK_SUCCESS && out && *out) {
        {
            std::lock_guard<std::mutex> guard(lock);
            internal_devices.erase(*out);
            seen.physical = physical;
            seen.device = *out;
            if (added) augmented_devices.push_back(*out);
            device_robustness[*out] = nr::pipeline_robustness_enabled(added ? augmented.info.pNext : info ? info->pNext : nullptr);
        }
        log("[nr] Vulkan device created by the game");
        if (device_callback) device_callback(*out);
    }
    return r;
}

void VKAPI_PTR hooked_destroy_device(VkDevice device, const VkAllocationCallbacks* allocator) {
    if (device) {
        std::lock_guard<std::mutex> guard(lock);
        internal_devices.erase(device);
    }
    real_destroy_device(device, allocator);
}

void VKAPI_PTR hooked_get_device_queue(VkDevice device, uint32_t family, uint32_t index,
                                       VkQueue* out) {
    real_get_device_queue(device, family, index, out);
    if (!out || !*out) return;
    if (nr::pe::bridge::creating_device() || nr::pe::bridge::is_own_device(device)) return;
    std::lock_guard<std::mutex> guard(lock);
    if (internal_devices.count(device) != 0) return;
    for (const auto& q : queues)
        if (q.queue == *out) return;
    queues.push_back({device, family, *out});
}

void VKAPI_PTR hooked_get_device_queue2(VkDevice device, const VkDeviceQueueInfo2* info,
                                        VkQueue* out) {
    real_get_device_queue2(device, info, out);
    if (!out || !*out || !info) return;
    if (nr::pe::bridge::creating_device() || nr::pe::bridge::is_own_device(device)) return;
    std::lock_guard<std::mutex> guard(lock);
    if (internal_devices.count(device) != 0) return;
    for (const auto& q : queues)
        if (q.queue == *out) return;
    queues.push_back({device, info->queueFamilyIndex, *out});
}

}  // namespace

void note_instance(VkInstance instance, const VkInstanceCreateInfo* info) {
    if (!instance) return;
    std::lock_guard<std::mutex> guard(lock);
    instance_tracker.note_instance(instance, info);
}

void note_physical_devices(VkInstance instance, uint32_t count, const VkPhysicalDevice* physical) {
    if (!instance || !physical) return;
    std::lock_guard<std::mutex> guard(lock);
    instance_tracker.note_physical_devices(instance, count, physical);
}

void forget_instance(VkInstance instance) {
    if (!instance) return;
    std::lock_guard<std::mutex> guard(lock);
    instance_tracker.forget_instance(instance);
    if (seen.instance == instance) seen.instance = VK_NULL_HANDLE;
}

VkResult create_with_network(PFN_vkCreateDevice create, VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                             const VkAllocationCallbacks* allocator, VkDevice* out, bool* added) {
    if (!enumerate_extensions) {
        if (HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll"))
            enumerate_extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                GetProcAddress(vulkan, "vkEnumerateDeviceExtensionProperties"));
    }
    Augmented augmented;
    *added = false;
    if (info) {
        try {
            *added = augment(physical, info, &augmented);
        } catch (const std::exception& e) {
            log("[nr] device features left as the game asked: %s", e.what());
        }
    }
    VkResult r = create(physical, *added ? &augmented.info : info, allocator, out);
    if (*added && r != VK_SUCCESS) {
        log("[nr] device creation with the network's features failed (%d); retrying as the game asked", int(r));
        *added = false;
        r = create(physical, info, allocator, out);
    }
    if (*added && r == VK_SUCCESS && augmented.binary.active) nr::binary::mark(*out);
    if (*added && r == VK_SUCCESS) {
        log("[nr] network features added to the game's device");
        std::lock_guard<std::mutex> guard(lock);
        augmented_devices.push_back(*out);
    }
    if (r == VK_SUCCESS && out && *out) {
        std::lock_guard<std::mutex> guard(lock);
        device_robustness[*out] = nr::pipeline_robustness_enabled(*added ? augmented.info.pNext : info ? info->pNext : nullptr);
    }
    return r;
}

bool install() {
    HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
    if (!vulkan) vulkan = LoadLibraryW(L"vulkan-1.dll");
    if (!vulkan) return false;   // not a Vulkan game; nothing to watch
    enumerate_extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        GetProcAddress(vulkan, "vkEnumerateDeviceExtensionProperties"));
    struct { const char* name; void* detour; void** original; } entries[] = {
        {"vkCreateInstance", reinterpret_cast<void*>(&hooked_create_instance),
         reinterpret_cast<void**>(&real_create_instance)},
        {"vkDestroyInstance", reinterpret_cast<void*>(&hooked_destroy_instance),
         reinterpret_cast<void**>(&real_destroy_instance)},
        {"vkCreateDevice", reinterpret_cast<void*>(&hooked_create_device),
         reinterpret_cast<void**>(&real_create_device)},
        {"vkDestroyDevice", reinterpret_cast<void*>(&hooked_destroy_device),
         reinterpret_cast<void**>(&real_destroy_device)},
        {"vkGetDeviceQueue", reinterpret_cast<void*>(&hooked_get_device_queue),
         reinterpret_cast<void**>(&real_get_device_queue)},
        {"vkEnumeratePhysicalDevices", reinterpret_cast<void*>(&hooked_enumerate_physical_devices),
         reinterpret_cast<void**>(&real_enumerate_physical_devices)},
        {"vkGetDeviceQueue2", reinterpret_cast<void*>(&hooked_get_device_queue2),
         reinterpret_cast<void**>(&real_get_device_queue2)},
    };
    bool all = true;
    for (const auto& entry : entries) {
        void* target = reinterpret_cast<void*>(GetProcAddress(vulkan, entry.name));
        if (!target || MH_CreateHook(target, entry.detour, entry.original) != MH_OK) {
            all = false;
            continue;
        }
        hooked_targets.push_back(target);
        if (MH_EnableHook(target) != MH_OK) all = false;
    }
    log("[nr] Vulkan device watch %s", all ? "installed" : "incomplete");
    return all;
}

void uninstall() {
    if (hooked_targets.empty()) return;
    for (void* target : hooked_targets) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }
    hooked_targets.clear();
    log("[nr] Vulkan device watch removed");
}

DeviceHandles handles() {
    std::lock_guard<std::mutex> guard(lock);
    return seen;
}

void note_handles(VkInstance instance, VkPhysicalDevice physical, VkDevice device) {
    std::lock_guard<std::mutex> guard(lock);
    if (instance) seen.instance = instance;
    if (physical) seen.physical = physical;
    if (device) seen.device = device;
}

void note_device(VkInstance instance, VkPhysicalDevice physical, VkDevice device, const std::vector<uint32_t>& families) {
    std::lock_guard<std::mutex> guard(lock);
    if (instance) seen.instance = instance;
    seen.physical = physical;
    seen.device = device;
    requested_families = families;
}

namespace {
VkPhysicalDevice g_gpu = VK_NULL_HANDLE;   // note_gpu's (guarded by `lock`)
}
void note_gpu(VkPhysicalDevice physical) {
    std::lock_guard<std::mutex> guard(lock);
    if (physical) g_gpu = physical;
}
std::string driver_name() {
    static std::string name;
    static VkPhysicalDevice named = VK_NULL_HANDLE;
    std::lock_guard<std::mutex> guard(lock);
    const VkPhysicalDevice p = g_gpu ? g_gpu : seen.physical;
    if (p && p != named) {
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
        vkGetPhysicalDeviceProperties2(p, &props);
        name = driver.driverInfo[0] ? driver.driverInfo : driver.driverName;
        // AMD's Windows driver gives its version alone ("26.10.1"): with its name, which says what the number is
        if (driver.driverInfo[0] >= '0' && driver.driverInfo[0] <= '9' && driver.driverName[0])
            name = std::string(driver.driverName) + " " + driver.driverInfo;
        named = p;
    }
    return name;
}
static std::string g_d3d12_gpu_name;
void note_d3d12_gpu_name(const std::string& n) {
    std::lock_guard<std::mutex> guard(lock);
    if (!n.empty()) g_d3d12_gpu_name = n;
}
std::string gpu_name() {
    static std::string name;
    static VkPhysicalDevice named = VK_NULL_HANDLE;
    std::lock_guard<std::mutex> guard(lock);
    const VkPhysicalDevice p = g_gpu ? g_gpu : seen.physical;
    if (p && p != named) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(p, &props);
        name = props.deviceName;
        named = p;
    }
    return !name.empty() ? name : g_d3d12_gpu_name;
}

void forget_device(VkDevice device) {
    if (!device) return;
    std::lock_guard<std::mutex> guard(lock);
    internal_devices.erase(device);
    queues.erase(std::remove_if(queues.begin(), queues.end(), [device](const Queue& q) { return q.device == device; }),
                 queues.end());
    if (seen.device == device) { seen.device = VK_NULL_HANDLE; requested_families.clear(); }
    augmented_devices.erase(std::remove(augmented_devices.begin(), augmented_devices.end(), device), augmented_devices.end());
    device_robustness.erase(device);
}

namespace {
std::atomic<bool> g_queue_locks{false};
std::mutex g_queue_locks_map;
std::map<VkQueue, std::unique_ptr<std::recursive_mutex>> g_queue_mutexes;
std::recursive_mutex* queue_mutex(VkQueue queue) {
    std::lock_guard<std::mutex> guard(g_queue_locks_map);
    auto& m = g_queue_mutexes[queue];
    if (!m) m = std::make_unique<std::recursive_mutex>();
    return m.get();
}
}  // namespace

void enable_queue_locks() { g_queue_locks = true; }
bool queue_locks_enabled() { return g_queue_locks.load(); }
void lock_queue(VkQueue queue) { if (g_queue_locks.load() && queue) queue_mutex(queue)->lock(); }
void unlock_queue(VkQueue queue) { if (g_queue_locks.load() && queue) queue_mutex(queue)->unlock(); }
bool lock_queue_held(VkQueue queue) {
    if (!g_queue_locks.load() || !queue) return false;
    queue_mutex(queue)->lock();
    return true;
}
void unlock_queue_held(VkQueue queue, bool held) { if (held) queue_mutex(queue)->unlock(); }

bool family_of(VkQueue queue, uint32_t* family) {
    std::lock_guard<std::mutex> guard(lock);
    if (!queue || !family) return false;
    for (const auto& q : queues)
        if (q.queue == queue) { *family = q.family; return true; }
    return false;
}

bool network_features_added(VkDevice device) {
    std::lock_guard<std::mutex> guard(lock);
    for (VkDevice d : augmented_devices)
        if (d == device) return true;
    return false;
}

bool pipeline_robustness_enabled(VkDevice device) {
    std::lock_guard<std::mutex> guard(lock);
    const auto it = device_robustness.find(device);
    return it != device_robustness.end() && it->second;
}

void set_device_callback(void (*callback)(VkDevice)) {
    std::lock_guard<std::mutex> guard(lock);
    device_callback = callback;
}

bool queue_for(VkDevice device, VkQueue* out, uint32_t* family) {
    if (!device || !out || !family) return false;
    VkPhysicalDevice physical{};
    std::vector<uint32_t> families;
    {
        std::lock_guard<std::mutex> guard(lock);
        physical = seen.physical;
        for (const auto& q : queues) if (q.device == device) families.push_back(q.family);
        if (!physical) return false;
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, props.data());
    auto usable = [&](uint32_t f) {
        return f < count && (props[f].queueFlags & VK_QUEUE_COMPUTE_BIT) && (props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT);
    };
    {
        std::lock_guard<std::mutex> guard(lock);
        for (const auto& q : queues)
            if (q.device == device && usable(q.family)) { *out = q.queue; *family = q.family; return true; }
        families = requested_families;
    }
    for (uint32_t f : families) {
        if (!usable(f)) continue;
        VkQueue q = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, f, 0, &q);   // through the export, so the hook records it
        if (q) { *out = q; *family = f; return true; }
    }
    return false;
}

bool queue(VkQueue* out, uint32_t* family) {
    std::lock_guard<std::mutex> guard(lock);
    if (!out || !family || !seen.device || !seen.physical) return false;
    // The network computes and transfers, and the colour conversion path also
    // needs graphics, so the family has to carry all of it. Asking the physical
    // device rather than assuming family 0 is the difference between working on
    // one driver and working on the machine in front of us.
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(seen.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(seen.physical, &count, props.data());
    for (const auto& q : queues) {
        if (q.device != seen.device || q.family >= count) continue;
        const VkQueueFlags flags = props[q.family].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) && (flags & VK_QUEUE_GRAPHICS_BIT)) {
            *out = q.queue; *family = q.family;
            return true;
        }
    }
    return false;
}

}  // namespace nr::pe::vkdevice
