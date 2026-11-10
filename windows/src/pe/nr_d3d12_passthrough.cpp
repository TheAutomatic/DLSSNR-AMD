// The D3D12 passthrough's switch and canary (nr_d3d12_passthrough.hpp): the switch from dlssnr-amd.ini, the canary on
// the game's own D3D12 device and queue.
#include "nr_d3d12_passthrough.hpp"
#include "nr_d3d12_passthrough_elf.hpp"
#include "nr_gpu_arch.hpp"
#include "nr_pipeline_binary.hpp"
#include "nr_path_u8.hpp"
#include "nr_pe_d3d12split.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_session.hpp"
#include "nr_pe_vkdevice.hpp"
#include <dxgi1_4.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace nr::pe::d3d12_passthrough {
namespace {

using nr::pe::log;

constexpr const char* kSection = "D3D12Passthrough";

std::mutex g_lock;            // the strings below
std::string g_ini;            // dlssnr-amd.ini (UTF-8)
bool g_enabled = false;
std::string g_backend = "not run", g_network, g_adapter;
std::atomic<int> g_canary{int(Canary::kNotRun)};
std::atomic<bool> g_canary_started{false};
std::string g_canary_line = "not run";
std::atomic<bool> g_rdna3{false};
std::atomic<uint32_t> g_gfx11_mach{0};
std::atomic<uint64_t> g_runs{0}, g_fallbacks{0}, g_waits{0};
std::atomic<bool> g_produced{false};   // the last recorded frame actually carried NR (not a copy-through)

std::string hr_text(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(hr));
    return b;
}
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
std::string read_file(const std::string& path) {
    std::string text;
    if (FILE* f = nr::u8::fopen(path.c_str(), "rb")) {
        char buf[4096];
        size_t got;
        while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, got);
        std::fclose(f);
    }
    return text;
}
// [D3D12Passthrough] Enabled as the file says it; `def` when the key is not there (an ini of an older version).
bool read_enabled(const std::string& path, bool def) {
    const std::string text = read_file(path);
    std::string section;
    size_t at = 0;
    while (at < text.size()) {
        size_t eol = text.find('\n', at);
        const std::string line = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        at = eol == std::string::npos ? text.size() : eol + 1;
        const std::string t = trim(line.substr(0, line.find_first_of(";#")));
        if (t.empty()) continue;
        if (t.front() == '[') {
            const auto e = t.find(']');
            section = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1)));
            continue;
        }
        const auto eq = t.find('=');
        if (section == lower(kSection) && eq != std::string::npos && lower(trim(t.substr(0, eq))) == "enabled") {
            const std::string v = trim(t.substr(eq + 1));
            if (v == "1") return true;
            if (v == "0") return false;
            log("[d3d12] dlssnr-amd.ini [%s] Enabled = %s not understood: %d", kSection, v.c_str(), def ? 1 : 0);
            return def;
        }
    }
    return def;
}
// [D3D12Passthrough] Enabled in the file, everything else as it was (the line rewritten in place, or added to its
// section, or the section added at the end) - as NetworkSwitch::write_key does for [Network].
void write_enabled(const std::string& path, bool on) {
    const std::string text = read_file(path), value = on ? "1" : "0";
    const bool crlf = text.find("\r\n") != std::string::npos;
    const std::string nl = crlf ? "\r\n" : "\n";
    std::string out, section;
    bool done = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t eol = text.find('\n', at);
        const std::string line = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at + 1);
        at = eol == std::string::npos ? text.size() : eol + 1;
        const std::string t = trim(line.substr(0, line.find_first_of(";#")));
        if (!t.empty() && t.front() == '[') {
            if (!done && section == lower(kSection)) { out += "Enabled = " + value + nl; done = true; }
            const auto e = t.find(']');
            section = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1)));
        } else if (!done && section == lower(kSection)) {
            const auto eq = t.find('=');
            if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == "enabled") {
                out += "Enabled = " + value + (line.size() && line.back() == '\n' ? nl : "");
                done = true;
                continue;
            }
        }
        out += line;
    }
    if (!done) {
        if (section != lower(kSection)) {
            if (!out.empty() && out.back() != '\n') out += nl;
            out += nl + "[" + kSection + "]" + nl;
        }
        out += "Enabled = " + value + nl;
    }
    if (FILE* f = nr::u8::fopen(path.c_str(), "wb")) {
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
    }
}
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---- the canary on the game's device ------------------------------------------------------------------------------
// A small ACO kernel (elf::kCanaryAcoElf) made into a pipeline the way the network's are, run on a known input and
// read back: the passthrough runs only when the driver took the ELF and the GPU computed exactly what the kernel
// computes.
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

