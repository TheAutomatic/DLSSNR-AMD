// Automated R33 regression gate for the zero-SetDescriptorHeaps D3D12 Direct route:
//   Case A: Pre-bound [ResourceDescriptorHeap, SamplerDescriptorHeap] + Graphics/Compute host state,
//           vtable probe asserts SetDescriptorHeaps == 0, Graphics == 0, ClearUAV == 0 across
//           Plain, RenoDX SDR+Preprocess+Look+Fast, RenoDX HDR+Preprocess+Autoscale, wilsjo2_convert,
//           hudfix_d3d12, wilsjo2_inspect, and copy_depth_r32, followed by GPU Dispatch of restored Compute state.
//   Case B: Mid-list SetDescriptorHeaps invalidates stale kind==1 Compute Descriptor Table without replaying it,
//           while Compute RootSignature, Root UAV, Root Constants, and PSO are restored cleanly.
//   Case C: Zero d3dcompiler_47.dll load + RDNA3 NRD12OUT archive verification and secondary RDNA3 GPU dispatch.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "nr_asi_wilsjo2.hpp"
#include "nr_d3d12_direct_state.hpp"
#include "nr_d3d12_net_plan.hpp"
#include "nr_d3d12_outer_elf.hpp"
#include "nr_d3d12_passthrough.hpp"
#include "nr_d3d12_passthrough_backend.hpp"
#include "nr_d3d12_passthrough_net.hpp"
#include "nr_dlssnr_model.hpp"
#include "nr_pe_config.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_session.hpp"
#include "nr_renodx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int query_model() { return nr::kSilverAardwoldOptimizedUltra; }

ID3D12Resource* make_tex2d(ID3D12Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool uav,
                           D3D12_RESOURCE_STATES init_state) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* res = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr, IID_PPV_ARGS(&res))))
        return nullptr;
    return res;
}

ID3D12Resource* make_buf(ID3D12Device* dev, D3D12_HEAP_TYPE type, uint64_t bytes, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{type};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* res = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&res))))
        return nullptr;
    return res;
}

void barrier_transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                        D3D12_RESOURCE_STATES to) {
    if (!r || from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmd->ResourceBarrier(1, &b);
}

// Vtable probe state installed over a copied ID3D12GraphicsCommandList vtable.
void** g_orig_vt = nullptr;
void* g_copied_vt[86]{};
int g_probe_set_heaps = 0;
int g_probe_gfx_rs = 0;
int g_probe_gfx_table = 0;
int g_probe_gfx_other = 0;
int g_probe_clear_uav = 0;
int g_probe_comp_table = 0;
ID3D12RootSignature* g_last_comp_rs = nullptr;
ID3D12PipelineState* g_last_pso = nullptr;
D3D12_GPU_VIRTUAL_ADDRESS g_last_comp_uav[8]{};
uint32_t g_last_comp_const[8]{};

void reset_probes() {
    g_probe_set_heaps = 0;
    g_probe_gfx_rs = 0;
    g_probe_gfx_table = 0;
    g_probe_gfx_other = 0;
    g_probe_clear_uav = 0;
    g_probe_comp_table = 0;
    g_last_comp_rs = nullptr;
    g_last_pso = nullptr;
    std::memset(g_last_comp_uav, 0, sizeof g_last_comp_uav);
    std::memset(g_last_comp_const, 0, sizeof g_last_comp_const);
}

using PFN_SetPso = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using PFN_SetHeaps = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using PFN_SetRS = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using PFN_SetTable = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using PFN_SetConst = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using PFN_SetConsts = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
using PFN_SetView = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using PFN_ClearUint = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_GPU_DESCRIPTOR_HANDLE,
                                               D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const UINT[4], UINT,
                                               const D3D12_RECT*);
using PFN_ClearFloat = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_GPU_DESCRIPTOR_HANDLE,
                                                D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const FLOAT[4], UINT,
                                                const D3D12_RECT*);

void STDMETHODCALLTYPE probe_set_pso(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso) {
    g_last_pso = pso;
    reinterpret_cast<PFN_SetPso>(g_orig_vt[25])(cl, pso);
}

void STDMETHODCALLTYPE probe_set_heaps(ID3D12GraphicsCommandList* cl, UINT n, ID3D12DescriptorHeap* const* heaps) {
    ++g_probe_set_heaps;
    if (n == 0 && !heaps) return;   // self-check pulse does not mutate host heap state
    reinterpret_cast<PFN_SetHeaps>(g_orig_vt[28])(cl, n, heaps);
}

void STDMETHODCALLTYPE probe_set_comp_rs(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* rs) {
    g_last_comp_rs = rs;
    reinterpret_cast<PFN_SetRS>(g_orig_vt[29])(cl, rs);
}

void STDMETHODCALLTYPE probe_set_gfx_rs(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* rs) {
    ++g_probe_gfx_rs;
    reinterpret_cast<PFN_SetRS>(g_orig_vt[30])(cl, rs);
}

void STDMETHODCALLTYPE probe_set_comp_table(ID3D12GraphicsCommandList* cl, UINT idx, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    ++g_probe_comp_table;
    reinterpret_cast<PFN_SetTable>(g_orig_vt[31])(cl, idx, h);
}

void STDMETHODCALLTYPE probe_set_gfx_table(ID3D12GraphicsCommandList* cl, UINT idx, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    ++g_probe_gfx_table;
    reinterpret_cast<PFN_SetTable>(g_orig_vt[32])(cl, idx, h);
}

