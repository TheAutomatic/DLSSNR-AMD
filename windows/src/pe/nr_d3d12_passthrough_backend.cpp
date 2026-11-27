#include "nr_pe_control.hpp"
#include "nr_frame_pair.hpp"
#include "nr_frame_chain.hpp"
#include "../core/nr_frame_reuse_policy.hpp"
#include "nr_frame_pair.hpp"
#include "nr_frame_chain.hpp"
// The D3D12 passthrough's frame path (nr_d3d12_passthrough_backend.hpp), on the game's command list: the RenoDX
// processing's frames through its D3D12 stage around the network; the network calls of the wilsjo2 processing's D3D12
// flow (nr_asi_wilsjo2.cpp) as PAL ELF copy-in, the network, PAL ELF writeback with zero SetDescriptorHeaps calls.
// Everything that can fail is checked before the first command is recorded, so a false return leaves the list as it
// came and the Vulkan bridge runs the frame.
#include "nr_d3d12_passthrough_backend.hpp"
#include "nr_d3d12_passthrough.hpp"
#include "nr_d3d12_passthrough_net.hpp"
#include "nr_d3d12_outer_elf.hpp"
#include "nr_d3d12_direct_state.hpp"
#include "nr_renodx_d3d12.hpp"   // the RenoDX stage on this route
#include "nr_frame_reuse_d3d12.hpp"   // NRFrameReuse around it
#include "nr_d3d12_shot.hpp"      // the NR screenshot's readback pair
#include "nr_screenshot.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_session.hpp"   // g_motion_without_depth_now
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nr::pe::d3d12_passthrough {
namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
// Uses retains the original recording set; D3D12 retirement requires actual completion,
// rather than Recording::ready()'s time/submission fallback for the bridge.
bool recordings_done(const nr::gpu::Uses& uses) {
    return std::all_of(uses.tickets.begin(), uses.tickets.end(), [](const auto& t) {
        return nr::pe::d3d12_direct::completed(t);
    });
}

// A view format of the resource's: typeless -> its UNORM / FLOAT member; an sRGB format (no typed UAV) -> unknown.
DXGI_FORMAT view_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;      // the motion guide's
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_UNKNOWN;
    default: return f;
    }
}
DXGI_FORMAT depth_view_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16_FLOAT:
        return DXGI_FORMAT_R16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}


struct Backend {
    ID3D12Device* device = nullptr;
    ID3D12RootSignature* outer_rs = nullptr;
    d3d12_outer::OuterKernel k_to_linear{}, k_to_linear32{}, k_from_linear{}, k_to_motion{}, k_to_depth{},
                             k_copy_depth_r32{};
    // The plain frame path's linear buffers, one set per extent (the bridge keeps a runtime per extent): a wilsjo2 flow
    // with layers at two sizes takes turns every frame and must not make them again each time. lin_* are this frame's.
    struct LinearSet {
        bool isolated=false;
        uint32_t w = 0, h = 0;
        ID3D12Resource *in = nullptr, *out = nullptr, *mv = nullptr, *depth = nullptr;
        ID3D12Resource* hist = nullptr;   // a Passes (fast) Interleaved pass's history, the network's layout (made on use)
        uint64_t used = 0;   // the frame that last used it (the LRU)
    };
    static constexpr size_t kLinearSets = 4;
    std::vector<LinearSet> linear_sets;
    ID3D12Resource *lin_in = nullptr, *lin_out = nullptr, *lin_mv = nullptr, *lin_depth = nullptr;
    // The plain frame path's extent per feature (Session::resized), and how many changes were logged
    std::map<uint64_t, std::pair<uint32_t, uint32_t>> feature_extent;
    uint64_t resizes = 0;
    ID3D12Resource* neutral_mv = nullptr;
    uint32_t w = 0, h = 0;
    // A shared_ptr, not a unique_ptr: a reader off the game's thread (the heartbeat, NR Control's status) copies
    //    the pointer under `owner_lock` and uses it outside the lock, so a device swap cannot free it under a reader.
    std::shared_ptr<D3d12PassthroughNetwork> network, split_network;
    std::shared_ptr<D3d12PassthroughNetwork>& current_network() {
        return nr::frame_pair::PresentAge::isolated()?split_network:network;
    }
    bool pending_isolated=false;
    bool broken = false;   // something of the device's could not be made: the Vulkan bridge from now on
    // Set while a device generation is being dropped: a stage's destructor calls retire() for the surfaces it still
    // holds, and those must be released here, not queued into the next generation's list (their watermark is gone).
    bool discarding = false;
    std::string broken_why;
    uint64_t tick = 0;
    // The backend's own completion signal: a frame number copied into a readback buffer by the same command list that
    // used the objects, so a retired stage or buffer is released only after the GPU ran past it. An evaluate count is
    // not evidence: a stack records many frames into one list that may still be unsubmitted.
    static constexpr uint32_t kDoneSlots = 128, kTimeRing = 16;
    ID3D12QueryHeap* queries = nullptr;
    ID3D12Resource* stamps = nullptr;
    uint8_t* stamps_map = nullptr;
    double hz = 0.0;
    nr::gpu::Ticket stamp_uses[kTimeRing] = {};
    float stage_ring[32] = {};
    size_t stage_ring_at = 0;
    ID3D12Resource* token = nullptr;     // upload: the frame numbers to echo
    uint64_t* token_map = nullptr;
    uint32_t token_slots = 0;            // growable: a slot is rewritten only after its holder executed
    ID3D12Resource* done = nullptr;      // readback: what the GPU echoed
    uint64_t* done_map = nullptr;
    uint64_t frames = 0;                 // frames recorded by this backend
    int64_t executed = -1;               // newest frame the GPU finished
    // The recording ticket of the frame being recorded: every resource that frame makes is retired by it, so the
    // resource's own recording is its completion evidence (not a global "newest frame finished" assumption).
    std::shared_ptr<nr::gpu::Recording> booking;
    // Every recording this generation made (one per recorded frame/list), kept so the generation is released only
    // when all of them are strictly done - not by the newest one, and not by a token maximum.
    struct FrameRecording {
        int64_t frame;                                // the backend frame number this recording belongs to
        uint64_t feature;                             // the game feature the frame belonged to
        std::shared_ptr<nr::gpu::Recording> rec;
    };
    std::vector<FrameRecording> recordings;
    // Frame reuse's even frames: second parts recorded, not submitted yet (submit_pending; a reference each)
    std::vector<ID3D12GraphicsCommandList*> pending_tails;
    // The evidence a retired object waits for: this generation's unfinished recordings at the moment it is retired,
    // conservatively - the frame being recorded is not proof by itself, an earlier recording of the same generation may
    // still be unsubmitted and run after a newer one finished (nr::gpu::Uses, the same owner-less use set the Vulkan
    // path retires with).
    nr::gpu::Uses uses_now() const {
        nr::gpu::Uses u;
        for (const auto& r : recordings)
            if (r.rec && !nr::pe::d3d12_direct::completed(r.rec)) u.tickets.push_back(r.rec);
        if (booking && std::find(u.tickets.begin(), u.tickets.end(), booking) == u.tickets.end())
            u.tickets.push_back(booking);
        return u;
    }
    // A grown-away token buffer, released by the recording of the frame that last read it (a predecessor of the token
    // block is read by the same list that copies this frame's number, so the frame's own recording is the evidence).
    struct RetiredToken {
        int64_t frame;
        nr::gpu::Uses uses;
        ID3D12Resource* p;
    };
    std::vector<RetiredToken> token_retired;
    // A value the GPU has not written yet reads as ~0, and only a slot whose holder matches its own index is a
    // completion: a frame number ahead of the newest record is a torn read, not evidence.
    void refresh_executed() {
        if (!done_map) return;
        const uint64_t max_f = frames ? frames - 1 : 0;
        for (uint32_t s = 0; s < kDoneSlots; ++s) {
            const uint64_t v = done_map[s];
            if (v != ~uint64_t(0) && v <= max_f && (v % kDoneSlots) == s && int64_t(v) > executed)
                executed = int64_t(v);
        }
    }
    struct Retired {
        int64_t frame;                                   // the backend's frame number that last used it
        nr::gpu::Uses uses;                               // the recordings that may still run past it
        IUnknown* p;
    };
    std::vector<Retired> retired;
    // The RenoDX route's stages, one a feature (nr_pe_session.cpp's RenoDXEntry: its working resolution's debounce
    // and its stage, keyed by everything a change of takes a new one).
    struct RenodxEntry {
        std::unique_ptr<renodx12::Stage> stage;
        uint32_t stack = 0;
        uint32_t w = 0, h = 0, nw = 0, nh = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        int hdr = 0, present = 0;
        float scale = 1.0f;
        bool follow = false;
        uint32_t applied_w = 0, applied_h = 0, pending_w = 0, pending_h = 0, pending_frames = 0;
        // NRFrameReuse around the stage (nr_frame_reuse_d3d12.hpp; the Vulkan session's RenoDXEntry fields): made at
        // the frame's extent, format and guide sizes; its images retire through the backend's watermark when it goes
        std::unique_ptr<frame_reuse12::Stage> reuse, pair_reuse;
        nr::renodx::Settings reuse_settings{};
        std::chrono::steady_clock::time_point alloc_retry_after{};
        std::string reuse_failed, reuse_note, reuse_ui;
        std::set<std::string> reuse_said;
        uint32_t reuse_passing = 0;                       // frames in a row with a passing reason (no vectors / depth)
        uint64_t reuse_frames = 0, reuse_epoch = 0;
        frame_reuse12::Stage::Stats reuse_window{}, reuse_logged{};
        float reuse_share = -1.f;
        std::chrono::steady_clock::time_point used{};     // its last frame (one NR layer running: the two-sided rule)
    };
    std::map<uint64_t, RenodxEntry> renodx;
    // A replaced stage (an extent change) is released once the recording of the frame that last used it is done - the
    // same evidence as `retired`, not the backend's newest-frame watermark: a stack records many frames into one list
    // that may still be unsubmitted, and a frame number says nothing about whether the GPU ran past that stage's work.
    struct RetiredStage {
        int64_t frame;
        nr::gpu::Uses uses;
        const char* why;   // why it was replaced: a key change, a failed set(), or the feature being released
        std::unique_ptr<renodx12::Stage> stage;
    };
    std::vector<RetiredStage> renodx_retired;
    // Declared last so ~Backend() tears it down before retired/recordings/booking it may push into.
    std::unique_ptr<d3d12_outer::OuterHeapPool> outer_pool;
};
Backend g;
std::atomic<float> g_stage_avg_ms{0.f}, g_last_fast_passes{0.f};
std::atomic<uint32_t> g_last_nr_w{0}, g_last_nr_h{0};
std::atomic<bool> g_last_fast_fallback{false};
std::atomic<uint8_t> g_last_il_missing{0};
// the RenoDX stage's last frame's facts (backend_net_status, NR Control's engine-data panel)
std::mutex g_facts_lock;
bool g_facts_known = false;
renodx12::Stage::Facts g_last_facts{};
// NRFrameReuse on this route, the last RenoDX frame's (backend_net_status: NR Control's line under the switch, the share
// of frames with NR): 0 off, 1 running, 2 cannot run (g_reuse_ui why, under g_facts_lock)
std::atomic<uint32_t> g_reuse_state{0};
std::atomic<uint64_t> g_w2_direct_ms{0};   // a wilsjo2 layer's last frame on this route (frame reuse's one-NR-layer rule)
std::atomic<float> g_reuse_share{-1.f};
std::string g_reuse_ui;
void note_stage_gpu(double ms) {
    if (g.stage_ring_at == 1 && ms < double(g.stage_ring[0]) * 0.75) g.stage_ring_at = 0;
    g.stage_ring[g.stage_ring_at % 32] = float(ms);
    ++g.stage_ring_at;
    const size_t n = std::min<size_t>(g.stage_ring_at, 32);
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += g.stage_ring[i];
    const float avg = float(s / double(n));
    g_stage_avg_ms.store(avg);

}
// Guards `g.device` and `g.network` against a reader that does not run inside evaluate (the heartbeat's device-removed
// poll, NR Control's status): the readers take a reference under it, the evaluate/drop_device path swaps them under it.
// Recursive because the writers already call one another (release_generation -> drop_device).
std::recursive_mutex g_owner_lock;
// Device generations that were swapped while work of theirs could still be in flight. Each is released only when its
// own completion evidence (its Recording/ticket, or its token watermark) says the GPU ran past its last frame.
std::vector<std::unique_ptr<Backend>> g_old_generations;

