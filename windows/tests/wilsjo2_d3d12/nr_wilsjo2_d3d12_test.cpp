// windows/tests/wilsjo2_d3d12: the wilsjo2 stage on the native D3D12 route (nr_asi_wilsjo2_d3d12.hpp, M5 v2) outside a
// game. The real flow (nr_wilsjo2_flow.hpp flow_run) on a D3D12 device with the shipped DXBC, the network replaced by
// an identity (its input copied to its answer by the stage's own copy program). The reference: the same flow on the
// Vulkan stage (the 0.0.8.1 route's, the shipped .spv) on the same GPU (vk_reference.hpp); the D3D12 output must match it.
// With nothing changed by the network the layer also mostly gives the frame back (shown). Also the HDR10 conversion
// there and back, against the Vulkan stage's.
//   nr_wilsjo2_d3d12_test.exe <folder with wilsjo2_<name>.dxbc> [<folder with wilsjo2_<name>.spv>]
#include <cstdarg>
#include <cstdio>
static void test_log(const char* f, ...) {
    va_list a;
    va_start(a, f);
    std::vfprintf(stdout, f, a);
    va_end(a);
    std::fputc('\n', stdout);
}
#define NR_W2D3D12_LOG test_log
#include "nr_asi_wilsjo2_d3d12.hpp"
#include "nr_wilsjo2_flow.hpp"

#include <cmath>
#include <memory>

namespace w2 = nr::wilsjo2;
using nr::asi::native::Stage12;

