// A standalone check of the native D3D12 ASD backend (windows/src/pe/nr_d3d12_asd*.cpp) outside any game: its
// registration, the canary and the frame path on a device of this test (a test process; the .asi itself never makes a
// device). Built by windows/build/build_d3d12_asd_test.sh with nr::pe::log and the split helpers stubbed.
//
//   nr_d3d12_asd_test.exe [early|late|unregister|dump-canary <header>]
//     early       register, then make the device: the same launch is expected to hit
//     late        make the device, then register: this launch is expected to miss (the next one hits)
//     dump-canary write nr_d3d12_asd_canary_blob.h from the SODB's g_pbptr (needs the 619 runtime's SODB reader)
//     unregister  remove this exe's registration of ours (and write Register = 0)
//     netplan <plan.bin> <m4 run dir>  the ACO network on a dumped plan_d3d12_network plan, temporal frames (net_frames)
// Needs <exe dir>\dlssnr-amd-asd\dlssnr-amd-asd.ini (Enabled = 1) and the SODB and PSDB it names.
#include "nr_d3d12_asd.hpp"
#include "nr_d3d12_asd_backend.hpp"
#include "nr_d3d12_asd_net.hpp"
#include "nr_d3d12_asd_reprofile.hpp"
#include "nr_d3d12_capture.hpp"
#include "nr_pe_config.hpp"
#include "nr_d3d12_net_plan.hpp"
#include "nr_dlssnr_model.hpp"
#include "nr_renodx.hpp"
#include "nr_renodx_d3d12.hpp"
#include <dxgi1_6.h>
#include "nr_pe_d3d12split.hpp"

#include <windows.h>
#include <shellapi.h>
#include <d3d12.h>

#include <algorithm>
#include <cmath>
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
// The backend resolves its shaders and model under module_folder() (in the .asi that is the game's folder); a test
// decides it, so the renodx mode can point at a real installation.
static std::string g_module_folder = ".";
void set_module_folder(const std::string& f) { g_module_folder = f; }
const char* module_folder() { return g_module_folder.c_str(); }
namespace split {
struct Snapshot {};
bool prepare(ID3D12GraphicsCommandList*, std::string*) { return true; }
bool disabled() { return false; }
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

// A motion guide as OptiScaler hands it over: R16G16_FLOAT in NON_PIXEL_SHADER_RESOURCE (zero: no motion).
ID3D12Resource* motion_texture(ID3D12Device* d, uint32_t w, uint32_t h) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16G16_FLOAT;
    rd.SampleDesc.Count = 1;
    ID3D12Resource* r = nullptr;
    d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&r));
    return r;
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
    ID3D12Resource* mv = motion_texture(d, w, h);
    fr.motion = mv;
    fr.motion_scale_x = fr.motion_scale_y = 1.0f;
    fr.feature = 1;
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
    if (done) release(mv);
    return ran && done && removed == S_OK && diff == 0;
}