// An upload or readback buffer of `bytes` in `state` (the completion pair).
ID3D12Resource* heap_buffer(ID3D12Device* device, D3D12_HEAP_TYPE type, uint64_t bytes, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

// The frame the object was last used in: the one about to be recorded (a replaced stage was used by the frame before,
// so this is one frame of margin, never less).
// A feature whose frame extent changed since its last evaluate starts its history again (Session::resized, which the
// bridge ORs into every path's Reset; Resize History's carry is not on the D3D12 passthrough). Remembers the extent.
bool resized(uint64_t feature, uint32_t w, uint32_t h) {
    auto& e = g.feature_extent[feature];
    const bool changed = e.first && (e.first != w || e.second != h);
    if (changed && ++g.resizes <= 8)
        nr::pe::log("[d3d12] feature #%u: %ux%u -> %ux%u, history restarts%s", unsigned(feature), e.first, e.second, w, h,
                    g.resizes == 8 ? "; not logged again" : "");
    e = {w, h};
    return changed;
}

void retire(IUnknown* p) {
    if (!p) return;
    if (g.discarding) { p->Release(); return; }
    g.retired.push_back({int64_t(g.frames), g.uses_now(), p});
}
void release_generation(Backend& b);
void release_renodx_feature_internal(uint64_t feature, bool clear_latch);

// Has this backend frame's own recording finished? Frames without a Recording (the harness's early frames, a device
// the split never hooked) fall back to the token watermark.
bool frame_done(int64_t frame) {
    // (a frame with frame reuse's second part has two recordings: done when both are)
    bool any = false;
    for (const auto& r : g.recordings)
        if (r.frame == frame && r.rec) {
            if (!nr::pe::d3d12_direct::completed(r.rec)) return false;
            any = true;
        }
    if (any) return true;
    g.refresh_executed();
    return g.executed >= frame;
}

// A generation is finished when every recording it made is strictly ready (sealed, every submission answered). A
// generation that made none (the offline harness, whose bookings are manually sealed) keeps the token watermark.
bool generation_done(Backend& b) {
    for (const auto& rec : b.recordings) {
        if (!rec.rec) continue;
        if (!nr::pe::d3d12_direct::completed(rec.rec)) return false;
    }
    if (!b.recordings.empty()) return true;
    b.refresh_executed();
    return b.frames == 0 || b.executed >= int64_t(b.frames) - 1;
}

// RenoDX's NR screenshot on this route, by nr_pe_session.cpp's final-picture rules: the pair is copied in the game's
// list around the stage and handed to the worker once that list's recording has finished (we own no fence here). Kept
// outside the generations: a device swap does not stop copies already recorded.
struct PendingShot {
    nr::pe::d3d12shot::Readback pair;
    std::shared_ptr<nr::gpu::Recording> ticket;   // the recording both copies are in
    bool deliver = false;                          // false: a copy the frame dropped, only released once it ran
    uint8_t hdr = 0;
    std::string meta;
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
};
std::vector<std::unique_ptr<PendingShot>> g_shots;
bool shot_pending() {
    for (const auto& p : g_shots) if (p->deliver) return true;
    return false;
}
void shot_poll() {
    for (size_t i = 0; i < g_shots.size();) {
        PendingShot& p = *g_shots[i];
        if (p.ticket->never_submitted()) {   // closed and reset without running: nothing wrote the buffers
            p.pair.release();
            if (p.deliver) nr::pe::screenshot::abandon("capture_failed", "the captured frame's commands never ran");
        } else if (nr::pe::d3d12_direct::completed(p.ticket)) {
            if (!p.deliver) {
                p.pair.release();
            } else if (!p.pair.deliver(p.hdr, std::move(p.meta))) {
                p.pair.release();
                nr::pe::screenshot::abandon("capture_failed", "the readback buffers could not be mapped");
            }
        } else if (std::chrono::steady_clock::now() - p.since > std::chrono::seconds(10)) {
            p.pair.forget();   // no proof the copies ran: the buffers stay (the GPU may still write them)
            if (p.deliver) nr::pe::screenshot::abandon("capture_failed", "the captured frame did not finish within 10 s");
        } else {
            ++i;
            continue;
        }
        g_shots.erase(g_shots.begin() + long(i));
    }
}

void sweep() {
    shot_poll();
    ++g.tick;
    // Previous generations: released only when their own evidence has passed their last recorded frame.
    for (size_t i = 0; i < g_old_generations.size();) {
        Backend& b = *g_old_generations[i];
        if (generation_done(b)) { release_generation(b); g_old_generations.erase(g_old_generations.begin() + long(i)); }
        else ++i;
    }
    // A recording whose every submission answered no longer gates the generation: drop it, so the set does not
    // accumulate one entry per frame for the life of the device.
    for (size_t i = 0; i < g.recordings.size();) {
        const bool dropped = g.recordings[i].rec && g.recordings[i].rec->never_submitted();
        if (dropped) release_renodx_feature_internal(g.recordings[i].feature, false);   // closed, never executed: history ends
        if (!g.recordings[i].rec || dropped || nr::pe::d3d12_direct::completed(g.recordings[i].rec))
            g.recordings.erase(g.recordings.begin() + long(i));
        else ++i;
    }
    g.refresh_executed();
    auto it = std::remove_if(g.retired.begin(), g.retired.end(), [](const auto& r) {
        // An owner released by its own recording's ticket once that recording is sealed and its GPU work finished;
        // a resource retired without a ticket keeps the backend's own watermark.
        if (!r.uses.tickets.empty()) {
            if (!recordings_done(r.uses)) return false;
        } else if (g.executed < r.frame) {
            return false;
        }
        r.p->Release();
        return true;
    });
    g.retired.erase(it, g.retired.end());
    // The same evidence as `retired`: the frame's own recording when there is one, the watermark otherwise.
    auto frame_evidence = [](int64_t frame, const nr::gpu::Uses& uses) {
        if (!uses.tickets.empty()) return recordings_done(uses);
        return g.executed >= frame;
    };
    auto rs = std::remove_if(g.renodx_retired.begin(), g.renodx_retired.end(), [&](const auto& r) {
        if (!frame_evidence(r.frame, r.uses)) return false;
        // A replaced stage's own destruction: one line, so an extent change can be seen to retire the old stage rather
        // than merely producing output for the new one.
        // The evidence, not the frame number: how many recordings the retirement waited on and that all of them are
        // done (each is sealed and every submission has answered).
        nr::pe::log("[d3d12] a replaced RenoDX stage was released (retired at frame %lld, reason %s, %zu recording(s) done, "
                    "stage %p)", (long long)r.frame, r.why ? r.why : "?", r.uses.tickets.size(),
                    static_cast<void*>(r.stage.get()));
        return true;
    });
    g.renodx_retired.erase(rs, g.renodx_retired.end());
    auto rt = std::remove_if(g.token_retired.begin(), g.token_retired.end(), [&](auto& r) {
        if (!frame_evidence(r.frame, r.uses)) return false;
        r.p->Release();
        return true;
    });
    g.token_retired.erase(rt, g.token_retired.end());
}

// Recorded at the end of every frame, after everything that uses a stage: the GPU copies this frame's number back,
// and sweep() releases what an older frame used.
void mark_frame(ID3D12GraphicsCommandList* cmd) {
    if (!g.token || !g.token_map || !g.done) return;
    const uint64_t frame = g.frames;
    // Grow the token block before rewriting a slot that the GPU has not finished: a stack records many frames into
    // one still-unsubmitted list, and reusing a slot would make the earlier frame's copy land in a later frame's
    // number, so sweep() would believe work is done that has not run.
    if (frame >= g.token_slots && int64_t(frame - g.token_slots) > g.executed) {
        const uint64_t slots = uint64_t(g.token_slots) * 2;
        ID3D12Resource* grown = heap_buffer(g.device, D3D12_HEAP_TYPE_UPLOAD, slots * 8, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* tm = nullptr;
        if (!grown || FAILED(grown->Map(0, nullptr, &tm)) || !tm) {
            release(grown);
            return;   // no completion signal this frame: retirement simply waits (never less safe)
        }
        g.token_retired.push_back({int64_t(g.frames), g.uses_now(), g.token});   // the last record that reads the old block
        g.token = grown;
        g.token_map = static_cast<uint64_t*>(tm);
        g.token_slots = uint32_t(slots);
    }
    // The upload block grows; the readback keeps kDoneSlots entries (copies run in queue order, so a readback slot
    // is rewritten only after the copy into it has run).
    const uint64_t up = (frame % g.token_slots) * 8;
    const uint64_t rb = (frame % Backend::kDoneSlots) * 8;
    g.token_map[up / 8] = frame;
    cmd->CopyBufferRegion(g.done, rb, g.token, up, 8);
    ++g.frames;
}

ID3D12Resource* linear_buffer(ID3D12Device* device, uint64_t bytes);
std::wstring widen(const char* u8);




bool build(ID3D12Device* device, std::string* why) {
    if (!d3d12_outer::create_outer_root_signature(device, &g.outer_rs, why)) return false;
    auto arch = d3d12_outer::shared_archive(why);
    if (!arch) return false;
    for (auto [name, k] : {
             std::pair{"backend/to_linear", &g.k_to_linear},
             std::pair{"backend/to_linear32", &g.k_to_linear32},
             std::pair{"backend/from_linear", &g.k_from_linear},
             std::pair{"backend/to_motion", &g.k_to_motion},
             std::pair{"backend/to_depth", &g.k_to_depth},
             std::pair{"backend/copy_depth_r32", &g.k_copy_depth_r32}}) {
        if (!arch->build_kernel(device, name, k, why)) return false;
    }
    if (!g.outer_pool) g.outer_pool = std::make_unique<d3d12_outer::OuterHeapPool>();
    if (!g.outer_pool->init(
            device, 256,
            [](const nr::gpu::Ticket& t) { return nr::pe::d3d12_direct::completed(t); },
            [](IUnknown* p) { retire(p); }, why)) {
        return false;
    }
    if (g.outer_pool->indirect_srd()) {
        nr::pe::log("[d3d12] outer SRD staging: indirect descriptor handle mode (1-level deref verified)");
    }
    // The ACO network, lowered by the planner the .asi sets (nr_asi_main.cpp).
    if (!net_planner()) { *why = "no network planner"; return false; }
    {
        std::lock_guard<std::recursive_mutex> l(g_owner_lock);
        g.network = make_aco_network();
    }
    const char* nwhy = "";
    if (!g.network->create(device, &nwhy)) { *why = std::string("the network: ") + nwhy; return false; }
    // The completion pair: a frame number the GPU echoes back once the commands that used a retired object have run.
    g.token = heap_buffer(device, D3D12_HEAP_TYPE_UPLOAD, Backend::kDoneSlots * 8, D3D12_RESOURCE_STATE_GENERIC_READ);
    g.done = heap_buffer(device, D3D12_HEAP_TYPE_READBACK, Backend::kDoneSlots * 8, D3D12_RESOURCE_STATE_COPY_DEST);
    g.token_slots = Backend::kDoneSlots;
    if (!g.token || !g.done) { *why = "the backend's completion buffers could not be made"; return false; }
    {
        void* m = nullptr;
        if (FAILED(g.token->Map(0, nullptr, &m)) || !m) { *why = "the backend's token buffer could not be mapped"; return false; }
        g.token_map = static_cast<uint64_t*>(m);
        m = nullptr;
        if (FAILED(g.done->Map(0, nullptr, &m)) || !m) { *why = "the backend's readback could not be mapped"; return false; }
        g.done_map = static_cast<uint64_t*>(m);
        std::memset(g.done_map, 0xFF, Backend::kDoneSlots * 8);
        D3D12_QUERY_HEAP_DESC qh{}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = 4 * Backend::kTimeRing;   // (+ frame reuse's second part)
        ID3D12CommandQueue* q = nullptr; D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; UINT64 hz = 0;
        if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) && q) { q->GetTimestampFrequency(&hz); q->Release(); }
        if (hz && SUCCEEDED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&g.queries)))) {
            g.stamps = heap_buffer(device, D3D12_HEAP_TYPE_READBACK, 32 * Backend::kTimeRing, D3D12_RESOURCE_STATE_COPY_DEST);
            void* sm = nullptr;
            if (g.stamps && SUCCEEDED(g.stamps->Map(0, nullptr, &sm)) && sm) {
                g.stamps_map = static_cast<uint8_t*>(sm); g.hz = double(hz); std::memset(g.stamps_map, 0, 32 * Backend::kTimeRing);
            }
        }
    }
    note_network(g.network->name());
    renodx12::prewarm_shaders(widen(nr::pe::module_folder()) + L"\\dlssnr-amd\\shaders\\renodx");
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd_mv{};
    rd_mv.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd_mv.Width = 1; rd_mv.Height = 1; rd_mv.DepthOrArraySize = 1; rd_mv.MipLevels = 1;
    rd_mv.Format = DXGI_FORMAT_R16G16_FLOAT;
    rd_mv.SampleDesc.Count = 1;
    // Neutral (0, 0) motion buffer for no-MV frames: D3D12_HEAP_FLAG_NONE without
    // D3D12_HEAP_FLAG_CREATE_NOT_ZEROED guarantees zero-initialisation (0.0f, 0.0f).
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd_mv,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                               nullptr, IID_PPV_ARGS(&g.neutral_mv)))) {
        *why = "CreateCommittedResource(neutral_mv) failed"; return false;
    }
    return true;
}

void release_generation(Backend& b);
void release_renodx_feature_internal(uint64_t feature, bool clear_latch);

void drop_device() {
    std::lock_guard<std::recursive_mutex> owner(g_owner_lock);
    // A device is swapped while a frame of ours may still be queued: the whole generation is handed over and released
    // by its own evidence, never here (a pointer change is not proof that the GPU is idle).
    if (g.device && (g.frames > 0 || !g.recordings.empty()) && !generation_done(g)) {
        g_old_generations.push_back(std::make_unique<Backend>(std::move(g)));
        g = Backend{};
        return;
    }
    g.discarding = true;   // the destructors below release their own surfaces here, not into the next generation
    for (auto* l : g.pending_tails) l->Release();   // (never submitted: nothing of this device runs any more)
    g.pending_tails.clear();
    g.renodx.clear();
    g.renodx_retired.clear();
    retire(g.outer_rs);
    for (auto* k : {&g.k_to_linear, &g.k_to_linear32, &g.k_from_linear, &g.k_to_motion, &g.k_to_depth, &g.k_copy_depth_r32}) {
        retire(k->pso);
        *k = {};
    }
    if (g.outer_pool) { g.outer_pool->reset(); g.outer_pool.reset(); }
    for (auto& ls : g.linear_sets) { retire(ls.in); retire(ls.out); retire(ls.mv); retire(ls.depth); retire(ls.hist); }
    g.linear_sets.clear();
    retire(g.neutral_mv);
    if (g.token_map) g.token->Unmap(0, nullptr);
    if (g.done_map) g.done->Unmap(0, nullptr);
    release(g.token); release(g.done);
    if (g.stamps_map) g.stamps->Unmap(0, nullptr);
    release(g.stamps); release(g.queries); g.stamps_map = nullptr; g.hz = 0.0;
    for (auto& u : g.stamp_uses) u.reset();
    for (auto& r : g.token_retired) r.p->Release();
    // The lists belong to this device: nothing of it is in flight once drop_device is called (the same reason the
    // resources above are released here), and a later device must not inherit watermarks from this one.
    for (auto& r : g.retired) r.p->Release();
    g.retired.clear();
    g.token_retired.clear();
    g.token_map = nullptr; g.done_map = nullptr; g.token_slots = 0; g.frames = 0; g.executed = -1;
    g.outer_rs = nullptr;
    g.lin_in = g.lin_out = g.lin_mv = g.lin_depth = nullptr;
    g.neutral_mv = nullptr;
    g.discarding = false;
    g.w = g.h = 0;
    g.split_network.reset(); g.network.reset();
    g.renodx.clear();   // its stages own nothing of the device's that outlives it
    if (g.device) g.device->Release();
    g.device = nullptr;
    g.broken = false;
}

