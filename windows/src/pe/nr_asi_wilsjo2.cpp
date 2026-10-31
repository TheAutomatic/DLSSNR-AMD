#include "nr_gpu_completion.hpp"
// The wilsjo2 style's per-frame flow on the .asi route: see nr_asi_wilsjo2.hpp. Section numbers are those of
// our specification of the style (not published). Where the forked OptiScaler as the 0.0.7 route ran it differs from the
// specification by one of our OptiScaler fixes (nr_pe_optifix.cpp), the fix is what is followed, and said so below.
#include "nr_asi_wilsjo2.hpp"
#include "nr_asi_wilsjo2_d3d12.hpp"
#include "nr_d3d12_asd.hpp"
#include "nr_d3d12_asd_backend.hpp"
#include "nr_pe_d3d12split.hpp"

#include "nr_pe_config.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_renodx_route.hpp"
#include "nr_pe_session.hpp"
#include "nr_vram_ledger.hpp"
#include "nr_wilsjo2_flow.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace nr::asi {
namespace {

using nr::pe::log;
namespace w2 = nr::wilsjo2;

// ---- our D3D12 surfaces ----------------------------------------------------------------------------------------
// Retired, not released at once: the GPU may still run a frame that uses them, in a command list the game has not
// even submitted yet. Released once 64 more runs went by AND 2 s passed (a run count alone goes by fast with several
// layers a frame, and a frame's lists are on the GPU within a few frames, far under 2 s).
nr::gpu::Uses gpu_uses;
struct Retired {
    nr::gpu::Uses uses;
    uint64_t at;
    ULONGLONG ms;
    ID3D12Resource* r;
};
std::vector<Retired> g_retired;
uint64_t g_clock = 0;
constexpr uint64_t kRetireRuns = 64;
constexpr ULONGLONG kRetireMs = 2000;

void retire(ID3D12Resource*& r) {
    if (r) g_retired.push_back({gpu_uses, g_clock + kRetireRuns, GetTickCount64() + kRetireMs, r});
    r = nullptr;
}

void tick() {
    gpu_uses.touch();
    ++g_clock;
    if (g_retired.empty()) return;
    const ULONGLONG now = GetTickCount64();
    g_retired.erase(std::remove_if(g_retired.begin(), g_retired.end(),
                                   [now](const Retired& e) {
                                       if (e.at > g_clock || e.ms > now || !e.uses.ready()) return false;
                                       e.r->Release();
                                       return true;
                                   }),
                    g_retired.end());
}

// Spec 4.8 table: default heap, one mip, UAV allowed, in the frame's format; UNORDERED_ACCESS between uses.
ID3D12Resource* make_surface(ID3D12Device* device, DXGI_FORMAT format, uint32_t w, uint32_t h) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = format; d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    nr::vram::track_d3d12(device, r, nr::vram::kWilsjo2);
    return r;
}

void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (!r || from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmd->ResourceBarrier(1, &b);
}

constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// The programs, one set for the process (made on the game's device at first use).
std::unique_ptr<w2::Stage> g_stage;
// The Stage could not be made (its programs, memory): tried again 5 s later rather than never (a passing shortage does
// not keep wilsjo2 off for the run); said once, and once when it comes back.
struct StageRetry {
    ULONGLONG failed_ms = 0;
    bool said = false;
    bool blocked() const { return failed_ms && GetTickCount64() - failed_ms < 5000; }
    void fail(const char* what) {
        failed_ms = GetTickCount64();
        if (!said) { said = true; log("[nr] wilsjo2 style unavailable: %s (tried again every 5 s)", what); }
    }
    void made() {
        if (failed_ms) log("[nr] wilsjo2 style available again");
        failed_ms = 0;
    }
};
StageRetry g_stage_retry;
// NR_TEST_STAGE_FAIL=<n> (tests): the first n Stages are not made (a passing failure, retried)
bool stage_fail_test() {
    static int left = [] { const char* e = std::getenv("NR_TEST_STAGE_FAIL"); return e ? std::atoi(e) : 0; }();
    if (left <= 0) return false;
    --left;
    return true;
}

w2::Image image_of(const nr::pe::Session::RawImage& r) {
    w2::Image i;
    i.image = r.image; i.format = r.format; i.width = r.width; i.height = r.height; i.layout = r.layout;
    return i;
}

// `noise`: the layer's NoiseEveryFrame (ours), for frames without motion vectors
nr::dlssnr::Controls6 controls_of(const w2::PassControls& pc, bool noise = false, uint32_t proxy_curve = 0, bool capture = true) {
    nr::dlssnr::Controls6 c;
    c.intensity = pc.intensity;
    c.style = int(pc.style);
    c.local_structure = pc.local_structure;
    c.local_tone = pc.local_tone;
    c.skin_structure = pc.skin_structure;
    c.use_auto_mask = pc.auto_mask ? 1 : 0;
    c.noise_every_frame = noise ? 1 : 0;
    c.proxy_curve = int(proxy_curve);
    c.model_capture_allowed = capture;
    return c;
}