// netplan <plan.bin> <m4 run dir>: the ACO network through evaluate_backend on the real lowering - nr::plan_d3d12_network
// as the .asi calls it, dumped by plan_dump into plan.bin (NRPLAN02) - with the temporal path on: 24 frames of
// <m4 run dir>\input\tex-f<N>.bin (640x384 RGBA32F, f0 and f1 in turn) with a motion guide (R16G16_FLOAT, a constant
// vector), Reset on frame 0 and frame 4, one feature. A sanity run, not a comparison: every frame must run on the
// network, the GPU finish with the device alive, every output float be finite and the alpha be the input's.
std::string g_plan, g_m4;
bool slurp_file(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->resize(size_t(n > 0 ? n : 0));
    const bool ok = n >= 0 && std::fread(out->data(), 1, out->size(), f) == out->size();
    std::fclose(f);
    return ok;
}
struct PlanReader {
    const std::vector<uint8_t>& b;
    size_t at = 0;
    bool ok = true;
    template <class T> T get() {
        T v{};
        if (at + sizeof v > b.size()) { ok = false; return v; }
        std::memcpy(&v, b.data() + at, sizeof v); at += sizeof v;
        return v;
    }
    std::vector<uint8_t> blob() {
        const uint64_t n = get<uint64_t>();
        if (!ok || at + n > b.size()) { ok = false; return {}; }
        std::vector<uint8_t> v(b.begin() + long(at), b.begin() + long(at + n)); at += n;
        return v;
    }
    std::string str() { const auto v = blob(); return std::string(v.begin(), v.end()); }
};
bool file_planner(uint32_t w, uint32_t h, int, bool, nr::D3d12NetPlan* p, std::string* why) {
    *p = nr::D3d12NetPlan{};
    std::vector<uint8_t> b;
    if (!slurp_file(g_plan, &b) || b.size() < 8 || std::memcmp(b.data(), "NRPLAN02", 8) != 0) { *why = "no NRPLAN02 " + g_plan; return false; }
    PlanReader r{b};
    r.at = 8;
    p->spv_dir = r.str(); p->model = r.str(); p->note = r.str();
    p->width = r.get<uint32_t>(); p->height = r.get<uint32_t>(); p->work_w = r.get<uint32_t>(); p->work_h = r.get<uint32_t>();
    p->style_grade = r.get<uint8_t>() != 0;
    p->act_total = r.get<uint64_t>(); p->weights_end = r.get<uint64_t>();
    p->weights = r.blob();
    const uint32_t nd = r.get<uint32_t>();
    for (uint32_t i = 0; r.ok && i < nd; ++i) {
        nr::D3d12NetPlan::Dispatch d;
        d.kern = r.str(); d.gx = r.get<uint32_t>(); d.gy = r.get<uint32_t>(); d.gz = r.get<uint32_t>();
        d.act_at = r.get<int32_t>(); d.push = r.blob();
        p->disp.push_back(std::move(d));
    }
    const uint32_t nn = r.get<uint32_t>();
    for (uint32_t i = 0; r.ok && i < nn; ++i) p->noise.push_back(r.get<nr::D3d12NetPlan::Noise>());
    auto& t = p->temporal;
    t.pre = r.get<int32_t>(); t.post = r.get<int32_t>();
    t.pre_spv = r.str(); t.post_spv = r.str(); t.post_grade_spv = r.str();
    t.pre_push_bytes = r.get<uint32_t>(); t.seed_at = r.get<uint32_t>(); t.noise_field_at = r.get<uint32_t>();
    t.blend_scale = r.get<float>(); t.motion_without_depth = r.get<uint8_t>() != 0;
    if (!r.ok) { *why = "plan.bin is truncated"; return false; }
    if (w != p->width || h != p->height) { *why = "plan.bin is for " + std::to_string(p->width) + "x" + std::to_string(p->height); return false; }
    return true;
}
ID3D12Resource* texture_of(ID3D12Device* d, uint32_t w, uint32_t h, DXGI_FORMAT format, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Flags = format == DXGI_FORMAT_R32G32B32A32_FLOAT ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = nullptr;
    d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r));
    return r;
}
uint16_t half_of(float f) {   // positive and negative normal values only (the test's motion)
    uint32_t x; std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int e = int((x >> 23) & 0xff) - 127 + 15;
    if (f == 0.0f || e <= 0) return uint16_t(sign);
    return uint16_t(sign | (uint32_t(e) << 10) | ((x >> 13) & 0x3ffu));
}
// v2.3 netstab: the same network, temporal path on, on sequences whose right answer is known to be steady.
//   variant 1: tex-f0 every frame, zero motion, reset on frame 0 only - the output must settle, not alternate
//   variant 2: the same with history 0 (the gate closed) - every frame must be the same bits
//   variant 3: tex-f0 translated 3 px right per frame (wrapping), motion -3 px (the previous position) - the output's
//              frame-to-frame change measured after undoing the shift, inside a border, must stay small
//   variant 5: tex-f0 frames 0-7, tex-f1 from frame 8, zero motion - frame 8's |out - in| shows how much history is blended
int g_variant = 0;
int net_frames(ID3D12Device* d) {
    const uint32_t w = 640, h = 384, pitch = w * 16, bytes = pitch * h;
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
    ID3D12Resource* color = texture_of(d, w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* output = texture_of(d, w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* motion = texture_of(d, w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* up = buffer(d, D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* mup = buffer(d, D3D12_HEAP_TYPE_UPLOAD, uint64_t(w) * 4 * h, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* rb = buffer(d, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 fence = 0;
    auto submit = [&]() {
        ID3D12CommandList* ls[] = {l};
        q->ExecuteCommandLists(1, ls);
        q->Signal(f, ++fence);
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        f->SetEventOnCompletion(fence, e);
        const bool done = WaitForSingleObject(e, 60000) == WAIT_OBJECT_0;
        CloseHandle(e);
        return done;
    };
    {
        // The guide: 2.5 px right, 1.25 px up of a 640x384 motion texture, MVecScale 1 (normalized by the backend's
        // caller: 1 / 640, 1 / 384).
        void* m = nullptr;
        mup->Map(0, nullptr, &m);
        auto* px = static_cast<uint16_t*>(m);
        const float mx = g_variant == 3 ? -3.0f : g_variant ? 0.0f : 2.5f, my = g_variant ? 0.0f : -1.25f;
        for (uint32_t i = 0; i < w * h; ++i) { px[i * 2] = half_of(mx); px[i * 2 + 1] = half_of(my); }
        mup->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION tex{}, lin{};
        tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; tex.pResource = motion;
        lin.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; lin.pResource = mup;
        lin.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16_FLOAT, w, h, 1, w * 4};
        l->CopyTextureRegion(&tex, 0, 0, 0, &lin, nullptr);
        barrier(l, motion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        l->Close();
        submit();
    }
    int rc = 0;
    std::vector<float> previous, before_previous;
    std::vector<uint8_t> base0;
    const int frames = g_variant ? 16 : 24;
    for (int frame = 0; frame < frames && rc == 0; ++frame) {
        std::vector<uint8_t> src;
        const std::string fi = std::to_string(g_variant == 5 ? (frame >= 8 ? 1 : 0) : g_variant ? 0 : frame & 1);
        if (!slurp_file(g_m4 + "\\input\\tex-f" + fi + ".bin", &src) || src.size() != bytes) {
            log("[net] frame %d: the input does not read", frame);
            rc = 10; break;
        }
        if (g_variant == 3) {   // content at x came from x - 3*frame (wrapping)
            if (base0.empty()) base0 = src;
            const uint32_t sh = uint32_t(3 * frame) % w;
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                    std::memcpy(&src[(size_t(y) * w + x) * 16], &base0[(size_t(y) * w + (x + w - sh) % w) * 16], 16);
        }
        bool ran = false, done = false;
        const DWORD t0 = GetTickCount();
        for (int attempt = 0; !ran && GetTickCount() - t0 < 180000; ++attempt) {
            void* m = nullptr;
            up->Map(0, nullptr, &m);
            std::memcpy(m, src.data(), bytes);
            up->Unmap(0, nullptr);
            a->Reset();
            l->Reset(a, nullptr);
            D3D12_TEXTURE_COPY_LOCATION tex{}, lin{};
            tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            lin.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            lin.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, w, h, 1, pitch};
            barrier(l, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            tex.pResource = color; lin.pResource = up;
            l->CopyTextureRegion(&tex, 0, 0, 0, &lin, nullptr);
            barrier(l, color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            nr::dlssnr::D3d12BackendFrame fr{};
            fr.device = d; fr.cmd = l; fr.color = color;
            fr.color_state = int(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            fr.output = output; fr.output_state = int(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            fr.width = w; fr.height = h;
            fr.style = 0; fr.intensity = 1.0f; fr.local_tone = 1.0f; fr.local_structure = 1.0f; fr.skin_structure = -1.0f;
            fr.motion = motion;
            fr.motion_scale_x = 1.0f / float(w); fr.motion_scale_y = 1.0f / float(h);
            fr.reset = frame == 0 || (!g_variant && frame == 4);
            fr.feature = 7;
            fr.history_strength = g_variant == 2 ? 0.0f : -1.0f;
            ran = asd::evaluate_backend(fr);
            if (ran) {
                barrier(l, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                tex.pResource = output; lin.pResource = rb;
                l->CopyTextureRegion(&lin, 0, 0, 0, &tex, nullptr);
                barrier(l, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            l->Close();
            done = submit();
            if (!done) break;
            if (!ran) Sleep(250);
        }
        const HRESULT removed = d->GetDeviceRemovedReason();
        const asd::Status st = asd::status();
        if (!ran || !done || removed != S_OK || st.backend.find("recorded nothing") != std::string::npos) {
            log("[net] frame %d: backend %s, GPU %s, device 0x%08lX; %s", frame, ran ? "ran" : "declined", done ? "done" : "TIMEOUT",
                static_cast<unsigned long>(removed), st.backend.c_str());
            rc = 11; break;
        }
        void* m = nullptr;
        rb->Map(0, nullptr, &m);
        const float* got = static_cast<const float*>(m);
        const float* in = reinterpret_cast<const float*>(src.data());
        uint64_t nonfinite = 0, alpha_bits = 0, same_as_input = 0;
        double sum_in = 0, prev_d = 0;
        for (uint32_t i = 0; i < w * h; ++i) {
            for (int c = 0; c < 3; ++c) {
                const float g = got[i * 4 + c];
                if (!(g == g) || g - g != 0) { ++nonfinite; continue; }
                sum_in += std::fabs(double(g) - double(in[i * 4 + c]));
                if (g == in[i * 4 + c]) ++same_as_input;
                if (!previous.empty()) prev_d += std::fabs(double(g) - double(previous[i * 4 + c]));
            }
            if (std::memcmp(&got[i * 4 + 3], &in[i * 4 + 3], 4) != 0) ++alpha_bits;
        }
        auto log_frame = [&]() {
            log("[net] frame %d (input f%s, reset %d): %llu non-finite, mean |out - in| %.4g (%llu of %u equal), mean |out - previous "
                "out| %.4g, alpha vs the input: %llu differ", frame, fi.c_str(), frame == 0 || frame == 4, (unsigned long long)nonfinite,
                sum_in / (double(w) * h * 3), (unsigned long long)same_as_input, w * h * 3,
                previous.empty() ? 0.0 : prev_d / (double(w) * h * 3), (unsigned long long)alpha_bits);
        };
        if (g_variant) {
            // the steadiness numbers: |out - previous| (after undoing the shift in variant 3, 16 px border left out),
            // |out - the one before the previous| (an alternation shows as this small while the first is large),
            // the same for the input, and the largest single-texel change
            double dp = 0, dpp = 0, din = 0, mx = 0; uint64_t n = 0;
            const int s3 = g_variant == 3 ? 3 : 0;
            if (!previous.empty())
                for (uint32_t y = 16; y < h - 16; ++y)
                    for (uint32_t x = 16; x < w - 16; ++x) {
                        const size_t i = (size_t(y) * w + x) * 4, j = (size_t(y) * w + x - s3) * 4, k = (size_t(y) * w + x - 2 * s3) * 4;
                        for (int c = 0; c < 3; ++c) {
                            const double e = std::fabs(double(got[i + c]) - previous[j + c]);
                            dp += e; mx = std::max(mx, e);
                            if (!before_previous.empty()) dpp += std::fabs(double(got[i + c]) - before_previous[k + c]);
                            ++n;
                        }
                    }
            log("[stab] variant %d frame %2d: mean |out-in| %.5f, mean |out-prev| %.6f, max %.4f, mean |out-prev2| %.6f, %llu non-finite",
                g_variant, frame, sum_in / (double(w) * h * 3), n ? dp / n : 0.0, mx, n && !before_previous.empty() ? dpp / n : 0.0,
                (unsigned long long)nonfinite);
            (void)din;
        } else if (frame < 6 || frame == 23) log_frame();
        before_previous = previous;
        previous.assign(got, got + size_t(w) * h * 4);
        if (g_variant && frame < 6 && std::getenv("NR_TEST_DUMP")) {
            FILE* o = std::fopen(("netstab-v" + std::to_string(g_variant) + std::getenv("NR_TEST_DUMP") + "-f" + std::to_string(frame) + ".bin").c_str(), "wb");
            if (o) { std::fwrite(got, 1, bytes, o); std::fclose(o); }
        }
        if (frame < 6 && !g_variant) {
            FILE* o = std::fopen(("netplan-out-f" + std::to_string(frame) + ".bin").c_str(), "wb");
            if (o) { std::fwrite(got, 1, bytes, o); std::fclose(o); }
        }
        rb->Unmap(0, nullptr);
        if (nonfinite || alpha_bits) rc = 12;
    }
    release(rb); release(mup); release(up); release(motion); release(output); release(color); release(f); release(l); release(a); release(q);
    return rc;
}

}  // namespace


// ---- renodx: the game's own first frame, reproduced offline ------------------------------------------------
// The ASD route as the game runs it: 4K, HDR10 (R10G10B10A2), the frame in place (colour == output, both in
// UNORDERED_ACCESS), the RenoDX stage and the real ACO network with the package's PSDB/carriers. The debug layer
// and DRED come from this exe's own D3D12 runtime, so they match the device - unlike inside the game, where the
// system layers and the app-local Agility ones do not.
//
//   renodx [W H] [game folder]
// Records one frame, closes the list (the HRESULT is the validation verdict), submits, waits, then reports the
// device's state, DRED's breadcrumbs/page faults and the debug layer's messages.

// The harness's own fault printer: WerSvc is off on this machine, so a crash otherwise leaves no trace at all.
static LONG CALLBACK harness_veh(EXCEPTION_POINTERS* ep) {
    const auto* r = ep->ExceptionRecord;
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(r->ExceptionAddress), &m) && m) {
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(m, path, MAX_PATH);
        log("[test] FAULT 0x%08lX at %p in %ls +0x%llX", static_cast<unsigned long>(r->ExceptionCode),
            r->ExceptionAddress, path,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(r->ExceptionAddress) -
                                            reinterpret_cast<uintptr_t>(m)));
    } else {
        log("[test] FAULT 0x%08lX at %p (no module)", static_cast<unsigned long>(r->ExceptionCode), r->ExceptionAddress);
    }
    if (ep->ContextRecord) {   // who called it: a null function pointer is only useful with its caller
        CONTEXT c = *ep->ContextRecord;
        for (int i = 0; i < 12 && c.Rip; ++i) {
            HMODULE mm = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(c.Rip), &mm);
            wchar_t path[MAX_PATH] = L"?";
            if (mm) GetModuleFileNameW(mm, path, MAX_PATH);
            log("[test]   from %ls+0x%llX", path, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(c.Rip) -
                                                                                  reinterpret_cast<uintptr_t>(mm)));
            DWORD64 base = 0;
            if (PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr)) {
                PVOID data = nullptr; DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &data, &frame, nullptr);
            } else {
                DWORD64 ret = 0; SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(c.Rsp), &ret, sizeof ret, &got) ||
                    got != sizeof ret) break;
                c.Rip = ret; c.Rsp += 8;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int renodx_run(int argc, char** argv) {
    const uint32_t W = argc > 2 ? uint32_t(std::atoi(argv[2])) : 3840, H = argc > 3 ? uint32_t(std::atoi(argv[3])) : 2160;
    const std::string game = argc > 4 ? argv[4]
                                           : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\PBZ\\platform\\steam";
    wchar_t here[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, here, MAX_PATH);
    if (wchar_t* slash = std::wcsrchr(here, L'\\')) *slash = 0;
    char here8[MAX_PATH * 3] = "";
    WideCharToMultiByte(CP_UTF8, 0, here, -1, here8, sizeof here8, nullptr, nullptr);
    AddVectoredExceptionHandler(1, &harness_veh);
    nr::pe::capture::start(here8, true, true);   // before any device: the debug layer and DRED apply to devices made after
    asd::start(GetModuleHandleW(nullptr));       // the ini beside this exe (dlssnr-amd-asd\), after the capture: it must
                                                 // not be the one that decides whether the debug layer is on
    nr::pe::configure_network(game);             // ACO Mode and the model, from the installation's dlssnr-amd.ini
    // Registration is what makes the driver use the ACO machine code, and it must be finished before a device exists
    // (in the game the ASI registers at load, before the game's device): let the thread finish first.
    if (!wait_for(&registration_done, 20000)) log("[test] renodx: registration did not finish in 20 s");
    log("[test] renodx: %ux%u, game folder %s", W, H, game.c_str());

    // The network for this installation: the runtime's own lowering (nr_native_plan.cpp).
    static std::string root = game;
    if (argc > 5) {   // a plan the game lowered (NR_DUMP_PLAN): no Vulkan session needed here
        g_plan = argv[5];
        g_m4 = argc > 6 ? argv[6] : "";
        asd::set_net_planner(&file_planner, nullptr);
        log("[test] renodx: plan %s", g_plan.c_str());
    } else {
        auto planner = +[](uint32_t w, uint32_t h, int model, bool experimental, nr::D3d12NetPlan* p, std::string* why) {
            return nr::plan_d3d12_network(root, w, h, experimental, p, why, model);
        };
        asd::set_net_planner(planner, &nr::d3d12_net_controls, nullptr);
    }

    // The device on the game's own adapter.
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* best = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* a = nullptr;
            if (factory->EnumAdapters1(i, &a) != S_OK || !a) break;
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            char name[256] = "";
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name - 1, nullptr, nullptr);
            log("[test] adapter %u: %s (vendor 0x%04X, %llu MB)", i, name, d.VendorId,
                static_cast<unsigned long long>(d.DedicatedVideoMemory >> 20));
            static SIZE_T best_mem = 0;
            if (d.VendorId == 0x1002 && d.DedicatedVideoMemory > best_mem) {
                best_mem = d.DedicatedVideoMemory;
                if (best) best->Release();
                best = a;
                continue;
            }
            a->Release();
        }
    }
    if (!best) { log("[test] renodx: no AMD adapter"); return 2; }
    ID3D12Device* d = nullptr;
    if (debug_on()) {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))) && dbg) { dbg->EnableDebugLayer(); dbg->Release(); }
    }
    log("[test] renodx: creating the device on %p", static_cast<void*>(best));
    const HRESULT hr = D3D12CreateDevice(best, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d));
    log("[test] renodx: D3D12CreateDevice 0x%08lX", static_cast<unsigned long>(hr));
    if (FAILED(hr) || !d) return 3;

    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* a = nullptr;
    ID3D12GraphicsCommandList* l = nullptr;
    ID3D12Fence* f = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    d->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l));
    d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
    uint64_t fence_value = 0;   // one monotonic counter: upload, every frame, every readback

    // The frame: the game's format 24 (R10G10B10A2), in place, in UNORDERED_ACCESS both ways.
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* frame = nullptr;
    const HRESULT hc = d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 nullptr, IID_PPV_ARGS(&frame));
    if (FAILED(hc) || !frame) { log("[test] renodx: the frame could not be made (0x%08lX)", static_cast<unsigned long>(hc)); return 4; }

    if (const char* fc = std::getenv("NR_TEST_FORCE_CANARY"); fc && fc[0] == '1') {
        asd::force_canary_for_test();
        log("[test] renodx: canary forced (test): no driver registration needed");
    }
    nr::pe::set_module_folder(game);
    // A fixed pattern in the frame (x*13+y*7 etc. at 10 bits), so the run has a defined input and its output can be
    // hashed and compared between runs and against another route later.
    {
        const uint64_t pitch = uint64_t(W) * 4;
        D3D12_TEXTURE_COPY_LOCATION dst_loc{}, src_loc{};
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.pResource = frame;
        dst_loc.SubresourceIndex = 0;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.pResource = nullptr;   // set below, once the upload buffer exists
        src_loc.PlacedFootprint.Footprint = {DXGI_FORMAT_R10G10B10A2_UNORM, W, H, 1, UINT(pitch)};
        ID3D12Resource* up = buffer(d, D3D12_HEAP_TYPE_UPLOAD, pitch * H, D3D12_RESOURCE_STATE_GENERIC_READ);
        src_loc.pResource = up;
        void* m = nullptr;
        if (!up || FAILED(up->Map(0, nullptr, &m)) || !m) { log("[test] renodx: the input buffer failed"); return 4; }
        auto* px = static_cast<uint32_t*>(m);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
                px[uint64_t(y) * W + x] = ((x * 13u + y * 7u) & 0x3FF) | (((x * 5u + y * 11u) & 0x3FF) << 10) |
                                          (((x * 3u + y * 17u) & 0x3FF) << 20) | (0x3FFu << 30);
        up->Unmap(0, nullptr);
        D3D12_RESOURCE_BARRIER tb{};
        tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tb.Transition.pResource = frame;
        tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        tb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        l->ResourceBarrier(1, &tb);
        l->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        tb.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        l->ResourceBarrier(1, &tb);
        l->Close();
        ID3D12CommandList* ls[] = {l};
        q->ExecuteCommandLists(1, ls);
        const uint64_t upload_fence = ++fence_value;
        q->Signal(f, upload_fence);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        f->SetEventOnCompletion(upload_fence, ev);
        const bool ok = WaitForSingleObject(ev, 60000) == WAIT_OBJECT_0;
        CloseHandle(ev);
        up->Release();
        if (!ok || f->GetCompletedValue() < upload_fence) { log("[test] renodx: the input copy did not finish"); return 4; }
        a->Reset();
        l->Reset(a, nullptr);
    }
    asd::on_device(d);
    if (!wait_for(&canary_done, 30000)) { log("[test] renodx: the canary did not finish"); return 5; }
    log("[test] renodx: canary %s", asd::status().canary.c_str());

    nr::renodx::Settings settings{};   // the shipped defaults: 1 pass, Standard, intensity 1
    if (const char* off = std::getenv("NR_TEST_NR_OFF"); off && off[0] == '1') {
        settings.nr_enabled = false;   // the copy path only: separates the stage's work from the host's own copies
        log("[test] renodx: NR disabled (copy path only)");
    }
    nr::dlssnr::D3d12BackendFrame fr{};
    fr.device = d; fr.cmd = l;
    // NR_TEST_SEPARATE_OUTPUT=1: a second texture as the output (copy_frame's two-barrier path, and a store into an
    // output that is not the colour).
    ID3D12Resource* out_tex = frame;
    if (const char* sep = std::getenv("NR_TEST_SEPARATE_OUTPUT"); sep && sep[0] == '1') {
        D3D12_HEAP_PROPERTIES ohp{}; ohp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC ord{};
        ord.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ord.Width = W; ord.Height = H; ord.DepthOrArraySize = 1; ord.MipLevels = 1;
        ord.Format = DXGI_FORMAT_R10G10B10A2_UNORM; ord.SampleDesc.Count = 1;
        ord.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(d->CreateCommittedResource(&ohp, D3D12_HEAP_FLAG_NONE, &ord, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                              nullptr, IID_PPV_ARGS(&out_tex))) || !out_tex) {
            log("[test] renodx: the separate output could not be made");
            return 4;
        }
        log("[test] renodx: separate output");
    }
    fr.color = frame; fr.color_state = int(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    fr.output = out_tex; fr.output_state = int(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // NR_TEST_GUIDES=1: motion and depth with fixed patterns, so the temporal path (history gate, latch) runs.
    if (const char* g = std::getenv("NR_TEST_GUIDES"); g && g[0] == '1') {
        const bool zero_guides = [] {
            const char* z = std::getenv("NR_TEST_ZERO_GUIDES");
            return z && z[0] == '1';
        }();
        auto make_guide = [&](DXGI_FORMAT fmt, uint32_t bpp, uint8_t fill_a, uint8_t fill_b) -> ID3D12Resource* {
            D3D12_HEAP_PROPERTIES gp{}; gp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC gr{};
            gr.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            gr.Width = W; gr.Height = H; gr.DepthOrArraySize = 1; gr.MipLevels = 1;
            gr.Format = fmt; gr.SampleDesc.Count = 1;
            ID3D12Resource* t = nullptr;
            if (FAILED(d->CreateCommittedResource(&gp, D3D12_HEAP_FLAG_NONE, &gr, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&t))) || !t) return nullptr;
            const uint64_t pitch = uint64_t(W) * bpp;
            ID3D12Resource* up = buffer(d, D3D12_HEAP_TYPE_UPLOAD, pitch * H, D3D12_RESOURCE_STATE_GENERIC_READ);
            void* m = nullptr;
            if (!up || FAILED(up->Map(0, nullptr, &m)) || !m) return nullptr;
            auto* p8 = static_cast<uint8_t*>(m);
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x) {
                    uint8_t* px = p8 + uint64_t(y) * pitch + uint64_t(x) * bpp;
                    px[0] = fill_a; px[1] = fill_b;
                    for (uint32_t k = 2; k < bpp; ++k) px[k] = zero_guides ? 0 : uint8_t((x * 3u + y * 5u) & 0xFF);
                }
            up->Unmap(0, nullptr);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.pResource = t; dst.SubresourceIndex = 0;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.pResource = up;
            src.PlacedFootprint.Footprint = {fmt, W, H, 1, UINT(pitch)};
            D3D12_RESOURCE_BARRIER tb{};
            tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            tb.Transition.pResource = t;
            tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            tb.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            l->ResourceBarrier(1, &tb);
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            l->ResourceBarrier(1, &tb);
            return t;
        };
        // motion: 1.5 px right, 0.75 px up in half floats; depth: a fixed gradient in floats
        auto half_of = [](float v) { uint32_t f; std::memcpy(&f, &v, 4); uint32_t s = (f >> 16) & 0x8000;
            int32_t e = int32_t((f >> 23) & 0xFF) - 127 + 15; uint32_t mnt = (f >> 13) & 0x3FF;
            if (e <= 0) return uint16_t(s); if (e >= 31) return uint16_t(s | 0x7C00);
            return uint16_t(s | (uint32_t(e) << 10) | mnt); };
        (void)half_of;
        ID3D12Resource* mv = make_guide(DXGI_FORMAT_R16G16_FLOAT, 4, 0, 0);
        ID3D12Resource* dp = make_guide(DXGI_FORMAT_R32_FLOAT, 4, 0, 0);
        if (!mv || !dp) { log("[test] renodx: the guides could not be made"); return 4; }
        fr.motion = mv;   // the backend takes the guides in NON_PIXEL_SHADER_RESOURCE
        fr.motion_x = 0; fr.motion_y = 0; fr.motion_w = W; fr.motion_h = H;
        fr.motion_scale_x = 1.0f / float(W); fr.motion_scale_y = 1.0f / float(H);
        fr.depth = dp;
        fr.depth_x = 0; fr.depth_y = 0; fr.depth_w = W; fr.depth_h = H;
        log("[test] renodx: guides (motion %ux%u, depth %ux%u)", W, H, W, H);
        // submit the guide uploads now
        const HRESULT gc = l->Close();
        if (FAILED(gc)) { log("[test] renodx: guides Close 0x%08lX", static_cast<unsigned long>(gc)); return 4; }
        ID3D12CommandList* gl[] = {l};
        q->ExecuteCommandLists(1, gl);
        const uint64_t gfence = ++fence_value;
        q->Signal(f, gfence);
        HANDLE gev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        f->SetEventOnCompletion(gfence, gev);
        const bool gok = WaitForSingleObject(gev, 60000) == WAIT_OBJECT_0;
        CloseHandle(gev);
        if (!gok || f->GetCompletedValue() < gfence) { log("[test] renodx: the guides did not finish"); return 4; }
        a->Reset();
        l->Reset(a, nullptr);
    }
    fr.width = W; fr.height = H;
    fr.style = 0; fr.intensity = 1.f; fr.local_tone = 1.f; fr.local_structure = 1.f; fr.skin_structure = -1.f;
    fr.automatic_mask = 0; fr.reversible_mode = 0;
    // NR_TEST_NOISE_EVERY_FRAME=0: the same noise rule as the reference host's Controls, for boundary comparisons.
    fr.noise_every_frame = [] {
        const char* n = std::getenv("NR_TEST_NOISE_EVERY_FRAME");
        return n && n[0] == '0' ? 0 : 1;
    }();
    fr.motion_scale_x = 1.f / float(W); fr.motion_scale_y = 1.f / float(H);
    fr.history_strength = -1.f;
    fr.feature = 1;
    fr.renodx_route = 1;
    fr.renodx_settings = &settings;
    // The network's input and answer, for a comparison with another route's boundary values.
    ID3D12Resource* cap_in = nullptr;
    ID3D12Resource* cap_out = nullptr;
    ID3D12Resource* cap_raw = nullptr;
    const bool capture_boundaries = std::getenv("NR_TEST_STAGE_DUMP") != nullptr;
    if (capture_boundaries) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = uint64_t(W) * H * 8; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&cap_in))) ||
            FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&cap_out)))) {
            log("[test] renodx: the boundary capture buffers could not be made");
            return 4;
        }
        rd.Width = uint64_t(W) * H * 16;
        if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&cap_raw)))) {
            log("[test] renodx: the raw capture buffer could not be made");
            return 4;
        }
        fr.capture_in = cap_in;
        fr.capture_out = cap_out;
        fr.capture_raw = cap_raw;
    }
    fr.renodx_present = 2;         // HDR10
    fr.renodx_white_nits = 203.f;
    fr.renodx_hdr_flag = 1;
    const int frames = argc > 6 ? std::max(1, std::atoi(argv[6])) : 1;
    int waiting = 0;
    fr.waiting = &waiting;
    bool ran = false;
    int attempt = 0;
    for (; attempt < 600 && !ran; ++attempt) {
        ran = asd::evaluate_backend(fr);
        if (!ran) {
            const std::string st = asd::status().backend;
            if (attempt % 25 == 0) log("[test] renodx: waiting for the network (%s)", st.c_str());
            // Nothing about these fixes itself by waiting: the canary verdict and a broken backend are final.
            if (st.find("canary ASD_MISS") != std::string::npos || st.find("ACO Mode is off") != std::string::npos)
                { log("[test] renodx: the network cannot run here (%s); stopping", st.c_str()); return 7; }
            Sleep(200);
        }
    }
    log("[test] renodx: evaluate -> %d (%s)", ran ? 1 : 0, asd::status().backend.c_str());
    if (!ran) return 7;

    // The frames after the first are what exercise the history, the latch and the reset paths.
    for (int fi = 0; fi < frames; ++fi) {
        if (fi > 0) {
            const HRESULT ra = a->Reset();
            if (FAILED(ra)) { log("[test] renodx: allocator Reset 0x%08lX", static_cast<unsigned long>(ra)); return 8; }
            const HRESULT rl = l->Reset(a, nullptr);
            if (FAILED(rl)) { log("[test] renodx: list Reset 0x%08lX", static_cast<unsigned long>(rl)); return 8; }
            fr.reset = false;
            ran = false;
            for (int attempt = 0; attempt < 600 && !ran; ++attempt) ran = asd::evaluate_backend(fr);
            if (!ran) { log("[test] renodx: frame %d: not run (%s)", fi, asd::status().backend.c_str()); return 7; }
        }
        const HRESULT closed = l->Close();
        log("[test] renodx: frame %d Close 0x%08lX", fi, static_cast<unsigned long>(closed));
        if (FAILED(closed)) { log("[test] renodx: FAILED - Close rejected the list"); return 8; }
        ID3D12CommandList* lists[] = {l};
        q->ExecuteCommandLists(1, lists);
        const HRESULT hs = q->Signal(f, ++fence_value);
        if (FAILED(hs)) { log("[test] renodx: Signal 0x%08lX", static_cast<unsigned long>(hs)); return 8; }
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const HRESULT hw = f->SetEventOnCompletion(fence_value, e);
        if (FAILED(hw)) { CloseHandle(e); log("[test] renodx: SetEventOnCompletion 0x%08lX", static_cast<unsigned long>(hw)); return 8; }
        const bool done = WaitForSingleObject(e, 60000) == WAIT_OBJECT_0;
        CloseHandle(e);
        if (!done || f->GetCompletedValue() < fence_value) {
            log("[test] renodx: frame %d: the GPU did not finish in 60 s (completed %llu of %llu)", fi,
                static_cast<unsigned long long>(f->GetCompletedValue()), static_cast<unsigned long long>(fence_value));
            return 9;
        }
        const HRESULT removed = d->GetDeviceRemovedReason();
        log("[test] renodx: frame %d submitted, fence %s, device reason 0x%08lX", fi,
            done ? "signalled" : "TIMED OUT", static_cast<unsigned long>(removed));
        // What the frame became: the same input and route must give the same bytes run to run.
        {
            if (a->Reset() == S_OK && l->Reset(a, nullptr) == S_OK) {
                const uint64_t pitch = uint64_t(W) * 4;
                ID3D12Resource* rb = buffer(d, D3D12_HEAP_TYPE_READBACK, pitch * H, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.pResource = rb;
                dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R10G10B10A2_UNORM, W, H, 1, UINT(pitch)};
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.pResource = out_tex; src.SubresourceIndex = 0;
                D3D12_RESOURCE_BARRIER tb{};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource = out_tex;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                tb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                tb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                l->ResourceBarrier(1, &tb);
                l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                tb.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                l->ResourceBarrier(1, &tb);
                const HRESULT rc = l->Close();
                if (SUCCEEDED(rc)) {
                    ID3D12CommandList* ls[] = {l};
                    q->ExecuteCommandLists(1, ls);
                    q->Signal(f, ++fence_value);
                    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    if (SUCCEEDED(f->SetEventOnCompletion(fence_value, ev)) &&
                        WaitForSingleObject(ev, 60000) == WAIT_OBJECT_0 && f->GetCompletedValue() >= fence_value) {
                        void* m = nullptr;
                        if (rb && SUCCEEDED(rb->Map(0, nullptr, &m)) && m) {
                            const auto* p8 = static_cast<const uint8_t*>(m);
                            uint64_t hh = 1469598103934665603ull;
                            for (uint64_t i = 0; i < pitch * H; ++i) { hh ^= p8[i]; hh *= 1099511628211ull; }
                            log("[test] renodx: frame %d output hash %016llX", fi, static_cast<unsigned long long>(hh));
                            // The network's own boundary capture (the precomputed noise field), written once the GPU
                            // has finished the frame it was recorded in.
                            if (fi == 0 && std::getenv("NR_NET_NOISE_DUMP")) {
                                std::string cw;
                                const bool wrote = nr::pe::asd::write_network_capture(
                                    std::getenv("NR_NET_NOISE_DUMP"), &cw);
                                log("[test] renodx: network capture %s (%s)", wrote ? "written" : "FAILED", cw.c_str());
                            }
                            // NR_TEST_DUMP=<dir>: each frame's raw output (R10G10B10A2, w*h*4 B) for a numeric
                            // comparison between two loads of the same frame (HLSL vs the ACO binary).
                            if (capture_boundaries) {
                                void* mi = nullptr;
                                void* mn = nullptr;
                                const uint64_t cb = uint64_t(W) * H * 8;
                                if (cap_in && SUCCEEDED(cap_in->Map(0, nullptr, &mi)) && mi &&
                                    cap_out && SUCCEEDED(cap_out->Map(0, nullptr, &mn)) && mn) {
                                    if (const char* sd = std::getenv("NR_TEST_STAGE_DUMP"); sd && sd[0]) {
                                        void* mr = nullptr;
                                        if (cap_raw && SUCCEEDED(cap_raw->Map(0, nullptr, &mr)) && mr) {
                                            char p3[512];
                                            std::snprintf(p3, sizeof p3, "%s\\raw_frame%d.bin", sd, fi);
                                            if (FILE* f = std::fopen(p3, "wb")) {
                                                std::fwrite(mr, 1, size_t(uint64_t(W) * H * 16), f);
                                                std::fclose(f);
                                            }
                                            cap_raw->Unmap(0, nullptr);
                                        }
                                        char p1[512], p2[512];
                                        std::snprintf(p1, sizeof p1, "%s\\proxy_frame%d.bin", sd, fi);
                                        std::snprintf(p2, sizeof p2, "%s\\neural_frame%d.bin", sd, fi);
                                        if (FILE* f = std::fopen(p1, "wb")) { std::fwrite(mi, 1, size_t(cb), f); std::fclose(f); }
                                        if (FILE* f = std::fopen(p2, "wb")) { std::fwrite(mn, 1, size_t(cb), f); std::fclose(f); }
                                        log("[test] renodx: boundary dumped (proxy %s, neural %s)", p1, p2);
                                    }
                                    cap_in->Unmap(0, nullptr);
                                    cap_out->Unmap(0, nullptr);
                                }
                            }
                            if (const char* dd = std::getenv("NR_TEST_DUMP"); dd && dd[0]) {
                                char path[512];
                                std::snprintf(path, sizeof path, "%s\\frame%d.bin", dd, fi);
                                if (FILE* out = std::fopen(path, "wb")) {
                                    std::fwrite(p8, 1, size_t(pitch * H), out);
                                    std::fclose(out);
                                    log("[test] renodx: frame %d output dumped to %s", fi, path);
                                }
                            }
                            rb->Unmap(0, nullptr);
                        }
                    }
                    CloseHandle(ev);
                }
                if (rb) rb->Release();
                // Leave the list closed, not opened: the next frame resets the allocator first, and an allocator
                // cannot be reset while an open list still refers to it.
                l->Close();
            }
        }
        nr::pe::capture::drain();
        if (FAILED(removed)) {
            nr::pe::capture::device_removed(removed);
            log("[test] renodx: FAILED at frame %d - see the diagnostic folder", fi);
            return 6;
        }
    }
    log("[test] renodx: %d frame(s) completed with no device error", frames);
    return 0;
}


