#pragma once
// Zero-descriptor-heap outer compute kernels via NRD12OUT ACO archives and amdxc64 PAL ELFs.
// Hardware 32-byte image SRDs are written through a small CPU staging descriptor heap into upload-buffer slots
// bound via Root UAV parameter 1; SetDescriptorHeaps and SetComputeRootDescriptorTable are never called.
#include "nr_d3d12_passthrough.hpp"
#include "nr_d3d12_passthrough_elf.hpp"
#include "nr_gpu_completion.hpp"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nr::pe::d3d12_outer {
namespace elf = nr::pe::d3d12_passthrough::elf;

inline constexpr uint32_t kSlotDescriptors = 64;
inline constexpr uint32_t kSlotBytes = kSlotDescriptors * 32u;   // 2048 bytes per dispatch slot
inline constexpr uint32_t kOuterPushOff = 1664u;
inline constexpr uint32_t kOuterGridOff = 1920u;
inline constexpr uint32_t kMaxPushBytes = 256u;

enum BindingKind : uint8_t {
    kSrvTex = 1,
    kUavTex = 2,
    kBuf = 3,
};

struct OuterBinding {
    uint16_t binding = 0;
    uint8_t kind = 0;
    uint8_t pad = 0;
    uint16_t offset = 0;
    uint16_t stride = 0;
};

struct OuterKernel {
    ID3D12PipelineState* pso = nullptr;
    uint32_t wg[3]{1, 1, 1};
    uint32_t root_push_dwords = 0;
    uint32_t desc_zero_bytes = kOuterPushOff;
    bool needs_slot_push = true;
    bool needs_slot_grid = true;
    std::vector<OuterBinding> bindings;

    const OuterBinding* find_binding(uint32_t binding) const {
        for (const auto& b : bindings)
            if (b.binding == binding) return &b;
        return nullptr;
    }
    const OuterBinding* find_binding(uint32_t binding, uint8_t kind) const {
        for (const auto& b : bindings)
            if (b.binding == binding && b.kind == kind) return &b;
        return nullptr;
    }
};

struct KernelEntry {
    std::string name;
    uint32_t wg[3]{1, 1, 1};
    std::vector<OuterBinding> bindings;
    elf::Bytes nrp;
};

inline bool create_outer_root_signature(ID3D12Device* device, ID3D12RootSignature** out_rs, std::string* why) {
    return elf::create_carrier_root_signature(device, elf::kOuterRootConstants, out_rs, why);
}

class OuterArchive {
public:
    bool load(const std::wstring& path, uint32_t gfx11_mach, std::string* why) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (loaded_ && loaded_path_ == path) return true;
        entries_.clear();
        by_name_.clear();
        loaded_ = false;

        elf::Bytes raw;
        if (!elf::read_file_w(path, &raw)) {
            if (why) *why = "outer ACO archive is missing";
            return false;
        }
        if (raw.size() < 24 || std::memcmp(raw.data(), "NRD12OUT", 8) != 0) {
            if (why) *why = "outer ACO archive header is invalid";
            return false;
        }
        const uint32_t version = elf::ru32(raw.data(), 8);
        const uint32_t mach = elf::ru32(raw.data(), 12);
        const uint32_t count = elf::ru32(raw.data(), 16);
        if (version != 1u || count == 0 || count > 4096u) {
            if (why) *why = "outer ACO archive version or count is unsupported";
            return false;
        }
        if (gfx11_mach == 0 && mach != 0x4Eu) {
            if (why) *why = "outer ACO archive target mismatch (expected gfx1201)";
            return false;
        }
        if (gfx11_mach != 0 && !elf::is_gfx11_mach(mach)) {
            if (why) *why = "outer ACO archive target mismatch (expected gfx11)";
            return false;
        }

