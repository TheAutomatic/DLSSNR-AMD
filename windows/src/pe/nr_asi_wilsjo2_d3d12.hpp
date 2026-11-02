// The wilsjo2 stage on the native D3D12 route (M5 v2): its programs - the Vulkan stage's own SPIR-V as DXBC
// (windows/build/build_wilsjo2_d3d12.sh) - as D3D12 compute pipelines the driver makes, recorded into the game's list.
// nr_asi_wilsjo2.cpp's route; a header so the offline test (nr_wilsjo2_d3d12_test.cpp) runs the very same class.
#pragma once

#include "nr_gpu_completion.hpp"
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
using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                        UINT, UINT, ID3DBlob**, ID3DBlob**);
inline ID3DBlob* compile_cs_hlsl(const char* source, size_t bytes, const char* entry, std::string* why) {
    static PFN_D3DCompile fn = [] {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        return m ? reinterpret_cast<PFN_D3DCompile>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile"))) : nullptr;
    }();
    if (!fn) {
        if (why) *why = "d3dcompiler_47.dll is missing";
        return nullptr;
    }
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = fn(source, bytes, nullptr, nullptr, nullptr, entry, "cs_5_0", 1u << 15, 0, &blob, &errors);
    if (FAILED(hr) || !blob) {
        if (why) *why = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "D3DCompile failed";
        if (errors) errors->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    return blob;
}

inline constexpr char kHalfCopyShader[] = R"hlsl(
Texture2D<float4> g_t0 : register(t0);
RWTexture2D<float4> g_u0 : register(u4);
ByteAddressBuffer g_cb : register(t6);

float3 fast_hybrid_encode(float3 v) {
    v = max(v, 0.0f);
    float m = max(v.r, max(v.g, v.b));
    if (m <= 1e-6f) return v;
    float e = (m - 0.75f) / 0.25f;
    float y = m <= 0.75f ? m : 0.75f + 0.25f * (e * rsqrt(e * e + 1.0f));
    return v * (y / m);
}

float3 fast_hybrid_decode(float3 y) {
    y = max(y, 0.0f);
    float m = max(y.r, max(y.g, y.b));
    if (m <= 1e-6f) return y;
    float u = min((m - 0.75f) / 0.25f, 0.999999f);
    float x = m <= 0.75f ? m : 0.75f + 0.25f * (u * rsqrt(max(1.0f - u * u, 1e-8f)));
    return y * (x / m);
}

[numthreads(8, 8, 1)]
void half_copy(uint3 id : SV_DispatchThreadID) {
    uint width = g_cb.Load(8);
    uint height = g_cb.Load(12);
    if (id.x >= width || id.y >= height) return;
    float4 v = g_t0.Load(int3(int2(id.xy), 0));
    uint dbg = g_cb.Load(24);
    uint cmp = g_cb.Load(52);
    uint way = max(dbg, cmp);
    if (way == 1u) v.rgb = fast_hybrid_encode(v.rgb);
    else if (way == 2u) v.rgb = fast_hybrid_decode(v.rgb);
    g_u0[id.xy] = v;
}
)hlsl";

#include "nr_fast_shaders.hpp"   // kFastShaders: the Passes (fast) programs, shared with the RenoDX stage on this route

class Stage12 final : public w2::Stage::Backend {
public:
    static constexpr UINT kSlots = 4096;
    static constexpr UINT kStride = 256;   // the Vulkan stage's push-constant block
    static_assert(sizeof(w2::Constants) <= kStride, "the constants fit the block");   // dispatches before a slot (6 descriptors, 256 bytes) is reused
    using Ensure = std::function<void(ID3D12Resource*, D3D12_RESOURCE_STATES)>;

