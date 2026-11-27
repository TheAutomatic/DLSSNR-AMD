#include "nr_frame_pair.hpp"
#include "nr_frame_chain.hpp"
#include "../core/nr_frame_reuse_policy.hpp"
#include "nr_pe_control.hpp"
#include "nr_path_u8.hpp"
#include "nr_gpu_completion.hpp"
// The wilsjo2 style's per-frame flow on the .asi route: see nr_asi_wilsjo2.hpp. Section numbers are those of
// our specification of the style (not published). Where the forked OptiScaler as the 0.0.7 route ran it differs from the
// specification by one of our OptiScaler fixes (nr_pe_optifix.cpp), the fix is what is followed, and said so below.
#include "nr_asi_wilsjo2.hpp"
#include "nr_asi_wilsjo2_d3d12.hpp"
#include "nr_d3d12_direct_state.hpp"
#include "nr_d3d12_passthrough.hpp"
#include "nr_d3d12_passthrough_backend.hpp"
#include "nr_frame_reuse_d3d12.hpp"

#include "nr_frame_pair.hpp"
#include "nr_frame_chain.hpp"
#include "nr_pe_config.hpp"
#include "nr_pe_interop.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_renodx_route.hpp"
#include "nr_pe_session.hpp"
#include "nr_vram_ledger.hpp"
#include "nr_wilsjo2_flow.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
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


// ---- the native D3D12 route: the stage's programs as D3D12 compute --------------------------------------------
// On the D3D12 passthrough (nr_d3d12_passthrough.hpp) once its check passed, the flow below records into the game's own
// list: the wilsjo2 programs loaded as pre-captured PAL ELFs from d3d12_outer.nra (zero SetDescriptorHeaps, zero
// d3dcompiler_47), with the network through nr::dlssnr::evaluate_d3d12 (the D3D12 passthrough's ACO network).
}  // namespace
namespace native {   // with the stage (nr_asi_wilsjo2_d3d12.hpp)

// The programs, made once on a thread of ours (no frame waits on the archive loader); the frames before take the
// bridge.
struct Programs {
    std::mutex m;
    std::condition_variable cv;
    int state = 0;   // 0 not started, 1 making, 2 made, 3 failed
    std::string why;
    ID3D12Device* device = nullptr;
    std::unique_ptr<w2::Stage> stage;   // over `impl`
    Stage12* impl = nullptr;
};
Programs g_programs;

// The recordings a replaced stage's last frames belong to: the strict check (every ticket of the registry that has not
// finished), so a stage of the previous device is freed once the GPU ran past its work instead of being leaked.
nr::gpu::Uses pending_uses() {
    std::vector<nr::gpu::Ticket> all;
    {
        std::lock_guard<std::mutex> g(nr::gpu::registry_mutex);   // the registry's lock only: completed() is asked after
        for (auto& [key, t] : nr::gpu::recordings) all.push_back(t);
    }
    nr::gpu::Uses u;
    for (auto& t : all)
        if (t && !nr::pe::d3d12_direct::completed(t)) u.tickets.push_back(t);
    return u;
}
nr::gpu::Uses merged_uses(nr::gpu::Uses a, const nr::gpu::Uses& b) {
    for (const auto& t : b.tickets)
        if (t && std::find(a.tickets.begin(), a.tickets.end(), t) == a.tickets.end()) a.tickets.push_back(t);
    return a;
}
struct RetiredStage {
    std::unique_ptr<w2::Stage> stage;
    nr::gpu::Uses uses;
};
std::vector<RetiredStage> g_retired_stages;

void sweep_retired_stages() {
    for (size_t i = 0; i < g_retired_stages.size();) {
        bool done = true;
        for (const auto& t : g_retired_stages[i].uses.tickets)
            if (t && !nr::pe::d3d12_direct::completed(t)) { done = false; break; }
        if (done) g_retired_stages.erase(g_retired_stages.begin() + long(i));
        else ++i;
    }
}

// Ready on `device`: the stage; else null and why (making them is started on the first call).
w2::Stage* programs(ID3D12Device* device, std::string* why, bool* building = nullptr) {
    if (building) *building = false;
    std::lock_guard<std::mutex> l(g_programs.m);
    if (g_programs.state == 2 && g_programs.device == device) return g_programs.stage.get();
    if (g_programs.state == 3 && g_programs.device == device) { *why = "its D3D12 programs: " + g_programs.why; return nullptr; }
    if (g_programs.state == 1) { if (building) *building = true; *why = "its D3D12 programs are being made"; return nullptr; }
    // first use, or another device: (re)made
    sweep_retired_stages();
    if (g_programs.stage) {
        // The stage's own recordings first: the registry only holds the recordings still registered, not every ticket a
        // stage's slots were used by.
        nr::gpu::Uses keep = pending_uses();
        if (g_programs.impl) keep = merged_uses(std::move(keep), g_programs.impl->uses());
        g_retired_stages.push_back({std::move(g_programs.stage), std::move(keep)});
    }
    g_programs.impl = nullptr;
    if (g_programs.device) g_programs.device->Release();
    device->AddRef();
    g_programs.device = device;
    g_programs.state = 1;
    if (building) *building = true;
    *why = "its D3D12 programs are being made";
    const std::wstring folder = nr::u8::wide(std::string(nr::pe::module_folder()) + "\\dlssnr-amd\\shaders\\d3d12");
    nr::pe::log("[d3d12] wilsjo2 on D3D12: making its programs (until they are made these frames pass through "
                "with no NR; the bridge is not used)");
    device->AddRef();
    std::thread([device, folder] {
        auto s = std::make_unique<Stage12>();
        s->set_tickets([](ID3D12GraphicsCommandList* l) { return nr::pe::d3d12_direct::book(l); },
                       [](const nr::gpu::Ticket& t) { return nr::pe::d3d12_direct::completed(t); });
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
            g_programs.cv.notify_all();
        }
        if (ok) nr::pe::log("[d3d12] wilsjo2 on D3D12: its programs are made (%.0f ms; PAL ELF compute pipelines)", ms);
        else nr::pe::log("[d3d12] ERROR: wilsjo2 on D3D12: its programs cannot be made: %s (the bridge runs it)", e.c_str());
        device->Release();
    }).detach();
    return nullptr;
}
// Standalone Present/utility callers wait briefly if the background loader just started so frame 1 stays on Direct.
w2::Stage* programs_wait(ID3D12Device* device, std::string* why) {
    if (w2::Stage* st = programs(device, why)) return st;
    std::unique_lock<std::mutex> l(g_programs.m);
    g_programs.cv.wait_for(l, std::chrono::milliseconds(250), [&] {
        return g_programs.device != device || g_programs.state != 1;
    });
    if (g_programs.device == device && g_programs.state == 2) return g_programs.stage.get();
    if (why) *why = g_programs.state == 3 ? ("its D3D12 programs: " + g_programs.why) : "its D3D12 programs are being made";
    return nullptr;
}

Stage12* programs_impl() {
    std::lock_guard<std::mutex> l(g_programs.m);
    return g_programs.state == 2 ? g_programs.impl : nullptr;
}

// The route's reason, said when it changes for this owner.
void say_route(std::string& last, const std::string& line) {
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    if (line == last) return;
    last = line;
    nr::pe::log("[d3d12] wilsjo2 style: %s", line.c_str());
}