// Releases everything of a generation whose work has finished: the immediate body above, run on that generation.
void release_generation(Backend& b) {
    std::lock_guard<std::recursive_mutex> owner(g_owner_lock);   // `g` itself is swapped here
    Backend keep = std::move(g);
    g = std::move(b);
    drop_device();          // the immediate-release body, on this generation
    g.discarding = false;
    g = std::move(keep);
}

ID3D12Resource* linear_buffer(ID3D12Device* device, uint64_t bytes) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

bool typed_uav(ID3D12Device* device, DXGI_FORMAT f) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = f;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs))) return false;
    return (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
}

void transition(D3D12_RESOURCE_BARRIER& b, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    b = D3D12_RESOURCE_BARRIER{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
}

// The caller sets the selected route once for an evaluation (frame reuse or a member of a frame reuse group).
thread_local const nr::dlssnr::D3d12BackendFrame* reuse_request=nullptr;

// Where the passthrough cannot serve the evaluation the Vulkan bridge runs it - frame reuse too, as every other mode (the
// route rule of every processing; a stream that changes route starts afresh there). A member of a frame reuse group
// takes its whole group there (nr_frame_pair.hpp PresentAge::group_bridge).
bool fallback(const std::string& why) {
    if (reuse_request && nr::frame_pair::PresentAge::active) nr::frame_pair::PresentAge::active->need_bridge();
    note_backend(("not run (the Vulkan bridge): " + why).c_str(), false);
    return false;
}

constexpr uint64_t kFallbackWindowMs = 60000;
constexpr uint32_t kFallbackLatchCount = 8;
constexpr uint32_t kFallbackSwitchLatchCount = 3;

struct FeatureFallback {
    uint64_t window_start_ms = 0;
    uint32_t fallbacks = 0;
    uint32_t transitions = 0;
    bool last_was_direct = false;
    bool latched = false;
    std::string latch_why;
};

// Kept outside Backend `g` so drop_device() on the same ID3D12Device* preserves per-feature bridge latches.
ID3D12Device* g_fallback_device = nullptr;
std::unordered_map<uint64_t, FeatureFallback> g_feature_fallback;
uint64_t g_last_direct_ms = 0;
uint64_t g_idle_evict_ms = 10000;

void release_renodx_feature_internal(uint64_t feature, bool clear_latch);

void maybe_evict_idle_backend() {
    if (!g.device || !g.network || g_last_direct_ms == 0) return;
    const uint64_t elapsed = GetTickCount64() - g_last_direct_ms;
    if (elapsed < g_idle_evict_ms) return;
    nr::pe::log("[d3d12] releasing idle D3D12 passthrough resources after %llu ms on the Vulkan bridge",
                (unsigned long long)elapsed);
    drop_device();
    g_last_direct_ms = 0;
}

bool fallback_for(uint64_t feature, const std::string& why) {
    const uint64_t now = GetTickCount64();
    FeatureFallback& ff = g_feature_fallback[feature];
    if (ff.window_start_ms == 0 || (now - ff.window_start_ms) > kFallbackWindowMs) {
        ff.window_start_ms = now;
        ff.fallbacks = 0;
        ff.transitions = 0;
    }
    ++ff.fallbacks;
    if (ff.last_was_direct) {
        ++ff.transitions;
        ff.last_was_direct = false;
    }
    if (!ff.latched && (ff.fallbacks >= kFallbackLatchCount || ff.transitions >= kFallbackSwitchLatchCount)) {
        ff.latched = true;
        ff.latch_why = why;
        nr::pe::log("[d3d12] feature #%llu latched to the Vulkan bridge after %u fallback(s) / %u switch(es) (%s)",
                    (unsigned long long)feature, ff.fallbacks, ff.transitions, why.c_str());
        release_renodx_feature_internal(feature, false);
    }
    if (ff.latched) {
        if (reuse_request && nr::frame_pair::PresentAge::active) nr::frame_pair::PresentAge::active->need_bridge();
        note_backend(("not run (the Vulkan bridge, latched): " + ff.latch_why).c_str(), false);
    } else fallback(why);
    return false;
}


std::wstring widen(const char* u8) {
    std::wstring out;
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, nullptr, 0);
    if (n > 0) { out.resize(size_t(n) - 1); MultiByteToWideChar(CP_UTF8, 0, u8, -1, out.data(), n); }
    return out;
}

// The RenoDX route's frame (nr_pe_session.cpp's run_renodx, on the D3D12 passthrough): the stage around the network, one a
// feature, the network through this backend's own (g.network) over its linear buffers.
uint64_t stack_feature(uint64_t feature, uint32_t k) {
    return k == 0 ? feature : (uint64_t(1) << 62) | (feature << 8) | k;
}
constexpr uint64_t kUnnamedStack = 0xFFFFFFu;

// NRFrameReuse for this feature's frame (the Vulkan session's reuse_stage, nr_pe_session.cpp, on this route): it runs on
// Passes (legacy), on 8-bit / 10-bit SDR frames with the game's motion vectors and depth; not able to run is not a
// failure - NR runs on every frame, as without it. A passing reason (no vectors / depth for a while: menus, loading) keeps
// the stage; it starts afresh when they come back. `two_sided`: the even-frames conditions (the Present, one NR layer,
// frame generation off or followed, HUD FIX off) - each rebuilt frame made from the NR frames before and after it.
constexpr uint32_t kReusePassingSaid = 30;   // frames
frame_reuse12::Stage* reuse_stage(Backend::RenodxEntry& e, ID3D12Device* device, const nr::renodx::Settings& s,
                                  const renodx12::Frame& rf, DXGI_FORMAT sf, DXGI_FORMAT df, uint32_t w, uint32_t h,
                                  bool neutral, bool scrgb, const char* blocked) {
    auto* age=nr::frame_pair::PresentAge::active;
    const bool batch=age && age->gathering() && age->paired;
    DXGI_FORMAT dfmt = DXGI_FORMAT_UNKNOWN;
    if (rf.depth) dfmt = depth_view_format(rf.depth->GetDesc().Format);
    const bool depth_ok = rf.depth && dfmt != DXGI_FORMAT_UNKNOWN;
    const bool no_motion = neutral || !rf.motion || !rf.fast_motion_available;
    const char* why = blocked ? blocked
                    : s.passes_mode == 1 && s.fast_method == 0 ? nr::frame_reuse::interleaved
                    : !frame_reuse12::Stage::supports(sf) || sf != df ? nr::frame_reuse::unsupported_format
                    : no_motion ? nr::frame_reuse::motion_missing
                    : rf.depth && !depth_ok ? nr::frame_reuse::depth_format : nullptr;
    if (!e.reuse_failed.empty() && std::chrono::steady_clock::now() >= e.alloc_retry_after) e.reuse_failed.clear();
    if (why || !e.reuse_failed.empty()) {
        const bool passing = why == nr::frame_reuse::motion_missing;
        e.reuse_ui = why ? std::string(why) : "Cannot run: " + e.reuse_failed;
        if (e.reuse && passing) {
            if (!e.reuse_passing) { e.reuse->flush(); e.reuse->reset(); }   // the first frame of the pause: afresh after it
        } else if (e.reuse) {
            e.reuse.reset();   // (its images retire through the watermark)
        }
        e.reuse_passing = passing ? e.reuse_passing + 1 : 0;
        if (e.reuse_note != e.reuse_ui) {
            e.reuse_note = e.reuse_ui;
            nr::pe::log("[nr] D3D12 frame reuse: %s (format %d)", e.reuse_ui.c_str(), int(sf));
        }
        return nullptr;
    }
    e.reuse_note.clear();
    e.reuse_passing = 0;
    const uint32_t mw = rf.motion_w, mh = rf.motion_h;
    const uint32_t dw = !rf.depth ? 1u : rf.depth_w ? rf.depth_w : uint32_t(rf.depth->GetDesc().Width);
    const uint32_t dh = !rf.depth ? 1u : rf.depth_h ? rf.depth_h : rf.depth->GetDesc().Height;
    if (e.reuse) {
        const auto& k = e.reuse->config();
        if (k.width != w || k.height != h || k.frame_format != sf || k.motion_width != mw || k.motion_height != mh ||
            k.depth_width != dw || k.depth_height != dh || k.context != depth_ok || k.scrgb != scrgb || k.batch_only!=batch)
            e.reuse.reset();
    }
    if (!e.reuse) {
        frame_reuse12::Stage::Config k;
        k.device = device;
        k.width = w; k.height = h; k.frame_format = sf;
        k.motion_width = mw; k.motion_height = mh; k.depth_width = dw; k.depth_height = dh;
        k.two_sided = true;
        k.scrgb = scrgb; k.batch_only=batch;
        k.context = depth_ok;
        // the even frames with it (the Vulkan session's `even` = bidi + delay): the network split over two frames, every
        // frame shown two frames later
        k.delay = true;
        k.stay_fixed = 6.0f;   // (a translucent HUD over a moving world may change a little in place)
        std::string err;   // (its images count as the RenoDX processing's memory: nr_frame_reuse_d3d12.cpp)
        e.reuse = frame_reuse12::Stage::make(k, [](IUnknown* r) { retire(r); }, &err);
        if (!e.reuse) {
            e.alloc_retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            e.reuse_failed = err.empty() ? std::string("unknown") : err;
            nr::pe::log("[nr] frame reuse could not be made on the D3D12 route: %s (retrying after one second)", e.reuse_failed.c_str());
            e.reuse_note = "could not be made";
            e.reuse_said.insert(e.reuse_note);
            e.reuse_ui = "Cannot run: " + e.reuse_failed;
            return nullptr;
        }
        e.reuse_settings = s;
        e.reuse_frames = 0; e.reuse_logged = {}; e.reuse_window = {}; e.reuse_share = -1.f;
        nr::pe::log("[nr] frame reuse (D3D12) at %ux%u, format %d, guides %ux%u / %ux%u: NR on 1 frame of 2 (%llu MiB)%s", w, h,
                    int(sf), mw, mh, dw, dh, (unsigned long long)(e.reuse->image_bytes() >> 20),
                    "; even frames: the network split over two frames, each rebuilt frame made from the NR frames "
                                "before and after it, every frame shown two frames later");
    } else if (!nr::pe::Session::reuse_settings_equal(e.reuse_settings, s)) {
        e.reuse->reset();
        e.reuse_settings = s;
    }
    return e.reuse.get();
}

