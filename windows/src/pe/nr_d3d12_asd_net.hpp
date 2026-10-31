#pragma once
// The real network of the native D3D12 ASD backend (M5): the lowered DLSS-NR graph (nr::D3d12NetPlan, the Vulkan
// runtime's own lowering) run on the game's D3D12 device and command list, every kernel the ACO machine code the
// PSDB holds for its carrier pipeline (net-carriers.bin beside the PSDB: the carrier's root signature and DXIL as the
// SODB has them, one per kernel).
//
// Laid out as nr::Runtime (windows/src/core/nr_runtime.cpp, the same in Mochi's linux/src/core/nr_runtime.cpp) runs a
// network, with the same names where the step is the same:
//   build     - the lowering (plan_d3d12_network: the runtime's model choice and build arguments, NrSession::build
//               plan_only), then one pipeline per kernel; on a thread of ours, the 0.0.8.1 path runs until it is done
//   retarget  - the network memory: the activation arena zero once, the weights with their tail, the noise field
//   controls  - the frame's controls into the push blocks (patch_push, the runtime's own code)
//   record    - every dispatch in plan order with a barrier after it, then the alpha pass
#include "nr_d3d12_asd_backend.hpp"
#include "nr_d3d12_net_plan.hpp"

#include <memory>
#include <string>

namespace nr::pe::asd {

// Who lowers the network: the .asi sets nr::plan_d3d12_network for its installation (nr_asi_main.cpp); a test sets
// one of its own. None set: the backend keeps the placeholder copy network.
using NetPlanner = bool (*)(uint32_t width, uint32_t height, bool experimental, nr::D3d12NetPlan* out, std::string* why);
using NetControlsPatch = void (*)(nr::D3d12NetPlan::Dispatch& d, const nr::D3d12NetControls& c, bool grade);
void set_net_planner(NetPlanner planner, NetControlsPatch patch);
NetPlanner net_planner();

// net-carriers.bin in `folder` (the PSDB's): the network's carriers.
std::unique_ptr<D3d12AsdNetwork> make_aco_network(const std::wstring& folder);

}  // namespace nr::pe::asd
