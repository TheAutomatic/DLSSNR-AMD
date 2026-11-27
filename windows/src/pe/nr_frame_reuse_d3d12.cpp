// Frame reuse on the D3D12 passthrough (nr_frame_reuse_d3d12.hpp): windows/src/core/nr_frame_reuse.cpp's record() on
// the path the product takes (the cadence decided on the CPU, fast_speed 0; no GPU decision, no conditional rendering),
// its dispatches and copies in the same order with the same push blocks, the kernels the outer archive's frame_reuse/*
// (the frame-reuse/*.spv the Vulkan routes run). Where D3D12 differs:
//   - the images are typeless in the frame's format group (copies to and from the game's textures need one), viewed
//     UNORM; the filter's output in that group too (the Vulkan stage's RGBA8 + a blit into a BGRA8 frame: the same codes)
//   - a blit that converts (the RGBA16F answer into the frame's format) is frame_copy.spv's store (the GPU decision's
//     copy on Vulkan, "as a rebuilt frame stores it")
//   - the second side's mode 2 writes the merged side into an image of its own (on Vulkan into Side0 it reads, per
//     texel): a resource is not read and written at once on D3D12; mode 3 reads it from there - the same values
//   - no clear of the images when made: every read of one is behind a write or a valid flag (D3D12 zeroes committed
//     resources besides)
//   - the even frames (delay): the second part is a direct list of the stage's own the caller submits on the frame's
//     queue before the next Present's list (Vulkan: a secondary buffer the next recording runs first - the same GPU
//     order); the frame's output (the picture the previous second part left in `held`) is stored right after the frame's
//     colour was copied, before the split - every command of the first list is recorded before any of the second, so the
//     states tracked here follow the GPU's order; the balance times a frame from its previous second part's start (that
//     list's first command) to the end of its first part (Vulkan: the recording's start, the waiting part run first)
#include "nr_frame_reuse_d3d12.hpp"
#include "nr_frame_pair.hpp"
#include "nr_d3d12_direct_state.hpp"
#include "nr_d3d12_il.hpp"
#include "nr_d3d12_outer_elf.hpp"
#include "nr_pe_log.hpp"
#include "nr_vram_ledger.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace nr::pe::d3d12_passthrough::frame_reuse12 {
namespace {
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// The frame's format group: the typeless format of the stage's own frame images (copies with the game's textures)
DXGI_FORMAT group_of(DXGI_FORMAT view) {
    switch (view) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    // 16-bit UNORM stays typed: the stages handed these frames view a typeless 16-bit image as FLOAT
    case DXGI_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
DXGI_FORMAT code_view(DXGI_FORMAT f) {
    if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) return DXGI_FORMAT_B8G8R8A8_UNORM;
    return f;
}
const char* kernel_suffix(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_UNORM: return "16u";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "16f";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "11f";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "10";
    default: return "";
    }
}

// The push blocks (nr_frame_reuse.cpp's, the shaders' layouts)
struct GuidePush {
    uint32_t mgrid[2], dgrid[2], mbase[2], msize[2], dbase[2], dsize[2]; float scale[2]; uint32_t flags;
};
struct ComposePush {
    uint32_t size[2], grid[2], mode, current_motion, bank, age, valid, previous; float blend, stay_margin; uint32_t protection, inverted;
    uint32_t side; float merge_k; float stay_fixed; uint32_t rebuild_flags;
};
struct ContextPush { uint32_t size[2], mode, which, inverted; };
struct DisplayPush { uint32_t size[2], mbase[2], mgrid[2]; float mscale[2]; uint32_t dbase[2], dgrid[2], flags, pad[3]; };
static_assert(sizeof(GuidePush) == 60 && sizeof(ComposePush) == 72 && sizeof(DisplayPush) == 64 && sizeof(ContextPush) == 20);
}  // namespace

bool Stage::supports(DXGI_FORMAT view) { return group_of(view) != DXGI_FORMAT_UNKNOWN; }

struct Stage::Impl {
    nr::frame_pair::Sources pair_sources;
    Config cfg;
    ID3D12Device* device = nullptr;
    std::function<void(IUnknown*)> retire;
    ID3D12RootSignature* rs = nullptr;
    std::shared_ptr<d3d12_outer::OuterArchive> arch;
    d3d12_outer::OuterHeapPool pool;
    d3d12_outer::OuterKernel k_guides, k_compose, k_context, k_display, k_copy, k_encode, k_decode;
    nr::gpu::Ticket ticket;
    bool rs_bound = false;
    bool ok = true;
    uint64_t bytes = 0;

    // A texture of the stage's own: the state it is in (tracked here), its views.
    struct Tex {
        ID3D12Resource* res = nullptr;
        DXGI_FORMAT srv = DXGI_FORMAT_UNKNOWN, uav = DXGI_FORMAT_UNKNOWN;
        uint32_t w = 0, h = 0;
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COMMON;
    };
    Tex native_input, native_answer;
    Tex current, anchor[2], edit[2], rendered, shown, motion[3], composed, depth, anchor_depth[2], exposure, anchor_exposure[2];
    Tex display_history[2], display_out, output_copy;
    // the two-sided rebuild: the kept frame's depth, the first side and its match, the merge of both by the second side
    // (side_merged: this file's, see the head), the rebuilt answer in the frame's format, the picture shown one recording later; the outline
    // context classes of the kept depth and of the anchors' depths and their tiles; the nearer surface near each pixel of
    // the kept frame (compose's mode 6, read by both sides' mode 2) and its depth at every pixel (mode 7: read by mode 6,
    // the sides and the fill)
    Tex pend_depth, side, side_match, side_merged, rebuilt8, bidi_held, context[3], context_tile, pick, depth_full;
    Frame pending_input{};
    bool bidi_pending = false, bidi_held_valid = false;
    uint32_t pend_slot = 0, pend_age = 1;
    bool display_valid = false;
    uint32_t display_bank = 0, display_prev = 0;
    uint64_t serial = 0;
    uint32_t phase = 0, latest = 0, age = 0, guide_count = 0, inverted = 0, current_gap = 1;
    bool valid = false, previous = false, reset_requested = false;
    uint64_t cpu_frames = 0, cpu_computed = 0;

    // The game's textures this record: the state each is in now (put back to the frame's at the end)
    std::map<ID3D12Resource*, D3D12_RESOURCE_STATES> game;

