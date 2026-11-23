#pragma once
#include <vulkan/vulkan.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace nr::diagfix {
enum class Fix { none, device, device_nobda, robust, all };
inline Fix parse(const char* value) {
    if (!value) return Fix::none;
    if (!std::strcmp(value, "device")) return Fix::device;
    if (!std::strcmp(value, "device-nobda")) return Fix::device_nobda;
    if (!std::strcmp(value, "robust")) return Fix::robust;
    if (!std::strcmp(value, "all")) return Fix::all;
    return Fix::none;
}
inline Fix active() { return parse(std::getenv("NR_DIAG_FIX")); }
inline const char* name(Fix f) {
    switch (f) {
    case Fix::device: return "device";
    case Fix::device_nobda: return "device-nobda";
    case Fix::robust: return "robust";
    case Fix::all: return "all";
    default: return "baseline";
    }
}
inline bool device(Fix f = active()) { return f == Fix::device || f == Fix::device_nobda || f == Fix::all; }
inline bool nobda(Fix f = active()) { return f == Fix::device_nobda || f == Fix::all; }
inline bool robust(Fix f = active()) { return f == Fix::robust || f == Fix::all; }
struct Caps { bool bda = false, subgroup = false, robustness = false, required = false; };
inline std::mutex caps_mutex;
inline std::map<VkDevice, Caps> devices;
inline Caps capabilities(VkDevice device) {
    std::lock_guard<std::mutex> guard(caps_mutex);
    const auto it = devices.find(device);
    return it == devices.end() ? Caps{} : it->second;
}
inline void remember(VkDevice device, Caps caps) { std::lock_guard<std::mutex> guard(caps_mutex); devices[device] = caps; }
inline void forget(VkDevice device) { std::lock_guard<std::mutex> guard(caps_mutex); devices.erase(device); }
inline bool pipeline_robustness(bool supported, VkDevice device) {
    return robust() ? capabilities(device).robustness : supported;
}
inline PFN_vkGetBufferDeviceAddress address_function(VkDevice device) {
    auto fn = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddress"));
    if (!fn) fn = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddressKHR"));
    return fn;
}
}
