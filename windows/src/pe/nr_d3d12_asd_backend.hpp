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

namespace nr::pe::asd {

// The linear buffers: float4 (RGBA, fp32) per pixel, row-major, width x height, as UAV-capable default-heap buffers.
// PLACEHOLDER layout: the real one comes with the network (M4).
struct LinearFrame {
    ID3D12Resource* buffer;
    uint64_t va;
    uint32_t width, height;
    uint64_t bytes;
};

// What the ACO network becomes on this side: records into the game's list. Both buffers arrive in UNORDERED_ACCESS
// (the input written, its UAV writes finished) and must be left so, `out` holding the result. `create` once per
// device, on the evaluate's thread; false: the backend does not run.
class D3d12AsdNetwork {
public:
    virtual ~D3d12AsdNetwork() = default;
    virtual const char* name() const = 0;
    virtual bool create(ID3D12Device* device, const char** why) = 0;
    virtual bool record(ID3D12GraphicsCommandList* cmd, const LinearFrame& in, const LinearFrame& out, const char** why) = 0;
};

// The backend slot nr::dlssnr::evaluate_d3d12 calls (nr::dlssnr::set_d3d12_backend, from InitializeASI).
bool evaluate_backend(const nr::dlssnr::D3d12BackendFrame& frame);

}  // namespace nr::pe::asd
