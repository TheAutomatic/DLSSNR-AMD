#include "nr_d3d12_direct_state.hpp"
#include "nr_hud_method_hook.hpp"
#include "nr_path_u8.hpp"
#include "nr_pe_log.hpp"

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace nr::pe::d3d12_direct {
namespace {

bool in_d3d12_core_module(const void* fn, std::string* out_mod = nullptr) {
    if (!fn) return false;
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(fn), &mod) || !mod) {
        return false;
    }
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(mod, path, MAX_PATH)) return false;
    const wchar_t* base = std::wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    if (out_mod) *out_mod = nr::u8::narrow(base);
    if (_wcsnicmp(base, L"d3d12", 5) == 0) return true;
    for (const wchar_t* p = base; *p; ++p)
        if (_wcsnicmp(p, L"vkd3d", 5) == 0) return true;
    return false;
}

struct DirectRootArgs {
    ID3D12RootSignature* rs = nullptr;
    bool rs_set = false;
    uint64_t mask = 0;
    uint8_t kind[64]{};   // 1 table, 2 CBV, 3 SRV, 4 UAV
    uint64_t value[64]{};
    uint64_t const_param_mask = 0;
    uint64_t const_dword_mask[64]{};
    std::unique_ptr<std::array<uint32_t, 64>> const_vals[64]{};

    void clear_bindings() {
        mask = 0;
        const_param_mask = 0;
    }

    void set_rs(ID3D12RootSignature* r) {
        if (!rs_set || r != rs) clear_bindings();
        rs = r;
        rs_set = true;
    }

    void set(UINT index, uint8_t k, uint64_t v) {
        if (index >= 64) return;
        mask |= (1ull << index);
        kind[index] = k;
        value[index] = v;
    }

    void constant(UINT index, UINT offset, uint32_t v) {
        if (index >= 64 || offset >= 64) return;
        if (!const_vals[index]) const_vals[index] = std::make_unique<std::array<uint32_t, 64>>();
        if (!(const_param_mask & (1ull << index))) {
            const_dword_mask[index] = 0;
            const_param_mask |= (1ull << index);
        }
        (*const_vals[index])[offset] = v;
        const_dword_mask[index] |= (1ull << offset);
    }

    void clear_root_view(UINT index, uint8_t k) {
        if (index >= 64) return;
        if ((mask & (1ull << index)) && (k == 0 || kind[index] == k)) {
            mask &= ~(1ull << index);
        }
    }
};

struct DirectCmdSigInfo {
    uint8_t pipeline_kind = 0;   // 1 = graphics, 2 = compute
    uint64_t view_clear_mask = 0;
    uint8_t view_clear_kind[64]{};
    uint64_t const_clear_param_mask = 0;
    uint64_t const_clear_dword_mask[64]{};
};

void apply_cmd_sig_to_root(DirectRootArgs& r, const DirectCmdSigInfo* sig) {
    if (!sig) return;
    for (UINT i = 0; i < 64; ++i) {
        if (sig->view_clear_mask & (1ull << i)) {
            r.clear_root_view(i, sig->view_clear_kind[i]);
        }
        if (sig->const_clear_param_mask & (1ull << i)) {
            const uint64_t dm = sig->const_clear_dword_mask[i];
            for (UINT off = 0; off < 64; ++off) {
                if (dm & (1ull << off)) r.constant(i, off, 0u);
            }
        }
    }
}

struct DirectListState {
    bool valid = false;
    bool unknown = false;
    UINT heap_count = 0;
    ID3D12DescriptorHeap* heaps[2]{};
    DirectRootArgs compute_root{};
    ID3D12PipelineState* pso = nullptr;
    bool pso_set = false;
    ID3D12StateObject* state_object = nullptr;
    bool so_set = false;
    bool so_last = false;

    void reset(ID3D12PipelineState* initial) {
        valid = true;
        unknown = false;
        heap_count = 0;
        heaps[0] = heaps[1] = nullptr;
        compute_root.rs = nullptr;
        compute_root.rs_set = false;
        compute_root.clear_bindings();
        pso = initial;
        pso_set = (initial != nullptr);
        state_object = nullptr;
        so_set = false;
        so_last = false;
    }

    void clear_state(ID3D12PipelineState* initial) {
        const bool v = valid;
        const bool u = unknown;
        reset(initial);
        valid = v;
        unknown = u;
    }
};