        size_t pos = 24;
        entries_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (pos + 2 > raw.size()) { if (why) *why = "truncated outer archive"; return false; }
            uint16_t name_len = 0;
            std::memcpy(&name_len, raw.data() + pos, 2);
            pos += 2;
            if (name_len == 0 || pos + name_len + 14 > raw.size()) {
                if (why) *why = "truncated outer archive entry";
                return false;
            }
            KernelEntry e;
            e.name.assign(reinterpret_cast<const char*>(raw.data() + pos), name_len);
            pos += name_len;
            std::memcpy(e.wg, raw.data() + pos, 12);
            pos += 12;
            const uint64_t wg_total = uint64_t(e.wg[0]) * e.wg[1] * e.wg[2];
            if (!e.wg[0] || !e.wg[1] || !e.wg[2] || wg_total > 1024ull) {
                if (why) *why = "outer archive kernel workgroup size is invalid: " + e.name;
                return false;
            }
            uint16_t nb = 0;
            std::memcpy(&nb, raw.data() + pos, 2);
            pos += 2;
            if (pos + size_t(nb) * 8u + 4u > raw.size()) {
                if (why) *why = "truncated outer archive bindings";
                return false;
            }
            e.bindings.resize(nb);
            for (uint16_t j = 0; j < nb; ++j) {
                OuterBinding& b = e.bindings[j];
                std::memcpy(&b.binding, raw.data() + pos, 2);
                b.kind = raw[pos + 2];
                b.pad = raw[pos + 3];
                std::memcpy(&b.offset, raw.data() + pos + 4, 2);
                std::memcpy(&b.stride, raw.data() + pos + 6, 2);
                pos += 8;
                if (b.kind != kSrvTex && b.kind != kUavTex && b.kind != kBuf) {
                    if (why) *why = "outer archive kernel has unsupported binding kind: " + e.name;
                    return false;
                }
                if (b.pad != 0) {
                    if (why) *why = "outer archive kernel has non-zero binding pad: " + e.name;
                    return false;
                }
                const bool stride_ok =
                    (b.kind == kSrvTex && (b.stride == 32u || b.stride == 64u)) ||
                    (b.kind == kUavTex && b.stride == 32u) ||
                    (b.kind == kBuf && b.stride == 16u);
                if (!stride_ok) {
                    if (why) *why = "outer archive kernel binding stride mismatch: " + e.name;
                    return false;
                }
                const uint32_t bytes = b.kind == kBuf ? 16u : 32u;
                const uint32_t align = b.kind == kBuf ? 4u : 32u;
                if ((b.offset % align) != 0 || uint32_t(b.offset) + bytes > kOuterPushOff) {
                    if (why) *why = "outer archive kernel binding offset out of bounds or unaligned: " + e.name;
                    return false;
                }
                for (uint16_t k = 0; k < j; ++k) {
                    const uint32_t k_bytes = e.bindings[k].kind == kBuf ? 16u : 32u;
                    const uint32_t a0 = b.offset, a1 = uint32_t(b.offset) + bytes;
                    const uint32_t b0 = e.bindings[k].offset, b1 = uint32_t(e.bindings[k].offset) + k_bytes;
                    if (a0 < b1 && b0 < a1) {
                        if (why) *why = "outer archive kernel has overlapping descriptor bindings: " + e.name;
                        return false;
                    }
                }
            }
            const uint32_t nrp_len = elf::ru32(raw.data(), pos);
            pos += 4;
            if (nrp_len < 96u || pos + nrp_len > raw.size()) {
                if (why) *why = "truncated outer archive NRP record";
                return false;
            }
            e.nrp.assign(raw.begin() + pos, raw.begin() + pos + nrp_len);
            pos += nrp_len;
            if (by_name_.count(e.name)) {
                if (why) *why = "duplicate outer archive kernel: " + e.name;
                return false;
            }
            by_name_[e.name] = entries_.size();
            entries_.push_back(std::move(e));
        }
        gfx11_mach_ = gfx11_mach;
        loaded_path_ = path;
        loaded_ = true;
        return true;
    }

    const KernelEntry* find(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = by_name_.find(name);
        return it == by_name_.end() ? nullptr : &entries_[it->second];
    }

    bool build_kernel(ID3D12Device* device, const std::string& name, OuterKernel* out, std::string* why) const {
        KernelEntry entry;
        uint32_t gfx11 = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = by_name_.find(name);
            if (it == by_name_.end()) {
                if (why) *why = "missing outer ACO kernel: " + name;
                return false;
            }
            entry = entries_[it->second];
            gfx11 = gfx11_mach_;
        }
        if (gfx11) {
            const uint32_t known = d3d12_passthrough::gfx11_mach();
            if (known) gfx11 = known;
        }
        elf::Bytes binary;
        uint32_t dims[3]{};
        elf::OuterElfLayout layout;
        std::string err;
        if (!elf::build_model_elf_from_nrp(entry.nrp.data(), entry.nrp.size(), entry.wg,
                                           &binary, dims, &err, gfx11, kOuterPushOff, kOuterGridOff,
                                           elf::kOuterRootConstants, &layout)) {
            if (why) *why = name + ": " + err;
            return false;
        }
        uint32_t mach_used = 0;
        ID3D12PipelineState* pso = nullptr;
        if (!elf::create_pso_from_elf(device, binary.data(), binary.size(), dims, &pso, &err, &mach_used) || !pso) {
            if (why) *why = name + ": " + err;
            return false;
        }
        if (gfx11 && mach_used) d3d12_passthrough::set_gfx11_mach(mach_used);
        uint32_t desc_end = 0;
        for (const auto& b : entry.bindings) {
            const uint32_t sz = b.kind == kBuf ? 16u : 32u;
            desc_end = (std::max)(desc_end, uint32_t(b.offset) + sz);
        }
        out->pso = pso;
        std::memcpy(out->wg, dims, sizeof dims);
        out->root_push_dwords = layout.root_push_dwords;
        out->desc_zero_bytes = desc_end;
        out->needs_slot_push = layout.needs_slot_push;
        out->needs_slot_grid = layout.needs_slot_grid;
        out->bindings = std::move(entry.bindings);
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::wstring loaded_path_;
    uint32_t gfx11_mach_ = 0;
    std::vector<KernelEntry> entries_;
    std::unordered_map<std::string, size_t> by_name_;
    bool loaded_ = false;
};

