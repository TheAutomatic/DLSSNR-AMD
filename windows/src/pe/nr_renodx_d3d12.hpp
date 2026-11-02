#pragma once
// The RenoDX stage (windows/src/core/nr_renodx.hpp, 0.0.8.1's Vulkan one) on the native D3D12 ASD route: the same
// host logic and RenoDX's own D3D12 shaders (windows/shaders/renodx *.cs_5_1.hlsl, compiled at runtime with
// d3dcompiler_47 exactly as they are, RENODX_VULKAN not defined - their cbuffer is register(b0), texture binding N
// is tN, image binding N is uN under the codec set's numbering: storage binding 4 -> u0), the network runs between
// encode and resolve through the backend's ASD network (D3d12AsdNetwork) over linear buffers. The frame is taken
// whole: colour copied in, the result stored into the output; no Vulkan bridge.
//
// SDR (hdr_mode 0, the legacy codec) only in this build; the backend keeps HDR frames on the 0.0.8.1 path.
#include "nr_d3d12_asd_backend.hpp"
#include "nr_renodx.hpp"

#include <d3d12.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace nr::pe::asd::renodx12 {
using Settings = nr::renodx::Settings;   // RenoDX's own (nr_renodx.hpp)

// RenoDX's FrameHdrMode on a DXGI format (nr_renodx.cpp's, on Vulkan's): 0 SDR, 1 linear HDR, 2 PQ.
uint8_t frame_hdr_mode(DXGI_FORMAT format, bool hdr_flag);

// One frame, as evaluate_backend hands it over (nr_renodx.hpp StageFrame on the D3D12 route).
struct Frame {
    ID3D12Resource* colour = nullptr;
    D3D12_RESOURCE_STATES colour_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* output = nullptr;
    D3D12_RESOURCE_STATES output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool reset = false;
    bool after_upscale = true;
    bool fast_motion_available = true;
    // The game's motion vectors: the texture, its state during the call, the subrect and the scale that turns a
    // texel into normalised screen units (D3d12BackendFrame's, already normalized_mv_scale).
    ID3D12Resource* motion = nullptr;
    D3D12_RESOURCE_STATES motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    uint32_t motion_x = 0, motion_y = 0, motion_w = 0, motion_h = 0;
    float motion_scale_x = 0.0f, motion_scale_y = 0.0f;
    bool motion_in_uav = false;   // OptiScaler's direct frame: the guide sits in UNORDERED_ACCESS
    ID3D12Resource* depth = nullptr;
    D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    uint32_t depth_x = 0, depth_y = 0, depth_w = 0, depth_h = 0;
    bool depth_inverted = false;
    // The game's exposure as DLSS was given it (the OptiScaler route), for the HDR feed.
    ID3D12Resource* exposure = nullptr;
    D3D12_RESOURCE_STATES exposure_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    float pre_exposure = 1.0f, exposure_scale = 1.0f;
    bool auto_exposure = false, create_flags_known = true;
    Preprocess preprocess{};
    // The frame's own model controls (D3d12BackendFrame's), the base the per-pass ones are built on.
    nr::D3d12NetControls base_controls{};
    bool motion_without_depth = true;   // the network's own ([Network] Motion Without Depth)
    float present_white_nits = 203.0f;
    // The network's motion/depth guides resampled by the stage (to_motion / to_depth at the network's extent);
    // `net` runs one pass: the controls the ASD network takes, its temporal inputs, and the pass's linear buffers
    // (both UNORDERED_ACCESS, `out` to hold the answer). Returns false when it recorded nothing.
    // Optional readback buffers (NW*NH*16 bytes each): when set, the network's input (the proxy) and its answer are
    // copied into them, so two routes' boundary values can be compared; unset costs nothing.
    ID3D12Resource* capture_in = nullptr;
    ID3D12Resource* capture_out = nullptr;
    // The network's answer straight from its linear buffer (NW*NH*16 bytes, float4): the raw value before the
    // stage copies it into a texture, so a zero can be told apart from a lost readback.
    ID3D12Resource* capture_raw = nullptr;
    // The pre block's real input on this route: the linear float4 proxy (NW*NH*16), which the f16 texture dump
    // cannot prove equal to the reference's decoded half image.
    ID3D12Resource* capture_in_raw = nullptr;
    // The guides as the network reads them (the stage's own resampling), NW*NH*16 bytes each: a proxy match does not
    // prove the guides match.
    ID3D12Resource* capture_mv = nullptr;
    ID3D12Resource* capture_depth = nullptr;
    using Net = std::function<bool(uint32_t pass, uint64_t feature, const nr::D3d12NetControls& controls,
                                   const NetTemporal& nt, ID3D12Resource* in, ID3D12Resource* out)>;
};
class Stage {
public:
    struct Config {
        ID3D12Device* device = nullptr;
        std::wstring shaders;        // the folder holding the RenoDX .cs_5_1.hlsl sources
        uint32_t width = 0, height = 0;
        uint32_t nr_width = 0, nr_height = 0;
        DXGI_FORMAT frame_format = DXGI_FORMAT_UNKNOWN;   // the colour's view format
        bool hdr_flag = false;
        uint32_t present = 0;         // nr_renodx.hpp StageConfig::present
        Settings settings{};
        uint64_t feature = 0;         // the feature this stage serves (its histories' owner)
    };
    // False: `why` (the programs, the resources). A stage that fails is not used again.
    static std::unique_ptr<Stage> make(const Config& c, std::string* why);
    ~Stage();

    // False: a change between relative and absolute units needs a new stage (the caller retires this one).
    bool set(const Settings& s);   // nr_renodx.cpp Stage::set (the history / recreate setting rules)
    uint8_t hdr_mode() const;

    // The frame through the stack (nr_renodx.cpp Stage::record: the SDR legacy codec and the HDR v6 one). Returns
    // how many passes ran the network (0: NR off - the frame copied to the output as it is).
    uint32_t record(ID3D12GraphicsCommandList* cmd, const Frame& f, const Frame::Net& net);
    // False when the last record() could not make a surface it needed: 0 passes then is not a copy-through.
    bool record_ok() const;
    // Answers whether the frame with this number has finished on the GPU; a descriptor slot is rewritten only then.
    void set_completed(std::function<bool(uint64_t)> f);
    // The caller's frame number for the next record: what its completed() predicate counts.
    void set_frame(uint64_t f);

    // The last frame's decisions (nr_renodx.hpp Stage::Facts), for the panel's wording.

    struct Facts {
        float fast_passes = 0.f;
        bool motion = false, fast_fallback = false;
        uint8_t hdr_mode = 0;
        bool absolute = false;
        bool prep_exposure = false;
        int feed = -1;
        int exposure_texture = -1;
        float divisor = 0.f;
    };
    Facts facts() const;

    // Where a resource this stage drops goes: the caller retires it (its own completion watermark), because a stage
    // may replace a surface while earlier frames that used it are still in flight. Unset: dropped at once.
    void set_retire(std::function<void(ID3D12Resource*)> f);

private:
    Stage() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nr::pe::asd::renodx12
