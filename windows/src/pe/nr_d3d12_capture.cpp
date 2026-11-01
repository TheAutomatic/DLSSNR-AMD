// Diagnostic capture for the D3D12 (ASD) route: see nr_d3d12_capture.hpp.
//
// The order matters and cannot be repeated: the debug layer and DRED are process-wide settings that only apply to
// devices made afterwards, so start() must run from InitializeASI (the ASI is loaded by the proxy before the game
// makes anything). A device made before that - a launcher's, a wrapper's - is not covered; the log says so.
#include "nr_d3d12_capture.hpp"

#include <dxgi1_6.h>
#include <dbghelp.h>
#include <algorithm>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include "nr_pe_log.hpp"

namespace nr::pe::capture {
namespace {

std::mutex g_mutex;
bool g_on = false;
std::wstring g_dir;
std::wstring g_game;
FILE* g_errors = nullptr;   // every debug-layer message, as it appears
FILE* g_run = nullptr;      // the run's header and the first frames' schedule
ID3D12Device* g_device = nullptr;
ID3D12InfoQueue* g_queue = nullptr;
UINT64 g_seen = 0;
bool g_removed_written = false;

std::string narrow(const wchar_t* s) {
    if (!s) return {};
    char b[2048] = {};
    WideCharToMultiByte(CP_UTF8, 0, s, -1, b, sizeof b - 1, nullptr, nullptr);
    return b;
}

std::wstring path_of(const wchar_t* leaf) { return g_dir + L"\\" + leaf; }

void write(FILE* f, const char* text) {
    if (!f || !text) return;
    std::fputs(text, f);
    std::fputc('\n', f);
    std::fflush(f);   // the process may be gone one call later
}

std::string file_line(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return "missing";
    const unsigned long long size = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    return std::to_string(size) + " B, written " + std::to_string(fa.ftLastWriteTime.dwLowDateTime);
}

void minidump() {
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    if (!dbg) return;
    using Fn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                             PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(dbg, "MiniDumpWriteDump"));
    if (!fn) { FreeLibrary(dbg); return; }
    const std::wstring path = path_of(L"crash.dmp");
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { FreeLibrary(dbg); return; }
    const BOOL ok = fn(GetCurrentProcess(), GetCurrentProcessId(), f,
                       MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                     MiniDumpWithUnloadedModules),
                       nullptr, nullptr, nullptr);
    CloseHandle(f);
    FreeLibrary(dbg);
    nr::pe::log("[capture] minidump %s: %s", narrow(path.c_str()).c_str(), ok ? "written" : "FAILED");
}

const char* op_name(D3D12_AUTO_BREADCRUMB_OP op) {
    switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTILES: return "CopyTiles";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return "ExecuteBundle";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
    case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
    case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BuildRaytracingAS";
    case D3D12_AUTO_BREADCRUMB_OP_EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO:
        return "EmitRaytracingASPostBuildInfo";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE: return "CopyRaytracingAS";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DispatchRays";
    case D3D12_AUTO_BREADCRUMB_OP_SETPIPELINESTATE1: return "SetPipelineState1";
    case D3D12_AUTO_BREADCRUMB_OP_INITIALIZEMETACOMMAND: return "InitializeMetaCommand";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEMETACOMMAND: return "ExecuteMetaCommand";
    case D3D12_AUTO_BREADCRUMB_OP_ESTIMATEMOTION: return "EstimateMotion";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEMOTIONVECTORHEAP: return "ResolveMotionVectorHeap";
    default: return "other";
    }
}

const char* severity_name(D3D12_MESSAGE_SEVERITY s) {
    switch (s) {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION: return "CORRUPTION";
    case D3D12_MESSAGE_SEVERITY_ERROR: return "ERROR";
    case D3D12_MESSAGE_SEVERITY_WARNING: return "WARNING";
    case D3D12_MESSAGE_SEVERITY_INFO: return "INFO";
    default: return "MESSAGE";
    }
}

}  // namespace

const std::wstring& folder() { return g_dir; }
bool on() { return g_on; }

