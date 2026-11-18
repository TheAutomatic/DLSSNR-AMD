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
    return elf::create_carrier_root_signature(device, 1, out_rs, why);
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
            }
            const uint32_t nrp_len = elf::ru32(raw.data(), pos);
            pos += 4;
            if (nrp_len < 96u || pos + nrp_len > raw.size()) {
                if (why) *why = "truncated outer archive NRP record";
                return false;
            }
            e.nrp.assign(raw.begin() + pos, raw.begin() + pos + nrp_len);
            pos += nrp_len;
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
        std::string err;
        if (!elf::build_model_elf_from_nrp(entry.nrp.data(), entry.nrp.size(), entry.wg,
                                           &binary, dims, &err, gfx11, kOuterPushOff, kOuterGridOff)) {
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
        out->pso = pso;
        std::memcpy(out->wg, dims, sizeof dims);
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
    std::mutex* srd_mu = nullptr;

    void zero() const { std::memset(cpu, 0, kSlotBytes); }

    void write_srv_tex(ID3D12Device* dev, const OuterBinding* b, ID3D12Resource* res, DXGI_FORMAT fmt,
                       uint32_t most_detailed_mip = 0, uint32_t mip_levels = 1) const {
        if (!b || b->kind == kBuf || !srd_cpu || !srd_mu) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = fmt;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MostDetailedMip = most_detailed_mip;
        d.Texture2D.MipLevels = mip_levels;
        std::lock_guard<std::mutex> lock(*srd_mu);
        dev->CreateShaderResourceView(res, &d, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(srd_cpu)});
        std::memcpy(cpu + b->offset, reinterpret_cast<const void*>(srd_cpu), 32u);
    }

    void write_uav_tex(ID3D12Device* dev, const OuterBinding* b, ID3D12Resource* res, DXGI_FORMAT fmt,
                       uint32_t mip_slice = 0) const {
        if (!b || b->kind == kBuf || !srd_cpu || !srd_mu) return;
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = fmt;
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = mip_slice;
        std::lock_guard<std::mutex> lock(*srd_mu);
        dev->CreateUnorderedAccessView(res, nullptr, &d, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(srd_cpu)});
        std::memcpy(cpu + b->offset, reinterpret_cast<const void*>(srd_cpu), 32u);
    }

    void write_buf(const OuterBinding* b, ID3D12Resource* res, uint64_t bytes = 0) const {
        if (!b || b->kind != kBuf || !res) return;
        const uint64_t va = res->GetGPUVirtualAddress();
        const uint64_t sz = bytes ? bytes : res->GetDesc().Width;
        elf::put_buf(reinterpret_cast<uint32_t*>(cpu + b->offset), va, sz);
    }

    void write_push(const void* data, uint32_t bytes) const {
        if (data && bytes) std::memcpy(cpu + kOuterPushOff, data, (std::min)(bytes, kMaxPushBytes));
    }

    void dispatch(ID3D12GraphicsCommandList* cmd, ID3D12RootSignature* rs_outer, const OuterKernel& k,
                  uint32_t gx, uint32_t gy = 1, uint32_t gz = 1, bool set_rs = true) const {
        const uint32_t grid[3] = {gx, gy, gz};
        std::memcpy(cpu + kOuterGridOff, grid, sizeof grid);
        if (set_rs) cmd->SetComputeRootSignature(rs_outer);
        cmd->SetPipelineState(k.pso);
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
            if (why) *why = "CPU descriptor staging heap handle is opaque";
            return false;
        }
        if (!verify_srd_layout(why)) {
            srd_heap_->Release();
            srd_heap_ = nullptr;
            srd_cpu_ = 0;
            return false;
        }
        if (!add_block(why)) {
            srd_heap_->Release();
            srd_heap_ = nullptr;
            srd_cpu_ = 0;
            return false;
        }
        return true;
    }

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
        device_ = nullptr;
    }

    OuterSlot acquire(const nr::gpu::Ticket& ticket, std::string* why = nullptr) {
        for (Block& b : blocks_) {
            OuterSlot s;
            if (try_claim(b, ticket, &s)) return s;
        }
        std::string err;
        if (!add_block(&err)) {
            if (why) *why = err;
            return {};
        }
        OuterSlot s;
        if (try_claim(blocks_.back(), ticket, &s)) return s;
        if (why) *why = "outer descriptor pool: fresh block has no aligned slot";
        return {};
    }

    size_t block_count() const { return blocks_.size(); }

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

    // Confirm CPU-only SRDs match SHADER_VISIBLE SRDs bit-for-bit before trusting them for GPU execution.
    bool verify_srd_layout(std::string* why) const {
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
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 1;
        td.Height = 1;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        ID3D12Resource* probe = nullptr;
        hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON,
                                              nullptr, IID_PPV_ARGS(&probe));
        if (FAILED(hr) || !probe) {
            vis->Release();
            if (why) *why = "CreateCommittedResource(SRD probe) " + elf::hr_hex(hr);
            return false;
        }
        const uintptr_t vis_cpu = uintptr_t(vis->GetCPUDescriptorHandleForHeapStart().ptr);
        if (!readable_ptr(vis_cpu, 32u)) {
            probe->Release();
            vis->Release();
            if (why) *why = "shader-visible descriptor heap handle is opaque";
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(probe, &srv, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(srd_cpu_)});
        device_->CreateShaderResourceView(probe, &srv, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(vis_cpu)});
        const bool same = std::memcmp(reinterpret_cast<const void*>(srd_cpu_),
                                      reinterpret_cast<const void*>(vis_cpu), 32u) == 0;
        probe->Release();
        vis->Release();
        if (!same) {
            if (why) *why = "CPU and shader-visible hardware SRD encodings differ";
            return false;
        }
        return true;
    }

    bool add_block(std::string* why) {
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
        return true;
    }

    bool try_claim(Block& b, const nr::gpu::Ticket& ticket, OuterSlot* out) {
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
            out->srd_mu = &srd_mutex_;
            out->zero();
            return true;
        }
        return false;
    }

    ID3D12Device* device_ = nullptr;
    ID3D12DescriptorHeap* srd_heap_ = nullptr;
    uintptr_t srd_cpu_ = 0;
    std::mutex srd_mutex_;
    uint32_t slots_per_block_ = 512;
    CompletedFn completed_;
    RetireFn retire_;
    std::vector<Block> blocks_;
};

}  // namespace nr::pe::d3d12_outer
