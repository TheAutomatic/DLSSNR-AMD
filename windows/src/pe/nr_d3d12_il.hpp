#pragma once
// Passes (fast) Interleaved's programs on the native D3D12 stages (the RenoDX one, nr_renodx_d3d12.cpp, and the wilsjo2
// one, nr_asi_wilsjo2_d3d12.hpp): the Vulkan stages' il / il_pp SPIR-V as HLSL (build_d3d12_hlsl.sh, SPIRV-Cross keeps
// each binding as its register: texture N -> tN, image N -> uN, the push block a cbuffer at b0). Their bindings are
// spread up to 43, so each program gets a root signature of its own made from the registers its text declares: the
// root constants, then one table of its SRVs and one of its UAVs, packed (the k-th declared register of a kind at the
// k-th descriptor) - a dispatch takes as many contiguous descriptors as the program declares registers.
// il's step is its specialization constant (SPIRV_CROSS_CONSTANT_ID_0): a pipeline per step, as the Vulkan stages'.
#include "nr_d3dcompiler.hpp"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace nr::pe::d3d12_il {

struct Layout {
    std::vector<uint32_t> srv, uav;   // the registers the program declares (tN, uN), ascending
    uint32_t dwords = 0;              // its push block, in root constants
    ID3D12RootSignature* rs = nullptr;
    uint32_t descriptors() const { return uint32_t(srv.size() + uav.size()); }
};

// The tN / uN registers a SPIRV-Cross HLSL text declares.
inline void registers(const std::string& text, Layout* out) {
    out->srv.clear();
    out->uav.clear();
    for (size_t at = text.find("register("); at != std::string::npos; at = text.find("register(", at + 9)) {
        const char kind = text[at + 9];
        if (kind != 't' && kind != 'u') continue;
        const uint32_t n = uint32_t(std::strtoul(text.c_str() + at + 10, nullptr, 10));
        auto& v = kind == 't' ? out->srv : out->uav;
        if (std::find(v.begin(), v.end(), n) == v.end()) v.push_back(n);
    }
    std::sort(out->srv.begin(), out->srv.end());
    std::sort(out->uav.begin(), out->uav.end());
}

// The root signature of `layout` (its registers and dwords set): constants at b0, the SRV table, the UAV table - each
// table one range per run of consecutive registers.
inline bool make_root_signature(ID3D12Device* device, Layout* layout, std::string* why) {
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges[2];
    const std::vector<uint32_t>* regs[2] = {&layout->srv, &layout->uav};
    for (int k = 0; k < 2; ++k) {
        const auto& v = *regs[k];
        for (size_t i = 0; i < v.size();) {
            size_t j = i + 1;
            while (j < v.size() && v[j] == v[j - 1] + 1) ++j;
            D3D12_DESCRIPTOR_RANGE r{};
            r.RangeType = k == 0 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            r.NumDescriptors = UINT(j - i);
            r.BaseShaderRegister = v[i];
            r.OffsetInDescriptorsFromTableStart = UINT(i);
            ranges[k].push_back(r);
            i = j;
        }
    }
    D3D12_ROOT_PARAMETER p[3]{};
    UINT np = 0;
    p[np].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[np].Constants.ShaderRegister = 0;
    p[np].Constants.Num32BitValues = layout->dwords;
    ++np;
    for (int k = 0; k < 2; ++k) {
        if (ranges[k].empty()) continue;
        p[np].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[np].DescriptorTable.NumDescriptorRanges = UINT(ranges[k].size());
        p[np].DescriptorTable.pDescriptorRanges = ranges[k].data();
        ++np;
    }
    for (UINT i = 0; i < np; ++i) p[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = np;
    rd.pParameters = p;
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (errors) errors->Release();
    if (FAILED(hr) || !blob) { *why = "D3D12SerializeRootSignature (Interleaved) failed"; return false; }
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&layout->rs));
    blob->Release();
    if (FAILED(hr) || !layout->rs) { *why = "CreateRootSignature (Interleaved) failed"; return false; }
    return true;
}

// One program's text compiled for `step` (il's specialization constant; ~0u: none, the text's own default), cs_5_1
// (registers past u7) with System32's compiler.
inline ID3DBlob* compile(const std::string& text, uint32_t step, std::string* why) {
    const PFN_D3DCompile47 fn = system_d3dcompile();
    if (!fn) { *why = "d3dcompiler_47.dll is missing"; return nullptr; }
    char value[16];
    std::snprintf(value, sizeof value, "%uu", step);
    const D3D_SHADER_MACRO macros[] = {{"SPIRV_CROSS_CONSTANT_ID_0", value}, {nullptr, nullptr}};
    ID3DBlob *code = nullptr, *errors = nullptr;
    const HRESULT hr = fn(text.data(), text.size(), "nr_il", step == ~0u ? nullptr : macros, nullptr, "main", "cs_5_1",
                          1u << 15, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        *why = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer())) : std::string("D3DCompile failed");
        if (errors) errors->Release();
        if (code) code->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    return code;
}

// A resource as one dispatch binds it: by its Vulkan binding, the view's format, read (SRV) or written (UAV).
struct Bind {
    uint32_t binding;
    ID3D12Resource* res;
    DXGI_FORMAT format;
    bool write;
};

// The dispatch's descriptors at `base` (layout.descriptors() of them, `inc` apart): each bound resource at its
// register's place; a register nothing is bound to gets a null descriptor (the step that declares it never reads it).
// False when a bound binding is not one of the program's registers of that kind.
inline bool write_descriptors(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE base, UINT inc, const Layout& layout,
                              const Bind* binds, size_t count) {
    const size_t ns = layout.srv.size();
    std::vector<const Bind*> at(layout.descriptors(), nullptr);
    for (size_t i = 0; i < count; ++i) {
        const auto& v = binds[i].write ? layout.uav : layout.srv;
        const auto it = std::find(v.begin(), v.end(), binds[i].binding);
        if (it == v.end()) return false;
        at[(binds[i].write ? ns : 0) + size_t(it - v.begin())] = &binds[i];
    }
    for (size_t k = 0; k < at.size(); ++k) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = base;
        h.ptr += SIZE_T(k) * inc;
        const Bind* b = at[k];
        if (k < ns) {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Format = b ? b->format : DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(b ? b->res : nullptr, &d, h);
        } else {
            D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
            d.Format = b ? b->format : DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(b ? b->res : nullptr, nullptr, &d, h);
        }
    }
    return true;
}

}  // namespace nr::pe::d3d12_il
