// The native D3D12 direct-ELF backend's initialization and canary (nr_d3d12_asd.hpp).
#include "nr_d3d12_asd.hpp"
#include "nr_d3d12_asd_canary_blob.h"
#include "nr_d3d12_asd_elf.hpp"
#include "nr_pe_d3d12split.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_session.hpp"
#include "nr_pe_vkdevice.hpp"
#include <dxgi1_4.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

namespace nr::pe::asd {
namespace {

using nr::pe::log;

std::mutex g_lock;            // the strings below
std::wstring g_folder, g_ini;
bool g_enabled = false;
std::string g_registration = "not run", g_backend = "not run", g_network;
std::atomic<int> g_canary{int(Canary::kNotRun)};
std::atomic<bool> g_canary_started{false};
std::string g_canary_line = "not run";
std::atomic<uint64_t> g_runs{0}, g_fallbacks{0};
std::atomic<bool> g_produced{false};   // the last recorded frame actually carried NR (not a copy-through)

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::string hr_text(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(hr));
    return b;
}
bool exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
void set_registration(const std::string& s) {
    { std::lock_guard<std::mutex> g(g_lock); g_registration = s; }
    log("[asd] registration: %s", s.c_str());
}
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---- the canary on the game's device ------------------------------------------------------------------------------
// The canary ACO ELF (elf::kCanaryAcoElf) and a known input: the ACO kernel writes aco_value().
constexpr uint32_t kN = 128, kWords = 32, kDescWord3 = 0x22014facu;
void fill_push(uint32_t* p) {
    p[0] = 3; p[1] = 0x21; p[2] = kN; p[3] = 0x100;
    for (uint32_t k = 4; k < kWords; ++k) p[k] = 0x30 + k;
}
uint32_t aco_value(uint32_t i, uint32_t in, const uint32_t* p) {
    uint32_t x = in ^ (p[3] + i);
    for (uint32_t k = 4; k < kWords; ++k) x ^= p[k] + (i << (k & 7));
    return x * p[0] + p[1] + i;
}
void put_desc(uint32_t* d, uint64_t va, uint32_t bytes) {
    d[0] = uint32_t(va); d[1] = uint32_t((va >> 32) & 0xffffu); d[2] = bytes; d[3] = kDescWord3;
}

struct Pipeline { std::vector<uint8_t> root, cs; UINT flags = 0, node_mask = 0; bool found = false; std::string why; };

ID3D12Resource* buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, uint64_t size, D3D12_RESOURCE_STATES state, bool uav) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

struct CanaryJob { ID3D12Device* device; ID3D12CommandQueue* queue; };
Canary run_canary(ID3D12Device* device, ID3D12CommandQueue* queue, std::string* out_line);
void finish_canary(Canary c, const std::string& line);

void finish_canary(Canary c, const std::string& line) {
    { std::lock_guard<std::mutex> g(g_lock); g_canary_line = std::string(canary_name(c)) + ": " + line; }
    g_canary = int(c);
    if (c != Canary::kHit) nr::pe::set_d3d12_native(false);   // what can run: as on the bridge (nr_pe_session.cpp)
    log("[asd] canary %s: %s%s", canary_name(c), line.c_str(),
        c == Canary::kHit ? " - the D3D12 ASD backend may run" : " - the D3D12 ASD backend stays off (the Vulkan bridge runs)");
}