struct DirectStateShard {
    std::mutex mutex;
    std::unordered_map<void*, DirectListState*> map;
};
struct QueueFence {
    ID3D12Fence* fence = nullptr;
    uint64_t value = 0;
};
struct DirectGlobals {
    DirectStateShard direct_shards[16];
    std::mutex direct_cmd_sig_mutex;
    std::unordered_map<void*, DirectCmdSigInfo> direct_cmd_sigs;
    std::mutex track_mutex;
    std::unordered_map<ID3D12CommandList*, nr::gpu::Ticket> recordings;
    std::unordered_map<ID3D12CommandQueue*, QueueFence> queue_fences;
    std::unordered_map<ID3D12CommandList*, ID3D12CommandAllocator*> list_to_alloc;
    std::mutex hook_install_mutex;
};
// Intentionally heap-allocated and never destroyed: D3D12Core detours (Release/Reset) continue
// firing during CRT atexit destructors of other translation units and DLL_PROCESS_DETACH.
DirectGlobals& globals() {
    static DirectGlobals* g = new DirectGlobals();
    return *g;
}
thread_local void* t_direct_last_key = nullptr;
thread_local DirectListState* t_direct_last_state = nullptr;

// Entries are never freed so thread_local caches on other threads never dangle.
DirectListState* direct_state_of(void* list) {
    if (!list) return nullptr;
    if (list == t_direct_last_key && t_direct_last_state) return t_direct_last_state;
    const size_t idx = (reinterpret_cast<uintptr_t>(list) >> 4) & 15u;
    DirectStateShard& shard = globals().direct_shards[idx];
    DirectListState* s = nullptr;
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto& ptr = shard.map[list];
        if (!ptr) ptr = new DirectListState();
        s = ptr;
    }
    t_direct_last_key = list;
    t_direct_last_state = s;
    return s;
}

void forget_direct_list(void* list) {
    if (!list) return;
    if (list == t_direct_last_key) {
        t_direct_last_key = nullptr;
        t_direct_last_state = nullptr;
    }
    const size_t idx = (reinterpret_cast<uintptr_t>(list) >> 4) & 15u;
    DirectStateShard& shard = globals().direct_shards[idx];
    std::lock_guard<std::mutex> lock(shard.mutex);
    const auto it = shard.map.find(list);
    if (it != shard.map.end() && it->second) {
        it->second->reset(nullptr);
        it->second->valid = false;
    }
}

void register_direct_cmd_sig(void* sig, const D3D12_COMMAND_SIGNATURE_DESC* desc) {
    if (!sig || !desc) return;
    DirectCmdSigInfo info{};
    if (desc->pArgumentDescs && desc->NumArgumentDescs > 0) {
        for (UINT i = 0; i < desc->NumArgumentDescs; ++i) {
            const D3D12_INDIRECT_ARGUMENT_DESC& a = desc->pArgumentDescs[i];
            switch (static_cast<uint32_t>(a.Type)) {
            case 0:    // DRAW
            case 1:    // DRAW_INDEXED
            case 3:    // VERTEX_BUFFER_VIEW
            case 4:    // INDEX_BUFFER_VIEW
            case 10:   // DISPATCH_MESH
                info.pipeline_kind = 1;
                break;
            case 2:    // DISPATCH
            case 9:    // DISPATCH_RAYS
                info.pipeline_kind = 2;
                break;
            case 5:    // CONSTANT
            case 11: { // INCREMENTING_CONSTANT
                const UINT idx = a.Constant.RootParameterIndex;
                const UINT off = a.Constant.DestOffsetIn32BitValues;
                const UINT cnt = (static_cast<uint32_t>(a.Type) == 11) ? 1u : a.Constant.Num32BitValuesToSet;
                if (idx < 64 && off < 64 && cnt > 0) {
                    info.const_clear_param_mask |= (1ull << idx);
                    for (UINT k = 0; k < cnt && (off + k) < 64; ++k)
                        info.const_clear_dword_mask[idx] |= (1ull << (off + k));
                }
                break;
            }
            case 6: { // CBV
                const UINT idx = a.ConstantBufferView.RootParameterIndex;
                if (idx < 64) { info.view_clear_mask |= (1ull << idx); info.view_clear_kind[idx] = 2; }
                break;
            }
            case 7: { // SRV
                const UINT idx = a.ShaderResourceView.RootParameterIndex;
                if (idx < 64) { info.view_clear_mask |= (1ull << idx); info.view_clear_kind[idx] = 3; }
                break;
            }
            case 8: { // UAV
                const UINT idx = a.UnorderedAccessView.RootParameterIndex;
                if (idx < 64) { info.view_clear_mask |= (1ull << idx); info.view_clear_kind[idx] = 4; }
                break;
            }
            default:
                break;
            }
        }
    }
    DirectGlobals& d = globals();
    std::lock_guard<std::mutex> lock(d.direct_cmd_sig_mutex);
    d.direct_cmd_sigs[sig] = info;
}