void start(const std::string& game_folder, bool wanted, bool wanted_debug_layer) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_on) return;
    // [D3D12ASD] Capture / NR_ASD_CAPTURE: on by default while the route is young - the debug layer costs frames and
    // this is the only way a reproduction leaves evidence.
    wchar_t env[8] = L"";
    if (!wanted || (GetEnvironmentVariableW(L"NR_ASD_CAPTURE", env, 8) && env[0] == L'0')) {
        nr::pe::log("[capture] off (NR_ASD_CAPTURE=0): a device fault leaves nothing to read");
        return;
    }
    // The game's folder: the main module's, whatever path the ASI itself sits in.
    wchar_t exe[MAX_PATH * 2] = L"";
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    std::wstring root = n ? std::wstring(exe, n) : std::wstring(game_folder.begin(), game_folder.end());
    if (const size_t slash = root.find_last_of(L"\\/"); slash != std::wstring::npos) root.resize(slash);
    g_game = root;
    g_dir = g_game + L"\\nr-diag";
    CreateDirectoryW(g_dir.c_str(), nullptr);

    g_run = _wfopen(path_of(L"run.txt").c_str(), L"wb");
    write(g_run, "DLSSNR-AMD diagnostic capture");
    write(g_run, ("folder: " + narrow(g_dir.c_str())).c_str());

    // The debug layer is NOT enabled by default: a game that ships its own Agility SDK layers (D3D12\x64\...) hands
    // D3D12CreateDevice an app-local D3D12Core, and the debug layer this ASI can create is the system one - the two do
    // not match, and device creation then faults inside D3D12SDKLayers.dll before the game's first frame. [D3D12ASD]
    // DebugLayer=1 asks for it anyway, for a case where the layers are known to match.
    g_on = true;   // the schedule and the device-removal report need nothing from D3D12
    bool debug_layer = false;
    wchar_t no_layer[4] = L"";
    if (GetEnvironmentVariableW(L"NR_CAPTURE_LAYER", no_layer, 4) && no_layer[0] == L'0') wanted_debug_layer = false;
#ifndef NR_ASD_NO_DEBUG_LAYER
    if (wanted_debug_layer) {
        ID3D12Debug* debug = nullptr;
        const HRESULT r = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
        if (SUCCEEDED(r) && debug) {
            debug->EnableDebugLayer();
            debug->Release();
            debug_layer = true;
            nr::pe::log("[capture] D3D12 debug layer on (DebugLayer=1): messages are captured below");
        } else {
            nr::pe::log("[capture] the D3D12 debug layer is not available (0x%08lX)", static_cast<unsigned long>(r));
        }
    } else {
        nr::pe::log("[capture] D3D12 debug layer off (DebugLayer=0): this game ships its own SDK layers; the schedule "
                    "and the device-removal report are captured without it");
    }
#endif

    // DRED: auto breadcrumbs (the last commands each list reached) and page faults (the address and the allocations
    // it may belong to). The settings interface comes from D3D12GetDebugInterface by its IID - no class id, and it
    // is read before the device is made, which is the only moment it applies.
    ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
    wchar_t no_dred[4] = L"";
    const bool want_dred = !(GetEnvironmentVariableW(L"NR_CAPTURE_DRED", no_dred, 4) && no_dred[0] == L'0');
    HRESULT rd = want_dred ? D3D12GetDebugInterface(IID_PPV_ARGS(&dred)) : E_NOTIMPL;
    bool breadcrumbs = false, faults = false;
    if (SUCCEEDED(rd) && dred) {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetWatsonDumpEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->Release();
        breadcrumbs = faults = true;
        nr::pe::log("[capture] DRED: breadcrumbs and page faults forced on (set before the device was made)");
    } else {
        nr::pe::log("[capture] DRED settings are not available (0x%08lX)", static_cast<unsigned long>(rd));
    }
    char line[256];
    std::snprintf(line, sizeof line, "debug layer: %s | DRED breadcrumbs: %s | page faults: %s",
                  debug_layer ? "on" : "off (DebugLayer=0)", breadcrumbs ? "on" : "no", faults ? "on" : "no");
    write(g_run, line);
    g_errors = _wfopen(path_of(L"d3d12-messages.txt").c_str(), L"wb");
    write(g_errors, "D3D12 debug layer messages, as they appeared");
    nr::pe::log("[capture] run folder %s", narrow(g_dir.c_str()).c_str());
}

void adopt(ID3D12Device* device) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!device || g_device) return;
    device->AddRef();
    g_device = device;
    if (g_on && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&g_queue))) && g_queue) {
        nr::pe::log("[capture] info queue: %llu message(s) stored so far",
                    static_cast<unsigned long long>(g_queue->GetNumStoredMessages()));
    }
    // The adapter and driver, for the run's header: an error is only reproducible against those.
    if (IDXGIDevice* dxgi = nullptr; SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && dxgi) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && adapter) {
            DXGI_ADAPTER_DESC d{};
            if (SUCCEEDED(adapter->GetDesc(&d))) {
                char b[512];
                std::snprintf(b, sizeof b, "%ls | vendor 0x%04X device 0x%04X | %llu MB | LUID %08lX:%08lX",
                              d.Description, d.VendorId, d.DeviceId,
                              static_cast<unsigned long long>(d.DedicatedVideoMemory >> 20),
                              static_cast<unsigned long>(d.AdapterLuid.HighPart),
                              static_cast<unsigned long>(d.AdapterLuid.LowPart));
                note("adapter", b);
            }
            adapter->Release();
        }
        dxgi->Release();
    }
}

void note(const char* key, const std::string& value) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_run) return;
    write(g_run, (std::string(key) + ": " + value).c_str());
    nr::pe::log("[capture] %s: %s", key, value.c_str());
}