// ---- the native D3D12 route (M5 v2): the stage's programs as D3D12 compute -------------------------------------
// On the native runtime with ASD (nr_d3d12_asd.hpp) at ASD_HIT, the flow below records into the game's own list: the
// wilsjo2 programs - the very SPIR-V the Vulkan stage loads, its 256-byte constant block moved from push constants to a
// read-only buffer (windows/build/w2_spv_d3d12.py), written as HLSL by SPIRV-Cross and compiled to DXBC
// (windows/build/build_wilsjo2_d3d12.sh) - created by the D3D12 driver as ordinary compute pipelines, as 0.0.8.1 has the
// Vulkan driver compile them; the network through nr::dlssnr::evaluate_d3d12, i.e. the ASD backend's ACO network.
// The layout is the Vulkan one's: t0..t3 (SRVs, static linear-clamp samplers s0..s3), u4 / u5 (UAVs), the constants at
// t6 (a root SRV into an upload ring). Passes (fast) and anything else not listed falls back to the Vulkan bridge.
}  // namespace
namespace native {   // with the stage (nr_asi_wilsjo2_d3d12.hpp)

// The programs, made once on a thread of ours (no frame waits on the driver's compiler); the frames before take the
// bridge.
struct Programs {
    std::mutex m;
    int state = 0;   // 0 not started, 1 making, 2 made, 3 failed
    std::string why;
    ID3D12Device* device = nullptr;
    std::unique_ptr<w2::Stage> stage;   // over `impl`
    Stage12* impl = nullptr;
};
Programs g_programs;

// Ready on `device`: the stage; else null and why (making them is started on the first call).
w2::Stage* programs(ID3D12Device* device, std::string* why) {
    std::lock_guard<std::mutex> l(g_programs.m);
    if (g_programs.state == 2 && g_programs.device == device) return g_programs.stage.get();
    if (g_programs.state == 3 && g_programs.device == device) { *why = "its D3D12 programs: " + g_programs.why; return nullptr; }
    if (g_programs.state == 1) { *why = "its D3D12 programs are being made"; return nullptr; }
    // first use, or another device: (re)made
    if (g_programs.stage) (void)g_programs.stage.release();   // the GPU may still run the old device's (never freed)
    g_programs.impl = nullptr;
    if (g_programs.device) g_programs.device->Release();
    device->AddRef();
    g_programs.device = device;
    g_programs.state = 1;
    *why = "its D3D12 programs are being made";
    const std::wstring folder = nr::pe::asd::folder() + L"\\wilsjo2-d3d12";
    nr::pe::log("[asd] wilsjo2 on D3D12: making its programs from %ls (the bridge runs these frames until they are made)",
                folder.c_str());
    device->AddRef();
    std::thread([device, folder] {
        auto s = std::make_unique<Stage12>();
        std::string e;
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = false;
        try { ok = s->make(device, folder, &e); } catch (const std::exception& x) { e = x.what(); }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> l(g_programs.m);
            if (g_programs.device == device) {
                if (ok) {
                    g_programs.impl = s.get();
                    g_programs.stage = std::make_unique<w2::Stage>(std::move(s));
                    g_programs.state = 2;
                } else {
                    g_programs.why = e;
                    g_programs.state = 3;
                }
            }
        }
        if (ok) nr::pe::log("[asd] wilsjo2 on D3D12: its programs are made (%.0f ms; the D3D12 driver's compute pipelines)", ms);
        else nr::pe::log("[asd] ERROR: wilsjo2 on D3D12: its programs cannot be made: %s (the bridge runs it)", e.c_str());
        device->Release();
    }).detach();
    return nullptr;
}
Stage12* programs_impl() {
    std::lock_guard<std::mutex> l(g_programs.m);
    return g_programs.state == 2 ? g_programs.impl : nullptr;
}

// The route's reason, said when it changes.
void say_route(const std::string& line) {
    static std::mutex m;
    static std::string last;
    std::lock_guard<std::mutex> l(m);
    if (line == last) return;
    last = line;
    nr::pe::log("[asd] wilsjo2 style: %s", line.c_str());
}

// The GPU time of a native run (the stage's programs and the ASD network together): two timestamps a run, a ring of
// kRing runs read back kRing runs later.
class Timing {
public:
    void begin(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, uint32_t w, uint32_t h) {
        if (!made_) make(device);
        if (!map_) return;
        slot_ = uint32_t(runs_ % kRing);
        if (runs_ >= kRing) read(slot_);
        ++runs_;
        w_ = w; h_ = h;
        cmd->EndQuery(heap_, D3D12_QUERY_TYPE_TIMESTAMP, slot_ * 2);
    }
    void end(ID3D12GraphicsCommandList* cmd) {
        if (!map_) return;
        cmd->EndQuery(heap_, D3D12_QUERY_TYPE_TIMESTAMP, slot_ * 2 + 1);
        cmd->ResolveQueryData(heap_, D3D12_QUERY_TYPE_TIMESTAMP, slot_ * 2, 2, buf_, uint64_t(slot_) * 16);
    }

private:
    static constexpr uint32_t kRing = 16;
    void make(ID3D12Device* device) {
        made_ = true;
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 2 * kRing;
        if (FAILED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&heap_)))) return;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 16 * kRing; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&buf_)))) return;
        ID3D12CommandQueue* q = nullptr;
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        UINT64 hz = 0;
        if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) && q) { q->GetTimestampFrequency(&hz); q->Release(); }
        void* m = nullptr;
        if (hz && SUCCEEDED(buf_->Map(0, nullptr, &m)) && m) { map_ = static_cast<uint8_t*>(m); hz_ = double(hz); }
        if (!map_) nr::pe::log("[asd] wilsjo2 on D3D12: its GPU time cannot be measured (no timestamp queries)");
    }
    void read(uint32_t slot) {
        uint64_t t[2];
        std::memcpy(t, map_ + uint64_t(slot) * 16, 16);
        if (!t[0] || t[1] <= t[0]) return;
        const double ms = double(t[1] - t[0]) * 1000.0 / hz_;
        if (ms > 1000.0) return;   // not (yet) a pair of this run's
        ++n_; sum_ += ms;
        min_ = n_ - n_base_ == 1 ? ms : std::min(min_, ms);
        max_ = std::max(max_, ms);
        if (n_ <= 3)
            nr::pe::log("[asd] wilsjo2 on D3D12: one run took %.3f ms on the GPU (%ux%u: the wilsjo2 programs + the ASD network)",
                        ms, w_, h_);
        if (n_ % 300 == 0) {
            nr::pe::log("[asd] wilsjo2 on D3D12: runs %llu-%llu: GPU avg %.3f ms (min %.3f, max %.3f), %ux%u; the network's own "
                        "share: the [asd] timing lines",
                        (unsigned long long)(n_ - 299), (unsigned long long)n_, sum_ / 300.0, min_, max_, w_, h_);
            sum_ = 0; min_ = 0; max_ = 0; n_base_ = n_;
        }
    }
    bool made_ = false;
    ID3D12QueryHeap* heap_ = nullptr;
    ID3D12Resource* buf_ = nullptr;
    uint8_t* map_ = nullptr;
    double hz_ = 0;
    uint64_t runs_ = 0, n_ = 0, n_base_ = 0;
    uint32_t slot_ = 0, w_ = 0, h_ = 0;
    double sum_ = 0, min_ = 0, max_ = 0;
};
Timing g_timing;

}  // namespace native