void finish_canary(Canary c, const std::string& line) {
    { std::lock_guard<std::mutex> g(g_lock); g_canary_line = std::string(canary_name(c)) + ": " + line; }
    g_canary = int(c);
    if (c != Canary::kHit) nr::pe::set_d3d12_native(false);   // what can run: as on the bridge (nr_pe_session.cpp)
    log("[d3d12] canary %s: %s%s", canary_name(c), line.c_str(),
        c == Canary::kHit ? " - the D3D12 passthrough may run" : " - the D3D12 passthrough stays off (the Vulkan bridge runs)");
}

// The canary on `device` with `queue`: kHit or kFail, and the line saying why.
Canary run_canary(ID3D12Device* device, ID3D12CommandQueue* queue, std::string* out_line) {
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
        // The kernel's interface: 11 root constants (its user SGPRs) and one root UAV.
        if (!elf::create_carrier_root_signature(device, 11, &rs, &why)) { line = why; break; }
        const uint32_t threads[3] = {64, 1, 1};
        const ULONGLONG t0 = GetTickCount64();
        const bool pso_ok = elf::create_pso_from_elf(device, elf::kCanaryAcoElf, sizeof elf::kCanaryAcoElf, threads, &pso, &why);
        log("[d3d12] canary: CreateComputePipelineFromElf %s in %llu ms", pso_ok ? "0x00000000" : why.c_str(),
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
        uint32_t mismatch = 0, first = ~0u;
        for (uint32_t i = 0; i < kN; ++i)
            if (got[i] != aco_value(i, input[i], push)) { ++mismatch; if (first == ~0u) first = i; }
        char text[160];
        std::snprintf(text, sizeof text, "%u values, %u differ from the ACO kernel's (first difference at %d)", kN,
                      mismatch, first == ~0u ? -1 : int(first));
        line = text;
        result = mismatch == 0 ? Canary::kHit : Canary::kFail;
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
    std::string line;
    const Canary result = run_canary(job.device, job.queue, &line);
    job.queue->Release();
    finish_canary(result, line);
    job.device->Release();
    return 0;
}

// The PCI vendor and device IDs of the adapter with `luid` as the kernel reports them (D3DKMT, gdi32): OptiScaler's
// DXGI spoofing rewrites GetDesc / GetDesc1 of every adapter (TargetVendorId auto = all), so an RDNA3 card reads as
// an NVIDIA one there and RDNA4's canary ran on it (an RX 7800 XT log, 11-08: 128 of 128 differ, passthrough off).
// The kernel thunks are not hooked. False when they cannot be had (the caller keeps what DXGI said).
bool kernel_ids(LUID luid, UINT* vendor, UINT* device_id) {
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi) return false;
    // D3DKMT_OPENADAPTERFROMLUID, _DEVICE_IDS, _QUERY_DEVICE_IDS, _QUERYADAPTERINFO, _CLOSEADAPTER (d3dkmthk.h)
    struct OpenFromLuid { LUID AdapterLuid; UINT hAdapter; };
    struct DeviceIds { UINT VendorID, DeviceID, SubVendorID, SubSystemID, RevisionID, BusType; };
    struct QueryDeviceIds { UINT PhysicalAdapterIndex; DeviceIds Ids; };
    struct QueryInfo { UINT hAdapter; UINT Type; void* pPrivateDriverData; UINT PrivateDriverDataSize; };
    struct CloseAdapter { UINT hAdapter; };
    using Open = LONG(APIENTRY*)(OpenFromLuid*);
    using Query = LONG(APIENTRY*)(const QueryInfo*);
    using Close = LONG(APIENTRY*)(const CloseAdapter*);
    const auto open = reinterpret_cast<Open>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid")));
    const auto query = reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo")));
    const auto close = reinterpret_cast<Close>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTCloseAdapter")));
    if (!open || !query || !close) return false;
    OpenFromLuid o{luid, 0};
    if (open(&o) != 0) return false;
    QueryDeviceIds ids{};
    const QueryInfo q{o.hAdapter, 31u /* KMTQAITYPE_PHYSICALADAPTERDEVICEIDS */, &ids, UINT(sizeof ids)};
    const bool ok = query(&q) == 0 && ids.Ids.VendorID != 0;
    const CloseAdapter c{o.hAdapter};
    close(&c);
    if (ok) { *vendor = ids.Ids.VendorID; *device_id = ids.Ids.DeviceID; }
    return ok;
}