namespace {
ID3D12Device* g_dev = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
ID3D12CommandAllocator* g_alloc = nullptr;
ID3D12GraphicsCommandList* g_cmd = nullptr;
ID3D12Fence* g_fence = nullptr;
UINT64 g_fv = 0;
int g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
void transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (a == b) return;
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    g_cmd->ResourceBarrier(1, &x);
}
bool submit() {
    g_cmd->Close();
    ID3D12CommandList* l[] = {g_cmd};
    g_queue->ExecuteCommandLists(1, l);
    g_queue->Signal(g_fence, ++g_fv);
    HANDLE e = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_fence->SetEventOnCompletion(g_fv, e);
    const DWORD w = WaitForSingleObject(e, 20000);
    CloseHandle(e);
    const HRESULT removed = g_dev->GetDeviceRemovedReason();
    g_alloc->Reset();
    g_cmd->Reset(g_alloc, nullptr);
    if (w != WAIT_OBJECT_0 || FAILED(removed)) {
        std::printf("the GPU work did not finish (wait %lu, removed 0x%08lX)\n", w, removed);
        return false;
    }
    return true;
}
ID3D12Resource* texture(DXGI_FORMAT f, uint32_t w, uint32_t h, D3D12_RESOURCE_STATES s, bool uav = true) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = f; d.SampleDesc.Count = 1;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = nullptr;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, s, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}
ID3D12Resource* buffer(uint64_t bytes, D3D12_HEAP_TYPE t, D3D12_RESOURCE_STATES s) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = t;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, s, nullptr, IID_PPV_ARGS(&r));
    return r;
}
uint16_t half_of(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t s = (x >> 16) & 0x8000;
    int e = int((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffff;
    if (e <= 0) return uint16_t(s);
    if (e >= 31) return uint16_t(s | 0x7c00);
    return uint16_t(s | (e << 10) | ((m + 0x1000) >> 13));
}
float float_of(uint16_t h) {
    const uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    uint32_t x;
    if (e == 0) x = s;   // (no denormals in these tests)
    else if (e == 31) x = s | 0x7f800000u | (m << 13);
    else x = s | ((e - 15 + 127) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}
// RGBA pixels as floats, in and out of R16G16B16A16_FLOAT / R10G10B10A2_UNORM / R16G16_FLOAT / R32_FLOAT
size_t texel_bytes(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
}
void pack(DXGI_FORMAT f, const float* p, uint8_t* o) {
    if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        uint16_t h[4] = {half_of(p[0]), half_of(p[1]), half_of(p[2]), half_of(p[3])};
        std::memcpy(o, h, 8);
    } else if (f == DXGI_FORMAT_R10G10B10A2_UNORM) {
        auto q = [](float v, float m) { return uint32_t(std::lround(std::clamp(v, 0.f, 1.f) * m)); };
        const uint32_t x = q(p[0], 1023) | (q(p[1], 1023) << 10) | (q(p[2], 1023) << 20) | (q(p[3], 3) << 30);
        std::memcpy(o, &x, 4);
    } else if (f == DXGI_FORMAT_R16G16_FLOAT) {
        uint16_t h[2] = {half_of(p[0]), half_of(p[1])};
        std::memcpy(o, h, 4);
    } else {
        std::memcpy(o, p, 4);
    }
}
void unpack(DXGI_FORMAT f, const uint8_t* i, float* p) {
    if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        uint16_t h[4];
        std::memcpy(h, i, 8);
        for (int k = 0; k < 4; ++k) p[k] = float_of(h[k]);
    } else {
        uint32_t x;
        std::memcpy(&x, i, 4);
        p[0] = float(x & 1023) / 1023.f; p[1] = float((x >> 10) & 1023) / 1023.f; p[2] = float((x >> 20) & 1023) / 1023.f;
        p[3] = float(x >> 30) / 3.f;
    }
}
// Upload `px` (w*h RGBA) into r (in state s, left in s).
void upload(ID3D12Resource* r, D3D12_RESOURCE_STATES s, const std::vector<float>& px) {
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g_dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
    ID3D12Resource* up = buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* m = nullptr;
    up->Map(0, nullptr, reinterpret_cast<void**>(&m));
    const size_t tb = texel_bytes(d.Format);
    for (uint32_t y = 0; y < d.Height; ++y)
        for (uint32_t x = 0; x < d.Width; ++x) pack(d.Format, &px[(size_t(y) * d.Width + x) * 4], m + fp.Offset + y * fp.Footprint.RowPitch + x * tb);
    up->Unmap(0, nullptr);
    transition(r, s, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = r; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(r, D3D12_RESOURCE_STATE_COPY_DEST, s);
    submit();
    up->Release();
}
std::vector<float> download(ID3D12Resource* r, D3D12_RESOURCE_STATES s) {
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g_dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
    ID3D12Resource* rb = buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(r, s, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    src.pResource = r; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(r, D3D12_RESOURCE_STATE_COPY_SOURCE, s);
    std::vector<float> px(size_t(d.Width) * d.Height * 4);
    if (!submit()) { rb->Release(); return {}; }
    uint8_t* m = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&m));
    const size_t tb = texel_bytes(d.Format);
    for (uint32_t y = 0; y < d.Height; ++y)
        for (uint32_t x = 0; x < d.Width; ++x) unpack(d.Format, m + fp.Offset + y * fp.Footprint.RowPitch + x * tb, &px[(size_t(y) * d.Width + x) * 4]);
    rb->Unmap(0, nullptr);
    rb->Release();
    return px;
}
double v_max(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) if (i % 4 != 3) m = std::max(m, std::fabs(double(a[i]) - b[i]));
    return m;
}
struct Diff { double max = 0, mean = 0; size_t bad = 0, worst = 0, over = 0; };
Diff compare(const std::vector<float>& a, const std::vector<float>& b) {
    Diff d;
    if (a.size() != b.size() || a.empty()) { d.bad = 1; return d; }
    size_t n = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (i % 4 == 3) continue;   // alpha
        if (!std::isfinite(b[i])) { ++d.bad; continue; }
        const double e = std::fabs(double(a[i]) - b[i]);
        if (e > d.max) { d.max = e; d.worst = i; }
        if (e > 0.05) ++d.over;
        d.mean += e;
        ++n;
    }
    d.mean /= double(std::max<size_t>(n, 1));
    return d;
}

DXGI_FORMAT dxgi_of(w2::Fmt f, DXGI_FORMAT frame) {
    switch (f) {
        case w2::Fmt::kFrame: return frame;
        case w2::Fmt::kHalf: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case w2::Fmt::kR32F: return DXGI_FORMAT_R32_FLOAT;
        case w2::Fmt::kRG32F: return DXGI_FORMAT_R32G32_FLOAT;
        case w2::Fmt::kF32: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case w2::Fmt::kRG16F: return DXGI_FORMAT_R16G16_FLOAT;
    }
    return frame;
}