    // ---- the even frames (Config::delay) ----
    // A second part's list: its allocator, the recording's ticket (reused once it has run), whether it leaves a picture
    struct Tail {
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        nr::gpu::Ticket ticket;
        bool picture = true;
        bool from_bidi = false;   // its picture is `bidi_held` itself (nothing after it in the part writes that image)
    };
    std::vector<Tail> tails;
    ID3D12GraphicsCommandList* pending = nullptr;   // the last record's second part, closed, not taken yet
    bool held_from_tail = false;                    // the second part submitted before this record leaves a picture in `held`
    bool held_from_bidi = false, held_bidi = false; // ... in `bidi_held` instead (that part's flag; this record's)
    Tex held;
    ID3D12GraphicsCommandList* splitting = nullptr;   // inside Complete: where the network's second part goes
    nr::gpu::Ticket splitting_ticket;                 // ... and its recording
    std::function<void(ID3D12GraphicsCommandList*, bool)> tail_mark;   // the caller's timing of the second part
    float share = 0.5f;
    std::vector<float> balance_errors;
    uint32_t balance_moves = 0;
    // the balance's times: per frame its start (the previous second part's first command, else its own first) and the
    // end of its first part (kTimed slots; ticks per ms), read once the frame's first part has run
    static constexpr uint32_t kTimed = 16;
    ID3D12QueryHeap* qheap = nullptr;
    ID3D12Resource* qread = nullptr;
    const uint64_t* qmap = nullptr;
    double ticks_per_ms = 0.0;
    struct Timed { uint64_t serial = ~0ull; char kind = 0; nr::gpu::Ticket done; };   // 'C' computed, 'R' rebuilt
    std::array<Timed, kTimed> timed{};
    std::array<bool, kTimed> start_in_tail{};   // this slot's start was written by the second part before it

    std::vector<Tex*> images() {
        return {&native_input, &native_answer, &current, &anchor[0], &anchor[1], &edit[0], &edit[1], &rendered, &shown, &motion[0], &motion[1], &motion[2],
                &composed, &depth, &anchor_depth[0], &anchor_depth[1], &exposure, &anchor_exposure[0], &anchor_exposure[1],
                &display_history[0], &display_history[1], &display_out, &output_copy, &pend_depth, &side, &side_match,
                &side_merged, &rebuilt8, &bidi_held, &context[0], &context[1], &context[2], &context_tile, &pick, &depth_full};
    }
    ~Impl() {
        // (`pending` is one of `tails`, the caller takes it after every record: never left here)
        auto gone = [&](IUnknown* r) { if (r) { if (retire) retire(r); else r->Release(); } };
        for (Tail& t : tails) { gone(t.list); gone(t.alloc); }
        gone(qheap);
        if (qread && qmap) qread->Unmap(0, nullptr);
        gone(qread);
        if (held.res) gone(held.res);
        for (Tex* t : images()) if (t->res) { if (retire) retire(t->res); else release(t->res); }
        pool.reset();
        for (auto* k : {&k_guides, &k_compose, &k_context, &k_display, &k_copy, &k_encode, &k_decode})
            if (k->pso) { if (retire) retire(k->pso); else release(k->pso); }
        if (rs) { if (retire) retire(rs); else release(rs); }
    }

