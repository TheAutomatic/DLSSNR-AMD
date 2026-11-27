#pragma once
// Passes (fast) Interleaved's programs on the native D3D12 stages (nr_renodx_d3d12.cpp and
// nr_asi_wilsjo2_d3d12.hpp): outer PAL ELF kernels loaded from NRD12OUT archives and populated
// through 32-byte hardware SRDs written into mapped upload slots via OuterHeapPool.
// Interleaved's step specialization constant (SPIRV_CROSS_CONSTANT_ID_0) selects the captured kernel.
#include "nr_d3d12_outer_elf.hpp"

#include <d3d12.h>

#include <cstddef>
#include <cstdint>

namespace nr::pe::d3d12_il {

// A resource as one dispatch binds it: by its Vulkan binding, the view's format, read (SRV) or written (UAV).
struct Bind {
    uint32_t binding;
    ID3D12Resource* res;
    DXGI_FORMAT format;
    bool write;
};

// Returns true when `kernel` exposes `binding` with texture access matching `write`.
inline bool has_binding(const d3d12_outer::OuterKernel& kernel, uint32_t binding, bool write) {
    return kernel.find_binding(binding, write ? d3d12_outer::kUavTex : d3d12_outer::kSrvTex) != nullptr;
}

// Writes the bound texture descriptors into `slot` using the byte offsets recorded in `kernel`.
inline void write_outer_binds(ID3D12Device* device, const d3d12_outer::OuterSlot& slot,
                              const d3d12_outer::OuterKernel& kernel, const Bind* binds, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const Bind& b = binds[i];
        if (!b.res) continue;
        const auto* ob = kernel.find_binding(b.binding);
        if (!ob) continue;
        if (b.write) slot.write_uav_tex(device, ob, b.res, b.format, 0);
        else slot.write_srv_tex(device, ob, b.res, b.format, 0, 1);
    }
}

}  // namespace nr::pe::d3d12_il
