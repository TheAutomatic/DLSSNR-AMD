#pragma once

#include <tlhelp32.h>

namespace nr::pe::vkdevice::diag {

bool enabled = false;
struct QueueState {
    VkDevice device;
    std::recursive_mutex mutex;
    std::atomic<DWORD> owner{0};
    explicit QueueState(VkDevice d) : device(d) {}
};
std::mutex registry_lock;
std::map<VkQueue, std::shared_ptr<QueueState>> states;
std::vector<void*> targets;
std::atomic<uint64_t> total{0}, contended{0}, threads{0}, unknown{0};
std::atomic<DWORD> build_thread{0};
std::atomic<uint64_t> build_contended{0}, build_threads{0};

std::shared_ptr<QueueState> state(VkQueue q) {
    std::lock_guard<std::mutex> guard(registry_lock);
    auto& s = states[q];
    if (!s) { s = std::make_shared<QueueState>(VkDevice{}); ++unknown; }
    return s;
}

struct Call {
    std::vector<std::shared_ptr<QueueState>> kept;
    std::vector<std::unique_lock<std::recursive_mutex>> held;
    explicit Call(std::vector<std::shared_ptr<QueueState>> queues) : kept(std::move(queues)) {
        const DWORD tid = GetCurrentThreadId();
        uint64_t pair = 0;
        ++total;
        for (const auto& q : kept) {
            held.emplace_back(q->mutex, std::defer_lock);
            if (!held.back().try_lock()) {
                const DWORD owner = q->owner.load();
                if (!pair) {
                    pair = (uint64_t(tid) << 32) | owner;
                    threads = pair;
                    const DWORD worker = build_thread.load();
                    if (tid == worker || owner == worker) { build_threads = pair; ++build_contended; }
                    ++contended;
                }
                held.back().lock();
            }
            q->owner = tid;
        }
    }
    explicit Call(VkQueue queue) : Call(std::vector<std::shared_ptr<QueueState>>{state(queue)}) {}
};

std::vector<std::shared_ptr<QueueState>> device_queues(VkDevice device) {
    std::vector<std::shared_ptr<QueueState>> result;
    std::lock_guard<std::mutex> guard(registry_lock);
    // Every device-wide wait acquires queues in the same handle order.
    for (const auto& [q, s] : states)
        if (s->device == device || !s->device) result.push_back(s);
    return result;
}

PFN_vkQueueSubmit submit = nullptr;
PFN_vkQueueSubmit2 submit2 = nullptr, submit2_khr = nullptr;
PFN_vkQueuePresentKHR present = nullptr;
PFN_vkQueueWaitIdle wait_idle = nullptr;
PFN_vkQueueBindSparse bind_sparse = nullptr;
PFN_vkDeviceWaitIdle device_wait_idle = nullptr;

VkResult VKAPI_PTR hooked_submit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    Call call(q); return submit(q, n, s, f);
}
VkResult VKAPI_PTR hooked_submit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    Call call(q); return submit2(q, n, s, f);
}
VkResult VKAPI_PTR hooked_submit2_khr(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    Call call(q); return submit2_khr(q, n, s, f);
}
VkResult VKAPI_PTR hooked_present(VkQueue q, const VkPresentInfoKHR* p) {
    Call call(q); return present(q, p);
}
VkResult VKAPI_PTR hooked_wait_idle(VkQueue q) {
    Call call(q); return wait_idle(q);
}
VkResult VKAPI_PTR hooked_bind_sparse(VkQueue q, uint32_t n, const VkBindSparseInfo* b, VkFence f) {
    Call call(q); return bind_sparse(q, n, b, f);
}
VkResult VKAPI_PTR hooked_device_wait_idle(VkDevice d) {
    Call call(device_queues(d)); return device_wait_idle(d);
}

HMODULE module_of(const void* address) {
    HMODULE module = nullptr;
    if (address) GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCSTR>(address), &module);
    return module;
}

