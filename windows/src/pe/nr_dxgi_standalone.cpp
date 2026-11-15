// Standalone DXGI proxy for apps without OptiScaler/DLSS/FSR/XeSS (media players, capture viewers, general DX11/DX12
// apps). Loads the system dxgi.dll from System32 and loads dlssnr-amd.asi outside DllMain on first DXGI call, calling
// InitializeASI() so NR Control hooks and Present layers attach directly at DXGI Present.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>

#include <atomic>
#include <cstring>
#include <cwchar>

namespace {

enum {
    kApplyCompatResolutionQuirking = 0,
    kCompatString,
    kCompatValue,
    kDXGIDumpJournal,
    kPIXBeginCapture,
    kPIXEndCapture,
    kPIXGetCaptureState,
    kSetAppCompatStringPointer,
    kUpdateHMDEmulationStatus,
    kCreateDXGIFactory,
    kCreateDXGIFactory1,
    kCreateDXGIFactory2,
    kDXGID3D10CreateDevice,
    kDXGID3D10CreateLayeredDevice,
    kDXGID3D10GetLayeredDeviceSize,
    kDXGID3D10RegisterLayers,
    kDXGIDeclareAdapterRemovalSupport,
    kDXGIDisableVBlankVirtualization,
    kDXGIGetDebugInterface1,
    kDXGIReportAdapterConfiguration,
    kProcCount
};

constexpr const char* kProcNames[kProcCount] = {
    "ApplyCompatResolutionQuirking",
    "CompatString",
    "CompatValue",
    "DXGIDumpJournal",
    "PIXBeginCapture",
    "PIXEndCapture",
    "PIXGetCaptureState",
    "SetAppCompatStringPointer",
    "UpdateHMDEmulationStatus",
    "CreateDXGIFactory",
    "CreateDXGIFactory1",
    "CreateDXGIFactory2",
    "DXGID3D10CreateDevice",
    "DXGID3D10CreateLayeredDevice",
    "DXGID3D10GetLayeredDeviceSize",
    "DXGID3D10RegisterLayers",
    "DXGIDeclareAdapterRemovalSupport",
    "DXGIDisableVBlankVirtualization",
    "DXGIGetDebugInterface1",
    "DXGIReportAdapterConfiguration",
};

HMODULE g_self = nullptr;
HMODULE g_sys = nullptr;
HMODULE g_asi = nullptr;
INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;
std::atomic<bool> g_hooks_enabled{false};
thread_local bool t_dispatching = false;

void load_system_dxgi() {
    if (g_sys) return;
    wchar_t sysdir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(sysdir, MAX_PATH);
    if (n > 0 && n + 10 < MAX_PATH) {
        std::wcscpy(sysdir + n, L"\\dxgi.dll");
        g_sys = LoadLibraryW(sysdir);
    }
    if (!g_sys) g_sys = LoadLibraryW(L"dxgi.dll.");
}

HMODULE load_asi_sibling() {
    if (!g_self) return nullptr;
    wchar_t dir[MAX_PATH * 2] = {};
    const DWORD len = GetModuleFileNameW(g_self, dir, MAX_PATH * 2);
    if (len == 0 || len >= MAX_PATH * 2) return nullptr;
    wchar_t* slash = std::wcsrchr(dir, L'\\');
    if (!slash) return nullptr;
    slash[1] = L'\0';
    const size_t base = size_t(slash + 1 - dir);
    constexpr const wchar_t* kRel[] = {L"dlssnr-amd.asi", L"dlssnr-amd\\dlssnr-amd.asi"};
    for (const wchar_t* rel : kRel) {
        if (base + std::wcslen(rel) + 1 > MAX_PATH * 2) continue;
        std::wcscpy(dir + base, rel);
        if (HMODULE m = LoadLibraryW(dir)) return m;
    }
    return nullptr;
}

}  // namespace

extern "C" {
void* g_dxgi_procs[kProcCount] = {};

void nr_standalone_init() {
    if (g_hooks_enabled.load(std::memory_order_acquire)) return;
    InitOnceExecuteOnce(
        &g_init_once,
        [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
            load_system_dxgi();
            if (g_sys) {
                for (int i = 0; i < kProcCount; ++i)
                    g_dxgi_procs[i] = reinterpret_cast<void*>(GetProcAddress(g_sys, kProcNames[i]));
            }
            g_asi = load_asi_sibling();
            if (g_asi) {
                using InitFn = void (*)();
                if (auto init = reinterpret_cast<InitFn>(GetProcAddress(g_asi, "InitializeASI"))) init();
            } else {
                OutputDebugStringA("[nr] standalone dxgi.dll: dlssnr-amd.asi was not found next to dxgi.dll\n");
            }
            g_hooks_enabled.store(true, std::memory_order_release);
            return TRUE;
        },
        nullptr, nullptr);
}
}  // extern "C"

