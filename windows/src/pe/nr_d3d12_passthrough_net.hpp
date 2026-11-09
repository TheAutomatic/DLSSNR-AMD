#pragma once
// The real network of the D3D12 passthrough: the lowered DLSS-NR graph (nr::D3d12NetPlan, the Vulkan runtime's own
// lowering) run on the game's D3D12 device and command list, every kernel a pipeline made from its ACO record (the ACO
// Mode records of the package, dlssnr-amd\aco) as a PAL ELF (nr_d3d12_passthrough_elf.hpp).
//
// Laid out as nr::Runtime (windows/src/core/nr_runtime.cpp) runs a network, with the same names where the step is the
// same:
//   build     - the lowering (plan_d3d12_network: the runtime's model choice and build arguments, NrSession::build
//               plan_only), then one pipeline per kernel; on a thread of ours, the frames pass through until it is done
//   retarget  - the network memory: the activation arena zero once, the weights with their tail; the noise table once
//               frames keep seed 0
//   controls  - the frame's controls into the push blocks (patch_push, the runtime's own code)
//   record    - every dispatch in plan order with a barrier after it, then the alpha pass
#include "nr_d3d12_passthrough_backend.hpp"
#include "nr_d3d12_net_plan.hpp"

#include <memory>
#include <string>

namespace nr::pe::d3d12_passthrough {

// Who lowers the network: the .asi sets nr::plan_d3d12_network for its installation (nr_asi_main.cpp); a test sets
// one of its own. None set: the backend keeps the placeholder copy network.
using NetPlanner = bool (*)(uint32_t width, uint32_t height, int model, bool experimental, nr::D3d12NetPlan* out, std::string* why);
using NetControlsPatch = void (*)(nr::D3d12NetPlan::Dispatch& d, const nr::D3d12NetControls& c, bool grade);
using NetModelQuery = int (*)();
void set_net_planner(NetPlanner planner, NetControlsPatch patch = nullptr, NetModelQuery model_query = nullptr);
NetPlanner net_planner();

std::unique_ptr<D3d12PassthroughNetwork> make_aco_network();

}  // namespace nr::pe::d3d12_passthrough