    ~Stage12() override {
        for (auto* p : fast_pso_) if (p) p->Release();
        if (rs_fast_) rs_fast_->Release();
        for (auto* p : pso_) if (p) p->Release();
        for (auto* c : copies_) c->Release();
        for (auto& b : blocks_) {
            if (b->mapped) b->upload->Unmap(0, nullptr);
            if (b->upload) b->upload->Release();
            if (b->heap) b->heap->Release();
        }
        blocks_.clear();
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
        // Half-copy pipeline (mode 9)
        {
            std::string err;
            ID3DBlob* code = compile_cs_hlsl(kHalfCopyShader, sizeof kHalfCopyShader - 1, "half_copy", &err);
            if (code) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
                pd.pRootSignature = rs_;
                pd.CS.pShaderBytecode = code->GetBufferPointer();
                pd.CS.BytecodeLength = code->GetBufferSize();
                if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_[w2::kHalfCopy])))) {
                    pso_[w2::kHalfCopy] = nullptr;
                }
                code->Release();
            }
            if (!pso_[w2::kHalfCopy]) {
                NR_W2D3D12_LOG("[asd] wilsjo2 on D3D12: half_copy HLSL compile failed: %s", err.c_str());
            }
        }
        // Root signature for fast programs
        {
            D3D12_ROOT_PARAMETER rp_fast[3]{};
            rp_fast[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp_fast[0].Constants.ShaderRegister = 0;
            rp_fast[0].Constants.RegisterSpace = 0;
            rp_fast[0].Constants.Num32BitValues = 24;
            rp_fast[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_DESCRIPTOR_RANGE r_srv{};
            r_srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            r_srv.NumDescriptors = 8;
            r_srv.BaseShaderRegister = 0;
            r_srv.RegisterSpace = 0;
            r_srv.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            rp_fast[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp_fast[1].DescriptorTable.NumDescriptorRanges = 1;
            rp_fast[1].DescriptorTable.pDescriptorRanges = &r_srv;
            rp_fast[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_DESCRIPTOR_RANGE r_uav{};
            r_uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            r_uav.NumDescriptors = 8;
            r_uav.BaseShaderRegister = 0;
            r_uav.RegisterSpace = 0;
            r_uav.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            rp_fast[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp_fast[2].DescriptorTable.NumDescriptorRanges = 1;
            rp_fast[2].DescriptorTable.pDescriptorRanges = &r_uav;
            rp_fast[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_STATIC_SAMPLER_DESC ss_fast{};
            ss_fast.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ss_fast.AddressU = ss_fast.AddressV = ss_fast.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            ss_fast.MaxAnisotropy = 1;
            ss_fast.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            ss_fast.MinLOD = 0.0f;
            ss_fast.MaxLOD = D3D12_FLOAT32_MAX;
            ss_fast.ShaderRegister = 0;
            ss_fast.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC rd_fast{};
            rd_fast.NumParameters = 3;
            rd_fast.pParameters = rp_fast;
            rd_fast.NumStaticSamplers = 1;
            rd_fast.pStaticSamplers = &ss_fast;

            blob = nullptr; errors = nullptr;
            hr = D3D12SerializeRootSignature(&rd_fast, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
            if (errors) errors->Release();
            if (SUCCEEDED(hr) && blob) {
                device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs_fast_));
                blob->Release();
            }

            if (rs_fast_) {
                const char* const names[] = {"st_fb_in", "st_prep", "st_box", "st_apply"};
                for (uint32_t i = 0; i < nr::fast::kProgramCount; ++i) {
                    std::string err;
                    ID3DBlob* code = compile_cs_hlsl(kFastShaders, sizeof kFastShaders - 1, names[i], &err);
                    if (code) {
                        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
                        pd.pRootSignature = rs_fast_;
                        pd.CS.pShaderBytecode = code->GetBufferPointer();
                        pd.CS.BytecodeLength = code->GetBufferSize();
                        if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&fast_pso_[i])))) {
                            fast_pso_[i] = nullptr;
                        }
                        code->Release();
                    }
                    if (!fast_pso_[i]) {
                        NR_W2D3D12_LOG("[asd] wilsjo2 on D3D12: %s HLSL compile failed: %s", names[i], err.c_str());
                    }
                }
            }
        }
        std::map<std::string, ID3D12PipelineState*> made;
        // Every pipeline this call made is released on the way out, including a required program failing halfway: the
        // earlier return used to skip the release loop and keep the initial references.
        struct MadeRelease {
            std::map<std::string, ID3D12PipelineState*>* made;
            ~MadeRelease() { for (auto& kv : *made) if (kv.second) kv.second->Release(); }
        } made_release{&made};
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
        inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device_ = device;
        return make_block(why);
    }
    // A recording on `cmd` (its state saved by the caller): our heap and root signature bound; `ensure` puts a resource
    // in a state (the route's tracking).
    void begin(ID3D12GraphicsCommandList* cmd, Ensure ensure) {
        cmd_ = cmd;
        ensure_ = std::move(ensure);
        bound_ = nullptr;   // a recording's first dispatch binds its block's heap
        if (!blocks_.empty()) {
            ID3D12DescriptorHeap* heaps[] = {blocks_.front()->heap};
            cmd_->SetDescriptorHeaps(1, heaps);
        }
        cmd_->SetComputeRootSignature(rs_);
    }
    void end() { cmd_ = nullptr; ensure_ = nullptr; bound_ = nullptr; }
    void end_recording() override { sweep_blocks(); }
    bool has_fast() const override {
        return fast_pso_[0] && fast_pso_[1] && fast_pso_[2] && fast_pso_[3] && pso_[w2::kHalfCopy];
    }
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
        const SlotRef sr = take_slot();
        const UINT slot = sr.index;
        bind_block(sr.block);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = sr.block->heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = sr.block->heap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(slot) * 16 * inc_;
        gpu.ptr += UINT64(slot) * 16 * inc_;
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
        std::memcpy(sr.block->mapped + uint64_t(slot) * kStride, &d.constants, sizeof(w2::Constants));
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
        gpu_uav.ptr += UINT64(4) * inc_;
        cmd_->SetComputeRootSignature(rs_);
        cmd_->SetPipelineState(pso_[mode]);
        cmd_->SetComputeRootDescriptorTable(0, gpu);
        cmd_->SetComputeRootDescriptorTable(1, gpu_uav);
        cmd_->SetComputeRootShaderResourceView(2, sr.block->upload->GetGPUVirtualAddress() + uint64_t(slot) * kStride);
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
    void record_fast(uint32_t program, const w2::Stage::FastImage* images, uint32_t count, const void* push,
                     uint32_t bytes, uint32_t gx, uint32_t gy) override {
        if (!cmd_) throw std::runtime_error("wilsjo2 stage (D3D12): fast recorded outside a recording");
        if (program >= nr::fast::kProgramCount || !fast_pso_[program] || !rs_fast_)
            throw std::runtime_error("wilsjo2 stage (D3D12): fast program " + std::to_string(program) + " not ready");

        auto is_uav = [](uint32_t prog, uint32_t b) -> bool {
            if (prog == nr::fast::kFbIn) return b >= 4;
            if (prog == nr::fast::kPrep) return b >= 4;
            if (prog == nr::fast::kBox) return b >= 4;
            if (prog == nr::fast::kApply) return b == 0 || b == 4 || b == 6 || b == 7;
            return false;
        };

        for (uint32_t i = 0; i < count; ++i) {
            ID3D12Resource* r = resource(images[i].image);
            if (!r) continue;
            if (is_uav(program, images[i].binding)) ensure_(r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            else ensure_(r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        const SlotRef sr = take_slot();
        const UINT slot = sr.index;
        bind_block(sr.block);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = sr.block->heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = sr.block->heap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(slot) * 16 * inc_;
        gpu.ptr += UINT64(slot) * 16 * inc_;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_srv = cpu;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav = cpu;
        cpu_uav.ptr += SIZE_T(8) * inc_;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv = gpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
        gpu_uav.ptr += UINT64(8) * inc_;

        for (uint32_t i = 0; i < count; ++i) {
            uint32_t b = images[i].binding;
            if (b >= 8) continue;
            ID3D12Resource* r = resource(images[i].image);
            if (!r) continue;
            const D3D12_RESOURCE_DESC rd = r->GetDesc();
            if (is_uav(program, b)) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
                v.Format = uav_format(rd.Format);
                if (v.Format == DXGI_FORMAT_UNKNOWN || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                    !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                    throw std::runtime_error("wilsjo2 stage (D3D12): fast image cannot be UAV written (format " + std::to_string(int(rd.Format)) + ")");
                v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                D3D12_CPU_DESCRIPTOR_HANDLE h = cpu_uav;
                h.ptr += SIZE_T(b) * inc_;
                device_->CreateUnorderedAccessView(r, nullptr, &v, h);
            } else {
                D3D12_SHADER_RESOURCE_VIEW_DESC v{};
                v.Format = srv_format(rd.Format);
                if (v.Format == DXGI_FORMAT_UNKNOWN || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.SampleDesc.Count != 1)
                    throw std::runtime_error("wilsjo2 stage (D3D12): fast image cannot be SRV read (format " + std::to_string(int(rd.Format)) + ")");
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                v.Texture2D.MostDetailedMip = images[i].image.mip;
                v.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE h = cpu_srv;
                h.ptr += SIZE_T(b) * inc_;
                device_->CreateShaderResourceView(r, &v, h);
            }
        }

        cmd_->SetComputeRootSignature(rs_fast_);
        cmd_->SetPipelineState(fast_pso_[program]);
        if (push && bytes) {
            cmd_->SetComputeRoot32BitConstants(0, (bytes + 3) / 4, push, 0);
        }
        cmd_->SetComputeRootDescriptorTable(1, gpu_srv);
        cmd_->SetComputeRootDescriptorTable(2, gpu_uav);
        cmd_->Dispatch(gx, gy, 1);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;
        cmd_->ResourceBarrier(1, &b);
        ++dispatches_;
    }

private:
    // One descriptor range and constants block (kSlots of each), with the recording each slot last belonged to. A slot is
    // reused only when that recording finished - the Vulkan stage's `slot()` rule - and a block whose slots are all still
    // in flight is replaced by a new one rather than written over (the dispatches already recorded read the old block).
    struct Block {
        ID3D12DescriptorHeap* heap = nullptr;
        ID3D12Resource* upload = nullptr;
        uint8_t* mapped = nullptr;
        std::vector<nr::gpu::Ticket> slots;
        nr::gpu::Uses uses;
        uint32_t next = 0;
    };
    struct SlotRef {
        Block* block;
        uint32_t index;
    };
public:
    using TicketOf = std::function<nr::gpu::Ticket(ID3D12GraphicsCommandList*)>;
    using DoneOf = std::function<bool(const nr::gpu::Ticket&)>;
    // The product's split: the recording a list is booked into, and its completion answer (strict, the same
    // split::completed the retirement paths use). Without them the stage has no evidence and reuses slots as before.
    void set_tickets(TicketOf t, DoneOf d) { ticket_of_ = std::move(t); done_of_ = std::move(d); }
    const nr::gpu::Uses& uses() const { return uses_; }
private:
    bool make_block(std::string* why) {
        struct Cleanup {
            Block& b;
            bool ok = false;
            ~Cleanup() {
                if (ok) return;
                if (b.mapped && b.upload) b.upload->Unmap(0, nullptr);
                if (b.upload) b.upload->Release();
                if (b.heap) b.heap->Release();
            }
        };
        auto owned = std::make_unique<Block>();
        Cleanup guard{*owned};
        Block* const b_ptr = owned.get();
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kSlots * 16;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&b_ptr->heap)))) { *why = "CreateDescriptorHeap failed"; return false; }
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = uint64_t(kSlots) * kStride; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&b_ptr->upload)))) { *why = "the constants' upload buffer could not be made"; return false; }
        void* m = nullptr;
        if (FAILED(b_ptr->upload->Map(0, nullptr, &m)) || !m) { *why = "the constants' upload buffer could not be mapped"; return false; }
        b_ptr->mapped = static_cast<uint8_t*>(m);
        b_ptr->slots.assign(kSlots, nullptr);
        guard.ok = true;   // the block is handed over with its surfaces intact
        blocks_.insert(blocks_.begin(), std::move(owned));   // [0] is the current one
        return true;
    }
    void bind_block(Block* b) {
        if (bound_ == b) return;
        ID3D12DescriptorHeap* heaps[] = {b->heap};
        cmd_->SetDescriptorHeaps(1, heaps);
        bound_ = b;
    }
    // What a slot's last user was: the split's recording for this list when the product set the hooks, nothing in the
    // offline test (then no ticket is evidence and a slot is reused at once, as before).
    nr::gpu::Ticket ticket_here() const { return ticket_of_ ? ticket_of_(cmd_) : nr::gpu::current(); }
    // No ticket is not proof that anything finished: without one a slot stays taken (the caller that has no split must
    // say so through done_of_, as the offline test does).
    bool done(const nr::gpu::Ticket& t) const {
        if (!t) return done_of_ ? done_of_(t) : false;
        return done_of_ ? done_of_(t) : t->ready();
    }
    // The use sets keep only what has not finished, one entry per ticket (the Vulkan Uses::touch rule, with the strict
    // answer): a game's slot sets otherwise grow by one entry per dispatch for the life of the process.
    void prune(nr::gpu::Uses& u) const {
        std::vector<nr::gpu::Ticket> keep;
        keep.reserve(u.tickets.size());
        for (auto& t : u.tickets) {
            if (done(t)) continue;
            if (std::find(keep.begin(), keep.end(), t) == keep.end()) keep.push_back(t);
        }
        u.tickets.swap(keep);
    }
    SlotRef take_slot() {
        if (blocks_.empty()) throw std::runtime_error("wilsjo2 stage (D3D12): no descriptor block");
        const nr::gpu::Ticket ticket = ticket_here();
        Block* b = blocks_.front().get();
        for (uint32_t n = 0; n < kSlots; ++n) {
            const uint32_t i = (b->next + n) % kSlots;
            if (b->slots[i] && !done(b->slots[i])) continue;
            b->slots[i] = ticket;
            prune(b->uses);
            prune(uses_);
            b->uses.tickets.push_back(ticket);
            uses_.tickets.push_back(ticket);
            b->next = (i + 1) % kSlots;
            return {b, i};
        }
        // Every slot of this block is still in flight: the next dispatch gets a block of its own.
        std::string why;
        if (!make_block(&why)) throw std::runtime_error("wilsjo2 stage (D3D12): " + why);
        b = blocks_.front().get();
        b->slots[0] = ticket;
        prune(b->uses);
        prune(uses_);
        b->uses.tickets.push_back(ticket);
        uses_.tickets.push_back(ticket);
        b->next = 1 % kSlots;
        return {b, 0};
    }
    // Blocks a later dispatch replaced are freed once nothing of theirs is in flight.
    void sweep_blocks() {
        for (size_t i = 1; i < blocks_.size();) {
            bool busy = false;
            for (const auto& t : blocks_[i]->uses.tickets)
                if (!done(t)) { busy = true; break; }
            if (busy) { ++i; continue; }
            Block& blk = *blocks_[i];
            if (blk.mapped) blk.upload->Unmap(0, nullptr);
            blk.upload->Release();
            blk.heap->Release();
            blocks_.erase(blocks_.begin() + long(i));
        }
    }
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
    ID3D12RootSignature* rs_fast_ = nullptr;
    ID3D12PipelineState* fast_pso_[nr::fast::kProgramCount]{};
    std::vector<std::unique_ptr<Block>> blocks_;   // [0] the block the next dispatch takes its slot from
    UINT inc_ = 0;
    Block* bound_ = nullptr;   // the heap this recording last bound
    TicketOf ticket_of_;
    DoneOf done_of_;
    nr::gpu::Uses uses_;   // every recording this stage recorded into (its real use set)
    uint64_t dispatches_ = 0;
    ID3D12GraphicsCommandList* cmd_ = nullptr;
    Ensure ensure_;
};

}  // namespace nr::asi::native