inline std::shared_ptr<OuterArchive> shared_archive(bool rdna3, std::string* why) {
    static std::mutex mu;
    static std::shared_ptr<OuterArchive> archives[2];
    const size_t idx = rdna3 ? 1u : 0u;
    std::lock_guard<std::mutex> lock(mu);
    if (archives[idx]) return archives[idx];
    const std::wstring base = d3d12_passthrough::aco_folder();
    const std::wstring path = rdna3 ? (base + L"\\rdna3\\d3d12_outer.nra") : (base + L"\\d3d12_outer.nra");
    auto arch = std::make_shared<OuterArchive>();
    std::string err;
    if (!arch->load(path, rdna3 ? 0x44u : 0u, &err)) {
        if (why) *why = err;
        return nullptr;
    }
    archives[idx] = arch;
    return arch;
}

inline std::shared_ptr<OuterArchive> shared_archive(std::string* why) {
    return shared_archive(d3d12_passthrough::rdna3(), why);
}

struct OuterSlot {
    uint8_t* cpu = nullptr;
    uint64_t gpu_va = 0;
    uintptr_t srd_cpu = 0;
    uintptr_t srd_data = 0;
    std::mutex* srd_mu = nullptr;
    mutable uint32_t rc[elf::kOuterRootConstants]{};

    void zero(uint32_t bytes = kSlotBytes) const {
        const uint32_t n = (std::min)(bytes, kSlotBytes);
        if (n) std::memset(cpu, 0, n);
    }

