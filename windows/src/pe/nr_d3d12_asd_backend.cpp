// The native D3D12 ASD backend's frame path (nr_d3d12_asd_backend.hpp): HLSL copy-in, the network, HLSL writeback, on
// the game's command list. Everything that can fail is checked before the first command is recorded, so a false
// return leaves the list as it came and the 0.0.8.1 path runs the frame.
#include "nr_d3d12_asd_backend.hpp"
#include "nr_d3d12_asd.hpp"
#include "nr_pe_d3d12split.hpp"
#include "nr_pe_log.hpp"

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nr::pe::asd {
namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// The network until M4's drops in: the output is the input.
class PlaceholderCopyNetwork final : public D3d12AsdNetwork {
public:
    const char* name() const override { return "PLACEHOLDER copy (input -> output; the M4 network is not in yet)"; }
    bool create(ID3D12Device*, const char**) override { return true; }
    bool record(ID3D12GraphicsCommandList* cmd, const LinearFrame& in, const LinearFrame& out, const char** why) override {
        if (in.bytes != out.bytes) { *why = "the linear buffers differ in size"; return false; }
        D3D12_RESOURCE_BARRIER b[2]{};
        for (auto& x : b) {
            x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            x.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        b[0].Transition.pResource = in.buffer;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[1].Transition.pResource = out.buffer;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cmd->ResourceBarrier(2, b);
        cmd->CopyBufferRegion(out.buffer, 0, in.buffer, 0, in.bytes);
        for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        cmd->ResourceBarrier(2, b);
        return true;
    }
};

using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                        UINT, UINT, ID3DBlob**, ID3DBlob**);
const char kShaders[] =
    "cbuffer C : register(b0) { uint g_w; uint g_h; uint g_tint; uint g_pad; };\n"
    "Texture2D<float4> src_tex : register(t0);\n"
    "RWTexture2D<float4> dst_tex : register(u0);\n"
    "StructuredBuffer<float4> lin_src : register(t1);\n"
    "RWStructuredBuffer<float4> lin_dst : register(u1);\n"
    "[numthreads(8, 8, 1)] void to_linear(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x < g_w && id.y < g_h) lin_dst[id.y * g_w + id.x] = src_tex.Load(int3(id.xy, 0));\n"
    "}\n"
    "[numthreads(8, 8, 1)] void from_linear(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= g_w || id.y >= g_h) return;\n"
    "    float4 v = lin_src[id.y * g_w + id.x];\n"
    "    if (g_tint) v.g *= 0.75;\n"   // DebugTint: a magenta cast, to see that this path wrote the frame
    "    dst_tex[id.xy] = v;\n"
    "}\n";

ID3DBlob* compile(const char* entry, std::string* why) {
    static PFN_D3DCompile fn = [] {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        return m ? reinterpret_cast<PFN_D3DCompile>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile"))) : nullptr;
    }();
    if (!fn) { *why = "d3dcompiler_47.dll (D3DCompile) is not available"; return nullptr; }
    ID3DBlob *code = nullptr, *errors = nullptr;
    const HRESULT hr = fn(kShaders, sizeof kShaders - 1, "nr_d3d12_asd", nullptr, nullptr, entry, "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        *why = std::string("the ") + entry + " shader did not compile";
        if (errors) { *why += ": "; *why += static_cast<const char*>(errors->GetBufferPointer()); }
        release(errors);
        release(code);
        return nullptr;
    }
    release(errors);
    return code;
}

// A view format of the resource's: typeless -> its UNORM / FLOAT member; an sRGB format (no typed UAV) -> unknown.
DXGI_FORMAT view_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_UNKNOWN;
    default: return f;
    }
}

struct Backend {
    ID3D12Device* device = nullptr;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState *to_linear = nullptr, *from_linear = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    UINT increment = 0, slot = 0;
    static constexpr UINT kSlots = 1024;   // two a frame: 512 evaluates before one is reused
    ID3D12Resource *lin_in = nullptr, *lin_out = nullptr;
    uint32_t w = 0, h = 0;
    std::unique_ptr<D3d12AsdNetwork> network;
    bool broken = false;   // something of the device's could not be made: the 0.0.8.1 path from now on
    std::string broken_why;
    uint64_t tick = 0;
    std::vector<std::pair<uint64_t, IUnknown*>> retired;   // released 64 evaluates later (the GPU may still use them)
};
Backend g;