void install(VkDevice device, const VkDeviceCreateInfo* info) {
    if (!enabled) return;
    HMODULE driver = nullptr;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry{}; entry.dwSize = sizeof entry;
        for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
            if (_wcsnicmp(entry.szModule, L"amdvlk", 6) || !GetProcAddress(entry.hModule, "vk_icdGetInstanceProcAddr")) continue;
            if (driver && driver != entry.hModule) { driver = nullptr; break; }
            driver = entry.hModule;
        }
        CloseHandle(snapshot);
    }
    auto gdpa = driver ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(driver, "vkGetDeviceProcAddr")) : nullptr;
    if (driver && !gdpa) {
        const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(driver, "vk_icdGetInstanceProcAddr"));
        gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(VK_NULL_HANDLE, "vkGetDeviceProcAddr"));
    }
    if (!gdpa || module_of(reinterpret_cast<void*>(gdpa)) != driver || !info) {
        log("[nr] diag queue lock: INCOMPLETE coverage: no unique AMD ICD device resolver; no driver hooks installed");
        return;
    }
    const auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(gdpa(device, "vkGetDeviceQueue"));
    const auto get_queue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(gdpa(device, "vkGetDeviceQueue2"));
    if (!get_queue || module_of(reinterpret_cast<void*>(get_queue)) != driver ||
        (get_queue2 && module_of(reinterpret_cast<void*>(get_queue2)) != driver)) {
        log("[nr] diag queue lock: INCOMPLETE coverage: queue resolver is not inside the AMD ICD");
        return;
    }
    bool covered = true;
    unsigned count = 0;
    for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i) {
        const auto& ci = info->pQueueCreateInfos[i];
        for (uint32_t j = 0; j < ci.queueCount; ++j) {
            VkQueue queue = VK_NULL_HANDLE;
            if (!ci.flags) get_queue(device, ci.queueFamilyIndex, j, &queue);
            else if (get_queue2) {
                VkDeviceQueueInfo2 qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
                qi.flags = ci.flags; qi.queueFamilyIndex = ci.queueFamilyIndex; qi.queueIndex = j;
                get_queue2(device, &qi, &queue);
            }
            if (!queue) { covered = false; continue; }
            std::lock_guard<std::mutex> guard(registry_lock);
            auto& s = states[queue];
            if (!s) s = std::make_shared<QueueState>(device);
            else s->device = device;
            ++count;
        }
    }
    struct Entry { const char* name; void* hook; void** real; bool optional; };
#define NR_DIAG_ENTRY(name, fn, optional) {name, reinterpret_cast<void*>(&hooked_##fn), reinterpret_cast<void**>(&fn), optional}
    const Entry entries[] = {
        NR_DIAG_ENTRY("vkQueueSubmit", submit, false),
        NR_DIAG_ENTRY("vkQueueSubmit2", submit2, true),
        NR_DIAG_ENTRY("vkQueueSubmit2KHR", submit2_khr, true),
        NR_DIAG_ENTRY("vkQueuePresentKHR", present, false),
        NR_DIAG_ENTRY("vkQueueWaitIdle", wait_idle, false),
        NR_DIAG_ENTRY("vkQueueBindSparse", bind_sparse, false),
        NR_DIAG_ENTRY("vkDeviceWaitIdle", device_wait_idle, false),
    };
#undef NR_DIAG_ENTRY
    static std::map<void*, std::pair<void*, bool>> installed;
    std::string summary;
    for (const auto& e : entries) {
        void* target = reinterpret_cast<void*>(gdpa(device, e.name));
        const char* status = "absent";
        if (!target) covered &= e.optional;
        else if (module_of(target) != driver) { status = "NOT-ICD"; covered = false; }
        else {
            auto prior = installed.find(target);
            if (prior != installed.end()) {
                // Only the two Submit2 names may legally share an entry point.
                if (*e.real == prior->second.first) status = "already-hooked";
                else if (e.optional && prior->second.second) { *e.real = prior->second.first; status = "alias"; }
                else { status = "CONFLICT"; covered = false; }
            } else if (*e.real) {
                status = "DIFFERENT-ENTRY"; covered = false;
            } else {
                const MH_STATUS created = MH_CreateHook(target, e.hook, e.real);
                if (created == MH_OK && MH_EnableHook(target) == MH_OK) {
                    installed[target] = {*e.real, e.optional};
                    targets.push_back(target);
                    status = "hooked";
                } else {
                    if (created == MH_OK) MH_RemoveHook(target);
                    *e.real = nullptr;
                    status = "FAILED"; covered = false;
                }
            }
        }
        char item[128];
        std::snprintf(item, sizeof item, " %s=%p/%s", e.name, target, status);
        summary += item;
    }
    log("[nr] diag queue lock: device %p ICD %p, %u queues;%s", static_cast<void*>(device),
        static_cast<void*>(driver), count, summary.c_str());
    if (!covered || !count) log("[nr] diag queue lock: INCOMPLETE coverage; this run cannot exclude a queue race");
}

} // namespace nr::pe::vkdevice::diag