void drain() {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_queue) {
        if (!g_device || !g_on) return;
        if (FAILED(g_device->QueryInterface(IID_PPV_ARGS(&g_queue))) || !g_queue) return;
    }
    const UINT64 n = g_queue->GetNumStoredMessages();
    for (UINT64 i = g_seen; i < n; ++i) {
        SIZE_T bytes = 0;
        if (FAILED(g_queue->GetMessage(i, nullptr, &bytes)) || !bytes) continue;
        std::vector<char> buf(bytes);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (FAILED(g_queue->GetMessage(i, m, &bytes))) continue;
        if (m->Severity < D3D12_MESSAGE_SEVERITY_WARNING) continue;
        char head[128];
        std::snprintf(head, sizeof head, "[%s id=%d] ", severity_name(m->Severity), static_cast<int>(m->ID));
        std::string text = head;
        const char* desc = reinterpret_cast<const char*>(m->pDescription);
        if (desc) text.append(desc, m->DescriptionByteLength ? std::min<size_t>(m->DescriptionByteLength - 1, 4000) : 0);
        write(g_errors, text.c_str());
        nr::pe::log("[d3d12] %s", text.c_str());
    }
    g_seen = n;
}

void mark(ID3D12GraphicsCommandList* cmd, const wchar_t* what) {
    if (!g_on || !cmd || !what) return;
    // PIX3 form (BeginEvent with a wide name): DRED's breadcrumbs record it, so the last marker before a fault is the
    // stage that faulted. Cheap enough to keep on for every dispatch of the network.
    cmd->BeginEvent(0, what, static_cast<UINT>(std::wcslen(what) * sizeof(wchar_t)));
}

void schedule(const std::string& line) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_run) return;
    write(g_run, line.c_str());
}

void name(ID3D12Object* object, const wchar_t* what) {
    if (object && what) object->SetName(what);
}

void device_removed(HRESULT why) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_removed_written) return;
    g_removed_written = true;
    const std::wstring path = path_of(L"device-removed.txt");
    FILE* f = _wfopen(path.c_str(), L"wb");
    char b[512];
    std::snprintf(b, sizeof b, "device removed: 0x%08lX", static_cast<unsigned long>(why));
    write(f, b);
    write(g_errors, b);
    nr::pe::log("[capture] %s - writing breadcrumbs, page faults and a minidump", b);
    drain();

    if (g_device) {
        ID3D12DeviceRemovedExtendedData* dred = nullptr;
        if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dred))) && dred) {
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
            if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc)) && bc.pHeadAutoBreadcrumbNode) {
                write(f, "auto breadcrumbs (the last commands each command list reached):");
                for (auto* node = bc.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
                    std::snprintf(b, sizeof b, "  list %p: %u breadcrumb(s), last completed %u of %u, name %ls",
                                  static_cast<void*>(node->pCommandList), node->BreadcrumbCount,
                                  node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0u, node->BreadcrumbCount,
                                  node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"(none)");
                    write(f, b);
                    // The tail of that list's breadcrumbs: the fault is at or just after the last one reached.
                    const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : node->BreadcrumbCount;
                    const UINT from = done > 12 ? done - 12 : 0;
                    for (UINT i = from; i < node->BreadcrumbCount; ++i) {
                        std::snprintf(b, sizeof b, "    [%u]%s op %u %s", i, i < done ? " done" : " RUNNING",
                                      static_cast<unsigned>(node->pCommandHistory[i]),
                                      op_name(node->pCommandHistory[i]));
                        write(f, b);
                    }
                }
            } else {
                write(f, "auto breadcrumbs: none (DRED was not on when the device was made)");
            }
            D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
            if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA) {
                std::snprintf(b, sizeof b, "page fault at 0x%llX", static_cast<unsigned long long>(pf.PageFaultVA));
                write(f, b);
                auto walk = [&](const char* kind, const D3D12_DRED_ALLOCATION_NODE* node) {
                    for (; node; node = node->pNext) {
                        char nm[256] = "";
                        if (node->ObjectNameW) WideCharToMultiByte(CP_UTF8, 0, node->ObjectNameW, -1, nm, sizeof nm - 1, nullptr, nullptr);
                        else if (node->ObjectNameA) std::snprintf(nm, sizeof nm, "%s", node->ObjectNameA);
                        std::snprintf(b, sizeof b, "  %s: %s (type %d)", kind, nm[0] ? nm : "(unnamed)",
                                      static_cast<int>(node->AllocationType));
                        write(f, b);
                    }
                };
                walk("existing", pf.pHeadExistingAllocationNode);
                walk("freed", pf.pHeadRecentFreedAllocationNode);
            } else {
                write(f, "page faults: none reported");
            }
            dred->Release();
        } else {
            write(f, "DRED data interface not available on this device (Agility SDK 1.4+ needed)");
        }
    }
    if (g_errors) { std::fclose(g_errors); g_errors = nullptr; }
    if (g_run) { std::fclose(g_run); g_run = nullptr; }
    nr::pe::log("[capture] wrote %s", narrow(path.c_str()).c_str());
    minidump();
}

}  // namespace nr::pe::capture
