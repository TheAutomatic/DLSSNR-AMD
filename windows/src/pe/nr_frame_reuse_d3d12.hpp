#pragma once
// Frame reuse (NRFrameReuse) on the D3D12 passthrough's RenoDX route: the schedule of windows/src/core/nr_frame_reuse.cpp
// (the Vulkan routes') and its shaders - the outer archive's frame_reuse/* kernels, the same SPIR-V the Vulkan routes run
// as RADV's machine code - around renodx12::Stage on the game's D3D12 device, never through the Vulkan bridge.
//
// The product's path, decided on the CPU: NR on every other frame, the frames between rebuilt along the game's motion
// vectors from the computed ones, the shown edit filtered over time (display.comp). With `two_sided` (the even-frames
// conditions: the Present, one NR layer, HUD FIX off, frame generation off or followed) a rebuilt frame is made from the
// NR frames before AND after it (compose.comp modes 4 / 2 / 5 / 3, the outline context of context.comp), every frame
// shown one frame later; with `delay` (the even frames, the Vulkan stage's Config::delay) the frame's work after `split`
// of its network goes into a command list of the stage's own - where the Vulkan stage records a secondary command buffer
// the next recording runs first, here a direct list the caller submits before the next Present's (submit_pending):
// every frame takes about the same GPU time (the split balanced on the GPU's times), every frame shown two frames later.
#include "nr_gpu_completion.hpp"

#include <d3d12.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace nr::pe::d3d12_passthrough::frame_reuse12 {

// A texture of the frame as the call has it: the state it is in (and is left in), its view format, its size.
struct Image {
    ID3D12Resource* res = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
};

// One frame (nr::renodx::StageFrame's fields this stage reads): colour read, output written (they may be one texture),
// the game's motion vectors (the region in texels, the scale to screen units) and depth (its region), its exposure.
struct Frame {
    Image colour, output, motion, depth, exposure;
    uint32_t motion_x = 0, motion_y = 0, motion_w = 0, motion_h = 0;
    float motion_scale_x = 1.f, motion_scale_y = 1.f;
    uint32_t depth_x = 0, depth_y = 0, depth_w = 0, depth_h = 0;
    bool depth_inverted = false;
    bool reset = false;
    // In the frame handed to Complete: `motion` is the stage's field of the vectors composed over two frames (after a
    // rebuilt frame), in screen units (scale 1; the caller's own vector scale not applied again); else the game's.
    bool motion_composed = false;
};

class Stage {
public:
    struct Config {
        ID3D12Device* device = nullptr;
        uint32_t width = 0, height = 0;
        DXGI_FORMAT frame_format = DXGI_FORMAT_UNKNOWN;   // the colour's and output's view format (supports())
        // the game's motion and depth textures' sizes (the guides' grids, as the Vulkan stage's GuideExtent)
        uint32_t motion_width = 0, motion_height = 0, depth_width = 0, depth_height = 0;
        bool pair_kernel = false, batch_only = false;
        bool scrgb = false;
        bool two_sided = true;
        bool delay = true;                  // even frames (with two_sided)
        float split = 0.5f;                  // the network's share in a frame's first part, to start from (then balanced)
        float stay_fixed = 2.f;              // compose's fixed-overlay test, codes a pixel may change in place
        bool context = true;                 // the two-sided merge weighs each side by its outline context
        bool display_filter = true;          // the shown edit's temporal filter on every frame
        uint32_t protection = 336625664u;    // compose.comp's rules (bits), the Vulkan stage's default
    };
    // SDR display codes in 8-bit, 10-bit or floating-point textures, the formats the Vulkan stage takes.
    static bool supports(DXGI_FORMAT view);
    // Null and `why`: its programs (the outer archive), its surfaces. `retire` releases a resource once the frames
    // that used it are done (the backend's watermark).
    static std::unique_ptr<Stage> make(const Config& c, std::function<void(IUnknown*)> retire, std::string* why);
    ~Stage();
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

    // The complete NR of the frame it is handed (colour = the stage's copy of the game's frame, output = the stage's
    // answer image, motion / depth / exposure the stage's or the game's, every image in the state it names and to be
    // left in it). Returns the passes that ran the network.
    using Complete = std::function<uint32_t(ID3D12GraphicsCommandList*, const Frame&)>;
    struct Result {
        uint8_t shown_late = 0, next_late = 0;
        bool ok = false;           // false: a surface or a program was missing - the output was not written
        bool completed = false;    // NR ran this frame
        uint32_t network_calls = 0;
    };
    // Records the frame into `cmd` (booked with the passthrough's direct state: d3d12_direct::book / Scope).
    Result record(ID3D12GraphicsCommandList* cmd, const Frame& f, const Complete& complete);
    using PairNetwork=std::function<uint32_t(ID3D12GraphicsCommandList*&,const Frame&)>;
    uint32_t render_pair(ID3D12GraphicsCommandList*&,Frame*,Frame&,const PairNetwork&);
    using Batch=std::function<uint32_t(ID3D12GraphicsCommandList*&,Frame*,Frame&,ID3D12GraphicsCommandList*,float)>;
    Result record_batch(ID3D12GraphicsCommandList*,const Frame&,const Batch&);
    static bool ensure_pair(std::unique_ptr<Stage>&,const Config&,std::function<void(IUnknown*)>,std::string*);
    // Inside the Complete callback of a delayed stage: the list the network goes on in after `share` of its work (the
    // steps of every pass together); tail null: no split.
    // (`ticket`: the second part's recording - what is recorded there books against it)
    struct Split { ID3D12GraphicsCommandList* tail = nullptr; float share = 0.f; nr::gpu::Ticket ticket; };
    Split split() const;
    // A delayed stage after record(): its second part, closed - the caller submits it on the frame's queue before
    // anything NR records next (list: a reference of the caller's, null: none; ticket: its recording). Once a record.
    struct Pending { ID3D12GraphicsCommandList* list = nullptr; nr::gpu::Ticket ticket; };
    Pending take_pending();
    // A delayed stage: called with the second part's list at its start (end false) and at its end (end true), for the
    // caller's timing of the frame's GPU work. Empty: nothing.
    void set_tail_marks(std::function<void(ID3D12GraphicsCommandList*, bool end)> f);
    // The share of the network in a frame's first part now and how often it moved (a delayed stage).
    float balance(uint32_t* moves = nullptr) const;
    // A settings change: the next frame starts afresh (nothing of the scene before is shown after it).
    void reset();
    // NR runs without the stage next (paused, NR off): nothing from before the gap is shown after it (the Vulkan stage's
    // flush; a waiting second part is the caller's to submit anyway).
    void flush();
    // Inside the Complete callback: how many frames old the history it continues is (1, or 2 after a rebuilt frame).
    uint32_t gap() const;
    uint64_t image_bytes() const;
    const Config& config() const;
    struct Stats { uint64_t frames = 0, computed = 0; };
    Stats stats() const;

private:
    Stage() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nr::pe::d3d12_passthrough::frame_reuse12
