// The wilsjo2 stage on the native D3D12 route: pre-captured PAL ELF outer kernels (d3d12_outer.nra)
// executed directly on the game's command list with mapped upload descriptor slots and compute-only
// state restoration (zero SetDescriptorHeaps calls, zero d3dcompiler_47 dependency).
#pragma once

#include "nr_d3d12_direct_state.hpp"
#include "nr_d3d12_il.hpp"
#include "nr_d3d12_outer_elf.hpp"
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
namespace d3d12_il = nr::pe::d3d12_il;
namespace d3d12_outer = nr::pe::d3d12_outer;

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
    {w2::kHudCompose, "hudfix_compose", false},
};

class Stage12 final : public w2::Stage::Backend {
public:
    static constexpr UINT kStride = 256;
    static_assert(sizeof(w2::Constants) <= kStride, "the constants fit the block");
    using Ensure = std::function<void(ID3D12Resource*, D3D12_RESOURCE_STATES)>;

    ~Stage12() override {
        direct_scope_.dismiss();
        outer_pool_.reset();
        for (auto& bank : il_kernels_) for (auto& k : bank) if (k.pso) { k.pso->Release(); k.pso = nullptr; }
        for (auto& k : fast_kernels_) if (k.pso) { k.pso->Release(); k.pso = nullptr; }
        for (auto& k : kernels_) if (k.pso) { k.pso->Release(); k.pso = nullptr; }
        for (auto* c : copies_) c->Release();
        copies_.clear();
        if (rs_outer_) { rs_outer_->Release(); rs_outer_ = nullptr; }
    }

    // Loads all wilsjo2, fast, and Interleaved PAL ELF pipelines from d3d12_outer.nra onto `device`.
    bool make(ID3D12Device* device, const std::wstring& /*folder*/, std::string* why) {
        device_ = device;
        if (!d3d12_outer::create_outer_root_signature(device, &rs_outer_, why)) return false;
        auto arch = d3d12_outer::shared_archive(why);
        if (!arch) return false;

        {
            std::string err;
            if (!arch->build_kernel(device, "wilsjo2/half_copy", &kernels_[w2::kHalfCopy], &err))
                NR_W2D3D12_LOG("[d3d12] wilsjo2 on D3D12: wilsjo2/half_copy %s", err.c_str());
        }
        {
            const char* const fast_keys[] = {
                "renodx/renodx_st_fb_in", "renodx/renodx_st_prep", "renodx/renodx_st_box", "renodx/renodx_st_apply",
            };
            for (uint32_t i = 0; i < nr::fast::kProgramCount; ++i) {
                std::string err;
                if (!arch->build_kernel(device, fast_keys[i], &fast_kernels_[i], &err))
                    NR_W2D3D12_LOG("[d3d12] wilsjo2 on D3D12: %s %s", fast_keys[i], err.c_str());
            }
        }
        {
            std::string err;
            if (!load_il(*arch, &err))
                NR_W2D3D12_LOG("[d3d12] wilsjo2 on D3D12: Interleaved's programs are not available (%s): Interleaved cannot run",
                               err.c_str());
        }

        std::map<std::string, d3d12_outer::OuterKernel> made;
        struct MadeRelease {
            std::map<std::string, d3d12_outer::OuterKernel>* made;
            ~MadeRelease() { for (auto& kv : *made) if (kv.second.pso) kv.second.pso->Release(); }
        } made_release{&made};

        for (const ProgramFile& pf : kPrograms) {
            auto it = made.find(pf.name);
            if (it == made.end()) {
                d3d12_outer::OuterKernel k;
                std::string err;
                const std::string key = std::string("wilsjo2/") + pf.name;
                if (!arch->build_kernel(device, key, &k, &err)) {
                    if (pf.required) { *why = key + " " + err; return false; }
                    NR_W2D3D12_LOG("[d3d12] wilsjo2 on D3D12: %s %s (its feature takes the bridge)", key.c_str(), err.c_str());
                }
                it = made.emplace(pf.name, std::move(k)).first;
            }
            if (it->second.pso) {
                if (kernels_[pf.mode].pso) kernels_[pf.mode].pso->Release();
                kernels_[pf.mode] = it->second;
                kernels_[pf.mode].pso->AddRef();
            }
        }

        return outer_pool_.init(
            device, 512,
            [this](const nr::gpu::Ticket& t) { return done(t); },
            nullptr, why);
    }