void wilsjo2_abandon_at_exit() { (void)g_stage.release(); }

// One OptiScaler feature's wilsjo2 state at one placement: the flow's (nr_wilsjo2_flow.hpp) and the passes' models
// (each pass its own feature, so its own history).
struct Wilsjo2Owner {
    bool capture_layer = false;
    bool at_present = false;   // wilsjo2_owner_at_present
    w2::FlowState flow;
    std::vector<nr::dlssnr::Feature*> models;      // pass k's at [k]
    std::map<ID3D12Resource*, bool> colour;        // our surfaces: colour-family (else read as a single-channel image)
    Wilsjo2VkOwner* vk = nullptr;                  // on the native runtime: the Vulkan flow's (run_bridged)
};

Wilsjo2Owner* wilsjo2_owner() { return new Wilsjo2Owner(); }
void wilsjo2_owner_at_present(Wilsjo2Owner& o) { o.at_present = true; }

namespace {
std::mutex g_spatial_lock;
std::map<int, w2::SpatialLayout> g_spatial_ran, g_spatial_present;   // layer row -> its last run with spatial compression
}  // namespace
void wilsjo2_note_spatial(int row, const w2::SpatialLayout& layout) {
    std::lock_guard<std::mutex> g(g_spatial_lock);
    g_spatial_ran[std::max(row, 1)] = layout;
}
void wilsjo2_present_spatial() {
    std::lock_guard<std::mutex> g(g_spatial_lock);
    g_spatial_present.swap(g_spatial_ran);
    g_spatial_ran.clear();
}
w2::SpatialLayout wilsjo2_spatial_layout(int row) {
    std::lock_guard<std::mutex> g(g_spatial_lock);
    const auto it = g_spatial_present.find(std::max(row, 1));
    return it == g_spatial_present.end() ? w2::SpatialLayout{} : it->second;
}
bool wilsjo2_spatial_running(int row) {
    std::lock_guard<std::mutex> g(g_spatial_lock);
    return g_spatial_present.count(std::max(row, 1)) != 0;
}

bool g_bridged_runs = false;   // run_bridged ran: its Vulkan surfaces are retired on this clock too

void wilsjo2_tick() {
    tick();
    if (g_bridged_runs) wilsjo2_vk_tick();
}