    void write_srv_tex(ID3D12Device* dev, const OuterBinding* b, ID3D12Resource* res, DXGI_FORMAT fmt,
                       uint32_t most_detailed_mip = 0, uint32_t mip_levels = 1) const {
        if (!b || b->kind == kBuf || !srd_cpu || !srd_data || !srd_mu) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = fmt;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MostDetailedMip = most_detailed_mip;
        d.Texture2D.MipLevels = mip_levels;
        std::lock_guard<std::mutex> lock(*srd_mu);
        dev->CreateShaderResourceView(res, &d, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(srd_cpu)});
        std::memcpy(cpu + b->offset, reinterpret_cast<const void*>(srd_data), 32u);
    }

    void write_uav_tex(ID3D12Device* dev, const OuterBinding* b, ID3D12Resource* res, DXGI_FORMAT fmt,
                       uint32_t mip_slice = 0) const {
        if (!b || b->kind == kBuf || !srd_cpu || !srd_data || !srd_mu) return;
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = fmt;
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = mip_slice;
        std::lock_guard<std::mutex> lock(*srd_mu);
        dev->CreateUnorderedAccessView(res, nullptr, &d, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(srd_cpu)});
        std::memcpy(cpu + b->offset, reinterpret_cast<const void*>(srd_data), 32u);
    }

    void write_buf(const OuterBinding* b, ID3D12Resource* res, uint64_t bytes = 0) const {
        if (!b || b->kind != kBuf || !res) return;
        const uint64_t va = res->GetGPUVirtualAddress();
        const uint64_t sz = bytes ? bytes : res->GetDesc().Width;
        elf::put_buf(reinterpret_cast<uint32_t*>(cpu + b->offset), va, sz);
    }

    void write_push(const void* data, uint32_t bytes, const OuterKernel* k = nullptr) const {
        if (!data || !bytes) return;
        const uint32_t rc_n = k ? k->root_push_dwords : elf::kOuterRootConstants;
        if (rc_n) {
            std::memset(rc, 0, sizeof(uint32_t) * rc_n);
            std::memcpy(rc, data, (std::min)(bytes, rc_n * 4u));
        }
        if (!k || k->needs_slot_push)
            std::memcpy(cpu + kOuterPushOff, data, (std::min)(bytes, kMaxPushBytes));
    }

    void dispatch(ID3D12GraphicsCommandList* cmd, ID3D12RootSignature* rs_outer, const OuterKernel& k,
                  uint32_t gx, uint32_t gy = 1, uint32_t gz = 1, bool set_rs = true) const {
        if (k.needs_slot_grid) {
            const uint32_t grid[3] = {gx, gy, gz};
            std::memcpy(cpu + kOuterGridOff, grid, sizeof grid);
        }
        if (set_rs) cmd->SetComputeRootSignature(rs_outer);
        cmd->SetPipelineState(k.pso);
        if (k.root_push_dwords) cmd->SetComputeRoot32BitConstants(0, k.root_push_dwords, rc, 0);
        cmd->SetComputeRootUnorderedAccessView(1, gpu_va);
        cmd->Dispatch(gx, gy, gz);
    }
};

class OuterHeapPool {
public:
    using CompletedFn = std::function<bool(const nr::gpu::Ticket&)>;
    using RetireFn = std::function<void(IUnknown*)>;

    OuterHeapPool() = default;
    ~OuterHeapPool() { reset(); }

    OuterHeapPool(const OuterHeapPool&) = delete;
    OuterHeapPool& operator=(const OuterHeapPool&) = delete;