// The adapter behind `device`, its name and architecture: the architecture from the kernel's PCI IDs (kernel_ids:
// OptiScaler's DXGI spoofing cannot change them), else from DXGI. OptiScaler may report the name as another vendor's
// (spoofed as NVIDIA), then the system's AMD adapter is named instead (TheAutomatic's way) - the name is only shown.
nr::arch::GpuArch adapter_arch(ID3D12Device* device, std::string* name) {
    nr::arch::GpuArch arch = nr::arch::GpuArch::unknown;
    IDXGIFactory4* fac = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) && fac) {
        IDXGIAdapter1* ad = nullptr;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(fac->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&ad))) && ad) {
            if (SUCCEEDED(ad->GetDesc1(&desc))) {
                if (desc.VendorId != 0x1002) {
                    for (UINT i = 0;; ++i) {
                        IDXGIAdapter1* a2 = nullptr;
                        if (fac->EnumAdapters1(i, &a2) != S_OK) break;
                        DXGI_ADAPTER_DESC1 d2{};
                        const bool amd = SUCCEEDED(a2->GetDesc1(&d2)) && d2.VendorId == 0x1002;
                        a2->Release();
                        if (amd) { desc = d2; break; }
                    }
                }
                *name = nr::u8::narrow(desc.Description);
                arch = nr::arch::from_device_id(desc.VendorId, desc.DeviceId);
            }
            ad->Release();
        }
        fac->Release();
    }
    UINT vendor = 0, device_id = 0;
    if (kernel_ids(device->GetAdapterLuid(), &vendor, &device_id)) {
        const nr::arch::GpuArch kernel = nr::arch::from_device_id(vendor, device_id);
        if (kernel != arch)
            log("[d3d12] the adapter is PCI %04x:%04x (%s) as the kernel reports it; DXGI said %s", vendor, device_id,
                nr::arch::name(kernel), nr::arch::name(arch));
        arch = kernel;
    }
    if (arch == nr::arch::GpuArch::unknown && (!vendor || vendor == 0x1002) && !name->empty())
        arch = nr::arch::from_name(name->c_str());
    return arch;
}

}  // namespace

const char* canary_name(Canary c) {
    switch (c) {
    case Canary::kHit: return "ok";
    case Canary::kFail: return "failed";
    case Canary::kRunning: return "running";
    default: return "not run";
    }
}

bool enabled() { return g_enabled; }
bool rdna3() { return g_rdna3.load(); }
uint32_t gfx11_mach() { return g_gfx11_mach.load(); }
void set_gfx11_mach(uint32_t mach) { g_gfx11_mach = mach; }
Canary canary() { return Canary(g_canary.load()); }