// The GPU time of a native run (the stage's programs and the D3D12 network together): two timestamps a run, a ring of
// kRing runs read back kRing runs later.
class Timing {
public:
    void begin(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, uint32_t w, uint32_t h) {
        if (device_ != device) release();   // the queries and the readback belong to the device that made them
        device_ = device;
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
        // The pair is the CPU's to read only once the recording that resolved it finished (the run itself may still be
        // queued, and reading a slot's old contents would be a sample of another run).
        ticket_[slot_] = nr::gpu::current();
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
        if (!map_) nr::pe::log("[d3d12] wilsjo2 on D3D12: its GPU time cannot be measured (no timestamp queries)");
    }
    void read(uint32_t slot) {
        if (ticket_[slot] && !nr::pe::d3d12_direct::completed(ticket_[slot])) return;   // not written yet: no sample, not a guess
        uint64_t t[2];
        std::memcpy(t, map_ + uint64_t(slot) * 16, 16);
        if (!t[0] || t[1] <= t[0]) return;
        const double ms = double(t[1] - t[0]) * 1000.0 / hz_;
        if (ms > 1000.0) return;   // not (yet) a pair of this run's
        ++n_; sum_ += ms;
        nr::dlssnr::note_native_ms(float(ms));   // NR Control's line
        min_ = n_ - n_base_ == 1 ? ms : std::min(min_, ms);
        max_ = std::max(max_, ms);
        if (n_ <= 3)
            nr::pe::log("[d3d12] wilsjo2 on D3D12: one run took %.3f ms on the GPU (%ux%u: the wilsjo2 programs + the D3D12 network)",
                        ms, w_, h_);
        if (n_ % 300 == 0) {
            nr::pe::log("[d3d12] wilsjo2 on D3D12: runs %llu-%llu: GPU avg %.3f ms (min %.3f, max %.3f), %ux%u; the network's own "
                        "share: the [d3d12] timing lines",
                        (unsigned long long)(n_ - 299), (unsigned long long)n_, sum_ / 300.0, min_, max_, w_, h_);
            sum_ = 0; min_ = 0; max_ = 0; n_base_ = n_;
        }
    }
    void release() {
        made_ = false;
        map_ = nullptr;
        if (buf_) { buf_->Unmap(0, nullptr); buf_->Release(); buf_ = nullptr; }
        if (heap_) { heap_->Release(); heap_ = nullptr; }
        for (auto& t : ticket_) t.reset();
        runs_ = 0; slot_ = 0;
    }
    bool made_ = false;
    ID3D12Device* device_ = nullptr;   // the device that made the queries (a swap drops them)
    ID3D12QueryHeap* heap_ = nullptr;
    ID3D12Resource* buf_ = nullptr;
    uint8_t* map_ = nullptr;
    double hz_ = 0;
    uint64_t runs_ = 0, n_ = 0, n_base_ = 0;
    nr::gpu::Ticket ticket_[kRing];   // the recording that resolved each slot (null: not resolved by us)
    uint32_t slot_ = 0, w_ = 0, h_ = 0;
    double sum_ = 0, min_ = 0, max_ = 0;
};
Timing g_timing;

}  // namespace native

void wilsjo2_abandon_at_exit() {
    (void)g_stage.release();
    // the D3D12 passthrough's stage and the replaced ones: never destroyed at the process's end (as g_stage)
    (void)native::g_programs.stage.release();
    new decltype(native::g_retired_stages)(std::move(native::g_retired_stages));
}

// One OptiScaler feature's wilsjo2 state at one placement: the flow's (nr_wilsjo2_flow.hpp) and the passes' models
// (each pass its own feature, so its own history).
struct Wilsjo2Owner {
    bool capture_layer = false;
    bool at_present = false;   // wilsjo2_owner_at_present
    // FrameReuse on an SDR frame on the bridge (or vkd3d-proton): the flow inside the model's session through this feature
    // (run_in_session); else the flow here (own_flow: its state is there to let go of when the other runs)
    nr::dlssnr::Feature* session = nullptr;
    uint32_t session_w = 0, session_h = 0;
    bool own_flow = false, reuse_hdr_said = false;
    // FrameReuse on the D3D12 passthrough: its stage around the flow here (nr_frame_reuse_d3d12.hpp, run_native_reuse;
    // the Vulkan session's ReuseFields)
    std::unique_ptr<nr::pe::d3d12_passthrough::frame_reuse12::Stage> reuse12, pair_reuse12;
    ID3D12Resource *group_linear=nullptr,*group_pq=nullptr;
    w2::Settings reuse_settings{};
    std::chrono::steady_clock::time_point alloc_retry_after{};
    std::string reuse_failed, reuse_note, reuse_ui;
    std::set<std::string> reuse_said;
    std::string route_said, native_run_said;
    uint64_t reuse_frames = 0, reuse_epoch = 0;
    nr::pe::d3d12_passthrough::frame_reuse12::Stage::Stats reuse_window{}, reuse_logged{};
    float reuse_share = -1.f;
    // even frames: the game's motion vectors copied for the flow (its second part runs after the game's next frame, so it
    // must not touch the game's own resources); NON_PIXEL_SHADER_RESOURCE between uses
    nr::Preprocess reuse_prep{};   // [Preprocess] its frame reuse last ran with (a change restarts it, as a setting's)
    w2::FlowState flow;
    std::vector<nr::dlssnr::Feature*> models;      // pass k's at [k]
    std::map<ID3D12Resource*, bool> colour;        // our surfaces: colour-family (else read as a single-channel image)
    Wilsjo2VkOwner* vk = nullptr;                  // on the native runtime: the Vulkan flow's (run_bridged)
};

Wilsjo2Owner* wilsjo2_owner() { return new Wilsjo2Owner(); }
void wilsjo2_owner_at_present(Wilsjo2Owner& o) { o.at_present = true; }
namespace {
std::atomic<uint32_t> g_present_runs{0};   // wilsjo2_note_present_runs: the layers this Present runs
}  // namespace
void wilsjo2_note_present_runs(uint32_t runs) { g_present_runs.store(runs); }

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
        case w2::Fmt::kFrame: {
            const DXGI_FORMAT u = native::uav_format(frame);
            return u != DXGI_FORMAT_UNKNOWN ? u : frame;
        }
        case w2::Fmt::kHalf: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case w2::Fmt::kR32F: return DXGI_FORMAT_R32_FLOAT;
        case w2::Fmt::kRG32F: return DXGI_FORMAT_R32G32_FLOAT;
        case w2::Fmt::kF32: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case w2::Fmt::kRG16F: return DXGI_FORMAT_R16G16_FLOAT;
        case w2::Fmt::kR16F: return DXGI_FORMAT_R16_FLOAT;
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
    bool has_fast() const override { return nstage_ ? nstage_->has_fast() : !g_stage || g_stage->has_fast(); }
    bool has_il() const override { return nstage_ ? nstage_->has_il() : !g_stage || g_stage->has_il(); }
    bool stage_made() const override { return nstage_ != nullptr || g_stage != nullptr; }
    bool style_graded() const override {
        bool on = false;
        nr::dlssnr::with_d3d12_session([&](nr::pe::Session& s) { on = s.style_grade_active(); });
        return on;
    }
    bool style_grade_accelerated() const override {
        bool on = false;
        nr::dlssnr::with_d3d12_session([&](nr::pe::Session& s) { on = s.style_grade_accelerated(); });
        return on;
    }
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
        made_ = true;
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
        // on the D3D12 flow the model's evaluate hands the call to the D3D12 passthrough (its network on the game's device)
        struct NativeCall {
            explicit NativeCall(bool on) : on_(on) { if (on_) nr::dlssnr::set_d3d12_native_call(true); }
            ~NativeCall() { if (on_) nr::dlssnr::set_d3d12_native_call(false); }
            bool on_;
        } native_call(nstage_ != nullptr);
        const auto rect = [](const w2::Rect& r) { nr::dlssnr::Rect o; o.x = r.x; o.y = r.y; o.width = r.width; o.height = r.height; return o; };
        route::HistoryRef hist;
        hist.d3d12 = c.history.h;   // (Interleaved: our resource, in UNORDERED_ACCESS)
        hist.raw = c.raw;
        hist.external = c.external_history;
        route::CallHistory history(hist);
        // frame reuse's even frames: the calls before the cut here, the one with it split, the rest (and all after) there
        route::NetSplit ns;
        std::optional<route::CallNetSplit> net_split;
        if (nstage_ && split_tail && cmd_ != split_tail) {
            const uint32_t lo = c.pass * split_steps;
            if (split_cut <= lo) cmd_ = split_tail;
            else if (split_steps && split_cut < lo + split_steps) { ns.tail = split_tail; ns.cut = split_cut - lo; net_split.emplace(&ns); }
        }
        const int result = nr::dlssnr::evaluate_d3d12(
            cmd_, o_.models[c.pass], nullptr, static_cast<ID3D12Resource*>(c.input.h), static_cast<ID3D12Resource*>(c.depth.h),
            static_cast<ID3D12Resource*>(c.motion.h), static_cast<ID3D12Resource*>(c.output.h), c.width, c.height,
            rect(c.depth_rect), rect(c.motion_rect), c.depth_inverted ? 1 : 0, c.reset ? 1 : 0,
            controls_of(c.controls, c.noise_every_frame, c.proxy_curve, c.last), c.mv_x, c.mv_y);
        if (net_split) cmd_ = split_tail;   // (the call's rest went there: what follows it too)
        route::end_run();
        return result == int(NVSDK_NGX_Result_Success) ? 0 : 1;
    }
    void passes_begin(const w2::FlowFacts& x) override {
        namespace route = nr::pe::renodx_route;
        style_.emplace(false);
        facts_.hdr = x.hdr; facts_.exposure = int(x.exposure); facts_.asked = x.asked; facts_.white = x.white;
        facts_.history = x.history; facts_.mode = x.mode; facts_.passes = x.passes;
        facts_.pre_history = x.pre_history; facts_.noise_every_frame = x.noise_every_frame;
        facts_.prep_outside = x.prep_outside; facts_.prep_missing = x.prep_missing;
        facts_.fast_passes = x.fast_passes; facts_.fast_fallback = x.fast_fallback; facts_.fast_fallback_spatial = x.fast_fallback_spatial; facts_.il_missing = x.il_missing;
        if (reuse_state) { facts_.reuse = reuse_state; facts_.reuse_note = reuse_ui; facts_.reuse_share = reuse_share; }
        if (history >= 0.f) facts_.history = history;   // (the network's history strength: evaluate_d3d12 reads it here)
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
        if (nstage_) return;   // the native route: no Vulkan recording to mark (NR Control's time: the [d3d12] timing lines)
        session_->set_call_layer(nr::pe::renodx_route::call_layer());   // the run's whole time, for NR Control
        session_->outer_begin(vk);
        // F5 (nr_screenshot.hpp): the frame before this layer's NR, while a capture is armed
        // (a Present whose screenshot pairs the frame that frame reuse shows with its input takes the pair itself)
        if (!nr::frame_pair::Observation::current)
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
    bool made() const { return made_; }   // a pass's feature was made this run (the flow leaves that frame as it came)

private:
    // One recording of the flow's into the game's list as D3D12 compute: the list's compute state saved and
    // restored on nimpl_->end(), the resources put in each dispatch's states through this route's tracking.
    bool record_native(const Recorder& fn) {
        std::string why;
        if (!nr::pe::d3d12_direct::ready(device_, cmd_, &why)) {
            log("[d3d12] wilsjo2 on D3D12: a recording could not be made: the command list's state is not known (%s)", why.c_str());
            return false;
        }
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
            if (said++ < 5) log("[d3d12] ERROR: wilsjo2 on D3D12: %s", x.what());
            ok = false;
        }
        nimpl_->end();
        nimpl_->end_recording();
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
    bool made_ = false;
    std::map<ID3D12Resource*, State> states_;
    nr::pe::Session* session_ = nullptr;
    w2::Stage* nstage_ = nullptr;
    native::Stage12* nimpl_ = nullptr;
    std::optional<nr::pe::renodx_route::CallStyle> style_;
    nr::pe::renodx_route::Wilsjo2Facts facts_;
    std::optional<nr::pe::renodx_route::CallWilsjo2> told_;
    nr::pe::renodx_route::Frame rf_;
public:
    // FrameReuse in NR Control (Wilsjo2Facts): 0 off (or said elsewhere), 1 runs (reuse_share of frames with NR lately), 2
    // cannot run (reuse_ui); the network's history strength in the layer's place (frame reuse: a history two frames old
    // is followed half as strongly), < 0 the layer's
    uint32_t reuse_state = 0;
    const char* reuse_ui = "";
    float reuse_share = -1.f;
    float history = -1.f;
    // Frame reuse's even frames (the D3D12 passthrough): the network calls' steps from `split_cut` on, of `split_steps` a
    // call, go into `split_tail` (the stage's second part), and so does everything this flow records after that point
    ID3D12GraphicsCommandList* split_tail = nullptr;
    uint32_t split_cut = 0, split_steps = 0;
};