bool renodx_frame(const nr::dlssnr::D3d12BackendFrame& f, D3D12_RESOURCE_STATES color_state,
                  D3D12_RESOURCE_STATES output_state) {
    if (!f.renodx_settings) return fallback_for(f.feature, "the RenoDX route without its frame (no settings)");
    // frame reuse's second part waits for the next Present (submit_pending): no NR records anywhere else before it ran
    // (a layer moved off the Present while one waits: its frame passes through once)
    bool tail_waits = false;
    { std::lock_guard<std::recursive_mutex> l(g_owner_lock); tail_waits = !g.pending_tails.empty() && g.pending_isolated==nr::frame_pair::PresentAge::isolated(); }
    if (tail_waits && f.waiting) {   // (a Present layer: its own second part was submitted at the Present's start)
        g.renodx[f.feature].used = std::chrono::steady_clock::now();   // (another NR layer: the delayed one stops delaying)
        *f.waiting = 1;
        note_backend("not run yet: frame reuse's second part waits for the Present", false, true);
        return false;
    }
    const nr::renodx::Settings& s = *f.renodx_settings;
    const D3D12_RESOURCE_DESC sd = f.color->GetDesc(), dd = f.output->GetDesc();
    if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        sd.DepthOrArraySize != 1 || dd.DepthOrArraySize != 1 || sd.SampleDesc.Count != 1 || dd.SampleDesc.Count != 1)
        return fallback_for(f.feature, "the RenoDX route: colour or output is not a single-sampled 2D texture");
    const DXGI_FORMAT df = view_format(dd.Format);
    const DXGI_FORMAT srgb_unorm = sd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM
                                 : sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_B8G8R8A8_UNORM
                                 : DXGI_FORMAT_UNKNOWN;
    const DXGI_FORMAT sf = (f.color != f.output && srgb_unorm != DXGI_FORMAT_UNKNOWN && srgb_unorm == df)
                           ? srgb_unorm : view_format(sd.Format);
    if (sf == DXGI_FORMAT_UNKNOWN || df == DXGI_FORMAT_UNKNOWN) return fallback_for(f.feature, "the RenoDX route: an sRGB format");
    if (!(dd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || !typed_uav(f.device, df))
        return fallback_for(f.feature, "the RenoDX route: the output's format has no typed UAV store");
    const uint32_t w = std::min<uint64_t>({f.width, sd.Width, dd.Width});
    const uint32_t h = std::min<uint64_t>({f.height, sd.Height, dd.Height});
    if (!w || !h) return fallback_for(f.feature, "the RenoDX route: an empty extent");

    // The network's extent (nr_pe_session.cpp's renodx_extent: RenoDX's ComputeNrWorkingResolution, a size the
    // game's guides change to taken once it has held kContractDebounceFrames).
    Backend::RenodxEntry& e = g.renodx[f.feature];
    // Session::record_renodx: only passes removed from the stack surrender their histories. The surviving passes
    // keep their latch/seed; re-added passes start fresh instead of reviving the history from an earlier stack.
    const uint32_t passes = renodx::stack_passes(s);
    const uint64_t named = f.feature ? f.feature : kUnnamedStack;
    if (g.current_network()) {
        for (uint32_t k = passes; k < e.stack; ++k) g.current_network()->release_feature(stack_feature(named, k));
        if (!f.feature && passes == 1 && e.stack > 1) g.current_network()->release_feature(named);
    }
    e.stack = passes;
    uint32_t nw = 0, nh = 0;
    // The game's render size (NRFollowInputRes): the motion vectors' region, the whole texture when the game gives
    // none (0) - Session::renodx_extent's guide size; f.motion_w alone made "Follow input resolution" fall back to
    // the Model resolution scale for every game that hands no subrect.
    const uint32_t guide_w = f.motion_w ? f.motion_w : f.motion ? uint32_t(f.motion->GetDesc().Width) : 0u;
    const uint32_t guide_h = f.motion_h ? f.motion_h : f.motion ? f.motion->GetDesc().Height : 0u;
    renodx::working_resolution(s, w, h, guide_w, guide_h, &nw, &nh);
    const bool settings_changed = e.scale != s.resolution_scale || e.follow != s.follow_input_res || e.w != w ||
                                  e.h != h;
    e.scale = s.resolution_scale;
    e.follow = s.follow_input_res;
    if (e.applied_w == 0 || settings_changed || (nw == e.applied_w && nh == e.applied_h)) {
        e.applied_w = nw; e.applied_h = nh; e.pending_frames = 0;
    } else {
        if (nw != e.pending_w || nh != e.pending_h) { e.pending_w = nw; e.pending_h = nh; e.pending_frames = 0; }
        if (++e.pending_frames >= renodx::kContractDebounceFrames) { e.applied_w = nw; e.applied_h = nh; e.pending_frames = 0; }
        else { nw = e.applied_w; nh = e.applied_h; }
    }

    std::string why;
    // NR off (NeuralUplift): the stage copies the frame through and never calls the network - none is needed, none
    // built (the Vulkan route asks for its runtime only with NR on, Session::renodx_frame)
    if (s.nr_enabled && !g.current_network()->ready(f.device, nw, nh, &why)) {
        if (g.current_network()->building() && f.waiting) {
            *f.waiting = 1;
            note_backend(("not run yet: the network: " + why + " (frames pass through with no NR; the bridge is not used)").c_str(), false, true);
            return false;
        }
        return fallback_for(f.feature, "the RenoDX route: the network: " + why);
    }
    note_network(g.current_network()->name());

    // The network's extent changed with the frame's staying (Model resolution, or NRFollowInputRes after its debounce):
    // the networks are kept per extent, so this feature's history there is from before - restart it as below.
    const bool net_resized = e.stage && (e.nw != nw || e.nh != nh);
    if (!e.stage || e.w != w || e.h != h || e.nw != nw || e.nh != nh || e.format != sf ||
        e.hdr != f.renodx_hdr_flag || e.present != f.renodx_present) {
        renodx12::Stage::Config c;
        c.device = f.device;
        c.shaders = widen(nr::pe::module_folder()) + L"\\dlssnr-amd\\shaders\\renodx";
        c.width = w; c.height = h;
        c.nr_width = nw; c.nr_height = nh;
        c.frame_format = sf;
        c.hdr_flag = f.renodx_hdr_flag != 0;
        c.present = uint32_t(f.renodx_present);
        c.settings = s;
        c.feature = f.feature;
        // Only an object that existed: on the first make there is nothing to retire, and a null entry would be logged
        // as a replaced stage later.
        if (e.stage) g.renodx_retired.push_back({int64_t(g.frames), g.uses_now(), "key", std::move(e.stage)});   // released once its frame's recording is done
        e.reuse.reset();   // (frame reuse's answers belong to that stage's histories)
        auto made = renodx12::Stage::make(c, &why);
        if (made) made->set_retire([](IUnknown* r) { retire(r); });   // released on the backend's watermark
        if (made) {
            made->set_completed([](uint64_t f) { return frame_done(int64_t(f)); });
        }
        if (!made) return fallback_for(f.feature, "the RenoDX route: " + why);
        e.stage = std::move(made);
        e.w = w; e.h = h; e.nw = nw; e.nh = nh; e.format = sf;
        e.hdr = f.renodx_hdr_flag; e.present = f.renodx_present;
        nr::pe::log("[nr] the RenoDX route (D3D12): feature #%u at %ux%u (network %ux%u)", unsigned(f.feature), w, h, nw, nh);
    }
    if (!e.stage->set(s)) {
        // Relative <-> absolute units (NRLinearUnitNits, NRSourceEncoding): a stage made with the new settings
        // takes the frame now (the Vulkan route retires the old one and waits a frame; the bridge is never used here).
        if (e.stage) g.renodx_retired.push_back({int64_t(g.frames), g.uses_now(), "set", std::move(e.stage)});
        e.reuse.reset();
        renodx12::Stage::Config c;
        c.device = f.device;
        c.shaders = widen(nr::pe::module_folder()) + L"\\dlssnr-amd\\shaders\\renodx";
        c.width = w; c.height = h;
        c.nr_width = nw; c.nr_height = nh;
        c.frame_format = sf;
        c.hdr_flag = f.renodx_hdr_flag != 0;
        c.present = uint32_t(f.renodx_present);
        c.settings = s;
        c.feature = f.feature;
        e.stage = renodx12::Stage::make(c, &why);
        if (e.stage) e.stage->set_retire([](IUnknown* r) { retire(r); });
        if (e.stage) e.stage->set_completed([](uint64_t f) { return frame_done(int64_t(f)); });
        if (!e.stage) return fallback_for(f.feature, "the RenoDX route: " + why);
    }

    renodx12::Frame rf;
    rf.colour = f.color;
    rf.colour_state = color_state;
    rf.output = f.output;
    rf.output_state = output_state;
    // A frame or network extent change restarts the feature's history: the networks here are kept per extent, and
    // one taken up again within 250 ms (net's stale rule) would blend the history it had before. (The bridge's
    // Session::run_renodx does not reset on these; see the dx01 notes.)
    rf.reset = f.reset != 0 || resized(f.feature, w, h) || net_resized;
    rf.after_upscale = f.renodx_after_upscale != 0;
    rf.fast_motion_available = f.renodx_fast_motion != 0;
    rf.motion = f.motion;
    rf.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    rf.motion_x = f.motion_x; rf.motion_y = f.motion_y;
    rf.motion_w = f.motion_w ? f.motion_w : (f.motion ? uint32_t(f.motion->GetDesc().Width) : 0);   // 0: the whole texture
    rf.motion_h = f.motion_h ? f.motion_h : (f.motion ? f.motion->GetDesc().Height : 0);
    rf.motion_scale_x = f.motion_scale_x;
    rf.motion_scale_y = f.motion_scale_y;
    rf.depth = f.depth;
    rf.depth_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    rf.depth_x = f.depth_x; rf.depth_y = f.depth_y;
    rf.depth_w = f.depth_w; rf.depth_h = f.depth_h;
    rf.depth_inverted = f.depth_inverted != 0;
    rf.exposure = f.exposure;
    rf.exposure_state = static_cast<D3D12_RESOURCE_STATES>(f.exposure_state);
    rf.pre_exposure = f.pre_exposure;
    rf.exposure_scale = f.exposure_scale;
    rf.auto_exposure = f.auto_exposure != 0;
    rf.create_flags_known = f.create_flags_known != 0;
    rf.preprocess.enabled = f.preprocess != 0;
    rf.preprocess.exposure = f.prep_exposure;
    rf.preprocess.bias_ev = f.prep_bias_ev;
    rf.preprocess.curve = f.prep_curve;
    rf.preprocess.contrast = f.prep_contrast;
    rf.preprocess.saturation = f.prep_saturation;
    rf.preprocess.inverse = f.prep_inverse != 0;
    rf.present_white_nits = f.renodx_white_nits;
    rf.motion_without_depth = g.current_network()->motion_without_depth();
    // The Present layers' depth copies follow the network that runs (Session::renodx_runtime sets it on the bridge)
    if (s.nr_enabled) nr::pe::g_motion_without_depth_now = rf.motion_without_depth ? 1 : 0;
    rf.base_controls.style = f.style;
    rf.base_controls.intensity = f.intensity;
    rf.base_controls.local_tone = f.local_tone;
    rf.base_controls.local_structure = f.local_structure;
    rf.base_controls.skin_structure = f.skin_structure;
    rf.base_controls.automatic_mask = f.automatic_mask != 0;
    // The network answers into the stage's RGBA16F image whatever the frame's format (the Vulkan route's RenoDX
    // runtime is made RGBA16F): no 8-bit rounding of the answer - the resolve and the store do the frame's format
    rf.base_controls.round_u8 = false;
    rf.base_controls.reversible = f.reversible_mode >= 0 && f.reversible_mode <= 4 ? uint32_t(f.reversible_mode) : 0u;
    rf.base_controls.history_strength = f.history_strength;
    rf.base_controls.pre_history = !f.pre_history_off;
    rf.base_controls.seed_fixed = f.seed_fixed != 0;
    rf.base_controls.noise_every_frame = f.noise_every_frame != 0;

    const uint64_t bytes = uint64_t(lin_pitch(nw, 8)) * nh * 8;   // (the stage's lin_proxy / lin_neural: lin_pitch rows)
    const char* nwhy = "NR is disabled";
    frame_reuse12::Stage* reuse = nullptr;   // NRFrameReuse this frame (below)
    bool in_reuse = false;                   // inside its Complete callback
    // Frame reuse's even frames: the list the network records into now (the frame's, then the stage's second part
    // once the split is passed), the second part, the cut in network dispatches over the passes together, the passes
    // recorded so far and the network's dispatches a pass (the Vulkan session's net_cmd / net_cut / net_steps)
    ID3D12GraphicsCommandList* net_cmd = f.cmd;
    ID3D12GraphicsCommandList* net_tail = nullptr;
    uint32_t net_cut = 0, net_pass = 0, net_steps = 0;
    const auto net = [&](uint32_t, uint64_t, const nr::D3d12NetControls& controls, NetTemporal nt,
                         ID3D12Resource* in, ID3D12Resource* out) {
        nt.motion_f16 = true;
        nr::D3d12NetControls c = controls;
        // Frame reuse: a history two frames old (after a rebuilt frame) is followed half as strongly (the Vulkan session's)
        if (in_reuse && reuse->gap() >= 2u) c.history_strength = 0.5f * (c.history_strength >= 0.f ? c.history_strength : 1.f);
        g.current_network()->controls(c);
        g.current_network()->temporal(nt);
        nwhy = "";
        const LinearFrame lin{in, in->GetGPUVirtualAddress(), nw, nh, bytes};
        const LinearFrame lout{out, out->GetGPUVirtualAddress(), nw, nh, bytes};
        bool split_here = false;
        if (net_tail && net_cmd != net_tail) {   // the passes before the cut here, the one with it split, the rest there
            const uint32_t lo = net_pass * net_steps;
            if (net_cut <= lo) net_cmd = net_tail;
            else if (net_cut < lo + net_steps) { g.current_network()->split(net_tail, net_cut - lo); split_here = true; }
        }
        ++net_pass;
        const bool ok = g.current_network()->record(net_cmd, lin, lout, &nwhy);
        if (split_here) net_cmd = net_tail;
        return ok;
    };
    using Jobs=nr::frame_pair::ChainJobs<ID3D12GraphicsCommandList*,frame_reuse12::Frame>;
    Jobs* jobs=nullptr;
    auto* age=nr::frame_pair::PresentAge::active;
    if ((!age || !age->gathering() || !age->paired || !s.frame_reuse) && e.pair_reuse) e.pair_reuse.reset();
    if (age && age->gathering()) {
        if (!s.nr_enabled) {   // (NR off: the group's frames pass - the Vulkan session's rule)
            g_reuse_state.store(2); g_reuse_share.store(-1.f);
            { std::lock_guard<std::mutex> lock(g_facts_lock); g_reuse_ui = "Waiting for the other frame reuse layers"; }
            note_backend_produced(false); return true;
        }
        const bool depth_ok = rf.depth && depth_view_format(rf.depth->GetDesc().Format) != DXGI_FORMAT_UNKNOWN;
        if (rf.depth && !depth_ok) return fallback_for(f.feature, std::string("frame reuse: ") + nr::frame_reuse::depth_format);
        Jobs::Pair pair;
        if(age->paired && s.frame_reuse) {
            frame_reuse12::Stage::Config cfg;
            cfg.device=f.device;cfg.width=w;cfg.height=h;cfg.frame_format=sf;
            cfg.context=depth_ok;cfg.scrgb=f.renodx_present==3;
            cfg.motion_width=rf.motion_w;cfg.motion_height=rf.motion_h;
            cfg.depth_width=!rf.depth?1:rf.depth_w?rf.depth_w:uint32_t(rf.depth->GetDesc().Width);
            cfg.depth_height=!rf.depth?1:rf.depth_h?rf.depth_h:rf.depth->GetDesc().Height;
            cfg.stay_fixed=6.f;
            if (!frame_reuse12::Stage::ensure_pair(e.pair_reuse,cfg,[](IUnknown* r){retire(r);},&why))
                return fallback_for(f.feature, "frame reuse: " + why);
            auto* kernel=e.pair_reuse.get();
            pair=[kernel](ID3D12GraphicsCommandList*& cmd,frame_reuse12::Frame* odd,frame_reuse12::Frame& even,const Jobs::Execute& run){
                return kernel->render_pair(cmd,odd,even,run);
            };
        }
        auto network=g.current_network();
        auto* stage=e.stage.get();
        const uint32_t steps=network->steps();
        auto* device=f.device;
        const int chosen=nr::dlssnr::frame_network();
        jobs=Jobs::add(device,uint32_t(sf),passes*steps,uintptr_t(stage)^uintptr_t(network.get()),
            [network,stage,rf,nw,nh,bytes,steps,s,device,chosen](ID3D12GraphicsCommandList*& cmd,const frame_reuse12::Frame& a,
                                         ID3D12GraphicsCommandList* tail,uint32_t cut,uint32_t gap) {
                auto& override=nr::frame_pair::PresentAge::active->net_override;
                struct RestoreChoice { int& value; int old; ~RestoreChoice(){value=old;} } restore{override,override};
                override=chosen;
                std::string err;
                if (!network->ready(device,nw,nh,&err)) return nr::frame_pair::failed_calls;
                auto r=rf;
                r.colour=a.colour.res; r.colour_state=a.colour.state; r.output=a.output.res; r.output_state=a.output.state;
                r.reset=a.reset; r.motion=a.motion.res; r.motion_state=a.motion.state;
                r.motion_x=a.motion_x; r.motion_y=a.motion_y; r.motion_w=a.motion_w; r.motion_h=a.motion_h;
                if (a.motion_composed) {
                    r.motion_scale_x=s.mvec_scale_x?1.f/s.mvec_scale_x:0.f;
                    r.motion_scale_y=s.mvec_scale_y?1.f/s.mvec_scale_y:0.f; r.motion_in_uav=false;
                }
                r.depth=a.depth.res; r.depth_state=a.depth.state; r.depth_x=a.depth_x; r.depth_y=a.depth_y;
                r.depth_w=a.depth_w; r.depth_h=a.depth_h; r.exposure=a.exposure.res; r.exposure_state=a.exposure.state;
                r.cmd_now=[&]{return cmd;};
                uint32_t pass=0;
                const auto count=stage->record(cmd,r,[&](uint32_t,uint64_t,const nr::D3d12NetControls& input,NetTemporal history,
                                              ID3D12Resource* in,ID3D12Resource* out) {
                    auto controls=input; if(gap>=2)controls.history_strength=.5f*(controls.history_strength>=0.f?controls.history_strength:1.f);
                    history.motion_f16=true; network->controls(controls); network->temporal(history);
                    const uint32_t start=steps*pass++;
                    const bool split=tail && cmd!=tail && cut>start && cut<start+steps;
                    if (tail && cut<=start)cmd=tail;
                    if (split)network->split(tail,cut-start);
                    const char* reason="";
                    const bool ok=network->record(cmd,{in,in->GetGPUVirtualAddress(),nw,nh,bytes},
                                                     {out,out->GetGPUVirtualAddress(),nw,nh,bytes},&reason);
                    if (split)cmd=tail;
                    return ok;
                });
                return stage->record_ok() ? count : nr::frame_pair::failed_calls;
            },std::move(pair));
        if (!jobs || !jobs->ready) {
            note_backend_produced(false); return true;
        }
    }
    nr::pe::d3d12_direct::Scope direct_scope(f.cmd);
    // RenoDX's NR screenshot (nr_pe_session.cpp run_renodx, the same rules): the final picture - OptiScaler's finished
    // picture, NR moved there for the capture (nr_pe_optifix.cpp lock_all), or the .asi's Present layer - with no inline
    // NR on it (clean); the frame before NR here, the output after NR below the stage.
    std::unique_ptr<PendingShot> capture;
    const bool final_frame = f.renodx_present != 0 || f.renodx_finished != 0;
    if (!shot_pending() && s.nr_enabled && !nr::frame_pair::Observation::current && nr::pe::screenshot::wants_final() && final_frame && f.renodx_clean) {
        if (nr::pe::vulkan_format_of(sf) != nr::pe::vulkan_format_of(df)) {
            nr::pe::screenshot::fail("unsupported_present", "the frame's input and output formats differ");
        } else if (!g.booking) {
            nr::pe::screenshot::fail("unsupported_present", "the frame's command list cannot be followed to its end");
        } else {
            D3D12_RESOURCE_DESC region = sd;
            region.Format = sf;
            region.Width = w;
            region.Height = h;
            capture = std::make_unique<PendingShot>();
            if (capture->pair.prepare(f.device, region)) {   // (a refusal has ended the request with its reason)
                capture->ticket = g.booking;
                capture->pair.copy(f.cmd, f.color, color_state, 0);
            } else {
                capture.reset();
            }
        }
    } else if (nr::pe::screenshot::armed() && !s.nr_enabled) {
        nr::pe::screenshot::fail("nr_disabled", "enable NR before taking a comparison");
    }
    if (s.nr_enabled && nr::pe::screenshot::wants_model())
        nr::pe::screenshot::fail("unsupported_diagnostic", "the D3D12 passthrough does not read the model's input and "
                                 "output ([D3D12Passthrough] Enabled = 0 does)");
    e.stage->set_frame(g.frames);
    uint32_t ts = Backend::kTimeRing;
    if (g.stamps_map) {
        for (uint32_t i = 0; i < Backend::kTimeRing; ++i) {
            if (g.stamp_uses[i] ? nr::pe::d3d12_direct::completed(g.stamp_uses[i]) : !g.booking) {
                uint64_t t4[4];   // the stage's start / end, and frame reuse's second part's start / end
                std::memcpy(t4, g.stamps_map + i * 32, 32);
                if (t4[0] && t4[1] > t4[0]) {
                    double ticks = double(t4[1] - t4[0]);
                    if (t4[2] && t4[3] > t4[2]) ticks += double(t4[3] - t4[2]);
                    note_stage_gpu(ticks * 1000.0 / g.hz);
                    std::memset(g.stamps_map + i * 32, 0, 32);
                }
                g.stamp_uses[i].reset();
            }
            if (!g.stamp_uses[i] && ts == Backend::kTimeRing) ts = i;
        }
        if (!g.booking) ts = uint32_t(g.frames % Backend::kTimeRing);
    }
    const bool timed = g.stamps_map && ts < Backend::kTimeRing;
    if (timed) {
        g.stamp_uses[ts] = g.booking;
        std::memset(g.stamps_map + ts * 32, 0, 32);
        f.cmd->EndQuery(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, ts * 4);
    }
    rf.net_history_bytes = s.nr_enabled ? g.current_network()->history_texel_bytes() : 0u;   // (ready() settled the network above)
    rf.style_grade = f.style_grade != 0;
    rf.style_grade_accel = f.style_grade_accel != 0;
    // NRFrameReuse (nr_frame_reuse_d3d12.hpp): two-sided under the even-frames conditions - the Present, this the only NR
    // layer running (another's within 5 s: the Vulkan session's chain_active), frame generation off or followed, HUD FIX
    // off (Session::frames_may_delay)
    const auto now = std::chrono::steady_clock::now();
    e.used = now;
    bool alone = (g_w2_direct_ms.load() == 0 || GetTickCount64() - g_w2_direct_ms.load() >= 5000) && !other_layers(f.layer);
    for (const auto& [k, other] : g.renodx)
        if (&other != &e && other.used.time_since_epoch().count() && now - other.used < std::chrono::seconds(5)) alone = false;
    const bool neutral = f.renodx_present && (s.present_guides == 2u || !f.motion);
    const char* blocked = !f.renodx_present ? nr::frame_reuse::placement
                        : s.present_guides==2 ? nr::frame_reuse::guides_off
                        : age && age->interleaved_conflict ? nr::frame_reuse::interleaved
                        : (nr::frame_pair::PresentAge::active ? nr::frame_pair::PresentAge::active->multiple : !alone) && !nr::frame_pair::PresentAge::terminal() && !nr::frame_pair::PresentAge::isolated() ? nr::frame_reuse::chain_busy : nr::pe::Session::delay_block();
    if (s.frame_reuse) {
        reuse = reuse_stage(e, f.device, s, rf, sf, df, w, h, neutral, f.renodx_present == 3, blocked);
    } else {   // switched off (NR Control, live): the stage and its memory go; on again starts afresh
        if (e.reuse) {
            nr::pe::log("[nr] frame reuse (D3D12) switched off: its %llu MiB go once the GPU is done with them",
                        (unsigned long long)(e.reuse->image_bytes() >> 20));
            e.reuse.reset();   // (its images retire through the watermark)
        }
        e.reuse_note.clear(); e.reuse_said.clear(); e.reuse_failed.clear(); e.reuse_ui.clear();
    }
    if (reuse && e.reuse_epoch!=(jobs?jobs->epoch:0)) { reuse->reset(); e.reuse_epoch=jobs?jobs->epoch:0; }
    uint32_t reuse_state = !s.frame_reuse ? 0u : reuse ? 1u : 2u;
    uint32_t ran = 0;
    int next_late=0;
    bool stage_ok = true, net_ok = false, touched = false, delayed = false;
    if (reuse && s.nr_enabled) {
        frame_reuse12::Frame fr;
        fr.colour = {f.color, color_state, sf, w, h};
        fr.output = {f.output, output_state, df, w, h};
        const D3D12_RESOURCE_DESC md = f.motion->GetDesc();
        fr.motion = {f.motion, rf.motion_state, view_format(md.Format), uint32_t(md.Width), md.Height};
        fr.motion_x = rf.motion_x; fr.motion_y = rf.motion_y; fr.motion_w = rf.motion_w; fr.motion_h = rf.motion_h;
        // the guides see the vectors as the network does: NRMVecScaleX / Y applied (renodx12 applies them itself)
        fr.motion_scale_x = rf.motion_scale_x * s.mvec_scale_x;
        fr.motion_scale_y = rf.motion_scale_y * s.mvec_scale_y;
        if (f.depth) {
            const D3D12_RESOURCE_DESC dd = f.depth->GetDesc();
            fr.depth = {f.depth, rf.depth_state, depth_view_format(dd.Format), uint32_t(dd.Width), dd.Height};
            fr.depth_x = rf.depth_x; fr.depth_y = rf.depth_y;
            fr.depth_w = rf.depth_w ? rf.depth_w : uint32_t(dd.Width);
            fr.depth_h = rf.depth_h ? rf.depth_h : dd.Height;
        }
        // NRDepthMode overrides the game's depth direction (as renodx12 does for its own reads)
        fr.depth_inverted = s.depth_mode == 1u ? false : s.depth_mode == 2u ? true : rf.depth_inverted;
        if (f.exposure) {
            const D3D12_RESOURCE_DESC xd = f.exposure->GetDesc();
            const DXGI_FORMAT xv = depth_view_format(xd.Format) != DXGI_FORMAT_UNKNOWN ? depth_view_format(xd.Format)
                                                                                     : view_format(xd.Format);
            fr.exposure = {f.exposure, rf.exposure_state, xv, uint32_t(xd.Width), xd.Height};
        }
        fr.reset = rf.reset;
        bool stage_ran_ok = true, stage_net = false, stage_touched = false;
        // the stage's time: its second part's start / end too (the slot's two other stamps; read once that part has run)
        if (timed) reuse->set_tail_marks([&](ID3D12GraphicsCommandList* t, bool end) {
            if (!end) { t->EndQuery(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, ts * 4 + 2); return; }
            t->EndQuery(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, ts * 4 + 3);
            t->ResolveQueryData(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, ts * 4 + 2, 2, g.stamps, uint64_t(ts) * 32 + 16);
        });
        const auto paired=[&](ID3D12GraphicsCommandList*& cmd,frame_reuse12::Frame* odd,frame_reuse12::Frame& even,
                              ID3D12GraphicsCommandList* tail,float share){stage_net=true;return jobs->record_pair(cmd,odd,even,tail,share);};
        const auto r=jobs && age->paired ? reuse->record_batch(f.cmd,fr,paired) : reuse->record(f.cmd, fr, [&](ID3D12GraphicsCommandList* cmd, const frame_reuse12::Frame& nf) {
            if (jobs) {
                const auto split=reuse->split();
                const auto n=jobs->record(cmd,nf,split.tail,split.share,reuse->gap());
                stage_net=n!=0;
                return n;
            }
            renodx12::Frame r2 = rf;
            r2.colour = nf.colour.res; r2.colour_state = nf.colour.state;
            r2.output = nf.output.res; r2.output_state = nf.output.state;
            r2.reset = nf.reset;
            r2.motion = nf.motion.res; r2.motion_state = nf.motion.state;
            r2.motion_x = nf.motion_x; r2.motion_y = nf.motion_y; r2.motion_w = nf.motion_w; r2.motion_h = nf.motion_h;
            if (nf.motion_composed) {   // the composed field is in screen units: the stage's own NRMVecScale undone
                r2.motion_scale_x = s.mvec_scale_x != 0.f ? 1.f / s.mvec_scale_x : 0.f;
                r2.motion_scale_y = s.mvec_scale_y != 0.f ? 1.f / s.mvec_scale_y : 0.f;
                r2.motion_in_uav = false;
            }
            r2.depth = nf.depth.res; r2.depth_state = nf.depth.state;
            r2.depth_x = nf.depth_x; r2.depth_y = nf.depth_y; r2.depth_w = nf.depth_w; r2.depth_h = nf.depth_h;
            r2.exposure = nf.exposure.res; r2.exposure_state = nf.exposure.state;
            // the even frames: the network split where the stage says (its share of every pass's dispatches together),
            // the stage going on in the second part after it
            const frame_reuse12::Stage::Split sp = reuse->split();
            net_cmd = cmd; net_pass = 0; net_tail = sp.tail;
            if (sp.tail) {
                net_steps = std::max(1u, g.current_network()->steps());
                const uint32_t total = std::max(1u, passes) * net_steps;
                net_cut = uint32_t(std::lround(double(sp.share) * total));
                r2.cmd_now = [&] { return net_cmd; };
            }
            in_reuse = true;
            const uint32_t n = e.stage->record(cmd, r2, net);
            in_reuse = false;
            net_tail = nullptr; net_cmd = f.cmd;
            stage_ran_ok = e.stage->record_ok();
            stage_net = e.stage->net_ran();
            stage_touched = e.stage->cmd_touched();
            return stage_ran_ok ? n : nr::frame_pair::failed_calls;
        });
        if (!r.ok) {   // (as the Vulkan session: the row says so; the stage starts afresh next frame)
            reuse_state = 2u; e.reuse_ui = nr::frame_reuse::anchor_error;
            nr::pe::control::layer_reuse_failed(f.layer,nr::frame_reuse::anchor_error);
        }
        if (jobs) {age->group_written=r.ok;age->group_share=e.reuse_share;}
        reuse->set_tail_marks({});
        // its second part (even frames): submitted before anything NR records next (submit_pending), and in use until it
        // has run (what it touches is retired behind it)
        const frame_reuse12::Stage::Pending tail = reuse->take_pending();
        if (tail.list) {
            std::lock_guard<std::recursive_mutex> l(g_owner_lock);   // (submit_pending runs at the next Present)
            g.pending_isolated=nr::frame_pair::PresentAge::isolated();
            g.pending_tails.push_back(tail.list);
            if (tail.ticket) g.recordings.push_back({int64_t(g.frames), f.feature, tail.ticket});
            if (timed && tail.ticket) g.stamp_uses[ts] = tail.ticket;   // (the slot read once the second part has run)
        }
        ran = r.network_calls;
        stage_ok = r.ok && stage_ran_ok;
        net_ok = r.completed && stage_net;
        touched = true;   // (the stage's copies are in the list either way)
        (void)stage_touched;
        next_late=r.next_late;
        delayed = r.shown_late != 0;   // (even frames: shown two frames later)
        // NR Control's share of frames with NR (windows of 60 frames) and the log's counts (the Vulkan session's)
        if ((e.reuse_frames + 1) % 60 == 0) {
            const frame_reuse12::Stage::Stats st = reuse->stats();
            if (st.frames > e.reuse_window.frames)
                e.reuse_share = float(double(st.computed - e.reuse_window.computed) / double(st.frames - e.reuse_window.frames));
            e.reuse_window = st;
        }
        if (++e.reuse_frames == 120 || e.reuse_frames % 600 == 0) {
            const frame_reuse12::Stage::Stats st = reuse->stats();
            nr::pe::log("[nr] feature #%u: frame reuse (D3D12), last %llu frames: NR on %llu, rebuilt %llu", unsigned(f.feature),
                        (unsigned long long)(st.frames - e.reuse_logged.frames),
                        (unsigned long long)(st.computed - e.reuse_logged.computed),
                        (unsigned long long)((st.frames - e.reuse_logged.frames) - (st.computed - e.reuse_logged.computed)));
            e.reuse_logged = st;
        }
    } else if (s.frame_reuse && !reuse) {
        if (f.output != f.color && sf == df) {
            D3D12_RESOURCE_BARRIER barrier[2];
            transition(barrier[0], f.color, color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(barrier[1], f.output, output_state, D3D12_RESOURCE_STATE_COPY_DEST);
            for (auto& b : barrier) if (b.Transition.StateBefore != b.Transition.StateAfter) f.cmd->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = f.color; dst.pResource = f.output;
            src.Type = dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_BOX box{0,0,0,w,h,1};
            f.cmd->CopyTextureRegion(&dst,0,0,0,&src,&box);
            for (auto& b : barrier) if (b.Transition.StateBefore != b.Transition.StateAfter) {
                std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                f.cmd->ResourceBarrier(1, &b);
            }
        }
    } else {
        if (reuse) { reuse->flush(); reuse->reset(); }   // NR off for this frame: the next frame with NR starts afresh
        ran = e.stage->record(f.cmd, rf, net);
        stage_ok = e.stage->record_ok();
        net_ok = e.stage->net_ran();
        touched = e.stage->cmd_touched();
    }
    // frame generation's inputs follow a Present whose frames are shown later (nr_asi_hudcap.cpp; Session::frames_delay)
    if (f.renodx_present) nr::pe::Session::note_frames_delayed(s.frame_reuse && delayed ? 2 : 0, s.frame_reuse ? next_late : 0);
    g_reuse_state.store(reuse_state);
    g_reuse_share.store(reuse_state == 1u ? e.reuse_share : -1.f);
    { std::lock_guard<std::mutex> l(g_facts_lock); g_reuse_ui = reuse_state == 2u ? e.reuse_ui : std::string(); }
    if (timed) {
        f.cmd->EndQuery(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, ts * 4 + 1);
        f.cmd->ResolveQueryData(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, ts * 4, 2, g.stamps, uint64_t(ts) * 32);
    }
    if (capture) {
        // (with frame reuse only a frame NR ran on; two-sided, its output is the frame before's picture: the request waits)
        if (stage_ok && net_ok && !delayed) {
            capture->pair.copy(f.cmd, f.output, output_state, 1);
            // HDR stays HDR (nr_pe_session.cpp's rule): PQ codes as they are, scRGB (1.0 = 80 nits) to PQ; SDR as display
            // codes. OptiScaler's finished picture is the back buffer: a float format is scRGB, 10 bits PQ when judged HDR.
            const bool pq = sf == DXGI_FORMAT_R10G10B10A2_UNORM;
            const bool linear = sf == DXGI_FORMAT_R16G16B16A16_FLOAT || sf == DXGI_FORMAT_R32G32B32A32_FLOAT ||
                                sf == DXGI_FORMAT_R11G11B10_FLOAT;
            capture->hdr = f.renodx_present ? uint8_t(f.renodx_present == 2 ? 2 : f.renodx_present == 3 ? 1 : 0)
                                            : uint8_t(linear ? 1 : pq && f.renodx_hdr_flag ? 2 : 0);
            capture->meta = std::string("path=") + (f.renodx_present ? "present" : "optiscaler-finished-picture") +
                            "\ncomparison=final-picture-before-after-nr\nroute=d3d12-passthrough\nstack=" +
                            std::to_string(passes) + "\nwidth=" + std::to_string(w) + "\nheight=" + std::to_string(h) +
                            "\nsource_format=" + std::to_string(int(nr::pe::vulkan_format_of(sf))) +
                            "\nhdr_mode=" + std::to_string(capture->hdr) + "\n";
            capture->deliver = true;
            nr::pe::screenshot::recorded();
        }
        // Otherwise the request stays armed for the next clean frame; the before-NR copy is already in the list, so
        // its buffers are released only once the list has run.
        g_shots.push_back(std::move(capture));
    }
    g_last_nr_w.store(nw); g_last_nr_h.store(nh);
    const auto facts = e.stage->facts();
    g_last_fast_passes.store(facts.fast_passes);
    g_last_fast_fallback.store(facts.fast_fallback);
    g_last_il_missing.store(facts.il_missing);
    { std::lock_guard<std::mutex> l(g_facts_lock); g_last_facts = facts; g_facts_known = true; }
    direct_scope.release();
    // 0 passes is a legitimate copy-through only when the stage says it made everything it needed; otherwise the
    // output may not have been written at all and the caller must take its own path.
    if (!stage_ok) {
        // The frame is dropped after part of it was recorded: its history can no longer be trusted (the original
        // Runtime's rule for a dropped recording), so this feature's state goes rather than a stale latch surviving.
        release_renodx_feature_internal(f.feature, false);
        if (touched) {
            note_backend_produced(false);
            note_backend("dropped partial RenoDX D3D12 frame (bridge skipped)", true);
            return true;
        }
        return fallback_for(f.feature, "the RenoDX stage could not make a surface this frame needs");
    }
    char line[200];
    const bool rebuilt = reuse && s.nr_enabled && !net_ok;   // (frame reuse: a frame rebuilt from the NR frames)
    if (rebuilt)
        std::snprintf(line, sizeof line, "ran frame reuse (D3D12) %ux%u: a rebuilt frame", w, h);
    else if (net_ok)
        std::snprintf(line, sizeof line, "ran the RenoDX stage (D3D12) %ux%u, network %ux%u: %u pass%s", w, h, nw, nh,
                      ran, ran == 1 ? "" : "es");
    else
        std::snprintf(line, sizeof line,
                      "ran the RenoDX stage (D3D12) %ux%u, network %ux%u: %u pass%s, the network recorded nothing (%s)",
                      w, h, nw, nh, ran, ran == 1 ? "" : "es", nwhy);
    (void)touched;
    // what Stage::record returns, the same ran the Vulkan path reports; a rebuilt frame carries NR's edit too
    note_backend_produced(ran > 0 || rebuilt);
    note_backend(line, true);
    return true;
}

void release_renodx_feature_internal(uint64_t feature, bool clear_latch) {
    if (clear_latch) g_feature_fallback.erase(feature);
    g.feature_extent.erase(feature);   // (Session::release_feature)
    if (auto it = g.renodx.find(feature); it != g.renodx.end()) {
        if (it->second.stage) g.renodx_retired.push_back({int64_t(g.frames), g.uses_now(), "feature", std::move(it->second.stage)});
        g.renodx.erase(it);
    }
    for (const auto& net : {g.network,g.split_network}) if (net) {
        const uint64_t named = feature ? feature : kUnnamedStack;
        net->release_feature(feature);
        if (!feature) net->release_feature(named);
        for (uint32_t k = 1; k < renodx::kMaxUnlockedPasses; ++k)
            net->release_feature(stack_feature(named, k));
    }
}
}  // namespace