void apply_direct_execute_indirect(DirectListState* s, void* sig) {
    if (!s) return;
    DirectCmdSigInfo info{};
    bool found = false;
    if (sig) {
        DirectGlobals& d = globals();
        std::lock_guard<std::mutex> lock(d.direct_cmd_sig_mutex);
        const auto it = d.direct_cmd_sigs.find(sig);
        if (it != d.direct_cmd_sigs.end()) {
            info = it->second;
            found = true;
        }
    }
    if (!found) {
        s->unknown = true;
        return;
    }
    if (info.pipeline_kind == 2) {
        apply_cmd_sig_to_root(s->compute_root, &info);
    }
}

// ---- recording completion tracking (independent of nr_pe_d3d12split.cpp) ----
std::atomic<bool> g_booked{false};

thread_local ID3D12CommandQueue* t_last_seen = nullptr;
thread_local ID3D12CommandQueue* t_last_direct = nullptr;

QueueFence* completion_fence_locked(ID3D12CommandQueue* q) {
    DirectGlobals& d = globals();
    auto it = d.queue_fences.find(q);
    if (it != d.queue_fences.end()) return &it->second;
    ID3D12Device* dev = nullptr;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return nullptr;
    QueueFence f;
    const HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f.fence));
    dev->Release();
    if (FAILED(hr) || !f.fence) return nullptr;
    q->AddRef();
    return &d.queue_fences.emplace(q, f).first->second;
}

void stamp_bookings_locked(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    DirectGlobals& d = globals();
    bool any = false;
    for (UINT i = 0; i < n && !any; ++i) {
        const auto it = d.recordings.find(lists[i]);
        if (it != d.recordings.end() && it->second && !it->second->sealed) any = true;
    }
    if (!any) return;
    QueueFence* f = completion_fence_locked(q);
    const bool signalled = f && f->fence && SUCCEEDED(q->Signal(f->fence, f->value + 1));
    if (signalled) ++f->value;
    for (UINT i = 0; i < n; ++i) {
        const auto it = d.recordings.find(lists[i]);
        if (it == d.recordings.end() || !it->second || it->second->sealed) continue;
        if (!signalled) {
            it->second->submitted(nullptr);
            continue;
        }
        ID3D12Fence* fence = f->fence;
        fence->AddRef();
        const uint64_t value = f->value;
        it->second->submitted([fence, value] {
            const uint64_t v = fence->GetCompletedValue();
            const bool done = v != UINT64_MAX && v >= value;
            if (done) fence->Release();
            return done;
        });
    }
}

void seal_list_recording(ID3D12CommandList* cl) {
    if (!cl || !g_booked.load(std::memory_order_acquire)) return;
    DirectGlobals& d = globals();
    std::lock_guard<std::mutex> lock(d.track_mutex);
    const auto it = d.recordings.find(cl);
    if (it != d.recordings.end() && it->second) it->second->seal();
}

void note_list_alloc(ID3D12CommandList* cl, ID3D12CommandAllocator* alloc) {
    if (!cl) return;
    DirectGlobals& d = globals();
    std::lock_guard<std::mutex> lock(d.track_mutex);
    if (alloc) d.list_to_alloc[cl] = alloc;
    else d.list_to_alloc.erase(cl);
}

void note_allocator_reset(ID3D12CommandAllocator* alloc) {
    if (!alloc || !g_booked.load(std::memory_order_acquire)) return;
    DirectGlobals& d = globals();
    std::lock_guard<std::mutex> lock(d.track_mutex);
    for (const auto& [cl, a] : d.list_to_alloc) {
        if (a != alloc) continue;
        const auto it = d.recordings.find(cl);
        if (it != d.recordings.end() && it->second) it->second->seal();
    }
}

void note_allocator_released(ID3D12CommandAllocator* alloc) {
    if (!alloc) return;
    DirectGlobals& d = globals();
    std::lock_guard<std::mutex> lock(d.track_mutex);
    for (auto it = d.list_to_alloc.begin(); it != d.list_to_alloc.end();) {
        if (it->second == alloc) {
            const auto rit = d.recordings.find(it->first);
            if (rit != d.recordings.end() && rit->second) rit->second->seal();
            it = d.list_to_alloc.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- MinHook installation & detours ----
std::atomic<bool> g_hooks_installed{false};
std::atomic<bool> g_hooks_ok{false};
thread_local int t_in_direct_record = 0;

const GUID kIID_List4 = {0x8754318e, 0xd3a9, 0x4541, {0x98, 0xcf, 0x64, 0x5b, 0x50, 0xdc, 0x48, 0x74}};
const GUID kIID_Dev4 = {0xe865df17, 0xa9ee, 0x46f9, {0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5}};

using PFN_CL_Release = ULONG(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using PFN_CL_Reset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using PFN_CL_ClearState = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using PFN_CL_SetPso = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using PFN_CL_SetPso1 = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, ID3D12StateObject*);
using PFN_CL_ExecBundle = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
using PFN_CL_SetHeaps = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using PFN_CL_SetComputeRs = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using PFN_CL_SetComputeTable = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using PFN_CL_SetComputeConst = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using PFN_CL_SetComputeConsts = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
using PFN_CL_SetComputeView = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using PFN_CL_ExecIndirect = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT,
                                                       ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
using PFN_CA_Reset = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandAllocator*);
using PFN_CA_Release = ULONG(STDMETHODCALLTYPE*)(ID3D12CommandAllocator*);
using PFN_Dev_CreateCL = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE,
                                                     ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
using PFN_Dev4_CreateCL1 = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, D3D12_COMMAND_LIST_TYPE, UINT, REFIID, void**);
using PFN_Dev_CreateCmdSig = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*,
                                                         ID3D12RootSignature*, REFIID, void**);