    // Begins a compute recording block on `cmd`, saving host compute state to restore in end().
    void begin(ID3D12GraphicsCommandList* cmd, Ensure ensure) {
        cmd_ = cmd;
        ensure_ = std::move(ensure);
        direct_scope_ = nr::pe::d3d12_direct::Scope(cmd);
    }
    void end() {
        direct_scope_.release();
        cmd_ = nullptr;
        ensure_ = nullptr;
    }
    void end_recording() override { sweep_copies(); }

    void sweep_copies() {
        ++recordings_;
        for (size_t i = 0; i < copies_.size();) {
            ID3D12Resource* c = copies_[i];
            const auto u = copy_use_.find(c);
            const bool idle = u == copy_use_.end() || (recordings_ - u->second.recording > 240 && done(u->second.ticket));
            if (!idle) { ++i; continue; }
            copy_use_.erase(c); copy_dest_.erase(c);
            c->Release();
            copies_.erase(copies_.begin() + long(i));
        }
    }
    bool has_fast() const override {
        return fast_kernels_[0].pso && fast_kernels_[1].pso && fast_kernels_[2].pso && fast_kernels_[3].pso &&
               kernels_[w2::kHalfCopy].pso;
    }
    bool has(w2::Mode mode) const override { return mode < kModes && kernels_[mode].pso != nullptr; }
    bool has_il() const override { return il_loaded_ && has_fast(); }