bool ensure_backend(ID3D12Device* device, std::string* why) {
    if (g.device != device) {
        drop_device();
        device->AddRef();
        {
            std::lock_guard<std::recursive_mutex> l(g_owner_lock);
            g.device = device;
        }
        std::string build_why;
        if (!build(device, &build_why)) {
            g.broken = true;
            g.broken_why = build_why;
        } else {
            g_last_direct_ms = GetTickCount64();
        }
    }
    if (g.broken) {
        if (why) *why = g.broken_why;
        return false;
    }
    if (nr::frame_pair::PresentAge::isolated() && !g.split_network) {
        auto network=make_aco_network();
        const char* error="";
        if (!network->create(device,&error)) { if (why) *why=error; return false; }
        std::lock_guard<std::recursive_mutex> l(g_owner_lock);
        g.split_network=std::move(network);
    }
    return true;
}

bool wilsjo2_ready(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, uint32_t w, uint32_t h, std::string* why,
                   bool* building) {
    if (building) *building = false;
    std::string w_;
    std::string& y = why ? *why : w_;
    if (!enabled()) { y = "the D3D12 passthrough is off (dlssnr-amd.ini [D3D12Passthrough] Enabled = 0)"; return false; }
    if (!device || !cmd) { y = "no device or command list"; return false; }
    // Canary runs before installing Direct state hooks, so unsupported adapters or failed canaries never leave hooks behind.
    on_device(device);   // the canary, once
    const Canary c = canary();
    if (c != Canary::kHit) {
        if (building && (c == Canary::kNotRun || c == Canary::kRunning)) *building = true;
        else maybe_evict_idle_backend();
        y = std::string("canary ") + (c == Canary::kFail ? status().canary : canary_name(c));
        return false;
    }
    // A list state not known yet (the hooks came after its Reset) waits rather than falling back to the bridge.
    bool wait_reset = false;
    if (!nr::pe::d3d12_direct::ready(device, cmd, &y, &wait_reset)) {
        if (building && wait_reset) *building = true;
        else maybe_evict_idle_backend();
        y = "the command list's state is not known (" + y + ")";
        return false;
    }
    if (!ensure_backend(device, &y)) { maybe_evict_idle_backend(); return false; }
    if (!g.current_network()->ready(device, w, h, &y)) {
        const bool is_building = g.current_network()->building();
        if (building) *building = is_building;
        if (!is_building) maybe_evict_idle_backend();
        y = "the network: " + y;
        return false;
    }
    g_w2_direct_ms = GetTickCount64();   // (a wilsjo2 layer wants this route: frame reuse's even frames stop - one NR layer)
    // frame reuse's second part waits for the next Present (submit_pending): no NR records anywhere before it has run
    bool tail_waits = false;
    { std::lock_guard<std::recursive_mutex> l(g_owner_lock); tail_waits = !g.pending_tails.empty() && g.pending_isolated==nr::frame_pair::PresentAge::isolated(); }
    if (tail_waits) {
        y = "frame reuse's second part waits for the Present";
        if (building) *building = true;
        return false;
    }
    note_network(g.current_network()->name());
    g_last_direct_ms = GetTickCount64();
    return true;
}

