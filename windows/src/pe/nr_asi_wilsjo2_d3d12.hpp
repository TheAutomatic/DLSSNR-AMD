// The wilsjo2 stage on the native D3D12 route (M5 v2): its programs - the Vulkan stage's own SPIR-V as DXBC
// (windows/build/build_wilsjo2_d3d12.sh) - as D3D12 compute pipelines the driver makes, recorded into the game's list.
// nr_asi_wilsjo2.cpp's route; a header so the offline test (nr_wilsjo2_d3d12_test.cpp) runs the very same class.
#pragma once

#include "nr_wilsjo2.hpp"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef NR_W2D3D12_LOG
#include "nr_pe_log.hpp"
#define NR_W2D3D12_LOG ::nr::pe::log
#endif

namespace nr::asi::native {
namespace w2 = nr::wilsjo2;

inline DXGI_FORMAT srv_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return DXGI_FORMAT_UNKNOWN;   // the bridge reads these (an sRGB view decodes; not decided here)
        default: return f;
    }
}
inline DXGI_FORMAT uav_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
        default: { const DXGI_FORMAT s = srv_format(f); return s == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
                                                         s == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ? DXGI_FORMAT_UNKNOWN : s; }
    }
}

// The surface as the flow's Image (nr_wilsjo2.hpp): `image` the resource, `format` its DXGI format (0: none).
inline w2::Image image(ID3D12Resource* r) {
    w2::Image i;
    if (!r) return i;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    i.image = reinterpret_cast<VkImage>(r);
    i.format = VkFormat(d.Format);
    i.width = uint32_t(d.Width); i.height = d.Height;
    return i;
}
inline ID3D12Resource* resource(const w2::Image& i) { return reinterpret_cast<ID3D12Resource*>(i.image); }

// mode -> program (the Vulkan stage's list, nr_wilsjo2.cpp Stage::Stage): required ones first
struct ProgramFile { w2::Mode mode; const char* name; bool required; };
inline constexpr ProgramFile kPrograms[] = {
    {w2::kEncode, "encode", true}, {w2::kResolve, "resolve", true}, {w2::kMeter, "meter", true},
    {w2::kClamp, "clamp", true}, {w2::kDownsample, "downsample", true},
    {w2::kOsUp, "os", false}, {w2::kOsDown, "os", false}, {w2::kOsEasu, "os", false},
    {w2::kSpatialPack, "spatial", false}, {w2::kSpatialUnpack, "spatial", false}, {w2::kSpatialGuides, "spatial_guides", false},
    {w2::kAutoExposure, "autoexposure", false},
    {w2::kFinishedToLinear, "finished", false}, {w2::kFinishedToPq, "finished", false},
    {w2::kLight, "light", false}, {w2::kPrepMeter, "prep_meter", false}, {w2::kPrepForward, "prep", false},
    {w2::kPrepBack, "prep", false}, {w2::kClassicUp, "classic", false},
    {w2::kCopy, "copy", false}, {w2::kCopyLoad, "copy_load", false}, {w2::kInspect, "inspect", false},
    {w2::kHudClear, "hudfix_clear", false}, {w2::kHudCount, "hudfix_count", false},
    {w2::kHudEstimate, "hudfix_estimate", false}, {w2::kHudCompose, "hudfix_compose", false},
};

class Stage12 final : public w2::Stage::Backend {
public:
    static constexpr UINT kSlots = 4096;
    static constexpr UINT kStride = 256;   // the Vulkan stage's push-constant block
    static_assert(sizeof(w2::Constants) <= kStride, "the constants fit the block");   // dispatches before a slot (6 descriptors, 256 bytes) is reused
    using Ensure = std::function<void(ID3D12Resource*, D3D12_RESOURCE_STATES)>;

