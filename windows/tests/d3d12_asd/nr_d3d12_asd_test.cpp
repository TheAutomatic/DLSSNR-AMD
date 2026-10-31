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
bool file_planner(uint32_t w, uint32_t h, bool, nr::D3d12NetPlan* p, std::string* why) {
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
    if (mode == "targets" && argc > 2) {   // targets <exe file name> [app name]: the driver's precompile targets for it
        log("[targets] %s", asd::precompile_targets(wide(argv[2]), wide(argc > 3 ? argv[3] : "ACO public entry probe")).c_str());
        return 0;
    }
    // reprofile <D3D12StateObjectCompiler.dll> <plugin dll> <source psdb> <sodb> <out psdb> <exe file name> [groups,]
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
        std::string groups = argc > 8 ? argv[8] : "g_pbptr";
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
