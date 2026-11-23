#pragma once

#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using DWORD = uint32_t;
using BOOL = int;
using HANDLE = void*;
using HMODULE = void*;
using LPCSTR = const char*;
struct MODULEENTRY32W { unsigned dwSize; wchar_t szModule[256]; HMODULE hModule; };
const HANDLE INVALID_HANDLE_VALUE = reinterpret_cast<HANDLE>(uintptr_t(-1));
constexpr unsigned TH32CS_SNAPMODULE = 1, GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 2,
                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT = 4;
constexpr int MH_OK = 0;
using MH_STATUS = int;
inline bool have_driver = false, hook_failure = false;
inline unsigned hooks = 0;
inline std::vector<std::string> messages;
inline const HMODULE driver_module = reinterpret_cast<HMODULE>(1);
inline const VkQueue test_queue = reinterpret_cast<VkQueue>(2);
inline DWORD GetCurrentThreadId() {
    static std::atomic<DWORD> next{0};
    static thread_local DWORD id = ++next;
    return id;
}
inline HANDLE CreateToolhelp32Snapshot(unsigned, unsigned) { return have_driver ? driver_module : INVALID_HANDLE_VALUE; }
inline BOOL Module32FirstW(HANDLE, MODULEENTRY32W* e) {
    std::wcscpy(e->szModule, L"amdvlk32.dll"); e->hModule = driver_module; return 1;
}
inline BOOL Module32NextW(HANDLE, MODULEENTRY32W*) { return 0; }
inline void CloseHandle(HANDLE) {}
inline int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n) { return std::wcsncmp(a, b, n); }
inline BOOL GetModuleHandleExA(unsigned, LPCSTR, HMODULE* m) { *m = driver_module; return 1; }
inline VkResult fake_submit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) { return VK_SUCCESS; }
inline VkResult fake_submit2(VkQueue, uint32_t, const VkSubmitInfo2*, VkFence) { return VK_SUCCESS; }
inline VkResult fake_present(VkQueue, const VkPresentInfoKHR*) { return VK_SUCCESS; }
inline VkResult fake_wait(VkQueue) { return VK_SUCCESS; }
inline VkResult fake_sparse(VkQueue, uint32_t, const VkBindSparseInfo*, VkFence) { return VK_SUCCESS; }
inline VkResult fake_device_wait(VkDevice) { return VK_SUCCESS; }
inline void fake_get_queue(VkDevice, uint32_t, uint32_t, VkQueue* q) { *q = test_queue; }
inline PFN_vkVoidFunction fake_gdpa(VkDevice, const char* n) {
#define ENTRY(name, fn) if (!std::strcmp(n, name)) return reinterpret_cast<PFN_vkVoidFunction>(&fn)
    ENTRY("vkGetDeviceQueue", fake_get_queue);
    ENTRY("vkQueueSubmit", fake_submit);
    ENTRY("vkQueueSubmit2", fake_submit2);
    ENTRY("vkQueueSubmit2KHR", fake_submit2);
    ENTRY("vkQueuePresentKHR", fake_present);
    ENTRY("vkQueueWaitIdle", fake_wait);
    ENTRY("vkQueueBindSparse", fake_sparse);
    ENTRY("vkDeviceWaitIdle", fake_device_wait);
#undef ENTRY
    return nullptr;
}
inline void* GetProcAddress(HMODULE, const char* n) {
    return !std::strcmp(n, "vkGetDeviceProcAddr") || !std::strcmp(n, "vk_icdGetInstanceProcAddr")
        ? reinterpret_cast<void*>(&fake_gdpa) : nullptr;
}
inline MH_STATUS MH_CreateHook(void* target, void*, void** original) {
    if (hook_failure) return 1;
    *original = target; ++hooks; return MH_OK;
}
inline MH_STATUS MH_EnableHook(void*) { return MH_OK; }
inline void MH_RemoveHook(void*) {}
namespace nr::pe {
inline void log(const char* format, ...) {
    char text[4096]; va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args); va_end(args);
    messages.emplace_back(text);
}
}