namespace {

using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using DebugInterface1Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);

template <class Fn, class... Args>
HRESULT forward_factory(const char* name, int proc_index, Args... args) {
    const bool hooked_before = g_hooks_enabled.load(std::memory_order_acquire);
    nr_standalone_init();
    if (!hooked_before && !t_dispatching && g_self) {
        if (auto self_fn = reinterpret_cast<Fn>(GetProcAddress(g_self, name))) {
            t_dispatching = true;
            const HRESULT hr = self_fn(args...);
            t_dispatching = false;
            return hr;
        }
    }
    auto sys_fn = reinterpret_cast<Fn>(g_dxgi_procs[proc_index]);
    return sys_fn ? sys_fn(args...) : E_FAIL;
}

}  // namespace

extern "C" {

__declspec(dllexport) void NR_Standalone_DXGI() {}

__attribute__((noinline)) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** factory) {
    return forward_factory<CreateFactoryFn>("CreateDXGIFactory", kCreateDXGIFactory, riid, factory);
}

__attribute__((noinline)) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory) {
    return forward_factory<CreateFactoryFn>("CreateDXGIFactory1", kCreateDXGIFactory1, riid, factory);
}

__attribute__((noinline)) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    return forward_factory<CreateFactory2Fn>("CreateDXGIFactory2", kCreateDXGIFactory2, flags, riid, factory);
}

__attribute__((noinline)) HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debug) {
    nr_standalone_init();
    auto sys_fn = reinterpret_cast<DebugInterface1Fn>(g_dxgi_procs[kDXGIGetDebugInterface1]);
    return sys_fn ? sys_fn(flags, riid, debug) : E_NOINTERFACE;
}

}  // extern "C"

// Preserve all Win64 argument registers (RCX/RDX/R8/R9, XMM0..XMM3) and caller stack frame across lazy init,
// then tail-jump into System32\dxgi.dll.
__asm__(
    ".text\n"
    ".macro NR_DXGI_FWD name, idx\n"
    ".globl \\name\n"
    "\\name:\n"
    "    movq g_dxgi_procs + 8*\\idx(%rip), %rax\n"
    "    testq %rax, %rax\n"
    "    jz 1f\n"
    "    jmpq *%rax\n"
    "1:\n"
    "    pushq %rcx\n"
    "    pushq %rdx\n"
    "    pushq %r8\n"
    "    pushq %r9\n"
    "    subq $0x68, %rsp\n"
    "    movdqu %xmm0, 0x20(%rsp)\n"
    "    movdqu %xmm1, 0x30(%rsp)\n"
    "    movdqu %xmm2, 0x40(%rsp)\n"
    "    movdqu %xmm3, 0x50(%rsp)\n"
    "    call nr_standalone_init\n"
    "    movdqu 0x50(%rsp), %xmm3\n"
    "    movdqu 0x40(%rsp), %xmm2\n"
    "    movdqu 0x30(%rsp), %xmm1\n"
    "    movdqu 0x20(%rsp), %xmm0\n"
    "    addq $0x68, %rsp\n"
    "    popq %r9\n"
    "    popq %r8\n"
    "    popq %rdx\n"
    "    popq %rcx\n"
    "    movq g_dxgi_procs + 8*\\idx(%rip), %rax\n"
    "    testq %rax, %rax\n"
    "    jz 2f\n"
    "    jmpq *%rax\n"
    "2:\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".endm\n"
    "NR_DXGI_FWD ApplyCompatResolutionQuirking, 0\n"
    "NR_DXGI_FWD CompatString, 1\n"
    "NR_DXGI_FWD CompatValue, 2\n"
    "NR_DXGI_FWD DXGIDumpJournal, 3\n"
    "NR_DXGI_FWD PIXBeginCapture, 4\n"
    "NR_DXGI_FWD PIXEndCapture, 5\n"
    "NR_DXGI_FWD PIXGetCaptureState, 6\n"
    "NR_DXGI_FWD SetAppCompatStringPointer, 7\n"
    "NR_DXGI_FWD UpdateHMDEmulationStatus, 8\n"
    "NR_DXGI_FWD DXGID3D10CreateDevice, 12\n"
    "NR_DXGI_FWD DXGID3D10CreateLayeredDevice, 13\n"
    "NR_DXGI_FWD DXGID3D10GetLayeredDeviceSize, 14\n"
    "NR_DXGI_FWD DXGID3D10RegisterLayers, 15\n"
    "NR_DXGI_FWD DXGIDeclareAdapterRemovalSupport, 16\n"
    "NR_DXGI_FWD DXGIDisableVBlankVirtualization, 17\n"
    "NR_DXGI_FWD DXGIReportAdapterConfiguration, 19\n");

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        if (g_asi) { FreeLibrary(g_asi); g_asi = nullptr; }
        if (g_sys) { FreeLibrary(g_sys); g_sys = nullptr; }
    }
    return TRUE;
}
