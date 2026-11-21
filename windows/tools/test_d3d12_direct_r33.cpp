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
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "nr_asi_wilsjo2.hpp"
#include "nr_d3d11_d3d12_bridge.hpp"
#include "nr_d3d12_direct_state.hpp"
#include "nr_d3d12_net_plan.hpp"
#include "nr_d3d12_outer_elf.hpp"
#include "nr_d3d12_passthrough.hpp"
#include "nr_d3d12_passthrough_backend.hpp"
#include "nr_d3d12_passthrough_net.hpp"
#include "nr_dlssnr_model.hpp"
#include "nr_gpu_arch.hpp"
#include "nr_hud_method_hook.hpp"
#include "nr_path_win.hpp"
#include "nr_pe_config.hpp"
#include "nr_pe_crash.hpp"
#include "nr_pe_d3d12split.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_session.hpp"
#include "nr_renodx.hpp"
#include "nr_renodx_d3d12.hpp"
#include "nr_swapchain_arbiter.hpp"
#include "ui/nr_ini_edit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

ID3D12Resource* make_buf(ID3D12Device* dev, D3D12_HEAP_TYPE type, uint64_t bytes, D3D12_RESOURCE_STATES state,
                         bool uav = false) {
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
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* res = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&res))))
        return nullptr;
    return res;
}

uint16_t float_to_f16(float x) {
    uint32_t bits = 0;
    std::memcpy(&bits, &x, sizeof bits);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exp = int32_t((bits >> 23) & 0xFFu) - 127;
    uint32_t mant = bits & 0x007FFFFFu;
    if (exp == 128) return uint16_t(sign | 0x7C00u | (mant ? 0x0200u : 0u));
    if (exp > 15) return uint16_t(sign | 0x7C00u);
    if (exp < -14) return uint16_t(sign);
    return uint16_t(sign | uint32_t(exp + 15) << 10 | (mant >> 13));
}

float f16_to_float(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x03FFu;
    uint32_t bits = 0;
    if (exp == 0) bits = mant ? 0u : sign;   // sufficient for normalized/zero test inputs
    else if (exp == 31) bits = sign | 0x7F800000u | (mant << 13);
    else bits = sign | ((exp + 112u) << 23) | (mant << 13);
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof out);
    return out;
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
uint32_t g_last_comp_param2 = 0;
D3D12_GPU_DESCRIPTOR_HANDLE g_last_comp_table{};

void reset_probes() {
    g_probe_set_heaps = 0;
    g_probe_gfx_rs = 0;
    g_probe_gfx_table = 0;
    g_probe_gfx_other = 0;
    g_probe_clear_uav = 0;
    g_probe_comp_table = 0;
    g_last_comp_rs = nullptr;
    g_last_pso = nullptr;
    g_last_comp_table = {};
    g_last_comp_param2 = 0;
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
    if (idx == 0) g_last_comp_table = h;
    reinterpret_cast<PFN_SetTable>(g_orig_vt[31])(cl, idx, h);
}

void STDMETHODCALLTYPE probe_set_gfx_table(ID3D12GraphicsCommandList* cl, UINT idx, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    ++g_probe_gfx_table;
    reinterpret_cast<PFN_SetTable>(g_orig_vt[32])(cl, idx, h);
}

void STDMETHODCALLTYPE probe_set_comp_const(ID3D12GraphicsCommandList* cl, UINT idx, UINT data, UINT off) {
    if (idx == 0u && off < 8u) g_last_comp_const[off] = data;
    if (idx == 2u && off == 0u) g_last_comp_param2 = data;
    reinterpret_cast<PFN_SetConst>(g_orig_vt[33])(cl, idx, data, off);
}

void STDMETHODCALLTYPE probe_set_gfx_const(ID3D12GraphicsCommandList* cl, UINT idx, UINT data, UINT off) {
    ++g_probe_gfx_other;
    reinterpret_cast<PFN_SetConst>(g_orig_vt[34])(cl, idx, data, off);
}

void STDMETHODCALLTYPE probe_set_comp_consts(ID3D12GraphicsCommandList* cl, UINT idx, UINT count, const void* data, UINT off) {
    if (idx == 0u && data) {
        const auto* v = static_cast<const uint32_t*>(data);
        for (UINT k = 0; k < count && off + k < 8u; ++k) g_last_comp_const[off + k] = v[k];
    }
    if (idx == 2u && data && off == 0u && count >= 1u) {
        g_last_comp_param2 = static_cast<const uint32_t*>(data)[0];
    }
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

bool wait_queue_strict(ID3D12CommandQueue* q, ID3D12Fence* fence, uint64_t val, HANDLE ev, const char* label) {
    if (FAILED(q->Signal(fence, val))) {
        std::fprintf(stderr, "FAIL: %s queue->Signal failed\n", label);
        return false;
    }
    if (fence->GetCompletedValue() < val) {
        if (FAILED(fence->SetEventOnCompletion(val, ev))) {
            std::fprintf(stderr, "FAIL: %s fence->SetEventOnCompletion failed\n", label);
            return false;
        }
        if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) {
            std::fprintf(stderr, "FAIL: %s WaitForSingleObject timed out\n", label);
            return false;
        }
    }
    const uint64_t got = fence->GetCompletedValue();
    if (got == UINT64_MAX || got < val) {
        std::fprintf(stderr, "FAIL: %s fence completed=%llu < %llu\n", label,
                     static_cast<unsigned long long>(got), static_cast<unsigned long long>(val));
        return false;
    }
    return true;
}

int run_sub_hook_rollback() {
    HMODULE sys_dxgi = LoadLibraryW(L"C:\\Windows\\System32\\dxgi.dll");
    using PFN_Fac = HRESULT(WINAPI*)(REFIID, void**);
    auto fn_fac = sys_dxgi ? reinterpret_cast<PFN_Fac>(reinterpret_cast<void*>(GetProcAddress(sys_dxgi, "CreateDXGIFactory1"))) : nullptr;
    IDXGIFactory4* fac = nullptr;
    if (!fn_fac || FAILED(fn_fac(IID_PPV_ARGS(&fac))) || !fac) return 1;
    IDXGIAdapter1* ad = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        ad->GetDesc1(&d);
        if (d.VendorId == 0x1002 && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
        ad->Release();
        ad = nullptr;
    }
    if (!ad) { fac->Release(); return 1; }
    IDXGIAdapter1* warp = nullptr;
    if (FAILED(fac->EnumWarpAdapter(IID_PPV_ARGS(&warp))) || !warp) {
        ad->Release();
        fac->Release();
        return 1;
    }
    ID3D12Device *dev1 = nullptr, *dev2 = nullptr;
    if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev1))) || !dev1 ||
        FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev2))) || !dev2 ||
        dev1 == dev2) {
        if (dev1) dev1->Release();
        if (dev2) dev2->Release();
        warp->Release();
        ad->Release();
        fac->Release();
        return 1;
    }
    std::string why;
    _putenv_s("NR_TEST_FAIL_HOOK_SLOT", "ExecuteCommandLists");
    const bool r1 = nr::pe::d3d12_direct::install_hooks(dev1, nullptr, &why);
    if (r1 || nr::pe::d3d12_direct::hooks_installed()) {
        std::fprintf(stderr, "FAIL: hook rollback did not trigger on ExecuteCommandLists failure\n");
        return 1;
    }
    _putenv_s("NR_TEST_FAIL_HOOK_SLOT", "");
    const bool r_same = nr::pe::d3d12_direct::install_hooks(dev1, nullptr, &why);
    if (r_same) {
        std::fprintf(stderr, "FAIL: failed device was retried unexpectedly\n");
        return 1;
    }
    const bool r2 = nr::pe::d3d12_direct::install_hooks(dev2, nullptr, &why);
    if (!r2 || !nr::pe::d3d12_direct::hooks_installed()) {
        std::fprintf(stderr, "FAIL: clean device retry failed after hook rollback: %s\n", why.c_str());
        return 1;
    }
    dev2->Release();
    dev1->Release();
    warp->Release();
    ad->Release();
    fac->Release();
    std::printf("[PASS] Subprocess hook-rollback: failed slot rolled back cleanly, retry on rebuilt device succeeded\n");
    return 0;
}

int run_sub_no_canary_hooks() {
    HMODULE self = GetModuleHandleW(nullptr);
    nr::pe::set_module(self);
    nr::pe::configure_network(nr::pe::module_folder());
    nr::pe::d3d12_passthrough::start();
    if (nr::pe::d3d12_direct::hooks_installed()) {
        std::fprintf(stderr, "FAIL: hooks installed prematurely before canary\n");
        return 1;
    }
    HMODULE sys_dxgi = LoadLibraryW(L"C:\\Windows\\System32\\dxgi.dll");
    using PFN_Fac = HRESULT(WINAPI*)(REFIID, void**);
    auto fn_fac = sys_dxgi ? reinterpret_cast<PFN_Fac>(reinterpret_cast<void*>(GetProcAddress(sys_dxgi, "CreateDXGIFactory1"))) : nullptr;
    IDXGIFactory4* fac = nullptr;
    if (!fn_fac || FAILED(fn_fac(IID_PPV_ARGS(&fac))) || !fac) return 1;
    IDXGIAdapter1* warp = nullptr;
    ID3D12Device* warp_dev = nullptr;
    if (SUCCEEDED(fac->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && warp) {
        D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&warp_dev));
    }
    if (warp_dev) {
        ID3D12CommandAllocator* w_alloc = nullptr;
        ID3D12GraphicsCommandList* w_cl = nullptr;
        ID3D12Resource* w_in = make_tex2d(warp_dev, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, true,
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D12Resource* w_out = make_tex2d(warp_dev, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, true,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (SUCCEEDED(warp_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&w_alloc))) &&
            SUCCEEDED(warp_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, w_alloc, nullptr,
                                                  IID_PPV_ARGS(&w_cl)))) {
            std::string w_why;
            bool w_build = false;
            nr::pe::d3d12_passthrough::wilsjo2_ready(warp_dev, w_cl, 16, 16, &w_why, &w_build);
            if (nr::pe::d3d12_direct::hooks_installed()) {
                std::fprintf(stderr, "FAIL: hooks installed by wilsjo2_ready before canary settled\n");
                return 1;
            }
            for (int i = 0; i < 100 && nr::pe::d3d12_passthrough::canary() == nr::pe::d3d12_passthrough::Canary::kRunning; ++i)
                Sleep(20);
            int w_waiting = 0;
            nr::dlssnr::D3d12BackendFrame w_bf{};
            w_bf.device = warp_dev;
            w_bf.cmd = w_cl;
            w_bf.color = w_in;
            w_bf.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            w_bf.output = w_out;
            w_bf.output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            w_bf.width = 16;
            w_bf.height = 16;
            w_bf.waiting = &w_waiting;
            nr::pe::d3d12_passthrough::evaluate_backend(w_bf);
            nr::pe::d3d12_passthrough::wilsjo2_ready(warp_dev, w_cl, 16, 16, &w_why, &w_build);
            w_cl->Close();
            w_cl->Release();
            w_alloc->Release();
        }
        if (w_out) w_out->Release();
        if (w_in) w_in->Release();
        if (nr::pe::d3d12_direct::hooks_installed()) {
            std::fprintf(stderr, "FAIL: hooks installed on non-RDNA3/4 WARP adapter after failed canary\n");
            return 1;
        }
        warp_dev->Release();
    }
    if (warp) warp->Release();
    fac->Release();
    std::printf("[PASS] Subprocess no-canary-hooks: 0 hooks installed before hit or on non-AMD WARP device\n");
    return 0;
}