void retire(IUnknown* p) { if (p) g.retired.emplace_back(g.tick + 64, p); }
void sweep() {
    ++g.tick;
    auto it = std::remove_if(g.retired.begin(), g.retired.end(), [](const auto& r) {
        if (r.first > g.tick) return false;
        r.second->Release();
        return true;
    });
    g.retired.erase(it, g.retired.end());
}

bool build(ID3D12Device* device, std::string* why) {
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    D3D12_ROOT_PARAMETER p[5]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 4;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable.NumDescriptorRanges = 1;
    p[2].DescriptorTable.pDescriptorRanges = &ranges[1];
    p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[3].Descriptor.ShaderRegister = 1;
    p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[4].Descriptor.ShaderRegister = 1;
    for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 5;
    rd.pParameters = p;
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    release(errors);
    if (FAILED(hr) || !blob) { *why = "D3D12SerializeRootSignature failed"; return false; }
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rs));
    release(blob);
    if (FAILED(hr)) { *why = "CreateRootSignature failed"; return false; }
    for (auto [entry, pso] : {std::pair{"to_linear", &g.to_linear}, std::pair{"from_linear", &g.from_linear}}) {
        ID3DBlob* code = compile(entry, why);
        if (!code) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g.rs;
        pd.CS.pShaderBytecode = code->GetBufferPointer();
        pd.CS.BytecodeLength = code->GetBufferSize();
        hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(pso));
        release(code);
        if (FAILED(hr)) { *why = std::string("CreateComputePipelineState(") + entry + ") failed"; return false; }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = Backend::kSlots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap)))) { *why = "CreateDescriptorHeap failed"; return false; }
    g.increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g.network = std::make_unique<PlaceholderCopyNetwork>();
    const char* nwhy = "";
    if (!g.network->create(device, &nwhy)) { *why = std::string("the network: ") + nwhy; return false; }
    note_network(g.network->name());
    return true;
}

void drop_device() {
    retire(g.rs); retire(g.to_linear); retire(g.from_linear); retire(g.heap); retire(g.lin_in); retire(g.lin_out);
    g.rs = nullptr; g.to_linear = g.from_linear = nullptr; g.heap = nullptr; g.lin_in = g.lin_out = nullptr;
    g.w = g.h = 0;
    g.network.reset();
    if (g.device) g.device->Release();
    g.device = nullptr;
    g.broken = false;
}

ID3D12Resource* linear_buffer(ID3D12Device* device, uint64_t bytes) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

bool typed_uav(ID3D12Device* device, DXGI_FORMAT f) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = f;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs))) return false;
    return (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
}

void transition(D3D12_RESOURCE_BARRIER& b, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    b = D3D12_RESOURCE_BARRIER{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
}

bool fallback(const std::string& why) {
    note_backend(("not run (the 0.0.8.1 path): " + why).c_str(), false);
    return false;
}

}  // namespace