    bool make(Tex& t, uint32_t w, uint32_t h, DXGI_FORMAT res_format, DXGI_FORMAT view, std::string* why) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = res_format; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&t.res))) || !t.res) {
            *why = "frame reuse: an image could not be made (" + std::to_string(w) + "x" + std::to_string(h) + ", format " +
                   std::to_string(int(res_format)) + ")";
            return false;
        }
        t.srv = t.uav = view; t.w = w; t.h = h; t.st = D3D12_RESOURCE_STATE_COMMON;
        nr::vram::track_d3d12(device, t.res, nr::vram::kRenoDX);
        const D3D12_RESOURCE_ALLOCATION_INFO ai = device->GetResourceAllocationInfo(0, 1, &rd);
        bytes += ai.SizeInBytes;
        return true;
    }
    bool frame_image(Tex& t, std::string* why) {
        return make(t, cfg.width, cfg.height, group_of(cfg.frame_format), code_view(cfg.frame_format), why);
    }

    bool build(std::string* why) {
        const Config& c = cfg;
        if (!d3d12_outer::create_outer_root_signature(device, &rs, why)) return false;
        arch = d3d12_outer::shared_archive(why);
        if (!arch) return false;
        const std::string suffix = kernel_suffix(c.frame_format);
        if (!arch->build_kernel(device, "frame_reuse/guides", &k_guides, why) ||
            !arch->build_kernel(device, c.frame_format==DXGI_FORMAT_R16G16B16A16_UNORM ? "frame_reuse/compose16u" : "frame_reuse/compose", &k_compose, why) ||
            (c.two_sided && c.context && !arch->build_kernel(device, "frame_reuse/context", &k_context, why)) ||
            (c.display_filter && !arch->build_kernel(device, "frame_reuse/display" + suffix, &k_display, why)) ||
            !arch->build_kernel(device, "frame_reuse/frame_copy" + suffix, &k_copy, why))
            return false;
        if (c.scrgb && (!arch->build_kernel(device, "frame_reuse/scrgb_encode", &k_encode, why) ||
                        !arch->build_kernel(device, "frame_reuse/scrgb_decode", &k_decode, why))) return false;
        if (!pool.init(device, 64, [](const nr::gpu::Ticket& t) { return nr::pe::d3d12_direct::completed(t); },
                       [this](IUnknown* r) { if (retire) retire(r); else r->Release(); }, why))
            return false;
        const uint32_t mw = c.motion_width, mh = c.motion_height, dw = c.depth_width, dh = c.depth_height;
        const DXGI_FORMAT f16 = DXGI_FORMAT_R16G16B16A16_FLOAT, rg32 = DXGI_FORMAT_R32G32_FLOAT, r32 = DXGI_FORMAT_R32_FLOAT;
        bool good;
        if (c.batch_only) {
            good=frame_image(current,why) && frame_image(anchor[0],why) && frame_image(bidi_held,why);
            for (auto& m:motion)good=good && make(m,mw,mh,rg32,rg32,why);
            good=good && make(depth,dw,dh,r32,r32,why) && make(pend_depth,dw,dh,r32,r32,why) &&
                 make(exposure,1,1,r32,r32,why) && make(anchor_exposure[1],1,1,r32,r32,why);
        } else {
            good = frame_image(current, why) && frame_image(rendered, why) && (c.frame_format==DXGI_FORMAT_R16G16B16A16_UNORM?frame_image(shown,why):make(shown,c.width,c.height,f16,f16,why));
            if (c.scrgb) good = good && frame_image(native_input, why) && frame_image(native_answer, why);
            for (Tex& x : anchor) good = good && frame_image(x, why);
            for (Tex& x : edit) good = good && make(x, c.width, c.height, f16, f16, why);
            for (Tex& x : motion) good = good && make(x, mw, mh, rg32, rg32, why);
            good = good && make(composed, mw, mh, rg32, rg32, why) && make(depth, dw, dh, r32, r32, why);
            for (Tex& x : anchor_depth) good = good && make(x, dw, dh, r32, r32, why);
            good = good && make(exposure, 1, 1, r32, r32, why);
            for (Tex& x : anchor_exposure) good = good && make(x, 1, 1, r32, r32, why);
            if (!c.display_filter && !c.delay) good = good && frame_image(output_copy, why);
            if (c.two_sided) {
                good = good && make(side, c.width, c.height, f16, f16, why) &&
                       make(side_match, c.width, c.height, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, why);
                good = good && make(side_merged, c.width, c.height, f16, f16, why) && make(pend_depth, dw, dh, r32, r32, why) &&
                       frame_image(rebuilt8, why) && frame_image(bidi_held, why) &&
                       make(pick, c.width, c.height, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, why) &&
                       make(depth_full, c.width, c.height, r32, r32, why);
                if (c.context) {
                    for (Tex& x : context) good = good && make(x, dw, dh, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, why);
                    good = good && make(context_tile, (dw + 15) / 16, (dh + 15) / 16, r32, r32, why);
                }
            }
            if (c.display_filter) {
                for (Tex& x : display_history) good = good && make(x, c.width, c.height, f16, f16, why);
                if (!c.delay || c.pair_kernel) good = good && frame_image(display_out, why);   // (else: held / bidi_held)
            }
        }
        if (good && c.delay && !c.pair_kernel) {
            if (!c.two_sided || !(c.split >= 0.f && c.split <= 1.f)) { *why = "frame reuse: even frames need the two-sided rebuild"; return false; }
            share = c.split;
            good = frame_image(held, why);
            D3D12_QUERY_HEAP_DESC qh{};
            qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            qh.Count = 2 * kTimed;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = 16 * kTimed; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            void* m = nullptr;
            ID3D12CommandQueue* q = nullptr;
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            UINT64 hz = 0;
            if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) && q) { q->GetTimestampFrequency(&hz); q->Release(); }
            if (SUCCEEDED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&qheap))) &&
                SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                          IID_PPV_ARGS(&qread))) &&
                SUCCEEDED(qread->Map(0, nullptr, &m)) && m && hz) {
                qmap = static_cast<const uint64_t*>(m);
                ticks_per_ms = double(hz) / 1000.0;
            } else {
                nr::pe::log("[nr] frame reuse (D3D12): no timestamp queries - the network's split stays at %.2f", double(share));
            }
        }
        return good;
    }
    // A second part's list: one whose last recording has run, else a new one; opened (Reset) and booked.
    int begin_tail(std::string* why) {
        size_t i = 0;
        for (; i < tails.size(); ++i)
            if (tails[i].list != pending && (!tails[i].ticket || nr::pe::d3d12_direct::completed(tails[i].ticket))) break;
        if (i == tails.size()) {
            Tail t;
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&t.alloc))) || !t.alloc ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, t.alloc, nullptr, IID_PPV_ARGS(&t.list))) ||
                !t.list) {
                if (t.alloc) t.alloc->Release();
                *why = "a command list for the second part could not be made";
                return -1;
            }
            tails.push_back(t);   // (made open)
        } else if (FAILED(tails[i].alloc->Reset()) || FAILED(tails[i].list->Reset(tails[i].alloc, nullptr))) {
            *why = "the second part's command list could not be reset";
            return -1;
        }
        // (made / Reset inside the frame's Scope, which the passthrough's hooks do not track: told so - else its last
        // recording's ticket came back for this one and its state stayed unknown to whatever records into it)
        nr::pe::d3d12_direct::own_list_opened(tails[i].list, tails[i].alloc);
        tails[i].ticket = nr::pe::d3d12_direct::book(tails[i].list);
        if (!tails[i].ticket) {
            tails[i].list->Close();
            *why = "the second part's command list is not tracked by the direct state";
            return -1;
        }
        tails[i].picture = true;
        tails[i].from_bidi = false;
        return int(i);
    }
    // The balance (nr_frame_reuse.cpp balance): a computed frame after a rebuilt one and before a rebuilt one - its slot
    // (the rebuilt frame's second part + its own first part) against the next (its second part + the rebuilt frame's
    // first part); the median of 9 such pairs moves the share.
    void balance() {
        if (serial < 6 || !qmap) return;
        const auto slot_ms = [&](uint64_t f, double* out) -> bool {
            const Timed& t = timed[f % kTimed];
            if (t.serial != f || !t.done || !nr::pe::d3d12_direct::completed(t.done)) return false;
            const uint64_t a = qmap[(f % kTimed) * 2], b = qmap[(f % kTimed) * 2 + 1];
            if (!a || b <= a) return false;
            *out = double(b - a) / ticks_per_ms;
            return true;
        };
        const auto rebuilt = [&](uint64_t f) { const Timed& t = timed[f % kTimed]; return t.serial == f && t.kind == 'R'; };
        const uint64_t a = serial - 4, b = serial - 3;
        if (timed[a % kTimed].serial != a || timed[a % kTimed].kind != 'C' || !rebuilt(a - 1) || !rebuilt(b)) return;
        double sa = 0, sb = 0;
        if (!slot_ms(a, &sa) || !slot_ms(b, &sb) || sa + sb <= 0) return;
        balance_errors.push_back(float((sa - sb) / (sa + sb)));
        if (balance_errors.size() < 9) return;
        std::sort(balance_errors.begin(), balance_errors.end());
        const float e = balance_errors[balance_errors.size() / 2];
        balance_errors.clear();
        share = std::clamp(share - 0.5f * e, 0.05f, 0.95f);
        ++balance_moves;
    }

    // ---- states and barriers ----------------------------------------------------------------------------------------
    void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        if (from == to) return;   // (a transition to the state a resource is in is an invalid call on D3D12)
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        cmd->ResourceBarrier(1, &b);
    }
    void to(ID3D12GraphicsCommandList* cmd, Tex& t, D3D12_RESOURCE_STATES s) { transition(cmd, t.res, t.st, s); t.st = s; }
    void to(ID3D12GraphicsCommandList* cmd, const Image& g, D3D12_RESOURCE_STATES s) {
        auto [it, fresh] = game.emplace(g.res, g.state);
        (void)fresh;
        transition(cmd, g.res, it->second, s);
        it->second = s;
    }
    void uav_barrier(ID3D12GraphicsCommandList* cmd) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cmd->ResourceBarrier(1, &b);
    }

    // ---- dispatches -------------------------------------------------------------------------------------------------
    // One binding of a dispatch: a texture of the stage's or the game's, read (SRV) or written (UAV). Only the images
    // the dispatch's mode reads or writes are bound and transitioned (the others' descriptors stay zero, never read).
    struct Use {
        uint32_t binding;
        Tex* tex;
        const Image* game;
        bool write;
    };
    static Use rd(uint32_t b, Tex& t) { return {b, &t, nullptr, false}; }
    static Use wr(uint32_t b, Tex& t) { return {b, &t, nullptr, true}; }
    static Use grd(uint32_t b, const Image& g) { return {b, nullptr, &g, false}; }
    void dispatch(ID3D12GraphicsCommandList* cmd, const d3d12_outer::OuterKernel& k, std::initializer_list<Use> uses,
                  const void* push, uint32_t push_bytes, uint32_t w, uint32_t h) {
        if (!ok) return;
        if (!k.pso || !rs) { ok = false; nr::pe::log("[nr] frame reuse (D3D12): a kernel is missing"); return; }
        std::vector<d3d12_il::Bind> binds;
        for (const Use& u : uses) {
            const D3D12_RESOURCE_STATES s = u.write ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if (u.tex) { to(cmd, *u.tex, s); binds.push_back({u.binding, u.tex->res, u.write ? u.tex->uav : u.tex->srv, u.write}); }
            else if (u.game && u.game->res) { to(cmd, *u.game, s); binds.push_back({u.binding, u.game->res, u.game->view, u.write}); }
        }
        std::string err;
        const d3d12_outer::OuterSlot slot = pool.acquire(ticket, &err, k.desc_zero_bytes);
        if (!slot.cpu) {
            ok = false;
            nr::pe::log("[nr] frame reuse (D3D12): %s", err.empty() ? "no outer descriptor slot" : err.c_str());
            return;
        }
        d3d12_il::write_outer_binds(device, slot, k, binds.data(), binds.size());
        if (push_bytes) slot.write_push(push, push_bytes, &k);
        slot.dispatch(cmd, rs, k, (w + 15) / 16, (h + 15) / 16, 1u, !rs_bound);
        rs_bound = true;
        uav_barrier(cmd);   // (the Vulkan stage's memory barrier after every dispatch)
    }

    // ---- copies -----------------------------------------------------------------------------------------------------
    // `src` (w x h from its origin) into `dst`: the same format group
    void copy_region(ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, ID3D12Resource* dst, uint32_t w, uint32_t h) {
        D3D12_TEXTURE_COPY_LOCATION s{}, d{};
        s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
        d.pResource = dst; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
        const D3D12_BOX box{0, 0, 0, w, h, 1};
        cmd->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
    }
    void copy(ID3D12GraphicsCommandList* cmd, Tex& src, Tex& dst) {
        to(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
        to(cmd, dst, D3D12_RESOURCE_STATE_COPY_DEST);
        copy_region(cmd, src.res, dst.res, dst.w, dst.h);
    }
    void copy(ID3D12GraphicsCommandList* cmd, const Image& src, Tex& dst) {   // the game's colour into one of ours
        to(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
        to(cmd, dst, D3D12_RESOURCE_STATE_COPY_DEST);
        copy_region(cmd, src.res, dst.res, dst.w, dst.h);
    }
    // `src` into the frame's output: a copy (a frame image), else frame_copy's store (the RGBA16F answer)
    void store_out(ID3D12GraphicsCommandList* cmd, Tex& src, const Image& out) {
        if (src.srv != code_view(out.view)) {
            // Only an unfiltered, undelayed frame can arrive in the compose
            // kernel's float format. Keep the computed answer alive for the
            // two-sided hold; the game's typed sRGB image is copy-only.
            store(cmd, src, output_copy);
            store_out(cmd, output_copy, out);
            return;
        }
        to(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
        to(cmd, out, D3D12_RESOURCE_STATE_COPY_DEST);
        copy_region(cmd, src.res, out.res, cfg.width, cfg.height);
    }
    // ... into one of ours
    void store(ID3D12GraphicsCommandList* cmd, Tex& src, Tex& dst) {
        if (src.srv == dst.srv) { copy(cmd, src, dst); return; }
        dispatch(cmd, k_copy, {rd(0, src), wr(1, dst)}, nullptr, 0, cfg.width, cfg.height);
    }

    // ---- the steps (nr_frame_reuse.cpp's) ---------------------------------------------------------------------------
    void record_guides(ID3D12GraphicsCommandList* cmd, const Frame& fr) {
        const GuidePush p{{motion[0].w, motion[0].h}, {depth.w, depth.h}, {fr.motion_x, fr.motion_y}, {fr.motion_w, fr.motion_h},
                          {fr.depth_x, fr.depth_y}, {fr.depth_w, fr.depth_h}, {fr.motion_scale_x, fr.motion_scale_y},
                          (fr.depth.res ? 1u : 0u) | (fr.exposure.res ? 2u : 0u)};
        Tex& mv = motion[serial % 3];
        dispatch(cmd, k_guides, {grd(0, fr.motion), grd(1, fr.depth), grd(2, fr.exposure), wr(3, mv), wr(4, depth), wr(5, exposure)},
                 &p, sizeof p, std::max(motion[0].w, depth.w), std::max(motion[0].h, depth.h));
    }
    ComposePush push(uint32_t mode, uint32_t bank, uint32_t a) const {
        return {{cfg.width, cfg.height}, {motion[0].w, motion[0].h}, mode, uint32_t(serial % 3), bank, a, valid ? 1u : 0u,
                previous ? 1u : 0u, .5f, 1.f / 255.f, cfg.protection, inverted, 0u, 0.f, cfg.stay_fixed / 255.f,
                ((cfg.context ? 2u : 1u) | ((cfg.scrgb || !cfg.context) ? 4u : 0u))};
    }
    // compose's mode 0 / 4 (the vectors composed: every motion bank read)
    void compose_field(ID3D12GraphicsCommandList* cmd, const ComposePush& p) {
        dispatch(cmd, k_compose, {rd(5, motion[0]), rd(6, motion[1]), rd(7, motion[2]), wr(12, composed)}, &p, sizeof p,
                 composed.w, composed.h);
    }
    // compose's mode 1: the completed answer's edit in its anchor's coordinates
    void compose_edit(ID3D12GraphicsCommandList* cmd, uint32_t bank) {
        const ComposePush p = push(1, bank, 0);
        dispatch(cmd, k_compose, {rd(bank ? 2 : 1, anchor[bank]), rd(8, rendered), wr(bank ? 11 : 10, edit[bank])}, &p, sizeof p,
                 cfg.width, cfg.height);
    }
    // compose's mode 2 from `bank` (side 0: into `shown`, the fill store into the other edit bank; side 1 / 2: the sides),
    // its depth `dep` (this frame's, or the kept frame's)
    void compose_rebuild(ID3D12GraphicsCommandList* cmd, const ComposePush& p, Tex& dep) {
        const uint32_t b = p.bank;
        const bool ctx = (p.rebuild_flags & 2u) && p.side != 0u;
        if (p.side == 0u) {
            dispatch(cmd, k_compose, {rd(0, current), rd(b ? 2 : 1, anchor[b]), rd(b ? 4 : 3, edit[b]), rd(5, motion[0]),
                                      rd(6, motion[1]), rd(7, motion[2]), rd(13, dep), rd(b ? 15 : 14, anchor_depth[b]),
                                      wr(9, shown), wr(b ? 10 : 11, edit[b ^ 1u])},
                     &p, sizeof p, cfg.width, cfg.height);
            return;
        }
        // side 1: (edit, trust) and its match term stored; side 2: merged with them into side_merged
        if (p.side == 1u && ctx)
            dispatch(cmd, k_compose, {rd(0, current), rd(b ? 2 : 1, anchor[b]), rd(b ? 4 : 3, edit[b]), rd(5, motion[0]),
                                      rd(6, motion[1]), rd(7, motion[2]), rd(13, dep), rd(b ? 15 : 14, anchor_depth[b]),
                                      rd(24, context[0]), rd(b ? 26 : 25, context[1u + b]), rd(28, pick), rd(30, depth_full),
                                      wr(16, side), wr(20, side_match)},
                     &p, sizeof p, cfg.width, cfg.height);
        else if (p.side == 1u)
            dispatch(cmd, k_compose, {rd(0, current), rd(b ? 2 : 1, anchor[b]), rd(b ? 4 : 3, edit[b]), rd(5, motion[0]),
                                      rd(6, motion[1]), rd(7, motion[2]), rd(13, dep), rd(b ? 15 : 14, anchor_depth[b]),
                                      rd(28, pick), rd(30, depth_full), wr(16, side), wr(20, side_match)},
                     &p, sizeof p, cfg.width, cfg.height);
        else if (ctx)
            dispatch(cmd, k_compose, {rd(0, current), rd(b ? 2 : 1, anchor[b]), rd(b ? 4 : 3, edit[b]), rd(5, motion[0]),
                                      rd(6, motion[1]), rd(7, motion[2]), rd(13, dep), rd(b ? 15 : 14, anchor_depth[b]),
                                      rd(24, context[0]), rd(b ? 26 : 25, context[1u + b]), rd(28, pick), rd(30, depth_full),
                                      rd(18, side), rd(22, side_match), wr(16, side_merged)},
                     &p, sizeof p, cfg.width, cfg.height);
        else
            dispatch(cmd, k_compose, {rd(0, current), rd(b ? 2 : 1, anchor[b]), rd(b ? 4 : 3, edit[b]), rd(5, motion[0]),
                                      rd(6, motion[1]), rd(7, motion[2]), rd(13, dep), rd(b ? 15 : 14, anchor_depth[b]),
                                      rd(28, pick), rd(30, depth_full), rd(18, side), rd(22, side_match), wr(16, side_merged)},
                     &p, sizeof p, cfg.width, cfg.height);
    }
    // compose's mode 3: the fill of the rebuilt frame's untrusted pixels (side 0: from the other edit bank; side 3: from
    // the merged side), weighed by depth `dep`
    void compose_fill(ID3D12GraphicsCommandList* cmd, const ComposePush& p, Tex& dep) {
        if (p.side == 3u && (p.rebuild_flags & 4u))
            dispatch(cmd, k_compose, {rd(0,current),rd(1,anchor[0]),rd(2,anchor[1]),rd(3,edit[0]),rd(4,edit[1]),
                                      rd(5,motion[0]),rd(6,motion[1]),rd(7,motion[2]),rd(13,dep),rd(18,side_merged),
                                      rd(30,depth_full),wr(9,shown)},
                     &p, sizeof p, cfg.width, cfg.height);
        else if (p.side == 3u)
            dispatch(cmd, k_compose, {rd(0, current), rd(13, dep), rd(18, side_merged), rd(30, depth_full), wr(9, shown)}, &p, sizeof p,
                     cfg.width, cfg.height);
        else
            dispatch(cmd, k_compose, {rd(0, current), rd(13, dep), rd(p.bank ^ 1u ? 4 : 3, edit[p.bank ^ 1u]), wr(9, shown)},
                     &p, sizeof p, cfg.width, cfg.height);
    }
    // the two-sided rebuild of the kept frame (current, pend_depth, vectors in motion[pend_slot]) from the answers in
    // banks `past` and `next` (this recording's) -> `shown` (nr_frame_reuse.cpp compose_two_sided; modes 7 and 6 before the sides)
    void compose_two_sided(ID3D12GraphicsCommandList* cmd, uint32_t past, uint32_t next) {
        const uint32_t now = uint32_t(serial % 3), free = (now + 1u) % 3u;
        if (pend_slot != (now + 2u) % 3u) { ok = false; nr::pe::log("[nr] frame reuse (D3D12): the kept frame is not the one before"); return; }
        ComposePush f = push(4, next, 1); f.current_motion = now; f.previous = pend_slot;
        compose_field(cmd, f);
        copy(cmd, composed, motion[free]);
        const ComposePush z = push(7, next, 0);   // the kept frame's depth at every pixel
        dispatch(cmd, k_compose, {rd(13, pend_depth), wr(29, depth_full)}, &z, sizeof z, cfg.width, cfg.height);
        const ComposePush n = push(6, next, 0);   // the nearer surfaces of the kept frame's pixels (both sides read them)
        dispatch(cmd, k_compose, {rd(30, depth_full), wr(27, pick)}, &n, sizeof n, cfg.width, cfg.height);
        ComposePush a = push(2, past, pend_age); a.current_motion = pend_slot; a.previous = 0; a.blend = 1; a.side = 1;
        compose_rebuild(cmd, a, pend_depth);
        ComposePush b = push(2, next, 1); b.current_motion = free; b.previous = now; b.blend = 1; b.side = 2;
        compose_rebuild(cmd, b, pend_depth);   // (the merge of both into side_merged)
        ComposePush fl = push(3, next, 0); fl.side = 3;
        compose_fill(cmd, fl, pend_depth);
    }
    // context.comp of the kept depth (which 0) or an anchor's depth (1 + its bank)
    void context_map(ID3D12GraphicsCommandList* cmd, uint32_t which) {
        if (!cfg.two_sided || !cfg.context) return;
        Tex& src = which == 0u ? pend_depth : anchor_depth[which - 1u];
        ContextPush c{{cfg.width, cfg.height}, 0, which, inverted};
        dispatch(cmd, k_context, {rd(which, src), wr(6, context_tile)}, &c, sizeof c, context[which].w, context[which].h);
        c.mode = 1;
        dispatch(cmd, k_context, {rd(which, src), rd(7, context_tile), wr(3u + which, context[which])}, &c, sizeof c,
                 context[which].w, context[which].h);
    }
    // display.comp into `out` (display_out, or a delayed stage's shown picture): input 0 / 1 the anchor banks, 2 `current`
    void filter_image(ID3D12GraphicsCommandList* cmd, uint32_t input, Tex& nr_answer, Tex& mot, Tex& dep, Tex& out) {
        Tex* inputs[3] = {&anchor[0], &anchor[1], &current};
        const DisplayPush p{{cfg.width, cfg.height}, {0, 0}, {motion[0].w, motion[0].h}, {1.f, 1.f}, {0, 0}, {depth.w, depth.h},
                            (display_valid ? 1u : 0u) | (inverted ? 4u : 0u), {0, 0, 0}};
        if (display_valid)
            dispatch(cmd, k_display, {rd(0, *inputs[input]), rd(1, nr_answer), rd(2, mot), rd(3, dep),
                                      rd(4, display_history[display_bank ^ 1u]), rd(5, *inputs[display_prev]),
                                      wr(6, display_history[display_bank]), wr(7, out)},
                     &p, sizeof p, cfg.width, cfg.height);
        else   // (no history yet: HistPrev and InputPrev are not read)
            dispatch(cmd, k_display, {rd(0, *inputs[input]), rd(1, nr_answer), rd(2, mot), rd(3, dep),
                                      wr(6, display_history[display_bank]), wr(7, out)},
                     &p, sizeof p, cfg.width, cfg.height);
        display_valid = true;
        display_bank ^= 1u;
        display_prev = input;
    }
    // The game's textures back in the states the frame gave them: `final` - the output's (colour and output one texture:
    // it leaves in the output's state), else as they came (before the Complete callback, which does not touch them)
    void restore_game(ID3D12GraphicsCommandList* cmd, const Frame& fr, bool final) {
        for (auto& [r, s] : game) {
            const Image* g = final && r == fr.output.res ? &fr.output : r == fr.colour.res ? &fr.colour
                           : r == fr.output.res ? &fr.output : r == fr.motion.res ? &fr.motion
                           : r == fr.depth.res ? &fr.depth : r == fr.exposure.res ? &fr.exposure : nullptr;
            if (g && s != g->state) { transition(cmd, r, s, g->state); s = g->state; }
        }
    }
    // this recording's output: the picture held from the recording before, else the game's frame
    void out_held(ID3D12GraphicsCommandList* cmd, const Frame& fr) {
        if (bidi_held_valid) { store_out(cmd, bidi_held, fr.output); return; }
        out_game(cmd, fr);
    }
    void out_game(ID3D12GraphicsCommandList* cmd, const Frame& fr) {
        if (fr.output.res == fr.colour.res) return;
        to(cmd, fr.colour, D3D12_RESOURCE_STATE_COPY_SOURCE);
        to(cmd, fr.output, D3D12_RESOURCE_STATE_COPY_DEST);
        copy_region(cmd, fr.colour.res, fr.output.res, cfg.width, cfg.height);
    }

    // A delayed stage: the frame's output - the picture the second part before left in `held`, else the game's frame
    // (Vulkan finish()), stored right after the frame's colour was copied: before any command of the second part
    void delayed_output(ID3D12GraphicsCommandList* cmd, const Frame& fr, bool held_valid,bool native=false) {
        pair_sources.show(held_valid, cfg.two_sided);
        Tex& picture = held_bidi ? bidi_held : held;
        if (held_valid && cfg.scrgb && !native) {
            dispatch(cmd, k_decode, {rd(0, picture), wr(1, shown)}, nullptr, 0, cfg.width, cfg.height);
            store_out(cmd, shown, fr.output);
        } else if (held_valid) store_out(cmd, picture, fr.output);
        else out_game(cmd, fr);
    }
    // The shown picture: into the output (a delayed stage: into `held`, the next frame's output)
    void show_out(ID3D12GraphicsCommandList* cmd, Tex& src, const Frame& fr) {
        if (cfg.delay) store(cmd, src, held);
        else store_out(cmd, src, fr.output);
    }
    // ... no new picture: the one held from the computed frame before (none yet: the game's frame / a second part
    // without one); in_place: nothing after this in the part writes bidi_held - the output is stored from it (no copy)
    void show_held(ID3D12GraphicsCommandList* cmd, const Frame& fr, int tail_i, bool in_place = false) {
        if (!cfg.delay) { out_held(cmd, fr); return; }
        if (bidi_held_valid && in_place && tail_i >= 0) tails[size_t(tail_i)].from_bidi = true;
        else if (bidi_held_valid) copy(cmd, bidi_held, held);
        else if (tail_i >= 0) tails[size_t(tail_i)].picture = false;
    }

    Image as_image(ID3D12GraphicsCommandList* cmd,Tex& t,D3D12_RESOURCE_STATES desired) {
        to(cmd,t,desired);
        return {t.res,t.st,t.srv,t.w,t.h};
    }
    void copy_owned(ID3D12GraphicsCommandList* cmd,const Image& src,Tex& dst) {
        transition(cmd,src.res,src.state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        to(cmd,dst,D3D12_RESOURCE_STATE_COPY_DEST);
        copy_region(cmd,src.res,dst.res,dst.w,dst.h);
        transition(cmd,src.res,D3D12_RESOURCE_STATE_COPY_SOURCE,src.state);
    }
    void input_colour(ID3D12GraphicsCommandList* cmd,const Image& source,Tex& work) {
        if (cfg.scrgb) {
            copy(cmd,source,native_input);
            dispatch(cmd,k_encode,{rd(0,native_input),wr(1,work)},nullptr,0,cfg.width,cfg.height);
        } else copy(cmd,source,work);
    }
    void write_native(ID3D12GraphicsCommandList* cmd,Tex& source,Tex& result) {
        if (cfg.scrgb) dispatch(cmd,k_decode,{rd(0,source),wr(1,result)},nullptr,0,cfg.width,cfg.height);
        else store(cmd,source,result);
    }
    uint32_t render_pair(ID3D12GraphicsCommandList*& cmd,Frame* odd,Frame& even,const PairNetwork& network) {
        ticket=nr::pe::d3d12_direct::book(cmd);rs_bound=false;ok=bool(ticket);game.clear();
        if(even.reset || reset_requested || !odd){valid=false;display_valid=false;reset_requested=false;}
        inverted=even.depth_inverted?1u:0u;
        if(odd) {
            record_guides(cmd,*odd);input_colour(cmd,odd->colour,current);copy(cmd,depth,pend_depth);context_map(cmd,0);
            pend_slot=uint32_t(serial++%3);pend_age=1;restore_game(cmd,*odd,true);game.clear();
        }
        const uint32_t past=latest,bank=valid?(past^1u):0u;
        record_guides(cmd,even);input_colour(cmd,even.colour,anchor[bank]);
        copy(cmd,depth,anchor_depth[bank]);context_map(cmd,bank+1); restore_game(cmd,even,true);game.clear();
        Frame f=even;
        constexpr auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        constexpr auto write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        f.colour=as_image(cmd,cfg.scrgb?native_input:anchor[bank],read);
        f.output=as_image(cmd,cfg.scrgb?native_answer:rendered,write);
        Tex* mv=&motion[serial%3];
        if(odd && valid){compose_field(cmd,push(0,bank,2));mv=&composed;}
        f.motion=as_image(cmd,*mv,read);f.motion_x=f.motion_y=0;f.motion_w=mv->w;f.motion_h=mv->h;
        f.motion_scale_x=f.motion_scale_y=1.f;f.motion_composed=true;
        if(even.depth.res){f.depth=as_image(cmd,anchor_depth[bank],read);f.depth_x=f.depth_y=0;}
        const uint32_t calls=network(cmd,f);
        if(!ok || calls==nr::frame_pair::failed_calls){reset_requested=true;return nr::frame_pair::failed_calls;}
        ticket=nr::pe::d3d12_direct::book(cmd);rs_bound=false;uav_barrier(cmd);
        if(cfg.scrgb)dispatch(cmd,k_encode,{rd(0,native_answer),wr(1,rendered)},nullptr,0,cfg.width,cfg.height);
        compose_edit(cmd,bank);
        const bool had=valid;previous=valid;valid=true;latest=bank;
        if(odd && had) {
            compose_two_sided(cmd,past,bank);store(cmd,shown,rebuilt8);filter_image(cmd,2,rebuilt8,motion[pend_slot],pend_depth,display_out);
            write_native(cmd,display_out,rebuilt8);odd->colour=odd->output=as_image(cmd,rebuilt8,write);
        }
        filter_image(cmd,bank,rendered,motion[serial%3],depth,display_out);
        write_native(cmd,display_out,bidi_held);even.colour=even.output=as_image(cmd,bidi_held,write);
        ++serial;return calls;
    }

    Result record(ID3D12GraphicsCommandList* cmd, const Frame& fr, const Complete& complete,const Batch& batch={}) {
        Result result;
        const Config& c = cfg;
        if (c.pair_kernel || c.batch_only!=bool(batch) || !cmd || (!complete && !batch) || !fr.colour.res || !fr.output.res || fr.colour.view != c.frame_format ||
            code_view(fr.output.view) != code_view(c.frame_format) || !fr.motion.res || fr.motion_w != c.motion_width ||
            fr.motion_h != c.motion_height || (fr.depth.res && (fr.depth_w != c.depth_width || fr.depth_h != c.depth_height))) {
            nr::pe::log("[nr] frame reuse (D3D12): the frame does not match the stage (extent, format or guides)");
            return result;
        }
        ID3D12GraphicsCommandList* const head = cmd;   // (a delayed stage goes on in its second part after the split)
        ticket = nr::pe::d3d12_direct::book(cmd);
        if (!ticket) return result;
        const nr::gpu::Ticket head_ticket = ticket;
        nr::pe::d3d12_direct::Scope scope(cmd);
        ok = true;
        rs_bound = false;
        game.clear();
        const bool reset = fr.reset || reset_requested;
        if (reset) {
            valid = previous = false; phase = age = guide_count = 0; reset_requested = false; display_valid = false;
            bidi_pending = bidi_held_valid = false;
        }
        // The even frames: this frame's slot timed (its start written by the second part before, else here), the
        // output that part left, this frame's second part opened (its first command: the next frame's start)
        int tail_i = -1;
        ID3D12GraphicsCommandList* tail = nullptr;
        std::optional<nr::pe::d3d12_direct::Scope> tail_scope;
        bool held_valid = false;
        const uint32_t q = uint32_t(serial % kTimed);
        if (c.delay) {
            balance();
            held_valid = held_from_tail && !reset;
            result.shown_late = held_valid ? 2 : 0;
            held_from_tail = false;
            held_bidi = held_from_bidi; held_from_bidi = false;
            timed[q] = Timed{serial, 0, {}};
            if (qmap && !start_in_tail[q]) head->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, q * 2);
            start_in_tail[q] = false;
            std::string why;
            tail_i = begin_tail(&why);
            if (tail_i < 0) {
                nr::pe::log("[nr] frame reuse (D3D12): %s - the frame is not recorded", why.c_str());
                return result;
            }
            tail = tails[size_t(tail_i)].list;
            tail_scope.emplace(tail);
            uav_barrier(tail);   // (it runs after this frame's first part and the game's next frame)
            const uint32_t qn = uint32_t((serial + 1) % kTimed);
            if (qmap) { tail->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, qn * 2); start_in_tail[qn] = true; }
            if (tail_mark) tail_mark(tail, false);
        }
        // the second part from here on: its own descriptor slots' ticket, no root signature bound in it yet
        const auto go_tail = [&] {
            if (!tail || cmd == tail) return;
            cmd = tail;
            ticket = tails[size_t(tail_i)].ticket;
            rs_bound = false;
        };
        // a delayed stage's end (every way out once its second part is open): the second part closed for the caller,
        // the first part's end stamped and resolved with its start
        const auto finish = [&](bool evaluated) {
            if (!tail) return;
            held_from_tail = tails[size_t(tail_i)].picture && ok;
            held_from_bidi = tails[size_t(tail_i)].from_bidi;
            if (tail_mark) tail_mark(tail, true);
            tail_scope.reset();
            if (FAILED(tail->Close())) {   // (not handed over: a list that did not close is never submitted)
                ok = false;
                nr::pe::log("[nr] frame reuse (D3D12): the second part's list could not be closed");
            } else {
                pending = tail;
            }
            timed[q].kind = evaluated ? 'C' : 'R';
            timed[q].done = head_ticket;
            if (qmap) {
                head->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, q * 2 + 1);
                head->ResolveQueryData(qheap, D3D12_QUERY_TYPE_TIMESTAMP, q * 2, 2, qread, uint64_t(q) * 16);
            }
        };
        inverted = fr.depth_inverted ? 1u : 0u;
        record_guides(cmd, fr);
        guide_count = std::min(guide_count + 1u, 3u);
        // the cadence: 1 computed frame of 2 (the Vulkan stage's compute 1, period 2)
        const bool evaluate = phase < 1u || !valid;
        if (batch) {
            Frame f=fr;
            constexpr auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            f.motion=as_image(cmd,motion[serial%3],read);f.motion_composed=true;
            f.motion_x=f.motion_y=0;f.motion_scale_x=f.motion_scale_y=1.f;f.depth_x=f.depth_y=0;
            Tex& raw=evaluate?anchor[0]:current;
            copy(cmd,fr.colour,raw);f.colour=f.output=as_image(cmd,raw,read);
            delayed_output(cmd,fr,held_valid,true);
            if(evaluate) {
                if(fr.depth.res)f.depth=as_image(cmd,depth,read);
                if(fr.exposure.res)f.exposure=as_image(cmd,exposure,read);
                f.reset=reset || !valid;
                auto* odd=bidi_pending && valid?&pending_input:nullptr;
                auto* cur=cmd;
                result.network_calls=batch(cur,odd,f,tail,share);
                ok=ok && result.network_calls!=nr::frame_pair::failed_calls;
                result.completed=ok;go_tail();rs_bound=false;
                if(odd)copy_owned(cmd,odd->colour,held);else tails[size_t(tail_i)].picture=false;
                copy_owned(cmd,f.colour,bidi_held);
                valid=bidi_held_valid=true;bidi_pending=false;
            } else {
                if(fr.depth.res){copy(cmd,depth,pend_depth);f.depth=as_image(cmd,pend_depth,read);}
                if(fr.exposure.res){copy(cmd,exposure,anchor_exposure[1]);f.exposure=as_image(cmd,anchor_exposure[1],read);}
                pending_input=f;bidi_pending=true;go_tail();show_held(cmd,fr,tail_i,true);
            }
        } else if (evaluate) {
            const uint32_t bank = valid ? (latest ^ 1u) : 0u, past = latest;
            const bool had_valid = valid;
            Tex& input = c.scrgb ? native_input : anchor[bank];
            Tex& answer = c.scrgb ? native_answer : rendered;
            copy(cmd, fr.colour, input);
            if (c.scrgb) dispatch(cmd, k_encode, {rd(0, input), wr(1, anchor[bank])}, nullptr, 0, c.width, c.height);
            if (c.delay) delayed_output(cmd, fr, held_valid);
            copy(cmd, depth, anchor_depth[bank]);
            context_map(cmd, 1u + bank);
            if (fr.exposure.res) copy(cmd, exposure, anchor_exposure[bank]);
            // the network's histories go on from the frames before; only the game's own reset restarts them
            Frame nf = fr;
            to(cmd, input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            to(cmd, answer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            nf.colour = {input.res, input.st, input.srv, input.w, input.h};
            nf.output = {answer.res, answer.st, answer.uav, answer.w, answer.h};
            nf.reset = fr.reset;
            const uint32_t gap = std::min(guide_count, 2u);
            current_gap = std::max(gap, 1u);
            // Both the head and tail read private guides. On the first anchor
            // the sampled field already has the game's vectors in screen units.
            Tex& network_motion=gap==1u?motion[serial%3]:composed;
            if (gap!=1u) compose_field(cmd,push(0,bank,gap));
            to(cmd,network_motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            nf.motion={network_motion.res,network_motion.st,network_motion.srv,network_motion.w,network_motion.h};
            nf.motion_x=nf.motion_y=0; nf.motion_w=network_motion.w; nf.motion_h=network_motion.h;
            nf.motion_scale_x=nf.motion_scale_y=1.f; nf.motion_composed=true;
            if (fr.depth.res) {
                to(cmd, anchor_depth[bank], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                nf.depth = {anchor_depth[bank].res, anchor_depth[bank].st, anchor_depth[bank].srv, anchor_depth[bank].w, anchor_depth[bank].h};
                nf.depth_x = nf.depth_y = 0; nf.depth_w = anchor_depth[bank].w; nf.depth_h = anchor_depth[bank].h;
            }
            if (fr.exposure.res) {
                to(cmd, anchor_exposure[bank], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                nf.exposure = {anchor_exposure[bank].res, anchor_exposure[bank].st, anchor_exposure[bank].srv, 1, 1};
            }
            restore_game(cmd, fr, false);   // (gap 1: the game's vectors go to Complete in their own state)
            if (!ok) { finish(true); restore_game(head, fr, true); return result; }
            splitting = tail;
            splitting_ticket = tail ? tails[size_t(tail_i)].ticket : nr::gpu::Ticket{};
            result.network_calls = complete(cmd, nf);   // (it leaves every image in the state it was handed)
            ok=ok && result.network_calls!=nr::frame_pair::failed_calls;
            splitting = nullptr;
            splitting_ticket = {};
            go_tail();   // (a delayed stage: the rest of the frame in its second part, as the network's rest)
            if (c.scrgb) dispatch(cmd, k_encode, {rd(0, answer), wr(1, rendered)}, nullptr, 0, c.width, c.height);
            if (phase == 0u) compose_edit(cmd, bank);   // (phase + 1 == compute)
            previous = valid; valid = true; latest = bank; age = 0; guide_count = 0; result.completed = true;
            if (fr.reset) display_valid = false;
            if (bidi_pending && had_valid) {
                compose_two_sided(cmd, past, bank);
                if (c.display_filter && c.delay) {   // (into `held`: the next frame's output)
                    store(cmd, shown, rebuilt8);
                    filter_image(cmd, 2, rebuilt8, motion[pend_slot], pend_depth, held);
                } else if (c.display_filter) {
                    store(cmd, shown, rebuilt8);
                    filter_image(cmd, 2, rebuilt8, motion[pend_slot], pend_depth, display_out);
                    show_out(cmd, display_out, fr);
                } else {
                    show_out(cmd, shown, fr);
                }
            } else {
                show_held(cmd, fr, tail_i);
            }
            if (c.display_filter) {
                filter_image(cmd, bank, rendered, motion[serial % 3], depth, bidi_held);
            } else {
                copy(cmd, rendered, bidi_held);
            }
            bidi_held_valid = true; bidi_pending = false;
        } else {   // kept for the next recording (composed there from both answers); the held picture out
            if (c.scrgb) {
                copy(cmd, fr.colour, native_input);
                dispatch(cmd, k_encode, {rd(0, native_input), wr(1, current)}, nullptr, 0, c.width, c.height);
            } else copy(cmd, fr.colour, current);
            if (c.delay) delayed_output(cmd, fr, held_valid);
            copy(cmd, depth, pend_depth);
            context_map(cmd, 0);
            age = phase;   // (phase + 1 - compute)
            pend_slot = uint32_t(serial % 3); pend_age = age; bidi_pending = true;
            go_tail();   // (delayed: the computed frame before's picture shown from `bidi_held` by the next frame)
            show_held(cmd, fr, tail_i, true);
        }
        if (result.network_calls == nr::frame_pair::failed_calls) {   // (the Vulkan stage's rule: afresh, nothing shown late)
            ok = false; result.completed = false; result.network_calls = 0; result.shown_late = 0;
            tails[size_t(tail_i)].picture = false; held_valid = false; valid = false; display_valid = false;
            reset_requested = true;
        }
        result.next_late=tails[size_t(tail_i)].picture && ok?2u:0u;
        finish(evaluate);
        restore_game(head, fr, true);
        ++cpu_frames;
        if (evaluate) ++cpu_computed;
        phase = (phase + 1u) % 2u;
        ++serial;
        result.ok = ok;
        return result;
    }
};

std::unique_ptr<Stage> Stage::make(const Config& c, std::function<void(IUnknown*)> retire, std::string* why) {
    if (!c.device || !c.width || !c.height || !supports(c.frame_format) || !c.motion_width || !c.motion_height ||
        !c.depth_width || !c.depth_height || !c.two_sided || !c.delay || (c.scrgb && c.frame_format != DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        *why = "frame reuse: invalid format, guide extent or two-sided schedule";
        return nullptr;
    }
    std::unique_ptr<Stage> s(new Stage());
    s->impl_ = std::make_unique<Impl>();
    s->impl_->cfg = c;
    s->impl_->device = c.device;
    s->impl_->retire = std::move(retire);
    if (!s->impl_->build(why)) return nullptr;
    return s;
}
bool Stage::ensure_pair(std::unique_ptr<Stage>& stage,const Config& c,std::function<void(IUnknown*)> retire,std::string* why) {
    if(stage) {
        const auto& k=stage->config();
        if(k.device==c.device && k.width==c.width && k.height==c.height && k.frame_format==c.frame_format &&
           k.scrgb==c.scrgb && k.context==c.context && k.motion_width==c.motion_width && k.motion_height==c.motion_height &&
           k.depth_width==c.depth_width && k.depth_height==c.depth_height) return true;
    }
    auto inner=c;inner.pair_kernel=true;
    stage=make(inner,std::move(retire),why);return bool(stage);
}
uint32_t Stage::render_pair(ID3D12GraphicsCommandList*& cmd,Frame* o,Frame& e,const PairNetwork& run) {
    return impl_->render_pair(cmd,o,e,run);
}
Stage::Result Stage::record_batch(ID3D12GraphicsCommandList* cmd,const Frame& f,const Batch& batch) {
    return impl_->record(cmd,f,{},batch);
}
Stage::~Stage() = default;
Stage::Result Stage::record(ID3D12GraphicsCommandList* cmd, const Frame& f, const Complete& complete) {
    return impl_->record(cmd, f, complete);
}
void Stage::reset() { impl_->reset_requested = true; }
void Stage::flush() {   // (Vulkan: the waiting part run with its answer going nowhere; nothing of before the gap shown after it)
    impl_->held_from_tail = false;
    impl_->bidi_pending = impl_->bidi_held_valid = false;
    impl_->start_in_tail.fill(false);
}
Stage::Split Stage::split() const {
    return {impl_->splitting, impl_->splitting ? impl_->share : 0.f, impl_->splitting ? impl_->splitting_ticket : nr::gpu::Ticket{}};
}
Stage::Pending Stage::take_pending() {
    Pending p;
    p.list = std::exchange(impl_->pending, nullptr);
    if (!p.list) return p;
    p.list->AddRef();
    for (const auto& t : impl_->tails) if (t.list == p.list) p.ticket = t.ticket;
    return p;
}
void Stage::set_tail_marks(std::function<void(ID3D12GraphicsCommandList*, bool)> f) { impl_->tail_mark = std::move(f); }
float Stage::balance(uint32_t* moves) const { if (moves) *moves = impl_->balance_moves; return impl_->share; }
uint32_t Stage::gap() const { return impl_->current_gap; }
uint64_t Stage::image_bytes() const { return impl_->bytes; }
const Stage::Config& Stage::config() const { return impl_->cfg; }
Stage::Stats Stage::stats() const { return {impl_->cpu_frames, impl_->cpu_computed}; }

}  // namespace nr::pe::d3d12_passthrough::frame_reuse12
