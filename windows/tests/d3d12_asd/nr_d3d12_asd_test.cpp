// A standalone check of the native D3D12 ASD backend (windows/src/pe/nr_d3d12_asd*.cpp) outside any game: its
// registration, the canary and the frame path on a device of this test (a test process; the .asi itself never makes a
// device). Built by windows/build/build_d3d12_asd_test.sh with nr::pe::log and the split helpers stubbed.
//
//   nr_d3d12_asd_test.exe [early|late|unregister|dump-canary <header>]
//     early       register, then make the device: the same launch is expected to hit
//     late        make the device, then register: this launch is expected to miss (the next one hits)
//     dump-canary write nr_d3d12_asd_canary_blob.h from the SODB's g_pbptr (needs the 619 runtime's SODB reader)
//     unregister  remove this exe's registration of ours (and write Register = 0)
// Needs <exe dir>\dlssnr-amd-asd\dlssnr-amd-asd.ini (Enabled = 1) and the SODB and PSDB it names.
#include "nr_d3d12_asd.hpp"
#include "nr_d3d12_asd_backend.hpp"
#include "nr_pe_d3d12split.hpp"

#include <windows.h>
#include <d3d12.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef NR_TEST_AGILITY
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = NR_TEST_AGILITY;
__declspec(dllexport) extern const char* const D3D12SDKPath = ".\\D3D12\\";
}
#endif

namespace nr::pe {
void log(const char* format, ...) {
    va_list a;
    va_start(a, format);
    std::vprintf(format, a);
    va_end(a);
    std::printf("\n");
    std::fflush(stdout);
}
namespace split {
struct Snapshot {};
bool prepare(ID3D12GraphicsCommandList*, std::string*) { return true; }
std::shared_ptr<Snapshot> snapshot(ID3D12GraphicsCommandList*) { return std::make_shared<Snapshot>(); }
void restore(ID3D12GraphicsCommandList*, const std::shared_ptr<Snapshot>&) {}
ID3D12CommandQueue* last_direct_queue() { return nullptr; }
}  // namespace split
}  // namespace nr::pe

namespace {

using nr::pe::log;
namespace asd = nr::pe::asd;

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool wait_for(bool (*done)(), int ms) {
    for (int t = 0; t < ms; t += 20) {
        if (done()) return true;
        Sleep(20);
    }
    return done();
}
bool registration_done() { return asd::status().registration != "not run"; }
bool canary_done() { const auto c = asd::canary(); return c != asd::Canary::kNotRun && c != asd::Canary::kRunning; }

// NR_TEST_DEBUG=1: the D3D12 debug layer on, and its messages (the runtime's shader cache / PSDB lines) printed.
bool debug_on() { const char* e = std::getenv("NR_TEST_DEBUG"); return e && *e == '1'; }
void dump_messages(ID3D12Device* d) {
    ID3D12InfoQueue* q = nullptr;
    if (!d || FAILED(d->QueryInterface(IID_PPV_ARGS(&q))) || !q) { log("[test] debug: no info queue"); return; }
    const UINT64 n = q->GetNumStoredMessages();
    log("[test] debug: %llu message(s)", (unsigned long long)n);
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        q->GetMessage(i, nullptr, &len);
        std::vector<char> b(len);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(b.data());
        if (SUCCEEDED(q->GetMessage(i, m, &len))) log("[test] debug %d/%d: %s", int(m->Severity), int(m->ID), m->pDescription);
    }
    q->Release();
}
ID3D12Device* make_device() {
    if (debug_on()) {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))) && dbg) { dbg->EnableDebugLayer(); dbg->Release(); log("[test] debug layer on"); }
        else log("[test] debug layer not available");
    }
    ID3D12Device* d = nullptr;
    const HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d));
    log("[test] D3D12CreateDevice 0x%08lX", static_cast<unsigned long>(hr));
    return SUCCEEDED(hr) ? d : nullptr;
}

ID3D12Resource* texture(ID3D12Device* d, uint32_t w, uint32_t h, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r));
    return r;
}
ID3D12Resource* buffer(ID3D12Device* d, D3D12_HEAP_TYPE type, uint64_t size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r));
    return r;
}
void barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    l->ResourceBarrier(1, &x);
}