// The canary on `device` with `queue`: ASD_HIT, ASD_MISS or ASD_FAIL, and the line saying why.
Canary run_canary(ID3D12Device* device, ID3D12CommandQueue* queue, std::string* out_line) {
    Pipeline pl;
    pl.root.assign(canary_blob::kRoot, canary_blob::kRoot + sizeof canary_blob::kRoot);
    pl.cs.assign(canary_blob::kCs, canary_blob::kCs + sizeof canary_blob::kCs);
    pl.flags = canary_blob::kFlags;
    pl.node_mask = canary_blob::kNodeMask;
    pl.found = true;
    std::string why;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12Resource *in = nullptr, *out = nullptr, *table = nullptr, *readback = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    Canary result = Canary::kFail;
    std::string line;
    do {
        if (!pl.found) { line = why; break; }
        HRESULT hr = device->CreateRootSignature(0, pl.root.data(), pl.root.size(), IID_PPV_ARGS(&rs));
        if (FAILED(hr)) { line = "CreateRootSignature " + hr_text(hr); break; }
        const uint32_t threads[3] = {64, 1, 1};
        const ULONGLONG t0 = GetTickCount64();
        const bool pso_ok = elf::create_pso_from_elf(device, elf::kCanaryAcoElf, sizeof elf::kCanaryAcoElf, threads, &pso, &why);
        log("[asd] canary: CreateComputePipelineFromElf %s in %llu ms", pso_ok ? "0x00000000" : why.c_str(),
            (unsigned long long)(GetTickCount64() - t0));
        if (!pso_ok) { line = why; break; }
        in = buffer(device, D3D12_HEAP_TYPE_UPLOAD, kN * 4, D3D12_RESOURCE_STATE_GENERIC_READ, false);
        table = buffer(device, D3D12_HEAP_TYPE_UPLOAD, 4096, D3D12_RESOURCE_STATE_GENERIC_READ, false);
        out = buffer(device, D3D12_HEAP_TYPE_DEFAULT, kN * 4, D3D12_RESOURCE_STATE_COMMON, true);
        readback = buffer(device, D3D12_HEAP_TYPE_READBACK, kN * 4, D3D12_RESOURCE_STATE_COPY_DEST, false);
        if (!in || !table || !out || !readback) { line = "the canary's buffers could not be made"; break; }
        uint32_t input[kN], push[kWords] = {};
        for (uint32_t i = 0; i < kN; ++i) input[i] = 0x11110000u + i;
        fill_push(push);
        const uint64_t in_va = in->GetGPUVirtualAddress(), out_va = out->GetGPUVirtualAddress();
        const uint64_t table_va = table->GetGPUVirtualAddress(), push_va = table_va + 256;
        if ((in_va >> 48) || (out_va >> 48) || (table_va >> 32) != (push_va >> 32)) { line = "buffer addresses out of range"; break; }
        void* map = nullptr;
        const D3D12_RANGE none{0, 0};
        if (FAILED(in->Map(0, &none, &map))) { line = "Map(input)"; break; }
        std::memcpy(map, input, sizeof input);
        in->Unmap(0, nullptr);
        if (FAILED(table->Map(0, &none, &map))) { line = "Map(table)"; break; }
        std::memset(map, 0, 4096);
        put_desc(static_cast<uint32_t*>(map), in_va, kN * 4);
        put_desc(reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(map) + 16), out_va, kN * 4);
        std::memcpy(static_cast<uint8_t*>(map) + 256, push, sizeof push);
        table->Unmap(0, nullptr);
        uint32_t roots[11] = {};
        roots[0] = uint32_t(table_va);
        roots[1] = uint32_t(push_va);
        for (uint32_t i = 0; i < 8; ++i) roots[2 + i] = push[i];
        roots[10] = uint32_t(table_va >> 32);

        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
            !(event = CreateEventW(nullptr, FALSE, FALSE, nullptr))) { line = "the canary's command list could not be made"; break; }
        list->SetComputeRootSignature(rs);
        list->SetPipelineState(pso);
        list->SetComputeRoot32BitConstants(0, 11, roots, 0);
        list->SetComputeRootUnorderedAccessView(1, out_va);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = out;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        list->ResourceBarrier(1, &b);
        list->Dispatch(2, 1, 1);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        list->CopyBufferRegion(readback, 0, out, 0, kN * 4);
        if (FAILED(list->Close())) { line = "Close(the canary's list)"; break; }
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence, 1)) || FAILED(fence->SetEventOnCompletion(1, event))) { line = "Signal"; break; }
        if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) {
            line = "the GPU did not finish the canary within 5 s";
            // what the GPU may still use is left as it is (never released under it)
            in = out = table = readback = nullptr; list = nullptr; alloc = nullptr; pso = nullptr; rs = nullptr;
            break;
        }
        if (FAILED(readback->Map(0, nullptr, &map))) { line = "Map(readback)"; break; }
        uint32_t got[kN];
        std::memcpy(got, map, sizeof got);
        readback->Unmap(0, nullptr);
        uint32_t acc = 0;
        for (uint32_t r : roots) acc += r;
        uint32_t aco_mis = 0, hlsl_mis = 0, first = ~0u;
        for (uint32_t i = 0; i < kN; ++i) {
            if (got[i] != aco_value(i, input[i], push)) { ++aco_mis; if (first == ~0u) first = i; }
            if (got[i] != (acc ^ i)) ++hlsl_mis;
        }
        char text[200];
        std::snprintf(text, sizeof text, "%u values: %u differ from the ACO kernel's, %u from the HLSL carrier's (first ACO "
                      "difference at %d)", kN, aco_mis, hlsl_mis, first == ~0u ? -1 : int(first));
        line = text;
        result = aco_mis == 0 ? Canary::kHit : hlsl_mis == 0 ? Canary::kMiss : Canary::kFail;
    } while (false);
    if (event) CloseHandle(event);
    release(fence); release(list); release(alloc); release(readback); release(table); release(out); release(in);
    release(pso); release(rs);
    *out_line = line;
    return result;
}

DWORD WINAPI canary_thread(void* p) {
    CanaryJob job = *static_cast<CanaryJob*>(p);
    delete static_cast<CanaryJob*>(p);
    ID3D12Device* device = job.device;
    std::string line;
    const Canary result = run_canary(device, job.queue, &line);
    job.queue->Release();
    finish_canary(result, line);
    device->Release();
    return 0;
}

}  // namespace

const char* canary_name(Canary c) {
    switch (c) {
    case Canary::kHit: return "ASD_HIT";
    case Canary::kMiss: return "ASD_MISS";
    case Canary::kFail: return "ASD_FAIL";
    case Canary::kRunning: return "running";
    default: return "not run";
    }
}