using PFN_Queue_Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

PFN_CL_Release real_cl_release = nullptr;
PFN_CL_Reset real_cl_reset = nullptr;
PFN_CL_ClearState real_cl_clear_state = nullptr;
PFN_CL_SetPso real_cl_set_pso = nullptr;
PFN_CL_SetPso1 real_cl_set_pso1 = nullptr;
PFN_CL_ExecBundle real_cl_exec_bundle = nullptr;
PFN_CL_SetHeaps real_cl_set_heaps = nullptr;
PFN_CL_SetComputeRs real_cl_set_compute_rs = nullptr;
PFN_CL_SetComputeTable real_cl_set_compute_table = nullptr;
PFN_CL_SetComputeConst real_cl_set_compute_const = nullptr;
PFN_CL_SetComputeConsts real_cl_set_compute_consts = nullptr;
PFN_CL_SetComputeView real_cl_set_compute_cbv = nullptr;
PFN_CL_SetComputeView real_cl_set_compute_srv = nullptr;
PFN_CL_SetComputeView real_cl_set_compute_uav = nullptr;
PFN_CL_ExecIndirect real_cl_exec_indirect = nullptr;
PFN_CA_Reset real_ca_reset = nullptr;
PFN_CA_Release real_ca_release = nullptr;
PFN_Dev_CreateCL real_dev_create_cl = nullptr;
PFN_Dev4_CreateCL1 real_dev4_create_cl1 = nullptr;
PFN_Dev_CreateCmdSig real_dev_create_cmd_sig = nullptr;
PFN_Queue_Execute real_queue_execute = nullptr;
void* g_hooked_ca_release_fn = nullptr;

ULONG STDMETHODCALLTYPE hk_cl_release(ID3D12GraphicsCommandList* cl) {
    const ULONG left = real_cl_release(cl);
    if (left == 0 && cl) {
        seal_list_recording(cl);
        note_list_alloc(cl, nullptr);
        forget_direct_list(cl);
    }
    return left;
}

HRESULT STDMETHODCALLTYPE hk_cl_reset(ID3D12GraphicsCommandList* cl, ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso) {
    const HRESULT hr = real_cl_reset(cl, alloc, pso);
    if (SUCCEEDED(hr) && cl && !t_in_direct_record) {
        seal_list_recording(cl);
        note_list_alloc(cl, alloc);
        direct_state_of(cl)->reset(pso);
    }
    return hr;
}

void STDMETHODCALLTYPE hk_cl_clear_state(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso) {
    if (cl && !t_in_direct_record) direct_state_of(cl)->clear_state(pso);
    real_cl_clear_state(cl, pso);
}

void STDMETHODCALLTYPE hk_cl_set_pso(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso) {
    if (cl && !t_in_direct_record) {
        DirectListState* s = direct_state_of(cl);
        s->pso = pso;
        s->pso_set = true;
        s->so_last = false;
    }
    real_cl_set_pso(cl, pso);
}

void STDMETHODCALLTYPE hk_cl_set_pso1(ID3D12GraphicsCommandList4* cl, ID3D12StateObject* so) {
    if (cl && !t_in_direct_record) {
        DirectListState* s = direct_state_of(cl);
        s->state_object = so;
        s->so_set = true;
        s->so_last = true;
    }
    real_cl_set_pso1(cl, so);
}