// FrameReuse on an SDR frame where the D3D12 passthrough does not run the layer (the bridge; vkd3d-proton): the layer's
// flow inside the model's session, frame reuse around it (nr_pe_session.cpp record_wilsjo2; the route marker
// renodx_route::CallW2Present), through the owner's own NR feature at the frame's extent. The flow here (its surfaces,
// its passes' features, the bridge's Vulkan flow of run_bridged) goes first: one of them runs the layer at a time.
bool run_in_session(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, Wilsjo2Owner& o, const Wilsjo2Frame& f,
                    const w2::Settings& s, ID3D12Resource* target, uint32_t w, uint32_t h, bool before_upscale) {
    namespace route = nr::pe::renodx_route;
    if (!target || !f.motion || !device) return false;   // (as the flow here: both guides, or no run)
    if (o.vk) { wilsjo2_vk_release(o.vk); o.vk = nullptr; }
    o.reuse12.reset();   // (the passthrough's frame reuse: its images retire through its watermark)
    if (o.own_flow) {   // (its sizes too: a later run of the flow here allocates afresh)
        D3d12Io io(nullptr, nullptr, o, nullptr, nullptr, false, 0, 0);
        w2::flow_release(io, o.flow);
        o.flow = w2::FlowState{};
        o.own_flow = false;
    }
    if (o.session && (o.session_w != w || o.session_h != h)) { nr::dlssnr::release_d3d12(o.session); o.session = nullptr; }
    if (!o.session) {
        int rc = 0;
        o.session = nr::dlssnr::create_d3d12(device, cmd, nullptr, w, h, 0, 1, nr::dlssnr::Controls6{}, &rc);
        if (!o.session) {
            log("[nr] wilsjo2 style: the frame reuse's NR feature could not be made (0x%08X): %s", unsigned(rc),
                nr::dlssnr::last_error());
            return false;
        }
        o.session_w = w; o.session_h = h;
    }
    route::W2Present wp;
    wp.settings = &s; wp.hdr = f.hdr;
    wp.at_present = o.at_present; wp.before_upscale = before_upscale; wp.pre_exposure = f.pre_exposure;
    route::CallW2Present present(&wp);
    route::CallStyle style(false);
    // what the flow here tells the route of its frame (D3d12Io::passes_begin / network)
    route::note_pass(f.flags, 1.0f, f.depth_rect.width, f.depth_rect.height, f.out_w ? f.out_w : w, f.out_h ? f.out_h : h);
    route::Frame rf;
    rf.before_upscale = before_upscale;
    rf.pre_exposure = f.pre_exposure;
    route::begin_run(rf);
    // the guides in the state the model reads them in (as the .asi's RenoDX layers hand them)
    transition(cmd, f.depth, f.depth_state, kSrv);
    if (f.motion != f.depth) transition(cmd, f.motion, f.motion_state, kSrv);
    const auto sane = [](float v) { return std::isfinite(v) && v != 0.0f ? v : 1.0f; };   // (as the flow reads it)
    nr::dlssnr::evaluate_d3d12(cmd, o.session, nullptr, target, f.depth, f.motion, target, w, h, f.depth_rect, f.motion_rect,
                               (f.flags & (1u << 3)) ? 1 : 0, f.reset ? 1 : 0, nr::dlssnr::Controls6{}, sane(f.mv_x),
                               sane(f.mv_y));
    if (f.motion != f.depth) transition(cmd, f.motion, kSrv, f.motion_state);
    transition(cmd, f.depth, kSrv, f.depth_state);
    route::end_run();
    if (wp.ran)   // peripheral compression's outlines in NR Control, as the flow here notes them (spatial_ran)
        nr::dlssnr::with_d3d12_session([&](nr::pe::Session& session) {
            if (session.spatial_active()) wilsjo2_note_spatial(route::call_layer(), session.spatial_layout());
        });
    return wp.ran;
}
// A frame of display codes (frame reuse's rules are in them): the Present's resolved SDR (the caller's `hdr` false), else
// an 8 / 10-bit UNORM frame - a float frame at the upscaler is scene light (as a RenoDX layer's frame_units: mode 1)
bool float_frame(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
            return true;
        default:
            return false;
    }
}
// Tests (gates only): NR_TEST_W2_SESSION=1 runs every SDR frame's flow inside the session (where the passthrough does not
// run the layer), frame reuse on or off - the session's flow byte-compared with the flow here
bool test_session() {
    static const bool on = [] { const char* e = std::getenv("NR_TEST_W2_SESSION"); return e && e[0] == '1'; }();
    return on;
}
}  // namespace