static DWORD g_uef_seen_err = 0;
static int g_uef_calls = 0;
static LONG WINAPI test_prev_uef(EXCEPTION_POINTERS*) {
    g_uef_seen_err = GetLastError();
    ++g_uef_calls;
    SetLastError(0x13579BDFu);
    return EXCEPTION_EXECUTE_HANDLER;
}
static DWORD g_veh_seen_err = 0;
static int g_veh_calls = 0;
static LONG CALLBACK test_veh_catcher(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == 0xE0123456u) {
        g_veh_seen_err = GetLastError();
        ++g_veh_calls;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
__attribute__((noinline)) static int STDMETHODCALLTYPE fake_mh_target(void* p) {
    volatile uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return (v != 0 && (v ^ 0x55u) != 0x55u) ? 1 : 0;
}
__attribute__((noinline)) static int STDMETHODCALLTYPE fake_mh_detour(void* p) {
    volatile uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return (v != 0 && (v ^ 0x55u) != 0x55u) ? 2 : 0;
}
using PFN_FakeTarget = int(STDMETHODCALLTYPE*)(void*);
static PFN_FakeTarget g_fake_real = nullptr;
static HRESULT STDMETHODCALLTYPE mock_wrap_reset(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*) { return S_OK; }
static HRESULT STDMETHODCALLTYPE mock_wrap_close(ID3D12GraphicsCommandList*) { return S_OK; }

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::strcmp(argv[1], "--test-hook-rollback") == 0) return run_sub_hook_rollback();
    if (argc > 1 && std::strcmp(argv[1], "--test-no-canary-hooks") == 0) return run_sub_no_canary_hooks();
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
        if (!wait_queue_strict(queue, fence, ++fence_val, fence_event, "primary")) std::exit(1);
    };

    // Create an untracked bundle before installing Direct hooks for Case G invalidation verification.
    ID3D12CommandAllocator* bundle_alloc = nullptr;
    ID3D12GraphicsCommandList* untracked_bundle = nullptr;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_alloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_alloc, nullptr,
                                         IID_PPV_ARGS(&untracked_bundle))) ||
        FAILED(untracked_bundle->Close())) {
        std::fprintf(stderr, "FAIL: untracked bundle creation\n");
        return 1;
    }

    // Create early CommandSignatures before d3d12_direct::install_hooks() for Case L1:
    // one observed via note_command_signature() (simulating hudcap's early CreateCommandSignature hook),
    // one kept untracked as a negative control.
    ID3D12CommandSignature* early_tracked_sig = nullptr;
    ID3D12CommandSignature* early_untracked_sig = nullptr;
    {
        D3D12_INDIRECT_ARGUMENT_DESC arg_desc{};
        arg_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC cs_desc{};
        cs_desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        cs_desc.NumArgumentDescs = 1;
        cs_desc.pArgumentDescs = &arg_desc;
        if (FAILED(device->CreateCommandSignature(&cs_desc, nullptr, IID_PPV_ARGS(&early_tracked_sig))) ||
            FAILED(device->CreateCommandSignature(&cs_desc, nullptr, IID_PPV_ARGS(&early_untracked_sig)))) {
            std::fprintf(stderr, "FAIL: early CreateCommandSignature setup\n");
            return 1;
        }
        nr::pe::d3d12_direct::note_command_signature(early_tracked_sig, &cs_desc);
    }

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
    host_slot.write_push(host_push, sizeof host_push, &k_host);

    // Pre-bind host [ResourceDescriptorHeap, SamplerDescriptorHeap], Graphics state, and Compute state.
    cmd->SetDescriptorHeaps(2, host_heaps);
    const UINT gfx_consts[2] = {0x11112222u, 0x33334444u};
    cmd->SetGraphicsRootSignature(rs_gfx);
    cmd->SetGraphicsRootDescriptorTable(0, host_heaps[0]->GetGPUDescriptorHandleForHeapStart());
    cmd->SetGraphicsRoot32BitConstants(1, 2, gfx_consts, 0);

    cmd->SetComputeRootSignature(rs_host_comp);
    cmd->SetPipelineState(k_host.pso);
    cmd->SetComputeRoot32BitConstants(0, 2, host_push, 0);
    cmd->SetComputeRoot32BitConstant(0, 0x13572468u, 4);
    cmd->SetComputeRootUnorderedAccessView(1, host_slot.gpu_va);

    attach_probe(cmd);
    reinterpret_cast<PFN_SetHeaps>(g_copied_vt[28])(cmd, 0, nullptr);
    const int self_check = g_probe_set_heaps;
    reset_probes();
    nr::pe::d3d12_direct::reset_counters();
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
        g_probe_gfx_other != 0 || g_probe_clear_uav != 0 ||
        nr::pe::d3d12_direct::nr_set_heap_count() != 0 || nr::pe::d3d12_direct::nr_set_table_count() != 0) {
        std::fprintf(stderr,
                     "FAIL: R33 Case A host mutation: SetDescriptorHeaps=%d SetGraphicsRS=%d SetGraphicsTable=%d "
                     "SetGraphicsOther=%d ClearUAV=%d nrHeaps=%llu nrTables=%llu\n",
                     g_probe_set_heaps, g_probe_gfx_rs, g_probe_gfx_table, g_probe_gfx_other, g_probe_clear_uav,
                     static_cast<unsigned long long>(nr::pe::d3d12_direct::nr_set_heap_count()),
                     static_cast<unsigned long long>(nr::pe::d3d12_direct::nr_set_table_count()));
        detach_probe(cmd);
        return 1;
    }
    if (g_last_comp_rs != rs_host_comp || g_last_pso != k_host.pso ||
        g_last_comp_uav[1] != host_slot.gpu_va ||
        g_last_comp_const[0] != CW || g_last_comp_const[1] != CH || g_last_comp_const[4] != 0x13572468u) {
        std::fprintf(stderr,
                     "FAIL: R33 Case A Compute state mismatch: rs=%p (exp %p) pso=%p (exp %p) uav=0x%llx (exp 0x%llx) "
                     "c0=0x%08x c1=0x%08x c4=0x%08x\n",
                     static_cast<void*>(g_last_comp_rs), static_cast<void*>(rs_host_comp),
                     static_cast<void*>(g_last_pso), static_cast<void*>(k_host.pso),
                     static_cast<unsigned long long>(g_last_comp_uav[1]),
                     static_cast<unsigned long long>(host_slot.gpu_va),
                     g_last_comp_const[0], g_last_comp_const[1], g_last_comp_const[4]);
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
        if (FAILED(host_rb->Map(0, nullptr, reinterpret_cast<void**>(&m))) || !m) {
            std::fprintf(stderr, "FAIL: Case A host_rb->Map failed\n");
            return 1;
        }
        double max_err = 0.0;
        for (uint32_t y = 0; y < CH; ++y) {
            const auto* row = reinterpret_cast<const float*>(m + uint64_t(y) * host_fp.Footprint.RowPitch);
            for (uint32_t x = 0; x < CW; ++x) {
                if (!std::isfinite(row[x])) {
                    host_rb->Unmap(0, nullptr);
                    std::fprintf(stderr, "FAIL: Case A non-finite float readback (%g)\n", double(row[x]));
                    return 1;
                }
                max_err = std::max(max_err, std::abs(double(row[x]) - double(expected[size_t(y) * CW + x])));
            }
        }
        host_rb->Unmap(0, nullptr);
        if (max_err > 1e-6) {
            std::fprintf(stderr, "FAIL: Case A restored Compute dispatch readback max_err=%g\n", max_err);
            return 1;
        }
    }
    std::printf("[PASS] Case A: SetDescriptorHeaps=0 (nr=%llu), Graphics=0, ClearUAV=0, Compute state restored & verified\n",
                static_cast<unsigned long long>(nr::pe::d3d12_direct::nr_set_heap_count()));

    // ---- Case B & B2: Stale Compute Table Invalidation & Valid Compute Table Restore ----
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
        g_last_comp_param2 != 0xCAFEBABEu || g_last_pso != k_host.pso) {
        std::fprintf(stderr,
                     "FAIL: Case B stale-table invalidation: ok=%d heaps=%d comp_table=%d rs=%p (exp %p) "
                     "uav=0x%llx (exp 0x%llx) c2=0x%08x pso=%p (exp %p)\n",
                     ok_caseb, g_probe_set_heaps, g_probe_comp_table,
                     static_cast<void*>(g_last_comp_rs), static_cast<void*>(rs_table),
                     static_cast<unsigned long long>(g_last_comp_uav[1]),
                     static_cast<unsigned long long>(caseb_slot.gpu_va),
                     g_last_comp_param2, static_cast<void*>(g_last_pso), static_cast<void*>(k_host.pso));
        return 1;
    }
    std::printf("[PASS] Case B: Mid-list SetDescriptorHeaps invalidated Compute table (replayed=%d), RS/UAV/Const/PSO restored\n",
                g_probe_comp_table);

    // ---- Case B2: Unchanged Compute Descriptor Table restored once without SetDescriptorHeaps ----
    alloc->Reset();
    cmd->Reset(alloc, nullptr);
    const nr::gpu::Ticket caseb2_ticket = nr::pe::d3d12_direct::book(cmd);
    const nr::pe::d3d12_outer::OuterSlot caseb2_slot = host_pool.acquire(caseb2_ticket, &why, k_host.desc_zero_bytes);
    if (!caseb2_slot.cpu) {
        std::fprintf(stderr, "FAIL: Case B2 host_pool.acquire: %s\n", why.c_str());
        return 1;
    }
    const D3D12_GPU_DESCRIPTOR_HANDLE tbl_h = host_heaps[0]->GetGPUDescriptorHandleForHeapStart();
    cmd->SetDescriptorHeaps(1, &host_heaps[0]);
    cmd->SetComputeRootSignature(rs_table);
    cmd->SetComputeRootDescriptorTable(0, tbl_h);
    cmd->SetComputeRootUnorderedAccessView(1, caseb2_slot.gpu_va);
    cmd->SetComputeRoot32BitConstant(2, 0xFACE1234u, 0);
    cmd->SetPipelineState(k_host.pso);

    attach_probe(cmd);
    reset_probes();
    nr::pe::d3d12_direct::reset_counters();
    const bool ok_caseb2 = nr::pe::d3d12_passthrough::copy_depth_r32(device, cmd, host_src, host_dst, &why);
    detach_probe(cmd);
    cmd->Close();
    {
        ID3D12CommandList* ls[] = {cmd};
        queue->ExecuteCommandLists(1, ls);
        wait_gpu();
    }
    if (!ok_caseb2 || g_probe_set_heaps != 0 || g_probe_comp_table != 1 ||
        g_last_comp_table.ptr != tbl_h.ptr || g_last_comp_rs != rs_table ||
        g_last_comp_uav[1] != caseb2_slot.gpu_va || g_last_comp_param2 != 0xFACE1234u ||
        g_last_pso != k_host.pso || nr::pe::d3d12_direct::host_table_replays() < 1 ||
        nr::pe::d3d12_direct::nr_set_heap_count() != 0 || nr::pe::d3d12_direct::nr_set_table_count() != 0) {
        std::fprintf(stderr,
                     "FAIL: Case B2 valid-table restore: ok=%d heaps=%d comp_table=%d replays=%llu tbl=0x%llx (exp 0x%llx) "
                     "rs=%p uav=0x%llx c2=0x%08x pso=%p nrH=%llu nrT=%llu\n",
                     ok_caseb2, g_probe_set_heaps, g_probe_comp_table,
                     static_cast<unsigned long long>(nr::pe::d3d12_direct::host_table_replays()),
                     static_cast<unsigned long long>(g_last_comp_table.ptr),
                     static_cast<unsigned long long>(tbl_h.ptr),
                     static_cast<void*>(g_last_comp_rs),
                     static_cast<unsigned long long>(g_last_comp_uav[1]),
                     g_last_comp_param2, static_cast<void*>(g_last_pso),
                     static_cast<unsigned long long>(nr::pe::d3d12_direct::nr_set_heap_count()),
                     static_cast<unsigned long long>(nr::pe::d3d12_direct::nr_set_table_count()));
        return 1;
    }
    std::printf("[PASS] Case B2: Valid Compute descriptor table restored once (replays=%llu), zero SetDescriptorHeaps\n",
                static_cast<unsigned long long>(nr::pe::d3d12_direct::host_table_replays()));

    // ---- D01: SRD probe rejection/recovery + RGBA16F & R32_TYPELESS roundtrip helper ----
    {
        nr::pe::d3d12_outer::OuterHeapPool probe_pool;
        _putenv_s("NR_TEST_FAIL_SRD_PROBE", "1");
        const bool bad_init = probe_pool.init(device, 16, nullptr, nullptr, &why);
        _putenv_s("NR_TEST_FAIL_SRD_PROBE", "");
        if (bad_init) {
            std::fprintf(stderr, "FAIL: D01 SRD probe failure injection did not fail pool init\n");
            return 1;
        }
        if (!probe_pool.init(device, 16, nullptr, nullptr, &why)) {
            std::fprintf(stderr, "FAIL: D01 SRD probe recovery init failed: %s\n", why.c_str());
            return 1;
        }
        probe_pool.reset();
    }

    auto run_outer_roundtrip = [&](ID3D12Device* dev, ID3D12CommandQueue* q, ID3D12CommandAllocator* ca,
                                   ID3D12GraphicsCommandList* cl, ID3D12Fence* f, HANDLE ev, uint64_t& fv,
                                   const std::shared_ptr<nr::pe::d3d12_outer::OuterArchive>& oa, bool is_rdna3,
                                   const char* label) -> bool {
        ID3D12RootSignature* rs = nullptr;
        nr::pe::d3d12_outer::OuterKernel k_tl{}, k_fl{}, k_dp{}, k_wc{}, k_il5{}, k_il3{};
        nr::pe::d3d12_outer::OuterHeapPool pool;
        std::string rwhy;
        if (!nr::pe::d3d12_outer::create_outer_root_signature(dev, &rs, &rwhy) ||
            !oa->build_kernel(dev, "backend/to_linear", &k_tl, &rwhy) ||
            !oa->build_kernel(dev, "backend/from_linear", &k_fl, &rwhy) ||
            !oa->build_kernel(dev, "backend/copy_depth_r32", &k_dp, &rwhy) ||
            !oa->build_kernel(dev, "wilsjo2/copy", &k_wc, &rwhy) ||
            !oa->build_kernel(dev, "il/il0_5", &k_il5, &rwhy) ||
            !oa->build_kernel(dev, "il/il0_3", &k_il3, &rwhy) ||
            !pool.init(dev, 16, nullptr, nullptr, &rwhy)) {
            std::fprintf(stderr, "FAIL: %s outer kernel setup: %s\n", label, rwhy.c_str());
            if (k_tl.pso) k_tl.pso->Release();
            if (k_fl.pso) k_fl.pso->Release();
            if (k_dp.pso) k_dp.pso->Release();
            if (k_wc.pso) k_wc.pso->Release();
            if (k_il5.pso) k_il5.pso->Release();
            if (k_il3.pso) k_il3.pso->Release();
            if (rs) rs->Release();
            return false;
        }
        const auto* b_tl_tex = k_tl.find_binding(0);
        const auto* b_tl_buf = k_tl.find_binding(3, nr::pe::d3d12_outer::kBuf);
        const auto* b_fl_buf = k_fl.find_binding(1, nr::pe::d3d12_outer::kBuf);
        const auto* b_fl_tex = k_fl.find_binding(2);
        const auto* b_dp_srv = k_dp.find_binding(0);
        const auto* b_dp_uav = k_dp.find_binding(2);
        const auto* b_wc_srv = k_wc.find_binding(0);
        const auto* b_wc_uav = k_wc.find_binding(4);
        const auto* b_il5_d  = k_il5.find_binding(29);
        const auto* b_il5_dc = k_il5.find_binding(30);
        const auto* b_il3_p  = k_il3.find_binding(0);
        const auto* b_il3_b  = k_il3.find_binding(2);
        if (!b_tl_tex || !b_tl_buf || !b_fl_buf || !b_fl_tex || !b_dp_srv || !b_dp_uav || !b_wc_srv || !b_wc_uav ||
            !b_il5_d || !b_il5_dc || !b_il3_p || !b_il3_b) {
            std::fprintf(stderr, "FAIL: %s outer kernel bindings incomplete\n", label);
            pool.reset();
            if (k_tl.pso) k_tl.pso->Release();
            if (k_fl.pso) k_fl.pso->Release();
            if (k_dp.pso) k_dp.pso->Release();
            if (k_wc.pso) k_wc.pso->Release();
            if (k_il5.pso) k_il5.pso->Release();
            if (k_il3.pso) k_il3.pso->Release();
            rs->Release();
            return false;
        }
        ID3D12Resource* f16_in = make_tex2d(dev, CW, CH, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12Resource* f16_out = make_tex2d(dev, CW, CH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Resource* il_base = make_tex2d(dev, CW, CH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12Resource* d32_in = make_tex2d(dev, CW, CH, DXGI_FORMAT_R32_TYPELESS, false, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12Resource* d32_out = make_tex2d(dev, CW, CH, DXGI_FORMAT_R32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Resource* d32_il_out = make_tex2d(dev, CW, CH, DXGI_FORMAT_R32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const uint32_t align = is_rdna3 ? 256u : 128u;
        const uint32_t pitch8 = (CW * 8u + align - 1u) / align * align / 8u;
        ID3D12Resource* lin_buf = make_buf(dev, D3D12_HEAP_TYPE_DEFAULT, uint64_t(pitch8) * CH * 8u,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT f16_fp{};
        UINT rows16 = 0;
        UINT64 rowb16 = 0, tot16 = 0;
        const D3D12_RESOURCE_DESC fd16 = f16_out->GetDesc();
        dev->GetCopyableFootprints(&fd16, 0, 1, 0, &f16_fp, &rows16, &rowb16, &tot16);
        ID3D12Resource* f16_up = make_buf(dev, D3D12_HEAP_TYPE_UPLOAD, tot16, D3D12_RESOURCE_STATE_GENERIC_READ);
        ID3D12Resource* f16_rb = make_buf(dev, D3D12_HEAP_TYPE_READBACK, tot16, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12Resource* il_up = make_buf(dev, D3D12_HEAP_TYPE_UPLOAD, tot16, D3D12_RESOURCE_STATE_GENERIC_READ);
        ID3D12Resource* il_rb = make_buf(dev, D3D12_HEAP_TYPE_READBACK, tot16, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT d32_fp{};
        UINT rows32 = 0;
        UINT64 rowb32 = 0, tot32 = 0;
        const D3D12_RESOURCE_DESC fd32 = d32_out->GetDesc();
        dev->GetCopyableFootprints(&fd32, 0, 1, 0, &d32_fp, &rows32, &rowb32, &tot32);
        ID3D12Resource* d32_up = make_buf(dev, D3D12_HEAP_TYPE_UPLOAD, tot32, D3D12_RESOURCE_STATE_GENERIC_READ);
        ID3D12Resource* d32_rb = make_buf(dev, D3D12_HEAP_TYPE_READBACK, tot32, D3D12_RESOURCE_STATE_COPY_DEST);

        bool ok = f16_in && f16_out && il_base && d32_in && d32_out && d32_il_out && lin_buf &&
                  f16_up && f16_rb && il_up && il_rb && d32_up && d32_rb;
        std::vector<float> exp_f16(size_t(CW) * CH * 4u);
        std::vector<float> exp_d32(size_t(CW) * CH);
        if (ok) {
            uint8_t* m16 = nullptr;
            f16_up->Map(0, nullptr, reinterpret_cast<void**>(&m16));
            for (uint32_t y = 0; y < CH; ++y) {
                auto* row = reinterpret_cast<uint16_t*>(m16 + uint64_t(y) * f16_fp.Footprint.RowPitch);
                for (uint32_t x = 0; x < CW; ++x) {
                    for (uint32_t c = 0; c < 4u; ++c) {
                        const float v = (c == 3u) ? 1.0f : (0.125f + float((x + y * 5u + c * 3u) & 31u) * 0.03125f);
                        const uint16_t h = float_to_f16(v);
                        row[size_t(x) * 4u + c] = h;
                        exp_f16[(size_t(y) * CW + x) * 4u + c] = f16_to_float(h);
                    }
                }
            }
            f16_up->Unmap(0, nullptr);

            uint8_t* mi = nullptr;
            il_up->Map(0, nullptr, reinterpret_cast<void**>(&mi));
            for (uint32_t y = 0; y < CH; ++y) {
                auto* row = reinterpret_cast<uint16_t*>(mi + uint64_t(y) * f16_fp.Footprint.RowPitch);
                for (uint32_t x = 0; x < CW; ++x) {
                    row[size_t(x) * 4u + 0] = float_to_f16(0.0625f);
                    row[size_t(x) * 4u + 1] = float_to_f16(0.8750f);
                    row[size_t(x) * 4u + 2] = float_to_f16(0.1250f);
                    row[size_t(x) * 4u + 3] = float_to_f16(0.5000f);
                }
            }
            il_up->Unmap(0, nullptr);

            uint8_t* m32 = nullptr;
            d32_up->Map(0, nullptr, reinterpret_cast<void**>(&m32));
            for (uint32_t y = 0; y < CH; ++y) {
                auto* row = reinterpret_cast<float*>(m32 + uint64_t(y) * d32_fp.Footprint.RowPitch);
                for (uint32_t x = 0; x < CW; ++x) {
                    const float v = 0.1f + float((x * 7u + y * 11u) & 127u) * 0.005f;
                    row[x] = v;
                    exp_d32[size_t(y) * CW + x] = v;
                }
            }
            d32_up->Unmap(0, nullptr);

            ca->Reset();
            cl->Reset(ca, nullptr);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = f16_in;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = f16_up;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = f16_fp;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            dst.pResource = d32_in;
            src.pResource = d32_up;
            src.PlacedFootprint = d32_fp;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            dst.pResource = il_base;
            src.pResource = il_up;
            src.PlacedFootprint = f16_fp;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            barrier_transition(cl, f16_in, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barrier_transition(cl, d32_in, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barrier_transition(cl, il_base, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            auto rec = std::make_shared<nr::gpu::Recording>();
            const auto slot_tl = pool.acquire(rec, &rwhy, k_tl.desc_zero_bytes);
            const auto slot_fl = pool.acquire(rec, &rwhy, k_fl.desc_zero_bytes);
            const auto slot_wc = pool.acquire(rec, &rwhy, k_wc.desc_zero_bytes);
            const auto slot_dp = pool.acquire(rec, &rwhy, k_dp.desc_zero_bytes);
            const auto slot_il5 = pool.acquire(rec, &rwhy, k_il5.desc_zero_bytes);
            const auto slot_il3 = pool.acquire(rec, &rwhy, k_il3.desc_zero_bytes);
            ok = slot_tl.cpu && slot_fl.cpu && slot_wc.cpu && slot_dp.cpu && slot_il5.cpu && slot_il3.cpu;
            if (ok) {
                const UINT lin_push[8] = {CW, CH, pitch8, 0u, 0u, 0u, 0u, 0u};
                slot_tl.write_srv_tex(dev, b_tl_tex, f16_in, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 1);
                slot_tl.write_buf(b_tl_buf, lin_buf, uint64_t(pitch8) * CH * 8u);
                slot_tl.write_push(lin_push, sizeof lin_push, &k_tl);
                slot_tl.dispatch(cl, rs, k_tl, (CW + 7u) / 8u, (CH + 7u) / 8u, 1u, true);

                D3D12_RESOURCE_BARRIER uavb{};
                uavb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                uavb.UAV.pResource = lin_buf;
                cl->ResourceBarrier(1, &uavb);

                slot_fl.write_buf(b_fl_buf, lin_buf, uint64_t(pitch8) * CH * 8u);
                slot_fl.write_uav_tex(dev, b_fl_tex, f16_out, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
                slot_fl.write_push(lin_push, sizeof lin_push, &k_fl);
                slot_fl.dispatch(cl, rs, k_fl, (CW + 7u) / 8u, (CH + 7u) / 8u, 1u, false);

                D3D12_RESOURCE_BARRIER wc_uavb{};
                wc_uavb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                wc_uavb.UAV.pResource = f16_out;
                cl->ResourceBarrier(1, &wc_uavb);

                UINT wc_push[13]{};
                wc_push[2] = 16u;
                wc_push[3] = 16u;
                wc_push[11] = 8u | (4u << 16);
                wc_push[12] = 12u | (6u << 16);
                slot_wc.write_srv_tex(dev, b_wc_srv, f16_in, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 1);
                slot_wc.write_uav_tex(dev, b_wc_uav, f16_out, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
                slot_wc.write_push(wc_push, sizeof wc_push, &k_wc);
                slot_wc.dispatch(cl, rs, k_wc, 2u, 2u, 1u, false);

                const UINT dp_push[2] = {CW, CH};
                slot_dp.write_srv_tex(dev, b_dp_srv, d32_in, DXGI_FORMAT_R32_FLOAT, 0, 1);
                slot_dp.write_uav_tex(dev, b_dp_uav, d32_out, DXGI_FORMAT_R32_FLOAT, 0);
                slot_dp.write_push(dp_push, sizeof dp_push, &k_dp);
                slot_dp.dispatch(cl, rs, k_dp, (CW + 7u) / 8u, (CH + 7u) / 8u, 1u, false);

                D3D12_RESOURCE_BARRIER il_uavb{};
                il_uavb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                cl->ResourceBarrier(1, &il_uavb);

                struct IlPushRec {
                    uint32_t size[2], mbase[2], mgrid[2];
                    float mscale[2];
                    uint32_t mode, slot, n, flags;
                    float k0, k1;
                    uint32_t dbase[2], dgrid[2], styles;
                    float pad0, inten[4];
                };
                IlPushRec ip{{CW, CH}, {0u, 0u}, {CW, CH}, {1.f, 1.f}, 5u, 0u, 2u, 0u, 0.02f, 0.06f,
                             {0u, 0u}, {CW, CH}, 0u, 0.f, {1.f, 1.f, 1.f, 1.f}};
                slot_il5.write_srv_tex(dev, b_il5_d, d32_in, DXGI_FORMAT_R32_FLOAT, 0, 1);
                slot_il5.write_uav_tex(dev, b_il5_dc, d32_il_out, DXGI_FORMAT_R32_FLOAT, 0);
                slot_il5.write_push(&ip, sizeof ip, &k_il5);
                slot_il5.dispatch(cl, rs, k_il5, (CW + 15u) / 16u, (CH + 15u) / 16u, 1u, false);

                cl->ResourceBarrier(1, &il_uavb);

                ip.mode = 3u;
                slot_il3.write_uav_tex(dev, b_il3_p, f16_out, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
                slot_il3.write_uav_tex(dev, b_il3_b, il_base, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
                slot_il3.write_push(&ip, sizeof ip, &k_il3);
                slot_il3.dispatch(cl, rs, k_il3, (CW + 15u) / 16u, (CH + 15u) / 16u, 1u, false);

                barrier_transition(cl, f16_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                barrier_transition(cl, d32_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                barrier_transition(cl, il_base, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

                dst.pResource = f16_rb;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = f16_fp;
                src.pResource = f16_out;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

                dst.pResource = il_rb;
                src.pResource = il_base;
                cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

                dst.pResource = d32_rb;
                dst.PlacedFootprint = d32_fp;
                src.pResource = d32_out;
                cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

                cl->Close();
                ID3D12CommandList* ls[] = {cl};
                q->ExecuteCommandLists(1, ls);
                ok = wait_queue_strict(q, f, ++fv, ev, label);
                rec->seal();
                if (ok && FAILED(dev->GetDeviceRemovedReason())) {
                    std::fprintf(stderr, "FAIL: %s device removed (0x%08lx)\n", label,
                                 static_cast<unsigned long>(dev->GetDeviceRemovedReason()));
                    ok = false;
                }
                if (ok) {
                    double err16 = 0.0;
                    f16_rb->Map(0, nullptr, reinterpret_cast<void**>(&m16));
                    for (uint32_t y = 0; y < CH && ok; ++y) {
                        const auto* row = reinterpret_cast<const uint16_t*>(m16 + uint64_t(y) * f16_fp.Footprint.RowPitch);
                        for (uint32_t x = 0; x < CW; ++x) {
                            for (uint32_t c = 0; c < 4u; ++c) {
                                const float got = f16_to_float(row[size_t(x) * 4u + c]);
                                if (!std::isfinite(got)) {
                                    std::fprintf(stderr, "FAIL: %s non-finite RGBA16F readback\n", label);
                                    ok = false;
                                    break;
                                }
                                const bool in_wc = (x >= 12u && x < 28u && y >= 6u && y < 22u);
                                const uint32_t sx = in_wc ? (x - 4u) : x;
                                const uint32_t sy = in_wc ? (y - 2u) : y;
                                err16 = std::max(err16, std::abs(double(got) - double(exp_f16[(size_t(sy) * CW + sx) * 4u + c])));
                            }
                            if (!ok) break;
                        }
                    }
                    f16_rb->Unmap(0, nullptr);

                    double err32 = 0.0;
                    d32_rb->Map(0, nullptr, reinterpret_cast<void**>(&m32));
                    for (uint32_t y = 0; y < CH && ok; ++y) {
                        const auto* row = reinterpret_cast<const float*>(m32 + uint64_t(y) * d32_fp.Footprint.RowPitch);
                        for (uint32_t x = 0; x < CW; ++x) {
                            if (!std::isfinite(row[x])) {
                                std::fprintf(stderr, "FAIL: %s non-finite R32_TYPELESS depth readback (%g)\n",
                                             label, double(row[x]));
                                ok = false;
                                break;
                            }
                            err32 = std::max(err32, std::abs(double(row[x]) - double(exp_d32[size_t(y) * CW + x])));
                        }
                    }
                    d32_rb->Unmap(0, nullptr);

                    if (ok && (err16 > 1e-3 || err32 > 1e-6)) {
                        std::fprintf(stderr, "FAIL: %s pixel mismatch (f16_err=%g, d32_err=%g)\n", label, err16, err32);
                        ok = false;
                    }
                    double err_il3 = 0.0;
                    il_rb->Map(0, nullptr, reinterpret_cast<void**>(&m16));
                    for (uint32_t y = 0; y < CH && ok; ++y) {
                        const auto* row = reinterpret_cast<const uint16_t*>(m16 + uint64_t(y) * f16_fp.Footprint.RowPitch);
                        for (uint32_t x = 0; x < CW; ++x) {
                            for (uint32_t c = 0; c < 4u; ++c) {
                                const float got = f16_to_float(row[size_t(x) * 4u + c]);
                                if (!std::isfinite(got)) {
                                    std::fprintf(stderr, "FAIL: %s non-finite il0_3 UAV read+write readback\n", label);
                                    ok = false;
                                    break;
                                }
                                const bool in_wc = (x >= 12u && x < 28u && y >= 6u && y < 22u);
                                const uint32_t sx = in_wc ? (x - 4u) : x;
                                const uint32_t sy = in_wc ? (y - 2u) : y;
                                err_il3 = std::max(err_il3, std::abs(double(got) - double(exp_f16[(size_t(sy) * CW + sx) * 4u + c])));
                            }
                            if (!ok) break;
                        }
                    }
                    il_rb->Unmap(0, nullptr);
                    if (ok && err_il3 > 1e-3) {
                        std::fprintf(stderr, "FAIL: %s il0_5->il0_3 RGBA16F UAV read+write mismatch (err=%g)\n", label, err_il3);
                        ok = false;
                    }
                    if (ok) {
                        std::printf("[PASS] D01 (%s): RGBA16F SRV/UAV (err=%g) + il0_5->il0_3 UAV R+W (err=%g) + Depth SRV (err=%g)\n",
                                    label, err16, err_il3, err32);
                    }
                }
            }
        }
        if (d32_rb) d32_rb->Release();
        if (d32_up) d32_up->Release();
        if (il_rb) il_rb->Release();
        if (il_up) il_up->Release();
        if (f16_rb) f16_rb->Release();
        if (f16_up) f16_up->Release();
        if (lin_buf) lin_buf->Release();
        if (d32_il_out) d32_il_out->Release();
        if (d32_out) d32_out->Release();
        if (d32_in) d32_in->Release();
        if (il_base) il_base->Release();
        if (f16_out) f16_out->Release();
        if (f16_in) f16_in->Release();
        pool.reset();
        if (k_il3.pso) k_il3.pso->Release();
        if (k_il5.pso) k_il5.pso->Release();
        if (k_wc.pso) k_wc.pso->Release();
        if (k_dp.pso) k_dp.pso->Release();
        if (k_fl.pso) k_fl.pso->Release();
        if (k_tl.pso) k_tl.pso->Release();
        rs->Release();
        return ok;
    };

    if (!run_outer_roundtrip(device, queue, alloc, cmd, fence, fence_event, fence_val, arch, false, "RDNA4 primary")) {
        return 1;
    }

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
    bool saw_rdna3_hw = false;
    for (IDXGIAdapter1* a3 : secondary_amd) {
        DXGI_ADAPTER_DESC1 desc3{};
        a3->GetDesc1(&desc3);
        const nr::arch::Identity id3 = nr::arch::identify(desc3.AdapterLuid, "dxgi", desc3.VendorId, desc3.DeviceId);
        std::printf("[INFO] Case C candidate: %ls (DeviceId=0x%04x, arch=%s)\n",
                    desc3.Description, static_cast<unsigned>(desc3.DeviceId), nr::arch::name(id3.arch));
        if (id3.arch != nr::arch::GpuArch::rdna3) continue;
        saw_rdna3_hw = true;
        ID3D12Device* dev3 = nullptr;
        if (FAILED(D3D12CreateDevice(a3, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev3))) || !dev3) continue;
        D3D12_COMMAND_QUEUE_DESC qd3{D3D12_COMMAND_LIST_TYPE_DIRECT};
        ID3D12CommandQueue* q3 = nullptr;
        ID3D12CommandAllocator* ca3 = nullptr;
        ID3D12GraphicsCommandList* cl3 = nullptr;
        ID3D12Fence* f3 = nullptr;
        HANDLE ev3 = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        uint64_t fv3 = 0;
        dev3->CreateCommandQueue(&qd3, IID_PPV_ARGS(&q3));
        dev3->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca3));
        dev3->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca3, nullptr, IID_PPV_ARGS(&cl3));
        dev3->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f3));
        bool ok3 = false;
        if (q3 && ca3 && cl3 && f3 && ev3) {
            cl3->Close();
            char lbl[96];
            std::snprintf(lbl, sizeof lbl, "RDNA3 DevId=0x%04x %s",
                          static_cast<unsigned>(desc3.DeviceId), nr::arch::name(id3.arch));
            ok3 = run_outer_roundtrip(dev3, q3, ca3, cl3, f3, ev3, fv3, rdna3_arch, true, lbl);
        }
        if (ev3) CloseHandle(ev3);
        if (f3) f3->Release();
        if (cl3) cl3->Release();
        if (ca3) ca3->Release();
        if (q3) q3->Release();
        dev3->Release();
        if (ok3) {
            rdna3_hw = 1;
            break;
        }
    }
    if (saw_rdna3_hw && rdna3_hw == 0) {
        std::fprintf(stderr, "FAIL: Case C RDNA3 hardware adapter was enumerated but hw_dispatch == 0\n");
        return 1;
    }
    std::printf("[PASS] Case C (archive): d3dcompiler_47.dll=nullptr, RDNA3 NRD12OUT archive verified\n");
    if (rdna3_hw)
        std::printf("[PASS] Case C (RDNA3 hw): Secondary RDNA3 hardware dispatch verified\n");
    else
        std::printf("[SKIP] Case C (RDNA3 hw): No secondary RDNA3 hardware adapter present\n");

    // ---- Case D: ReShade-style command list vtable wrapper safety ----
    {
        ID3D12CommandAllocator* ma = nullptr;
        ID3D12GraphicsCommandList* mc = nullptr;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ma))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ma, nullptr, IID_PPV_ARGS(&mc)))) {
            std::fprintf(stderr, "FAIL: Case D mock command list setup\n");
            return 1;
        }
        void** orig_mock_vt = *reinterpret_cast<void***>(mc);
        void* mock_vt[86]{};
        std::memcpy(mock_vt, orig_mock_vt, sizeof mock_vt);
        mock_vt[9] = reinterpret_cast<void*>(&mock_wrap_close);
        mock_vt[10] = reinterpret_cast<void*>(&mock_wrap_reset);
        *reinterpret_cast<void***>(mc) = mock_vt;

        std::string sp_why, dr_why;
        const bool sp_ok = nr::pe::split::prepare(mc, &sp_why);
        const bool dr_ok = nr::pe::d3d12_direct::ready(device, mc, &dr_why);
        *reinterpret_cast<void***>(mc) = orig_mock_vt;
        mc->Close();
        mc->Release();
        ma->Release();

        if (sp_ok || dr_ok ||
            mock_vt[9] != reinterpret_cast<void*>(&mock_wrap_close) ||
            mock_vt[10] != reinterpret_cast<void*>(&mock_wrap_reset)) {
            std::fprintf(stderr, "FAIL: Case D wrapper safety sp_ok=%d dr_ok=%d\n", sp_ok, dr_ok);
            return 1;
        }
        std::printf("[PASS] Case D: External vtable wrapper safely declined (split=%d direct=%d, vtable intact)\n",
                    sp_ok, dr_ok);
    }

    // ---- Case E: Bounded OuterHeapPool (max_blocks=2) + Stage::cmd_touched ----
    {
        bool allow_reuse = false;
        nr::pe::d3d12_outer::OuterHeapPool bpool;
        if (!bpool.init(device, 16, [&](const nr::gpu::Ticket&) { return allow_reuse; }, nullptr, &why)) {
            std::fprintf(stderr, "FAIL: Case E bounded pool init: %s\n", why.c_str());
            return 1;
        }
        bpool.set_max_blocks(2);
        auto fake_t = std::make_shared<nr::gpu::Recording>();
        for (int i = 0; i < 32; ++i) {
            const auto sl = bpool.acquire(fake_t, &why);
            if (!sl.cpu) {
                std::fprintf(stderr, "FAIL: Case E bounded pool acquire #%d failed early: %s\n", i, why.c_str());
                return 1;
            }
        }
        if (bpool.block_count() != 2 || bpool.peak_blocks() != 2 || bpool.reserve(1, &why) ||
            bpool.acquire(fake_t, &why).cpu != nullptr) {
            std::fprintf(stderr, "FAIL: Case E bounded pool did not cap at max_blocks=2 (blocks=%zu peak=%zu)\n",
                         bpool.block_count(), bpool.peak_blocks());
            return 1;
        }
        allow_reuse = true;
        if (!bpool.reserve(1, &why) || !bpool.acquire(fake_t, &why).cpu ||
            bpool.block_count() != 2 || bpool.peak_blocks() != 2) {
            std::fprintf(stderr, "FAIL: Case E bounded pool did not reuse completed slots (%s)\n", why.c_str());
            return 1;
        }
        bpool.reset();

        nr::pe::d3d12_passthrough::renodx12::Stage::Config scfg{};
        scfg.device = device;
        scfg.width = CW;
        scfg.height = CH;
        scfg.nr_width = CW;
        scfg.nr_height = CH;
        scfg.frame_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scfg.settings.nr_enabled = false;
        auto st = nr::pe::d3d12_passthrough::renodx12::Stage::make(scfg, &why);
        if (!st) {
            std::fprintf(stderr, "FAIL: Case E Stage::make: %s\n", why.c_str());
            return 1;
        }
        ID3D12Resource* sin = make_tex2d(device, CW, CH, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D12Resource* sout = make_tex2d(device, CW, CH, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        nr::pe::d3d12_passthrough::renodx12::Frame rf{};
        rf.colour = sin;
        rf.output = sout;
        auto noop_net = [](uint32_t, uint64_t, const nr::D3d12NetControls&,
                           const nr::pe::d3d12_passthrough::NetTemporal&,
                           ID3D12Resource*, ID3D12Resource*) { return false; };
        st->record(nullptr, rf, noop_net);
        if (st->record_ok() || st->cmd_touched()) {
            std::fprintf(stderr, "FAIL: Case E Stage::record(nullptr) ok=%d touched=%d\n", st->record_ok(), st->cmd_touched());
            return 1;
        }
        alloc->Reset();
        cmd->Reset(alloc, nullptr);
        st->record(cmd, rf, noop_net);
        const bool ok_st = st->record_ok();
        const bool touched_st = st->cmd_touched();
        cmd->Close();
        {
            ID3D12CommandList* ls[] = {cmd};
            queue->ExecuteCommandLists(1, ls);
            wait_gpu();
        }
        st.reset();
        sout->Release();
        sin->Release();
        if (!ok_st || !touched_st) {
            std::fprintf(stderr, "FAIL: Case E Stage::record(cmd) ok=%d touched=%d\n", ok_st, touched_st);
            return 1;
        }

        // D02 evaluate_backend failure branches: pre-recording vs post-dispatch partial cleanup & recovery
        nr::renodx::Settings r_e{};
        r_e.nr_enabled = true;
        r_e.passes = 1;
        r_e.codec_mode = 1;
        r_e.transfer_strength = 0.8f;   // force non-fast_lin path: dispatch #1 = kEncodeL, dispatch #2 = kDecodeL
        int wait_e = 0;
        auto make_e_frame = [&]() {
            nr::dlssnr::D3d12BackendFrame bf = fill_plain_frame(&wait_e);
            bf.feature = 0x55AAu;
            bf.motion = nullptr; bf.motion_w = 0; bf.motion_h = 0;
            bf.depth = nullptr; bf.depth_w = 0; bf.depth_h = 0;
            bf.renodx_route = 1;
            bf.renodx_present = 1;
            bf.renodx_white_nits = 203.0f;
            bf.renodx_settings = &r_e;
            return bf;
        };
        auto run_e_eval = [&]() -> bool {
            alloc->Reset();
            cmd->Reset(alloc, nullptr);
            const bool res = nr::pe::d3d12_passthrough::evaluate_backend(make_e_frame());
            cmd->Close();
            ID3D12CommandList* ls[] = {cmd};
            queue->ExecuteCommandLists(1, ls);
            wait_gpu();
            return res;
        };
        using RenoStage = nr::pe::d3d12_passthrough::renodx12::Stage;
        RenoStage::set_test_fail_dispatch_at(0);
        if (!run_e_eval()) {
            std::fprintf(stderr, "FAIL: Case E evaluate_backend warmup frame returned false\n");
            return 1;
        }
        RenoStage::set_test_fail_dispatch_at(1);
        const bool pre_res = run_e_eval();
        RenoStage::set_test_fail_dispatch_at(0);
        if (pre_res) {
            std::fprintf(stderr, "FAIL: Case E pre-recording failure unexpectedly returned true\n");
            return 1;
        }
        if (!run_e_eval()) {
            std::fprintf(stderr, "FAIL: Case E evaluate_backend re-prime after pre-recording failure returned false\n");
            return 1;
        }
        RenoStage::set_test_fail_dispatch_at(2);
        const bool post_res = run_e_eval();
        const bool post_produced = nr::pe::d3d12_passthrough::backend_produced();
        RenoStage::set_test_fail_dispatch_at(0);
        const std::string post_line = nr::pe::d3d12_passthrough::status().backend;
        if (!post_res || post_produced || post_line.find("dropped partial RenoDX D3D12 frame") == std::string::npos) {
            std::fprintf(stderr, "FAIL: Case E post-dispatch failure post_res=%d produced=%d line=%s\n",
                         post_res, post_produced ? 1 : 0, post_line.c_str());
            return 1;
        }
        if (!run_e_eval() || !nr::pe::d3d12_passthrough::backend_produced()) {
            std::fprintf(stderr, "FAIL: Case E evaluate_backend recovery frame failed\n");
            return 1;
        }
        std::printf("[PASS] Case E: Bounded OuterHeapPool + Stage::cmd_touched + evaluate_backend pre/post fail recovery verified\n");
    }

    // ---- Case F: Delayed-fence completed() false->true, allocator swap, & Queue Signal failure ----
    {
        ID3D12Fence* delay_f = nullptr;
        ID3D12CommandAllocator* fa = nullptr;
        ID3D12CommandAllocator* fa2 = nullptr;
        ID3D12GraphicsCommandList* fc = nullptr;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&delay_f))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&fa))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&fa2))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, fa, nullptr, IID_PPV_ARGS(&fc)))) {
            std::fprintf(stderr, "FAIL: Case F setup\n");
            return 1;
        }
        const nr::gpu::Ticket tf = nr::pe::d3d12_direct::book(fc);
        queue->Wait(delay_f, 1);
        fc->Close();
        {
            ID3D12CommandList* ls[] = {fc};
            queue->ExecuteCommandLists(1, ls);
        }
        fc->Reset(fa2, nullptr);
        if (nr::pe::d3d12_direct::completed(tf)) {
            delay_f->Signal(1);
            wait_gpu();
            std::fprintf(stderr, "FAIL: Case F completed() returned true before delayed fence signaled\n");
            return 1;
        }
        delay_f->Signal(1);
        wait_gpu();
        const bool done_after = nr::pe::d3d12_direct::completed(tf);
        fc->Close();
        fa->Reset();
        fc->Reset(fa, nullptr);

        // Allocator swap + Reset(fa2, nullptr) tracking
        const nr::gpu::Ticket t_a1 = nr::pe::d3d12_direct::book(fc);
        fc->Close();
        fc->Reset(fa2, nullptr);
        const bool a1_sealed = t_a1 && t_a1->sealed;
        const nr::gpu::Ticket t_a2 = nr::pe::d3d12_direct::book(fc);
        fc->Close();
        fa->Reset();   // old allocator Reset must NOT seal ticket recorded against fa2
        const bool a2_kept_after_fa = t_a2 && !t_a2->sealed;
        fa2->Reset();  // owning allocator Reset MUST seal t_a2
        const bool a2_sealed_after_fa2 = t_a2 && t_a2->sealed;
        fc->Reset(fa2, nullptr);

        // Real Queue::ExecuteCommandLists Signal failure via test hook
        const nr::gpu::Ticket t_sig = nr::pe::d3d12_direct::book(fc);
        fc->Close();
        nr::pe::d3d12_direct::set_test_fail_queue_signal(true);
        {
            ID3D12CommandList* ls[] = {fc};
            queue->ExecuteCommandLists(1, ls);
        }
        nr::pe::d3d12_direct::set_test_fail_queue_signal(false);
        fc->Reset(fa2, nullptr);
        wait_gpu();
        const bool sig_unk_ok = t_sig && t_sig->unknown_submit && !t_sig->strict_ready() && !nr::pe::d3d12_direct::completed(t_sig);

        auto unk = std::make_shared<nr::gpu::Recording>();
        unk->seal();
        unk->submitted(nullptr);
        const bool unk_ok = unk->unknown_submit && !unk->strict_ready() && !nr::pe::d3d12_direct::completed(unk);
        fc->Close();
        fc->Release();
        fa2->Release();
        fa->Release();
        delay_f->Release();
        if (!done_after || !a1_sealed || !a2_kept_after_fa || !a2_sealed_after_fa2 || !sig_unk_ok || !unk_ok) {
            std::fprintf(stderr,
                         "FAIL: Case F done_after=%d a1_sealed=%d a2_kept=%d a2_sealed=%d sig_unk=%d unk=%d\n",
                         done_after, a1_sealed, a2_kept_after_fa, a2_sealed_after_fa2, sig_unk_ok, unk_ok);
            return 1;
        }
        std::printf("[PASS] Case F: Delayed fence, allocator swap Reset(fa2), & Queue Signal fail unknown_submit verified\n");
    }

    // ---- Case G: Untracked bundle, MethodHook rollback/remove, 32x lifecycle cleanup, and hook subprocesses ----
    {
        alloc->Reset();
        cmd->Reset(alloc, nullptr);
        if (!nr::pe::d3d12_direct::ready(device, cmd, &why)) {
            std::fprintf(stderr, "FAIL: Case G ready before ExecuteBundle: %s\n", why.c_str());
            return 1;
        }
        cmd->ExecuteBundle(untracked_bundle);
        if (nr::pe::d3d12_direct::ready(device, cmd, &why)) {
            std::fprintf(stderr, "FAIL: Case G untracked ExecuteBundle did not invalidate Compute state\n");
            return 1;
        }
        cmd->Close();
        alloc->Reset();
        cmd->Reset(alloc, nullptr);
        if (!nr::pe::d3d12_direct::ready(device, cmd, &why)) {
            std::fprintf(stderr, "FAIL: Case G ready did not recover after Reset: %s\n", why.c_str());
            return 1;
        }
        cmd->Close();

        using FakeHook = nr::hudcap::MethodHook<PFN_FakeTarget>;
        g_fake_real = reinterpret_cast<PFN_FakeTarget>(uintptr_t(0x1234));
        if (FakeHook::install_raw(nullptr, &fake_mh_detour, &g_fake_real) == MH_OK || g_fake_real != nullptr) {
            std::fprintf(stderr, "FAIL: Case G MethodHook::install_raw(nullptr) did not clean out_next\n");
            return 1;
        }
        int dummy = 42;
        if (FakeHook::install_raw(reinterpret_cast<void*>(&fake_mh_target), &fake_mh_detour, &g_fake_real) != MH_OK ||
            fake_mh_target(&dummy) != 2) {
            std::fprintf(stderr, "FAIL: Case G MethodHook retry install/call failed\n");
            return 1;
        }
        if (FakeHook::remove_raw(reinterpret_cast<void*>(&fake_mh_target), &fake_mh_detour, &g_fake_real) != MH_OK ||
            fake_mh_target(&dummy) != 1 || g_fake_real == nullptr || g_fake_real(&dummy) != 1) {
            std::fprintf(stderr, "FAIL: Case G MethodHook remove_raw cleanup failed\n");
            return 1;
        }
        if (FakeHook::install_raw(reinterpret_cast<void*>(&fake_mh_target), &fake_mh_detour, &g_fake_real) != MH_OK ||
            fake_mh_target(&dummy) != 2 ||
            FakeHook::remove_raw(reinterpret_cast<void*>(&fake_mh_target), &fake_mh_detour, &g_fake_real) != MH_OK ||
            fake_mh_target(&dummy) != 1) {
            std::fprintf(stderr, "FAIL: Case G MethodHook disabled-slot re-install failed\n");
            return 1;
        }

        wait_gpu();
        const size_t base_lists = nr::pe::d3d12_direct::tracked_list_count();
        const size_t base_recs = nr::pe::d3d12_direct::tracked_recording_count();
        const size_t base_queues = nr::pe::d3d12_direct::tracked_queue_fence_count();
        HANDLE tev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (int i = 0; i < 32; ++i) {
            ID3D12CommandQueue* tq = nullptr;
            ID3D12CommandAllocator* ta = nullptr;
            ID3D12GraphicsCommandList* tl = nullptr;
            ID3D12Fence* tf = nullptr;
            if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&tq))) ||
                FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ta))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ta, nullptr, IID_PPV_ARGS(&tl))) ||
                FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tf)))) {
                std::fprintf(stderr, "FAIL: Case G 32x resource creation at %d\n", i);
                return 1;
            }
            (void)nr::pe::d3d12_direct::book(tl);
            tl->Close();
            ID3D12CommandList* ls[] = {tl};
            tq->ExecuteCommandLists(1, ls);
            if (!wait_queue_strict(tq, tf, 1, tev, "caseg_32x")) return 1;
            tf->Release();
            tl->Release();
            ta->Release();
            tq->Release();
        }
        CloseHandle(tev);
        const size_t end_lists = nr::pe::d3d12_direct::tracked_list_count();
        const size_t end_recs = nr::pe::d3d12_direct::tracked_recording_count();
        const size_t end_queues = nr::pe::d3d12_direct::tracked_queue_fence_count();
        if (end_lists != base_lists || end_recs != base_recs || end_queues != base_queues) {
            std::fprintf(stderr,
                         "FAIL: Case G 32x lifecycle leak: lists %zu->%zu, recs %zu->%zu, queues %zu->%zu\n",
                         base_lists, end_lists, base_recs, end_recs, base_queues, end_queues);
            return 1;
        }

        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        for (const wchar_t* flag : {L"--test-hook-rollback", L"--test-no-canary-hooks"}) {
            std::wstring cmdline = std::wstring(L"\"") + exe_path + L"\" " + flag;
            STARTUPINFOW si{};
            si.cb = sizeof si;
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(exe_path, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                std::fprintf(stderr, "FAIL: Case G CreateProcessW(%ls) err=%lu\n", flag, GetLastError());
                return 1;
            }
            const DWORD w = WaitForSingleObject(pi.hProcess, 30000);
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            if (w != WAIT_OBJECT_0 || code != 0) {
                std::fprintf(stderr, "FAIL: Case G subprocess %ls failed (wait=%lu code=%lu)\n", flag, w, code);
                return 1;
            }
        }

        // T01 OuterArchive::load negative test: stride=0 & pad!=0 rejection
        {
            const std::wstring src_nra = nr::pe::d3d12_passthrough::aco_folder() + L"\\d3d12_outer.nra";
            nr::pe::d3d12_passthrough::elf::Bytes nra_bytes;
            if (!nr::pe::d3d12_passthrough::elf::read_file_w(src_nra, &nra_bytes) || nra_bytes.size() < 64) {
                std::fprintf(stderr, "FAIL: Case G cannot read d3d12_outer.nra for stride validation check\n");
                return 1;
            }
            uint16_t nlen = 0;
            std::memcpy(&nlen, nra_bytes.data() + 24, 2);
            const size_t bind0_off = 24u + 2u + size_t(nlen) + 14u;
            if (bind0_off + 8u > nra_bytes.size()) {
                std::fprintf(stderr, "FAIL: Case G nra layout offset out of range\n");
                return 1;
            }
            wchar_t tmp_dir[MAX_PATH]{};
            GetTempPathW(MAX_PATH, tmp_dir);
            const std::wstring bad_path = std::wstring(tmp_dir) + L"nr_test_bad_outer.nra";
            auto check_reject = [&](size_t mut_off, uint8_t mut_val, const char* what) -> bool {
                nr::pe::d3d12_passthrough::elf::Bytes mut = nra_bytes;
                mut[mut_off] = mut_val;
                if (mut_off == bind0_off + 6u) mut[mut_off + 1u] = 0;
                FILE* tfp = _wfopen(bad_path.c_str(), L"wb");
                if (!tfp) return false;
                std::fwrite(mut.data(), 1, mut.size(), tfp);
                std::fclose(tfp);
                nr::pe::d3d12_outer::OuterArchive bad_arch;
                std::string bwhy;
                const bool loaded = bad_arch.load(bad_path, 0u, &bwhy);
                DeleteFileW(bad_path.c_str());
                if (loaded) {
                    std::fprintf(stderr, "FAIL: Case G OuterArchive::load accepted invalid %s\n", what);
                    return false;
                }
                return true;
            };
            if (!check_reject(bind0_off + 6u, 0u, "stride=0") || !check_reject(bind0_off + 3u, 1u, "pad=1")) return 1;
        }
        std::printf("[PASS] Case G: Untracked bundle, MethodHook, 32x cleanup (%zu/%zu/%zu), OuterArchive stride/pad reject, & hook subprocesses\n",
                    end_lists, end_recs, end_queues);
    }

    // ---- Case F2: Mixed Split + Direct batch submission & repeat submission without Reset ----
    {
        ID3D12Fence* mf = nullptr;
        ID3D12CommandAllocator *ma1 = nullptr, *ma2 = nullptr, *ma1_b = nullptr, *ma2_b = nullptr, *ma1_c = nullptr;
        ID3D12GraphicsCommandList *cut_cl = nullptr, *dir_cl = nullptr;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mf))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ma1))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ma2))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ma1_b))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ma2_b))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ma1_c))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ma1, nullptr, IID_PPV_ARGS(&cut_cl))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ma2, nullptr, IID_PPV_ARGS(&dir_cl)))) {
            std::fprintf(stderr, "FAIL: Case F2 mixed split+direct setup\n");
            return 1;
        }
        // Hook command list/queue class with split, then Close+Reset so cut_cl state is clean.
        nr::pe::split::prepare(cut_cl, &why);
        cut_cl->Close();
        ma1->Reset();
        cut_cl->Reset(ma1, nullptr);
        if (!nr::pe::split::prepare(cut_cl, &why)) {
            std::fprintf(stderr, "FAIL: Case F2 split::prepare after Reset: %s\n", why.c_str());
            return 1;
        }
        const nr::gpu::Ticket t_cut = nr::pe::d3d12_direct::book(cut_cl);
        const nr::gpu::Ticket t_dir = nr::pe::d3d12_direct::book(dir_cl);
        auto snap = nr::pe::split::snapshot(cut_cl);
        nr::pe::split::Job job{[&](ID3D12CommandQueue* qq) { qq->Wait(mf, 2); }, [] {}};
        if (!snap || !nr::pe::split::cut(cut_cl, std::move(job), snap, &why)) {
            std::fprintf(stderr, "FAIL: Case F2 split::cut: %s\n", why.c_str());
            return 1;
        }
        if (nr::pe::d3d12_direct::ready(device, cut_cl, &why)) {
            std::fprintf(stderr, "FAIL: Case F2 d3d12_direct::ready succeeded on actively cut command list\n");
            return 1;
        }
        cut_cl->Close();
        dir_cl->Close();
        {
            ID3D12CommandList* mix[2] = {cut_cl, dir_cl};
            queue->ExecuteCommandLists(2, mix);
        }
        cut_cl->Reset(ma1_b, nullptr);
        dir_cl->Reset(ma2_b, nullptr);
        Sleep(50);
        if (nr::pe::d3d12_direct::completed(t_cut) || nr::pe::d3d12_direct::completed(t_dir)) {
            mf->Signal(2);
            wait_gpu();
            std::fprintf(stderr, "FAIL: Case F2 mixed batch completed early before continuation fence signaled (cut=%d dir=%d)\n",
                         nr::pe::d3d12_direct::completed(t_cut), nr::pe::d3d12_direct::completed(t_dir));
            return 1;
        }
        mf->Signal(2);
        wait_gpu();
        if (!nr::pe::d3d12_direct::completed(t_cut) || !nr::pe::d3d12_direct::completed(t_dir)) {
            std::fprintf(stderr, "FAIL: Case F2 mixed batch did not complete after fence signaled (cut=%d dir=%d)\n",
                         nr::pe::d3d12_direct::completed(t_cut), nr::pe::d3d12_direct::completed(t_dir));
            return 1;
        }
        if (!nr::pe::d3d12_direct::ready(device, dir_cl, &why)) {
            std::fprintf(stderr, "FAIL: Case F2 d3d12_direct::ready rejected split-patched command list: %s\n", why.c_str());
            return 1;
        }
        {
            int w_f2 = 0;
            nr::dlssnr::D3d12BackendFrame f_f2 = fill_plain_frame(&w_f2);
            f_f2.cmd = dir_cl;
            const bool eval_f2 = nr::pe::d3d12_passthrough::evaluate_backend(f_f2);
            dir_cl->Close();
            ID3D12CommandList* ls_f2[] = {dir_cl};
            queue->ExecuteCommandLists(1, ls_f2);
            wait_gpu();
            ma2_b->Reset();
            dir_cl->Reset(ma2_b, nullptr);
            if (!eval_f2 || !nr::pe::d3d12_passthrough::backend_produced()) {
                std::fprintf(stderr, "FAIL: Case F2 Direct->Bridge->Direct evaluate_backend failed (eval=%d produced=%d)\n",
                             eval_f2, nr::pe::d3d12_passthrough::backend_produced() ? 1 : 0);
                return 1;
            }
        }

        // Repeat submission of held cut_cl without Reset
        const nr::gpu::Ticket t_rep = nr::pe::d3d12_direct::book(cut_cl);
        auto snap2 = nr::pe::split::snapshot(cut_cl);
        nr::pe::split::Job job2{[](ID3D12CommandQueue*) {}, [] {}};
        if (!snap2 || !nr::pe::split::cut(cut_cl, std::move(job2), snap2, &why)) {
            std::fprintf(stderr, "FAIL: Case F2 repeat split::cut: %s\n", why.c_str());
            return 1;
        }
        cut_cl->Close();
        {
            ID3D12CommandList* ls[] = {cut_cl};
            queue->ExecuteCommandLists(1, ls);
            wait_gpu();
            queue->Wait(mf, 3);
            queue->ExecuteCommandLists(1, ls);
        }
        cut_cl->Reset(ma1_c, nullptr);
        Sleep(40);
        if (nr::pe::d3d12_direct::completed(t_rep)) {
            mf->Signal(3);
            wait_gpu();
            std::fprintf(stderr, "FAIL: Case F2 repeat submission completed before 2nd fence signaled\n");
            return 1;
        }
        mf->Signal(3);
        wait_gpu();
        if (!nr::pe::d3d12_direct::completed(t_rep)) {
            std::fprintf(stderr, "FAIL: Case F2 repeat submission did not complete after 2nd fence signaled\n");
            return 1;
        }
        cut_cl->Close();
        dir_cl->Close();
        dir_cl->Release();
        cut_cl->Release();
        ma1_c->Release();
        ma2_b->Release();
        ma1_b->Release();
        ma2->Release();
        ma1->Release();
        mf->Release();
        std::printf("[PASS] Case F2: Mixed split+Direct batch & repeat cut submission tracking verified\n");
    }

    // ---- Case H: Crash watch LastError/UEF preservation + dxgi-standalone export check ----
    {
        LPTOP_LEVEL_EXCEPTION_FILTER orig_uef = SetUnhandledExceptionFilter(&test_prev_uef);
        nr::pe::install_crash_watch(nullptr);
        HANDLE catcher = AddVectoredExceptionHandler(0, &test_veh_catcher);
        g_veh_seen_err = 0;
        g_veh_calls = 0;
        SetLastError(0x0BADF00Du);
        RaiseException(0xE0123456u, 0, 0, nullptr);
        const DWORD err_after = GetLastError();
        RemoveVectoredExceptionHandler(catcher);
        nr::pe::remove_crash_watch();
        LPTOP_LEVEL_EXCEPTION_FILTER restored_uef = SetUnhandledExceptionFilter(orig_uef);
        if (err_after != 0x0BADF00Du || g_veh_calls != 1 || g_veh_seen_err != 0x0BADF00Du ||
            restored_uef != &test_prev_uef) {
            std::fprintf(stderr, "FAIL: Case H crash watch preservation err=0x%lx calls=%d seen=0x%lx uef=%d\n",
                         err_after, g_veh_calls, g_veh_seen_err, restored_uef == &test_prev_uef);
            return 1;
        }

        int standalone_ok = 0;
        wchar_t exe_dir[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
        if (wchar_t* slash = std::wcsrchr(exe_dir, L'\\')) *(slash + 1) = L'\0';
        const std::wstring standalone_path = std::wstring(exe_dir) + L"dxgi-standalone.dll";
        if (GetFileAttributesW(standalone_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            HMODULE hs = LoadLibraryW(standalone_path.c_str());
            if (!hs || !GetProcAddress(hs, "CreateDXGIFactory") ||
                !GetProcAddress(hs, "CreateDXGIFactory1") || !GetProcAddress(hs, "CreateDXGIFactory2")) {
                if (hs) FreeLibrary(hs);
                std::fprintf(stderr, "FAIL: Case H dxgi-standalone.dll load/export check failed\n");
                return 1;
            }
            FreeLibrary(hs);
            standalone_ok = 1;
        }
        std::printf("[PASS] Case H: Crash watch LastError & UEF preserved, dxgi-standalone=%d\n", standalone_ok);
    }

    // ---- Case I: Writable Directory Fallback (Read-Only App Dir -> LOCALAPPDATA) ----
    {
        wchar_t tmp_w[MAX_PATH * 4]{};
        const DWORD tn = GetTempPathW(MAX_PATH * 4, tmp_w);
        if (!tn || tn >= MAX_PATH * 4) {
            std::fprintf(stderr, "FAIL: Case I GetTempPathW\n");
            return 1;
        }
        std::string base = nr::u8::narrow(tmp_w);
        while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();
        base += "\\nr_case_i_" + std::to_string(GetCurrentProcessId());
        const std::string wdir = base + "\\writable";
        if (!nr::u8::make_dirs(wdir.c_str()) || !nr::u8::dir_writable(wdir.c_str())) {
            std::fprintf(stderr, "FAIL: Case I writable dir setup '%s'\n", wdir.c_str());
            return 1;
        }
        bool fb_w = true;
        const std::string wc = nr::u8::writable_cache_root(wdir, &fb_w);
        if (fb_w || wc != wdir) {
            std::fprintf(stderr, "FAIL: Case I writable_cache_root fell back on writable dir (%d, '%s')\n",
                         fb_w ? 1 : 0, wc.c_str());
            return 1;
        }
        const std::string wa = nr::u8::writable_app_dir(wdir, &fb_w);
        if (fb_w || wa != wdir) {
            std::fprintf(stderr, "FAIL: Case I writable_app_dir fell back on writable dir (%d, '%s')\n",
                         fb_w ? 1 : 0, wa.c_str());
            return 1;
        }

        // Create a regular file named `blocker` so `blocker\ro_child` is deterministically non-writable/non-creatable
        // even under elevated tokens.
        const std::string blocker = base + "\\blocker";
        FILE* bf = nr::u8::fopen(blocker.c_str(), "wb");
        if (!bf) {
            std::fprintf(stderr, "FAIL: Case I blocker file create\n");
            return 1;
        }
        std::fputs("ro", bf);
        std::fclose(bf);
        const std::string ro_dir = blocker + "\\ro_child";
        if (nr::u8::dir_writable(ro_dir.c_str())) {
            std::fprintf(stderr, "FAIL: Case I dir_writable returned true on blocker child '%s'\n", ro_dir.c_str());
            return 1;
        }
        bool fb_ro_c = false;
        const std::string rc = nr::u8::writable_cache_root(ro_dir, &fb_ro_c);
        const std::string rc2 = nr::u8::writable_cache_root(ro_dir, &fb_ro_c);
        if (!fb_ro_c || rc != rc2 || rc.find("\\cache") == std::string::npos || !nr::u8::dir_writable(rc.c_str())) {
            std::fprintf(stderr, "FAIL: Case I writable_cache_root fallback failed (fb=%d rc='%s')\n",
                         fb_ro_c ? 1 : 0, rc.c_str());
            return 1;
        }
        bool fb_ro_a = false;
        const std::string ra = nr::u8::writable_app_dir(ro_dir, &fb_ro_a);
        const std::string ra2 = nr::u8::writable_app_dir(ro_dir, &fb_ro_a);
        if (!fb_ro_a || ra != ra2 || ra.find("\\apps\\") == std::string::npos || !nr::u8::dir_writable(ra.c_str())) {
            std::fprintf(stderr, "FAIL: Case I writable_app_dir fallback failed (fb=%d ra='%s')\n",
                         fb_ro_a ? 1 : 0, ra.c_str());
            return 1;
        }

        bool fb_ini_w = true;
        const std::string wi = nr::u8::writable_ini_path(wdir, &fb_ini_w);
        if (fb_ini_w || wi != (wdir + "\\dlssnr-amd.ini")) {
            std::fprintf(stderr, "FAIL: Case I writable_ini_path fell back on writable dir (%d, '%s')\n",
                         fb_ini_w ? 1 : 0, wi.c_str());
            return 1;
        }
        bool fb_ini_ro = false;
        const std::string ri = nr::u8::writable_ini_path(ro_dir, &fb_ini_ro);
        const std::string ri2 = nr::u8::writable_ini_path(ro_dir, &fb_ini_ro);
        if (!fb_ini_ro || ri != ri2 || ri != (ra + "\\dlssnr-amd.ini")) {
            std::fprintf(stderr, "FAIL: Case I writable_ini_path fallback failed (fb=%d ri='%s' exp='%s\\dlssnr-amd.ini')\n",
                         fb_ini_ro ? 1 : 0, ri.c_str(), ra.c_str());
            return 1;
        }

        // Verify seed_writable_ini seeds from read-only source, clears FILE_ATTRIBUTE_READONLY,
        // works with nr::ini_edit::Ini::save(), and does not overwrite existing edited settings.
        const std::string seed_src = base + "\\seed_src";
        const std::string src_ini = seed_src + "\\dlssnr-amd.ini";
        if (!nr::u8::make_dirs(seed_src.c_str())) {
            std::fprintf(stderr, "FAIL: Case I seed_src dir setup\n");
            return 1;
        }
        if (FILE* sf = nr::u8::fopen(src_ini.c_str(), "wb")) {
            std::fputs("[Network]\r\nModel = CC_SMALL_FP8_EXPERIMENTAL\r\n", sf);
            std::fclose(sf);
        } else {
            std::fprintf(stderr, "FAIL: Case I seed_src ini create\n");
            return 1;
        }
        SetFileAttributesW(nr::u8::wide(src_ini).c_str(), FILE_ATTRIBUTE_READONLY);
        DeleteFileW(nr::u8::wide(ri).c_str());
        if (!nr::u8::seed_writable_ini(seed_src, ri)) {
            std::fprintf(stderr, "FAIL: Case I seed_writable_ini failed for '%s' -> '%s'\n",
                         src_ini.c_str(), ri.c_str());
            return 1;
        }
        const DWORD dst_attr = nr::u8::attributes(ri);
        if (dst_attr == INVALID_FILE_ATTRIBUTES || (dst_attr & FILE_ATTRIBUTE_READONLY) != 0) {
            std::fprintf(stderr, "FAIL: Case I seeded ini invalid or still read-only (0x%lx)\n",
                         static_cast<unsigned long>(dst_attr));
            return 1;
        }
        char mod_buf[96]{};
        nr::u8::profile("Network", "Model", "", mod_buf, sizeof(mod_buf), ri);
        if (std::strcmp(mod_buf, "CC_SMALL_FP8_EXPERIMENTAL") != 0) {
            std::fprintf(stderr, "FAIL: Case I seeded ini model='%s' expected CC_SMALL_FP8_EXPERIMENTAL\n", mod_buf);
            return 1;
        }
        {
            nr::ini_edit::Ini editor{nr::u8::path(ri)};
            if (!editor.refresh()) {
                std::fprintf(stderr, "FAIL: Case I Ini::refresh failed on '%s'\n", ri.c_str());
                return 1;
            }
            editor.set("Network", "Model", "CC_SILVER_AARDWOLD_OPTIMIZED");
            editor.save();
        }
        nr::u8::profile("Network", "Model", "", mod_buf, sizeof(mod_buf), ri);
        if (std::strcmp(mod_buf, "CC_SILVER_AARDWOLD_OPTIMIZED") != 0) {
            std::fprintf(stderr, "FAIL: Case I Ini::save did not persist Model='%s'\n", mod_buf);
            return 1;
        }
        if (nr::u8::seed_writable_ini(seed_src, ri)) {
            std::fprintf(stderr, "FAIL: Case I seed_writable_ini overwrote existing ini '%s'\n", ri.c_str());
            return 1;
        }
        nr::u8::profile("Network", "Model", "", mod_buf, sizeof(mod_buf), ri);
        if (std::strcmp(mod_buf, "CC_SILVER_AARDWOLD_OPTIMIZED") != 0) {
            std::fprintf(stderr, "FAIL: Case I edited ini was clobbered after second seed ('%s')\n", mod_buf);
            return 1;
        }

        // Verify fallback seeding from dlssnr-amd\dlssnr-amd.default.ini when dlssnr-amd.ini is absent.
        const std::string seed_def_src = base + "\\seed_def_src";
        const std::string seed_def_sub = seed_def_src + "\\dlssnr-amd";
        const std::string seed_def_ini = seed_def_sub + "\\dlssnr-amd.default.ini";
        const std::string ri_def = ra + "\\dlssnr-amd.default-test.ini";
        if (!nr::u8::make_dirs(seed_def_sub.c_str())) {
            std::fprintf(stderr, "FAIL: Case I seed_def_sub dir setup\n");
            return 1;
        }
        if (FILE* df = nr::u8::fopen(seed_def_ini.c_str(), "wb")) {
            std::fputs("[Network]\r\nModel = CC_SILVER_AARDWOLD\r\n", df);
            std::fclose(df);
        } else {
            std::fprintf(stderr, "FAIL: Case I seed_def_ini create\n");
            return 1;
        }
        SetFileAttributesW(nr::u8::wide(seed_def_ini).c_str(), FILE_ATTRIBUTE_READONLY);
        DeleteFileW(nr::u8::wide(ri_def).c_str());
        if (!nr::u8::seed_writable_ini(seed_def_src, ri_def)) {
            std::fprintf(stderr, "FAIL: Case I default.ini fallback seed failed\n");
            return 1;
        }
        const DWORD def_dst_attr = nr::u8::attributes(ri_def);
        if (def_dst_attr == INVALID_FILE_ATTRIBUTES || (def_dst_attr & FILE_ATTRIBUTE_READONLY) != 0) {
            std::fprintf(stderr, "FAIL: Case I seeded default ini invalid or still read-only (0x%lx)\n",
                         static_cast<unsigned long>(def_dst_attr));
            return 1;
        }
        nr::u8::profile("Network", "Model", "", mod_buf, sizeof(mod_buf), ri_def);
        if (std::strcmp(mod_buf, "CC_SILVER_AARDWOLD") != 0) {
            std::fprintf(stderr, "FAIL: Case I default.ini model='%s' expected CC_SILVER_AARDWOLD\n", mod_buf);
            return 1;
        }

        DeleteFileW(nr::u8::wide(ri_def).c_str());
        DeleteFileW(nr::u8::wide(ri).c_str());
        RemoveDirectoryW(nr::u8::wide(ra).c_str());
        nr::u8::clear_readonly(seed_def_ini);
        DeleteFileW(nr::u8::wide(seed_def_ini).c_str());
        RemoveDirectoryW(nr::u8::wide(seed_def_sub).c_str());
        RemoveDirectoryW(nr::u8::wide(seed_def_src).c_str());
        nr::u8::clear_readonly(src_ini);
        DeleteFileW(nr::u8::wide(src_ini).c_str());
        RemoveDirectoryW(nr::u8::wide(seed_src).c_str());
        DeleteFileW(nr::u8::wide(blocker).c_str());
        RemoveDirectoryW(nr::u8::wide(wdir).c_str());
        RemoveDirectoryW(nr::u8::wide(base).c_str());
        std::printf("[PASS] Case I: Writable directory & ini fallback verified (cache='%s', app='%s', ini='%s')\n",
                    rc.c_str(), ra.c_str(), ri.c_str());
    }

    // ---- Case J: D3D11 -> D3D12 Synchronous Bridge + RenoDX+NR Evaluation + LRU Slots + No-Deadlock Guard ----
    {
        ID3D11Device* d11_dev = nullptr;
        ID3D11DeviceContext* d11_ctx = nullptr;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fls, 2, D3D11_SDK_VERSION,
                                     &d11_dev, &fl, &d11_ctx)) || !d11_dev || !d11_ctx) {
            std::fprintf(stderr, "FAIL: Case J D3D11CreateDevice\n");
            return 1;
        }

        constexpr uint32_t jw = 256, jh = 256;
        std::vector<uint8_t> init_rgba(size_t(jw) * jh * 4);
        for (uint32_t y = 0; y < jh; ++y) {
            for (uint32_t x = 0; x < jw; ++x) {
                const size_t idx = (size_t(y) * jw + x) * 4;
                init_rgba[idx + 0] = static_cast<uint8_t>((x * 3 + y) & 0xFFu);
                init_rgba[idx + 1] = static_cast<uint8_t>((x + y * 5) & 0xFFu);
                init_rgba[idx + 2] = static_cast<uint8_t>(((x ^ y) * 7) & 0xFFu);
                init_rgba[idx + 3] = 255;
            }
        }
        auto make_d11_tex = [&](uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool staging, const void* init, UINT pitch) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = fmt;
            td.SampleDesc.Count = 1;
            td.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
            td.BindFlags = staging ? 0 : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            td.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
            D3D11_SUBRESOURCE_DATA sd{init, pitch, 0};
            ID3D11Texture2D* t = nullptr;
            if (FAILED(d11_dev->CreateTexture2D(&td, init ? &sd : nullptr, &t))) return static_cast<ID3D11Texture2D*>(nullptr);
            return t;
        };

        ID3D11Texture2D* tex11 = make_d11_tex(jw, jh, DXGI_FORMAT_R8G8B8A8_UNORM, false, init_rgba.data(), jw * 4);
        ID3D11Texture2D* hud11 = make_d11_tex(jw, jh, DXGI_FORMAT_R8G8B8A8_UNORM, false, init_rgba.data(), jw * 4);
        ID3D11Texture2D* rb11 = make_d11_tex(jw, jh, DXGI_FORMAT_R8G8B8A8_UNORM, true, nullptr, 0);
        if (!tex11 || !hud11 || !rb11) {
            std::fprintf(stderr, "FAIL: Case J D3D11 texture creation\n");
            return 1;
        }

        nr::pe::d11_bridge::Bridge bridge;
        ID3D12CommandAllocator* j_alloc = nullptr;
        ID3D12GraphicsCommandList* j_cmd = nullptr;
        ID3D12Resource* j_uav = nullptr;
        ID3D12Resource* j_hud_copy = nullptr;
        ID3D12Fence* j_wait_fence = nullptr;
        ID3D12Device* j_d12_dev = nullptr;
        ID3D12CommandQueue* j_d12_queue = nullptr;
        HANDLE j_wait_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        uint64_t j_wait_val = 0;

        nr::renodx::Settings r_j{};
        r_j.nr_enabled = true;
        r_j.passes = 1;
        r_j.codec_mode = 1;
        r_j.transfer_strength = 0.8f;

        bool produced_ok = false;
        bool hud_copied = false;
        for (int poll = 0; poll < 600; ++poll) {
            int eval_waiting = 0;
            bool eval_ran = false;
            const auto st = bridge.execute(d11_dev, tex11, hud11, [&](nr::pe::d11_bridge::FrameContext& fc) {
                if (!fc.device || !fc.queue || !fc.back || !fc.signal_out_fence) return;
                if (!j_alloc) {
                    j_d12_dev = fc.device;
                    j_d12_queue = fc.queue;
                    j_d12_dev->AddRef();
                    j_d12_queue->AddRef();
                    if (FAILED(fc.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&j_alloc))) ||
                        FAILED(fc.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, j_alloc, nullptr, IID_PPV_ARGS(&j_cmd))) ||
                        FAILED(fc.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&j_wait_fence)))) {
                        return;
                    }
                    j_cmd->Close();
                    j_uav = make_tex2d(fc.device, jw, jh, DXGI_FORMAT_R8G8B8A8_UNORM, true,
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    j_hud_copy = make_tex2d(fc.device, jw, jh, DXGI_FORMAT_R8G8B8A8_UNORM, false,
                                            D3D12_RESOURCE_STATE_COPY_DEST);
                    if (!j_uav || !j_hud_copy) return;
                }
                if (FAILED(j_alloc->Reset()) || FAILED(j_cmd->Reset(j_alloc, nullptr))) return;

                auto b = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
                    if (before == after) return;
                    D3D12_RESOURCE_BARRIER rb{};
                    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    rb.Transition.pResource = r;
                    rb.Transition.StateBefore = before;
                    rb.Transition.StateAfter = after;
                    rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    j_cmd->ResourceBarrier(1, &rb);
                };

                b(fc.back, fc.back_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                b(j_uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                j_cmd->CopyResource(j_uav, fc.back);
                b(j_uav, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (fc.hudless) {
                    b(fc.hudless, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    j_cmd->CopyResource(j_hud_copy, fc.hudless);
                    b(fc.hudless, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                    hud_copied = true;
                }

                nr::dlssnr::D3d12BackendFrame bf{};
                bf.device = fc.device;
                bf.cmd = j_cmd;
                bf.color = j_uav;
                bf.color_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                bf.output = j_uav;
                bf.output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                bf.width = jw;
                bf.height = jh;
                bf.intensity = 1.0f;
                bf.local_tone = 1.0f;
                bf.local_structure = 1.0f;
                bf.skin_structure = -1.0f;
                bf.history_f16 = 1;
                bf.feature = 0x7711u;
                bf.history_strength = -1.0f;
                bf.renodx_route = 1;
                bf.renodx_present = 1;
                bf.renodx_white_nits = 203.0f;
                bf.renodx_settings = &r_j;
                bf.waiting = &eval_waiting;
                eval_ran = nr::pe::d3d12_passthrough::evaluate_backend(bf);

                b(j_uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                b(fc.back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                j_cmd->CopyResource(fc.back, j_uav);
                b(fc.back, D3D12_RESOURCE_STATE_COPY_DEST, fc.back_state);
                b(j_uav, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (FAILED(j_cmd->Close())) return;

                ID3D12CommandList* lists[] = {j_cmd};
                fc.queue->ExecuteCommandLists(1, lists);
                fc.submitted = true;
                if (*fc.signal_out_fence) {
                    fc.output_signal = fc.queue->Signal(*fc.signal_out_fence, fc.signal_out_val);
                }
            });
            if (j_d12_queue && j_wait_fence && j_wait_ev) {
                if (SUCCEEDED(j_d12_queue->Signal(j_wait_fence, ++j_wait_val)) &&
                    j_wait_fence->GetCompletedValue() < j_wait_val) {
                    ResetEvent(j_wait_ev);
                    if (SUCCEEDED(j_wait_fence->SetEventOnCompletion(j_wait_val, j_wait_ev))) {
                        WaitForSingleObject(j_wait_ev, 5000);
                    }
                }
            }
            if (st == nr::pe::d11_bridge::Status::kFallback) {
                std::fprintf(stderr, "FAIL: Case J bridge returned kFallback unexpectedly (%s)\n",
                             bridge.last_error().c_str());
                return 1;
            }
            if (st == nr::pe::d11_bridge::Status::kRan && eval_ran &&
                nr::pe::d3d12_passthrough::backend_produced()) {
                produced_ok = true;
                break;
            }
            Sleep(20);
        }
        if (!produced_ok || !hud_copied) {
            std::fprintf(stderr, "FAIL: Case J D3D11->D3D12 evaluate_backend failed (produced=%d hud=%d)\n",
                         produced_ok ? 1 : 0, hud_copied ? 1 : 0);
            return 1;
        }

        // Read back tex11 on D3D11 and confirm synchronous GPU copy modified pixels vs initial pattern.
        d11_ctx->CopyResource(rb11, tex11);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(d11_ctx->Map(rb11, 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
            std::fprintf(stderr, "FAIL: Case J D3D11 staging map failed\n");
            return 1;
        }
        size_t diff_pixels = 0;
        const auto* out_ptr = static_cast<const uint8_t*>(mapped.pData);
        for (uint32_t y = 0; y < jh; ++y) {
            const uint8_t* row = out_ptr + size_t(y) * mapped.RowPitch;
            const uint8_t* ref = init_rgba.data() + size_t(y) * jw * 4;
            for (uint32_t x = 0; x < jw; ++x) {
                if (row[x * 4 + 0] != ref[x * 4 + 0] || row[x * 4 + 1] != ref[x * 4 + 1] ||
                    row[x * 4 + 2] != ref[x * 4 + 2]) {
                    ++diff_pixels;
                }
            }
        }
        d11_ctx->Unmap(rb11, 0);
        if (diff_pixels == 0) {
            std::fprintf(stderr, "FAIL: Case J D3D11 output matched input byte-for-byte after NR run\n");
            return 1;
        }

        // A callback returning without submitting never waits on the output fence.
        d11_ctx->UpdateSubresource(tex11, 0, nullptr, init_rgba.data(), jw * 4, 0);
        const auto st_noop = bridge.execute(d11_dev, tex11, nullptr, [](nr::pe::d11_bridge::FrameContext&) {
            // Deliberately return without executing or signalling.
        });
        if (st_noop != nr::pe::d11_bridge::Status::kRan) {
            std::fprintf(stderr, "FAIL: Case J early-return no-op returned status %d\n", int(st_noop));
            return 1;
        }
        d11_ctx->CopyResource(rb11, tex11);
        if (FAILED(d11_ctx->Map(rb11, 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
            std::fprintf(stderr, "FAIL: Case J no-op readback Map failed\n");
            return 1;
        }
        bool noop_intact = true;
        const auto* noop_ptr = static_cast<const uint8_t*>(mapped.pData);
        for (uint32_t y = 0; y < jh && noop_intact; ++y) {
            if (std::memcmp(noop_ptr + size_t(y) * mapped.RowPitch,
                            init_rgba.data() + size_t(y) * jw * 4, jw * 4) != 0) {
                noop_intact = false;
            }
        }
        d11_ctx->Unmap(rb11, 0);
        if (!noop_intact) {
            std::fprintf(stderr, "FAIL: Case J early-return modified tex11 unexpectedly\n");
            return 1;
        }

        // Hold the D3D12 queue behind a CPU-controlled fence and omit the output Signal.
        // No D3D11 slot access (including on the next Present) is safe until the queue catches up.
        const uint64_t unblock = ++j_wait_val;
        const auto missed = bridge.execute(d11_dev, tex11, hud11, [&](nr::pe::d11_bridge::FrameContext& fc) {
            fc.submitted = SUCCEEDED(fc.queue->Wait(j_wait_fence, unblock));
        });
        bool reused = false;
        const auto held = bridge.execute(d11_dev, tex11, hud11, [&](nr::pe::d11_bridge::FrameContext&) { reused = true; });
        if (FAILED(j_wait_fence->Signal(unblock)) || reused ||
            missed != nr::pe::d11_bridge::Status::kPassthrough || held != nr::pe::d11_bridge::Status::kPassthrough) {
            std::fprintf(stderr, "FAIL: Case J unsignalled output did not hold the bridge (%d, %d, reused=%d)\n",
                         int(missed), int(held), int(reused));
            return 1;
        }
        bool recovered = false;
        for (int i = 0; !recovered && i < 500; ++i) {
            const auto st = bridge.execute(d11_dev, tex11, hud11, [&](nr::pe::d11_bridge::FrameContext&) { recovered = true; });
            if (st == nr::pe::d11_bridge::Status::kFallback) break;
            if (!recovered) Sleep(1);
        }
        if (!recovered) {
            std::fprintf(stderr, "FAIL: Case J D3D12 bridge did not resume after the queue completed\n");
            return 1;
        }

        // Test LRU eviction (kMaxSlots = 4) across 5 distinct (w, h, fmt) combinations.
        const struct { uint32_t w, h; DXGI_FORMAT fmt; } combos[] = {
            {128, 128, DXGI_FORMAT_R8G8B8A8_UNORM},
            {96, 96, DXGI_FORMAT_B8G8R8A8_UNORM},
            {64, 64, DXGI_FORMAT_R16G16B16A16_FLOAT},
            {80, 80, DXGI_FORMAT_R8G8B8A8_UNORM},
            {48, 48, DXGI_FORMAT_R8G8B8A8_UNORM},
        };
        for (const auto& c : combos) {
            ID3D11Texture2D* ct = make_d11_tex(c.w, c.h, c.fmt, false, nullptr, 0);
            if (!ct) {
                std::fprintf(stderr, "FAIL: Case J resize tex (%ux%u fmt=%d)\n", c.w, c.h, int(c.fmt));
                return 1;
            }
            const auto cs = bridge.execute(d11_dev, ct, nullptr, [](nr::pe::d11_bridge::FrameContext&) {});
            ct->Release();
            if (cs != nr::pe::d11_bridge::Status::kRan) {
                std::fprintf(stderr, "FAIL: Case J resize bridge status=%d (%ux%u fmt=%d)\n", int(cs), c.w, c.h, int(c.fmt));
                return 1;
            }
        }
        if (bridge.cached_slot_count() != 4) {
            std::fprintf(stderr, "FAIL: Case J LRU slot count=%u (expected 4)\n", bridge.cached_slot_count());
            return 1;
        }

        // Test NR_D11_D3D12_BRIDGE=0 escape hatch: read when a device's route is chosen, so on a bridge that has
        // not met the device yet.
        bridge.reset();
        SetEnvironmentVariableA("NR_D11_D3D12_BRIDGE", "0");
        const auto st_off = bridge.execute(d11_dev, tex11, nullptr, [](nr::pe::d11_bridge::FrameContext&) {});
        SetEnvironmentVariableA("NR_D11_D3D12_BRIDGE", nullptr);
        if (st_off != nr::pe::d11_bridge::Status::kFallback) {
            std::fprintf(stderr, "FAIL: Case J NR_D11_D3D12_BRIDGE=0 returned status %d (expected kFallback)\n", int(st_off));
            return 1;
        }

        if (j_d12_queue && j_wait_fence && j_wait_ev) {
            if (SUCCEEDED(j_d12_queue->Signal(j_wait_fence, ++j_wait_val))) {
                ResetEvent(j_wait_ev);
                if (j_wait_fence->GetCompletedValue() < j_wait_val &&
                    SUCCEEDED(j_wait_fence->SetEventOnCompletion(j_wait_val, j_wait_ev))) {
                    WaitForSingleObject(j_wait_ev, 5000);
                }
            }
        }
        bridge.reset();
        if (j_wait_ev) CloseHandle(j_wait_ev);
        if (j_wait_fence) j_wait_fence->Release();
        if (j_hud_copy) j_hud_copy->Release();
        if (j_uav) j_uav->Release();
        if (j_cmd) j_cmd->Release();
        if (j_alloc) j_alloc->Release();
        if (j_d12_queue) j_d12_queue->Release();
        if (j_d12_dev) j_d12_dev->Release();
        rb11->Release();
        hud11->Release();
        tex11->Release();
        d11_ctx->Release();
        d11_dev->Release();
        std::printf("[PASS] Case J: D3D11->D3D12 bridge NR inference (diff_px=%zu), hudless copy, LRU=4, no-deadlock & escape hatch\n",
                    diff_pixels);
    }

    // ---- Case J2: the bridge's handoff by role (an evaluate's textures), the route's latch, a second device ----
    {
        namespace d11b = nr::pe::d11_bridge;
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        ID3D11Device* dev2 = nullptr;
        ID3D11DeviceContext* ctx2 = nullptr;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fls, 2, D3D11_SDK_VERSION, &dev, &fl, &ctx)) ||
            FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fls, 2, D3D11_SDK_VERSION, &dev2, &fl, &ctx2)) ||
            !dev || !ctx || !dev2 || !ctx2) {
            std::fprintf(stderr, "FAIL: Case J2 D3D11CreateDevice\n");
            return 1;
        }
        constexpr uint32_t cw = 128, ch = 96, rx = 16, ry = 8, rw = 64, rh = 32, ow = 160, oh = 120;
        std::vector<uint8_t> colour_px(size_t(cw) * ch * 4), output_px(size_t(ow) * oh * 4);
        for (uint32_t y = 0; y < ch; ++y)
            for (uint32_t x = 0; x < cw; ++x) {
                uint8_t* px = &colour_px[(size_t(y) * cw + x) * 4];
                px[0] = uint8_t(x * 2 + 1); px[1] = uint8_t(y * 2 + 3); px[2] = uint8_t(x ^ y); px[3] = 255;
            }
        for (size_t i = 0; i < output_px.size(); ++i) output_px[i] = uint8_t(0x40 + (i & 0x3F));
        const auto make = [](ID3D11Device* d, uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind, bool staging,
                             const void* init, UINT pitch) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt; td.SampleDesc.Count = 1;
            td.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
            td.BindFlags = staging ? 0 : bind;
            td.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
            D3D11_SUBRESOURCE_DATA sd{init, pitch, 0};
            ID3D11Texture2D* t = nullptr;
            if (FAILED(d->CreateTexture2D(&td, init ? &sd : nullptr, &t))) t = nullptr;
            return t;
        };
        const UINT colour_bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* colour = make(dev, cw, ch, DXGI_FORMAT_R8G8B8A8_UNORM, colour_bind, false, colour_px.data(), cw * 4);
        ID3D11Texture2D* mine = make(dev, rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, false, nullptr, 0);
        ID3D11Texture2D* output = make(dev, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, colour_bind, false, output_px.data(), ow * 4);
        ID3D11Texture2D* depth = make(dev, cw, ch, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE,
                                      false, nullptr, 0);
        ID3D11Texture2D* motion = make(dev, cw, ch, DXGI_FORMAT_R16G16_FLOAT, colour_bind, false, nullptr, 0);
        ID3D11Texture2D* depth_out = make(dev, cw, ch, DXGI_FORMAT_R32_FLOAT, colour_bind, false, nullptr, 0);
        ID3D11Texture2D* rb_colour = make(dev, cw, ch, DXGI_FORMAT_R8G8B8A8_UNORM, 0, true, nullptr, 0);
        ID3D11Texture2D* rb_mine = make(dev, rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, 0, true, nullptr, 0);
        ID3D11Texture2D* rb_output = make(dev, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, 0, true, nullptr, 0);
        ID3D11Texture2D* rb_depth = make(dev, cw, ch, DXGI_FORMAT_R32_FLOAT, 0, true, nullptr, 0);
        ID3D11Texture2D* back2 = make(dev2, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, colour_bind, false, nullptr, 0);
        ID3D11DepthStencilView* dsv = nullptr;
        if (depth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
            dd.Format = DXGI_FORMAT_D32_FLOAT;
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            dev->CreateDepthStencilView(depth, &dd, &dsv);
        }
        if (!colour || !mine || !output || !depth || !motion || !depth_out || !rb_colour || !rb_mine || !rb_output ||
            !rb_depth || !back2 || !dsv) {
            std::fprintf(stderr, "FAIL: Case J2 D3D11 texture creation\n");
            return 1;
        }
        constexpr float kDepth = 0.25f;
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, kDepth, 0);

        d11b::Bridge bridge;
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* cmd = nullptr;
        // On D3D12: the colour's region into the output's corner, the depth (R32_FLOAT there) into `depth_out`.
        DXGI_FORMAT depth_fmt12 = DXGI_FORMAT_UNKNOWN;
        uint64_t colour_w12 = 0;
        uint32_t colour_h12 = 0;
        bool all_roles = false;
        const auto work = [&](d11b::FrameContext& fc) {
            ID3D12Resource* const c = fc.find(d11b::Role::kColour);
            ID3D12Resource* const o = fc.find(d11b::Role::kOutput);
            ID3D12Resource* const d = fc.find(d11b::Role::kDepth);
            ID3D12Resource* const m = fc.find(d11b::Role::kMotion);
            ID3D12Resource* const e = fc.find(d11b::Role::kExposure);
            all_roles = c && o && d && m && e;
            if (!all_roles) return;
            depth_fmt12 = d->GetDesc().Format;
            colour_w12 = c->GetDesc().Width;
            colour_h12 = c->GetDesc().Height;
            if (!cmd) {
                if (FAILED(fc.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
                    FAILED(fc.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd))))
                    return;
            } else if (FAILED(alloc->Reset()) || FAILED(cmd->Reset(alloc, nullptr))) {
                return;   // (the bridge waited for the list's last run before it came back)
            }
            const D3D12_RESOURCE_STATES common = D3D12_RESOURCE_STATE_COMMON;
            barrier_transition(cmd, c, common, D3D12_RESOURCE_STATE_COPY_SOURCE);
            barrier_transition(cmd, d, common, D3D12_RESOURCE_STATE_COPY_SOURCE);
            barrier_transition(cmd, o, common, D3D12_RESOURCE_STATE_COPY_DEST);
            barrier_transition(cmd, e, common, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = o; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = c; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            cmd->CopyResource(e, d);
            barrier_transition(cmd, e, D3D12_RESOURCE_STATE_COPY_DEST, common);
            barrier_transition(cmd, o, D3D12_RESOURCE_STATE_COPY_DEST, common);
            barrier_transition(cmd, d, D3D12_RESOURCE_STATE_COPY_SOURCE, common);
            barrier_transition(cmd, c, D3D12_RESOURCE_STATE_COPY_SOURCE, common);
            if (FAILED(cmd->Close())) return;
            ID3D12CommandList* lists[] = {cmd};
            fc.queue->ExecuteCommandLists(1, lists);
            fc.submitted = true;
            if (*fc.signal_out_fence) fc.output_signal = fc.queue->Signal(*fc.signal_out_fence, fc.signal_out_val);
        };
        const auto handoff = [&](d11b::Handoff* items) {
            items[0] = {}; items[0].role = d11b::Role::kColour; items[0].tex11 = colour;
            items[0].x = rx; items[0].y = ry; items[0].w = rw; items[0].h = rh;
            items[0].back = true; items[0].back_to = mine; items[0].writable = true;
            items[1] = {}; items[1].role = d11b::Role::kOutput; items[1].tex11 = output; items[1].back = true; items[1].writable = true;
            items[2] = {}; items[2].role = d11b::Role::kDepth; items[2].tex11 = depth; items[2].depth = true;
            items[3] = {}; items[3].role = d11b::Role::kMotion; items[3].tex11 = motion;
            // (the depth as D3D12 saw it comes back in this one)
            items[4] = {}; items[4].role = d11b::Role::kExposure; items[4].tex11 = depth_out; items[4].in = false; items[4].back = true;
        };
        d11b::Handoff items[5];
        handoff(items);
        d11b::Status st = d11b::Status::kPassthrough;
        for (int attempt = 0; attempt < 240 && st != d11b::Status::kRan; ++attempt) {
            st = bridge.execute(dev, items, 5, false, work);
            if (st == d11b::Status::kFallback) break;
            if (st != d11b::Status::kRan) Sleep(50);
        }
        if (st != d11b::Status::kRan || !all_roles) {
            std::fprintf(stderr, "FAIL: Case J2 handoff by role status=%d all_roles=%d (%s)\n", int(st), int(all_roles),
                         bridge.last_error().c_str());
            return 1;
        }
        if (depth_fmt12 != DXGI_FORMAT_R32_FLOAT || colour_w12 != rw || colour_h12 != rh) {
            std::fprintf(stderr, "FAIL: Case J2 shared textures: depth fmt=%d (R32_FLOAT expected), colour %llux%u (%ux%u expected)\n",
                         int(depth_fmt12), static_cast<unsigned long long>(colour_w12), colour_h12, rw, rh);
            return 1;
        }
        const auto read = [&](ID3D11Texture2D* rb, ID3D11Texture2D* tex, uint32_t w, uint32_t h, uint32_t bpp, std::vector<uint8_t>& out) {
            ctx->CopyResource(rb, tex);
            D3D11_MAPPED_SUBRESOURCE map{};
            if (FAILED(ctx->Map(rb, 0, D3D11_MAP_READ, 0, &map)) || !map.pData) return false;
            out.resize(size_t(w) * h * bpp);
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy(&out[size_t(y) * w * bpp], static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch, size_t(w) * bpp);
            ctx->Unmap(rb, 0);
            return true;
        };
        std::vector<uint8_t> got_colour, got_mine, got_output, got_depth;
        if (!read(rb_colour, colour, cw, ch, 4, got_colour) || !read(rb_mine, mine, rw, rh, 4, got_mine) ||
            !read(rb_output, output, ow, oh, 4, got_output) || !read(rb_depth, depth_out, cw, ch, 4, got_depth)) {
            std::fprintf(stderr, "FAIL: Case J2 D3D11 readback\n");
            return 1;
        }
        size_t bad_mine = 0, bad_out = 0, bad_depth = 0;
        for (uint32_t y = 0; y < rh; ++y)
            for (uint32_t x = 0; x < rw; ++x)
                bad_mine += std::memcmp(&got_mine[(size_t(y) * rw + x) * 4], &colour_px[(size_t(y + ry) * cw + x + rx) * 4], 4) != 0;
        for (uint32_t y = 0; y < oh; ++y)
            for (uint32_t x = 0; x < ow; ++x) {
                const uint8_t* want = x < rw && y < rh ? &colour_px[(size_t(y + ry) * cw + x + rx) * 4]
                                                       : &output_px[(size_t(y) * ow + x) * 4];
                bad_out += std::memcmp(&got_output[(size_t(y) * ow + x) * 4], want, 4) != 0;
            }
        for (size_t i = 0; i < size_t(cw) * ch; ++i) {
            float v = 0.f;
            std::memcpy(&v, &got_depth[i * 4], 4);
            bad_depth += v != kDepth;
        }
        if (got_colour != colour_px || bad_mine || bad_out || bad_depth) {
            std::fprintf(stderr, "FAIL: Case J2 round trip: game's colour %s, region copy bad=%zu, output bad=%zu, depth bad=%zu\n",
                         got_colour == colour_px ? "untouched" : "WRITTEN", bad_mine, bad_out, bad_depth);
            return 1;
        }

        // The latch: an error on the bridge's device is a frame passed through, never the Vulkan route - nor is the
        // escape hatch read again for a device whose route is chosen.
        d11b::Handoff broken[2];
        handoff(items);
        broken[0] = items[0];
        broken[1] = {};
        broken[1].role = d11b::Role::kMotion;   // (no texture)
        int callbacks = 0;
        const d11b::Status st_err = bridge.execute(dev, broken, 2, false, [&](d11b::FrameContext&) { ++callbacks; });
        SetEnvironmentVariableA("NR_D11_D3D12_BRIDGE", "0");
        handoff(items);
        const d11b::Status st_latched = bridge.execute(dev, items, 5, false, work);
        const d11b::Route r_latched = bridge.decide(dev, true);
        SetEnvironmentVariableA("NR_D11_D3D12_BRIDGE", nullptr);
        if (st_err != d11b::Status::kPassthrough || callbacks != 0 || st_latched != d11b::Status::kRan ||
            r_latched != d11b::Route::kBridge) {
            std::fprintf(stderr, "FAIL: Case J2 latch: error status=%d (callbacks=%d), with the hatch set status=%d route=%d\n",
                         int(st_err), callbacks, int(st_latched), int(r_latched));
            return 1;
        }

        // A second D3D11 device: its own route, chosen once; neither device is sent to Vulkan as the bridge moves.
        const d11b::Route r2 = bridge.decide(dev2, false);
        const d11b::Status st2 = bridge.execute(dev2, back2, nullptr, [](d11b::FrameContext&) {});
        handoff(items);
        const d11b::Status st1 = bridge.execute(dev, items, 5, false, work);
        if (r2 != d11b::Route::kBridge || st2 == d11b::Status::kFallback || st1 == d11b::Status::kFallback ||
            bridge.route(dev) != d11b::Route::kBridge || bridge.route(dev2) != d11b::Route::kBridge) {
            std::fprintf(stderr, "FAIL: Case J2 second device: route=%d status=%d, first device status=%d route=%d\n", int(r2),
                         int(st2), int(st1), int(bridge.route(dev)));
            return 1;
        }

        // Each binding of the bridge to a device has its own number (what one device's frames leave on D3D12 -
        // guides - is told from another's by it).
        uint64_t bind2 = 0, bind1 = 0;
        bridge.execute(dev2, back2, nullptr, [&](d11b::FrameContext& fc) { bind2 = fc.binding; });
        handoff(items);
        bridge.execute(dev, items, 5, false, [&](d11b::FrameContext& fc) { bind1 = fc.binding; });
        if (!bind1 || !bind2 || bind1 == bind2) {
            std::fprintf(stderr, "FAIL: Case J2 bindings: device 1's %llu, device 2's %llu (two different ones, neither 0)\n",
                         static_cast<unsigned long long>(bind1), static_cast<unsigned long long>(bind2));
            return 1;
        }

        // The route stays with the device itself: more devices than any table held keep theirs, and a new device is
        // undecided whatever lived at its address before.
        {
            ID3D11Device* more[6] = {};
            bool kept = true;
            for (ID3D11Device*& d : more) {
                if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fls, 2, D3D11_SDK_VERSION, &d, &fl,
                                             nullptr)) || !d) {
                    std::fprintf(stderr, "FAIL: Case J2 D3D11CreateDevice (more devices)\n");
                    return 1;
                }
                kept = kept && bridge.route(d) == d11b::Route::kUndecided && bridge.decide(d, false) == d11b::Route::kBridge;
            }
            for (ID3D11Device* d : more) kept = kept && bridge.route(d) == d11b::Route::kBridge;
            kept = kept && bridge.route(dev) == d11b::Route::kBridge && bridge.route(dev2) == d11b::Route::kBridge;
            for (ID3D11Device* d : more) d->Release();
            bridge.reset();
            const bool forgotten = bridge.route(dev) == d11b::Route::kUndecided && bridge.route(dev2) == d11b::Route::kUndecided;
            if (!kept || !forgotten) {
                std::fprintf(stderr, "FAIL: Case J2 routes of 8 devices kept=%d, forgotten at reset=%d\n", int(kept), int(forgotten));
                return 1;
            }
        }

        if (cmd) cmd->Release();
        if (alloc) alloc->Release();
        dsv->Release();
        for (ID3D11Texture2D* t : {colour, mine, output, depth, motion, depth_out, rb_colour, rb_mine, rb_output, rb_depth, back2})
            t->Release();
        ctx2->Release(); dev2->Release();
        ctx->Release(); dev->Release();
        std::printf("[PASS] Case J2: handoff by role (colour region, output in place, depth as R32_FLOAT), route latch, "
                    "second device (statuses %d, %d), a number per binding, 8 devices' routes\n", int(st2), int(st1));
    }

    // ---- Case K: Multi-swapchain primary arbiter ----
    {
        nr::pe::SwapChainArbiter arb;
        auto* sc1 = reinterpret_cast<IDXGISwapChain*>(uintptr_t(0x10001));
        auto* sc2 = reinterpret_cast<IDXGISwapChain*>(uintptr_t(0x10002));
        auto* sc3 = reinterpret_cast<IDXGISwapChain*>(uintptr_t(0x10003));
        HWND hwnd1 = reinterpret_cast<HWND>(uintptr_t(0x1001));
        HWND hwnd2 = reinterpret_cast<HWND>(uintptr_t(0x1002));
        HWND hwnd3 = reinterpret_cast<HWND>(uintptr_t(0x1003));

        bool alive1 = true, alive2 = true, alive3 = true;
        bool vis1 = true, vis2 = true, vis3 = true;
        HWND focused = hwnd1;
        auto alive_fn = [&](HWND h) {
            if (h == hwnd1) return alive1;
            if (h == hwnd2) return alive2;
            if (h == hwnd3) return alive3;
            return true;
        };
        auto vis_fn = [&](HWND h) {
            if (h == hwnd1) return vis1;
            if (h == hwnd2) return vis2;
            if (h == hwnd3) return vis3;
            return true;
        };
        auto foc_fn = [&](HWND h) { return h && h == focused; };

        uint64_t t = 1000;
        // 1. Single swap chain always primary.
        if (!arb.observe(sc1, hwnd1, 2412, 1128, t, alive_fn, vis_fn, foc_fn)) {
            std::fprintf(stderr, "FAIL: Case K single swapchain not primary\n");
            return 1;
        }

        // 2. OBS: 4K projector (unfocused) vs 2412x1128 preview (focused) -> 4K wins consistently.
        t += 16;
        if (!arb.observe(sc2, hwnd2, 3840, 2160, t, alive_fn, vis_fn, foc_fn)) {
            std::fprintf(stderr, "FAIL: Case K 4K projector did not take primary\n");
            return 1;
        }
        for (int i = 0; i < 4; ++i) {
            t += 16;
            if (arb.observe(sc1, hwnd1, 2412, 1128, t, alive_fn, vis_fn, foc_fn)) {
                std::fprintf(stderr, "FAIL: Case K preview stole primary while 4K active\n");
                return 1;
            }
            if (!arb.observe(sc2, hwnd2, 3840, 2160, t, alive_fn, vis_fn, foc_fn)) {
                std::fprintf(stderr, "FAIL: Case K 4K lost primary during alternation\n");
                return 1;
            }
        }

        // 3. Closing projector (!alive) immediately returns primary to preview.
        alive2 = false;
        t += 16;
        if (!arb.observe(sc1, hwnd1, 2412, 1128, t, alive_fn, vis_fn, foc_fn)) {
            std::fprintf(stderr, "FAIL: Case K closed projector did not immediately restore preview\n");
            return 1;
        }

        // 4. Stale projector (>600ms without Present) returns primary to preview.
        alive2 = true;
        t += 16;
        if (!arb.observe(sc2, hwnd2, 3840, 2160, t, alive_fn, vis_fn, foc_fn)) {
            std::fprintf(stderr, "FAIL: Case K re-activated projector did not win\n");
            return 1;
        }
        t += 700;
        if (!arb.observe(sc1, hwnd1, 2412, 1128, t, alive_fn, vis_fn, foc_fn)) {
            std::fprintf(stderr, "FAIL: Case K stale projector (>600ms) did not hand back primary\n");
            return 1;
        }

        // 5. Tiny <64x64 dummy (focused) must not steal primary from meaningful video window.
        arb.reset();
        focused = hwnd2;
        if (!arb.observe(sc1, hwnd1, 1920, 1080, t, alive_fn, vis_fn, foc_fn) ||
            arb.observe(sc2, hwnd2, 16, 16, t + 16, alive_fn, vis_fn, foc_fn) ||
            !arb.observe(sc1, hwnd1, 1920, 1080, t + 32, alive_fn, vis_fn, foc_fn)) {
            std::fprintf(stderr, "FAIL: Case K tiny dummy swapchain interfered with video window\n");
            return 1;
        }

        // 6. Minimized/hidden 4K loses to visible 1080p.
        arb.reset();
        vis1 = true; vis2 = false; focused = hwnd2;
        t += 100;
        arb.observe(sc2, hwnd2, 3840, 2160, t, alive_fn, vis_fn, foc_fn);
        if (!arb.observe(sc1, hwnd1, 1920, 1080, t + 16, alive_fn, vis_fn, foc_fn) ||
            arb.observe(sc2, hwnd2, 3840, 2160, t + 32, alive_fn, vis_fn, foc_fn)) {
            std::fprintf(stderr, "FAIL: Case K hidden 4K beat visible 1080p\n");
            return 1;
        }

        // 7. Equal/near-equal area within 5%: focus breaks tie; unfocused stays sticky.
        arb.reset();
        vis1 = vis2 = vis3 = true;
        focused = hwnd1;
        t += 100;
        arb.observe(sc1, hwnd1, 1920, 1080, t, alive_fn, vis_fn, foc_fn);
        arb.observe(sc2, hwnd2, 1920, 1080, t + 16, alive_fn, vis_fn, foc_fn);
        focused = hwnd2;
        if (!arb.observe(sc2, hwnd2, 1920, 1080, t + 32, alive_fn, vis_fn, foc_fn)) {
            std::fprintf(stderr, "FAIL: Case K focus tie-breaker did not select sc2\n");
            return 1;
        }
        focused = hwnd3; // neither sc1 nor sc2 focused -> sc2 stays sticky
        if (arb.observe(sc1, hwnd1, 1920, 1080, t + 48, alive_fn, vis_fn, foc_fn) ||
            !arb.observe(sc2, hwnd2, 1920, 1080, t + 64, alive_fn, vis_fn, foc_fn)) {
            std::fprintf(stderr, "FAIL: Case K unfocused equal-area did not stay sticky on sc2\n");
            return 1;
        }

        // 8. Same-HWND replacement / proxy swap chain updates in place and remains primary.
        arb.reset();
        t += 100;
        if (!arb.observe(sc1, hwnd1, 3840, 2160, t, alive_fn, vis_fn, foc_fn) ||
            !arb.observe(sc3, hwnd1, 1920, 1080, t + 16, alive_fn, vis_fn, foc_fn)) {
            std::fprintf(stderr, "FAIL: Case K same-HWND replacement lost primary\n");
            return 1;
        }

        std::printf("[PASS] Case K: SwapChainArbiter (OBS 4K+preview, close/stale fallback, tiny dummy, hidden, focus tie-break, same-HWND)\n");
    }

    // ---- Case L: Early CommandSignature tracking, unregistered copied split vtable, and per-feature fallback latch ----
    {
        // L1: CommandSignature created before d3d12_direct::install_hooks() and observed via note_command_signature().
        ID3D12Resource* arg_buf = make_buf(device, D3D12_HEAP_TYPE_DEFAULT, 64, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        if (!arg_buf) {
            std::fprintf(stderr, "FAIL: Case L1 arg_buf creation\n");
            return 1;
        }
        alloc->Reset();
        cmd->Reset(alloc, nullptr);
        cmd->ExecuteIndirect(early_tracked_sig, 0, arg_buf, 0, nullptr, 0);
        if (!nr::pe::d3d12_direct::ready(device, cmd, &why)) {
            std::fprintf(stderr, "FAIL: Case L1 early note_command_signature was rejected (%s)\n", why.c_str());
            return 1;
        }
        cmd->ExecuteIndirect(early_untracked_sig, 0, arg_buf, 0, nullptr, 0);
        std::string indirect_why;
        if (nr::pe::d3d12_direct::ready(device, cmd, &indirect_why) ||
            indirect_why.find("ExecuteIndirect") == std::string::npos ||
            indirect_why.find("bundle") != std::string::npos) {
            std::fprintf(stderr, "FAIL: Case L1 untracked ExecuteIndirect mismatch (%s)\n", indirect_why.c_str());
            return 1;
        }
        cmd->Close();
        alloc->Reset();
        cmd->Reset(alloc, nullptr);
        if (!nr::pe::d3d12_direct::ready(device, cmd, &why)) {
            std::fprintf(stderr, "FAIL: Case L1 ready did not recover after Reset (%s)\n", why.c_str());
            return 1;
        }
        cmd->Close();
        arg_buf->Release();
        early_untracked_sig->Release();
        early_tracked_sig->Release();

        // L2: Unregistered private-memory vtable copy holding split stubs must be accepted by is_split_list & ready().
        ID3D12CommandAllocator* ma2 = nullptr;
        ID3D12GraphicsCommandList* mc2 = nullptr;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ma2))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ma2, nullptr, IID_PPV_ARGS(&mc2)))) {
            std::fprintf(stderr, "FAIL: Case L2 command list setup\n");
            return 1;
        }
        if (!nr::pe::split::is_split_list(mc2)) {
            if (!nr::pe::split::prepare(mc2, &why)) {
                std::fprintf(stderr, "FAIL: Case L2 split::prepare (%s)\n", why.c_str());
                return 1;
            }
            mc2->Close();
            ma2->Reset();
            mc2->Reset(ma2, nullptr);
        }
        void** orig_vt2 = *reinterpret_cast<void***>(mc2);
        void* copied_split_vt[86]{};
        std::memcpy(copied_split_vt, orig_vt2, sizeof copied_split_vt);
        *reinterpret_cast<void***>(mc2) = copied_split_vt;
        const bool l2_split = nr::pe::split::is_split_list(mc2);
        const bool l2_ready = nr::pe::d3d12_direct::ready(device, mc2, &why);
        *reinterpret_cast<void***>(mc2) = orig_vt2;
        mc2->Close();
        mc2->Release();
        ma2->Release();
        if (!l2_split || !l2_ready) {
            std::fprintf(stderr, "FAIL: Case L2 unregistered copied split vtable split=%d ready=%d (%s)\n",
                         l2_split, l2_ready, why.c_str());
            return 1;
        }

        // L3: Per-feature fallback latch, sibling feature isolation, release-clears-latch, and idle VRAM eviction.
        nr::pe::d3d12_passthrough::test_reset_fallback_state();
        nr::pe::d3d12_passthrough::set_test_idle_evict_ms(10000);
        ID3D12Resource* tex_srgb = make_tex2d(device, W, H, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!tex_srgb) {
            std::fprintf(stderr, "FAIL: Case L3 tex_srgb setup\n");
            return 1;
        }
        nr::renodx::Settings r_l3{};
        r_l3.nr_enabled = true;
        r_l3.passes = 1;
        r_l3.codec_mode = 1;
        r_l3.transfer_strength = 1.0f;
        int wait_l3 = 0;
        auto run_l3 = [&](uint64_t feature, bool srgb_fail) -> bool {
            alloc->Reset();
            cmd->Reset(alloc, nullptr);
            nr::dlssnr::D3d12BackendFrame bf = fill_plain_frame(&wait_l3);
            bf.feature = feature;
            bf.motion = nullptr; bf.motion_w = 0; bf.motion_h = 0;
            bf.depth = nullptr; bf.depth_w = 0; bf.depth_h = 0;
            bf.renodx_route = 1;
            bf.renodx_present = 1;
            bf.renodx_white_nits = 203.0f;
            bf.renodx_settings = &r_l3;
            if (srgb_fail) {
                bf.color = tex_srgb;
                bf.output = tex_srgb;
                bf.output_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }
            const bool res = nr::pe::d3d12_passthrough::evaluate_backend(bf);
            cmd->Close();
            ID3D12CommandList* ls[] = {cmd};
            queue->ExecuteCommandLists(1, ls);
            wait_gpu();
            return res;
        };

        if (!run_l3(0x7701u, false) || !run_l3(0x7702u, false) || !nr::pe::d3d12_passthrough::test_has_resident_network()) {
            std::fprintf(stderr, "FAIL: Case L3 warmup on features A/B failed\n");
            return 1;
        }
        for (int i = 0; i < 8; ++i) {
            if (run_l3(0x7701u, true)) {
                std::fprintf(stderr, "FAIL: Case L3 sRGB fallback unexpectedly succeeded\n");
                return 1;
            }
        }
        if (nr::pe::d3d12_passthrough::status().backend.find("latched") == std::string::npos) {
            std::fprintf(stderr, "FAIL: Case L3 feature A did not latch (%s)\n",
                         nr::pe::d3d12_passthrough::status().backend.c_str());
            return 1;
        }
        if (run_l3(0x7701u, false)) {
            std::fprintf(stderr, "FAIL: Case L3 latched feature A ping-ponged back to Direct\n");
            return 1;
        }
        if (!run_l3(0x7702u, false)) {
            std::fprintf(stderr, "FAIL: Case L3 sibling feature B was blocked by feature A latch\n");
            return 1;
        }
        nr::pe::d3d12_passthrough::release_renodx_feature(0x7701u);
        if (!run_l3(0x7701u, false)) {
            std::fprintf(stderr, "FAIL: Case L3 release_renodx_feature did not clear feature A latch\n");
            return 1;
        }

        // Idle VRAM release after feature C latches and idle threshold elapses with no Direct runs.
        nr::pe::d3d12_passthrough::set_test_idle_evict_ms(30);
        if (!run_l3(0x7703u, false)) {
            std::fprintf(stderr, "FAIL: Case L3 warmup on feature C failed\n");
            return 1;
        }
        for (int i = 0; i < 8; ++i) (void)run_l3(0x7703u, true);
        Sleep(50);
        if (run_l3(0x7703u, false)) {
            std::fprintf(stderr, "FAIL: Case L3 latched feature C ran Direct during idle eviction check\n");
            return 1;
        }
        // One follow-up pass after GPU idle completes any deferred generation sweep.
        if (nr::pe::d3d12_passthrough::test_has_resident_network()) (void)run_l3(0x7703u, false);
        if (nr::pe::d3d12_passthrough::test_has_resident_network()) {
            std::fprintf(stderr, "FAIL: Case L3 idle D3D12 passthrough network remained resident after timeout\n");
            return 1;
        }
        nr::pe::d3d12_passthrough::set_test_idle_evict_ms(10000);
        nr::pe::d3d12_passthrough::test_reset_fallback_state();
        tex_srgb->Release();

        std::printf("[PASS] Case L: Early CommandSignature (%p), unregistered copied split vtable, & per-feature latch/idle evict\n",
                    static_cast<void*>(orig_vt2));
    }

    // Clean exit after GPU completion.
    untracked_bundle->Release();
    bundle_alloc->Release();
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