namespace {
// The flow's route on D3D12 (nr_wilsjo2_flow.hpp FlowIo): our surfaces are committed resources in UNORDERED_ACCESS
// between uses, read as NON_PIXEL_SHADER_RESOURCE; the game's guides read in NON_PIXEL_SHADER_RESOURCE and given back in
// their states; the stage's dispatches recorded into the game's list through the model's session (record_raw); the
// model through nr::dlssnr::evaluate_d3d12.
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

class D3d12Io final : public w2::FlowIo {
public:
    D3d12Io(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, Wilsjo2Owner& o, const Wilsjo2Frame* f,
            ID3D12Resource* target, bool before_upscale, uint32_t w, uint32_t h)
        : cmd_(cmd), device_(device), o_(o), f_(f), before_(before_upscale), w_(w), h_(h) {
        if (target) { frame_ = target->GetDesc().Format; track(target, kUav, true); }
        if (f) {
            if (f->depth) track(f->depth, f->depth_state, false);
            if (f->motion && f->motion != f->depth) track(f->motion, f->motion_state, false);
            if (f->exposure && !states_.count(f->exposure)) track(f->exposure, f->exposure_state, false);
        }
    }
    w2::Surf make(w2::Fmt format, uint32_t w, uint32_t h, bool colour) override {
        ID3D12Resource* r = make_surface(device_, dxgi_of(format, frame_), w, h);
        if (!r) return {};
        o_.colour[r] = colour;
        return {r};
    }
    void retire(w2::Surf& s) override {
        auto* r = static_cast<ID3D12Resource*>(s.h);
        if (r) { o_.colour.erase(r); states_.erase(r); }
        ::nr::asi::retire(r);
        s = {};
    }
    void use(w2::Surf s, w2::Access a) override {
        auto* r = static_cast<ID3D12Resource*>(s.h);
        if (!r) return;
        auto& st = state(r);
        const D3D12_RESOURCE_STATES want = a == w2::Access::kRead ? kSrv : kUav;
        transition(cmd_, r, st.now, want);
        st.now = want;
    }
    // The native D3D12 route (the stage `stage` over `impl`, set before the run): no session, no Vulkan.
    void go_native(w2::Stage* stage, native::Stage12* impl) { nstage_ = stage; nimpl_ = impl; }
    bool native() const { return nstage_ != nullptr; }
    bool record(const Recorder& fn) override {
        if (nstage_) return record_native(fn);
        bool ok = false;
        nr::dlssnr::with_d3d12_session([&](nr::pe::Session& session) {
            session_ = &session;
            ok = session.record_raw(device_, cmd_, [&](VkCommandBuffer vk, const HostDevice& host,
                                                       const nr::pe::Session::RawLookup& lookup) {
                if (!g_stage) {   // the programs, on the game's device, at the first frame
                    try {
                        if (stage_fail_test()) throw std::runtime_error("NR_TEST_STAGE_FAIL");
                        g_stage = std::make_unique<w2::Stage>(host, std::string(nr::pe::module_folder()) +
                                                                        "\\dlssnr-amd\\shaders\\wilsjo2");
                        g_stage_retry.made();
                    } catch (const std::exception& x) {
                        g_stage_retry.fail(x.what());
                        return false;
                    }
                }
                const ImageOf image_of = [&](w2::Surf s) {
                    auto* r = static_cast<ID3D12Resource*>(s.h);
                    if (!r) return w2::Image{};
                    const auto& st = state(r);
                    return ::nr::asi::image_of(lookup(r, st.now, st.colour));
                };
                return fn(vk, *g_stage, image_of);
            });
            if (g_stage) g_stage->end_recording();
            session_ = nullptr;
        });
        return ok;
    }
    bool has(w2::Mode mode) const override { return nstage_ ? nstage_->has(mode) : g_stage && g_stage->has(mode); }
    bool has_fast() const override { return nstage_ ? false : !g_stage || g_stage->has_fast(); }
    bool stage_failed() const override { return nstage_ ? false : g_stage_retry.blocked(); }
    w2::PassReady pass(uint32_t k, const w2::PassControls& pc, uint32_t mw, uint32_t mh) override {
        if (k < o_.models.size() && o_.models[k]) return w2::PassReady::kReady;
        int rc = 0;
        nr::dlssnr::Feature* m = nr::dlssnr::create_d3d12(device_, cmd_, nullptr, mw, mh, int(pc.preset), 1, controls_of(pc), &rc);
        if (!m) {
            log("[nr] wilsjo2 style: pass %u's feature could not be made (0x%08X): %s", k + 1, unsigned(rc),
                nr::dlssnr::last_error());
            return w2::PassReady::kFailed;
        }
        if (o_.models.size() <= k) o_.models.resize(k + 1, nullptr);
        o_.models[k] = m;
        return w2::PassReady::kMade;
    }
    void release_passes(size_t from) override {
        for (size_t k = from; k < o_.models.size(); ++k)
            if (o_.models[k]) nr::dlssnr::release_d3d12(o_.models[k]);
        o_.models.resize(std::min(from, o_.models.size()));
    }
    int network(const w2::NetCall& c) override {
        namespace route = nr::pe::renodx_route;
        route::begin_run(rf_);
        const auto rect = [](const w2::Rect& r) { nr::dlssnr::Rect o; o.x = r.x; o.y = r.y; o.width = r.width; o.height = r.height; return o; };
        const int result = nr::dlssnr::evaluate_d3d12(
            cmd_, o_.models[c.pass], nullptr, static_cast<ID3D12Resource*>(c.input.h), static_cast<ID3D12Resource*>(c.depth.h),
            static_cast<ID3D12Resource*>(c.motion.h), static_cast<ID3D12Resource*>(c.output.h), c.width, c.height,
            rect(c.depth_rect), rect(c.motion_rect), c.depth_inverted ? 1 : 0, c.reset ? 1 : 0,
            controls_of(c.controls, c.noise_every_frame, c.proxy_curve, c.last), c.mv_x, c.mv_y);
        route::end_run();
        return result == int(NVSDK_NGX_Result_Success) ? 0 : 1;
    }
    void passes_begin(const w2::FlowFacts& x) override {
        namespace route = nr::pe::renodx_route;
        style_.emplace(false);
        facts_.hdr = x.hdr; facts_.exposure = int(x.exposure); facts_.asked = x.asked; facts_.white = x.white;
        facts_.history = x.history; facts_.mode = x.mode; facts_.passes = x.passes;
        facts_.prep_outside = x.prep_outside; facts_.prep_missing = x.prep_missing;
        facts_.fast_passes = x.fast_passes; facts_.fast_fallback = x.fast_fallback;
        told_.emplace(&facts_);
        // the output NR Control shows: before the upscaler the upscaler's (the frame is the render's; "No upscaling" was
        // shown), else the frame
        route::note_pass(f_->flags, 1.0f, f_->depth_rect.width, f_->depth_rect.height, f_->out_w ? f_->out_w : w_,
                         f_->out_h ? f_->out_h : h_);
        rf_.before_upscale = before_;
        rf_.pre_exposure = f_->pre_exposure;
        rf_.exposure = f_->exposure;
        rf_.exposure_state = uint32_t(f_->exposure_state);
    }
    void passes_end() override { told_.reset(); style_.reset(); }
    void run_begin(VkCommandBuffer vk, const w2::Image& frame, bool hdr) override {
        if (nstage_) return;   // the native route: no Vulkan recording to mark (NR Control's time: the [asd] timing lines)
        session_->set_call_layer(nr::pe::renodx_route::call_layer());   // the run's whole time, for NR Control
        session_->outer_begin(vk);
        // F5 (nr_screenshot.hpp): the frame before this layer's NR, while a capture is armed
        o_.capture_layer = session_->capture_begin(vk, frame.image, frame.format, w_, h_, frame.layout, hdr ? 1 : 0,
                               o_.at_present ? "present" : before_ ? "nr-before-upscaler" : "nr-after-upscaler");
    }
    void run_end(VkCommandBuffer vk, const w2::Image& frame) override {
        if (nstage_) return;
        session_->set_call_layer(nr::pe::renodx_route::call_layer());
        if (o_.capture_layer) { session_->capture_end(vk, frame.image, frame.layout); o_.capture_layer = false; }   // and after it
        session_->outer_end(vk);
    }
    void finish() override {
        for (auto& [r, st] : states_) { transition(cmd_, r, st.now, st.home); st.now = st.home; }
        if (o_.capture_layer) {
            nr::dlssnr::with_d3d12_session([](nr::pe::Session& s) { s.capture_abort(); });
            o_.capture_layer = false;
        }
    }
    void say(const char* text) override { log("[nr] wilsjo2 style: %s", text); }
    void spatial_ran(const w2::SpatialLayout& layout) override { wilsjo2_note_spatial(nr::pe::renodx_route::call_layer(), layout); }
    DXGI_FORMAT frame_format() const { return frame_; }

private:
    // One recording of the flow's into the game's list as D3D12 compute: the list's state saved and given back (the
    // ASD backend's way), the resources put in each dispatch's states through this route's tracking.
    bool record_native(const Recorder& fn) {
        std::string why;
        if (!nr::pe::split::prepare(cmd_, &why)) {
            log("[asd] wilsjo2 on D3D12: a recording could not be made: the command list's state is not known (%s)", why.c_str());
            return false;
        }
        const auto saved = nr::pe::split::snapshot(cmd_);
        nimpl_->begin(cmd_, [this](ID3D12Resource* r, D3D12_RESOURCE_STATES want) {
            auto& st = state(r);
            transition(cmd_, r, st.now, want);
            st.now = want;
        });
        const ImageOf image_of = [](w2::Surf s) { return native::image(static_cast<ID3D12Resource*>(s.h)); };
        bool ok = false;
        try {
            ok = fn(VK_NULL_HANDLE, *nstage_, image_of);
        } catch (const std::exception& x) {
            static int said = 0;
            if (said++ < 5) log("[asd] ERROR: wilsjo2 on D3D12: %s", x.what());
            ok = false;
        }
        nimpl_->end();
        nr::pe::split::restore(cmd_, saved);
        return ok;
    }
    struct State { D3D12_RESOURCE_STATES now, home; bool colour; };
    void track(ID3D12Resource* r, D3D12_RESOURCE_STATES s, bool colour) { states_[r] = State{s, s, colour}; }
    State& state(ID3D12Resource* r) {
        auto it = states_.find(r);
        if (it == states_.end()) {   // one of ours: made in UNORDERED_ACCESS, kept there between runs
            const auto c = o_.colour.find(r);
            it = states_.emplace(r, State{kUav, kUav, c == o_.colour.end() || c->second}).first;
        }
        return it->second;
    }
    ID3D12GraphicsCommandList* cmd_;
    ID3D12Device* device_;
    Wilsjo2Owner& o_;
    const Wilsjo2Frame* f_;
    bool before_;
    uint32_t w_, h_;
    DXGI_FORMAT frame_ = DXGI_FORMAT_UNKNOWN;
    std::map<ID3D12Resource*, State> states_;
    nr::pe::Session* session_ = nullptr;
    w2::Stage* nstage_ = nullptr;
    native::Stage12* nimpl_ = nullptr;
    std::optional<nr::pe::renodx_route::CallStyle> style_;
    nr::pe::renodx_route::Wilsjo2Facts facts_;
    std::optional<nr::pe::renodx_route::CallWilsjo2> told_;
    nr::pe::renodx_route::Frame rf_;
};
}  // namespace