    void record_il(uint32_t program, uint32_t step, const w2::Stage::FastImage* images, uint32_t count, const void* push,
                   uint32_t bytes, uint32_t gx, uint32_t gy) override {
        if (!cmd_) throw std::runtime_error("wilsjo2 stage (D3D12): Interleaved recorded outside a recording");
        if (!il_loaded_ || program > 2u || step >= 7u || !il_kernels_[program][step].pso || !rs_outer_)
            throw std::runtime_error("wilsjo2 stage (D3D12): no Interleaved program " + std::to_string(program) + "/" + std::to_string(step));
        const d3d12_outer::OuterKernel& k = il_kernels_[program][step];
        std::vector<d3d12_il::Bind> binds;
        for (uint32_t i = 0; i < count; ++i) {
            ID3D12Resource* r = resource(images[i].image);
            const uint32_t b = images[i].binding;
            if (program != 1u && step != 0u && (b == 44u || b == 45u)) continue;
            const bool write = d3d12_il::has_binding(k, b, true);
            const bool read = d3d12_il::has_binding(k, b, false);
            if (!r || (!write && !read)) continue;
            const D3D12_RESOURCE_DESC rd = r->GetDesc();
            const DXGI_FORMAT f = write ? uav_format(rd.Format) : srv_format(rd.Format);
            if (f == DXGI_FORMAT_UNKNOWN || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                (write && !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
                throw std::runtime_error("wilsjo2 stage (D3D12): an Interleaved image it cannot " + std::string(write ? "write" : "read") +
                                         " (format " + std::to_string(int(rd.Format)) + ")");
            ensure_(r, write ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            binds.push_back({b, r, f, write});
        }
        const d3d12_outer::OuterSlot slot = acquire_slot();
        d3d12_il::write_outer_binds(device_, slot, k, binds.data(), binds.size());
        slot.write_push(push, bytes);
        slot.dispatch(cmd_, rs_outer_, k, gx, gy, 1u, true);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;
        cmd_->ResourceBarrier(1, &b);
        ++dispatches_;
    }

    void record(const w2::Dispatch& d) override {
        const uint32_t mode = d.constants.mode;
        if (!cmd_) throw std::runtime_error("wilsjo2 stage (D3D12): recorded outside a recording");
        if (mode >= kModes || !kernels_[mode].pso || !rs_outer_)
            throw std::runtime_error("wilsjo2 stage (D3D12): no program for mode " + std::to_string(mode));
        if (!d.t0 || !d.u0) throw std::runtime_error("wilsjo2 stage (D3D12): a dispatch without t0 or u0");
        const d3d12_outer::OuterKernel& k = kernels_[mode];

        // Spec 6.3: a read slot without a resource is given t0's, a missing u1 is given u0's.
        w2::Image read_images[4] = {d.t0, d.t1 ? d.t1 : d.t0, d.t2 ? d.t2 : d.t0, d.t3 ? d.t3 : d.t0};
        const w2::Image* reads[4] = {&read_images[0], &read_images[1], &read_images[2], &read_images[3]};
        const w2::Image* writes[2] = {&d.u0, d.u1 ? &d.u1 : &d.u0};
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

        const d3d12_outer::OuterSlot slot = acquire_slot();
        for (UINT i = 0; i < 4; ++i) {
            ID3D12Resource* r = resource(*reads[i]);
            const D3D12_RESOURCE_DESC rd = r->GetDesc();
            const DXGI_FORMAT fmt = srv_format(rd.Format);
            if (fmt == DXGI_FORMAT_UNKNOWN || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.SampleDesc.Count != 1)
                throw std::runtime_error("wilsjo2 stage (D3D12): an image it cannot read (format " + std::to_string(int(rd.Format)) + ")");
            if (reads[i]->layer != 0 || reads[i]->mip >= rd.MipLevels)
                throw std::runtime_error("wilsjo2 stage (D3D12): a read of an array layer or a missing mip level");
            if (const auto* ob = k.find_binding(i))
                slot.write_srv_tex(device_, ob, r, fmt, reads[i]->mip, 1);
        }
        for (UINT i = 0; i < 2; ++i) {
            ID3D12Resource* r = resource(*writes[i]);
            const D3D12_RESOURCE_DESC rd = r->GetDesc();
            const DXGI_FORMAT fmt = uav_format(rd.Format);
            if (fmt == DXGI_FORMAT_UNKNOWN || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                throw std::runtime_error("wilsjo2 stage (D3D12): an image it cannot write (format " + std::to_string(int(rd.Format)) + ")");
            if (const auto* ob = k.find_binding(4u + i))
                slot.write_uav_tex(device_, ob, r, fmt, 0);
        }
        slot.write_push(&d.constants, sizeof(w2::Constants));

        const uint32_t gx = mode == w2::kMeter ? d.constants.width : mode == w2::kPrepMeter ? 1u : (d.constants.width + 7u) / 8u;
        const uint32_t gy = mode == w2::kMeter ? d.constants.height : mode == w2::kPrepMeter ? 1u : (d.constants.height + 7u) / 8u;
        slot.dispatch(cmd_, rs_outer_, k, gx, gy, 1u, true);

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
        if (program >= nr::fast::kProgramCount || !fast_kernels_[program].pso || !rs_outer_)
            throw std::runtime_error("wilsjo2 stage (D3D12): fast program " + std::to_string(program) + " not ready");
        const d3d12_outer::OuterKernel& k = fast_kernels_[program];

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

        const d3d12_outer::OuterSlot slot = acquire_slot();
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t b = images[i].binding;
            if (b >= 8u) continue;
            ID3D12Resource* r = resource(images[i].image);
            if (!r) continue;
            const D3D12_RESOURCE_DESC rd = r->GetDesc();
            if (is_uav(program, b)) {
                const DXGI_FORMAT fmt = uav_format(rd.Format);
                if (fmt == DXGI_FORMAT_UNKNOWN || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                    !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                    throw std::runtime_error("wilsjo2 stage (D3D12): fast image cannot be UAV written (format " + std::to_string(int(rd.Format)) + ")");
                if (const auto* ob = k.find_binding(b))
                    slot.write_uav_tex(device_, ob, r, fmt, 0);
            } else {
                const DXGI_FORMAT fmt = srv_format(rd.Format);
                if (fmt == DXGI_FORMAT_UNKNOWN || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.SampleDesc.Count != 1)
                    throw std::runtime_error("wilsjo2 stage (D3D12): fast image cannot be SRV read (format " + std::to_string(int(rd.Format)) + ")");
                if (const auto* ob = k.find_binding(b))
                    slot.write_srv_tex(device_, ob, r, fmt, images[i].image.mip, 1);
            }
        }
        slot.write_push(push, bytes);
        slot.dispatch(cmd_, rs_outer_, k, gx, gy, 1u, true);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;
        cmd_->ResourceBarrier(1, &b);
        ++dispatches_;
    }

public:
    using TicketOf = std::function<nr::gpu::Ticket(ID3D12GraphicsCommandList*)>;
    using DoneOf = std::function<bool(const nr::gpu::Ticket&)>;
    void set_tickets(TicketOf t, DoneOf d) { ticket_of_ = std::move(t); done_of_ = std::move(d); }
    const nr::gpu::Uses& uses() const { return uses_; }

private:
    d3d12_outer::OuterKernel il_kernels_[3][7]{};   // P2, pyramid, P1
    bool il_loaded_ = false;

    bool load_il(d3d12_outer::OuterArchive& arch, std::string* why) {
        for (uint32_t step : {0u, 1u, 3u, 4u, 5u, 6u}) {
            if (!arch.build_kernel(device_, "il/il_" + std::to_string(step), &il_kernels_[0][step], why)) return false;
        }
        if (!arch.build_kernel(device_, "il/il_pp", &il_kernels_[1][0], why)) return false;
        for (uint32_t step : {0u, 1u, 3u, 4u, 5u}) {
            if (!arch.build_kernel(device_, "il/il0_" + std::to_string(step), &il_kernels_[2][step], why)) return false;
        }
        il_loaded_ = true;
        return true;
    }

    nr::gpu::Ticket ticket_here() const { return ticket_of_ ? ticket_of_(cmd_) : nr::gpu::current(); }
    bool done(const nr::gpu::Ticket& t) const {
        if (!t) return false;
        return done_of_ ? done_of_(t) : t->ready();
    }
    void prune(nr::gpu::Uses& u) const {
        std::vector<nr::gpu::Ticket> keep;
        keep.reserve(u.tickets.size());
        for (auto& t : u.tickets) {
            if (done(t)) continue;
            if (std::find(keep.begin(), keep.end(), t) == keep.end()) keep.push_back(t);
        }
        u.tickets.swap(keep);
    }
    d3d12_outer::OuterSlot acquire_slot() {
        const nr::gpu::Ticket ticket = ticket_here();
        prune(uses_);
        if (ticket && std::find(uses_.tickets.begin(), uses_.tickets.end(), ticket) == uses_.tickets.end())
            uses_.tickets.push_back(ticket);
        std::string err;
        d3d12_outer::OuterSlot slot = outer_pool_.acquire(ticket, &err);
        if (!slot.cpu)
            throw std::runtime_error("wilsjo2 stage (D3D12): " + (err.empty() ? "no outer descriptor slot" : err));
        return slot;
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
        copy_use_[c] = CopyUse{ticket_here(), recordings_};
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
    struct CopyUse { nr::gpu::Ticket ticket; uint64_t recording = 0; };
    std::map<ID3D12Resource*, CopyUse> copy_use_;   // each copy's last use (sweep_copies)
    uint64_t recordings_ = 0;
    static constexpr uint32_t kModes = 128;
    ID3D12Device* device_ = nullptr;
    ID3D12RootSignature* rs_outer_ = nullptr;
    d3d12_outer::OuterKernel kernels_[kModes]{};
    d3d12_outer::OuterKernel fast_kernels_[nr::fast::kProgramCount]{};
    d3d12_outer::OuterHeapPool outer_pool_;
    nr::pe::d3d12_direct::Scope direct_scope_;
    TicketOf ticket_of_;
    DoneOf done_of_;
    nr::gpu::Uses uses_;
    uint64_t dispatches_ = 0;
    ID3D12GraphicsCommandList* cmd_ = nullptr;
    Ensure ensure_;
};

}  // namespace nr::asi::native
