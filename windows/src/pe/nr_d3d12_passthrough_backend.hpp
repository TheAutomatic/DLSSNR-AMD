#pragma once
// The D3D12 passthrough's frame path, beside the Vulkan bridge: on the game's own command list and device, no
// interop -
//
//   colour --(HLSL copy-in)--> linear buffer --(D3d12PassthroughNetwork)--> linear buffer --(HLSL writeback)--> output
//
// (the RenoDX route: its stage around the network, nr_renodx_d3d12.hpp). Runs only on its check passed
// (nr_d3d12_passthrough.hpp); any failure before a command is recorded hands the frame back to the Vulkan bridge.
#include <d3d12.h>
#include <cstdint>

#include "nr_dlssnr_model.hpp"
#include "nr_d3d12_net_plan.hpp"
#include "nr_gpu_completion.hpp"

#include <string>

namespace nr::pe::d3d12_passthrough {

// The linear buffers: float4 (RGBA, fp32) per pixel, row-major, width x height, as UAV-capable default-heap buffers.
struct PassthroughNetStatus {
    int running_net = -1;
    int want_net = -1;
    bool building = false;
    int building_net = -1;
    float building_seconds = 0.f;
    float gpu_ms = 0.f;
    float stage_ms = 0.f;
    uint32_t nr_w = 0, nr_h = 0;
    float fast_passes = 0.f;
    bool fast_fallback = false;
    uint8_t il_missing = 0;   // Passes (fast) Interleaved chosen and not run (renodx12::Stage::Facts::il_missing)
    // The RenoDX stage's last frame (renodx12::Stage::facts), for NR Control's engine-data panel: on the passthrough
    // the Vulkan session's own facts are an older bridge frame's (or none)
    bool renodx_known = false;
    uint8_t renodx_hdr_mode = 0;
    bool renodx_absolute = false;
    int renodx_feed = -1;
    int renodx_exposure_texture = -1;
    float renodx_divisor = 0.f;
    // NRFrameReuse on this route (nr::control::LayerStatus::reuse: 0 off, 1 running, 2 cannot run), the share of frames
    // NR ran on (< 0: not known yet) and NR Control's line when it cannot run
    uint32_t reuse = 0;
    float reuse_share = -1.f;
    std::string reuse_note;
};

struct LinearFrame {
    ID3D12Resource* buffer;
    uint64_t va;
    uint32_t width, height;
    uint64_t bytes;
};

// The frame's temporal inputs (nr::Runtime's TemporalFrame + EngineFrame as the D3D12 session fills them): the engine's
// motion already resampled onto the frame (a linear RGBA32F buffer like `in`, .xy the motion vector times MVecScale
// over the motion region's extent; see evaluate_backend), DLSSNR.Reset, the feature the history belongs to.
struct NetTemporal {
    uint64_t motion_va = 0;
    float motion_scale_x = 1.0f, motion_scale_y = 1.0f;
    uint64_t depth_va = 0;
    bool have_depth = false;
    bool depth_inverted = false;
    bool reset = false;
    uint64_t feature = 0;
    bool have_motion = true;
    bool motion_f16 = false;
    // Passes (fast) Interleaved: the history this call's network reads in place of the feature's own (the pass's
    // history as Interleaved made it), a linear buffer laid out as the feature's history images (rows of lin_pitch
    // texels, history_texel_bytes() a texel); 0 the feature's own. Read only where the history is (the gate open).
    uint64_t history_va = 0;
    bool external_history = false;
};

// What the ACO network becomes on this side: records into the game's list. Both buffers arrive in UNORDERED_ACCESS
// (the input written, its UAV writes finished) and must be left so, `out` holding the result. `create` once per
// device, on the evaluate's thread; false: the backend does not run.
class D3d12PassthroughNetwork {
public:
    virtual ~D3d12PassthroughNetwork() = default;
    virtual const char* name() const = 0;
    virtual bool create(ID3D12Device* device, const char** why) = 0;
    // Before anything is recorded, every evaluate: false (and why) - not ready for this extent yet (the network builds
    // on a thread) or never; the frame takes the Vulkan bridge.
    virtual bool ready(ID3D12Device*, uint32_t, uint32_t, std::string*) { return true; }
    // After a false ready(): true when only because the network for that extent is still building (it will run
    // these frames; the caller passes them through, no NR, rather than starting the bridge), false when it never will.
    virtual bool building() const { return false; }
    // [Network] Motion Without Depth: true when the network reads motion without depth (default 1).
    virtual bool motion_without_depth() const { return true; }
    // The frame's model controls, before record.
    virtual PassthroughNetStatus status() const { return {}; }
    virtual void controls(const nr::D3d12NetControls&) {}
    // [Network] History Precision (true: 16, the history images RGBA16F), live: a change starts the history afresh.
    virtual void history_f16(bool) {}
    virtual void style_grade(bool) {}   // [Network] Style Grade, live
    virtual void style_grade_accel(bool) {}   // [Network] Style Grade Acceleration, live (the grade in closed form)
    // The frame's temporal inputs, before record.
    virtual void temporal(const NetTemporal&) {}
    // The bytes of one history texel of the network ready() settled on for the next record (8 RGBA16F, 16 RGBA32F;
    // 0 none): the layout NetTemporal::history_va must have.
    virtual uint32_t history_texel_bytes() const { return 0; }
    virtual bool record(ID3D12GraphicsCommandList* cmd, const LinearFrame& in, const LinearFrame& out, const char** why) = 0;
    // Frame reuse's even frames: the next record goes on in `tail` (a list of the caller's, executed after `cmd`) from
    // its dispatch `cut` (0 < cut < steps()); once. The network's dispatches per record of the network ready() settled on.
    virtual void split(ID3D12GraphicsCommandList* /*tail*/, uint32_t /*cut*/) {}
    virtual uint32_t steps() const { return 0; }
    // The caller destroyed this feature (model.cpp's release_feature): its history, latch and seed must go now.
    virtual void release_feature(uint64_t) {}
};

// Copies a depth-stencil or R32/R16 texture to an R32_FLOAT UAV texture without touching descriptor heaps.
// Caller keeps `src` shader-readable and `dst` in UNORDERED_ACCESS; restores host compute state.
bool copy_depth_r32(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* src,
                    ID3D12Resource* dst, std::string* why);

// The wilsjo2 layer on the native runtime, before anything of its frame is recorded: whether its network calls
// will run on this backend - the passthrough enabled, its check passed, the backend made on `device`, the command list's state
// known and the network ready for w x h (one is built on a thread otherwise: false until it is). False: `why`, and the
// layer takes the Vulkan bridge for the frame.
// `building` (may be null): set when false only because the network is still being made (no bridge then), the command
// list's state is not known yet / its recording is blocked this moment, or the canary has not answered yet (not run,
// running): those frames wait too. The bridge only for lasting reasons.
bool wilsjo2_ready(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, uint32_t w, uint32_t h, std::string* why,
                   bool* building = nullptr);

// The backend slot nr::dlssnr::evaluate_d3d12 calls (nr::dlssnr::set_d3d12_backend, from InitializeASI).
bool evaluate_backend(const nr::dlssnr::D3d12BackendFrame& frame);
// Frame reuse's even frames on this route: a frame's work after the network's split is a command list of the backend's
// own that must run before anything NR records next - the next Present's list (the .asi's Present layers call this
// first, on the swap chain's queue), or `queue` at once when NR is about to record elsewhere. Nothing waiting: nothing.
void submit_pending(ID3D12CommandQueue* queue);
// A resource used by NR's recordings on this route outside the backend's own frames (frame reuse around the wilsjo2
// processing's D3D12 flow, nr_asi_wilsjo2.cpp): released once the frames recorded so far are done (the backend's watermark).
void retire_resource(IUnknown* resource);
// The same route's frame reuse around the wilsjo2 flow, even frames: the network's dispatches a call (the split's unit;
// 0: none known - no split), and a frame's second part handed over (`list`: a reference, released once submitted at the
// next Present by submit_pending; `ticket`: its recording, which the frame's retirements wait for).
uint32_t network_steps();
void push_pending(ID3D12GraphicsCommandList* list, const nr::gpu::Ticket& ticket);
// ... a frame's recording on the game's list (its rebuilt frames too: no network call of the passthrough's marks it):
// what the frame reuse stage retires waits for it
void note_recording(ID3D12GraphicsCommandList* list);
// The NR chain's layers that ran on this route (D3d12BackendFrame::layer, or noted by the .asi): another than `layer`
// within the last 5 s (frame reuse's even frames need one NR layer running)
void note_layer(int layer);
bool other_layers(int layer);
PassthroughNetStatus backend_net_status();
// The process is ending (nr::dlssnr::abandon_at_exit, from DllMain): the network and the RenoDX stages are left as they
// are, never destroyed - their destructors would run after amdxc64.dll / d3d12 were detached (loaded after us) and
// release pipelines made through the driver's extension (a fail-fast inside amdxc64.dll at every game exit).
void abandon_at_exit();


// A game feature the caller destroyed: its RenoDX stage and the network's per-feature state are retired (the stage
// through the completion watermark), so a later feature that reuses the id starts clean.
void release_renodx_feature(uint64_t feature);
// The D3D12 passthrough's device as it stands (S_OK when there is none): a removed device is the one signal a game that dies
// without an exception leaves behind, so a watchdog can poll it.
HRESULT device_removed_reason();


}  // namespace nr::pe::d3d12_passthrough