// The frame path on a 61x37 RGBA8 frame (odd, so the 8x8 groups' edges are exercised): separate colour and output,
// then in place (colour == output, UNORDERED_ACCESS). True when the output is the input, texel for texel.
bool frame_path(ID3D12Device* d, bool in_place) {
    const uint32_t w = 61, h = 37, pitch = 256, bytes = pitch * h;
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* a = nullptr;
    ID3D12GraphicsCommandList* l = nullptr;
    ID3D12Fence* f = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    d->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l));
    d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
    ID3D12Resource* color = texture(d, w, h, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* output = in_place ? color : texture(d, w, h, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (in_place) output->AddRef();
    ID3D12Resource* up = buffer(d, D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* rb = buffer(d, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint8_t> src(bytes);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w * 4; ++x) src[y * pitch + x] = uint8_t((x * 7 + y * 13 + x * y) & 0xff);
    void* m = nullptr;
    up->Map(0, nullptr, &m);
    std::memcpy(m, src.data(), bytes);
    up->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION tex{}, lin{};
    tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    lin.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    lin.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, pitch};
    tex.pResource = color;
    lin.pResource = up;
    l->CopyTextureRegion(&tex, 0, 0, 0, &lin, nullptr);
    const D3D12_RESOURCE_STATES cs = in_place ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier(l, color, D3D12_RESOURCE_STATE_COPY_DEST, cs);
    nr::dlssnr::D3d12BackendFrame fr{};
    fr.device = d;
    fr.cmd = l;
    fr.color = color;
    fr.color_state = int(cs);
    fr.output = output;
    fr.output_state = int(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    fr.width = w;
    fr.height = h;
    const bool ran = asd::evaluate_backend(fr);
    barrier(l, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    tex.pResource = output;
    lin.pResource = rb;
    l->CopyTextureRegion(&lin, 0, 0, 0, &tex, nullptr);
    l->Close();
    ID3D12CommandList* ls[] = {l};
    q->ExecuteCommandLists(1, ls);
    q->Signal(f, 1);
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    f->SetEventOnCompletion(1, e);
    const bool done = WaitForSingleObject(e, 5000) == WAIT_OBJECT_0;
    CloseHandle(e);
    const HRESULT removed = d->GetDeviceRemovedReason();
    uint32_t diff = 0;
    if (done) {
        rb->Map(0, nullptr, &m);
        const auto* got = static_cast<const uint8_t*>(m);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w * 4; ++x) diff += got[y * pitch + x] != src[y * pitch + x];
        rb->Unmap(0, nullptr);
    }
    log("[test] frame path %s: backend %s, GPU %s, device 0x%08lX, %u of %u bytes differ", in_place ? "in place" : "colour -> output",
        ran ? "ran" : "declined", done ? "done" : "TIMEOUT", static_cast<unsigned long>(removed), diff, w * h * 4);
    release(rb); release(up); release(output); release(color); release(f); release(l); release(a); release(q);
    return ran && done && removed == S_OK && diff == 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "early";
    log("[test] mode %s, d3d12.dll %s at start", mode.c_str(), GetModuleHandleW(L"d3d12.dll") ? "loaded" : "not loaded");
    if (mode == "dump-canary" && argc > 2) {
        asd::start(GetModuleHandleW(nullptr));
        std::vector<uint8_t> root, cs;
        unsigned flags = 0, node_mask = 0;
        std::string why;
        if (!asd::sodb_pipeline("g_pbptr", &root, &cs, &flags, &node_mask, &why)) { log("[test] dump: %s", why.c_str()); return 1; }
        FILE* f = std::fopen(argv[2], "wb");
        if (!f) return 1;
        auto bytes = [&](const char* name, const std::vector<uint8_t>& v) {
            std::fprintf(f, "inline constexpr unsigned char %s[%zu] = {", name, v.size());
            for (size_t i = 0; i < v.size(); ++i) std::fprintf(f, "%s0x%02x,", i % 24 ? "" : "\n    ", v[i]);
            std::fprintf(f, "\n};\n");
        };
        std::fprintf(f, "#pragma once\n// Generated by nr_d3d12_asd_test dump-canary from the lab's selfreg-c carrier.sodb, key \"g_pbptr\":\n"
                        "// the pbptr carrier's pipeline exactly as stored (serialized root signature, DXIL). PLACEHOLDER (lab canary).\n"
                        "namespace nr::pe::asd::canary_blob {\n");
        bytes("kRoot", root);
        bytes("kCs", cs);
        std::fprintf(f, "inline constexpr unsigned kFlags = %u, kNodeMask = %u;\n}  // namespace nr::pe::asd::canary_blob\n", flags, node_mask);
        std::fclose(f);
        log("[test] dump: root signature %zu bytes, DXIL %zu bytes, flags %u, node mask %u -> %s", root.size(), cs.size(), flags,
            node_mask, argv[2]);
        return 0;
    }
    if (mode == "targets" && argc > 2) {   // targets <exe file name> [app name]: the driver's precompile targets for it
        auto wide = [](const char* a) { const int n = MultiByteToWideChar(CP_ACP, 0, a, -1, nullptr, 0);
            std::wstring w(size_t(n > 0 ? n : 1), L'\0'); MultiByteToWideChar(CP_ACP, 0, a, -1, w.data(), n); w.resize(n > 0 ? n - 1 : 0); return w; };
        log("[targets] %s", asd::precompile_targets(wide(argv[2]), wide(argc > 3 ? argv[3] : "ACO public entry probe")).c_str());
        return 0;
    }
    if ((mode == "system-register" || mode == "system-unregister") && argc > 2) {
        asd::start(GetModuleHandleW(nullptr), false);   // the ini only: no user-scope registration of this tool
        for (int i = 2; i < argc; ++i) {
            const int n = MultiByteToWideChar(CP_ACP, 0, argv[i], -1, nullptr, 0);
            std::wstring w(n, L'\0');
            MultiByteToWideChar(CP_ACP, 0, argv[i], -1, w.data(), n);
            w.resize(n ? n - 1 : 0);
            if (mode == "system-unregister") {
                for (const wchar_t* name : {L"DLSSNR-AMD-ASD", L"DLSSNR-AMD-ASD-FINAL"})
                    log("[reg] %s %s: %s", mode.c_str(), argv[i], asd::system_registration(w, true, name).c_str());
            } else {
                log("[reg] %s %s: %s", mode.c_str(), argv[i],
                    asd::system_registration(w, false, i == 2 ? L"DLSSNR-AMD-ASD" : L"DLSSNR-AMD-ASD-FINAL").c_str());
            }
        }
        return 0;
    }
    if (mode == "unregister") {
        asd::start(GetModuleHandleW(nullptr));
        asd::unregister_async();
        Sleep(200);
        for (int i = 0; i < 100 && asd::status().registration.rfind("unregister", 0) != 0; ++i) Sleep(50);
        log("[test] %s", asd::status().registration.c_str());
        return 0;
    }
    ID3D12Device* device = nullptr;
    if (mode == "late") device = make_device();
    asd::start(GetModuleHandleW(nullptr));
    if (!asd::enabled()) { log("[test] the ini is missing or Enabled = 0"); return 2; }
    if (!wait_for(&registration_done, 15000)) log("[test] the registration did not finish in 15 s");
    if (!device) device = make_device();
    if (!device) return 3;
    {
        wchar_t core[MAX_PATH] = L"";
        if (HMODULE m = GetModuleHandleW(L"D3D12Core.dll")) GetModuleFileNameW(m, core, MAX_PATH);
        char c8[MAX_PATH * 3] = "";
        WideCharToMultiByte(CP_UTF8, 0, core, -1, c8, sizeof c8, nullptr, nullptr);
        log("[test] D3D12Core.dll in use: %s", c8[0] ? c8 : "(none)");
    }
    asd::on_device(device);
    if (!wait_for(&canary_done, 30000)) { log("[test] the canary did not finish in 30 s"); return 4; }
    const asd::Status s = asd::status();
    log("[test] registration: %s", s.registration.c_str());
    log("[test] canary: %s", s.canary.c_str());
    int rc = 0;
    if (asd::canary() == asd::Canary::kHit) {
        const bool a = frame_path(device, false), b = frame_path(device, true);
        rc = a && b ? 0 : 5;
        const asd::Status t = asd::status();
        log("[test] backend: %s; network: %s; %llu runs, %llu fallbacks", t.backend.c_str(), t.network.c_str(),
            (unsigned long long)t.runs, (unsigned long long)t.fallbacks);
    } else {
        nr::dlssnr::D3d12BackendFrame fr{};
        log("[test] no ASD_HIT: the backend must decline (%s)", asd::evaluate_backend(fr) ? "it RAN" : "declined");
        rc = 1;
    }
    // v4: the canary thread's fresh-device diagnostics run on after the game's canary; wait for them before exiting.
    if (!wait_for(&asd::diagnostics_done, 60000)) log("[test] the fresh-device diagnostics did not finish in 60 s");
    if (debug_on()) dump_messages(device);
    device->Release();
    log("[test] RESULT %s %d", asd::canary_name(asd::canary()), rc);
    return rc;
}