bool evaluate_backend(const nr::dlssnr::D3d12BackendFrame& f) {
    if (!f.device || !f.cmd || !f.color || !f.output) return false;
    const auto color_state = static_cast<D3D12_RESOURCE_STATES>(f.color_state);
    const auto output_state = static_cast<D3D12_RESOURCE_STATES>(f.output_state);
    sweep();
    std::string why;
    if (!nr::pe::split::prepare(f.cmd, &why)) return fallback("the command list's state is not known (" + why + ")");
    on_device(f.device);   // the canary, once
    const Canary c = canary();
    if (c != Canary::kHit) return fallback(std::string("canary ") + canary_name(c));

    if (g.device != f.device) {
        drop_device();
        f.device->AddRef();
        g.device = f.device;
        if (!build(f.device, &why)) { g.broken = true; g.broken_why = why; }
    }
    if (g.broken) return fallback(g.broken_why);

    const D3D12_RESOURCE_DESC sd = f.color->GetDesc(), dd = f.output->GetDesc();
    if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        sd.DepthOrArraySize != 1 || dd.DepthOrArraySize != 1 || sd.SampleDesc.Count != 1 || dd.SampleDesc.Count != 1)
        return fallback("colour or output is not a single-sampled 2D texture");
    const DXGI_FORMAT sf = view_format(sd.Format), df = view_format(dd.Format);
    if (sf == DXGI_FORMAT_UNKNOWN || df == DXGI_FORMAT_UNKNOWN) return fallback("an sRGB colour or output format");
    if (!(dd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || !typed_uav(f.device, df))
        return fallback("the output's format has no typed UAV store");
    const uint32_t w = std::min<uint64_t>({f.width, sd.Width, dd.Width}), h = std::min<uint32_t>({f.height, sd.Height, dd.Height});
    if (!w || !h) return fallback("an empty extent");
    if (w != g.w || h != g.h) {
        retire(g.lin_in); retire(g.lin_out);
        const uint64_t bytes = uint64_t(w) * h * 16;
        g.lin_in = linear_buffer(f.device, bytes);
        g.lin_out = linear_buffer(f.device, bytes);
        if (!g.lin_in || !g.lin_out) {
            retire(g.lin_in); retire(g.lin_out);
            g.lin_in = g.lin_out = nullptr;
            g.w = g.h = 0;
            return fallback("the linear buffers could not be made");
        }
        g.w = w; g.h = h;
    }
    const auto state = nr::pe::split::snapshot(f.cmd);
    if (!state) return fallback("no snapshot of the command list's state");

    // The descriptors: the colour's SRV, the output's UAV.
    const UINT slot = g.slot;
    g.slot = (g.slot + 2) % Backend::kSlots;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(slot) * g.increment;
    gpu.ptr += UINT64(slot) * g.increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = sf;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    f.device->CreateShaderResourceView(f.color, &srv, cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = df;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav = cpu;
    cpu_uav.ptr += g.increment;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
    gpu_uav.ptr += g.increment;
    f.device->CreateUnorderedAccessView(f.output, nullptr, &uav, cpu_uav);

    const bool in_place = f.color == f.output;
    const LinearFrame in{g.lin_in, g.lin_in->GetGPUVirtualAddress(), w, h, uint64_t(w) * h * 16};
    const LinearFrame out{g.lin_out, g.lin_out->GetGPUVirtualAddress(), w, h, in.bytes};
    const UINT constants[4] = {w, h, debug_tint() ? 1u : 0u, 0u};
    const UINT gx = (w + 7) / 8, gy = (h + 7) / 8;
    auto* cmd = f.cmd;
    D3D12_RESOURCE_BARRIER b[3];

    // copy-in: colour -> lin_in
    int n = 0;
    transition(b[n++], g.lin_in, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(b[n++], g.lin_out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (color_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        transition(b[n++], f.color, color_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(n, b);
    ID3D12DescriptorHeap* heaps[] = {g.heap};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(g.rs);
    cmd->SetPipelineState(g.to_linear);
    cmd->SetComputeRoot32BitConstants(0, 4, constants, 0);
    cmd->SetComputeRootDescriptorTable(1, gpu);
    cmd->SetComputeRootDescriptorTable(2, gpu_uav);
    cmd->SetComputeRootUnorderedAccessView(4, in.va);
    cmd->Dispatch(gx, gy, 1);
    b[0] = D3D12_RESOURCE_BARRIER{};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b[0].UAV.pResource = g.lin_in;
    cmd->ResourceBarrier(1, b);

    // the network (both in UNORDERED_ACCESS, and left so)
    const char* nwhy = "";
    const bool net_ok = g.network->record(cmd, in, out, &nwhy);

    // writeback: lin_out -> output (the input copied straight back when the network recorded nothing)
    n = 0;
    transition(b[n++], g.lin_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (in_place) transition(b[n++], f.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, output_state);
    else if (color_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        transition(b[n++], f.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, color_state);
    if (!net_ok) transition(b[n++], g.lin_in, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(n, b);
    cmd->SetPipelineState(g.from_linear);
    cmd->SetComputeRootShaderResourceView(3, net_ok ? out.va : in.va);
    cmd->Dispatch(gx, gy, 1);
    n = 0;
    b[n] = D3D12_RESOURCE_BARRIER{};
    b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b[n++].UAV.pResource = f.output;
    cmd->ResourceBarrier(n, b);
    n = 0;
    transition(b[n++], g.lin_in, net_ok ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    transition(b[n++], g.lin_out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    cmd->ResourceBarrier(n, b);
    nr::pe::split::restore(cmd, state);

    char line[200];
    std::snprintf(line, sizeof line, "ran %ux%u%s: %s", w, h, in_place ? " in place" : "",
                  net_ok ? g.network->name() : (std::string("the network recorded nothing (") + nwhy + "): input copied back").c_str());
    note_backend(line, true);
    return true;
}

}  // namespace nr::pe::asd