void STDMETHODCALLTYPE hk_cl_exec_bundle(ID3D12GraphicsCommandList* cl, ID3D12GraphicsCommandList* bundle) {
    if (cl && !t_in_direct_record) {
        DirectListState* s = direct_state_of(cl);
        const DirectListState* bs = (bundle && bundle != cl) ? direct_state_of(bundle) : nullptr;
        if (bundle && bundle != cl) direct_state_of(cl);
        if (bs && bs->valid) {
            if (bs->unknown) {
                s->unknown = true;
            } else {
                const DirectRootArgs& br = bs->compute_root;
                if (br.rs_set) s->compute_root.set_rs(br.rs);
                for (UINT i = 0; i < 64; ++i) {
                    if (br.mask & (1ull << i)) s->compute_root.set(i, br.kind[i], br.value[i]);
                    if ((br.const_param_mask & (1ull << i)) && br.const_vals[i]) {
                        const uint64_t dm = br.const_dword_mask[i];
                        for (UINT off = 0; off < 64; ++off)
                            if (dm & (1ull << off)) s->compute_root.constant(i, off, (*br.const_vals[i])[off]);
                    }
                }
                if (bs->pso_set) { s->pso = bs->pso; s->pso_set = true; s->so_last = false; }
                if (bs->so_set) { s->state_object = bs->state_object; s->so_set = true; s->so_last = bs->so_last; }
            }
        }
    }
    real_cl_exec_bundle(cl, bundle);
}

// Read-only SetDescriptorHeaps observation: invalidates stale compute descriptor table handles when heaps change.
void STDMETHODCALLTYPE hk_cl_set_heaps(ID3D12GraphicsCommandList* cl, UINT count, ID3D12DescriptorHeap* const* heaps) {
    if (cl && !t_in_direct_record) {
        DirectListState* s = direct_state_of(cl);
        const UINT n = (std::min)(count, 2u);
        bool same = (n == s->heap_count);
        for (UINT i = 0; same && i < n; ++i) same = (s->heaps[i] == (heaps ? heaps[i] : nullptr));
        if (!same) {
            for (UINT i = 0; i < 64; ++i) {
                if ((s->compute_root.mask & (1ull << i)) && s->compute_root.kind[i] == 1)
                    s->compute_root.mask &= ~(1ull << i);
            }
        }
        s->heap_count = n;
        for (UINT i = 0; i < n; ++i) s->heaps[i] = heaps ? heaps[i] : nullptr;
    }
    real_cl_set_heaps(cl, count, heaps);
}

void STDMETHODCALLTYPE hk_cl_set_compute_rs(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* rs) {
    if (cl && !t_in_direct_record) direct_state_of(cl)->compute_root.set_rs(rs);
    real_cl_set_compute_rs(cl, rs);
}

void STDMETHODCALLTYPE hk_cl_set_compute_table(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (cl && !t_in_direct_record) direct_state_of(cl)->compute_root.set(index, 1, h.ptr);
    real_cl_set_compute_table(cl, index, h);
}

void STDMETHODCALLTYPE hk_cl_set_compute_const(ID3D12GraphicsCommandList* cl, UINT index, UINT val, UINT offset) {
    if (cl && !t_in_direct_record) direct_state_of(cl)->compute_root.constant(index, offset, val);
    real_cl_set_compute_const(cl, index, val, offset);
}

void STDMETHODCALLTYPE hk_cl_set_compute_consts(ID3D12GraphicsCommandList* cl, UINT index, UINT count, const void* data, UINT offset) {
    if (cl && data && !t_in_direct_record) {
        DirectRootArgs& r = direct_state_of(cl)->compute_root;
        const auto* v = static_cast<const uint32_t*>(data);
        for (UINT i = 0; i < count; ++i) r.constant(index, offset + i, v[i]);
    }
    real_cl_set_compute_consts(cl, index, count, data, offset);
}

void STDMETHODCALLTYPE hk_cl_set_compute_cbv(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (cl && !t_in_direct_record) direct_state_of(cl)->compute_root.set(index, 2, va);
    real_cl_set_compute_cbv(cl, index, va);
}

void STDMETHODCALLTYPE hk_cl_set_compute_srv(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (cl && !t_in_direct_record) direct_state_of(cl)->compute_root.set(index, 3, va);
    real_cl_set_compute_srv(cl, index, va);
}

void STDMETHODCALLTYPE hk_cl_set_compute_uav(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (cl && !t_in_direct_record) direct_state_of(cl)->compute_root.set(index, 4, va);
    real_cl_set_compute_uav(cl, index, va);
}

void STDMETHODCALLTYPE hk_cl_exec_indirect(ID3D12GraphicsCommandList* cl, ID3D12CommandSignature* sig,
                                           UINT max_count, ID3D12Resource* arg_buf, UINT64 arg_offset,
                                           ID3D12Resource* count_buf, UINT64 count_offset) {
    if (cl && !t_in_direct_record) apply_direct_execute_indirect(direct_state_of(cl), sig);
    real_cl_exec_indirect(cl, sig, max_count, arg_buf, arg_offset, count_buf, count_offset);
}