void wilsjo2_release(Wilsjo2Owner* o) {
    if (!o) return;
    if (o->vk) { wilsjo2_vk_release(o->vk); o->vk = nullptr; }
    D3d12Io io(nullptr, nullptr, *o, nullptr, nullptr, false, 0, 0);
    w2::flow_release(io, o->flow);
    delete o;
}

w2::Settings wilsjo2_settings(const nr::pe::LayerConfig& layer) {
    w2::Settings s;
    for (const auto& [key, value] : layer.keys) {
        std::string k = key;
        for (char& c : k) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (k == "runbeforesr") continue;   // the layer's Placement says it
        if (!w2::parse_key(s, key, value)) {
            static std::set<std::string> said;
            if (said.insert(key).second)
                log("[nr] wilsjo2 processing: dlssnr-amd.ini [Layer%d] %s = %s not understood", layer.number, key.c_str(),
                    value.c_str());
        }
    }
    s.run_before_sr = layer.placement == nr::pe::Placement::kBefore;
    return s;
}

namespace {
// ---- the native runtime: the whole flow in one recording of the bridge -----------------------------------------
// On the bridge (nr_pe_bridge.hpp) a recording is a cut of the game's list with a copy of every image it names into
// its twin and back. The flow below makes one a step - six to nine a frame, each copying the frame-sized images - which
// made wilsjo2 several times as costly as the RenoDX style, and in a game that executes its lists a few frames late a
// frame's recordings went past the bridge's staleness limit: dropped, the history restarted every time, NR as good as
// off. So on the bridge the Vulkan flow (nr_asi_wilsjo2_vk.cpp, as the D3D11 route runs it) runs instead, on the twins
// of the frame and its guides in one recording, the model passes on the D3D12 session's networks
// (nr::dlssnr::evaluate_bridge12). Under vkd3d-proton the flow below records into the game's list itself.
bool run_bridged(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, Wilsjo2Owner& o, const Wilsjo2Frame& f,
                 const w2::Settings& s, ID3D12Resource* target, uint32_t w, uint32_t h, bool before_upscale) {
    using Raw = nr::pe::Session::RawImage;
    VkCommandBuffer vk = VK_NULL_HANDLE;
    Raw t, d, m, e;
    nr::pe::DeviceHandles handles;
    nr::dlssnr::with_d3d12_session([&](nr::pe::Session& session) {
        vk = session.bridge12_begin(device, cmd);
        if (!vk) return;
        handles = session.vulkan_handles();
        t = session.bridge12_twin(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        d = session.bridge12_twin(f.depth, f.depth_state, false);
        m = session.bridge12_twin(f.motion, f.motion_state, false);
        if (f.exposure) e = session.bridge12_twin(f.exposure, f.exposure_state, false);
    });
    if (!vk) return false;
    const auto give_up = [] { nr::dlssnr::with_d3d12_session([](nr::pe::Session& session) { session.bridge12_abandon(); }); };
    if (!t.image || !d.image || !m.image) { give_up(); return false; }
    const auto image = [](const Raw& r) {
        Wilsjo2VkImage i;
        i.image = r.image; i.format = r.format; i.width = r.width; i.height = r.height; i.layout = r.layout; i.usage = r.usage;
        return i;
    };
    wilsjo2_vk_device(handles.device, handles.physical);
    if (!o.vk) {
        o.vk = wilsjo2_vk_owner();
        wilsjo2_vk_owner_bridge12(*o.vk);
        if (o.at_present) wilsjo2_vk_owner_at_present(*o.vk);
    }
    g_bridged_runs = true;
    Wilsjo2VkFrame vf;
    vf.depth = image(d);
    vf.motion = image(m);
    if (e.image) vf.exposure = image(e);
    vf.flags = f.flags;
    vf.hdr = vf.hdr_before = f.hdr;   // the caller's word for the image processed, at either placement
    vf.reset = f.reset;
    vf.pre_exposure = f.pre_exposure;
    vf.mv_x = f.mv_x; vf.mv_y = f.mv_y;
    vf.depth_rect = f.depth_rect; vf.motion_rect = f.motion_rect;
    vf.out_w = f.out_w; vf.out_h = f.out_h;
    const bool ran = wilsjo2_run_vk(vk, *o.vk, vf, s, image(t), w, h, before_upscale);
    if (!ran) { give_up(); return false; }
    nr::dlssnr::with_d3d12_session([](nr::pe::Session& session) { session.bridge12_end(); });
    return true;
}
}  // namespace

namespace {
// ---- the native runtime with ASD: the flow in the game's list, its programs D3D12 compute, its network ASD's ----
// Whether this run takes it: "" yes; else why not (the run takes the bridge, as 0.0.8.1 does). Decided before anything
// of the run is recorded, so a frame's NR runs once, on one route.
std::string native_refusal(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const Wilsjo2Owner& o, const Wilsjo2Frame& f,
                           const w2::Settings& s, ID3D12Resource* target, uint32_t w, uint32_t h, w2::Stage** stage,
                           native::Stage12** impl) {
    if (!nr::pe::asd::enabled()) return "ASD is off (dlssnr-amd-asd.ini)";
    if (w2::fast_requested(s)) return "Passes (fast) is not on the D3D12 route (ASD)";
    if (nr::dlssnr::preprocess_settings().active()) return "[Preprocess] is on (its network calls are not ASD's)";
    const auto readable = [](ID3D12Resource* r, const char* what) -> std::string {
        const D3D12_RESOURCE_DESC d = r->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1)
            return std::string("the ") + what + " is not a single-sample 2-D texture";
        if (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) return std::string("the ") + what + " cannot be read by a shader";
        if (native::srv_format(d.Format) == DXGI_FORMAT_UNKNOWN)
            return std::string("the ") + what + "'s format " + std::to_string(int(d.Format)) + " (sRGB) is the bridge's";
        return "";
    };
    for (auto [r, what] : {std::pair<ID3D12Resource*, const char*>{target, "frame"}, {f.depth, "depth"}, {f.motion, "motion"}})
        if (std::string y = readable(r, what); !y.empty()) return y;
    const D3D12_RESOURCE_DESC td = target->GetDesc();
    if (!(td.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || native::uav_format(td.Format) == DXGI_FORMAT_UNKNOWN)
        return "the frame cannot be written by a compute shader (no UAV)";
    if (f.exposure) {
        const D3D12_RESOURCE_DESC ed = f.exposure->GetDesc();
        if (ed.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && native::srv_format(ed.Format) == DXGI_FORMAT_UNKNOWN)
            return "the exposure texture's format is the bridge's";
    }
    std::string why;
    *stage = native::programs(device, &why);
    if (!*stage) return why;
    *impl = native::programs_impl();
    if (!*impl) return "its D3D12 programs are being made";
    uint32_t mw = w, mh = h;
    w2::flow_model_extent(s, w, h, o.flow, true, &mw, &mh);
    if (!nr::pe::asd::wilsjo2_ready(device, cmd, mw, mh, &why)) return why;
    return "";
}
}  // namespace

bool wilsjo2_run(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, Wilsjo2Owner& o, const Wilsjo2Frame& f,
                 const w2::Settings& s, ID3D12Resource* target, uint32_t w, uint32_t h, bool before_upscale) {
    gpu_uses.touch();
    w2::Stage* nstage = nullptr;
    native::Stage12* nimpl = nullptr;
    if (target && f.depth && f.motion && device) {
        bool bridged = false;
        nr::dlssnr::with_d3d12_session([&](nr::pe::Session& session) { bridged = session.bridged(device); });
        if (bridged) {
            o.capture_layer = false;
            const std::string why = native_refusal(cmd, device, o, f, s, target, w, h, &nstage, &nimpl);
            if (!why.empty()) {
                if (nr::pe::asd::enabled()) native::say_route("the Vulkan bridge (the 0.0.8.1 route) for now: " + why);
                return run_bridged(cmd, device, o, f, s, target, w, h, before_upscale);
            }
            native::say_route(std::string("on the D3D12 route (canary ") + nr::pe::asd::canary_name(nr::pe::asd::canary()) +
                              "): its programs D3D12 compute, its network ACO through ASD");
        }
    }
    o.capture_layer = false;
    if (!target || !f.depth || !f.motion) return false;
    D3d12Io io(cmd, device, o, &f, target, before_upscale, w, h);
    if (nstage) io.go_native(nstage, nimpl);
    w2::FlowFrame ff;
    ff.target = {target};
    ff.width = w; ff.height = h;
    const DXGI_FORMAT format = io.frame_format();
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R16G16B16A16_UNORM:
            ff.unorm = true;
            break;
        default:
            break;
    }
    ff.direct_ok = true;   // D3D12: the frame is a UAV the stage reads and writes at its extent
    ff.depth = {f.depth};
    ff.motion = {f.motion};
    {
        const D3D12_RESOURCE_DESC dd = f.depth->GetDesc(), md = f.motion->GetDesc();
        char g[192];
        std::snprintf(g, sizeof g, "%u:%llux%u:%u:%llux%u:%u,%u,%u,%u:%u,%u,%u,%u", unsigned(dd.Format),
                      (unsigned long long)dd.Width, dd.Height, unsigned(md.Format), (unsigned long long)md.Width, md.Height,
                      f.depth_rect.x, f.depth_rect.y, f.depth_rect.width, f.depth_rect.height, f.motion_rect.x,
                      f.motion_rect.y, f.motion_rect.width, f.motion_rect.height);
        ff.guide_signature = g;
    }
    ff.depth_rect = {f.depth_rect.x, f.depth_rect.y, f.depth_rect.width, f.depth_rect.height};
    ff.motion_rect = {f.motion_rect.x, f.motion_rect.y, f.motion_rect.width, f.motion_rect.height};
    if (f.exposure) {
        ff.exposure = {f.exposure};
        const D3D12_RESOURCE_DESC ed = f.exposure->GetDesc();
        // Any size, as the fork binds it: the programs read its texel (0, 0) only (DL:TB's is 2 x 1, R32_TYPELESS,
        // read as float by the lookup). A 2-D single-sample texture is what a sampler2D can be bound to.
        ff.exposure_valid = ed.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && ed.SampleDesc.Count == 1 &&
                            ed.DepthOrArraySize == 1;
    }
    ff.flags = f.flags;
    ff.hdr = f.hdr;
    ff.reset = f.reset;
    ff.pre_exposure = f.pre_exposure;
    ff.mv_x = f.mv_x; ff.mv_y = f.mv_y;
    ff.preprocess = nr::dlssnr::preprocess_settings();
    ff.before_upscale = before_upscale;
    ff.at_present = o.at_present;
    if (!nstage) return w2::flow_run(io, o.flow, ff, s, uint64_t(format));
    // the native route: timed (the stage's programs and the ASD network together, as the bridge's "network" time is)
    native::g_timing.begin(device, cmd, w, h);
    const bool ran = w2::flow_run(io, o.flow, ff, s, uint64_t(format));
    native::g_timing.end(cmd);
    if (ran && o.vk) {   // the bridge's own surfaces of this owner, no longer used
        wilsjo2_vk_release(o.vk);
        o.vk = nullptr;
    }
    if (!ran) native::say_route("a run on the D3D12 route was not recorded (see above); the frame passes through");
    return ran;
}