void STDMETHODCALLTYPE probe_set_comp_const(ID3D12GraphicsCommandList* cl, UINT idx, UINT data, UINT off) {
    if (idx < 8u && off == 0u) g_last_comp_const[idx] = data;
    reinterpret_cast<PFN_SetConst>(g_orig_vt[33])(cl, idx, data, off);
}

void STDMETHODCALLTYPE probe_set_gfx_const(ID3D12GraphicsCommandList* cl, UINT idx, UINT data, UINT off) {
    ++g_probe_gfx_other;
    reinterpret_cast<PFN_SetConst>(g_orig_vt[34])(cl, idx, data, off);
}

void STDMETHODCALLTYPE probe_set_comp_consts(ID3D12GraphicsCommandList* cl, UINT idx, UINT count, const void* data, UINT off) {
    if (idx < 8u && data && off == 0u && count >= 1u) std::memcpy(&g_last_comp_const[idx], data, 4u);
    reinterpret_cast<PFN_SetConsts>(g_orig_vt[35])(cl, idx, count, data, off);
}

void STDMETHODCALLTYPE probe_set_gfx_consts(ID3D12GraphicsCommandList* cl, UINT idx, UINT count, const void* data, UINT off) {
    ++g_probe_gfx_other;
    reinterpret_cast<PFN_SetConsts>(g_orig_vt[36])(cl, idx, count, data, off);
}

void STDMETHODCALLTYPE probe_set_gfx_cbv(ID3D12GraphicsCommandList* cl, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) {
    ++g_probe_gfx_other;
    reinterpret_cast<PFN_SetView>(g_orig_vt[38])(cl, idx, va);
}

void STDMETHODCALLTYPE probe_set_gfx_srv(ID3D12GraphicsCommandList* cl, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) {
    ++g_probe_gfx_other;
    reinterpret_cast<PFN_SetView>(g_orig_vt[40])(cl, idx, va);
}

void STDMETHODCALLTYPE probe_set_comp_uav(ID3D12GraphicsCommandList* cl, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (idx < 8u) g_last_comp_uav[idx] = va;
    reinterpret_cast<PFN_SetView>(g_orig_vt[41])(cl, idx, va);
}

void STDMETHODCALLTYPE probe_set_gfx_uav(ID3D12GraphicsCommandList* cl, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) {
    ++g_probe_gfx_other;
    reinterpret_cast<PFN_SetView>(g_orig_vt[42])(cl, idx, va);
}

void STDMETHODCALLTYPE probe_clear_uint(ID3D12GraphicsCommandList* cl, D3D12_GPU_DESCRIPTOR_HANDLE g,
                                        D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* r, const UINT v[4], UINT n,
                                        const D3D12_RECT* rects) {
    ++g_probe_clear_uav;
    reinterpret_cast<PFN_ClearUint>(g_orig_vt[49])(cl, g, c, r, v, n, rects);
}

void STDMETHODCALLTYPE probe_clear_float(ID3D12GraphicsCommandList* cl, D3D12_GPU_DESCRIPTOR_HANDLE g,
                                         D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* r, const FLOAT v[4], UINT n,
                                         const D3D12_RECT* rects) {
    ++g_probe_clear_uav;
    reinterpret_cast<PFN_ClearFloat>(g_orig_vt[50])(cl, g, c, r, v, n, rects);
}

void attach_probe(ID3D12GraphicsCommandList* cmd) {
    g_orig_vt = *reinterpret_cast<void***>(cmd);
    std::memcpy(g_copied_vt, g_orig_vt, sizeof g_copied_vt);
    g_copied_vt[25] = reinterpret_cast<void*>(&probe_set_pso);
    g_copied_vt[28] = reinterpret_cast<void*>(&probe_set_heaps);
    g_copied_vt[29] = reinterpret_cast<void*>(&probe_set_comp_rs);
    g_copied_vt[30] = reinterpret_cast<void*>(&probe_set_gfx_rs);
    g_copied_vt[31] = reinterpret_cast<void*>(&probe_set_comp_table);
    g_copied_vt[32] = reinterpret_cast<void*>(&probe_set_gfx_table);
    g_copied_vt[33] = reinterpret_cast<void*>(&probe_set_comp_const);
    g_copied_vt[34] = reinterpret_cast<void*>(&probe_set_gfx_const);
    g_copied_vt[35] = reinterpret_cast<void*>(&probe_set_comp_consts);
    g_copied_vt[36] = reinterpret_cast<void*>(&probe_set_gfx_consts);
    g_copied_vt[38] = reinterpret_cast<void*>(&probe_set_gfx_cbv);
    g_copied_vt[40] = reinterpret_cast<void*>(&probe_set_gfx_srv);
    g_copied_vt[41] = reinterpret_cast<void*>(&probe_set_comp_uav);
    g_copied_vt[42] = reinterpret_cast<void*>(&probe_set_gfx_uav);
    g_copied_vt[49] = reinterpret_cast<void*>(&probe_clear_uint);
    g_copied_vt[50] = reinterpret_cast<void*>(&probe_clear_float);
    *reinterpret_cast<void***>(cmd) = g_copied_vt;
}

void detach_probe(ID3D12GraphicsCommandList* cmd) {
    if (g_orig_vt) *reinterpret_cast<void***>(cmd) = g_orig_vt;
}