    bool init(ID3D12Device* device, uint32_t slots_per_block, CompletedFn completed, RetireFn retire,
              std::string* why) {
        reset();
        device_ = device;
        slots_per_block_ = (std::max)(slots_per_block, 16u);
        completed_ = std::move(completed);
        retire_ = std::move(retire);
        if (!device_) {
            if (why) *why = "outer descriptor pool: null device";
            return false;
        }
        const UINT inc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (inc != 32u) {
            if (why) *why = "outer descriptor pool: unexpected descriptor increment " + std::to_string(inc);
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 4;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        const HRESULT hr_cpu = device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srd_heap_));
        if (FAILED(hr_cpu) || !srd_heap_) {
            if (why) *why = "CreateDescriptorHeap(CPU staging) " + elf::hr_hex(hr_cpu);
            return false;
        }
        srd_cpu_ = uintptr_t(srd_heap_->GetCPUDescriptorHandleForHeapStart().ptr);
        if (!readable_ptr(srd_cpu_, 32u)) {
            srd_heap_->Release();
            srd_heap_ = nullptr;
            srd_cpu_ = 0;
            srd_data_ = 0;
            if (why) *why = "CPU descriptor staging heap handle is opaque";
            return false;
        }
        if (!verify_srd_layout(&srd_data_, why)) {
            srd_heap_->Release();
            srd_heap_ = nullptr;
            srd_cpu_ = 0;
            srd_data_ = 0;
            return false;
        }
        if (!add_block(why)) {
            srd_heap_->Release();
            srd_heap_ = nullptr;
            srd_cpu_ = 0;
            srd_data_ = 0;
            return false;
        }
        return true;
    }

    bool indirect_srd() const { return srd_data_ != 0 && srd_data_ != srd_cpu_; }

    void set_callbacks(CompletedFn completed, RetireFn retire) {
        completed_ = std::move(completed);
        retire_ = std::move(retire);
    }

    void reset() {
        for (Block& b : blocks_) {
            if (!b.upload) continue;
            if (b.cpu_base) b.upload->Unmap(0, nullptr);
            if (retire_) retire_(b.upload);
            else b.upload->Release();
            b.upload = nullptr;
            b.cpu_base = 0;
        }
        blocks_.clear();
        if (srd_heap_) {
            if (retire_) retire_(srd_heap_);
            else srd_heap_->Release();
            srd_heap_ = nullptr;
        }
        srd_cpu_ = 0;
        srd_data_ = 0;
        device_ = nullptr;
        peak_blocks_ = 0;
    }

    OuterSlot acquire(const nr::gpu::Ticket& ticket, std::string* why = nullptr, uint32_t zero_bytes = kSlotBytes) {
        for (Block& b : blocks_) {
            OuterSlot s;
            if (try_claim(b, ticket, &s, zero_bytes)) return s;
        }
        std::string err;
        if (!add_block(&err)) {
            if (why) *why = err;
            return {};
        }
        OuterSlot s;
        if (try_claim(blocks_.back(), ticket, &s, zero_bytes)) return s;
        if (why) *why = "outer descriptor pool: fresh block has no aligned slot";
        return {};
    }

    bool reserve(uint32_t count, std::string* why = nullptr) {
        uint32_t free_slots = count_free_slots();
        while (free_slots < count) {
            if (!add_block(why)) return false;
            free_slots = count_free_slots();
        }
        return true;
    }

    void set_max_blocks(uint32_t max_blocks) { max_blocks_ = (std::max)(max_blocks, 1u); }
    size_t block_count() const { return blocks_.size(); }
    size_t peak_blocks() const { return peak_blocks_; }