// ---- stage: the RenoDX D3D12 stage alone, under validation -----------------------------------------------
// No ACO network (its canary and the debug layer/DRED cannot both be in play: the layer makes the driver ignore
// the precompiled pipelines the canary compares). The stage's own commands are what this exercises: 4K, format 24,
// in place, UAV, the shipped settings. The net callback copies in->out so the pass still runs end to end.
//
//   stage [W H] [game folder]
//
// Measured here (2026-11-02): the stage alone removes the device with 0x887A0001 - Close() returns 0x00000000 and
// the fence signals, so the list is accepted when it is recorded and the device is lost at or after execution. The
// same with the debug layer and GPU-based validation on (empty info queue) and with both off, so the failure is not
// an artefact of the validation settings and it needs no ACO network to reproduce.
// Next step is inside this mode: nr_enabled=false (copies only), then the passes one at a time, comparing the
// device's reason after each.
std::wstring widen(const char* s) {
    if (!s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(size_t(n ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}
int stage_run(int argc, char** argv) {
    const uint32_t W = argc > 2 ? uint32_t(std::atoi(argv[2])) : 3840, H = argc > 3 ? uint32_t(std::atoi(argv[3])) : 2160;
    const std::wstring game = argc > 4 ? widen(argv[4])
                                       : L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\PBZ\\platform\\steam";
    wchar_t here[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, here, MAX_PATH);
    if (wchar_t* slash = std::wcsrchr(here, L'\\')) *slash = 0;
    char here8[MAX_PATH * 3] = "";
    WideCharToMultiByte(CP_UTF8, 0, here, -1, here8, sizeof here8, nullptr, nullptr);
    AddVectoredExceptionHandler(1, &harness_veh);
    nr::pe::capture::start(here8, true, true);   // debug layer + DRED, before the device

    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* best = nullptr;
    SIZE_T best_mem = 0;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* a = nullptr;
            if (factory->EnumAdapters1(i, &a) != S_OK || !a) break;
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (d.VendorId == 0x1002 && d.DedicatedVideoMemory > best_mem) {
                best_mem = d.DedicatedVideoMemory;
                if (best) best->Release();
                best = a;
                continue;
            }
            a->Release();
        }
    }
    if (!best) { log("[test] stage: no AMD adapter"); return 2; }
    ID3D12Device* d = nullptr;
    const HRESULT hr = D3D12CreateDevice(best, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d));
    log("[test] stage: D3D12CreateDevice 0x%08lX", static_cast<unsigned long>(hr));
    if (FAILED(hr) || !d) return 3;
    // The capture attaches its info queue and DRED to the device here: without the backend there is no other caller.
    nr::pe::capture::adopt(d);

    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* a = nullptr;
    ID3D12GraphicsCommandList* l = nullptr;
    ID3D12Fence* f = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    d->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l));
    d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R10G10B10A2_UNORM; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* frame_tex = nullptr;
    if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                          IID_PPV_ARGS(&frame_tex))) || !frame_tex) { log("[test] stage: no frame texture"); return 4; }

    nr::pe::asd::renodx12::Stage::Config cfg{};
    cfg.device = d;
    cfg.shaders = game + L"\\dlssnr-amd\\shaders\\renodx";
    cfg.width = W; cfg.height = H; cfg.nr_width = W; cfg.nr_height = H;
    cfg.frame_format = DXGI_FORMAT_R10G10B10A2_UNORM;
    cfg.hdr_flag = true;
    cfg.present = 2;
    cfg.settings = nr::renodx::Settings{};
    cfg.feature = 1;
    std::string why;
    auto stage = nr::pe::asd::renodx12::Stage::make(cfg, &why);
    log("[test] stage: make -> %s (%s)", stage ? "ok" : "FAILED", why.c_str());
    if (!stage) return 5;

    nr::pe::asd::renodx12::Frame fr{};
    fr.colour = frame_tex; fr.colour_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    fr.output = frame_tex; fr.output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    fr.after_upscale = true;
    fr.base_controls = nr::D3d12NetControls{};
    fr.motion_without_depth = true;
    fr.present_white_nits = 203.f;
    auto net = [](uint32_t, uint64_t, const nr::D3d12NetControls&, const nr::pe::asd::NetTemporal&, ID3D12Resource* in,
                  ID3D12Resource* out) {
        return in != nullptr && out != nullptr;   // the stage's own commands are what is under test
    };
    const uint32_t passes = stage->record(l, fr, net);
    log("[test] stage: record -> %u pass(es)", passes);

    const HRESULT closed = l->Close();
    log("[test] stage: Close 0x%08lX", static_cast<unsigned long>(closed));
    ID3D12CommandList* lists[] = {l};
    q->ExecuteCommandLists(1, lists);
    q->Signal(f, 1);
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    f->SetEventOnCompletion(1, e);
    const bool done = WaitForSingleObject(e, 60000) == WAIT_OBJECT_0;
    CloseHandle(e);
    const HRESULT removed = d->GetDeviceRemovedReason();
    log("[test] stage: submitted, fence %s, device reason 0x%08lX", done ? "signalled" : "TIMED OUT",
        static_cast<unsigned long>(removed));
    nr::pe::capture::drain();
    if (FAILED(removed)) { nr::pe::capture::device_removed(removed); return 6; }
    log("[test] stage: no device error");
    return 0;
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "early";
        auto wide = [](const char* a) { const int n = MultiByteToWideChar(CP_ACP, 0, a, -1, nullptr, 0);
            std::wstring w(size_t(n > 0 ? n : 1), L'\0'); MultiByteToWideChar(CP_ACP, 0, a, -1, w.data(), n); w.resize(n > 0 ? n - 1 : 0); return w; };
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
    if (mode == "stage") return stage_run(argc, argv);
    if (mode == "renodx") return renodx_run(argc, argv);
    if (mode == "targets" && argc > 2) {   // targets <exe file name> [app name]: the driver's precompile targets for it
        log("[targets] %s", asd::precompile_targets(wide(argv[2]), wide(argc > 3 ? argv[3] : "ACO public entry probe")).c_str());
        return 0;
    }
    // reprofile <D3D12StateObjectCompiler.dll> <plugin dll> <source psdb> <sodb> <out psdb> <exe file name> [groups,]
    //   (no groups: g_pbptr + m2-<tag> for every record of net-carriers.bin beside the source PSDB - the real network)
    //   (profile <D3D12StateObjectCompiler.dll> <plugin dll> <exe file name>: only the profile the plugin gives it)
    if ((mode == "reprofile" && argc > 7) || (mode == "profile" && argc > 4)) {
        // The arguments as UTF-16 (paths outside the ANSI code page, e.g. a Chinese folder name).
        int wargc = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        auto warg = [&](int i) { return wargv && i < wargc ? std::wstring(wargv[i]) : wide(argv[i]); };
        asd::ReprofileJob job;
        job.compiler_dll = warg(2); job.plugin_dll = warg(3); job.family = L"AMD_NAVI48:gfx1201";
        job.name = L"ACO public entry probe"; job.version = 1; job.engine = L"ACO lab"; job.engine_version = 1;
        job.src_exe = L"asd_probe.exe";
        if (mode == "profile") {
            job.exe = warg(4);
            log("[reprofile] %s", asd::reprofile_psdb(job, true).text.c_str());
            return 0;
        }
        job.src_psdb = warg(4); job.sodb = warg(5); job.out_psdb = warg(6); job.exe = warg(7);
        std::string groups = argc > 8 ? argv[8] : "";   // none: every group of ours (net-carriers.bin beside the source PSDB)
        for (size_t at = 0; at <= groups.size();) {
            size_t e = groups.find(',', at);
            if (e == std::string::npos) e = groups.size();
            if (e > at) job.groups.push_back(groups.substr(at, e - at));
            at = e + 1;
        }
        const auto r = asd::reprofile_psdb(job);
        log("[reprofile] %s: %s", r.ok ? "OK" : "FAILED", r.text.c_str());
        return r.ok ? 0 : 1;
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
    const bool netstab = mode == "netstab";
    if (netstab) { g_variant = argc > 4 ? std::atoi(argv[4]) : 1; }
    if (mode == "netplan" || netstab) {
        if (argc < 4) { log("[test] netplan <plan.bin> <m4 run dir>"); return 2; }
        g_plan = argv[2]; g_m4 = argv[3];
        asd::set_net_planner(&file_planner, nullptr);   // the dumped push blocks as they are: no controls patch
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
        if (mode == "netplan" || netstab) {
            rc = net_frames(device);
        } else {
            const bool a = frame_path(device, false), b = frame_path(device, true);
            rc = a && b ? 0 : 5;
        }
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
