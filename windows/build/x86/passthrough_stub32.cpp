// 32-bit build of dlssnr_amd.addon32: the D3D12 passthrough backend, NR Control's transport and the panel's install are
// 64-bit modules (they link D3D12 and the bridge). The model and the session reference their state, so a 32-bit
// process - which only ever takes the Vulkan route through DXVK - answers as "no passthrough, no panel" here.
#include "nr_d3d12_passthrough.hpp"
#include "nr_d3d12_passthrough_backend.hpp"
#include "nr_pe_control.hpp"
#include "nr_pe_menu.hpp"

namespace nr::pe::d3d12_passthrough {
bool backend_produced() { return false; }
PassthroughNetStatus backend_net_status() { return PassthroughNetStatus{}; }
Status status() { return Status{}; }
nr::arch::GpuArch arch() { return nr::arch::GpuArch::unknown; }
void release_renodx_feature(uint64_t) {}
}

namespace nr::pe::control {
void report(const Status&) {}
}

namespace nr::pe::menu {
void install() {}
}