private:
    struct Block {
        ID3D12Resource* upload = nullptr;
        uintptr_t cpu_base = 0;
        uint64_t gpu_base = 0;
        std::vector<nr::gpu::Ticket> owners;
        std::vector<uint8_t> used;
        uint32_t next = 0;
    };

    static bool readable_ptr(uintptr_t ptr, size_t bytes) {
        if (!ptr) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(ptr), &mbi, sizeof mbi) == 0) return false;
        constexpr DWORD kReadable =
            PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & kReadable) == 0) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        return uintptr_t(mbi.BaseAddress) + mbi.RegionSize >= ptr + bytes;
    }

    static bool all_zero_32(uintptr_t p) {
        uint32_t w[8]{};
        std::memcpy(w, reinterpret_cast<const void*>(p), 32u);
        for (uint32_t v : w) {
            if (v != 0) return false;
        }
        return true;
    }

    // Confirm CPU staging SRDs match SHADER_VISIBLE hardware SRDs bit-for-bit (direct or 1-level indirect).
    bool verify_srd_layout(uintptr_t* out_srd_data, std::string* why) const {
        if (!out_srd_data) return false;
        *out_srd_data = 0;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12DescriptorHeap* vis = nullptr;
        HRESULT hr = device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&vis));
        if (FAILED(hr) || !vis) {
            if (why) *why = "CreateDescriptorHeap(SRD probe) " + elf::hr_hex(hr);
            return false;
        }
        const uintptr_t vis_cpu = uintptr_t(vis->GetCPUDescriptorHandleForHeapStart().ptr);
        if (!readable_ptr(vis_cpu, 32u)) {
            vis->Release();
            if (why) *why = "shader-visible descriptor heap handle is opaque";
            return false;
        }
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 1;
        td.Height = 1;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* probe_tex = nullptr;
        hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON,
                                              nullptr, IID_PPV_ARGS(&probe_tex));
        td.Format = DXGI_FORMAT_R32_TYPELESS;
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource* probe_depth = nullptr;
        HRESULT hr_d = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON,
                                                        nullptr, IID_PPV_ARGS(&probe_depth));
        if (FAILED(hr) || !probe_tex || FAILED(hr_d) || !probe_depth) {
            if (probe_tex) probe_tex->Release();
            if (probe_depth) probe_depth->Release();
            vis->Release();
            if (why) *why = "CreateCommittedResource(SRD probe) " + elf::hr_hex(FAILED(hr) ? hr : hr_d);
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv16f{};
        srv16f.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv16f.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv16f.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv16f.Texture2D.MipLevels = 1;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav16f{};
        uav16f.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav16f.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv32d = srv16f;
        srv32d.Format = DXGI_FORMAT_R32_FLOAT;

        auto probe_pair = [&](uintptr_t cpu_data, uintptr_t vis_data, bool indirect) -> bool {
            auto intact = [&]() -> bool {
                if (!indirect) return true;
                uintptr_t cur_cpu = 0, cur_vis = 0;
                std::memcpy(&cur_cpu, reinterpret_cast<const void*>(srd_cpu_), sizeof(cur_cpu));
                std::memcpy(&cur_vis, reinterpret_cast<const void*>(vis_cpu), sizeof(cur_vis));
                return cur_cpu == cpu_data && cur_vis == vis_data;
            };
            auto eq_nonzero = [&]() -> bool {
                return !all_zero_32(cpu_data) &&
                       std::memcmp(reinterpret_cast<const void*>(cpu_data),
                                   reinterpret_cast<const void*>(vis_data), 32u) == 0;
            };

            device_->CreateShaderResourceView(probe_tex, &srv16f, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(srd_cpu_)});
            device_->CreateShaderResourceView(probe_tex, &srv16f, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(vis_cpu)});
            MemoryBarrier();
            if (!intact() || !eq_nonzero()) return false;
            uint8_t srv16f_ref[32]{};
            std::memcpy(srv16f_ref, reinterpret_cast<const void*>(cpu_data), 32u);

            device_->CreateUnorderedAccessView(probe_tex, nullptr, &uav16f, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(srd_cpu_)});
            device_->CreateUnorderedAccessView(probe_tex, nullptr, &uav16f, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(vis_cpu)});
            MemoryBarrier();
            if (!intact() || !eq_nonzero()) return false;

            device_->CreateShaderResourceView(probe_depth, &srv32d, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(srd_cpu_)});
            device_->CreateShaderResourceView(probe_depth, &srv32d, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(vis_cpu)});
            MemoryBarrier();
            if (!intact() || !eq_nonzero() ||
                std::memcmp(reinterpret_cast<const void*>(cpu_data), srv16f_ref, 32u) == 0) {
                return false;
            }
            return true;
        };

        bool ok = false;
        if (probe_pair(srd_cpu_, vis_cpu, false)) {
            *out_srd_data = srd_cpu_;
            ok = true;
        } else {
            uintptr_t ind_cpu = 0, ind_vis = 0;
            std::memcpy(&ind_cpu, reinterpret_cast<const void*>(srd_cpu_), sizeof(ind_cpu));
            std::memcpy(&ind_vis, reinterpret_cast<const void*>(vis_cpu), sizeof(ind_vis));
            const bool ind_eligible =
                ind_cpu != 0 && ind_vis != 0 && ind_cpu != ind_vis &&
                ind_cpu != srd_cpu_ && ind_vis != vis_cpu &&
                (ind_cpu % 4u) == 0 && (ind_vis % 4u) == 0 &&
                readable_ptr(ind_cpu, 32u) && readable_ptr(ind_vis, 32u);
            if (ind_eligible && probe_pair(ind_cpu, ind_vis, true)) {
                *out_srd_data = ind_cpu;
                ok = true;
            }
        }
        if (!ok && why) *why = "CPU and shader-visible hardware SRD encodings differ";
        probe_depth->Release();
        probe_tex->Release();
        vis->Release();
        return ok;
    }

    uint32_t count_free_slots() const {
        uint32_t n_free = 0;
        for (const Block& b : blocks_) {
            for (uint32_t idx = 0; idx < slots_per_block_; ++idx) {
                const uint64_t va = b.gpu_base + uint64_t(idx) * kSlotBytes;
                if ((va >> 32) != ((va + kSlotBytes - 1u) >> 32)) continue;
                if (b.used[idx]) {
                    if (!b.owners[idx]) continue;
                    const bool done = completed_ ? completed_(b.owners[idx]) : b.owners[idx]->ready();
                    if (!done) continue;
                }
                ++n_free;
            }
        }
        return n_free;
    }

    bool add_block(std::string* why) {
        if (blocks_.size() >= max_blocks_) {
            if (why) *why = "outer descriptor pool reached max block cap (" + std::to_string(max_blocks_) + ")";
            return false;
        }
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = uint64_t(slots_per_block_) * kSlotBytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* upload = nullptr;
        HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr, IID_PPV_ARGS(&upload));
        if (FAILED(hr) || !upload) {
            if (why) *why = "CreateCommittedResource(outer upload) " + elf::hr_hex(hr);
            return false;
        }
        void* mapped = nullptr;
        const D3D12_RANGE none{0, 0};
        hr = upload->Map(0, &none, &mapped);
        if (FAILED(hr) || !mapped) {
            upload->Release();
            if (why) *why = "Map(outer upload) " + elf::hr_hex(hr);
            return false;
        }
        Block b;
        b.upload = upload;
        b.cpu_base = uintptr_t(mapped);
        b.gpu_base = upload->GetGPUVirtualAddress();
        b.owners.resize(slots_per_block_);
        b.used.assign(slots_per_block_, 0u);
        blocks_.push_back(std::move(b));
        peak_blocks_ = (std::max)(peak_blocks_, blocks_.size());
        return true;
    }

    bool try_claim(Block& b, const nr::gpu::Ticket& ticket, OuterSlot* out, uint32_t zero_bytes) {
        for (uint32_t n = 0; n < slots_per_block_; ++n) {
            const uint32_t idx = (b.next + n) % slots_per_block_;
            const uint64_t va = b.gpu_base + uint64_t(idx) * kSlotBytes;
            // S_GETPC_B64 sets M0 to a single high 32-bit word for the whole 2 KB slot.
            if ((va >> 32) != ((va + kSlotBytes - 1u) >> 32)) continue;
            if (b.used[idx]) {
                if (!b.owners[idx]) continue;
                const bool done = completed_ ? completed_(b.owners[idx]) : b.owners[idx]->ready();
                if (!done) continue;
            }
            b.used[idx] = 1u;
            b.owners[idx] = ticket;
            b.next = (idx + 1u) % slots_per_block_;
            out->cpu = reinterpret_cast<uint8_t*>(b.cpu_base + uintptr_t(idx) * kSlotBytes);
            out->gpu_va = va;
            out->srd_cpu = srd_cpu_;
            out->srd_data = srd_data_;
            out->srd_mu = &srd_mutex_;
            out->zero(zero_bytes);
            return true;
        }
        return false;
    }

    ID3D12Device* device_ = nullptr;
    ID3D12DescriptorHeap* srd_heap_ = nullptr;
    uintptr_t srd_cpu_ = 0;
    uintptr_t srd_data_ = 0;
    std::mutex srd_mutex_;
    uint32_t slots_per_block_ = 512;
    uint32_t max_blocks_ = 64;
    size_t peak_blocks_ = 0;
    CompletedFn completed_;
    RetireFn retire_;
    std::vector<Block> blocks_;
};

}  // namespace nr::pe::d3d12_outer