bool wilsjo2_convert(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* source,
                     ID3D12Resource* original, ID3D12Resource* target, uint32_t w, uint32_t h, bool to_linear) {
    if (device && source && target && (to_linear || original) && nr::pe::asd::enabled()) {
        // HDR10 at Present on the native runtime with ASD: the conversion as D3D12 compute where its program is made (the
        // caller has source / original readable, target writable)
        bool bridged = false;
        nr::dlssnr::with_d3d12_session([&](nr::pe::Session& session) { bridged = session.bridged(device); });
        std::string why;
        w2::Stage* st = bridged ? native::programs(device, &why) : nullptr;
        native::Stage12* impl = st ? native::programs_impl() : nullptr;
        const w2::Mode mode = to_linear ? w2::kFinishedToLinear : w2::kFinishedToPq;
        if (impl && st->has(mode) && nr::pe::split::prepare(cmd, &why)) {
            const auto saved = nr::pe::split::snapshot(cmd);
            impl->begin(cmd, [](ID3D12Resource*, D3D12_RESOURCE_STATES) {});
            bool ok = true;
            try {
                w2::Dispatch d;
                d.t0 = native::image(source);
                if (!to_linear) d.t2 = native::image(original);
                d.u0 = native::image(target);
                d.constants.mode = mode;
                d.constants.width = w;
                d.constants.height = h;
                st->record(VK_NULL_HANDLE, d);
            } catch (const std::exception& x) {
                log("[asd] ERROR: wilsjo2 on D3D12: the HDR10 conversion: %s", x.what());
                ok = false;
            }
            impl->end();
            nr::pe::split::restore(cmd, saved);
            return ok;
        }
    }
    if (g_stage_retry.blocked()) return false;
    bool recorded = false;
    nr::dlssnr::with_d3d12_session([&](nr::pe::Session& session) {
        recorded = session.record_raw(device, cmd, [&](VkCommandBuffer vk, const HostDevice& host,
                                                       const nr::pe::Session::RawLookup& lookup) {
            if (!g_stage) {
                try {
                    if (stage_fail_test()) throw std::runtime_error("NR_TEST_STAGE_FAIL");
                    g_stage = std::make_unique<w2::Stage>(host, std::string(nr::pe::module_folder()) +
                                                                    "\\dlssnr-amd\\shaders\\wilsjo2");
                    g_stage_retry.made();
                } catch (const std::exception& x) {
                    g_stage_retry.fail(x.what());
                    return false;
                }
            }
            w2::Dispatch d;
            d.t0 = image_of(lookup(source, kSrv, true));
            if (!to_linear) d.t2 = image_of(lookup(original, kSrv, true));
            d.u0 = image_of(lookup(target, kUav, true));
            if (!d.t0.format || !d.u0.format || (!to_linear && !d.t2.format)) return false;
            d.constants.mode = to_linear ? w2::kFinishedToLinear : w2::kFinishedToPq;
            d.constants.width = w;
            d.constants.height = h;
            if (!g_stage->has(w2::Mode(d.constants.mode))) return false;   // the conversion's program missing
            g_stage->record(vk, d);
            return true;
        });
        if (g_stage) g_stage->end_recording();
    });
    return recorded;
}