HRESULT STDMETHODCALLTYPE hk_ca_reset(ID3D12CommandAllocator* ca) {
    const HRESULT hr = real_ca_reset(ca);
    if (SUCCEEDED(hr) && ca) note_allocator_reset(ca);
    return hr;
}

ULONG STDMETHODCALLTYPE hk_ca_release(ID3D12CommandAllocator* ca) {
    const ULONG left = real_ca_release(ca);
    if (left == 0 && ca) note_allocator_released(ca);
    return left;
}

HRESULT STDMETHODCALLTYPE hk_dev_create_cmd_sig(ID3D12Device* dev, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                                ID3D12RootSignature* rs, REFIID riid, void** out) {
    const HRESULT hr = real_dev_create_cmd_sig(dev, desc, rs, riid, out);
    if (SUCCEEDED(hr) && out && *out && desc) {
        ID3D12CommandSignature* sig = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&sig))) && sig) {
            register_direct_cmd_sig(sig, desc);
            sig->Release();
        } else {
            register_direct_cmd_sig(*out, desc);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_dev_create_cl(ID3D12Device* dev, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                           ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso, REFIID riid, void** out) {
    const HRESULT hr = real_dev_create_cl(dev, mask, type, alloc, pso, riid, out);
    if (SUCCEEDED(hr) && out && *out && !t_in_direct_record) {
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&list))) && list) {
            note_list_alloc(list, alloc);
            direct_state_of(list)->reset(pso);
            list->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_dev4_create_cl1(void* dev4, UINT mask, D3D12_COMMAND_LIST_TYPE type, UINT flags,
                                             REFIID riid, void** out) {
    const HRESULT hr = real_dev4_create_cl1(dev4, mask, type, flags, riid, out);
    if (SUCCEEDED(hr) && out && *out && !t_in_direct_record) {
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&list))) && list) {
            note_list_alloc(list, nullptr);
            direct_state_of(list)->reset(nullptr);
            list->Release();
        }
    }
    return hr;
}

void STDMETHODCALLTYPE hk_queue_execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    real_queue_execute(q, n, lists);
    if (q != t_last_seen) {
        t_last_seen = q;
        if (q && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) t_last_direct = q;
    }
    if (g_booked.load(std::memory_order_acquire) && q && lists && n) {
        std::lock_guard<std::mutex> lock(globals().track_mutex);
        stamp_bookings_locked(q, n, lists);
    }
}

bool restore_direct_state(ID3D12GraphicsCommandList* cmd) {
    const DirectListState* s = direct_state_of(cmd);
    if (!s || !s->valid || s->unknown) return false;

    ++t_in_direct_record;
    const DirectRootArgs& r = s->compute_root;
    if (r.rs_set && r.rs) {
        cmd->SetComputeRootSignature(r.rs);
        for (UINT i = 0; i < 64; ++i) {
            if (r.mask & (1ull << i)) {
                switch (r.kind[i]) {
                case 1: cmd->SetComputeRootDescriptorTable(i, {r.value[i]}); break;
                case 2: cmd->SetComputeRootConstantBufferView(i, r.value[i]); break;
                case 3: cmd->SetComputeRootShaderResourceView(i, r.value[i]); break;
                case 4: cmd->SetComputeRootUnorderedAccessView(i, r.value[i]); break;
                default: break;
                }
            }
            if ((r.const_param_mask & (1ull << i)) && r.const_vals[i]) {
                uint64_t dm = r.const_dword_mask[i];
                while (dm) {
                    const UINT start = UINT(__builtin_ctzll(dm));
                    UINT count = 0;
                    while (start + count < 64 && (dm & (1ull << (start + count)))) ++count;
                    cmd->SetComputeRoot32BitConstants(i, count, &(*r.const_vals[i])[start], start);
                    const uint64_t run = (count == 64) ? ~0ull : ((1ull << count) - 1ull);
                    dm &= ~(run << start);
                }
            }
        }
    }
    if (s->so_last && s->so_set && s->state_object) {
        ID3D12GraphicsCommandList4* cl4 = nullptr;
        if (SUCCEEDED(cmd->QueryInterface(kIID_List4, reinterpret_cast<void**>(&cl4))) && cl4) {
            cl4->SetPipelineState1(s->state_object);
            cl4->Release();
        }
    } else if (s->pso_set && s->pso) {
        cmd->SetPipelineState(s->pso);
    }
    --t_in_direct_record;
    return true;
}