bool copy_depth_r32(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* src,
                    ID3D12Resource* dst, std::string* why) {
    if (!enabled()) { if (why) *why = "the D3D12 passthrough is off"; return false; }
    if (!device || !cmd || !src || !dst) { if (why) *why = "depth copy: missing device, list, or texture"; return false; }
    on_device(device);
    const Canary c = canary();
    if (c != Canary::kHit) {
        if (why) *why = std::string("canary ") + (c == Canary::kFail ? status().canary : canary_name(c));
        return false;
    }
    if (!nr::pe::d3d12_direct::ready(device, cmd, why)) return false;
    if (!ensure_backend(device, why)) return false;
    if (!g.outer_rs || !g.k_copy_depth_r32.pso) {
        if (why) *why = "copy_depth_r32 outer kernel is not built";
        return false;
    }
    const D3D12_RESOURCE_DESC sd = src->GetDesc(), dd = dst->GetDesc();
    if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        sd.SampleDesc.Count != 1 || dd.SampleDesc.Count != 1) {
        if (why) *why = "depth copy expects single-sampled 2D textures";
        return false;
    }
    const DXGI_FORMAT sf = depth_view_format(sd.Format), df = view_format(dd.Format);
    if (sf == DXGI_FORMAT_UNKNOWN || df == DXGI_FORMAT_UNKNOWN ||
        !(dd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        if (why) *why = "unsupported depth copy texture format or missing UAV flag";
        return false;
    }
    const uint32_t w = std::min<uint64_t>(sd.Width, dd.Width), h = std::min<uint64_t>(sd.Height, dd.Height);
    if (!w || !h) {
        if (why) *why = "depth copy: empty extent";
        return false;
    }
    const auto* b_src = g.k_copy_depth_r32.find_binding(0);
    const auto* b_dst = g.k_copy_depth_r32.find_binding(2);
    if (!b_src || !b_dst) {
        if (why) *why = "copy_depth_r32 descriptor bindings missing";
        return false;
    }
    const nr::gpu::Ticket ticket = nr::pe::d3d12_direct::book(cmd);
    if (!ticket) {
        if (why) *why = "depth copy command list could not be booked";
        return false;
    }
    if (!g.outer_pool) {
        if (why) *why = "outer descriptor pool is not built";
        return false;
    }
    d3d12_outer::OuterSlot slot = g.outer_pool->acquire(ticket, why, g.k_copy_depth_r32.desc_zero_bytes);
    if (!slot.cpu) return false;
    g.recordings.push_back({int64_t(g.frames), 0u, ticket});
    const UINT push[2] = {w, h};
    slot.write_srv_tex(device, b_src, src, sf, 0, 1);
    slot.write_uav_tex(device, b_dst, dst, df, 0);
    slot.write_push(push, sizeof push, &g.k_copy_depth_r32);
    nr::pe::d3d12_direct::Scope direct_scope(cmd);
    slot.dispatch(cmd, g.outer_rs, g.k_copy_depth_r32, (w + 7u) / 8u, (h + 7u) / 8u, 1u, true);
    return true;
}