bool wilsjo2_hudfix(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const HudFixD3D12& r, uint32_t w, uint32_t h,
                    uint32_t source, bool show_mask, bool fresh, uint32_t parity, bool estimate_too) {
    if (g_stage_retry.blocked()) return false;
    bool recorded = false;
    nr::dlssnr::with_d3d12_session([&](nr::pe::Session& session) {
        recorded = session.record_raw(device, cmd, [&](VkCommandBuffer vk, const HostDevice& host,
                                                       const nr::pe::Session::RawLookup& lookup) {
            if (!g_stage) {
                try {
                    if (stage_fail_test()) throw std::runtime_error("NR_TEST_STAGE_FAIL");
                    g_stage = std::make_unique<w2::Stage>(host, std::string(nr::pe::module_folder()) +
                                                                    "\\dlssnr-amd\\shaders\\wilsjo2");
                    g_stage_retry.made();
                } catch (const std::exception& x) {
                    g_stage_retry.fail(x.what());
                    return false;
                }
            }
            const w2::Image back = image_of(lookup(r.back, kUav, true)), frame = image_of(lookup(r.frame, kUav, true)),
                            alpha = image_of(lookup(r.alpha, kUav, true)), stats = image_of(lookup(r.stats, kUav, true));
            const w2::Image ref = source == 4 ? image_of(lookup(r.previous, kUav, true))
                                  : r.hudless ? image_of(lookup(r.hudless, kUav, true)) : back;
            const bool warm = estimate_too && source == 2 && r.previous;
            const w2::Image motion = r.motion ? image_of(lookup(r.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false))
                                              : w2::Image{};
            const w2::Image prev = warm ? image_of(lookup(r.previous, kUav, true)) : ref;
            if (!back.format || !frame.format || !alpha.format || !stats.format || !ref.format || !prev.format) return false;
            uint32_t src = source;
            const auto run = [&](uint32_t mode, const w2::Image& t0, const w2::Image* t1, const w2::Image* t2,
                                 const w2::Image* t3, const w2::Image& u0, uint32_t width, uint32_t height) {
                w2::Dispatch d;
                d.t0 = t0;
                if (t1) d.t1 = *t1;
                if (t2) d.t2 = *t2;
                if (t3) d.t3 = *t3;
                d.u0 = u0;
                d.u1 = stats;
                d.constants = w2::hudfix_constants(mode, width, height, src, show_mask, fresh, parity);
                if (mode == w2::kHudEstimate && motion.format) {   // the estimate with the game's motion vectors
                    d.t2 = motion;
                    w2::hudfix_motion(d.constants, r.motion_scale_x, r.motion_scale_y, r.motion_x, r.motion_y,
                                      r.motion_step_x, r.motion_step_y);
                }
                g_stage->record(vk, d);
            };
            try {
                run(w2::kHudClear, back, nullptr, nullptr, nullptr, frame, 1, 1);
                if (warm) {   // the estimate's state, this frame against the last
                    src = 4;
                    run(w2::kHudCount, back, &prev, nullptr, nullptr, frame, w, h);
                    run(w2::kHudEstimate, back, &prev, nullptr, nullptr, alpha, w, h);
                    src = source;
                }
                if (source == 2 || source == 4) run(w2::kHudCount, back, &ref, nullptr, nullptr, frame, w, h);
                if (source == 4) run(w2::kHudEstimate, back, &ref, nullptr, nullptr, alpha, w, h);
                run(w2::kHudCompose, back, &ref, &frame, &alpha, frame, w, h);
            } catch (const std::exception& x) {
                log("[nr] HUD FIX: %s", x.what());
                return false;
            }
            return true;
        });
        if (g_stage) g_stage->end_recording();
    });
    return recorded;
}