void wilsjo2_release(Wilsjo2Owner* o) {
    if (!o) return;
    o->reuse12.reset();   // (its images retire through the D3D12 passthrough's watermark)
    if (o->vk) { wilsjo2_vk_release(o->vk); o->vk = nullptr; }
    D3d12Io io(nullptr, nullptr, *o, nullptr, nullptr, false, 0, 0);
    w2::flow_release(io, o->flow);
    if (o->session) nr::dlssnr::release_d3d12(o->session);
    if(o->group_linear)nr::pe::d3d12_passthrough::retire_resource(o->group_linear);
    if(o->group_pq)nr::pe::d3d12_passthrough::retire_resource(o->group_pq);
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
// ---- the D3D12 passthrough: the flow in the game's list, its programs D3D12 compute, its network the D3D12 passthrough's ----
// Whether this run takes it: "" yes; else why not (the run takes the bridge). Decided before anything
// of the run is recorded, so a frame's NR runs once, on one route.
std::string native_refusal(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const Wilsjo2Owner& o, const Wilsjo2Frame& f,
                           const w2::Settings& s, ID3D12Resource* target, uint32_t w, uint32_t h, w2::Stage** stage,
                           native::Stage12** impl, bool* waiting) {
    *waiting = false;
    if (!nr::pe::d3d12_passthrough::enabled())
        return "the D3D12 passthrough is off (dlssnr-amd.ini [D3D12Passthrough])";
    const auto readable = [&](ID3D12Resource* r, const char* what) -> std::string {
        if (!r && s.frame_reuse && std::strcmp(what,"depth")==0) return "";
        if (!r) return std::string("the ") + what + " is missing";
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
    // While the passthrough's pieces are made - the programs, the network - the frames pass through with no NR (the
    // bridge is neither run nor built for them); both are made at once. The bridge only where the passthrough will not run.
    std::string why;
    bool programs_building = false;
    *stage = native::programs(device, &why, &programs_building);
    uint32_t mw = w, mh = h;
    w2::flow_model_extent(s, w, h, o.flow, true, &mw, &mh);
    std::string nwhy;
    bool building = false;
    // (this layer wants the passthrough: noted before anything can refuse it - a layer waiting for another layer's second
    // part must still keep that one from evening its frames)
    nr::pe::d3d12_passthrough::note_layer(nr::pe::renodx_route::call_layer());
    const bool net_ready = nr::pe::d3d12_passthrough::wilsjo2_ready(device, cmd, mw, mh, &nwhy, &building);
    if (!net_ready && !building) return nwhy;   // the passthrough will not run it: the bridge (passing states set `building`)
    if (!*stage) {
        if (programs_building) { *waiting = true; return why; }
        return why;   // the programs cannot be made: the bridge
    }
    *impl = native::programs_impl();
    if (!*impl) { *waiting = true; return "its D3D12 programs are being made"; }
    if (!net_ready) { *waiting = true; return nwhy; }
    return "";
}
}  // namespace

// What a run of the flow here tells NR Control of the layer's frame reuse, and the network's history strength in the
// layer's place (FlowExtras::history < 0: the layer's)
// (split_*: frame reuse's even frames - the network calls go on in `split_tail` from `split_cut` of all their steps,
// `split_steps` a call, and everything recorded after that point there)
struct FlowExtras {
    uint32_t reuse_state = 0; const char* reuse_ui = ""; float reuse_share = -1.f; float history = -1.f;
    bool group_anchor = false;   // a frame reuse group's anchor run (every member, its own frame reuse on or not)
    ID3D12GraphicsCommandList* split_tail = nullptr; uint32_t split_cut = 0, split_steps = 0;
};
static bool run_flow(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, Wilsjo2Owner& o, const Wilsjo2Frame& f,
                     const w2::Settings& s, ID3D12Resource* target, uint32_t w, uint32_t h, bool before_upscale,
                     w2::Stage* nstage, native::Stage12* nimpl, const FlowExtras& x);
static bool run_native_reuse(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, Wilsjo2Owner& o, const Wilsjo2Frame& f,
                             const w2::Settings& s, ID3D12Resource* target, uint32_t w, uint32_t h, bool before_upscale,
                             w2::Stage* nstage, native::Stage12* nimpl);

bool wilsjo2_run(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, Wilsjo2Owner& o, const Wilsjo2Frame& f,
                 const w2::Settings& s, ID3D12Resource* target, uint32_t w, uint32_t h, bool before_upscale) {
    gpu_uses.touch();
    w2::Stage* nstage = nullptr;
    native::Stage12* nimpl = nullptr;
    // The diagnostic session override applies to SDR. A selected reuse layer owns its HDR encoding.
    const bool sdr = !f.hdr && target && (o.at_present || !float_frame(target->GetDesc().Format));
    const bool reuse = s.frame_reuse || (nr::frame_pair::PresentAge::active && nr::frame_pair::PresentAge::active->gathering());
    const bool in_session = (s.frame_reuse || (nr::frame_pair::PresentAge::active && nr::frame_pair::PresentAge::active->gathering()) || (test_session() && sdr));   // (where the passthrough does not run the layer)
    if (!s.frame_reuse) {   // switched off (NR Control, live): the passthrough's stage goes, a failed one may be made again
        o.reuse12.reset();
        o.reuse_failed.clear(); o.reuse_note.clear(); o.reuse_said.clear(); o.reuse_ui.clear();
    }
    if (target && (f.depth || reuse) && f.motion && device) {
        bool bridged = false;
        nr::dlssnr::with_d3d12_session([&](nr::pe::Session& session) { bridged = session.bridged(device); });
        if (bridged) {
            o.capture_layer = false;
            bool waiting = false;
            // a frame reuse group runs on one route: where another member needs the Vulkan bridge, this one goes too
            auto* age = nr::frame_pair::PresentAge::active;
            const bool group_bridge = age && age->group_bridge();
            const std::string why = group_bridge ? std::string("another layer of its frame reuse group runs on the Vulkan bridge")
                                  : reuse && cmd->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE
                                  ? std::string("frame reuse on a compute command list (its second part cannot follow it on this route)")
                                                 : native_refusal(cmd, device, o, f, s, target, w, h, &nstage, &nimpl, &waiting);
            if (!why.empty() && waiting) {   // the passthrough runs it once made; until then no NR, no bridge
                if (reuse) {
                    const std::string note = "Waiting for D3D12 Direct: " + why;
                    nr::pe::control::layer_reuse_waiting(nr::pe::renodx_route::call_layer(), note.c_str());
                    nr::pe::Session::note_frames_delayed(0);
                }
                native::say_route(o.route_said, "waiting for the D3D12 passthrough (" + why + "): frames pass through with no NR; the bridge is not used");
                return false;
            }
            if (!why.empty()) {
                // frame reuse too takes the bridge where the passthrough cannot serve it (the route rule of every
                // processing, as RenoDX's frame reuse); a group member takes its group along
                if (reuse && age && !group_bridge) age->need_bridge();
                if (nr::pe::d3d12_passthrough::enabled()) {
                    native::say_route(o.route_said, "the Vulkan bridge for now: " + why);
                    nr::pe::d3d12_passthrough::note_bridge_fallback();
                }
                o.reuse12.reset();   // (the passthrough's frame reuse: not this frame's route)
                if (in_session) return run_in_session(cmd, device, o, f, s, target, w, h, before_upscale);
                if (o.session) { nr::dlssnr::release_d3d12(o.session); o.session = nullptr; }
                if (o.at_present) nr::pe::Session::note_frames_delayed(0);   // (this Present's frames on time)
                return run_bridged(cmd, device, o, f, s, target, w, h, before_upscale);
            }
            namespace pt = nr::pe::d3d12_passthrough;
            native::say_route(o.route_said,
                              std::string("on the D3D12 route (canary ") + pt::canary_name(pt::canary()) +
                              "): its programs D3D12 compute, its network ACO on the game's device");
        }
    }
    o.capture_layer = false;
    if (in_session && !nstage) return run_in_session(cmd, device, o, f, s, target, w, h, before_upscale);   // (vkd3d-proton)
    if (o.session) { nr::dlssnr::release_d3d12(o.session); o.session = nullptr; }
    if (reuse && nstage) return run_native_reuse(cmd, device, o, f, s, target, w, h, before_upscale, nstage, nimpl);
    if (o.reuse12) o.reuse12.reset();   // (switched off: its images retire through the watermark)
    if (o.at_present) nr::pe::Session::note_frames_delayed(0);   // (this Present's frames on time)
    FlowExtras x;
    return run_flow(cmd, device, o, f, s, target, w, h, before_upscale, nstage, nimpl, x);
}

// The flow here on `target` (in place, UNORDERED_ACCESS), on the bridge's vkd3d-proton or the D3D12 passthrough (`nstage`).
static bool run_flow(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, Wilsjo2Owner& o, const Wilsjo2Frame& f,
                     const w2::Settings& s, ID3D12Resource* target, uint32_t w, uint32_t h, bool before_upscale,
                     w2::Stage* nstage, native::Stage12* nimpl, const FlowExtras& x) {
    if (!target || (!f.depth && !s.frame_reuse) || !f.motion) return false;
    if (nstage && (x.reuse_state==1 || x.group_anchor) && f.hdr && native::srv_format(target->GetDesc().Format)==DXGI_FORMAT_R10G10B10A2_UNORM &&
        nr::frame_pair::PresentAge::active && nr::frame_pair::PresentAge::active->gathering()) {
        const auto ensure=[&](ID3D12Resource*& r,DXGI_FORMAT format) {
            if (r && (r->GetDesc().Width!=w || r->GetDesc().Height!=h)) {
                nr::pe::d3d12_passthrough::retire_resource(r); r=nullptr;
            }
            if (!r)r=make_surface(device,format,w,h);
            return r!=nullptr;
        };
        if (!ensure(o.group_linear,DXGI_FORMAT_R16G16B16A16_FLOAT) || !ensure(o.group_pq,DXGI_FORMAT_R10G10B10A2_UNORM))
            return false;
        transition(cmd,target,kUav,kSrv);
        const bool decoded=wilsjo2_convert(cmd,device,target,nullptr,o.group_linear,w,h,true,true);
        transition(cmd,target,kSrv,kUav);
        if (!decoded || !run_flow(cmd,device,o,f,s,o.group_linear,w,h,before_upscale,nstage,nimpl,x)) return false;
        auto* end=x.split_tail?x.split_tail:cmd;
        transition(end,o.group_linear,kUav,kSrv); transition(end,target,kUav,kSrv);
        const bool encoded=wilsjo2_convert(end,device,o.group_linear,target,o.group_pq,w,h,false,true);
        transition(end,o.group_linear,kSrv,kUav); transition(end,target,kSrv,kUav);
        if (!encoded) return false;
        transition(end,o.group_pq,kUav,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(end,target,kUav,D3D12_RESOURCE_STATE_COPY_DEST);
        end->CopyResource(target,o.group_pq);
        transition(end,o.group_pq,D3D12_RESOURCE_STATE_COPY_SOURCE,kUav);
        transition(end,target,D3D12_RESOURCE_STATE_COPY_DEST,kUav);
        return true;
    }
    D3d12Io io(cmd, device, o, &f, target, before_upscale, w, h);
    if (nstage) io.go_native(nstage, nimpl);
    io.reuse_state = x.reuse_state; io.reuse_ui = x.reuse_ui; io.reuse_share = x.reuse_share; io.history = x.history;
    io.split_tail = x.split_tail; io.split_cut = x.split_cut; io.split_steps = x.split_steps;
    o.own_flow = true;
    w2::FlowFrame ff;
    ff.target = {target};
    ff.width = w; ff.height = h;
    const DXGI_FORMAT raw_fmt = io.frame_format();
    const DXGI_FORMAT uav_fmt = native::uav_format(raw_fmt);
    const DXGI_FORMAT format = uav_fmt != DXGI_FORMAT_UNKNOWN ? uav_fmt : raw_fmt;
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
        const D3D12_RESOURCE_DESC dd = f.depth ? f.depth->GetDesc() : D3D12_RESOURCE_DESC{}, md = f.motion->GetDesc();
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
    {   // what the native route runs on, said once per change for this owner (no per-frame lines)
        static std::mutex m;
        const D3D12_RESOURCE_DESC td = target->GetDesc(), md = f.motion->GetDesc();
        char line[400];
        std::snprintf(line, sizeof line,
                      "[d3d12] wilsjo2 on D3D12: runs %s on a %llux%u texture (format %u, flags 0x%X), frame %ux%u, %s, %s; motion "
                      "%llux%u format %u, rect %u,%u %ux%u, MVecScale %.3g %.3g; reset %d",
                      o.at_present ? "at Present" : before_upscale ? "before the upscaler" : "after the upscaler",
                      (unsigned long long)td.Width, td.Height, unsigned(td.Format),
                      unsigned(td.Flags), w, h, f.hdr ? "HDR (linear)" : "SDR", ff.unorm ? "UNORM" : "float",
                      (unsigned long long)md.Width, md.Height, unsigned(md.Format), f.motion_rect.x, f.motion_rect.y,
                      f.motion_rect.width, f.motion_rect.height, double(f.mv_x), double(f.mv_y), f.reset ? 1 : 0);
        std::lock_guard<std::mutex> l(m);
        // the reset flag and the pointer move between frames; the line is compared without the reset
        std::string key(line);
        key = key.substr(0, key.rfind("; reset"));
        if (key != o.native_run_said) { o.native_run_said = key; nr::pe::log("%s", line); }
        // the textures it is handed, each said once (the first 8): at Present the back buffers in turn; elsewhere more
        // than one would be the game's / OptiScaler's outputs in turn (with one feature's history across them)
        static std::set<std::pair<const void*, bool>> seen;
        const auto k = std::make_pair(static_cast<const void*>(target), o.at_present);
        if (seen.size() < 8 && seen.insert(k).second)
            nr::pe::log("[d3d12] wilsjo2 on D3D12: texture %p is run on (%s; %zu distinct so far)", static_cast<void*>(target),
                        o.at_present ? "at Present" : "in the evaluate", seen.size());
    }
    // the native route: timed (the stage's programs and the D3D12 network together, as the bridge's "network" time is)
    native::g_timing.begin(device, cmd, w, h);
    const bool ran = w2::flow_run(io, o.flow, ff, s, uint64_t(format));
    native::g_timing.end(cmd);
    if (ran && o.vk) {   // the bridge's own surfaces of this owner, no longer used
        wilsjo2_vk_release(o.vk);
        o.vk = nullptr;
    }
    if (!ran)
        native::say_route(o.route_said,
                          io.made() ? "a run on the D3D12 route made its feature (the flow runs it from the next frame); "
                                      "this frame passes through"
                                    : "a run on the D3D12 route was not recorded (see above); the frame passes through");
    return ran;
}

// FrameReuse on the D3D12 passthrough (the IRON RULE's Direct route): the flow here, natively, inside the passthrough's
// frame reuse stage (nr_frame_reuse_d3d12.hpp) as the backend runs a RenoDX layer's (nr_d3d12_passthrough_backend.cpp
// reuse_stage / renodx_frame): NR on every other frame, the frames between rebuilt along the game's motion vectors.
// Two-sided with even frames under the Vulkan routes' conditions (the Present, one NR layer running, frame generation off or
// followed, HUD FIX off): the flow's network calls split at the stage's share (the passthrough's plain call goes on in the
// stage's second part: renodx_route::CallNetSplit), the second part handed to the passthrough, submitted at the next
// Present (submit_pending), every frame shown two frames later. 
// UNTESTED: the D3D12 passthrough runs on AMD's Windows driver only.
static bool run_native_reuse(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, Wilsjo2Owner& o, const Wilsjo2Frame& f,
                             const w2::Settings& s, ID3D12Resource* target, uint32_t w, uint32_t h, bool before_upscale,
                             w2::Stage* nstage, native::Stage12* nimpl) {
    namespace fr12 = nr::pe::d3d12_passthrough::frame_reuse12;
    const D3D12_RESOURCE_DESC td = target->GetDesc(), md = f.motion->GetDesc(), dd = f.depth ? f.depth->GetDesc() : D3D12_RESOURCE_DESC{};
    const DXGI_FORMAT view = native::srv_format(td.Format);
    const DXGI_FORMAT mview = native::srv_format(md.Format), dview = native::srv_format(dd.Format);
    const uint32_t mx = f.motion_rect.x, my = f.motion_rect.y;
    const uint32_t mw = f.motion_rect.width ? f.motion_rect.width : uint32_t(md.Width);
    const uint32_t mh = f.motion_rect.height ? f.motion_rect.height : md.Height;
    const uint32_t dx = f.depth_rect.x, dy = f.depth_rect.y;
    const uint32_t dw = f.depth_rect.width ? f.depth_rect.width : uint32_t(dd.Width);
    const uint32_t dh = f.depth_rect.height ? f.depth_rect.height : dd.Height;
    const int layer = nr::pe::renodx_route::call_layer();
    bool alone = g_present_runs.load() <= 1u && !nr::pe::d3d12_passthrough::other_layers(layer);
    if (alone) nr::dlssnr::with_d3d12_session([&](nr::pe::Session& session) { alone = !session.layers_running(); });
    auto refuse = [&](const std::string& reason) {
        o.reuse12.reset();
        if (o.at_present) nr::pe::Session::note_frames_delayed(0);
        if (o.reuse_note != reason) log("[nr] D3D12 wilsjo2 frame reuse: %s", reason.c_str());
        o.reuse_note = reason;
        nr::pe::control::layer_reuse_failed(layer, reason.c_str());
        return false;
    };
    if (!o.at_present) return refuse(nr::frame_reuse::placement);
    if ((nr::frame_pair::PresentAge::active && nr::frame_pair::PresentAge::active->interleaved_conflict) || (s.passes_mode == 1 && s.fast_method == 0)) return refuse(nr::frame_reuse::interleaved);
    if ((nr::frame_pair::PresentAge::active ? nr::frame_pair::PresentAge::active->multiple : !alone) && !nr::frame_pair::PresentAge::terminal() && !nr::frame_pair::PresentAge::isolated()) return refuse(nr::frame_reuse::chain_busy);
    if (const char* why = nr::pe::Session::delay_block()) return refuse(why);
    if (!fr12::Stage::supports(view)) return refuse(nr::frame_reuse::unsupported_format);
    if (mview == DXGI_FORMAT_UNKNOWN || (f.depth && dview == DXGI_FORMAT_UNKNOWN))
        return refuse(nr::frame_reuse::guide_format);
    if (!o.reuse_failed.empty()) {
        if (std::chrono::steady_clock::now() < o.alloc_retry_after) return refuse("Cannot run: " + o.reuse_failed);
        o.reuse_failed.clear();
    }
    o.reuse_note.clear();
    fr12::Frame fr;
    fr.colour = {target, kUav, view, w, h};
    fr.output = fr.colour;
    fr.motion = {f.motion, f.motion_state, mview, uint32_t(md.Width), md.Height};
    fr.motion_x = mx; fr.motion_y = my; fr.motion_w = mw; fr.motion_h = mh;
    // the stage reads the vectors in screen units: the game's scale (pixels of the motion region, as the flow's) over it
    const auto sane = [](float v) { return std::isfinite(v) && v != 0.0f ? v : 1.0f; };   // (as the flow reads it)
    fr.motion_scale_x = sane(f.mv_x) / float(std::max(mw, 1u));
    fr.motion_scale_y = sane(f.mv_y) / float(std::max(mh, 1u));
    fr.depth = {f.depth, f.depth_state, dview, uint32_t(dd.Width), dd.Height};
    fr.depth_x = dx; fr.depth_y = dy; fr.depth_w = dw; fr.depth_h = dh;
    fr.depth_inverted = (f.flags & (1u << 3)) != 0;
    if (f.exposure) {
        const D3D12_RESOURCE_DESC xd = f.exposure->GetDesc();
        if (native::srv_format(xd.Format) != DXGI_FORMAT_UNKNOWN)
            fr.exposure = {f.exposure, f.exposure_state, native::srv_format(xd.Format), uint32_t(xd.Width), xd.Height};
    }
    fr.reset = f.reset;
    bool flow_ok = true;
    using Jobs=nr::frame_pair::ChainJobs<ID3D12GraphicsCommandList*,fr12::Frame>;
    const uint32_t steps=std::max(1u,w2::passes_requested(s))*nr::pe::d3d12_passthrough::network_steps();
    const int chosen_model=nr::dlssnr::frame_network(), chosen_layer=nr::pe::renodx_route::call_layer();
    auto anchor=[device,&o,f,s,w,h,before_upscale,nstage,nimpl,chosen_model,chosen_layer](ID3D12GraphicsCommandList*& list,const fr12::Frame& nf,
                    ID3D12GraphicsCommandList* tail,uint32_t cut,uint32_t gap) -> uint32_t {
        nr::pe::renodx_route::CallLayer row(chosen_layer);
        nr::pe::renodx_route::CallNetwork model(chosen_model);
        auto* a=nr::frame_pair::PresentAge::active;
        struct Choice { nr::frame_pair::PresentAge* age; int old;
            ~Choice(){if(age) age->net_override=old;}
        } restore{a,a?a->net_override:-1};
        if (a) a->net_override=chosen_model;
        // the stage's copy of the frame (its anchor) into its answer image, the flow on that in place
        if(nf.colour.res!=nf.output.res) {
            transition(list,nf.colour.res,nf.colour.state,D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(list,nf.output.res,nf.output.state,D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(nf.output.res,nf.colour.res);
            transition(list,nf.output.res,D3D12_RESOURCE_STATE_COPY_DEST,kUav);
            transition(list,nf.colour.res,D3D12_RESOURCE_STATE_COPY_SOURCE,nf.colour.state);
        } else transition(list,nf.output.res,nf.output.state,kUav);
        // a member without the game's depth (wilsjo2 needs it; frame reuse's own run does not): the anchor as it is
        if (!nf.depth.res && !s.frame_reuse) {
            transition(list, nf.output.res, kUav, nf.output.state);
            if (tail) list = tail;   // (the group's cursor past the cut, as after a run)
            return 0u;
        }
        Wilsjo2Frame g = f;
        g.depth = nf.depth.res; g.depth_state = nf.depth.state;
        g.depth_rect = {nf.depth_x, nf.depth_y, nf.depth_w, nf.depth_h};
        g.motion = nf.motion.res; g.motion_state = nf.motion.state;
        g.motion_rect = {nf.motion_x, nf.motion_y, nf.motion_w, nf.motion_h};
        if (nf.motion_composed) {   // the stage's field over two frames, in screen units: the scale that reads them so
            g.mv_x = float(std::max(nf.motion_w, 1u));
            g.mv_y = float(std::max(nf.motion_h, 1u));
        }
        g.exposure = nf.exposure.res; g.exposure_state = nf.exposure.state;
        g.reset = nf.reset;
        FlowExtras x;
        x.reuse_state = s.frame_reuse?1u:0u; x.reuse_share = o.reuse_share;
        x.group_anchor = true;   // (an ordinary member converts the group's PQ anchor too)
        if (gap>=2) x.history = 0.5f * s.history_strength;   // a history two frames old: half the layer's
        if (tail) {
            x.split_tail = tail;
            x.split_steps = nr::pe::d3d12_passthrough::network_steps();
            x.split_cut = cut;
        }
        const bool flow_ok = run_flow(list, device, o, g, s, nf.output.res, w, h, before_upscale, nstage, nimpl, x);
        // (the answer's state back where the flow ended: the second part when it split)
        transition(x.split_tail ? x.split_tail : list, nf.output.res, kUav, nf.output.state);
        if(tail)list=tail;
        return flow_ok ? std::max(1u, w2::passes_requested(s)) : nr::frame_pair::failed_calls;
    };
    Jobs* jobs=nullptr;
    auto* age=nr::frame_pair::PresentAge::active;
    if ((!age || !age->gathering() || !age->paired || !s.frame_reuse) && o.pair_reuse12) o.pair_reuse12.reset();
    if (age && age->gathering()) {
        Jobs::Pair pair;
        if (age->paired && s.frame_reuse) {
            fr12::Stage::Config cfg; cfg.device=device;cfg.width=w;cfg.height=h;cfg.frame_format=view;
            cfg.motion_width=mw;cfg.motion_height=mh;cfg.depth_width=std::max(1u,dw);cfg.depth_height=std::max(1u,dh);
            cfg.context=bool(f.depth);cfg.scrgb=f.hdr && view==DXGI_FORMAT_R16G16B16A16_FLOAT;cfg.stay_fixed=6.f;
            std::string err;
            if (!fr12::Stage::ensure_pair(o.pair_reuse12,cfg,[](IUnknown* r){nr::pe::d3d12_passthrough::retire_resource(r);},&err))
                return refuse("Cannot run: "+err);
            auto* kernel=o.pair_reuse12.get();
            pair=[kernel](ID3D12GraphicsCommandList*& c,fr12::Frame* odd,fr12::Frame& even,const Jobs::Execute& run){
                return kernel->render_pair(c,odd,even,run);
            };
        }
        jobs=Jobs::add(device,unsigned(view),steps,uintptr_t(&o),anchor,std::move(pair));
        if (!jobs || !jobs->ready) return true;
    }
    if (o.reuse12) {
        const auto& k = o.reuse12->config();
        if (k.width != w || k.height != h || k.frame_format != view || k.motion_width != mw || k.motion_height != mh ||
            k.depth_width != std::max(1u,dw) || k.depth_height != std::max(1u,dh) || k.context != bool(f.depth) || k.scrgb != (f.hdr && view==DXGI_FORMAT_R16G16B16A16_FLOAT) || k.batch_only!=bool(jobs && age->paired))
            o.reuse12.reset();
    }
    if (!o.reuse12) {
        fr12::Stage::Config k;
        k.device = device;
        k.width = w; k.height = h; k.frame_format = view;
        k.motion_width = mw; k.motion_height = mh; k.depth_width = std::max(1u,dw); k.depth_height = std::max(1u,dh); k.context = f.depth != nullptr;
        k.two_sided = true;
        k.scrgb = f.hdr && view==DXGI_FORMAT_R16G16B16A16_FLOAT; k.batch_only=jobs && age->paired;
        k.delay = true;
        k.stay_fixed = 6.0f;   // (a translucent HUD over a moving world may change a little in place)
        std::string err;   // (its images count as the processing's memory: nr_frame_reuse_d3d12.cpp)
        o.reuse12 = fr12::Stage::make(k, [](IUnknown* r) { nr::pe::d3d12_passthrough::retire_resource(r); }, &err);
        if (!o.reuse12) {
            o.alloc_retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            o.reuse_failed = err.empty() ? std::string("unknown") : err;
            return refuse("Cannot run: " + o.reuse_failed);
        }
        o.reuse_settings = s;
        o.reuse_frames = 0; o.reuse_logged = {}; o.reuse_window = {}; o.reuse_share = -1.f;
        log("[nr] frame reuse (D3D12, wilsjo2 layer %d) at %ux%u, format %d, guides %ux%u / %ux%u: NR on 1 frame of 2 (%llu MiB)%s",
            nr::pe::renodx_route::call_layer(), w, h, int(view), mw, mh, dw, dh, (unsigned long long)(o.reuse12->image_bytes() >> 20),
            "; even frames: the network split over two frames, each rebuilt frame made from the NR frames before and "
                   "after it, every frame shown two frames later");
    } else if (!w2::reuse_equal(o.reuse_settings, s)) {
        o.reuse12->reset();
        o.reuse_settings = s;
    }
    {   // [Preprocess] changed: as a setting's change, afresh
        const nr::Preprocess b = nr::dlssnr::preprocess_settings();
        const nr::Preprocess& a = o.reuse_prep;
        if (a.enabled != b.enabled || a.exposure != b.exposure || a.bias_ev != b.bias_ev || a.curve != b.curve ||
            a.contrast != b.contrast || a.saturation != b.saturation || a.inverse != b.inverse) {
            o.reuse12->reset();
            o.reuse_prep = b;
        }
    }
    nr::pe::d3d12_passthrough::note_recording(cmd);
    if (o.reuse_epoch!=(jobs?jobs->epoch:0)) { o.reuse12->reset(); o.reuse_epoch=jobs?jobs->epoch:0; }
    const auto paired=[&](ID3D12GraphicsCommandList*& c,fr12::Frame* odd,fr12::Frame& even,ID3D12GraphicsCommandList* tail,float share){
        return jobs->record_pair(c,odd,even,tail,share);
    };
    const auto r=jobs && age->paired ? o.reuse12->record_batch(cmd,fr,paired) : o.reuse12->record(cmd,fr,[&](ID3D12GraphicsCommandList* list,const fr12::Frame& nf) {
        const auto split=o.reuse12->split();
        if (jobs) return jobs->record(list,nf,split.tail,split.share,o.reuse12->gap());
        const uint32_t n=anchor(list,nf,split.tail,uint32_t(std::lround(double(split.share)*steps)),o.reuse12->gap());
        flow_ok=n!=nr::frame_pair::failed_calls && n!=0;
        return n;
    });
    if (jobs) { age->group_written=r.ok; age->group_share=o.reuse_share; }
    // even frames: the frame's second part to the passthrough, run at the next Present before anything NR records next
    const fr12::Stage::Pending pending = o.reuse12->take_pending();
    if (pending.list) nr::pe::d3d12_passthrough::push_pending(pending.list, pending.ticket);
    // (what the flow recorded there uses its surfaces: their retirement waits for that part too)
    if (pending.ticket) gpu_uses.tickets.push_back(pending.ticket);
    // frame generation's inputs follow a Present whose frames are shown later (nr_asi_hudcap.cpp; Session::frames_delay)
    if (o.at_present) nr::pe::Session::note_frames_delayed(r.ok ? r.shown_late : 0,r.ok ? r.next_late : 0);
    if ((o.reuse_frames + 1) % 60 == 0) {   // NR Control's share of frames with NR: windows of 60 frames
        const fr12::Stage::Stats st = o.reuse12->stats();
        if (st.frames > o.reuse_window.frames)
            o.reuse_share = float(double(st.computed - o.reuse_window.computed) / double(st.frames - o.reuse_window.frames));
        o.reuse_window = st;
    }
    if (++o.reuse_frames == 120 || o.reuse_frames % 600 == 0) {
        const fr12::Stage::Stats st = o.reuse12->stats();
        log("[nr] wilsjo2 layer %d: frame reuse (D3D12), last %llu frames: NR on %llu, rebuilt %llu",
            nr::pe::renodx_route::call_layer(), (unsigned long long)(st.frames - o.reuse_logged.frames),
            (unsigned long long)(st.computed - o.reuse_logged.computed),
            (unsigned long long)((st.frames - o.reuse_logged.frames) - (st.computed - o.reuse_logged.computed)));
        o.reuse_logged = st;
    }
    if (!r.ok) return refuse(o.flow.reuse_failure ? o.flow.reuse_failure : nr::frame_reuse::anchor_error);
    return !r.completed || flow_ok;
}

namespace {
struct DirectStateEntry {
    D3D12_RESOURCE_STATES initial, now;
};

bool is_vkd3d_device(ID3D12Device* device) {
    static const bool forced_bridge = [] {
        char v[4] = {};
        const DWORD n = GetEnvironmentVariableA("NR_BRIDGE", v, sizeof v);
        return n > 0 && n < sizeof v && v[0] == '1';
    }();
    if (forced_bridge || !device) return false;
    ID3D12DXVKInteropDevice* interop = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D12DXVKInteropDevice), reinterpret_cast<void**>(&interop))) &&
        interop) {
        interop->Release();
        return true;
    }
    return false;
}

template <class Fn>
std::optional<bool> run_direct_utility(
    ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const char* what,
    const std::vector<w2::Mode>& required_modes,
    std::initializer_list<std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES>> initial_states, const Fn& fn) {
    if (!device || !cmd || !nr::pe::d3d12_passthrough::enabled() || stage_fail_test()) return std::nullopt;
    if (nr::pe::d3d12_passthrough::canary() != nr::pe::d3d12_passthrough::Canary::kHit) return std::nullopt;
    if (is_vkd3d_device(device)) return std::nullopt;
    std::string why;
    if (!nr::pe::d3d12_direct::ready(device, cmd, &why)) return std::nullopt;
    w2::Stage* st = native::programs_wait(device, &why);
    native::Stage12* impl = st ? native::programs_impl() : nullptr;
    if (!st || !impl) return std::nullopt;
    for (w2::Mode m : required_modes) {
        if (!st->has(m)) return std::nullopt;
    }

    std::map<ID3D12Resource*, DirectStateEntry> tracked;
    for (const auto& kv : initial_states) {
        if (kv.first && tracked.find(kv.first) == tracked.end()) tracked[kv.first] = {kv.second, kv.second};
    }
    bool ok = true;
    impl->begin(cmd, [&](ID3D12Resource* r, D3D12_RESOURCE_STATES want) {
        if (!r) return;
        auto it = tracked.find(r);
        if (it == tracked.end()) it = tracked.emplace(r, DirectStateEntry{kUav, kUav}).first;
        transition(cmd, r, it->second.now, want);
        it->second.now = want;
    });
    try {
        ok = fn(*st);
    } catch (const std::exception& x) {
        log("[d3d12] ERROR: wilsjo2 on D3D12: %s: %s", what, x.what());
        ok = false;
    }
    for (auto& kv : tracked) {
        transition(cmd, kv.first, kv.second.now, kv.second.initial);
        kv.second.now = kv.second.initial;
    }
    impl->end();
    impl->end_recording();
    return ok;
}

bool direct_srv_ok(ID3D12Resource* r) {
    if (!r) return false;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.SampleDesc.Count == 1 &&
           native::srv_format(d.Format) != DXGI_FORMAT_UNKNOWN;
}

bool direct_uav_ok(ID3D12Resource* r) {
    if (!r) return false;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.SampleDesc.Count == 1 &&
           (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
           native::uav_format(d.Format) != DXGI_FORMAT_UNKNOWN;
}
}  // namespace

bool wilsjo2_convert(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* source,
                     ID3D12Resource* original, ID3D12Resource* target, uint32_t w, uint32_t h, bool to_linear, bool direct_only) {
    if (direct_srv_ok(source) && direct_uav_ok(target) && (to_linear || direct_srv_ok(original))) {
        const w2::Mode mode = to_linear ? w2::kFinishedToLinear : w2::kFinishedToPq;
        if (const auto r = run_direct_utility(
                cmd, device, "HDR10 conversion", {mode},
                {{source, kSrv}, {to_linear ? nullptr : original, kSrv}, {target, kUav}},
                [&](w2::Stage& st) {
                    w2::Dispatch d;
                    d.t0 = native::image(source);
                    if (!to_linear) d.t2 = native::image(original);
                    d.u0 = native::image(target);
                    d.constants.mode = mode;
                    d.constants.width = w;
                    d.constants.height = h;
                    st.record(VK_NULL_HANDLE, d);
                    return true;
                })) {
            return *r;
        }
    }
    if (direct_only || g_stage_retry.blocked()) return false;
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

bool hudfix_d3d12(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const HudFixD3D12& r, uint32_t w, uint32_t h,
                  uint32_t source, bool show_mask) {
    ID3D12Resource* alpha_res = r.alpha ? r.alpha : r.back;
    ID3D12Resource* ref_res = r.hudless ? r.hudless : r.back;
    if (direct_srv_ok(r.back) && direct_uav_ok(r.frame) && direct_uav_ok(r.stats) && direct_srv_ok(alpha_res) &&
        direct_srv_ok(ref_res)) {
        const std::vector<w2::Mode> run_modes = (source == 2 || source == 5)
            ? std::vector<w2::Mode>{w2::kHudClear, w2::kHudCount, w2::kHudCompose}
            : std::vector<w2::Mode>{w2::kHudClear, w2::kHudCompose};
        if (const auto res = run_direct_utility(
                cmd, device, "HUD FIX", run_modes,
                {{r.back, kUav}, {r.frame, kUav}, {r.stats, kUav}, {r.alpha, kUav}, {r.hudless, kUav}},
                [&](w2::Stage& st) {
                    const w2::Image back = native::image(r.back), frame = native::image(r.frame),
                                    stats = native::image(r.stats), alpha = native::image(alpha_res),
                                    ref = native::image(ref_res);
                    const auto run = [&](uint32_t mode, const w2::Image& t0, const w2::Image* t1, const w2::Image* t2,
                                         const w2::Image* t3, const w2::Image& u0, uint32_t width, uint32_t height) {
                        w2::Dispatch d;
                        d.t0 = t0;
                        if (t1) d.t1 = *t1;
                        if (t2) d.t2 = *t2;
                        if (t3) d.t3 = *t3;
                        d.u0 = u0;
                        d.u1 = stats;
                        d.constants = w2::hudfix_constants(mode, width, height, source, show_mask);
                        st.record(VK_NULL_HANDLE, d);
                    };
                    run(w2::kHudClear, back, nullptr, nullptr, nullptr, frame, 1, 1);
                    if (source == 2 || source == 5) run(w2::kHudCount, back, &ref, nullptr, nullptr, frame, w, h);
                    run(w2::kHudCompose, back, &ref, &frame, &alpha, frame, w, h);
                    return true;
                })) {
            return *res;
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
            const w2::Image back = image_of(lookup(r.back, kUav, true)), frame = image_of(lookup(r.frame, kUav, true)),
                            stats = image_of(lookup(r.stats, kUav, true));
            const w2::Image alpha = r.alpha ? image_of(lookup(r.alpha, kUav, true)) : back;
            const w2::Image ref = r.hudless ? image_of(lookup(r.hudless, kUav, true)) : back;
            if (!back.format || !frame.format || !alpha.format || !stats.format || !ref.format) return false;
            const auto run = [&](uint32_t mode, const w2::Image& t0, const w2::Image* t1, const w2::Image* t2,
                                 const w2::Image* t3, const w2::Image& u0, uint32_t width, uint32_t height) {
                w2::Dispatch d;
                d.t0 = t0;
                if (t1) d.t1 = *t1;
                if (t2) d.t2 = *t2;
                if (t3) d.t3 = *t3;
                d.u0 = u0;
                d.u1 = stats;
                d.constants = w2::hudfix_constants(mode, width, height, source, show_mask);
                g_stage->record(vk, d);
            };
            try {
                run(w2::kHudClear, back, nullptr, nullptr, nullptr, frame, 1, 1);
                if (source == 2 || source == 5) run(w2::kHudCount, back, &ref, nullptr, nullptr, frame, w, h);
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

bool hudfix_select_d3d12(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* frame,
                         ID3D12Resource* hudless, ID3D12Resource* stats, ID3D12Resource* out, uint32_t w, uint32_t h) {
    if (direct_srv_ok(frame) && direct_srv_ok(hudless) && direct_uav_ok(stats) && direct_uav_ok(out)) {
        if (const auto res = run_direct_utility(
                cmd, device, "HUD FIX select", {w2::kHudClear, w2::kHudCount, w2::kHudCompose},
                {{frame, kUav}, {hudless, kUav}, {stats, kUav}, {out, kUav}},
                [&](w2::Stage& st) {
                    const w2::Image f = native::image(frame), hl = native::image(hudless),
                                    s_img = native::image(stats), o = native::image(out);
                    const auto run = [&](uint32_t mode, uint32_t source, const w2::Image* t1, const w2::Image& u0,
                                         uint32_t width, uint32_t height) {
                        w2::Dispatch d;
                        d.t0 = f;
                        if (t1) d.t1 = *t1;
                        d.u0 = u0;
                        d.u1 = s_img;
                        d.constants = w2::hudfix_constants(mode, width, height, source, false);
                        st.record(VK_NULL_HANDLE, d);
                    };
                    run(w2::kHudClear, 2, nullptr, o, 1, 1);
                    run(w2::kHudCount, 2, &hl, o, w, h);
                    run(w2::kHudCompose, 6, &hl, o, w, h);
                    return true;
                })) {
            return *res;
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
            const w2::Image f = image_of(lookup(frame, kUav, true)), hl = image_of(lookup(hudless, kUav, true)),
                            st = image_of(lookup(stats, kUav, true)), o = image_of(lookup(out, kUav, true));
            if (!f.format || !hl.format || !st.format || !o.format) return false;
            const auto run = [&](uint32_t mode, uint32_t source, const w2::Image* t1, const w2::Image& u0, uint32_t width,
                                 uint32_t height) {
                w2::Dispatch d;
                d.t0 = f;
                if (t1) d.t1 = *t1;
                d.u0 = u0;
                d.u1 = st;
                d.constants = w2::hudfix_constants(mode, width, height, source, false);
                g_stage->record(vk, d);
            };
            try {
                run(w2::kHudClear, 2, nullptr, o, 1, 1);
                run(w2::kHudCount, 2, &hl, o, w, h);
                run(w2::kHudCompose, 6, &hl, o, w, h);
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

bool wilsjo2_fg_delta(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* hudless, ID3D12Resource* before,
                      ID3D12Resource* after, ID3D12Resource* zero, ID3D12Resource* stats, ID3D12Resource* out, uint32_t w,
                      uint32_t h) {
    if (direct_srv_ok(hudless) && direct_srv_ok(before) && direct_srv_ok(after) && direct_srv_ok(zero) &&
        direct_uav_ok(out) && direct_uav_ok(stats)) {
        if (const auto res = run_direct_utility(
                cmd, device, "FG HUD-less delta", {w2::kHudCompose},
                {{hudless, kUav}, {before, kUav}, {after, kUav}, {zero, kUav}, {out, kUav}, {stats, kUav}},
                [&](w2::Stage& st) {
                    w2::Dispatch d;
                    d.t0 = native::image(hudless);
                    d.t1 = native::image(before);
                    d.t2 = native::image(after);
                    d.t3 = native::image(zero);
                    d.u0 = native::image(out);
                    d.u1 = native::image(stats);
                    d.constants = w2::hudfix_constants(w2::kHudCompose, w, h, 0u, false);
                    st.record(VK_NULL_HANDLE, d);
                    return true;
                })) {
            return *res;
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
            d.t0 = image_of(lookup(hudless, kUav, true));   // Back: the HUD-less frame
            d.t1 = image_of(lookup(before, kUav, true));    // Hudless: the frame before the layers
            d.t2 = image_of(lookup(after, kUav, true));     // Enhanced: the frame after them
            d.t3 = image_of(lookup(zero, kUav, true));      // a = 0
            d.u0 = image_of(lookup(out, kUav, true));
            d.u1 = image_of(lookup(stats, kUav, true));
            if (!d.t0.format || !d.t1.format || !d.t2.format || !d.t3.format || !d.u0.format || !d.u1.format) return false;
            d.constants = w2::hudfix_constants(w2::kHudCompose, w, h, 0u, false);   // HudMode 0: a = t3.r
            try {
                g_stage->record(vk, d);
            } catch (const std::exception& x) {
                log("[nr] frame generation's HUD-less frame: %s", x.what());
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
    if (direct_srv_ok(before) && direct_srv_ok(after) && direct_uav_ok(target)) {
        if (const auto res = run_direct_utility(
                cmd, device, "NR comparison", {w2::kInspect},
                {{before, kSrv}, {after, kSrv}, {target, kUav}},
                [&](w2::Stage& st) {
                    w2::Dispatch d;
                    d.t0 = native::image(before);
                    d.t1 = native::image(after);
                    d.u0 = native::image(target);
                    d.constants.mode = w2::kInspect;
                    d.constants.width = w;
                    d.constants.height = h;
                    d.constants.compare_mode = compare;
                    d.constants.compare_split = split;
                    d.constants.compare_zoom = zoom;
                    d.constants.compare_swap = swap ? 1u : 0u;
                    d.constants.apply_model = apply_model ? 1u : 0u;
                    st.record(VK_NULL_HANDLE, d);
                    return true;
                })) {
            return *res;
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