bool evaluate_backend(const nr::dlssnr::D3d12BackendFrame& f) {
    if (!f.device || !f.cmd || !f.color || !f.output) return false;
    note_layer(f.layer);   // (which layers run here: frame reuse's even frames need one)
    const auto color_state = static_cast<D3D12_RESOURCE_STATES>(f.color_state);
    const auto output_state = static_cast<D3D12_RESOURCE_STATES>(f.output_state);
    const bool renodx = f.renodx_route != 0;

    // Nested members book their own lists. Restore the parent's completion
    // ticket for its remaining outer commands and its final timestamp.
    struct Booking {
        nr::gpu::Ticket previous=g.booking;
        ~Booking(){g.booking=std::move(previous);}
    } booking;
    struct Selection {
        const nr::dlssnr::D3d12BackendFrame* before=reuse_request;
        ~Selection(){reuse_request=before;}
    } selection;
    reuse_request=((f.renodx_settings && f.renodx_settings->frame_reuse) ||
                   (nr::frame_pair::PresentAge::active && nr::frame_pair::PresentAge::active->gathering())) ? &f : nullptr;
    sweep();
    if (renodx && reuse_request && nr::frame_pair::PresentAge::active && nr::frame_pair::PresentAge::active->group_bridge()) {
        note_backend("not run (the Vulkan bridge): another layer of its frame reuse group runs there", false);
        return false;   // (no need_bridge: the member that needs the bridge renews it)
    }
    if (f.device != g_fallback_device) {
        g_feature_fallback.clear();
        g_fallback_device = f.device;
    }
    if (!reuse_request && !g_feature_fallback.empty()) {
        const auto it = g_feature_fallback.find(f.feature);
        if (it != g_feature_fallback.end() && it->second.latched) {
            note_backend(("not run (the Vulkan bridge, latched): " + it->second.latch_why).c_str(), false);
            maybe_evict_idle_backend();
            return false;
        }
    }
    std::string why;
    // As wilsjo2_ready: passing states wait (the frame as it came, no NR, no bridge); lasting ones take the bridge
    const auto wait = [&](const std::string& y) {
        *f.waiting = 1;
        note_backend(("not run yet: " + y + " (frames pass through with no NR; the bridge is not used)").c_str(), false, true);
        return false;
    };
    const auto fail_fallback = [&](const std::string& y) {
        fallback_for(f.feature, y);
        maybe_evict_idle_backend();
        return false;
    };
    on_device(f.device);   // the canary, once
    const Canary c = canary();
    if (c != Canary::kHit) {
        if (f.waiting && (c == Canary::kNotRun || c == Canary::kRunning)) return wait(std::string("canary ") + (c == Canary::kFail ? status().canary : canary_name(c)));
        maybe_evict_idle_backend();
        return fallback(std::string("canary ") + (c == Canary::kFail ? status().canary : canary_name(c)));
    }
    bool wait_reset = false;
    if (!nr::pe::d3d12_direct::ready(f.device, f.cmd, &why, &wait_reset)) {
        if (f.waiting && wait_reset) return wait("the command list's state is not known (" + why + ")");
        return fail_fallback("the command list's state is not known (" + why + ")");
    }
    if (!ensure_backend(f.device, &why)) return fail_fallback(why);
    g.current_network()->history_f16(f.history_f16 != 0);   // [Network] History Precision, live
    g.current_network()->style_grade(f.style_grade != 0);   // [Network] Style Grade, live
    g.current_network()->style_grade_accel(f.style_grade_accel != 0);   // [Network] Style Grade Acceleration, live

    // Frame reuse on a COMPUTE list (DLSS-G's evaluate through Streamline): its second part is a DIRECT list run on the
    // Present's queue, with no order against this compute list - the Vulkan bridge runs it there (its second part a
    // secondary of the compute family inside this list)
    if (renodx && reuse_request && f.cmd->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE)
        return fail_fallback("frame reuse on a compute command list (its second part cannot follow it on this route)");
    if (renodx) {
        g.booking = nr::pe::d3d12_direct::book(f.cmd);
        if (g.booking) g.recordings.push_back({int64_t(g.frames), f.feature, g.booking});
        const bool ran = renodx_frame(f, color_state, output_state);
        // Marked even when the stage reports failure: a failed frame may have recorded some of its dispatches, and
        // those commands still hold internal resources that must not be released until the GPU ran past them.
        mark_frame(f.cmd);
        if (ran) {
            g_last_direct_ms = GetTickCount64();
            if (!g_feature_fallback.empty()) {
                const auto it = g_feature_fallback.find(f.feature);
                if (it != g_feature_fallback.end()) it->second.last_was_direct = true;
            }
        } else if (!f.waiting || !*f.waiting) {
            maybe_evict_idle_backend();
        }
        return ran;
    }

    // The network call of the wilsjo2 flow on D3D12: Preprocess is the flow's own there (wilsjo2_prep, prep_outside);
    // one that asks the network's runtime for it is not this path's.
    if (f.preprocess) return fail_fallback("Preprocess in the network's runtime (the wilsjo2 flow runs its own)");
    const D3D12_RESOURCE_DESC sd = f.color->GetDesc(), dd = f.output->GetDesc();
    if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        sd.DepthOrArraySize != 1 || dd.DepthOrArraySize != 1 || sd.SampleDesc.Count != 1 || dd.SampleDesc.Count != 1)
        return fail_fallback("colour or output is not a single-sampled 2D texture");
    const DXGI_FORMAT sf = view_format(sd.Format), df = view_format(dd.Format);
    if (sf == DXGI_FORMAT_UNKNOWN || df == DXGI_FORMAT_UNKNOWN) return fail_fallback("an sRGB colour or output format");
    const bool have_motion = f.motion != nullptr;
    ID3D12Resource* motion_res = f.motion;
    D3D12_RESOURCE_DESC md{};
    DXGI_FORMAT mf = DXGI_FORMAT_UNKNOWN;
    if (have_motion) {
        md = f.motion->GetDesc();
        mf = view_format(md.Format);
        if (md.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || md.SampleDesc.Count != 1 || mf == DXGI_FORMAT_UNKNOWN || !md.Width ||
            !md.Height)
            return fail_fallback("the motion guide is not a single-sampled 2D texture");
    } else {
        if (!g.neutral_mv) return fail_fallback("the neutral motion texture is missing");
        motion_res = g.neutral_mv;
        md = g.neutral_mv->GetDesc();
        mf = DXGI_FORMAT_R16G16_FLOAT;
    }
    if (!(dd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || !typed_uav(f.device, df))
        return fail_fallback("the output's format has no typed UAV store");
    const uint32_t w = std::min<uint64_t>({f.width, sd.Width, dd.Width}), h = std::min<uint32_t>({f.height, sd.Height, dd.Height});
    if (!w || !h) return fail_fallback("an empty extent");
    Backend::LinearSet* set = nullptr;
    for (auto& ls : g.linear_sets)
        if (ls.w == w && ls.h == h && ls.isolated==nr::frame_pair::PresentAge::isolated()) { set = &ls; break; }
    if (!set) {
        if (g.linear_sets.size() >= Backend::kLinearSets) {   // the least recently used extent goes (retired: in flight)
            auto lru = g.linear_sets.begin();
            for (auto it = g.linear_sets.begin(); it != g.linear_sets.end(); ++it) if (it->used < lru->used) lru = it;
            retire(lru->in); retire(lru->out); retire(lru->mv); retire(lru->depth); retire(lru->hist);
            g.linear_sets.erase(lru);
        }
        const uint64_t bytes8 = uint64_t(lin_pitch(w, 8)) * h * 8;      // (rows of lin_pitch texels)
        Backend::LinearSet ls;
        ls.w = w; ls.h = h; ls.isolated=nr::frame_pair::PresentAge::isolated();
        ls.in = linear_buffer(f.device, bytes8);
        ls.out = linear_buffer(f.device, bytes8);
        ls.mv = linear_buffer(f.device, bytes8);
        // depth: made below when a frame reads it (Motion Without Depth = 1, the default: never; the Vulkan runtime's
        // 1 x 1 stand-in) - 4K RGBA32F is 127 MiB
        if (!ls.in || !ls.out || !ls.mv) {
            retire(ls.in); retire(ls.out); retire(ls.mv);
            return fail_fallback("the linear buffers could not be made");
        }
        g.linear_sets.push_back(ls);
        set = &g.linear_sets.back();
    }
    set->used = uint64_t(g.frames);
    g.lin_in = set->in; g.lin_out = set->out; g.lin_mv = set->mv; g.lin_depth = set->depth;
    g.w = w; g.h = h;
    const bool wants_depth = have_motion && f.depth && !g.current_network()->motion_without_depth();
    DXGI_FORMAT depth_view = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_DESC depth_desc{};
    if (wants_depth) {
        depth_desc = f.depth->GetDesc();
        if (depth_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && depth_desc.SampleDesc.Count == 1 &&
            !(depth_desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) {
            depth_view = depth_view_format(depth_desc.Format);
        }
    }
    bool have_depth = depth_view != DXGI_FORMAT_UNKNOWN;
    if (have_depth && !g.lin_depth) {
        g.lin_depth = set->depth = linear_buffer(f.device, uint64_t(lin_pitch(w, 16)) * h * 16);
        if (!g.lin_depth) {
            have_depth = false;
            nr::pe::log("[d3d12] the depth guide's buffer could not be made; running without depth");
        }
    }
    if (wants_depth && !have_depth) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            nr::pe::log("[d3d12] the depth buffer (DXGI format %u) cannot be read; running without depth",
                        unsigned(depth_desc.Format));
        }
    }
    if (have_depth) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            nr::pe::log("[d3d12] depth read from guide (DXGI format %u -> %u)", unsigned(depth_desc.Format),
                        unsigned(depth_view));
        }
    }
    uint32_t hist_bytes = 0;   // a Passes (fast) Interleaved pass's history handed in: the network's history texel (8 / 16)
    {
        std::string nwhy;
        if (!g.current_network()->ready(f.device, w, h, &nwhy)) {
            // While the passthrough's network builds, no bridge: the model passes the frame through with no NR
            if (g.current_network()->building() && f.waiting) {
                *f.waiting = 1;
                note_backend(("not run yet: " + nwhy + " (frames pass through with no NR; the bridge is not used)").c_str(), false, true);
                return false;
            }
            return fail_fallback("the network: " + nwhy);
        }
        note_network(g.current_network()->name());   // "(building)" until a network is active, then the model
        nr::D3d12NetControls nc;
        nc.style = f.style;
        nc.intensity = f.intensity;
        nc.local_tone = f.local_tone;
        nc.local_structure = f.local_structure;
        nc.skin_structure = f.skin_structure;
        nc.automatic_mask = f.automatic_mask != 0;
        nc.round_u8 = df == DXGI_FORMAT_R8G8B8A8_UNORM || df == DXGI_FORMAT_B8G8R8A8_UNORM || df == DXGI_FORMAT_B8G8R8X8_UNORM;
        nc.reversible = f.reversible_mode >= 0 && f.reversible_mode <= 4 ? uint32_t(f.reversible_mode) : 0u;   // reversible_of
        nc.history_strength = f.history_strength;
        nc.pre_history = !f.pre_history_off;
        nc.seed_fixed = f.seed_fixed != 0;
        nc.noise_every_frame = f.noise_every_frame != 0;
        nc.no_grade = f.no_grade != 0;
        g.current_network()->controls(nc);


        NetTemporal nt;
        nt.motion_va = g.lin_mv->GetGPUVirtualAddress();
        nt.motion_scale_x = have_motion ? f.motion_scale_x : 1.0f;
        nt.motion_scale_y = have_motion ? f.motion_scale_y : 1.0f;
        nt.depth_va = have_depth ? g.lin_depth->GetGPUVirtualAddress() : 0;
        nt.have_depth = have_depth;
        nt.depth_inverted = f.depth_inverted != 0;
        // A feature whose extent changed starts its history again (Session::resized: the bridge's frame and
        // wilsjo2 paths OR it into the frame's Reset; Resize History's carry is not on this path) - a network per
        // extent here, so A -> B -> A within 250 ms would otherwise blend the old A history with its gate open.
        nt.reset = f.reset != 0 || resized(f.feature, w, h);
        nt.feature = f.feature;
        nt.have_motion = have_motion;
        nt.motion_f16 = true;
        // Interleaved's pass history (wilsjo2 on the D3D12 flow): copied below into the network's history layout and read
        // in place of the feature's own (the Vulkan runtime's EngineFrame::history blit)
        if (f.external_history && nt.reset && !f.history) {
            const uint32_t hb = g.current_network()->history_texel_bytes();
            if (hb == 8u || hb == 16u) nt.external_history = true;
        }
        if (f.history && have_motion) {
            const D3D12_RESOURCE_DESC hd = f.history->GetDesc();
            const uint32_t hb = g.current_network()->history_texel_bytes();
            if ((hb == 8u || hb == 16u) && hd.Width >= w && hd.Height >= h) {
                const uint64_t need = uint64_t(lin_pitch(w, hb)) * h * hb;
                if (set->hist && set->hist->GetDesc().Width < need) { retire(set->hist); set->hist = nullptr; }
                if (!set->hist) set->hist = linear_buffer(f.device, need);
                if (set->hist) {
                    hist_bytes = hb;
                    nt.history_va = set->hist->GetGPUVirtualAddress();
                    nt.external_history = f.external_history != 0;
                } else {
                    nr::pe::log("[d3d12] Interleaved: the pass history's buffer could not be made; its network runs on its own history");
                }
            }
        }
        g.current_network()->temporal(nt);
    }
    const bool in_place = f.color == f.output;
    const LinearFrame in{g.lin_in, g.lin_in->GetGPUVirtualAddress(), w, h, uint64_t(lin_pitch(w, 8)) * h * 8};
    const LinearFrame out{g.lin_out, g.lin_out->GetGPUVirtualAddress(), w, h, in.bytes};
    // constant 2 = the rows' pitch in texels (lin_pitch)
    const UINT p8 = lin_pitch(w, 8), p16 = lin_pitch(w, 16);   // the rows' texels (lin_pitch)
    UINT constants[8] = {w, h, p8, 0u, 0u, 0u, 0u, 0u};
    UINT mv_constants[8] = {w, h, p8, 0u, 0u, 0u, 0u, 0u};
    UINT depth_constants[8] = {w, h, p16, 0u, 0u, 0u, 0u, 0u};
    if (have_motion) {
        // nr_runtime.cpp's params[8..11] for a guide sampled in place: the subrect's share of the allocation, its base.
        const float mtw = float(md.Width), mth = float(md.Height);
        const float mw = float(f.motion_w ? f.motion_w : uint32_t(md.Width)), mh = float(f.motion_h ? f.motion_h : md.Height);
        const float region[4] = {mw / mtw, mh / mth, float(f.motion_x) / mtw, float(f.motion_y) / mth};
        std::memcpy(mv_constants + 4, region, sizeof region);
    } else {
        const float region[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        std::memcpy(mv_constants + 4, region, sizeof region);
    }
    if (have_depth) {
        float region[4];
        if (f.depth_x >= depth_desc.Width || f.depth_y >= depth_desc.Height) {
            region[0] = float(depth_desc.Width);
            region[1] = float(depth_desc.Height);
            region[2] = 0.0f;
            region[3] = 0.0f;
        } else {
            const uint32_t max_w = uint32_t(depth_desc.Width) - f.depth_x;
            const uint32_t max_h = depth_desc.Height - f.depth_y;
            region[0] = float(f.depth_w ? std::min(f.depth_w, max_w) : max_w);
            region[1] = float(f.depth_h ? std::min(f.depth_h, max_h) : max_h);
            region[2] = float(f.depth_x);
            region[3] = float(f.depth_y);
        }
        std::memcpy(depth_constants + 4, region, sizeof region);
    }

    const auto* b_in_tex = g.k_to_linear.find_binding(0);
    const auto* b_in_buf = g.k_to_linear.find_binding(3, d3d12_outer::kBuf);
    const auto* b_mv_tex = g.k_to_motion.find_binding(0);
    const auto* b_mv_buf = g.k_to_motion.find_binding(3, d3d12_outer::kBuf);
    const auto* b_from_buf = g.k_from_linear.find_binding(1, d3d12_outer::kBuf);
    const auto* b_from_tex = g.k_from_linear.find_binding(2);
    const auto* b_depth_tex = have_depth ? g.k_to_depth.find_binding(0) : nullptr;
    const auto* b_depth_buf = have_depth ? g.k_to_depth.find_binding(3, d3d12_outer::kBuf) : nullptr;
    const d3d12_outer::OuterKernel& k_hist = hist_bytes == 16u ? g.k_to_linear32 : g.k_to_linear;
    const auto* b_hist_tex = hist_bytes ? k_hist.find_binding(0) : nullptr;
    const auto* b_hist_buf = hist_bytes ? k_hist.find_binding(3, d3d12_outer::kBuf) : nullptr;
    if (!g.outer_rs || !b_in_tex || !b_in_buf || !b_mv_tex || !b_mv_buf || !b_from_buf || !b_from_tex ||
        (have_depth && (!b_depth_tex || !b_depth_buf)) || (hist_bytes && (!b_hist_tex || !b_hist_buf))) {
        return fail_fallback("outer shader bindings incomplete");
    }

    const nr::gpu::Ticket ticket = nr::pe::d3d12_direct::book(f.cmd);
    if (!ticket) return fail_fallback("the command list could not be booked");
    // Frame reuse's even frames (the wilsjo2 flow, nr_asi_wilsjo2.cpp run_native_reuse): the network goes on in the
    // stage's second part after its cut, and the writeback after it there - its descriptor slot booked on that part's
    // recording (it runs at the next Present, after this list's recording may have finished)
    auto* const tail = static_cast<ID3D12GraphicsCommandList*>(f.split_tail);
    const bool split = tail && tail != f.cmd && f.split_cut > 0 && f.split_cut < g.current_network()->steps();
    const nr::gpu::Ticket tail_ticket = split ? nr::pe::d3d12_direct::book(tail) : nr::gpu::Ticket{};
    if (split && !tail_ticket) return fail_fallback("frame reuse's second part could not be booked");
    // Acquire all outer descriptor slots before emitting the first barrier or dispatch so any failure leaves `cmd` clean.
    if (!g.outer_pool) return fail_fallback("outer descriptor pool is not built");
    const uint32_t need_slots = 3u + (have_depth ? 1u : 0u) + (hist_bytes ? 1u : 0u);
    std::string slot_why;
    if (!g.outer_pool->reserve(need_slots, &slot_why)) {
        return fail_fallback(slot_why.empty() ? "no outer descriptor slot available" : slot_why);
    }
    const d3d12_outer::OuterSlot s_in = g.outer_pool->acquire(ticket, &slot_why, g.k_to_linear.desc_zero_bytes);
    const d3d12_outer::OuterSlot s_mv = g.outer_pool->acquire(ticket, &slot_why, g.k_to_motion.desc_zero_bytes);
    const d3d12_outer::OuterSlot s_depth = have_depth ? g.outer_pool->acquire(ticket, &slot_why, g.k_to_depth.desc_zero_bytes) : d3d12_outer::OuterSlot{};
    const d3d12_outer::OuterSlot s_hist = hist_bytes ? g.outer_pool->acquire(ticket, &slot_why, k_hist.desc_zero_bytes) : d3d12_outer::OuterSlot{};
    const d3d12_outer::OuterSlot s_out = g.outer_pool->acquire(split ? tail_ticket : ticket, &slot_why,
                                                               g.k_from_linear.desc_zero_bytes);
    if (!s_in.cpu || !s_mv.cpu || (have_depth && !s_depth.cpu) || (hist_bytes && !s_hist.cpu) || !s_out.cpu) {
        return fail_fallback(slot_why.empty() ? "no outer descriptor slot available" : slot_why);
    }

    g.booking = ticket;
    g.recordings.push_back({int64_t(g.frames), f.feature, g.booking});
    if (split) g.recordings.push_back({int64_t(g.frames), f.feature, tail_ticket});   // (the frame's second recording)

    s_in.write_srv_tex(f.device, b_in_tex, f.color, sf, 0, 1);
    s_in.write_buf(b_in_buf, g.lin_in, in.bytes);
    s_in.write_push(constants, sizeof constants, &g.k_to_linear);

    s_mv.write_srv_tex(f.device, b_mv_tex, motion_res, mf, 0, 1);
    s_mv.write_buf(b_mv_buf, g.lin_mv, uint64_t(p8) * h * 8);
    s_mv.write_push(mv_constants, sizeof mv_constants, &g.k_to_motion);

    if (have_depth) {
        s_depth.write_srv_tex(f.device, b_depth_tex, f.depth, depth_view, 0, 1);
        s_depth.write_buf(b_depth_buf, g.lin_depth, uint64_t(p16) * h * 16);
        s_depth.write_push(depth_constants, sizeof depth_constants, &g.k_to_depth);
    }
    if (hist_bytes) {
        const UINT hist_constants[8] = {w, h, lin_pitch(w, hist_bytes), 0u, 0u, 0u, 0u, 0u};
        s_hist.write_srv_tex(f.device, b_hist_tex, f.history, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 1);
        s_hist.write_buf(b_hist_buf, set->hist, uint64_t(hist_constants[2]) * h * hist_bytes);
        s_hist.write_push(hist_constants, sizeof hist_constants, &k_hist);
    }
    s_out.write_uav_tex(f.device, b_from_tex, f.output, df, 0);
    s_out.write_push(constants, sizeof constants, &g.k_from_linear);

    const UINT gx = (w + 7) / 8, gy = (h + 7) / 8;
    auto* cmd = f.cmd;
    D3D12_RESOURCE_BARRIER b[10];
    ID3D12Resource* copy_back = g.lin_in;
    const char* nwhy = "";
    bool net_ok = false;

    {
        nr::pe::d3d12_direct::Scope direct_scope(cmd);
        // copy-in: colour -> lin_in, motion -> lin_mv, depth -> lin_depth
        int n = 0;
        transition(b[n++], g.lin_in, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(b[n++], g.lin_out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(b[n++], g.lin_mv, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (have_depth)
            transition(b[n++], g.lin_depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (color_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            transition(b[n++], f.color, color_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(n, b);

        s_in.dispatch(cmd, g.outer_rs, g.k_to_linear, gx, gy, 1, true);
        s_mv.dispatch(cmd, g.outer_rs, g.k_to_motion, gx, gy, 1, false);
        if (have_depth) s_depth.dispatch(cmd, g.outer_rs, g.k_to_depth, gx, gy, 1, false);
        if (hist_bytes) {   // Interleaved's pass history (UNORDERED_ACCESS, ours) -> set->hist in the network's history layout
            D3D12_RESOURCE_BARRIER hb2[2];
            transition(hb2[0], f.history, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transition(hb2[1], set->hist, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->ResourceBarrier(2, hb2);
            s_hist.dispatch(cmd, g.outer_rs, k_hist, gx, gy, 1, false);
            transition(hb2[0], f.history, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->ResourceBarrier(1, hb2);
        }
        b[0] = D3D12_RESOURCE_BARRIER{};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b[0].UAV.pResource = nullptr;   // lin_in, lin_mv and lin_depth
        cmd->ResourceBarrier(1, b);

        // the network (both in UNORDERED_ACCESS, and left so); split: on in the second part after the cut
        if (split) g.current_network()->split(tail, f.split_cut);
        net_ok = g.current_network()->record(cmd, in, out, &nwhy);
        std::optional<nr::pe::d3d12_direct::Scope> tail_scope;
        ID3D12GraphicsCommandList* const wc = split ? tail : cmd;   // (the writeback's list)
        if (split) tail_scope.emplace(tail);

        // writeback: lin_out -> output (the input copied straight back when the network recorded nothing)
        n = 0;
        transition(b[n++], g.lin_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (in_place) transition(b[n++], f.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, output_state);
        else if (color_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            transition(b[n++], f.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, color_state);
        if (!net_ok) transition(b[n++], copy_back, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        wc->ResourceBarrier(n, b);
        s_out.write_buf(b_from_buf, net_ok ? out.buffer : copy_back, in.bytes);
        s_out.dispatch(wc, g.outer_rs, g.k_from_linear, gx, gy, 1, true);
        n = 0;
        b[n] = D3D12_RESOURCE_BARRIER{};
        b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b[n++].UAV.pResource = f.output;
        wc->ResourceBarrier(n, b);
        n = 0;
        transition(b[n++], copy_back, net_ok ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
        transition(b[n++], g.lin_out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        transition(b[n++], g.lin_mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        if (have_depth)
            transition(b[n++], g.lin_depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        if (hist_bytes) transition(b[n++], set->hist, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        wc->ResourceBarrier(n, b);
    }

    // The same recording evidence as the RenoDX route: a generation's completeness is every frame it recorded,
    // ordinary and RenoDX alike - one branch alone must not stand for the generation.
    mark_frame(f.cmd);
    char line[200];
    std::snprintf(line, sizeof line, "ran %ux%u%s: %s", w, h, in_place ? " in place" : "",
                  net_ok ? g.current_network()->name() : (std::string("the network recorded nothing (") + nwhy + "): input copied back").c_str());
    note_backend_produced(net_ok);
    note_backend(line, true);
    g_last_direct_ms = GetTickCount64();
    if (!g_feature_fallback.empty()) {
        const auto it = g_feature_fallback.find(f.feature);
        if (it != g_feature_fallback.end()) it->second.last_was_direct = true;
    }
    return true;
}
void release_renodx_feature(uint64_t feature) {
    release_renodx_feature_internal(feature, true);
}


HRESULT device_removed_reason() {
    ID3D12Device* d = nullptr;
    {
        std::lock_guard<std::recursive_mutex> l(g_owner_lock);
        d = g.device;
        if (d) d->AddRef();
    }
    if (!d) return S_OK;
    const HRESULT hr = d->GetDeviceRemovedReason();
    d->Release();
    return hr;
}
void abandon_at_exit() {
    // No lock: the game's other threads are gone, one may have died holding it. Moved into memory never freed, so no
    // destructor of theirs runs.
    new decltype(g.outer_pool)(std::move(g.outer_pool));
    new std::shared_ptr<D3d12PassthroughNetwork>(std::move(g.split_network));
    new std::shared_ptr<D3d12PassthroughNetwork>(std::move(g.network));
    new decltype(g.renodx)(std::move(g.renodx));
    new decltype(g.renodx_retired)(std::move(g.renodx_retired));
    new decltype(g.retired)(std::move(g.retired));
    new decltype(g.recordings)(std::move(g.recordings));
    new decltype(g.booking)(std::move(g.booking));
    new decltype(g_old_generations)(std::move(g_old_generations));
}

uint32_t network_steps() {
    std::lock_guard<std::recursive_mutex> l(g_owner_lock);
    return g.current_network() ? g.current_network()->steps() : 0u;
}

namespace {
std::mutex g_layers_lock;
std::map<int, uint64_t> g_layers_ms;   // layer -> its last run here (GetTickCount64)
}  // namespace
void note_layer(int layer) {
    std::lock_guard<std::mutex> l(g_layers_lock);
    g_layers_ms[layer] = GetTickCount64();
}
bool other_layers(int layer) {
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> l(g_layers_lock);
    for (const auto& [n, at] : g_layers_ms)
        if (n != layer && now - at < 5000) return true;
    return false;
}

void note_recording(ID3D12GraphicsCommandList* list) {
    if (!list) return;
    const nr::gpu::Ticket t = nr::pe::d3d12_direct::book(list);
    if (!t) return;
    std::lock_guard<std::recursive_mutex> l(g_owner_lock);
    g.recordings.push_back({int64_t(g.frames), 0u, t});
}

void push_pending(ID3D12GraphicsCommandList* list, const nr::gpu::Ticket& ticket) {
    if (!list) return;
    std::lock_guard<std::recursive_mutex> l(g_owner_lock);
    g.pending_isolated=nr::frame_pair::PresentAge::isolated();
    g.pending_tails.push_back(list);
    if (ticket) g.recordings.push_back({int64_t(g.frames), 0u, ticket});
}

void retire_resource(IUnknown* resource) {
    if (!resource) return;
    std::lock_guard<std::recursive_mutex> l(g_owner_lock);
    retire(resource);
}

void submit_pending(ID3D12CommandQueue* queue) {
    std::vector<ID3D12GraphicsCommandList*> lists;
    {
        std::lock_guard<std::recursive_mutex> l(g_owner_lock);
        if (g.pending_tails.empty() || !queue) return;
        lists.swap(g.pending_tails);
    }
    std::vector<ID3D12CommandList*> v(lists.begin(), lists.end());
    queue->ExecuteCommandLists(UINT(v.size()), v.data());
    for (auto* l : lists) l->Release();
}

PassthroughNetStatus backend_net_status() {
    std::shared_ptr<D3d12PassthroughNetwork> n;
    {
        std::lock_guard<std::recursive_mutex> l(g_owner_lock);
        n = g.current_network();
    }
    PassthroughNetStatus s = n ? n->status() : PassthroughNetStatus{};
    s.stage_ms = g_stage_avg_ms.load();
    s.nr_w = g_last_nr_w.load();
    s.nr_h = g_last_nr_h.load();
    s.fast_passes = g_last_fast_passes.load();
    s.fast_fallback = g_last_fast_fallback.load();
    s.il_missing = g_last_il_missing.load();
    {
        std::lock_guard<std::mutex> l(g_facts_lock);
        s.renodx_known = g_facts_known;
        s.renodx_hdr_mode = g_last_facts.hdr_mode;
        s.renodx_absolute = g_last_facts.absolute;
        s.renodx_feed = g_last_facts.feed;
        s.renodx_exposure_texture = g_last_facts.exposure_texture;
        s.renodx_divisor = g_last_facts.divisor;
        s.reuse_note = g_reuse_ui;
    }
    s.reuse = g_reuse_state.load();
    s.reuse_share = g_reuse_share.load();
    return s;
}


}  // namespace nr::pe::d3d12_passthrough