void start() {
    g_ini = std::string(nr::pe::module_folder()) + "\\dlssnr-amd.ini";
    g_enabled = read_enabled(g_ini, kDefaultEnabled);
    log("[d3d12] D3D12 passthrough %s (dlssnr-amd.ini [%s] Enabled = %d)", g_enabled ? "on" : "off", kSection,
        g_enabled ? 1 : 0);
}

void on_device(ID3D12Device* device) {
    if (!g_enabled || !device || g_canary_started.load()) return;
    std::string name;
    const nr::arch::GpuArch arch = adapter_arch(device, &name);
    if (!name.empty()) nr::pe::vkdevice::note_d3d12_gpu_name(name);
    { std::lock_guard<std::mutex> g(g_lock); g_adapter = name + " (" + nr::arch::name(arch) + ")"; }
    // The game's direct queue when this thread has handed it frames (as NR's Present layers find it); otherwise a
    // queue of ours on the game's device. Never a device of ours.
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
    if (arch == nr::arch::GpuArch::rdna3) {
        // RDNA3: the network runs as the RDNA3 records in gfx11 ELFs; RDNA4's canary is gfx12 machine code, not run here
        g_rdna3 = true;
        std::string why;
        if (!nr::binary::use_rdna3(&why)) {
            finish_canary(Canary::kFail, "RDNA3: " + why);
            return;
        }
        nr::pe::set_d3d12_native(true, true);
        finish_canary(Canary::kHit, "RDNA3 (" + name + "): the passthrough runs the RDNA3 records (no gfx11 canary)");
        return;
    }
    if (arch != nr::arch::GpuArch::rdna4) {
        finish_canary(Canary::kFail, "unsupported or unknown GPU architecture (" + name + ")");
        return;
    }
    // From here NR's network choice is the passthrough's (nr_pe_session.cpp: what can run is not the Vulkan device's),
    // until the check says otherwise (finish_canary). Only here: a game that never evaluates on D3D12 (D3D11 - also
    // with OptiScaler's D3D11-with-D3D12 upscalers, whose D3D12 device is OptiScaler's own -, Vulkan) keeps the bridge's.
    nr::pe::set_d3d12_native(true);
    g_canary = int(Canary::kRunning);
    if (queue) {
        queue->AddRef();
    } else {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        const HRESULT hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        if (FAILED(hr) || !queue) { finish_canary(Canary::kFail, "CreateCommandQueue " + hr_text(hr)); return; }
        log("[d3d12] canary: on a queue of ours (the game's direct queue is not known on this thread)");
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
    write_enabled(g_ini, on);
    log("[d3d12] dlssnr-amd.ini [%s] Enabled = %d written (from the next launch)", kSection, on ? 1 : 0);
}

std::wstring aco_folder() {
    return nr::u8::wide(std::string(nr::pe::module_folder()) + "\\dlssnr-amd\\aco");
}

Status status() {
    Status s;
    std::lock_guard<std::mutex> g(g_lock);
    s.enabled = g_enabled;
    s.adapter = g_adapter;
    s.canary = g_canary_line;
    s.backend = g_backend;
    s.network = g_network;
    s.runs = g_runs.load();
    s.fallbacks = g_fallbacks.load();
    s.waits = g_waits.load();
    return s;
}
bool backend_produced() { return g_produced.load(); }
void note_backend_produced(bool produced) { g_produced = produced; }
void note_backend(const char* line, bool ran, bool waiting) {
    (ran ? g_runs : waiting ? g_waits : g_fallbacks)++;
    std::lock_guard<std::mutex> g(g_lock);
    if (g_backend != line) {
        g_backend = line;
        log("[d3d12] backend: %s", line);
    }
}
void note_network(const char* name) {
    std::lock_guard<std::mutex> g(g_lock);
    g_network = name ? name : "";
}

}  // namespace nr::pe::d3d12_passthrough