bool make_table_test_rs(ID3D12Device* dev, ID3D12RootSignature** out_rs) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 10;
    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 1;
    p[0].DescriptorTable.pDescriptorRanges = &range;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[1].Descriptor.ShaderRegister = 10;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[2].Constants.ShaderRegister = 10;
    p[2].Constants.Num32BitValues = 1;
    for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 3;
    rd.pParameters = p;
    ID3DBlob *blob = nullptr, *err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (err) err->Release();
    if (FAILED(hr) || !blob) return false;
    hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(out_rs));
    blob->Release();
    return SUCCEEDED(hr) && *out_rs;
}

bool make_gfx_test_rs(ID3D12Device* dev, ID3D12RootSignature** out_rs) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 1;
    p[0].DescriptorTable.pDescriptorRanges = &range;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.ShaderRegister = 0;
    p[1].Constants.Num32BitValues = 2;
    for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 2;
    rd.pParameters = p;
    ID3DBlob *blob = nullptr, *err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (err) err->Release();
    if (FAILED(hr) || !blob) return false;
    hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(out_rs));
    blob->Release();
    return SUCCEEDED(hr) && *out_rs;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (GetModuleHandleW(L"d3dcompiler_47.dll") != nullptr) {
        std::fprintf(stderr, "FAIL: R33 pre-check found d3dcompiler_47.dll already loaded\n");
        return 1;
    }

    HMODULE self = GetModuleHandleW(nullptr);
    nr::pe::set_module(self);
    nr::pe::configure_network(nr::pe::module_folder());
    nr::pe::d3d12_passthrough::start();
    if (!nr::pe::d3d12_passthrough::enabled()) {
        std::fprintf(stderr, "FAIL: R33 d3d12_passthrough::enabled() is false (check dlssnr-amd.ini)\n");
        return 1;
    }
    nr::pe::d3d12_passthrough::set_net_planner(
        [](uint32_t w, uint32_t h, int model, bool experimental, nr::D3d12NetPlan* out, std::string* why) {
            return nr::plan_d3d12_network(nr::pe::module_folder(), w, h, experimental, out, why, model,
                                          nr::pe::d3d12_passthrough::arch());
        },
        &nr::d3d12_net_controls, &query_model);
    nr::dlssnr::set_d3d12_backend(&nr::pe::d3d12_passthrough::evaluate_backend);

    HMODULE sys_dxgi = LoadLibraryW(L"C:\\Windows\\System32\\dxgi.dll");
    using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto fn_factory = sys_dxgi
        ? reinterpret_cast<PFN_CreateDXGIFactory1>(reinterpret_cast<void*>(GetProcAddress(sys_dxgi, "CreateDXGIFactory1")))
        : nullptr;
    IDXGIFactory4* factory = nullptr;
    if (!fn_factory || FAILED(fn_factory(IID_PPV_ARGS(&factory))) || !factory) {
        std::fprintf(stderr, "FAIL: CreateDXGIFactory1\n");
        return 1;
    }

    IDXGIAdapter1* adapter = nullptr;
    std::vector<IDXGIAdapter1*> secondary_amd;
    {
        SIZE_T best = 0;
        UINT best_idx = ~0u;
        std::vector<IDXGIAdapter1*> all;
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            all.push_back(a);
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (d.VendorId == 0x1002 && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > best) {
                best = d.DedicatedVideoMemory;
                best_idx = i;
            }
        }
        for (UINT i = 0; i < all.size(); ++i) {
            if (i == best_idx) {
                adapter = all[i];
            } else {
                DXGI_ADAPTER_DESC1 d{};
                all[i]->GetDesc1(&d);
                if (d.VendorId == 0x1002 && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) secondary_amd.push_back(all[i]);
                else all[i]->Release();
            }
        }
    }
    if (!adapter) {
        std::fprintf(stderr, "FAIL: No discrete AMD adapter found\n");
        return 1;
    }

    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))) || !device) {
        std::fprintf(stderr, "FAIL: D3D12CreateDevice\n");
        return 1;
    }

    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Fence* fence = nullptr;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        std::fprintf(stderr, "FAIL: D3D12 command queue/list/fence creation\n");
        return 1;
    }
    HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t fence_val = 0;
    auto wait_gpu = [&]() {
        queue->Signal(fence, ++fence_val);
        if (fence->GetCompletedValue() < fence_val) {
            fence->SetEventOnCompletion(fence_val, fence_event);
            WaitForSingleObject(fence_event, 10000);
        }
    };

    std::string why;
    if (!nr::pe::d3d12_direct::install_hooks(device, cmd, &why)) {
        std::fprintf(stderr, "FAIL: d3d12_direct::install_hooks: %s\n", why.c_str());
        return 1;
    }
    cmd->Close();
    alloc->Reset();
    cmd->Reset(alloc, nullptr);
    if (!nr::pe::d3d12_direct::ready(device, cmd, &why)) {
        std::fprintf(stderr, "FAIL: d3d12_direct::ready after Reset: %s\n", why.c_str());
        return 1;
    }

    nr::pe::d3d12_passthrough::on_device(device);
    for (int i = 0; i < 200 && nr::pe::d3d12_passthrough::canary() == nr::pe::d3d12_passthrough::Canary::kRunning; ++i)
        Sleep(20);
    if (nr::pe::d3d12_passthrough::canary() != nr::pe::d3d12_passthrough::Canary::kHit) {
        std::fprintf(stderr, "FAIL: Canary did not hit (%s)\n",
                     nr::pe::d3d12_passthrough::canary_name(nr::pe::d3d12_passthrough::canary()));
        return 1;
    }

    constexpr uint32_t W = 256, H = 256;
    ID3D12Resource* tex_in = make_tex2d(device, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* tex_out = make_tex2d(device, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* tex_f16_in = make_tex2d(device, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* tex_f16_out = make_tex2d(device, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* tex_pq_in = make_tex2d(device, W, H, DXGI_FORMAT_R10G10B10A2_UNORM, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* tex_pq_out = make_tex2d(device, W, H, DXGI_FORMAT_R10G10B10A2_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* motion = make_tex2d(device, W, H, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* depth = make_tex2d(device, W, H, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* r32_dst = make_tex2d(device, W, H, DXGI_FORMAT_R32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* exp_1x1 = make_tex2d(device, 1, 1, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* hud_back = make_tex2d(device, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* hud_less = make_tex2d(device, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* hud_frame = make_tex2d(device, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* hud_stats = make_tex2d(device, 8, 1, DXGI_FORMAT_R32_UINT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Host verification resources for the restored Compute dispatch (64x64 R32_FLOAT copy).
    constexpr uint32_t CW = 64, CH = 64;
    ID3D12Resource* host_src = make_tex2d(device, CW, CH, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* host_dst = make_tex2d(device, CW, CH, DXGI_FORMAT_R32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC host_td = host_src->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT host_fp{};
    UINT host_rows = 0;
    UINT64 host_row_bytes = 0, host_total = 0;
    device->GetCopyableFootprints(&host_td, 0, 1, 0, &host_fp, &host_rows, &host_row_bytes, &host_total);
    ID3D12Resource* host_up = make_buf(device, D3D12_HEAP_TYPE_UPLOAD, host_total, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* host_rb = make_buf(device, D3D12_HEAP_TYPE_READBACK, host_total, D3D12_RESOURCE_STATE_COPY_DEST);

    std::vector<float> expected(CW * CH);
    {
        uint8_t* m = nullptr;
        host_up->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (uint32_t y = 0; y < CH; ++y) {
            auto* row = reinterpret_cast<float*>(m + uint64_t(y) * host_fp.Footprint.RowPitch);
            for (uint32_t x = 0; x < CW; ++x) {
                const float v = 0.25f + float((x + y * 3u) & 63u) * 0.005f;
                row[x] = v;
                expected[size_t(y) * CW + x] = v;
            }
        }
        host_up->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = host_src;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = host_up;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = host_fp;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier_transition(cmd, host_src, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmd->Close();
        ID3D12CommandList* ls[] = {cmd};
        queue->ExecuteCommandLists(1, ls);
        wait_gpu();
        alloc->Reset();
        cmd->Reset(alloc, nullptr);
    }

    auto fill_plain_frame = [&](int* waiting) {
        nr::dlssnr::D3d12BackendFrame f{};
        f.device = device;
        f.cmd = cmd;
        f.color = tex_in;
        f.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        f.output = tex_out;
        f.output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        f.width = W;
        f.height = H;
        f.intensity = 1.0f;
        f.local_tone = 1.0f;
        f.local_structure = 1.0f;
        f.skin_structure = -1.0f;
        f.history_f16 = 1;
        f.motion = motion;
        f.motion_w = W;
        f.motion_h = H;
        f.motion_scale_x = 1.0f;
        f.motion_scale_y = 1.0f;
        f.depth = depth;
        f.depth_w = W;
        f.depth_h = H;
        f.depth_inverted = 1;
        f.feature = 1;
        f.history_strength = -1.0f;
        f.waiting = waiting;
        return f;
    };

    // Warm up the 256x256 ACO network build before installing the strict vtable probe.
    bool net_warm = false;
    for (int poll = 0; poll < 600; ++poll) {
        alloc->Reset();
        cmd->Reset(alloc, nullptr);
        int waiting = 0;
        const bool ok = nr::pe::d3d12_passthrough::evaluate_backend(fill_plain_frame(&waiting));
        cmd->Close();
        ID3D12CommandList* ls[] = {cmd};
        queue->ExecuteCommandLists(1, ls);
        wait_gpu();
        if (ok) { net_warm = true; break; }
        Sleep(50);
    }
    if (!net_warm) {
        std::fprintf(stderr, "FAIL: Network warmup at 256x256 failed/timed out\n");
        return 1;
    }

    // Dual host shader-visible heaps: Resource + Sampler.
    ID3D12DescriptorHeap* host_heaps[2]{};
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 16;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&host_heaps[0])))) {
            std::fprintf(stderr, "FAIL: CreateDescriptorHeap(res)\n");
            return 1;
        }
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&host_heaps[1])))) {
            std::fprintf(stderr, "FAIL: CreateDescriptorHeap(sampler)\n");
            return 1;
        }
    }

    ID3D12RootSignature* rs_gfx = nullptr;
    ID3D12RootSignature* rs_host_comp = nullptr;
    ID3D12RootSignature* rs_table = nullptr;
    if (!make_gfx_test_rs(device, &rs_gfx) ||
        !nr::pe::d3d12_outer::create_outer_root_signature(device, &rs_host_comp, &why) ||
        !make_table_test_rs(device, &rs_table)) {
        std::fprintf(stderr, "FAIL: RootSignature creation (%s)\n", why.c_str());
        return 1;
    }

    auto arch = nr::pe::d3d12_outer::shared_archive(&why);
    nr::pe::d3d12_outer::OuterKernel k_host{};
    nr::pe::d3d12_outer::OuterHeapPool host_pool;
    if (!arch || !arch->build_kernel(device, "backend/copy_depth_r32", &k_host, &why) ||
        !host_pool.init(
            device, 32,
            [](const nr::gpu::Ticket& t) { return nr::pe::d3d12_direct::completed(t); },
            nullptr, &why)) {
        std::fprintf(stderr, "FAIL: Host copy_depth_r32 kernel setup: %s\n", why.c_str());
        return 1;
    }

    // ---- Case A: Dual heaps + Graphics + Compute state preserved across all Direct paths + GPU execution ----
    alloc->Reset();
    cmd->Reset(alloc, nullptr);
    const nr::gpu::Ticket host_ticket = nr::pe::d3d12_direct::book(cmd);
    const nr::pe::d3d12_outer::OuterSlot host_slot = host_pool.acquire(host_ticket, &why);
    if (!host_slot.cpu) {
        std::fprintf(stderr, "FAIL: host_pool.acquire: %s\n", why.c_str());
        return 1;
    }
    const UINT host_push[2] = {CW, CH};
    host_slot.write_srv_tex(device, k_host.find_binding(0), host_src, DXGI_FORMAT_R32_FLOAT, 0, 1);
    host_slot.write_uav_tex(device, k_host.find_binding(2), host_dst, DXGI_FORMAT_R32_FLOAT, 0);
    host_slot.write_push(host_push, sizeof host_push);

    // Pre-bind host [ResourceDescriptorHeap, SamplerDescriptorHeap], Graphics state, and Compute state.
    cmd->SetDescriptorHeaps(2, host_heaps);
    const UINT gfx_consts[2] = {0x11112222u, 0x33334444u};
    cmd->SetGraphicsRootSignature(rs_gfx);
    cmd->SetGraphicsRootDescriptorTable(0, host_heaps[0]->GetGPUDescriptorHandleForHeapStart());
    cmd->SetGraphicsRoot32BitConstants(1, 2, gfx_consts, 0);

    cmd->SetComputeRootSignature(rs_host_comp);
    cmd->SetPipelineState(k_host.pso);
    cmd->SetComputeRoot32BitConstant(0, 0x13572468u, 0);
    cmd->SetComputeRootUnorderedAccessView(1, host_slot.gpu_va);

    attach_probe(cmd);
    reinterpret_cast<PFN_SetHeaps>(g_copied_vt[28])(cmd, 0, nullptr);
    const int self_check = g_probe_set_heaps;
    reset_probes();
    if (self_check != 1) {
        std::fprintf(stderr, "FAIL: Probe self-check failed (%d)\n", self_check);
        detach_probe(cmd);
        return 1;
    }

    int waiting = 0;
    const bool ok_plain = nr::pe::d3d12_passthrough::evaluate_backend(fill_plain_frame(&waiting));

    nr::renodx::Settings s_sdr{};
    s_sdr.nr_enabled = true;
    s_sdr.passes = 1;
    s_sdr.passes_mode = 1;
    s_sdr.fast_method = 1;
    s_sdr.fast_passes[0] = 3.0f;
    s_sdr.fast_fine[0] = 1.0f;
    s_sdr.look_mode = true;
    s_sdr.look_detail = 1.25f;
    s_sdr.look_stabilize = 2;
    nr::dlssnr::D3d12BackendFrame f_sdr = fill_plain_frame(&waiting);
    f_sdr.feature = 2;
    f_sdr.renodx_route = 1;
    f_sdr.renodx_settings = &s_sdr;
    f_sdr.renodx_present = 1;
    f_sdr.renodx_white_nits = 203.0f;
    f_sdr.renodx_fast_motion = 1;
    f_sdr.renodx_after_upscale = 1;
    f_sdr.preprocess = 1;
    f_sdr.prep_exposure = 1;
    f_sdr.prep_curve = 1;
    f_sdr.prep_contrast = 1.05f;
    f_sdr.prep_saturation = 1.0f;
    f_sdr.prep_inverse = 1;
    const bool ok_sdr = nr::pe::d3d12_passthrough::evaluate_backend(f_sdr);

    nr::renodx::Settings s_hdr{};
    s_hdr.nr_enabled = true;
    s_hdr.passes = 1;
    s_hdr.passes_mode = 0;
    s_hdr.feed_mode = 0;
    nr::dlssnr::D3d12BackendFrame f_hdr = fill_plain_frame(&waiting);
    f_hdr.color = tex_f16_in;
    f_hdr.output = tex_f16_out;
    f_hdr.feature = 3;
    f_hdr.renodx_route = 1;
    f_hdr.renodx_settings = &s_hdr;
    f_hdr.renodx_present = 2;
    f_hdr.renodx_hdr_flag = 1;
    f_hdr.renodx_white_nits = 203.0f;
    f_hdr.exposure = exp_1x1;
    f_hdr.exposure_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    f_hdr.pre_exposure = 1.0f;
    f_hdr.exposure_scale = 1.0f;
    f_hdr.create_flags_known = 1;
    f_hdr.auto_exposure = 1;
    f_hdr.preprocess = 1;
    f_hdr.prep_exposure = 1;
    f_hdr.prep_curve = 1;
    f_hdr.prep_contrast = 1.05f;
    f_hdr.prep_saturation = 1.0f;
    f_hdr.prep_inverse = 1;
    const bool ok_hdr = nr::pe::d3d12_passthrough::evaluate_backend(f_hdr);

    const bool ok_conv1 = nr::asi::wilsjo2_convert(cmd, device, tex_pq_in, tex_pq_in, tex_f16_out, W, H, true);
    const bool ok_conv2 = nr::asi::wilsjo2_convert(cmd, device, tex_f16_in, tex_pq_in, tex_pq_out, W, H, false);

    nr::asi::HudFixD3D12 hf{};
    hf.back = hud_back;
    hf.hudless = hud_less;
    hf.frame = hud_frame;
    hf.stats = hud_stats;
    const bool ok_hf = nr::asi::hudfix_d3d12(cmd, device, hf, W, H, 2, false);
    const bool ok_inspect = nr::asi::wilsjo2_inspect(cmd, device, tex_in, tex_f16_in, tex_out, W, H, 1, 0.5f, 1.0f, false, false);
    const bool ok_depth = nr::pe::d3d12_passthrough::copy_depth_r32(device, cmd, depth, r32_dst, &why);

    if (!ok_plain || !ok_sdr || !ok_hdr || !ok_conv1 || !ok_conv2 || !ok_hf || !ok_inspect || !ok_depth) {
        std::fprintf(stderr,
                     "FAIL: Direct workload returned false (plain=%d sdr=%d hdr=%d conv1=%d conv2=%d hf=%d inspect=%d depth=%d why=%s)\n",
                     ok_plain, ok_sdr, ok_hdr, ok_conv1, ok_conv2, ok_hf, ok_inspect, ok_depth, why.c_str());
        detach_probe(cmd);
        return 1;
    }

    if (g_probe_set_heaps != 0 || g_probe_gfx_rs != 0 || g_probe_gfx_table != 0 ||
        g_probe_gfx_other != 0 || g_probe_clear_uav != 0) {
        std::fprintf(stderr,
                     "FAIL: R33 Case A host mutation: SetDescriptorHeaps=%d SetGraphicsRS=%d SetGraphicsTable=%d "
                     "SetGraphicsOther=%d ClearUAV=%d\n",
                     g_probe_set_heaps, g_probe_gfx_rs, g_probe_gfx_table, g_probe_gfx_other, g_probe_clear_uav);
        detach_probe(cmd);
        return 1;
    }
    if (g_last_comp_rs != rs_host_comp || g_last_pso != k_host.pso ||
        g_last_comp_uav[1] != host_slot.gpu_va || g_last_comp_const[0] != 0x13572468u) {
        std::fprintf(stderr,
                     "FAIL: R33 Case A Compute state mismatch: rs=%p (exp %p) pso=%p (exp %p) uav=0x%llx (exp 0x%llx) "
                     "c0=0x%08x\n",
                     static_cast<void*>(g_last_comp_rs), static_cast<void*>(rs_host_comp),
                     static_cast<void*>(g_last_pso), static_cast<void*>(k_host.pso),
                     static_cast<unsigned long long>(g_last_comp_uav[1]),
                     static_cast<unsigned long long>(host_slot.gpu_va), g_last_comp_const[0]);
        detach_probe(cmd);
        return 1;
    }

    // Dispatch using ONLY the restored host Compute state (no rebinding).
    cmd->Dispatch((CW + 7u) / 8u, (CH + 7u) / 8u, 1u);
    detach_probe(cmd);

    barrier_transition(cmd, host_dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = host_rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = host_fp;
        src.pResource = host_dst;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    barrier_transition(cmd, host_dst, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->Close();
    {
        ID3D12CommandList* ls[] = {cmd};
        queue->ExecuteCommandLists(1, ls);
        wait_gpu();
    }
    if (FAILED(device->GetDeviceRemovedReason())) {
        std::fprintf(stderr, "FAIL: Case A device removed (0x%08lx)\n",
                     static_cast<unsigned long>(device->GetDeviceRemovedReason()));
        return 1;
    }
    {
        uint8_t* m = nullptr;
        host_rb->Map(0, nullptr, reinterpret_cast<void**>(&m));
        double max_err = 0.0;
        for (uint32_t y = 0; y < CH; ++y) {
            const auto* row = reinterpret_cast<const float*>(m + uint64_t(y) * host_fp.Footprint.RowPitch);
            for (uint32_t x = 0; x < CW; ++x)
                max_err = std::max(max_err, std::abs(double(row[x]) - double(expected[size_t(y) * CW + x])));
        }
        host_rb->Unmap(0, nullptr);
        if (max_err > 1e-6) {
            std::fprintf(stderr, "FAIL: Case A restored Compute dispatch readback max_err=%g\n", max_err);
            return 1;
        }
    }
    std::printf("[PASS] Case A: SetDescriptorHeaps=0, Graphics=0, ClearUAV=0, Compute state restored and GPU-verified\n");

    // ---- Case B: Read-only SetDescriptorHeaps observer invalidates stale kind==1 Compute Descriptor Table ----
    ID3D12DescriptorHeap* alt_heap = nullptr;
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 8;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&alt_heap)))) {
            std::fprintf(stderr, "FAIL: CreateDescriptorHeap(alt_heap)\n");
            return 1;
        }
    }
    alloc->Reset();
    cmd->Reset(alloc, nullptr);
    const nr::gpu::Ticket caseb_ticket = nr::pe::d3d12_direct::book(cmd);
    const nr::pe::d3d12_outer::OuterSlot caseb_slot = host_pool.acquire(caseb_ticket, &why);
    if (!caseb_slot.cpu) {
        std::fprintf(stderr, "FAIL: Case B host_pool.acquire: %s\n", why.c_str());
        return 1;
    }
    cmd->SetDescriptorHeaps(1, &host_heaps[0]);
    cmd->SetComputeRootSignature(rs_table);
    cmd->SetComputeRootDescriptorTable(0, host_heaps[0]->GetGPUDescriptorHandleForHeapStart());
    cmd->SetComputeRootUnorderedAccessView(1, caseb_slot.gpu_va);
    cmd->SetComputeRoot32BitConstant(2, 0xCAFEBABEu, 0);
    cmd->SetPipelineState(k_host.pso);
    // Switch heap without rebinding param 0 -> read-only observer must invalidate stale table entry (kind == 1).
    cmd->SetDescriptorHeaps(1, &alt_heap);

    attach_probe(cmd);
    reset_probes();
    const bool ok_caseb = nr::pe::d3d12_passthrough::copy_depth_r32(device, cmd, host_src, host_dst, &why);
    detach_probe(cmd);
    cmd->Close();
    {
        ID3D12CommandList* ls[] = {cmd};
        queue->ExecuteCommandLists(1, ls);
        wait_gpu();
    }
    if (!ok_caseb || g_probe_set_heaps != 0 || g_probe_comp_table != 0 ||
        g_last_comp_rs != rs_table || g_last_comp_uav[1] != caseb_slot.gpu_va ||
        g_last_comp_const[2] != 0xCAFEBABEu || g_last_pso != k_host.pso) {
        std::fprintf(stderr,
                     "FAIL: Case B stale-table invalidation: ok=%d heaps=%d comp_table=%d rs=%p (exp %p) "
                     "uav=0x%llx (exp 0x%llx) c2=0x%08x pso=%p (exp %p)\n",
                     ok_caseb, g_probe_set_heaps, g_probe_comp_table,
                     static_cast<void*>(g_last_comp_rs), static_cast<void*>(rs_table),
                     static_cast<unsigned long long>(g_last_comp_uav[1]),
                     static_cast<unsigned long long>(caseb_slot.gpu_va),
                     g_last_comp_const[2], static_cast<void*>(g_last_pso), static_cast<void*>(k_host.pso));
        return 1;
    }
    std::printf("[PASS] Case B: Mid-list SetDescriptorHeaps invalidated Compute table (replayed=%d), RS/UAV/Const/PSO restored\n",
                g_probe_comp_table);

    // ---- Case C: Zero d3dcompiler_47.dll + RDNA3 NRD12OUT archive & secondary iGPU check ----
    if (GetModuleHandleW(L"d3dcompiler_47.dll") != nullptr) {
        std::fprintf(stderr, "FAIL: Case C detected d3dcompiler_47.dll loaded during Direct recording\n");
        return 1;
    }
    auto rdna3_arch = nr::pe::d3d12_outer::shared_archive(true, &why);
    if (!rdna3_arch || !rdna3_arch->find("backend/copy_depth_r32") ||
        !rdna3_arch->find("renodx/v6_resolve_commit") || !rdna3_arch->find("wilsjo2/hudfix_clear") ||
        !rdna3_arch->find("il/il_0")) {
        std::fprintf(stderr, "FAIL: Case C RDNA3 NRD12OUT archive check failed: %s\n", why.c_str());
        return 1;
    }

    int rdna3_hw = 0;
    for (IDXGIAdapter1* a3 : secondary_amd) {
        DXGI_ADAPTER_DESC1 desc3{};
        a3->GetDesc1(&desc3);
        ID3D12Device* dev3 = nullptr;
        if (FAILED(D3D12CreateDevice(a3, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev3))) || !dev3) continue;
        ID3D12RootSignature* rs3 = nullptr;
        nr::pe::d3d12_outer::OuterKernel k3{};
        nr::pe::d3d12_outer::OuterHeapPool pool3;
        std::string why3;
        if (nr::pe::d3d12_outer::create_outer_root_signature(dev3, &rs3, &why3) &&
            rdna3_arch->build_kernel(dev3, "backend/copy_depth_r32", &k3, &why3) &&
            pool3.init(dev3, 16, nullptr, nullptr, &why3)) {
            D3D12_COMMAND_QUEUE_DESC qd3{D3D12_COMMAND_LIST_TYPE_DIRECT};
            ID3D12CommandQueue* q3 = nullptr;
            ID3D12CommandAllocator* ca3 = nullptr;
            ID3D12GraphicsCommandList* cl3 = nullptr;
            ID3D12Fence* f3 = nullptr;
            dev3->CreateCommandQueue(&qd3, IID_PPV_ARGS(&q3));
            dev3->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca3));
            dev3->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca3, nullptr, IID_PPV_ARGS(&cl3));
            dev3->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f3));
            ID3D12Resource* s3 = make_tex2d(dev3, CW, CH, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
            ID3D12Resource* d3 = make_tex2d(dev3, CW, CH, DXGI_FORMAT_R32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12Resource* up3 = make_buf(dev3, D3D12_HEAP_TYPE_UPLOAD, host_total, D3D12_RESOURCE_STATE_GENERIC_READ);
            ID3D12Resource* rb3 = make_buf(dev3, D3D12_HEAP_TYPE_READBACK, host_total, D3D12_RESOURCE_STATE_COPY_DEST);
            if (q3 && ca3 && cl3 && f3 && s3 && d3 && up3 && rb3) {
                uint8_t* m = nullptr;
                up3->Map(0, nullptr, reinterpret_cast<void**>(&m));
                for (uint32_t y = 0; y < CH; ++y) {
                    auto* row = reinterpret_cast<float*>(m + uint64_t(y) * host_fp.Footprint.RowPitch);
                    for (uint32_t x = 0; x < CW; ++x) row[x] = expected[size_t(y) * CW + x];
                }
                up3->Unmap(0, nullptr);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = s3;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.pResource = up3;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = host_fp;
                cl3->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                barrier_transition(cl3, s3, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                auto rec3 = std::make_shared<nr::gpu::Recording>();
                const auto slot3 = pool3.acquire(rec3, &why3);
                if (slot3.cpu) {
                    slot3.write_srv_tex(dev3, k3.find_binding(0), s3, DXGI_FORMAT_R32_FLOAT, 0, 1);
                    slot3.write_uav_tex(dev3, k3.find_binding(2), d3, DXGI_FORMAT_R32_FLOAT, 0);
                    slot3.write_push(host_push, sizeof host_push);
                    slot3.dispatch(cl3, rs3, k3, (CW + 7u) / 8u, (CH + 7u) / 8u, 1u, true);
                    barrier_transition(cl3, d3, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    dst.pResource = rb3;
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint = host_fp;
                    src.pResource = d3;
                    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    cl3->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    cl3->Close();
                    ID3D12CommandList* ls3[] = {cl3};
                    q3->ExecuteCommandLists(1, ls3);
                    HANDLE ev3 = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    q3->Signal(f3, 1);
                    if (f3->GetCompletedValue() < 1) {
                        f3->SetEventOnCompletion(1, ev3);
                        WaitForSingleObject(ev3, 10000);
                    }
                    CloseHandle(ev3);
                    rec3->seal();
                    double err3 = 0.0;
                    rb3->Map(0, nullptr, reinterpret_cast<void**>(&m));
                    for (uint32_t y = 0; y < CH; ++y) {
                        const auto* row = reinterpret_cast<const float*>(m + uint64_t(y) * host_fp.Footprint.RowPitch);
                        for (uint32_t x = 0; x < CW; ++x)
                            err3 = std::max(err3, std::abs(double(row[x]) - double(expected[size_t(y) * CW + x])));
                    }
                    rb3->Unmap(0, nullptr);
                    if (err3 > 1e-6 || FAILED(dev3->GetDeviceRemovedReason())) {
                        std::fprintf(stderr, "FAIL: Case C RDNA3 hardware readback err=%g removed=0x%08lx\n",
                                     err3, static_cast<unsigned long>(dev3->GetDeviceRemovedReason()));
                        return 1;
                    }
                    rdna3_hw = 1;
                    std::printf("[PASS] Case C (RDNA3 secondary adapter %ls): copy_depth_r32 exact match (err=%g)\n",
                                desc3.Description, err3);
                }
            }
            if (rb3) rb3->Release();
            if (up3) up3->Release();
            if (d3) d3->Release();
            if (s3) s3->Release();
            if (f3) f3->Release();
            if (cl3) cl3->Release();
            if (ca3) ca3->Release();
            if (q3) q3->Release();
            pool3.reset();
        } else {
            std::printf("[INFO] Case C: secondary AMD adapter %ls skipped (%s)\n", desc3.Description, why3.c_str());
        }
        if (k3.pso) k3.pso->Release();
        if (rs3) rs3->Release();
        dev3->Release();
        if (rdna3_hw) break;
    }
    std::printf("[PASS] Case C: d3dcompiler_47.dll=nullptr, RDNA3 archive verified (hw_dispatch=%d)\n", rdna3_hw);

    // Clean exit after GPU completion.
    alt_heap->Release();
    host_pool.reset();
    if (k_host.pso) k_host.pso->Release();
    rs_table->Release();
    rs_host_comp->Release();
    rs_gfx->Release();
    host_heaps[0]->Release();
    host_heaps[1]->Release();
    host_rb->Release();
    host_up->Release();
    host_dst->Release();
    host_src->Release();
    hud_stats->Release();
    hud_frame->Release();
    hud_less->Release();
    hud_back->Release();
    exp_1x1->Release();
    r32_dst->Release();
    depth->Release();
    motion->Release();
    tex_pq_out->Release();
    tex_pq_in->Release();
    tex_f16_out->Release();
    tex_f16_in->Release();
    tex_out->Release();
    tex_in->Release();
    CloseHandle(fence_event);
    fence->Release();
    cmd->Release();
    alloc->Release();
    queue->Release();
    device->Release();
    adapter->Release();
    for (IDXGIAdapter1* a : secondary_amd) a->Release();
    factory->Release();
    return 0;
}