    ~Stage12() override {
        for (auto* p : pso_) if (p) p->Release();
        for (auto* c : copies_) c->Release();
        if (upload_) upload_->Release();
        if (heap_) heap_->Release();
        if (rs_) rs_->Release();
    }
    // On a thread of ours: the programs of `folder` (wilsjo2_<name>.dxbc). False: why.
    bool make(ID3D12Device* device, const std::wstring& folder, std::string* why) {
        device_ = device;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 4;   // t0..t3
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        ranges[1].BaseShaderRegister = 4;   // u4, u5 (the Vulkan bindings 4, 5)
        D3D12_ROOT_PARAMETER p[3]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[0].DescriptorTable.NumDescriptorRanges = 1;
        p[0].DescriptorTable.pDescriptorRanges = &ranges[0];
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1;
        p[1].DescriptorTable.pDescriptorRanges = &ranges[1];
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[2].Descriptor.ShaderRegister = 6;   // the constants (ByteAddressBuffer cb : register(t6))
        for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // Spec 1.2 (the Vulkan stage's sampler): bilinear, mip-linear, clamp to the edge on all three axes, no LOD clamp
        D3D12_STATIC_SAMPLER_DESC ss[4]{};
        for (UINT i = 0; i < 4; ++i) {
            ss[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            ss[i].MaxAnisotropy = 1;
            ss[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            ss[i].MinLOD = 0.0f;
            ss[i].MaxLOD = D3D12_FLOAT32_MAX;
            ss[i].ShaderRegister = i;
            ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 3; rd.pParameters = p;
        rd.NumStaticSamplers = 4; rd.pStaticSamplers = ss;
        ID3DBlob *blob = nullptr, *errors = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        if (errors) errors->Release();
        if (FAILED(hr) || !blob) { *why = "D3D12SerializeRootSignature failed"; return false; }
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs_));
        blob->Release();
        if (FAILED(hr)) { *why = "CreateRootSignature failed"; return false; }
        std::map<std::string, ID3D12PipelineState*> made;
        for (const ProgramFile& pf : kPrograms) {
            auto it = made.find(pf.name);
            if (it == made.end()) {
                const std::wstring path = folder + L"\\wilsjo2_" + std::wstring(pf.name, pf.name + std::strlen(pf.name)) + L".dxbc";
                std::vector<char> code;
                if (FILE* f = _wfopen(path.c_str(), L"rb")) {
                    char buf[65536];
                    size_t n;
                    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) code.insert(code.end(), buf, buf + n);
                    std::fclose(f);
                }
                ID3D12PipelineState* pso = nullptr;
                if (!code.empty()) {
                    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
                    pd.pRootSignature = rs_;
                    pd.CS.pShaderBytecode = code.data();
                    pd.CS.BytecodeLength = code.size();
                    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) pso = nullptr;
                }
                it = made.emplace(pf.name, pso).first;
                if (!pso) {
                    if (pf.required) { *why = std::string("wilsjo2_") + pf.name + ".dxbc " + (code.empty() ? "is missing" : "does not make a pipeline"); return false; }
                    NR_W2D3D12_LOG("[asd] wilsjo2 on D3D12: wilsjo2_%s.dxbc %s (its feature takes the bridge)", pf.name,
                                code.empty() ? "is missing" : "does not make a pipeline");
                }
            }
            if (it->second) {
                if (pso_[pf.mode]) pso_[pf.mode]->Release();
                it->second->AddRef();
                pso_[pf.mode] = it->second;
            }
        }
        for (auto& [name, pso] : made) if (pso) pso->Release();
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kSlots * 6;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) { *why = "CreateDescriptorHeap failed"; return false; }
        inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = uint64_t(kSlots) * kStride; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&upload_)))) { *why = "the constants' upload buffer could not be made"; return false; }
        void* m = nullptr;
        if (FAILED(upload_->Map(0, nullptr, &m)) || !m) { *why = "the constants' upload buffer could not be mapped"; return false; }
        mapped_ = static_cast<uint8_t*>(m);
        return true;
    }
    // A recording on `cmd` (its state saved by the caller): our heap and root signature bound; `ensure` puts a resource
    // in a state (the route's tracking).
    void begin(ID3D12GraphicsCommandList* cmd, Ensure ensure) {
        cmd_ = cmd;
        ensure_ = std::move(ensure);
        ID3D12DescriptorHeap* heaps[] = {heap_};
        cmd_->SetDescriptorHeaps(1, heaps);
        cmd_->SetComputeRootSignature(rs_);
    }
    void end() { cmd_ = nullptr; ensure_ = nullptr; }
    bool has(w2::Mode mode) const override { return mode < kModes && pso_[mode]; }
    void record(const w2::Dispatch& d) override {
        const uint32_t mode = d.constants.mode;
        if (!cmd_) throw std::runtime_error("wilsjo2 stage (D3D12): recorded outside a recording");
        if (mode >= kModes || !pso_[mode]) throw std::runtime_error("wilsjo2 stage (D3D12): no program for mode " + std::to_string(mode));
        if (!d.t0 || !d.u0) throw std::runtime_error("wilsjo2 stage (D3D12): a dispatch without t0 or u0");
        // Spec 6.3: a read slot without a resource is given t0's, a missing u1 is given u0's.
        w2::Image read_images[4] = {d.t0, d.t1 ? d.t1 : d.t0, d.t2 ? d.t2 : d.t0, d.t3 ? d.t3 : d.t0};
        const w2::Image* reads[4] = {&read_images[0], &read_images[1], &read_images[2], &read_images[3]};
        const w2::Image* writes[2] = {&d.u0, d.u1 ? &d.u1 : &d.u0};
        // An image read and written by one dispatch (the resolve in place: the frame at t0 / t2 and u0): read through a
        // copy of it made just before - what each invocation reads in Vulkan's GENERAL layout, the image as it was; one
        // resource cannot be a shader resource and an unordered-access one at once in D3D12.
        ID3D12Resource *copied = nullptr, *copy = nullptr;
        for (w2::Image& ri : read_images) {
            ID3D12Resource* r = resource(ri);
            if (r != resource(d.u0) && r != resource(*writes[1])) continue;
            if (copied && r != copied) throw std::runtime_error("wilsjo2 stage (D3D12): a dispatch reads two of the images it writes");
            if (r != copied) { copied = r; copy = copy_of(r); }
            ri.image = reinterpret_cast<VkImage>(copy);
        }
        for (const w2::Image* wi : writes) ensure_(resource(*wi), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (const w2::Image* ri : reads)
            if (!is_copy(resource(*ri))) ensure_(resource(*ri), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const UINT slot = slot_;
        slot_ = (slot_ + 1) % kSlots;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(slot) * 6 * inc_;
        gpu.ptr += UINT64(slot) * 6 * inc_;
        for (UINT i = 0; i < 4; ++i) {
            ID3D12Resource* r = resource(*reads[i]);
            const D3D12_RESOURCE_DESC rd = r->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = srv_format(rd.Format);
            if (v.Format == DXGI_FORMAT_UNKNOWN || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.SampleDesc.Count != 1)
                throw std::runtime_error("wilsjo2 stage (D3D12): an image it cannot read (format " + std::to_string(int(rd.Format)) + ")");
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (reads[i]->layer != 0 || reads[i]->mip >= rd.MipLevels)
                throw std::runtime_error("wilsjo2 stage (D3D12): a read of an array layer or a missing mip level");
            v.Texture2D.MostDetailedMip = reads[i]->mip;
            v.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
            h.ptr += SIZE_T(i) * inc_;
            device_->CreateShaderResourceView(r, &v, h);
        }
        for (UINT i = 0; i < 2; ++i) {
            ID3D12Resource* r = resource(*writes[i]);
            const D3D12_RESOURCE_DESC rd = r->GetDesc();
            D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
            v.Format = uav_format(rd.Format);
            if (v.Format == DXGI_FORMAT_UNKNOWN || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                throw std::runtime_error("wilsjo2 stage (D3D12): an image it cannot write (format " + std::to_string(int(rd.Format)) + ")");
            v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
            h.ptr += SIZE_T(4 + i) * inc_;
            device_->CreateUnorderedAccessView(r, nullptr, &v, h);
        }
        std::memcpy(mapped_ + uint64_t(slot) * kStride, &d.constants, sizeof(w2::Constants));
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
        gpu_uav.ptr += UINT64(4) * inc_;
        cmd_->SetPipelineState(pso_[mode]);
        cmd_->SetComputeRootDescriptorTable(0, gpu);
        cmd_->SetComputeRootDescriptorTable(1, gpu_uav);
        cmd_->SetComputeRootShaderResourceView(2, upload_->GetGPUVirtualAddress() + uint64_t(slot) * kStride);
        // Spec 6.4: 8x8 groups over Width x Height; the meter one group a tile, Width x Height groups.
        if (mode == w2::kMeter) cmd_->Dispatch(d.constants.width, d.constants.height, 1);
        else if (mode == w2::kPrepMeter) cmd_->Dispatch(1, 1, 1);   // one 16 x 16 group over the whole image
        else cmd_->Dispatch((d.constants.width + 7) / 8, (d.constants.height + 7) / 8, 1);
        // the Vulkan stage's barrier after every dispatch: its writes seen by whatever comes next
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;
        cmd_->ResourceBarrier(1, &b);
        ++dispatches_;
    }
    uint64_t dispatches() const { return dispatches_; }

private:
    // The copy of `r` this dispatch reads (one per resource description, kept; left in NON_PIXEL_SHADER_RESOURCE).
    ID3D12Resource* copy_of(ID3D12Resource* r) {
        D3D12_RESOURCE_DESC rd = r->GetDesc();
        rd.Flags &= ~(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
        for (auto& c : copies_) {
            const D3D12_RESOURCE_DESC cd = c->GetDesc();
            if (cd.Format == rd.Format && cd.Width == rd.Width && cd.Height == rd.Height && cd.MipLevels == rd.MipLevels &&
                cd.DepthOrArraySize == rd.DepthOrArraySize && cd.SampleDesc.Count == rd.SampleDesc.Count && cd.Flags == rd.Flags)
                return copy_into(r, c);
        }
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ID3D12Resource* c = nullptr;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&c))) || !c)
            throw std::runtime_error("wilsjo2 stage (D3D12): the copy of an image read and written at once could not be made");
        copies_.push_back(c);
        copy_dest_.insert(c);
        return copy_into(r, c);
    }
    ID3D12Resource* copy_into(ID3D12Resource* r, ID3D12Resource* c) {
        ensure_(r, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = c;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (!copy_dest_.count(c)) {
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            cmd_->ResourceBarrier(1, &b);
        }
        cmd_->CopyResource(c, r);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cmd_->ResourceBarrier(1, &b);
        copy_dest_.erase(c);
        return c;
    }
    bool is_copy(ID3D12Resource* r) const { return std::find(copies_.begin(), copies_.end(), r) != copies_.end(); }
    std::vector<ID3D12Resource*> copies_;
    std::set<ID3D12Resource*> copy_dest_;   // made, not yet copied into (in COPY_DEST)
    static constexpr uint32_t kModes = 128;
    ID3D12Device* device_ = nullptr;
    ID3D12RootSignature* rs_ = nullptr;
    ID3D12PipelineState* pso_[kModes]{};
    ID3D12DescriptorHeap* heap_ = nullptr;
    ID3D12Resource* upload_ = nullptr;
    uint8_t* mapped_ = nullptr;
    UINT inc_ = 0, slot_ = 0;
    uint64_t dispatches_ = 0;
    ID3D12GraphicsCommandList* cmd_ = nullptr;
    Ensure ensure_;
};

}  // namespace nr::asi::native