template <class Fn>
bool install_core_method(void* obj, unsigned slot, Fn detour, Fn* real, const char* name, std::string* why) {
    if (!obj) return false;
    void* fn = (*reinterpret_cast<void***>(obj))[slot];
    std::string mod;
    if (!in_d3d12_core_module(fn, &mod)) {
        if (why) *why = std::string("direct state hook refused: ") + name + " outside D3D12 module (" + mod + ")";
        return false;
    }
    const MH_STATUS st = nr::hudcap::MethodHook<Fn>::install_raw(fn, detour, real);
    if (st != MH_OK) {
        if (why) *why = std::string("direct state hook failed on ") + name + " (" + MH_StatusToString(st) + ")";
        return false;
    }
    return true;
}

}  // namespace

bool install_hooks(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, std::string* why) {
    if (g_hooks_installed.load(std::memory_order_acquire))
        return g_hooks_ok.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(globals().hook_install_mutex);
    if (g_hooks_installed.load(std::memory_order_relaxed))
        return g_hooks_ok.load(std::memory_order_relaxed);

    if (!dev) {
        if (why) *why = "direct state hooks: null device";
        return false;
    }
    ID3D12CommandAllocator* temp_alloc = nullptr;
    ID3D12GraphicsCommandList* temp_cl = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&temp_alloc))) || !temp_alloc) {
        if (why) *why = "direct state hooks: CreateCommandAllocator failed";
        return false;
    }
    if (!cl) {
        if (SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, temp_alloc, nullptr, IID_PPV_ARGS(&temp_cl))) && temp_cl) {
            temp_cl->Close();
            cl = temp_cl;
        }
    }
    ID3D12CommandQueue* temp_q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&temp_q));

    struct Hold {
        ID3D12GraphicsCommandList* l;
        ID3D12CommandAllocator* a;
        ID3D12CommandQueue* q;
        ~Hold() { if (l) l->Release(); if (a) a->Release(); if (q) q->Release(); }
    } hold{temp_cl, temp_alloc, temp_q};

    if (!cl || !temp_q) {
        if (why) *why = "direct state hooks: probe object creation failed";
        return false;
    }

    // Install Allocator::Reset / Release and Device::CreateCommandSignature first.
    {
        void** cavt = *reinterpret_cast<void***>(temp_alloc);
        std::string mod;
        if (in_d3d12_core_module(cavt[8], &mod))
            nr::hudcap::MethodHook<PFN_CA_Reset>::install_raw(cavt[8], &hk_ca_reset, &real_ca_reset);
        if (in_d3d12_core_module(cavt[2])) {
            if (nr::hudcap::MethodHook<PFN_CA_Release>::install_raw(cavt[2], &hk_ca_release, &real_ca_release) == MH_OK)
                g_hooked_ca_release_fn = cavt[2];
        }
    }

    bool ok = true;
    ok = install_core_method(dev, 41, &hk_dev_create_cmd_sig, &real_dev_create_cmd_sig, "CreateCommandSignature", why) && ok;
    ok = install_core_method(cl, 10, &hk_cl_reset, &real_cl_reset, "Reset", why) && ok;
    ok = install_core_method(cl, 11, &hk_cl_clear_state, &real_cl_clear_state, "ClearState", why) && ok;
    ok = install_core_method(cl, 25, &hk_cl_set_pso, &real_cl_set_pso, "SetPipelineState", why) && ok;
    ok = install_core_method(cl, 27, &hk_cl_exec_bundle, &real_cl_exec_bundle, "ExecuteBundle", why) && ok;
    ok = install_core_method(cl, 28, &hk_cl_set_heaps, &real_cl_set_heaps, "SetDescriptorHeaps", why) && ok;
    ok = install_core_method(cl, 29, &hk_cl_set_compute_rs, &real_cl_set_compute_rs, "SetComputeRootSignature", why) && ok;
    ok = install_core_method(cl, 31, &hk_cl_set_compute_table, &real_cl_set_compute_table, "SetComputeRootDescriptorTable", why) && ok;
    ok = install_core_method(cl, 33, &hk_cl_set_compute_const, &real_cl_set_compute_const, "SetComputeRoot32BitConstant", why) && ok;
    ok = install_core_method(cl, 35, &hk_cl_set_compute_consts, &real_cl_set_compute_consts, "SetComputeRoot32BitConstants", why) && ok;
    ok = install_core_method(cl, 37, &hk_cl_set_compute_cbv, &real_cl_set_compute_cbv, "SetComputeRootConstantBufferView", why) && ok;
    ok = install_core_method(cl, 39, &hk_cl_set_compute_srv, &real_cl_set_compute_srv, "SetComputeRootShaderResourceView", why) && ok;
    ok = install_core_method(cl, 41, &hk_cl_set_compute_uav, &real_cl_set_compute_uav, "SetComputeRootUnorderedAccessView", why) && ok;
    ok = install_core_method(cl, 59, &hk_cl_exec_indirect, &real_cl_exec_indirect, "ExecuteIndirect", why) && ok;

    void** clvt = *reinterpret_cast<void***>(cl);
    if (clvt[2] != g_hooked_ca_release_fn && in_d3d12_core_module(clvt[2]))
        nr::hudcap::MethodHook<PFN_CL_Release>::install_raw(clvt[2], &hk_cl_release, &real_cl_release);

    ID3D12GraphicsCommandList4* cl4 = nullptr;
    if (SUCCEEDED(cl->QueryInterface(kIID_List4, reinterpret_cast<void**>(&cl4))) && cl4) {
        void** vt4 = *reinterpret_cast<void***>(cl4);
        if (in_d3d12_core_module(vt4[75]))
            nr::hudcap::MethodHook<PFN_CL_SetPso1>::install_raw(vt4[75], &hk_cl_set_pso1, &real_cl_set_pso1);
        cl4->Release();
    }

    void** dvt = *reinterpret_cast<void***>(dev);
    if (in_d3d12_core_module(dvt[12]))
        nr::hudcap::MethodHook<PFN_Dev_CreateCL>::install_raw(dvt[12], &hk_dev_create_cl, &real_dev_create_cl);
    IUnknown* dev4 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(kIID_Dev4, reinterpret_cast<void**>(&dev4))) && dev4) {
        void** dvt4 = *reinterpret_cast<void***>(dev4);
        if (in_d3d12_core_module(dvt4[51]))
            nr::hudcap::MethodHook<PFN_Dev4_CreateCL1>::install_raw(dvt4[51], &hk_dev4_create_cl1, &real_dev4_create_cl1);
        dev4->Release();
    }

    ok = install_core_method(temp_q, 10, &hk_queue_execute, &real_queue_execute, "ExecuteCommandLists", why) && ok;

    g_hooks_ok.store(ok, std::memory_order_relaxed);
    g_hooks_installed.store(true, std::memory_order_release);
    if (ok) {
        log("[nr] Direct compute-state hooks active (compute-only tracking, read-only heap observer)");
    } else if (why && !why->empty()) {
        log("[nr] %s", why->c_str());
    }
    return ok;
}