const std::wstring& folder() { return g_folder; }
std::wstring ini_path() { return g_ini; }
bool enabled() { return g_enabled; }
Canary canary() { return Canary(g_canary.load()); }

void start(HMODULE self) {
    std::wstring here(32768, L'\0');
    here.resize(GetModuleFileNameW(self, here.data(), DWORD(here.size())));
    const size_t slash = here.find_last_of(L"\\/");
    here = slash == std::wstring::npos ? L"." : here.substr(0, slash);
    g_folder = here + L"\\dlssnr-amd-asd";
    g_ini = g_folder + L"\\dlssnr-amd-asd.ini";
    if (!exists(g_ini)) { CreateDirectoryW(g_folder.c_str(), nullptr); WritePrivateProfileStringW(L"D3D12ASD", L"Enabled", L"1", g_ini.c_str()); }
    g_enabled = GetPrivateProfileIntW(L"D3D12ASD", L"Enabled", 1, g_ini.c_str()) != 0;
    log("[asd] D3D12 direct-ELF backend %s (%s: Enabled %d)", g_enabled ? "on" : "off", utf8(g_ini).c_str(),
        g_enabled ? 1 : 0);
    if (!g_enabled) return;
    set_registration("direct ELF (IAmdExtD3DDevice5::CreateComputePipelineFromElf; no ASD registration)");
}

void on_device(ID3D12Device* device) {
    if (!g_enabled || !device || g_canary_started.load()) return;
    {
        IDXGIFactory4* fac = nullptr;
        IDXGIAdapter1* ad = nullptr;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) && fac) {
            if (SUCCEEDED(fac->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&ad))) && ad) {
                if (SUCCEEDED(ad->GetDesc1(&desc))) {
                    std::string nm = utf8(desc.Description);
                    if (nm.find("NVIDIA") != std::string::npos && desc.VendorId != 0x1002) {
                        // Spoofed by OptiScaler: check if another adapter in the system is AMD RDNA4
                        for (UINT i = 0; ; ++i) {
                            IDXGIAdapter1* a2 = nullptr;
                            if (fac->EnumAdapters1(i, &a2) != S_OK) break;
                            DXGI_ADAPTER_DESC1 d2{};
                            if (SUCCEEDED(a2->GetDesc1(&d2)) && d2.VendorId == 0x1002) { nm = utf8(d2.Description); a2->Release(); break; }
                            a2->Release();
                        }
                    }
                    nr::pe::vkdevice::note_d3d12_gpu_name(nm);
                }
                ad->Release();
            }
            fac->Release();
        }
    }
    // The game's direct queue when this thread has handed it frames (as NR's Present layers find it); otherwise a
    // compute queue of ours on the game's device. Never a device of ours.
    ID3D12CommandQueue* queue = nr::pe::split::last_direct_queue();
    if (queue) {
        ID3D12Device* owner = nullptr;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&owner))) || !owner) queue = nullptr;
        else {
            if (owner != device) queue = nullptr;
            owner->Release();
        }
    }
    if (g_canary_started.exchange(true)) return;
    g_canary = int(Canary::kRunning);
    if (queue) {
        queue->AddRef();
    } else {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        const HRESULT hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        if (FAILED(hr) || !queue) { finish_canary(Canary::kFail, "CreateCommandQueue " + hr_text(hr)); return; }
        log("[asd] canary: on a queue of ours (the game's direct queue is not known on this thread)");
    }
    device->AddRef();
    auto* job = new CanaryJob{device, queue};
    if (HANDLE t = CreateThread(nullptr, 0, &canary_thread, job, 0, nullptr)) {
        CloseHandle(t);
    } else {
        queue->Release();
        device->Release();
        delete job;
        finish_canary(Canary::kFail, "no thread");
    }
}

void set_enabled(bool on) {
    if (g_ini.empty()) return;
    WritePrivateProfileStringW(L"D3D12ASD", L"Enabled", on ? L"1" : L"0", g_ini.c_str());
    log("[asd] Enabled = %d written (from the next launch)", on ? 1 : 0);
}

Status status() {
    Status s;
    std::lock_guard<std::mutex> g(g_lock);
    s.enabled = g_enabled;
    s.register_on = true;
    s.files_found = !g_folder.empty();
    s.folder = utf8(g_folder);
    s.registration = g_registration;
    s.canary = g_canary_line;
    s.backend = g_backend;
    s.network = g_network;
    s.runs = g_runs.load();
    s.fallbacks = g_fallbacks.load();
    return s;
}
bool backend_produced() { return g_produced.load(); }
void note_backend_produced(bool produced) { g_produced = produced; }
void note_backend(const char* line, bool ran) {
    (ran ? g_runs : g_fallbacks)++;
    std::lock_guard<std::mutex> g(g_lock);
    if (g_backend != line) {
        g_backend = line;
        log("[asd] backend: %s", line);
    }
}
void note_network(const char* name) {
    std::lock_guard<std::mutex> g(g_lock);
    g_network = name ? name : "";
}

}  // namespace nr::pe::asd