bool wilsjo2_inspect(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* before, ID3D12Resource* after,
                     ID3D12Resource* target, uint32_t w, uint32_t h, uint32_t compare, float split, float zoom, bool swap,
                     bool apply_model) {
    if (g_stage_retry.blocked()) return false;
    bool recorded = false;
    nr::dlssnr::with_d3d12_session([&](nr::pe::Session& session) {
        recorded = session.record_raw(device, cmd, [&](VkCommandBuffer vk, const HostDevice& host,
                                                       const nr::pe::Session::RawLookup& lookup) {
            if (!g_stage) {
                try {
                    if (stage_fail_test()) throw std::runtime_error("NR_TEST_STAGE_FAIL");
                    g_stage = std::make_unique<w2::Stage>(host, std::string(nr::pe::module_folder()) +
                                                                    "\\dlssnr-amd\\shaders\\wilsjo2");
                    g_stage_retry.made();
                } catch (const std::exception& x) {
                    g_stage_retry.fail(x.what());
                    return false;
                }
            }
            w2::Dispatch d;
            d.t0 = image_of(lookup(before, kSrv, true));
            d.t1 = image_of(lookup(after, kSrv, true));
            d.u0 = image_of(lookup(target, kUav, true));
            if (!d.t0.format || !d.t1.format || !d.u0.format) return false;
            d.constants.mode = w2::kInspect;
            d.constants.width = w;
            d.constants.height = h;
            d.constants.compare_mode = compare;
            d.constants.compare_split = split;
            d.constants.compare_zoom = zoom;
            d.constants.compare_swap = swap ? 1u : 0u;
            d.constants.apply_model = apply_model ? 1u : 0u;
            try {
                g_stage->record(vk, d);
            } catch (const std::exception& x) {
                log("[nr] NR comparison: %s", x.what());
                return false;
            }
            return true;
        });
        if (g_stage) g_stage->end_recording();
    });
    return recorded;
}

}  // namespace nr::asi