bool ready(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, std::string* why, bool* waiting_reset) {
    if (waiting_reset) *waiting_reset = false;
    if (!install_hooks(dev, cl, why)) return false;
    if (!cl) {
        if (why) *why = "null command list";
        return false;
    }
    const DirectListState* s = direct_state_of(cl);
    if (!s || !s->valid) {
        if (waiting_reset) *waiting_reset = true;
        if (why) *why = "the command list was made before the direct state hooks";
        return false;
    }
    if (s->unknown) {
        if (why) *why = "the command list recorded an unknown ExecuteIndirect signature";
        return false;
    }
    return true;
}

Scope::Scope(ID3D12GraphicsCommandList* cl, bool restore) : cl_(cl), restore_(restore), entered_(true) {
    ++t_in_direct_record;
}

Scope::~Scope() {
    release();
}

void Scope::release() {
    if (!entered_) return;
    entered_ = false;
    const int after = --t_in_direct_record;
    if (after == 0 && restore_ && cl_) restore_direct_state(cl_);
    cl_ = nullptr;
    restore_ = false;
}

nr::gpu::Ticket book(ID3D12CommandList* list) {
    if (!list) return nullptr;
    DirectGlobals& d = globals();
    std::lock_guard<std::mutex> lock(d.track_mutex);
    const auto it = d.recordings.find(list);
    if (it != d.recordings.end() && it->second && !it->second->sealed) return it->second;
    auto rec = std::make_shared<nr::gpu::Recording>();
    d.recordings[list] = rec;
    g_booked.store(true, std::memory_order_release);
    return rec;
}

bool completed(const nr::gpu::Ticket& rec) {
    if (!rec) return false;
    if (rec->strict_ready()) return true;
    if (!rec->quiet_ready()) return false;
    static int said = 0;
    if (said < 3) {
        ++said;
        log("[nr] D3D12 passthrough: a recorded command list ran and finished but was not reset for 2 s (%s); taken as finished",
            rec->sealed ? "sealed" : rec->ever_submitted ? "executed, kept by the game" : "never executed");
    }
    return true;
}

void note_execution(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    if (!queue || !lists || !count || !g_booked.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(globals().track_mutex);
    stamp_bookings_locked(queue, count, lists);
}

void seal(const nr::gpu::Ticket& rec) { if (rec) rec->seal(); }
void invalidate(const nr::gpu::Ticket& rec) { if (rec) rec->seal(); }
ID3D12CommandQueue* last_direct_queue() { return t_last_direct; }

}  // namespace nr::pe::d3d12_direct
