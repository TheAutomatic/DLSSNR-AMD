#pragma once
// The native D3D12 ASD backend's frame path (M5 shell), beside the Vulkan bridge: on the game's own command list and
// device, no interop -
//
//   colour --(HLSL copy-in)--> linear buffer --(D3d12AsdNetwork)--> linear buffer --(HLSL writeback)--> output
//
// The network is the one interface the lab's ACO network (M4) drops into; until then PlaceholderCopyNetwork copies its
// input to its output, so the output is the frame as it came in. Runs only on ASD_HIT (nr_d3d12_asd.hpp); any failure
// before a command is recorded hands the frame back to the 0.0.8.1 path.
#include <d3d12.h>
#include <cstdint>

#include "nr_dlssnr_model.hpp"
#include "nr_d3d12_net_plan.hpp"

#include <string>

namespace nr::pe::asd {

// The linear buffers: float4 (RGBA, fp32) per pixel, row-major, width x height, as UAV-capable default-heap buffers.
// PLACEHOLDER layout: the real one comes with the network (M4).
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
    bool reset = false;
    uint64_t feature = 0;
};

// What the ACO network becomes on this side: records into the game's list. Both buffers arrive in UNORDERED_ACCESS
// (the input written, its UAV writes finished) and must be left so, `out` holding the result. `create` once per
// device, on the evaluate's thread; false: the backend does not run.
class D3d12AsdNetwork {
public:
    virtual ~D3d12AsdNetwork() = default;
    virtual const char* name() const = 0;
    virtual bool create(ID3D12Device* device, const char** why) = 0;
    // Before anything is recorded, every evaluate: false (and why) - not ready for this extent yet (the network builds
    // on a thread) or never; the frame takes the 0.0.8.1 path.
    virtual bool ready(ID3D12Device*, uint32_t, uint32_t, std::string*) { return true; }
    // The frame's model controls, before record.
    virtual void controls(const nr::D3d12NetControls&) {}
    // The frame's temporal inputs, before record.
    virtual void temporal(const NetTemporal&) {}
    virtual bool record(ID3D12GraphicsCommandList* cmd, const LinearFrame& in, const LinearFrame& out, const char** why) = 0;
};

// An HLSL compute shader (cs_5_0) through d3dcompiler_47.dll; null and why when it does not compile.
ID3DBlob* compile_cs(const char* source, size_t bytes, const char* entry, std::string* why);

// The backend slot nr::dlssnr::evaluate_d3d12 calls (nr::dlssnr::set_d3d12_backend, from InitializeASI).
bool evaluate_backend(const nr::dlssnr::D3d12BackendFrame& frame);

}  // namespace nr::pe::asd