// The route as nr_asi_wilsjo2.cpp's D3d12Io on its native path, the network an identity.
class TestIo final : public w2::FlowIo {
public:
    TestIo(w2::Stage& stage, Stage12& impl, ID3D12Resource* target, ID3D12Resource* depth, ID3D12Resource* motion)
        : stage_(stage), impl_(impl), frame_(target->GetDesc().Format) {
        st_[target] = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        st_[depth] = {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        st_[motion] = st_[depth];
    }
    w2::Surf make(w2::Fmt f, uint32_t w, uint32_t h, bool) override {
        ID3D12Resource* r = texture(dxgi_of(f, frame_), w, h, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (r) st_[r] = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        ++made_;
        return {r};
    }
    void retire(w2::Surf& s) override {
        if (s.h) { st_.erase(static_cast<ID3D12Resource*>(s.h)); retired_.push_back(static_cast<ID3D12Resource*>(s.h)); }
        s = {};
    }
    void use(w2::Surf s, w2::Access a) override {
        if (s.h) to(static_cast<ID3D12Resource*>(s.h), a == w2::Access::kRead ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                                            : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    bool record(const Recorder& fn) override {
        impl_.begin(g_cmd, [this](ID3D12Resource* r, D3D12_RESOURCE_STATES s) { to(r, s); });
        bool ok = false;
        try {
            ok = fn(VK_NULL_HANDLE, stage_, [](w2::Surf s) { return nr::asi::native::image(static_cast<ID3D12Resource*>(s.h)); });
        } catch (const std::exception& x) {
            std::printf("record: %s\n", x.what());
        }
        impl_.end();
        ++records_;
        return ok;
    }
    bool has(w2::Mode m) const override { return stage_.has(m); }
    bool has_fast() const override { return false; }
    bool stage_failed() const override { return false; }
    w2::PassReady pass(uint32_t, const w2::PassControls&, uint32_t, uint32_t) override { return w2::PassReady::kReady; }
    void release_passes(size_t) override {}
    int network(const w2::NetCall& c) override {   // the identity network
        use(c.input, w2::Access::kRead);
        use(c.output, w2::Access::kWrite);
        ++networks_;
        return record([&](VkCommandBuffer vk, w2::Stage& stage, const ImageOf& image_of) {
            w2::Dispatch d;
            d.t0 = image_of(c.input);
            d.u0 = image_of(c.output);
            d.constants = w2::copy_constants(w2::kCopy, 0, 0, 0, 0, c.width, c.height);
            stage.record(vk, d);
            return true;
        }) ? 0 : 1;
    }
    void finish() override { for (auto& [r, s] : st_) { transition(r, s.now, s.home); s.now = s.home; } }
    void say(const char* t) override { std::printf("flow: %s\n", t); }
    int records_ = 0, networks_ = 0, made_ = 0;
    std::vector<ID3D12Resource*> retired_;

private:
    struct S { D3D12_RESOURCE_STATES now, home; };
    void to(ID3D12Resource* r, D3D12_RESOURCE_STATES want) {
        auto it = st_.find(r);
        if (it == st_.end()) { std::printf("untracked resource\n"); return; }
        transition(r, it->second.now, want);
        it->second.now = want;
    }
    w2::Stage& stage_;
    Stage12& impl_;
    DXGI_FORMAT frame_;
    std::map<ID3D12Resource*, S> st_;
};

}  // namespace
#include "vk_reference.hpp"
namespace {
w2::Stage* g_vk_stage = nullptr;   // the Vulkan reference (empty: no reference, the identity check gates instead)

w2::FlowFrame frame_of(w2::Surf target, w2::Surf depth, w2::Surf motion, uint32_t w, uint32_t h, DXGI_FORMAT format,
                       bool hdr, int frame) {
    w2::FlowFrame ff;
    ff.target = target;
    ff.width = w; ff.height = h;
    ff.unorm = format == DXGI_FORMAT_R10G10B10A2_UNORM;
    ff.direct_ok = true;
    ff.depth = depth;
    ff.motion = motion;
    ff.guide_signature = "test";
    ff.depth_rect = {0, 0, w, h};
    ff.motion_rect = {0, 0, w, h};
    ff.hdr = hdr;
    ff.reset = frame == 0;
    return ff;
}
// The same 4 frames on the Vulkan stage: its last output.
std::vector<float> vk_flow(DXGI_FORMAT format, bool hdr, const std::vector<float>& in, const std::vector<float>& dz,
                           const std::vector<float>& mz, uint32_t w, uint32_t h, bool* all) {
    VkImage target = vkref::texture(format, w, h), depth = vkref::texture(DXGI_FORMAT_R32_FLOAT, w, h),
            motion = vkref::texture(DXGI_FORMAT_R16G16_FLOAT, w, h);
    vkref::upload(depth, dz);
    vkref::upload(motion, mz);
    w2::FlowState state;
    w2::Settings s;
    vkref::Io io(*g_vk_stage, format);
    std::vector<float> out;
    *all = true;
    for (int frame = 0; frame < 4; ++frame) {
        vkref::upload(target, in);
        const w2::FlowFrame ff = frame_of({target}, {depth}, {motion}, w, h, format, hdr, frame);
        *all = w2::flow_run(io, state, ff, s, uint64_t(format)) && *all;
        io.finish();
        if (frame == 3) out = vkref::download(target);
    }
    for (VkImage i : io.retired_) vkref::destroy(i);
    vkref::destroy(target); vkref::destroy(depth); vkref::destroy(motion);
    return out;
}

void flow_case(w2::Stage& stage, Stage12& impl, DXGI_FORMAT format, bool hdr, const char* name, double tolerance) {
    const uint32_t w = 640, h = 360;
    std::vector<float> in(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            float* p = &in[(size_t(y) * w + x) * 4];
            const float u = float(x) / (w - 1), v = float(y) / (h - 1);
            const float scale = hdr ? 4.0f : 1.0f;
            p[0] = scale * u; p[1] = scale * v * 0.8f; p[2] = scale * (0.25f + 0.5f * u * v) * (((x / 16 + y / 16) & 1) ? 1.f : 0.5f);
            p[3] = 1.0f;
        }
    ID3D12Resource* target = texture(format, w, h, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* depth = texture(DXGI_FORMAT_R32_FLOAT, w, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false);
    ID3D12Resource* motion = texture(DXGI_FORMAT_R16G16_FLOAT, w, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false);
    std::vector<float> dz(size_t(w) * h * 4, 0.5f), mz(size_t(w) * h * 4, 0.0f);
    upload(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, dz);
    upload(motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, mz);
    w2::FlowState state;
    w2::Settings s;
    TestIo io(stage, impl, target, depth, motion);
    bool all = true;
    std::vector<float> out;
    for (int frame = 0; frame < 4; ++frame) {
        upload(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, in);
        const w2::FlowFrame ff = frame_of({target}, {depth}, {motion}, w, h, format, hdr, frame);
        const bool ran = w2::flow_run(io, state, ff, s, uint64_t(format));
        all = all && ran && submit();
        if (frame == 3) out = download(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    const Diff d = compare(in, out);
    if (!out.empty()) {
        const size_t px = d.worst / 4;
        std::printf("%s: worst at (%zu, %zu) channel %zu: in %.4f %.4f %.4f, out %.4f %.4f %.4f; %zu channel(s) off by > 0.05\n",
                    name, px % w, px / w, d.worst % 4, in[px * 4], in[px * 4 + 1], in[px * 4 + 2], out[px * 4], out[px * 4 + 1],
                    out[px * 4 + 2], d.over);
    }
    std::printf("%s: %d records, %d network calls, %d surfaces; identity network: max |out - in| %.5f, mean %.6f, "
                "non-finite %zu\n", name, io.records_, io.networks_, io.made_, d.max, d.mean, d.bad);
    check(all, (std::string(name) + ": 4 frames recorded and run, no device removal").c_str());
    check(io.networks_ >= 4, (std::string(name) + ": the network called every frame").c_str());
    if (g_vk_stage) {
        bool vk_all = false;
        const std::vector<float> ref = vk_flow(format, hdr, in, dz, mz, w, h, &vk_all);
        const Diff v = compare(ref, out);
        const Diff vi = compare(in, ref);
        std::printf("%s: the Vulkan stage (the 0.0.8.1 route) gives max |out - in| %.5f with %zu channel(s) off by > 0.05; "
                    "D3D12 vs Vulkan: max %.6f, mean %.7f, non-finite %zu\n", name, vi.max, vi.over, v.max, v.mean, v.bad);
        if (!ref.empty()) {
            const size_t px = v.worst / 4;
            std::printf("%s: D3D12 vs Vulkan worst at (%zu, %zu): Vulkan %.5f %.5f %.5f, D3D12 %.5f %.5f %.5f\n", name, px % w,
                        px / w, ref[px * 4], ref[px * 4 + 1], ref[px * 4 + 2], out[px * 4], out[px * 4 + 1], out[px * 4 + 2]);
        }
        check(vk_all, (std::string(name) + ": the Vulkan reference ran").c_str());
        check(v.bad == 0 && v.max <= tolerance, (std::string(name) + ": the D3D12 route gives the Vulkan route's picture").c_str());
    } else {
        check(d.bad == 0 && d.max <= tolerance, (std::string(name) + ": the identity network gives the frame back").c_str());
    }
    for (auto* r : io.retired_) r->Release();
    target->Release(); depth->Release(); motion->Release();
}

void pq_case(w2::Stage& stage, Stage12& impl) {
    const uint32_t w = 512, h = 256;
    std::vector<float> pq(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            float* p = &pq[(size_t(y) * w + x) * 4];
            p[0] = float(x) / (w - 1); p[1] = float(y) / (h - 1); p[2] = 0.5f * (p[0] + p[1]) * 0.9f; p[3] = 1.0f;
        }
    ID3D12Resource* p0 = texture(DXGI_FORMAT_R10G10B10A2_UNORM, w, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* lin = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* p1 = texture(DXGI_FORMAT_R10G10B10A2_UNORM, w, h, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    upload(p0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, pq);
    const auto noop = [](ID3D12Resource*, D3D12_RESOURCE_STATES) {};
    bool ok = true;
    try {
        impl.begin(g_cmd, noop);
        w2::Dispatch d;
        d.t0 = nr::asi::native::image(p0);
        d.u0 = nr::asi::native::image(lin);
        d.constants.mode = w2::kFinishedToLinear; d.constants.width = w; d.constants.height = h;
        stage.record(VK_NULL_HANDLE, d);
        impl.end();
        transition(lin, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        impl.begin(g_cmd, noop);
        w2::Dispatch e;
        e.t0 = nr::asi::native::image(lin);
        e.t2 = nr::asi::native::image(p0);
        e.u0 = nr::asi::native::image(p1);
        e.constants.mode = w2::kFinishedToPq; e.constants.width = w; e.constants.height = h;
        stage.record(VK_NULL_HANDLE, e);
        impl.end();
        transition(lin, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    } catch (const std::exception& x) {
        std::printf("pq: %s\n", x.what());
        ok = false;
    }
    ok = submit() && ok;
    const std::vector<float> l = download(lin, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const std::vector<float> back = download(p1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // the reference: wilsjo2_finished.comp's math in float on the CPU, the linear picture rounded to 16-bit float as
    // its texture holds it, the result to 10 bits
    std::vector<float> ref(pq.size());
    {
        const float kM1 = 2610.0f / 16384.0f, kM2 = 2523.0f / 32.0f, kC1 = 3424.0f / 4096.0f, kC2 = 2413.0f / 128.0f,
                    kC3 = 2392.0f / 128.0f;
        auto sat = [](float v) { return std::clamp(v, 0.f, 1.f); };
        auto dec = [&](float c) {
            const float p = std::pow(sat(c), 1.0f / kM2);
            return std::pow(std::max(p - kC1, 0.f) / std::max(kC2 - kC3 * p, 1e-6f), 1.0f / kM1) * 125.0f;
        };
        auto enc = [&](float l) {
            const float p = std::pow(sat(l / 125.0f), kM1);
            return std::pow((kC1 + kC2 * p) / (1.0f + kC3 * p), kM2);
        };
        for (size_t i = 0; i < pq.size(); i += 4) {
            const float q0 = std::lround(pq[i] * 1023) / 1023.f, q1 = std::lround(pq[i + 1] * 1023) / 1023.f,
                        q2 = std::lround(pq[i + 2] * 1023) / 1023.f;
            const float r = dec(q0), g = dec(q1), b = dec(q2);
            float l[3] = {1.6604910f * r - 0.5876411f * g - 0.0728499f * b, -0.1245505f * r + 1.1328999f * g - 0.0083494f * b,
                          -0.0181508f * r - 0.1005789f * g + 1.1187297f * b};
            for (float& v : l) v = float_of(half_of(v));
            ref[i] = enc(0.6274039f * l[0] + 0.3292830f * l[1] + 0.0433131f * l[2]);
            ref[i + 1] = enc(0.0690973f * l[0] + 0.9195404f * l[1] + 0.0113623f * l[2]);
            ref[i + 2] = enc(0.0163914f * l[0] + 0.0880133f * l[1] + 0.8955953f * l[2]);
            ref[i + 3] = 1.0f;
        }
    }
    std::vector<float> vk_back, vk_lin;
    if (g_vk_stage) {
        VkImage v0 = vkref::texture(DXGI_FORMAT_R10G10B10A2_UNORM, w, h), vl = vkref::texture(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h),
                v1 = vkref::texture(DXGI_FORMAT_R10G10B10A2_UNORM, w, h);
        vkref::upload(v0, pq);
        try {
            vkref::begin();
            w2::Dispatch d;
            d.t0 = vkref::image(v0);
            d.u0 = vkref::image(vl);
            d.constants.mode = w2::kFinishedToLinear; d.constants.width = w; d.constants.height = h;
            g_vk_stage->record(vkref::g_cmd, d);
            w2::Dispatch e;
            e.t0 = vkref::image(vl);
            e.t2 = vkref::image(v0);
            e.u0 = vkref::image(v1);
            e.constants.mode = w2::kFinishedToPq; e.constants.width = w; e.constants.height = h;
            g_vk_stage->record(vkref::g_cmd, e);
            vkref::barrier_all();
            vkref::submit();
            g_vk_stage->end_recording();
            vk_back = vkref::download(v1);
            vk_lin = vkref::download(vl);
        } catch (const std::exception& x) {
            std::printf("vulkan pq: %s\n", x.what());
        }
        vkref::destroy(v0); vkref::destroy(vl); vkref::destroy(v1);
    }
    const Diff in_out = compare(pq, back);
    std::printf("HDR10: there and back vs the input: max %.5f (the 16-bit linear picture's rounding near black of "
                "saturated colours)\n", in_out.max);
    const Diff d = compare(ref, back);
    double peak = 0;
    for (size_t i = 0; i < l.size(); ++i) if (i % 4 != 3) peak = std::max(peak, double(l[i]));
    std::printf("HDR10: PQ -> linear (peak %.2f in scRGB units) -> PQ vs the CPU reference: max %.5f (1/1023 = %.5f), mean %.6f\n",
                peak, d.max, 1.0 / 1023, d.mean);
    check(ok, "HDR10: both conversions recorded and run");
    if (g_vk_stage) {
        // pow's last bits differ between the two compilers; the BT.2020 -> BT.709 matrix subtracts values up to ~200
        // for a channel near 0 of a saturated colour, so there an absolute difference of that size's float32 rounding
        // shows. Both measured against the pixel's brightest channel: linear to one 16-bit float step plus 2^-14 of
        // the pixel's peak; the PQ result in display light (nits) to 0.2 % of the pixel's brightest channel + 0.05 nits.
        auto nits = [](float code) {
            const float kM1 = 2610.0f / 16384.0f, kM2 = 2523.0f / 32.0f, kC1 = 3424.0f / 4096.0f, kC2 = 2413.0f / 128.0f,
                        kC3 = 2392.0f / 128.0f;
            const float p = std::pow(std::clamp(code, 0.f, 1.f), 1.0f / kM2);
            return 10000.0 * std::pow(std::max(p - kC1, 0.f) / std::max(kC2 - kC3 * p, 1e-6f), 1.0f / kM1);
        };
        size_t lin_bad = vk_lin.size() == l.size() ? 0 : 1, lin_steps = 0, pq_bad = vk_back.size() == back.size() ? 0 : 1,
               pq_codes = 0, worst_px = 0;
        double lin_worst = 0, pq_worst = 0;
        for (size_t px = 0; px * 4 < l.size() && px * 4 < vk_lin.size() && px * 4 < back.size() && px * 4 < vk_back.size(); ++px) {
            double peak_l = 0, peak_n = 0;
            for (int c = 0; c < 3; ++c) {
                peak_l = std::max(peak_l, std::fabs(double(vk_lin[px * 4 + c])));
                peak_n = std::max(peak_n, nits(vk_back[px * 4 + c]));
            }
            for (int c = 0; c < 3; ++c) {
                const size_t i = px * 4 + c;
                const double e = std::fabs(double(l[i]) - vk_lin[i]);
                const double step = std::max(std::fabs(double(vk_lin[i])), 1.0 / 16384) / 1024.0;
                if (e > 1.01 * step) ++lin_steps;
                const double lt = step + peak_l / 16384.0;
                if (!std::isfinite(l[i]) || e > lt) ++lin_bad;
                lin_worst = std::max(lin_worst, e / lt);
                if (std::fabs(double(back[i]) - vk_back[i]) > 2.5 / 1023) ++pq_codes;
                const double en = std::fabs(nits(back[i]) - nits(vk_back[i])), nt = 0.002 * peak_n + 0.05;
                if (!std::isfinite(back[i]) || en > nt) ++pq_bad;
                if (en / nt > pq_worst) { pq_worst = en / nt; worst_px = px; }
            }
        }
        const size_t channels = back.size() / 4 * 3;
        std::printf("HDR10: PQ -> linear, D3D12 vs the Vulkan stage: %zu of %zu value(s) more than one 16-bit float step apart; "
                    "worst %.3f of the bound, %zu over it\n", lin_steps, channels, lin_worst, lin_bad);
        std::printf("HDR10: PQ -> linear -> PQ, D3D12 vs the Vulkan stage: %zu of %zu channel(s) more than 2 codes apart (max %.5f); "
                    "in display light worst %.3f of the bound at (%zu, %zu) [Vulkan %.4f %.4f %.4f, D3D12 %.4f %.4f %.4f], %zu over it\n",
                    pq_codes, channels, v_max(vk_back, back), pq_worst, worst_px % w, worst_px / w, vk_back[worst_px * 4],
                    vk_back[worst_px * 4 + 1], vk_back[worst_px * 4 + 2], back[worst_px * 4], back[worst_px * 4 + 1],
                    back[worst_px * 4 + 2], pq_bad);
        check(lin_bad == 0, "HDR10: PQ -> linear as the Vulkan route computes it");
        check(pq_bad == 0 && pq_codes * 100 <= channels,
              "HDR10: PQ -> linear -> PQ as the Vulkan route computes it (in display light; at most 1 % of channels > 2 codes apart)");
    } else {
        check(d.bad == 0 && d.max <= 2.5 / 1023, "HDR10: PQ -> linear -> PQ as wilsjo2_finished.comp computes it (CPU reference)");
    }
    check(peak > 10.0, "HDR10: the linear picture is HDR (scRGB above 1)");
    p0->Release(); lin->Release(); p1->Release();
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) { std::printf("usage: nr_wilsjo2_d3d12_test <dxbc folder> [<spv folder>]\n"); return 2; }
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev)))) { std::printf("no D3D12 device\n"); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue));
    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc));
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_cmd));
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    const LUID luid = g_dev->GetAdapterLuid();
    std::printf("device LUID %08lX:%08lX\n", luid.HighPart, luid.LowPart);
    std::wstring folder;
    for (const char* c = argv[1]; *c; ++c) folder += wchar_t(*c);
    auto impl = std::make_unique<Stage12>();
    Stage12* raw = impl.get();
    std::string why;
    const bool made = raw->make(g_dev, folder, &why);
    check(made, ("the programs made as D3D12 compute pipelines" + (made ? std::string() : ": " + why)).c_str());
    if (!made) return 1;
    w2::Stage stage(std::move(impl));
    std::unique_ptr<w2::Stage> vk_stage;
    if (argc == 3) {
        std::string vwhy;
        if (!vkref::init(luid, &vwhy)) {
            check(false, ("the Vulkan reference: " + vwhy).c_str());
        } else {
            try {
                vk_stage = std::make_unique<w2::Stage>(vkref::host(), std::string(argv[2]));
                g_vk_stage = vk_stage.get();
                check(true, "the Vulkan reference stage made from the .spv");
            } catch (const std::exception& x) {
                check(false, (std::string("the Vulkan reference stage: ") + x.what()).c_str());
            }
        }
    }
    int modes = 0;
    for (const auto& p : nr::asi::native::kPrograms) modes += stage.has(p.mode) ? 1 : 0;
    std::printf("%d of %zu modes have a program\n", modes, std::size(nr::asi::native::kPrograms));
    check(modes == int(std::size(nr::asi::native::kPrograms)), "every mode of the Vulkan stage's list has its program");
    flow_case(stage, *raw, DXGI_FORMAT_R16G16B16A16_FLOAT, true, "HDR linear RGBA16F (the Present PQ path's frame)", 0.01);
    flow_case(stage, *raw, DXGI_FORMAT_R10G10B10A2_UNORM, false, "SDR R10G10B10A2 (direct: read and written in place)", 2.5 / 1023);
    pq_case(stage, *raw);
    std::printf("%s: %llu dispatches, %d failure(s)\n", g_fail ? "FAILED" : "ALL PASSED", (unsigned long long)raw->dispatches(), g_fail);
    (void)stage;
    return g_fail ? 1 : 0;
}
