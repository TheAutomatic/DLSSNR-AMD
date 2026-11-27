#include "nr_pe_session.hpp"
#include "nr_frame_reuse_policy.hpp"
// HUD FIX's capture on the .asi route, D3D12 (nr_asi_hudcap.hpp; the rules: nr_hud_capture.hpp).
// What ReShade's events give the add-on, from hooks of our own (MinHook, the functions of the game's D3D12 objects,
// each hooked once and never removed; a detour calls the original first or last as the call needs):
//   the device: its render-target views (a descriptor -> the resource it views: which bind is of a back buffer, copied
//               descriptors followed) and its pipelines' depth test and blending (CreateGraphicsPipelineState,
//               CreatePipelineState, a pipeline library's LoadGraphicsPipeline / LoadPipeline) - from the game's swap
//               chain on, so its pipelines are known when the capture is armed later;
//   the lists:  the render targets, viewport, scissor and pipeline bound, each draw, clear, copy and resolve on a back
//               buffer, render passes, Reset (from the first armed frame on);
//   the queue:  ExecuteCommandLists - the lists' writes in the GPU's order.
// The copy goes into the game's list right before the first overlay after a scene write (every such place: lists are
// recorded on many threads, not in the GPU's order; nr::hudcap::kMaxCopies a frame at most), with the back buffer moved
// from the state that write has it in to COPY_SOURCE and back. Not followed: bundles (a bundle's write on the back buffer
// makes the frame not captured), copies inside a render pass (none can be made there).
// The chain capture (nr_hud_capture.hpp chain_picks; armed while the back buffer's last writes are scene writes): images
// of the back buffer's size whose format the HUD-less copy takes are followed on the lists as the back buffer is (their
// draws, clears and copies), and copied into an image of ours each (chain_dst) where their finished picture is - at the
// game's own transition out of the write state (render target, unordered access: a compute pass, copy or resolve
// destination), its state then the one the game names in that barrier, or before the first overlay drawn over them.
// Once one was picked at a Present, only the images picked lately are copied.
// D3D11 (further down): the immediate context's functions hooked (its vtable, from the device's GetImmediateContext); its
// bound states read at a draw on the back buffer (their GetDesc), the copy made by that context, at the last scene ->
// overlay place of the last frame (one copy a frame: the context runs in its own order). A deferred context's commands
// are not seen.
#include "nr_asi_hudcap.hpp"
#include "nr_d3d12_direct_state.hpp"
#include "nr_hud_capture.hpp"
#include "nr_hud_trace.hpp"
#include "nr_hud_root12.hpp"
#include "nr_hud_method_hook.hpp"
#include "nr_hud_indirect.hpp"
#include "nr_hud_inventory.hpp"
#include "nr_hud_slot_filter.hpp"
#include <map>
#include <memory>
#include "nr_pe_log.hpp"
#include "nr_pe_menu.hpp"
#include "nr_path_u8.hpp"
#include "nr_vram_ledger.hpp"

#include <MinHook.h>
#include <psapi.h>   // K32EnumProcessModules: every libxess_fg.dll of the process

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nr::asi::hudcap {
namespace {

using nr::pe::log;
using nr::hudcap::Rec;
using nr::hudcap::kLost;
using nr::hudcap::kScene;
using nr::hudcap::kOverlay;
using nr::hudcap::kCopy;
using nr::hudcap::kUnknown;
using nr::hudcap::kChain;
constexpr uint8_t kCleared = 6;   // a chain candidate's last write was a clear (an image the HUD is drawn into after)

// D3D12 pieces this MinGW's headers do not have (ID3D12GraphicsCommandList4's render passes, ID3D12Device2's pipeline
// state streams): only what is read.
struct RenderPassTarget {   // D3D12_RENDER_PASS_RENDER_TARGET_DESC's head (the first one is read)
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    UINT begin_type;        // D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE: 2 CLEAR
};
struct StreamDesc {         // D3D12_PIPELINE_STATE_STREAM_DESC
    SIZE_T size;
    void* stream;
};

thread_local int t_own = 0;
thread_local int t_in_present = 0;   // this thread is inside the Present hook (PresentCall)
thread_local int t_dlssg_handed = 0;  // inside a DLSS-G evaluate already handed the shown frame's inputs (DlssgHanded)

// One opt-in capture per process. File I/O only at the last requested Present, never per draw.
// A global observation frame is deliberately NOT called a game/FG frame identity.
struct Trace {
    bool enabled = false;
    uint64_t start = 0, frames = 60;
    std::atomic<uint64_t> frame{0}, generation{1};
    std::atomic<bool> finished{false};
    std::unique_ptr<nr::hudtrace::Ring> ring;
    Trace() {
        const char* e = std::getenv("NR_HUD_TRACE");
        enabled = e && std::strcmp(e, "1") == 0;
        if (!enabled) return;
        auto number = [](const char* key, uint64_t fallback, uint64_t max) {
            const char* v = std::getenv(key);
            if (!v || !*v || *v == '-') return fallback;
            char* end = nullptr;
            const auto n = std::strtoull(v, &end, 10);
            return end && !*end && n <= max ? uint64_t(n) : fallback;
        };
        start = number("NR_HUD_TRACE_START", 0, 1000000);
        frames = std::max(uint64_t(1), number("NR_HUD_TRACE_FRAMES", 60, 600));
        ring.reset(new nr::hudtrace::Ring(65536));
    }
    bool active() const { return enabled && !finished.load(std::memory_order_relaxed) && frame.load() >= start; }
    void add(const char* kind, uint64_t list=0, uint64_t gen=0, uint64_t a=0, uint64_t b=0, uint64_t c=0, uint64_t d=0) {
        if (!active()) return;
        ring->add({0, frame.load(), list, gen, a,b,c,d,GetCurrentThreadId(),kind});
    }
    void end_present() {
        if (!enabled || finished.load()) return;
        const uint64_t n = frame.fetch_add(1)+1;
        if (n < start+frames || finished.exchange(true)) return;
        uint64_t lost=0;
        auto events=ring->freeze(lost);
        const std::string file=std::string(nr::pe::module_folder())+"/dlssnr-hud-trace-"+std::to_string(GetCurrentProcessId())+".csv";
        FILE* f=nr::u8::fopen(file,"wb");
        if (!f) { log("[nr] HUD trace: export failed: %s",file.c_str()); return; }
        std::fprintf(f,"# hudtrace-v1 cpu-observation-order; lost=%llu; resource-addresses-not-lifetime-ids; no-enhanced-barriers-or-queue-waits\n",(unsigned long long)lost);
        std::fprintf(f,"seq,frame,thread,kind,list,generation,a,b,c,d\n");
        for (const auto& e:events)
            std::fprintf(f,"%llu,%llu,%u,%s,%llx,%llu,%llx,%llx,%llx,%llx\n",
                (unsigned long long)e.seq,(unsigned long long)e.frame,e.thread,e.kind,(unsigned long long)e.list,
                (unsigned long long)e.generation,(unsigned long long)e.a,(unsigned long long)e.b,
                (unsigned long long)e.c,(unsigned long long)e.d);
        const bool failed=std::ferror(f)!=0;
        const int closed=std::fclose(f);
        log("[nr] HUD trace: %s (%llu events, %llu overwritten): %s", failed || closed ? "export incomplete" : "saved",
            (unsigned long long)events.size(),(unsigned long long)lost,file.c_str());
    }
};
Trace& trace() { static Trace t; return t; }
struct TracePresent {
    ~TracePresent() { trace().end_present(); }
};

// Reasons on submitted kUnknown records (Rec::how is otherwise the chain-copy kind).
// kWhyComputeView: an indirect compute dispatch while the back buffer is bound, its proof left to the Present
// (the back buffer had no UAV view made by then: d3d12_present); dropped there when proven, else kWhyComputeUav.
enum : uint8_t { kWhyPso=1, kWhySignature=2, kWhyRaster=3, kWhyComputeUav=4, kWhyBundle=5, kWhyComputeView=6 };
std::atomic<uint64_t> indirect_by_state{0}, indirect_by_view{0}, indirect_view_seen{0}, indirect_unwatched{0};
std::atomic<uint64_t> signature_seen{0}, signature_saved{0}, indirect_skipped{0};
static const GUID kIndirectMetadata = {0x3e2d7614,0xbcd9,0x4a7b,{0x99,0xaf,0x22,0x37,0x6c,0x9d,0x13,0x01}};
struct IndirectMetadata { uint32_t version=1; nr::hudcap::IndirectKind kind=nr::hudcap::IndirectKind::Unknown; };
nr::hudcap::IndirectKind signature_kind(ID3D12CommandSignature* sig) {
    IndirectMetadata data; UINT size=sizeof(data);
    if (!sig || FAILED(sig->GetPrivateData(kIndirectMetadata,&size,&data)) || size!=sizeof(data) || data.version!=1)
        return nr::hudcap::IndirectKind::Unknown;
    return data.kind;
}

struct PsoFacts {
    int8_t depth = -1, blend = -1;
};
struct List {
    uint64_t trace_generation = 0; // recording identity only, not resource lifetime
    uint64_t back = 0;                // the back buffer bound as a render target (else 0)
    bool dsv = false;
    ID3D12PipelineState* pso = nullptr;
    D3D12_VIEWPORT vp{};
    D3D12_RECT scissor{};
    bool in_pass = false;
    uint8_t last = 0;
    std::vector<Rec> recs;
    // the chain capture: a candidate bound as render target 0 (else 0), and the candidates written on this list with
    // their last write's kind (kScene, kOverlay, kCleared, kUnknown)
    uint64_t rt = 0;
    struct Cand { uint64_t image; uint8_t last; uint32_t state = 0; bool state_known = false; };
    std::vector<Cand> cands;
    Cand* cand(uint64_t image, bool make) {
        for (Cand& c : cands)
            if (c.image == image) return &c;
        if (!make) return nullptr;
        cands.push_back({image, 0});
        return &cands.back();
    }
    // the input side: the graphics root signature's descriptor tables bound (GPU handles, by root parameter; 0 none)
    ID3D12RootSignature* root = nullptr;
    std::shared_ptr<const nr::hudcap::RootLayout> layout;
    std::vector<uint64_t> tables;
    uint64_t heap_cpu=0, heap_gpu=0, heap_count=0;
    // A back buffer's state as this list's own legacy transitions left it (seen while the capture was armed, in the
    // capture's epoch `epoch`); valid=false: a split barrier's begin, an aliasing barrier, or an enhanced barrier since.
    struct BackState { uint64_t image; uint32_t state; uint64_t epoch; bool valid; };
    std::vector<BackState> back_states;
    BackState* back_state(uint64_t image, bool make) {
        for (BackState& b : back_states)
            if (b.image == image) return &b;
        if (!make) return nullptr;
        back_states.push_back({image, 0, 0, false});
        return &back_states.back();
    }
    void reset() {
        back = 0; dsv = false; pso = nullptr; vp = {}; scissor = {}; in_pass = false; last = 0;
        back_states.clear();
        recs.clear();
        rt = 0;
        cands.clear();
        root=nullptr; layout.reset(); tables.clear(); heap_cpu=heap_gpu=heap_count=0;
    }
};

struct ChainSnapshot {
    ID3D12Resource* dst=nullptr;
    ID3D12Device* device=nullptr;
    bool untracked=false;
    uint32_t w=0,h=0; DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    std::mutex mutex;
    std::vector<nr::gpu::Ticket> uses;
    uint64_t retired_at=0;   // the Present it went back to the pool (chain_prune: spares)
    ~ChainSnapshot() { if (dst) dst->Release(); if (device) device->Release(); }
    // a use is over when its recording is sealed and done, or (not sealed: nr_gpu_completion.hpp ran_ready) has run
    static bool done(const nr::gpu::Ticket& t) { return t->capture_ready() || t->ran_ready(); }
    void touch(const nr::gpu::Ticket& t) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!t) untracked=true;
        uses.erase(std::remove_if(uses.begin(),uses.end(),[](const auto& u){return done(u);}),uses.end());
        if (t && std::find(uses.begin(),uses.end(),t)==uses.end()) uses.push_back(t);
    }
    bool ready() {
        std::lock_guard<std::mutex> lock(mutex);
        return !untracked && std::all_of(uses.begin(),uses.end(),[](const auto& t){return done(t);});
    }
};
struct State {
    std::atomic<bool> armed{false};
    std::atomic<uint64_t> arm_epoch{1};      // +1 each time the capture is armed again (list states from before: void)
    std::atomic<ID3D12Device*> device{nullptr};
    std::atomic<uint64_t> backs[8];
    std::atomic<bool> back_watched[8];       // backs[i]'s device had every UAV view creation followed (watched_devices)
    std::atomic<bool> enhanced_unhooked{false}; // ID3D12GraphicsCommandList7::Barrier exists but is not followed
    std::atomic<uint32_t> back_w{0}, back_h{0};
    std::atomic<ID3D12Resource*> hudless{nullptr};
    std::atomic<uint32_t> copies{0};
    std::atomic<bool> foreign{false};
    UINT rtv_step = 0;
    std::shared_mutex views_m;
    std::unordered_map<uint64_t, uint64_t> rtvs;           // RTV descriptor -> the resource it views
    std::shared_mutex pipes_m;
    std::unordered_map<ID3D12PipelineState*, PsoFacts> pipes;
    std::mutex m;                                          // lists' map, frame, retired
    std::unordered_map<void*, List> lists;                 // never erased (threads keep pointers to the entries)
    std::vector<Rec> frame;                                // the executed lists' writes, in the GPU's order
    struct Retired { ID3D12Resource* r; uint64_t at; };
    std::vector<Retired> retired;                          // earlier copies' images, released 64 Presents later
    uint64_t presents = 0;
    bool device_hooked = false, lists_hooked = false, late = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // the chain capture (chain_m: the copies' images, the picked and refused images, the check's state)
    std::atomic<bool> chain{false};          // armed: copies are made
    std::atomic<int> back_group{0};          // format_group of the HUD-less copy's format
    std::atomic<uint64_t> chain_stamp{0};    // the Presents (g.presents, read on the recording threads)
    std::mutex chain_m;
    struct ChainDst { uint64_t image = 0; ID3D12Resource* dst = nullptr; uint64_t used = 0, wrote = ~0ull; std::shared_ptr<ChainSnapshot> owner; };   // wrote: the
                                                                         // write side's copy's frame (the input side skips it)
    ChainDst chain_dst[4];
    std::vector<std::shared_ptr<ChainSnapshot>> chain_retired; // leases + strict completion, not Present age
    uint64_t chain_keys[4] = {};             // the images picked lately: diagnostic history only, never a capture whitelist
    struct Refused { uint64_t image, until; uint32_t strikes = 1; };
    std::vector<Refused> refused;            // images whose copy did not fit the presented frame, refused till `until`
    struct Check { uint64_t image = 0; uint32_t ok = 0, bad = 0; bool trusted = false; uint64_t at = 0; bool fitted = false; uint32_t misfits = 0; };
    Check checks[4];                         // the check by image: fitting results in a row, misfits in a row (trusted)
    uint64_t clean_at = 0;                   // the last Present whose back buffer had nothing after its scene write
    uint32_t misses = 0;                     // armed Presents in a row without a pick
    uint32_t miss_reports = 0;               // bounded diagnostics for source rediscovery
    uint32_t said = 0;                       // the check's lines logged
    bool chain_said = false;                 // (log) the first frame the chain serves
    uint32_t captured_run = 0;               // Presents in a row the capture before the HUD served (the chain not needed)
    std::unordered_set<uint32_t> other_formats; // guarded by diag_m; each unsupported format logged once
    // the input side: shader-resource views (a CBV_SRV_UAV descriptor's CPU address -> the image a 2D texture view views)
    // and the shader-visible heaps (their CPU / GPU starts) the game binds
    // (the views themselves: g_views below)
    // the candidates whose writes the hooks saw, and the frame (chain_stamp) they last saw one in (under chain_m): the
    // input side takes only this frame's - a stale descriptor in a bound table may name a freed resource
    std::unordered_map<uint64_t, uint64_t> written;
    UINT srv_step = 0;
    // (the diagnostic line) the last final pass's inputs of the back buffer's size this frame, by image: format, how many
    // were copied by each side this frame
    std::mutex diag_m;
    std::vector<std::pair<uint64_t, uint32_t>> final_inputs;
    std::atomic<uint32_t> copies_write{0}, copies_input{0};
    std::atomic<uint64_t> input_draws{0}, input_no_root{0}, input_no_heap{0}, input_probes{0};
    std::string said_diag;
    uint32_t diag_lines = 0;
} g;
// The input side's views (nr_hud_slot_filter.hpp ViewTable: sharded, a lock-free filter in front), the resources that
// ever had a UAV view made (set-only), the devices whose every UAV view creation was followed (hooked as they were
// made, before the device was handed to its creator).
nr::hudcap::ViewTable g_views;
nr::hudcap::AddressBloom g_uav_views;
std::mutex g_watched_m;
std::vector<ID3D12Device*> g_watched_devices;
// The views' bookkeeping runs only while HUD FIX wants the capture (Auto, Captured): its hooks (CBV / SRV / UAV
// creation) are installed then, never with HUD FIX off from the start (0.0.8.3's cost: none of them); off again, a flag
// stops it (g_views_on). g_wanted: -1 not known yet (the device's creation asks wanted_query: [HudFix] Mode in the ini).
std::atomic<bool> g_views_on{false}, g_views_hooked{false};
std::atomic<int> g_wanted{-1};
bool (*g_wanted_query)() = nullptr;
std::mutex g_views_m;   // installing / stopping
int format_group(DXGI_FORMAT f);

bool is_back(uint64_t h) {
    if (!h) return false;
    for (const auto& b : g.backs)
        if (b.load(std::memory_order_relaxed) == h) return true;
    return false;
}

List* list_of(void* cl) {
    thread_local void* last_cl = nullptr;
    thread_local List* last_state = nullptr;
    if (cl == last_cl) return last_state;
    std::lock_guard<std::mutex> lk(g.m);
    List* s = &g.lists[cl];
    last_cl = cl;
    last_state = s;
    return s;
}

void trace_list(const char* kind, ID3D12GraphicsCommandList* cl, uint64_t a=0, uint64_t b=0, uint64_t c=0, uint64_t d=0) {
    if (t_own || !trace().active()) return;
    List* s = list_of(cl);
    if (!s->trace_generation) s->trace_generation = trace().generation.fetch_add(1);
    trace().add(kind, reinterpret_cast<uint64_t>(cl), s->trace_generation, a,b,c,d);
}

PsoFacts facts_of(ID3D12PipelineState* p) {
    if (!p) return {};
    std::shared_lock<std::shared_mutex> lk(g.pipes_m);
    const auto it = g.pipes.find(p);
    if (it != g.pipes.end()) return it->second;
    PsoFacts unknown;   // made before the capture followed the device: not known
    unknown.depth = unknown.blend = -2;
    return unknown;
}

void transition(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (a == b) return;
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    l->ResourceBarrier(1, &x);
}

// A write of `kind` on back buffer `b` in `cl` (`state`: what the game has it in for that write): first the copy, where
// an overlay follows a scene write or opens the list.
void write(ID3D12GraphicsCommandList* cl, List& s, uint64_t b, uint8_t kind, D3D12_RESOURCE_STATES state) {
    if (kind == kOverlay && s.last != kOverlay) {
        ID3D12Resource* h = g.hudless.load(std::memory_order_acquire);
        if (h && !s.in_pass && g.copies.fetch_add(1, std::memory_order_relaxed) < nr::hudcap::kMaxCopies) {
            ++t_own;
            ID3D12Resource* back = reinterpret_cast<ID3D12Resource*>(b);
            transition(cl, back, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(cl, h, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyResource(h, back);
            transition(cl, h, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(cl, back, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            --t_own;
            s.recs.push_back({kCopy, b, reinterpret_cast<uint64_t>(h)});
        }
    }
    if (s.recs.size() >= nr::hudcap::kMaxRecs) {   // never executed: kept short, the frame not trusted
        s.recs.clear();
        s.recs.push_back({kLost, b});
    }
    s.recs.push_back({kind, b, 0, uint8_t(kind == kUnknown ? kWhyPso : 0)});
    s.last = kind;
}

bool full_box(const D3D12_BOX* box) {
    return !box || (box->left == 0 && box->top == 0 && box->right >= g.back_w.load(std::memory_order_relaxed) &&
                    box->bottom >= g.back_h.load(std::memory_order_relaxed));
}

uint8_t draw_kind(const List& s, uint32_t vertices, uint32_t instances, bool indirect) {
    const PsoFacts f = facts_of(s.pso);
    nr::hudcap::Draw d;
    d.dsv = s.dsv;
    d.depth = f.depth;
    d.blend = f.blend;
    d.scissor_on = true;   // D3D12 always cuts by its scissor
    d.vp[0] = s.vp.TopLeftX; d.vp[1] = s.vp.TopLeftY; d.vp[2] = s.vp.Width; d.vp[3] = s.vp.Height;
    d.sc[0] = s.scissor.left; d.sc[1] = s.scissor.top; d.sc[2] = s.scissor.right; d.sc[3] = s.scissor.bottom;
    d.vertices = vertices; d.instances = instances; d.indirect = indirect;
    return nr::hudcap::draw_kind(d, g.back_w.load(std::memory_order_relaxed), g.back_h.load(std::memory_order_relaxed));
}

bool on() { return g.armed.load(std::memory_order_relaxed) && !t_own; }

// ---- the chain capture -----------------------------------------------------------------------------------------------
// A candidate: a 2D colour image of the back buffer's size, one level, one sample, of the HUD-less copy's format group
// (CopyResource), not a back buffer.
bool chain_candidate(ID3D12Resource* r) {
    if (!r || !g.chain.load(std::memory_order_relaxed)) return false;
    const uint64_t h = reinterpret_cast<uint64_t>(r);
    if (is_back(h)) return false;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != g.back_w.load(std::memory_order_relaxed) ||
        d.Height != g.back_h.load(std::memory_order_relaxed) || d.DepthOrArraySize != 1 || d.MipLevels != 1 ||
        d.SampleDesc.Count != 1)
        return false;
    if (format_group(d.Format) != g.back_group.load(std::memory_order_relaxed)) {
        bool first;
        {
            std::lock_guard<std::mutex> lk(g.diag_m);
            first = g.other_formats.insert(uint32_t(d.Format)).second;
        }
        if (first) {
            log("[nr] HUD FIX chain: an image of the back buffer's size in DXGI format %u is not taken (the copy takes "
                "the back buffer's format group only)", unsigned(d.Format));
        }
        return false;
    }
    return true;
}
bool chain_wanted(uint64_t image) {
    std::lock_guard<std::mutex> lk(g.chain_m);
    const uint64_t now = g.chain_stamp.load(std::memory_order_relaxed);
    for (const auto& x : g.refused)
        if (x.image == image && x.until > now) return false;
    // Buffer rings routinely change the scene address each frame. A prior pick must NEVER veto a new
    // current-frame candidate: reopening only after a miss still loses every other frame on a rotating ring.
    // Refusal, per-frame copy budget and immutable snapshot slots bound the work independently.
    return true;
}
std::atomic<uint64_t> g_chain_made{0};   // chain snapshot images made this session (the HUD FIX Captured line)
// Spares: a finished snapshot of the current shape stays in the pool for kChainSpareKeep Presents after it went back, so
// the next copies take it. Before (DL:TB 11-10, 4K, Captured): the first finished one was taken and every other finished
// one released at once, so the copies after it in the same frame found nothing finished and made new 4K images - 250-350
// made per 10 s with 0-28 waiting. The most recently returned spare is taken first: spares the copies no longer need
// stay unused and go after kChainSpareKeep Presents (also when the capture stops: the 120-Present report prunes).
constexpr uint64_t kChainSpareKeep = 120;
// (under chain_m) `take`: the most recently returned finished snapshot of the current shape, out of the pool; finished
// ones of another shape / device and spares unused for kChainSpareKeep Presents released
std::shared_ptr<ChainSnapshot> chain_prune(uint64_t now, bool take) {
    const auto fits=[](const std::shared_ptr<ChainSnapshot>& s) {
        return s->device==g.device.load() && s->w==g.back_w.load() && s->h==g.back_h.load() && s->format==g.format;
    };
    std::shared_ptr<ChainSnapshot> snapshot;
    if (take) {
        auto best=g.chain_retired.end();
        for (auto it=g.chain_retired.begin();it!=g.chain_retired.end();++it)
            if (it->use_count()==1 && fits(*it) && (*it)->ready() &&
                (best==g.chain_retired.end() || (*it)->retired_at>=(*best)->retired_at)) best=it;
        if (best!=g.chain_retired.end()) { snapshot=std::move(*best); g.chain_retired.erase(best); }
    }
    for (auto it=g.chain_retired.begin();it!=g.chain_retired.end();) {
        if (it->use_count()!=1 || !(*it)->ready() || (fits(*it) && now-(*it)->retired_at<kChainSpareKeep)) {++it;continue;}
        it=g.chain_retired.erase(it);   // released
    }
    return snapshot;
}
// (under chain_m) a snapshot back to the pool: reused once its uses are done (ChainSnapshot::ready)
void chain_retire(std::shared_ptr<ChainSnapshot>&& s) {
    s->retired_at=g.chain_stamp.load();
    g.chain_retired.push_back(std::move(s));
}
// A snapshot is immutable while a recording/submission/Present still owns it. Reuse completed retired storage.
std::shared_ptr<ChainSnapshot> chain_dst_for(uint64_t image, const nr::gpu::Ticket& writer) {
    std::lock_guard<std::mutex> lk(g.chain_m);
    const uint64_t now=g.chain_stamp.load();
    State::ChainDst* slot=nullptr;
    for (auto& e:g.chain_dst) {
        if (e.used!=now || !e.dst) if (!slot || e.used<slot->used) slot=&e;
    }
    if (!slot) return {};
    if (slot->owner) chain_retire(std::move(slot->owner));
    *slot={};
    std::shared_ptr<ChainSnapshot> snapshot=chain_prune(now,true);
    ID3D12Device* device=g.device.load();
    if (!device) return {};
    const bool reused=bool(snapshot);
    if (!snapshot) {
        snapshot=std::make_shared<ChainSnapshot>();
        snapshot->device=device; device->AddRef();
        snapshot->w=g.back_w.load(); snapshot->h=g.back_h.load(); snapshot->format=g.format;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width=snapshot->w; rd.Height=snapshot->h; rd.DepthOrArraySize=1; rd.MipLevels=1;
        rd.Format=g.format; rd.SampleDesc.Count=1; rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr,IID_PPV_ARGS(&snapshot->dst)))) return {};
        nr::vram::track_d3d12(device, snapshot->dst, nr::vram::kCopies);   // (in NR's VRAM breakdown: frame copies)
        g_chain_made.fetch_add(1, std::memory_order_relaxed);
    }
    trace().add(reused ? "pool-reuse" : "pool-allocate",0,0,reinterpret_cast<uint64_t>(snapshot->dst),g.chain_retired.size(),now);
    snapshot->touch(writer);
    *slot={image,snapshot->dst,now,~0ull,snapshot};
    return snapshot;
}
// `image` (in `state`, the game's) copied into its chain image on `cl`; `how` as Rec::how (the input side skips an image
// the write side copied this frame: that copy is its finished scene, before any overlay drawn over it)
void chain_copy(ID3D12GraphicsCommandList* cl, List& s, uint64_t image, D3D12_RESOURCE_STATES state, uint8_t how = 0) {
    // (NR_TEST_CHAIN_INPUT_ONLY=1, a test hook: the input side alone)
    static const bool input_only = [] { const char* e = std::getenv("NR_TEST_CHAIN_INPUT_ONLY"); return e && e[0] == '1'; }();
    if (s.in_pass || (input_only && how == 0) || !chain_wanted(image)) return;
    const uint64_t now = g.chain_stamp.load(std::memory_order_relaxed);
    if (how == 1) {
        std::lock_guard<std::mutex> lk(g.chain_m);
        for (const State::ChainDst& e : g.chain_dst)
            if (e.image == image && e.wrote == now) return;
    }
    if (g.copies.fetch_add(1, std::memory_order_relaxed) >= 2 * nr::hudcap::kMaxCopies) return;
    const auto writer=d3d12_recording(cl);
    if (!writer) return;
    const auto snapshot=chain_dst_for(image,writer);
    if (!snapshot) return;
    ID3D12Resource* dst=snapshot->dst;
    if (how == 0) {
        std::lock_guard<std::mutex> lk(g.chain_m);
        for (State::ChainDst& e : g.chain_dst)
            if (e.image == image) e.wrote = now;
    }
    (how ? g.copies_input : g.copies_write).fetch_add(1, std::memory_order_relaxed);
    ID3D12Resource* src = reinterpret_cast<ID3D12Resource*>(image);
    ++t_own;
    transition(cl, src, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(cl, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(dst, src);
    transition(cl, dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cl, src, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    --t_own;
    if (s.recs.size() >= nr::hudcap::kMaxRecs) {
        s.recs.clear();
        s.recs.push_back({kLost, 0});
    }
    trace_list("snapshot", cl, image, reinterpret_cast<uint64_t>(dst), state, how);
    s.recs.push_back({kChain, image, reinterpret_cast<uint64_t>(dst), how, snapshot});
}
// Resolve typed SRV ranges of the bound graphics root signature against its CURRENT CBV/SRV/UAV heap.
// A bound view is a possible input, not evidence that the shader reads it or that it is HUD-free.
void chain_inputs(ID3D12GraphicsCommandList* cl, List& s) {
    ++g.input_draws;
    if (!s.layout) { ++g.input_no_root; return; }
    if (!g.srv_step || !s.heap_count) { ++g.input_no_heap; return; }
    std::vector<std::pair<uint64_t,uint64_t>> probes;
    {
        for (size_t i=0;i<s.tables.size() && i<s.layout->tables.size();++i) {
            const uint64_t table=s.tables[i];
            if (table<s.heap_gpu || (table-s.heap_gpu)%g.srv_step) continue;
            const uint64_t base=(table-s.heap_gpu)/g.srv_step;
            if (base>=s.heap_count) continue;
            for (auto span:s.layout->tables[i]) {
                span=nr::hudcap::clip_span(span,s.heap_count-base);
                if (!span.count) continue;
                const uint64_t begin=s.heap_cpu+(base+span.offset)*g.srv_step, end=begin+span.count*g.srv_step;
                g_views.each(begin,end,[&](uint64_t at,uint64_t res){ probes.push_back({s.heap_gpu+(at-s.heap_cpu),res}); });
            }
        }
    }
    g.input_probes.fetch_add(probes.size());
    std::vector<std::pair<uint64_t, uint32_t>> seen;
    for (const auto& probe:probes) {
        const uint64_t image=probe.second;
        trace_list("input-probe", cl, probe.first, image);
            if (!image) continue;
            {   // only an image observed written recently (freshness filter, NOT an ownership proof; another
                // thread's Present moves the count)
                std::lock_guard<std::mutex> lk(g.chain_m);
                const auto it = g.written.find(image);
                const uint64_t now = g.chain_stamp.load(std::memory_order_relaxed);
                if (it == g.written.end() || it->second + 1 < now || it->second > now) {
                    trace_list("input-age-reject", cl, image, it == g.written.end() ? ~0ull : it->second, now);
                    continue;
                }
            }
            bool dup = false;
            for (const auto& x : seen) dup = dup || x.first == image;
            if (dup || !chain_candidate(reinterpret_cast<ID3D12Resource*>(image))) continue;
            seen.push_back({image, uint32_t(reinterpret_cast<ID3D12Resource*>(image)->GetDesc().Format)});
            const List::Cand* c = s.cand(image, false);
            const D3D12_RESOURCE_STATES st = c && c->state_known ? D3D12_RESOURCE_STATES(c->state)
                                                                 : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            chain_copy(cl, s, image, st, 1);
    }
    std::lock_guard<std::mutex> lk(g.diag_m);
    g.final_inputs = std::move(seen);
}
void mark_written(uint64_t image) {
    std::lock_guard<std::mutex> lk(g.chain_m);
    if (g.written.size() > 4096) g.written.clear();
    g.written[image] = g.chain_stamp.load(std::memory_order_relaxed);
}
// A write of `kind` on candidate `image` (`state`: the game's for it): before an overlay over a finished scene, the copy
void chain_write(ID3D12GraphicsCommandList* cl, List& s, uint64_t image, uint8_t kind, D3D12_RESOURCE_STATES state) {
    trace_list("candidate-write", cl, image, kind, state);
    mark_written(image);
    List::Cand* c = s.cand(image, true);
    if (kind == kOverlay && c->last == kScene) chain_copy(cl, s, image, state);
    c->last = kind;
}

// ---- the detours --------------------------------------------------------------------------------------------------
using PFN_Reset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using PFN_Draw = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using PFN_DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using PFN_CopyTextureRegion = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT,
                                                       UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using PFN_CopyResource = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using PFN_Resolve = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT);
using PFN_Viewports = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using PFN_Scissors = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
using PFN_SetPipeline = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using PFN_ExecuteBundle = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
using PFN_SetTargets = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                                const D3D12_CPU_DESCRIPTOR_HANDLE*);
using PFN_ClearTarget = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT,
                                                 const D3D12_RECT*);
using PFN_ExecuteIndirect = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*,
                                                     UINT64, ID3D12Resource*, UINT64);
using PFN_BeginPass = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const RenderPassTarget*, const void*, UINT);
using PFN_EndPass = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using PFN_Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PFN_CreateRtv = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                               D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CopyDescriptors = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                                     const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_CopyDescriptorsSimple = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_CreateGraphics = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using PFN_CreateStream = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const StreamDesc*, REFIID, void**);
using PFN_LoadGraphics = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,
                                                     REFIID, void**);
using PFN_LoadStream = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, LPCWSTR, const StreamDesc*, REFIID, void**);

PFN_Reset real_reset;
PFN_Draw real_draw;
PFN_DrawIndexed real_draw_indexed;
PFN_CopyTextureRegion real_copy_region;
PFN_CopyResource real_copy_resource;
PFN_Resolve real_resolve;
PFN_Viewports real_viewports;
PFN_Scissors real_scissors;
PFN_SetPipeline real_set_pipeline;
PFN_ExecuteBundle real_execute_bundle;
PFN_SetTargets real_set_targets;
PFN_ClearTarget real_clear_target;
PFN_ExecuteIndirect real_execute_indirect;
PFN_BeginPass real_begin_pass;
PFN_EndPass real_end_pass;
PFN_Execute real_execute;








using PFN_CreateSignature = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID, void**);
HRESULT STDMETHODCALLTYPE hk_create_signature(PFN_CreateSignature real, ID3D12Device* device,
    const D3D12_COMMAND_SIGNATURE_DESC* desc, ID3D12RootSignature* root, REFIID iid, void** out) {
    const HRESULT hr=real(device,desc,root,iid,out);
    if (SUCCEEDED(hr) && out && *out && desc) {
        ++signature_seen;
        ID3D12CommandSignature* sig=nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&sig))) && sig) {
            nr::pe::d3d12_direct::note_command_signature(sig, desc);
            IndirectMetadata data; data.kind=nr::hudcap::indirect_kind(desc->NumArgumentDescs,desc->pArgumentDescs);
            // Object-owned bytes: no global pointer cache, no stale entry on address reuse.
            if (SUCCEEDED(sig->SetPrivateData(kIndirectMetadata,sizeof(data),&data))) ++signature_saved;
            sig->Release();
        }
    }
    return hr;
}

void remember(void** out, const PsoFacts& f) {
    if (!out || !*out) return;
    std::unique_lock<std::shared_mutex> lk(g.pipes_m);
    g.pipes[static_cast<ID3D12PipelineState*>(*out)] = f;
}
PsoFacts facts_of_desc(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d) {
    PsoFacts f;
    if (!d) return f;
    f.depth = d->DepthStencilState.DepthEnable && d->DSVFormat != DXGI_FORMAT_UNKNOWN ? 1 : 0;
    f.blend = d->BlendState.RenderTarget[0].BlendEnable ? 1 : 0;
    return f;
}
// A pipeline state stream's depth test and blending (D3D12_PIPELINE_STATE_SUBOBJECT_TYPE; each subobject a type word
// and its payload, both at pointer alignment). A subobject this reader does not know ends the reading (the rest stays
// not known).
PsoFacts facts_of_stream(const StreamDesc* d) {
    PsoFacts f;
    if (!d || !d->stream) return f;
    const uint8_t* p = static_cast<const uint8_t*>(d->stream);
    const uint8_t* end = p + d->size;
    // payload sizes by type (x64), 0: not known
    static const uint32_t kPayload[30] = {8, 16, 16, 16, 16, 16, 16, 32, 328, 4, 44, 52, 16, 4, 4, 36, 4, 8, 4, 16, 4,
                                          56, 24, 0, 16, 16, 60, 44, 40, 16};
    static const uint32_t kAlign[30] = {8, 8, 8, 8, 8, 8, 8, 8, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 8, 4,
                                        4, 8, 0, 8, 8, 4, 4, 4, 8};
    bool depth_set = false, dsv_known = false, dsv_format = false, whole = true;
    int depth_enable = -1;
    while (p + sizeof(UINT) <= end) {
        UINT type;
        std::memcpy(&type, p, sizeof type);
        if (type >= 30 || !kPayload[type]) { whole = false; break; }
        const size_t at = (sizeof(UINT) + kAlign[type] - 1) / kAlign[type] * kAlign[type];
        const uint8_t* payload = p + at;
        if (payload + kPayload[type] > end) { whole = false; break; }
        if (type == 8) {   // BLEND: D3D12_BLEND_DESC, RenderTarget[0].BlendEnable after two BOOLs
            BOOL b;
            std::memcpy(&b, payload + 8, sizeof b);
            f.blend = b ? 1 : 0;
        } else if (type == 11 || type == 21 || type == 26) {   // DEPTH_STENCIL / 1 / 2: DepthEnable first
            BOOL b;
            std::memcpy(&b, payload, sizeof b);
            depth_enable = b ? 1 : 0;
            depth_set = true;
        } else if (type == 16) {   // DEPTH_STENCIL_FORMAT
            DXGI_FORMAT fm;
            std::memcpy(&fm, payload, sizeof fm);
            dsv_known = true;
            dsv_format = fm != DXGI_FORMAT_UNKNOWN;
        }
        p += (at + kPayload[type] + sizeof(void*) - 1) / sizeof(void*) * sizeof(void*);
    }
    if (depth_set) f.depth = depth_enable && (!dsv_known || dsv_format) ? 1 : 0;
    else if (dsv_known && whole) f.depth = dsv_format ? 1 : 0;   // the default: depth on, where a depth format is set
    else if (!whole) f.depth = -2;                               // a stream read in part: not known
    if (f.blend < 0) f.blend = whole ? 0 : -2;                   // the default: off; read in part: not known
    return f;
}

HRESULT STDMETHODCALLTYPE hk_create_graphics(PFN_CreateGraphics real_create_graphics, ID3D12Device* d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID iid,
                                             void** out) {
    const HRESULT hr = real_create_graphics(d, desc, iid, out);
    if (SUCCEEDED(hr)) remember(out, facts_of_desc(desc));
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_create_stream(PFN_CreateStream real_create_stream, ID3D12Device* d, const StreamDesc* desc, REFIID iid, void** out) {
    const HRESULT hr = real_create_stream(d, desc, iid, out);
    if (SUCCEEDED(hr)) remember(out, facts_of_stream(desc));
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_load_graphics(PFN_LoadGraphics real_load_graphics, IUnknown* lib, LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                           REFIID iid, void** out) {
    const HRESULT hr = real_load_graphics(lib, name, desc, iid, out);
    if (SUCCEEDED(hr)) remember(out, facts_of_desc(desc));
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_load_stream(PFN_LoadStream real_load_stream, IUnknown* lib, LPCWSTR name, const StreamDesc* desc, REFIID iid,
                                         void** out) {
    const HRESULT hr = real_load_stream(lib, name, desc, iid, out);
    if (SUCCEEDED(hr)) remember(out, facts_of_stream(desc));
    return hr;
}
void STDMETHODCALLTYPE hk_create_rtv(PFN_CreateRtv real_create_rtv, ID3D12Device* d, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                     D3D12_CPU_DESCRIPTOR_HANDLE h) {
    real_create_rtv(d, r, desc, h);
    std::unique_lock<std::shared_mutex> lk(g.views_m);
    if (r) g.rtvs[h.ptr] = reinterpret_cast<uint64_t>(r);
    else g.rtvs.erase(h.ptr);
}
// A view other than a tracked texture's written at `at`: the tracked one there (if any) goes. The usual call (a constant
// buffer, a UAV, a small or mipped texture's view - thousands a frame in some games) finds none without a lock.
void erase_srv(SIZE_T at) { g_views.erase(at); }
// The input side's views: a 2D texture's shader-resource view (mip 0 seen; any other kind of view clears the address)
using PFN_CreateSrv = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*,
                                               D3D12_CPU_DESCRIPTOR_HANDLE);

void STDMETHODCALLTYPE hk_create_srv(PFN_CreateSrv real_create_srv, ID3D12Device* d, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                     D3D12_CPU_DESCRIPTOR_HANDLE h) {
    real_create_srv(d, r, desc, h);
    if (!g_views_on.load(std::memory_order_relaxed)) return;
    if (!t_own) trace().add("srv-create", 0, 0, h.ptr, reinterpret_cast<uint64_t>(r), desc ? desc->ViewDimension : 0);
    // (recorded from the start - views are made once, at load, and copied into the bound heaps each frame - but only a
    // 2D texture of one level and one sample, 640 x 360 or more: a back buffer's size could be one)
    bool keep = r && (!desc || desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D);
    if (keep) {
        const D3D12_RESOURCE_DESC rd = r->GetDesc();
        if (!t_own) trace().add("resource-shape", 0, 0, reinterpret_cast<uint64_t>(r), rd.Format, rd.Width, (uint64_t(rd.Height)<<32)|rd.MipLevels);
        keep = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.MipLevels == 1 && rd.SampleDesc.Count == 1 &&
               rd.DepthOrArraySize == 1 && rd.Width >= 640 && rd.Height >= 360;
    }
    if (keep) {
        g_views.set(h.ptr, reinterpret_cast<uint64_t>(r));
        return;
    }
    erase_srv(h.ptr);
}
// Keep persistent heaps current while capture is off too. Almost every copy touches no tracked view: the table's filter
// says so without a lock (the game's descriptor copies run on all of its recording threads; one lock for all of them
// made them wait for each other, HUD FIX off as well). A copy that may touch one locks only the shards of its ranges.
void copy_srvs(SIZE_T dst, SIZE_T src, UINT n, UINT step) {
    if (!step || !n) return;
    g_views.copy(dst, src, uint64_t(n)*step, [](uint64_t at, uint64_t from, uint64_t res) { trace().add("srv-copy",0,0,at,from,res); });
}
void copy_views(SIZE_T dst, SIZE_T src, UINT n, UINT step) {
    if (!step) return;
    std::unique_lock<std::shared_mutex> lk(g.views_m);
    for (UINT i = 0; i < n; ++i) {
        const auto it = g.rtvs.find(src + SIZE_T(i) * step);
        if (it != g.rtvs.end()) g.rtvs[dst + SIZE_T(i) * step] = it->second;
        else g.rtvs.erase(dst + SIZE_T(i) * step);
    }
}
void STDMETHODCALLTYPE hk_copy_descriptors_simple(PFN_CopyDescriptorsSimple real_copy_descriptors_simple, ID3D12Device* d, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    real_copy_descriptors_simple(d, n, dst, src, type);
    if (!g_views_on.load(std::memory_order_relaxed)) return;
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV) copy_views(dst.ptr, src.ptr, n, d->GetDescriptorHandleIncrementSize(type));
    else if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && g_views_on.load(std::memory_order_relaxed))
        copy_srvs(dst.ptr, src.ptr, n, d->GetDescriptorHandleIncrementSize(type));
}
void STDMETHODCALLTYPE hk_copy_descriptors(PFN_CopyDescriptors real_copy_descriptors, ID3D12Device* d, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dst, const UINT* dsizes,
                                           UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* src, const UINT* ssizes,
                                           D3D12_DESCRIPTOR_HEAP_TYPE type) {
    real_copy_descriptors(d, nd, dst, dsizes, ns, src, ssizes, type);
    if (!g_views_on.load(std::memory_order_relaxed)) return;
    if ((type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) || !dst || !src) return;
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && !g_views_on.load(std::memory_order_relaxed)) return;
    const UINT step = d->GetDescriptorHandleIncrementSize(type);
    if (!step) return;
    // the ranges walked together, descriptor by descriptor (a null size array: ranges of one)
    UINT di = 0, si = 0, doff = 0, soff = 0;
    while (di < nd && si < ns) {
        const UINT dn = dsizes ? dsizes[di] : 1, sn = ssizes ? ssizes[si] : 1;
        const UINT n = std::min(dn - doff, sn - soff);
        if (type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV) copy_views(dst[di].ptr + SIZE_T(doff) * step, src[si].ptr + SIZE_T(soff) * step, n, step);
        else copy_srvs(dst[di].ptr + SIZE_T(doff) * step, src[si].ptr + SIZE_T(soff) * step, n, step);
        doff += n; soff += n;
        if (doff == dn) { ++di; doff = 0; }
        if (soff == sn) { ++si; soff = 0; }
    }
}

HRESULT STDMETHODCALLTYPE hk_reset(ID3D12GraphicsCommandList* cl, ID3D12CommandAllocator* a, ID3D12PipelineState* p) {
    const HRESULT hr = real_reset(cl, a, p);
    if (SUCCEEDED(hr)) nr::gpu::reset(cl);
    if (!t_own && trace().active()) {
        if (SUCCEEDED(hr)) list_of(cl)->trace_generation = trace().generation.fetch_add(1);
        trace_list("reset", cl, uint32_t(hr));
    }
    if (SUCCEEDED(hr) && g.lists_hooked && !t_own) {
        List* s = list_of(cl);
        s->reset();
        s->pso = p;
    }
    return hr;
}
void STDMETHODCALLTYPE hk_set_targets(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
                                      const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    if (on()) {
        List* s = list_of(cl);
        s->back = 0;
        s->rt = 0;
        s->dsv = dsv != nullptr;
        uint64_t first = 0;
        if (rtvs && n) {
            std::shared_lock<std::shared_mutex> lk(g.views_m);
            for (UINT i = 0; i < n; ++i) {
                const SIZE_T at = single ? rtvs[0].ptr + SIZE_T(i) * g.rtv_step : rtvs[i].ptr;
                const auto it = g.rtvs.find(at);
                trace_list("rt-bind", cl, i, at, it == g.rtvs.end() ? 0 : it->second, dsv ? dsv->ptr : 0);
                if (i == 0 && it != g.rtvs.end()) first = it->second;
                if (it != g.rtvs.end() && is_back(it->second)) { s->back = it->second; break; }
            }
        }
        if (!s->back && first && chain_candidate(reinterpret_cast<ID3D12Resource*>(first))) s->rt = first;
    }
    real_set_targets(cl, n, rtvs, single, dsv);
}
void STDMETHODCALLTYPE hk_viewports(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_VIEWPORT* v) {
    if (on() && n && v) list_of(cl)->vp = v[0];
    real_viewports(cl, n, v);
}
void STDMETHODCALLTYPE hk_scissors(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_RECT* r) {
    if (on() && n && r) list_of(cl)->scissor = r[0];
    real_scissors(cl, n, r);
}
void STDMETHODCALLTYPE hk_set_pipeline(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* p) {
    trace_list("pso", cl, reinterpret_cast<uint64_t>(p));
    if (on()) list_of(cl)->pso = p;
    real_set_pipeline(cl, p);
}
void STDMETHODCALLTYPE hk_draw(ID3D12GraphicsCommandList* cl, UINT v, UINT i, UINT fv, UINT fi) {
    trace_list("draw", cl, v, i, fv, fi);
    if (on()) {
        List* s = list_of(cl);
        if (s->back) {
            const uint8_t kind = draw_kind(*s, v, i, false);
            if (kind == kScene && !s->dsv && g.chain.load(std::memory_order_relaxed)) chain_inputs(cl, *s);
            write(cl, *s, s->back, kind, D3D12_RESOURCE_STATE_RENDER_TARGET);
        } else if (s->rt) chain_write(cl, *s, s->rt, draw_kind(*s, v, i, false), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    real_draw(cl, v, i, fv, fi);
}
void STDMETHODCALLTYPE hk_draw_indexed(ID3D12GraphicsCommandList* cl, UINT n, UINT i, UINT fi, INT vo, UINT fin) {
    trace_list("draw-indexed", cl, n, i, fi, uint32_t(vo));
    if (on()) {
        List* s = list_of(cl);
        if (s->back) {
            const uint8_t kind = draw_kind(*s, n, i, false);
            if (kind == kScene && !s->dsv && g.chain.load(std::memory_order_relaxed)) chain_inputs(cl, *s);
            write(cl, *s, s->back, kind, D3D12_RESOURCE_STATE_RENDER_TARGET);
        } else if (s->rt) chain_write(cl, *s, s->rt, draw_kind(*s, n, i, false), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    real_draw_indexed(cl, n, i, fi, vo, fin);
}
void STDMETHODCALLTYPE hk_execute_indirect(ID3D12GraphicsCommandList* cl, ID3D12CommandSignature* sig, UINT max, ID3D12Resource* args,
                                           UINT64 off, ID3D12Resource* count, UINT64 coff) {
    if (on()) {
        List* s = list_of(cl);
        const uint64_t target=s->back ? s->back : s->rt;
        if (target) {
            const auto kind=signature_kind(sig);
            const bool allow_uav=(reinterpret_cast<ID3D12Resource*>(target)->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)!=0;
            // A compute pass writes a texture only through a UAV view, in UNORDERED_ACCESS. The back buffer this list
            // itself moved to a state without it (and not back) is not written; otherwise whether it ever had a UAV view
            // is asked at the Present (a view may be made after the recording, before the submission).
            bool by_state=false;
            if (kind==nr::hudcap::IndirectKind::Dispatch && max && allow_uav && s->back==target && !g.enhanced_unhooked.load()) {
                const List::BackState* st=s->back_state(target,false);
                by_state=nr::hudcap::compute_cannot_write_by_state(st && st->valid && st->epoch==g.arm_epoch.load(), st ? st->state : 0);
            }
            if (by_state) {
                ++indirect_skipped; ++indirect_by_state;
                trace_list("indirect-no-target-write-state",cl,target,uint32_t(kind),max,allow_uav);
            } else if (kind==nr::hudcap::IndirectKind::Dispatch && max && allow_uav && s->back==target) {
                // (left as it is: the frame is judged at the Present; s->last unchanged, the dispatch taken as no write)
                trace_list("indirect-compute-view-pending",cl,target,uint32_t(kind),max,allow_uav);
                if (s->recs.size() >= nr::hudcap::kMaxRecs) { s->recs.clear(); s->recs.push_back({kLost,s->back}); }
                s->recs.push_back({kUnknown,s->back,0,kWhyComputeView});
            } else if (nr::hudcap::indirect_cannot_write(kind,max,allow_uav)) {
                ++indirect_skipped;
                trace_list("indirect-no-target-write",cl,target,uint32_t(kind),max,allow_uav);
            } else {
                const uint8_t why=kind==nr::hudcap::IndirectKind::Dispatch ? kWhyComputeUav :
                    kind==nr::hudcap::IndirectKind::Raster ? kWhyRaster : kWhySignature;
                trace_list("indirect-unknown-write",cl,target,why,max,allow_uav);
                if (s->back) {
                    if (s->recs.size() >= nr::hudcap::kMaxRecs) { s->recs.clear(); s->recs.push_back({kLost,s->back}); }
                    s->recs.push_back({kUnknown,s->back,0,why});
                    s->last=kOverlay;
                } else s->cand(s->rt,true)->last=kUnknown;
            }
        }
    }
    real_execute_indirect(cl, sig, max, args, off, count, coff);
}
void STDMETHODCALLTYPE hk_clear_target(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT* c, UINT n,
                                       const D3D12_RECT* r) {
    if (on()) {
        uint64_t res = 0;
        {
            std::shared_lock<std::shared_mutex> lk(g.views_m);
            const auto it = g.rtvs.find(rtv.ptr);
            if (it != g.rtvs.end()) res = it->second;
        }
        if (is_back(res)) write(cl, *list_of(cl), res, n ? kOverlay : kScene, D3D12_RESOURCE_STATE_RENDER_TARGET);
        else if (res && g.chain.load(std::memory_order_relaxed)) {
            List* s = list_of(cl);
            if (res == s->rt || s->cand(res, false) || chain_candidate(reinterpret_cast<ID3D12Resource*>(res)))
                chain_write(cl, *s, res, n ? kOverlay : kCleared, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }
    real_clear_target(cl, rtv, c, n, r);
}
void STDMETHODCALLTYPE hk_copy_region(ID3D12GraphicsCommandList* cl, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
                                      const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    if (on() && dst && is_back(reinterpret_cast<uint64_t>(dst->pResource))) {
        // the extent copied: the box, else the whole source (a texture's top level, or a buffer's footprint)
        uint64_t cw = 0, ch = 0;
        if (box) { cw = box->right - box->left; ch = box->bottom - box->top; }
        else if (src && src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
            cw = src->PlacedFootprint.Footprint.Width; ch = src->PlacedFootprint.Footprint.Height;
        } else if (src && src->pResource) {
            const D3D12_RESOURCE_DESC sd = src->pResource->GetDesc();
            cw = sd.Width; ch = sd.Height;
        }
        const bool whole = x == 0 && y == 0 && cw >= g.back_w.load() && ch >= g.back_h.load();
        write(cl, *list_of(cl), reinterpret_cast<uint64_t>(dst->pResource), whole ? kScene : kOverlay,
              D3D12_RESOURCE_STATE_COPY_DEST);
    } else if (on() && dst && dst->pResource && g.chain.load(std::memory_order_relaxed) &&
               dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && chain_candidate(dst->pResource)) {
        const bool whole = x == 0 && y == 0 && (!box || (box->right - box->left >= g.back_w.load() &&
                                                         box->bottom - box->top >= g.back_h.load()));
        chain_write(cl, *list_of(cl), reinterpret_cast<uint64_t>(dst->pResource), whole ? kScene : kOverlay,
                    D3D12_RESOURCE_STATE_COPY_DEST);
    }
    real_copy_region(cl, dst, x, y, z, src, box);
}
void STDMETHODCALLTYPE hk_copy_resource(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src) {
    trace_list("copy-resource", cl, reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src));
    if (on() && is_back(reinterpret_cast<uint64_t>(dst)))
        write(cl, *list_of(cl), reinterpret_cast<uint64_t>(dst), kScene, D3D12_RESOURCE_STATE_COPY_DEST);
    else if (on() && g.chain.load(std::memory_order_relaxed) && chain_candidate(dst))
        chain_write(cl, *list_of(cl), reinterpret_cast<uint64_t>(dst), kScene, D3D12_RESOURCE_STATE_COPY_DEST);
    real_copy_resource(cl, dst, src);
}
void STDMETHODCALLTYPE hk_resolve(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, UINT ds, ID3D12Resource* src, UINT ss,
                                  DXGI_FORMAT f) {
    if (on() && is_back(reinterpret_cast<uint64_t>(dst)))
        write(cl, *list_of(cl), reinterpret_cast<uint64_t>(dst), kScene, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    real_resolve(cl, dst, ds, src, ss, f);
}
void STDMETHODCALLTYPE hk_execute_bundle(ID3D12GraphicsCommandList* cl, ID3D12GraphicsCommandList* bundle) {
    // the bundle's draws see no back buffer of their own (its targets are the list's): a bound back buffer makes the
    // frame not trusted
    if (on()) {
        List* s = list_of(cl);
        if (s->back) { s->recs.push_back({kUnknown, s->back, 0, kWhyBundle}); s->last = kOverlay; }
    }
    real_execute_bundle(cl, bundle);
}
void STDMETHODCALLTYPE hk_begin_pass(ID3D12GraphicsCommandList* cl, UINT n, const RenderPassTarget* rts, const void* ds, UINT flags) {
    if (on()) {
        List* s = list_of(cl);
        s->back = 0;
        s->dsv = ds != nullptr;
        s->in_pass = true;
        if (n && rts) {
            uint64_t res = 0;
            {
                std::shared_lock<std::shared_mutex> lk(g.views_m);
                const auto it = g.rtvs.find(rts[0].cpu.ptr);
                if (it != g.rtvs.end()) res = it->second;
            }
            if (is_back(res)) {
                s->back = res;
                if (rts[0].begin_type == 2) write(cl, *s, res, kScene, D3D12_RESOURCE_STATE_RENDER_TARGET);   // a clear
            } else if (res && chain_candidate(reinterpret_cast<ID3D12Resource*>(res))) {
                s->rt = res;
                if (rts[0].begin_type == 2) s->cand(res, true)->last = kCleared;
            }
        }
    }
    real_begin_pass(cl, n, rts, ds, flags);
}
void STDMETHODCALLTYPE hk_end_pass(ID3D12GraphicsCommandList* cl) {
    if (on()) {
        List* s = list_of(cl);
        s->in_pass = false;
        s->back = 0;
        s->rt = 0;
    }
    real_end_pass(cl);
}
// The input side: the shader-visible heaps the game binds (their starts, learned once each), the graphics root
// signature's tables bound (a new root signature: none)
using PFN_SetHeaps = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using PFN_SetRootSig = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using PFN_SetTable = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
PFN_SetHeaps real_set_heaps;
PFN_SetRootSig real_set_root_sig;
PFN_SetTable real_set_table;
void STDMETHODCALLTYPE hk_set_heaps(ID3D12GraphicsCommandList* cl, UINT n, ID3D12DescriptorHeap* const* heaps) {
    if (on()) {
        uint64_t cpu=0,gpu=0,count=0;
        if (heaps) for (UINT i=0;i<n;++i) {
            auto* h=heaps[i]; if (!h) continue;
            const auto d=h->GetDesc();
            if (d.Type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !(d.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) continue;
            cpu=h->GetCPUDescriptorHandleForHeapStart().ptr; gpu=h->GetGPUDescriptorHandleForHeapStart().ptr;
            count=d.NumDescriptors;
        }
        List* s=list_of(cl);
        if (s->heap_cpu!=cpu || s->heap_gpu!=gpu || s->heap_count!=count) for (auto& t:s->tables) t=0;
        s->heap_cpu=cpu; s->heap_gpu=gpu; s->heap_count=count;
        trace_list("heap",cl,cpu,gpu,count,g.srv_step);
    }
    real_set_heaps(cl,n,heaps);
}
void STDMETHODCALLTYPE hk_set_root_sig(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* r) {
    trace_list("graphics-root", cl, reinterpret_cast<uint64_t>(r));
    if (on()) {
        List* s=list_of(cl);
        if (s->root!=r) {
            s->root=r; s->layout=nr::hudcap::root_layout(r);
            s->tables.assign(s->layout ? s->layout->tables.size() : 0,0);
            trace_list(s->layout ? "root-layout-bound" : "root-layout-unknown",cl,reinterpret_cast<uint64_t>(r),s->tables.size());
        }
    }
    real_set_root_sig(cl, r);
}
void STDMETHODCALLTYPE hk_set_table(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    trace_list("graphics-table", cl, index, h.ptr);
    if (on()) { List* s=list_of(cl); if (index<s->tables.size()) s->tables[index]=h.ptr; }
    real_set_table(cl, index, h);
}
using PFN_TraceDispatch = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using PFN_TraceRoot = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using PFN_TraceTable = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
PFN_TraceDispatch real_trace_dispatch;
PFN_TraceRoot real_trace_root;
PFN_TraceTable real_trace_table;
void STDMETHODCALLTYPE hk_trace_dispatch(ID3D12GraphicsCommandList* cl, UINT x, UINT y, UINT z) {
    trace_list("dispatch", cl, x,y,z); real_trace_dispatch(cl,x,y,z);
}
void STDMETHODCALLTYPE hk_trace_root(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* r) {
    trace_list("compute-root", cl, reinterpret_cast<uint64_t>(r)); real_trace_root(cl,r);
}
void STDMETHODCALLTYPE hk_trace_table(ID3D12GraphicsCommandList* cl, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    trace_list("compute-table", cl, i,h.ptr); real_trace_table(cl,i,h);
}
// The game's barriers: a candidate leaving a write state (render target, unordered access, copy or resolve destination)
// with a finished scene in it is copied first, in that state (the game names it). A candidate this list saw no write on
// that leaves unordered access was written by a compute pass: a scene write. (A split barrier's end: the image is in
// between, not copied.)
using PFN_Barrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
PFN_Barrier real_barrier;
void STDMETHODCALLTYPE hk_barrier(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    if (!t_own && trace().active() && b) for (UINT i=0;i<n;++i) {
        if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
            trace_list("transition", cl, reinterpret_cast<uint64_t>(b[i].Transition.pResource),
                b[i].Transition.StateBefore, b[i].Transition.StateAfter,
                (uint64_t(b[i].Flags)<<32) | b[i].Transition.Subresource);
        else if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
            trace_list("uav-barrier", cl, reinterpret_cast<uint64_t>(b[i].UAV.pResource));
        else if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING)
            trace_list("alias-barrier", cl, reinterpret_cast<uint64_t>(b[i].Aliasing.pResourceBefore),
                reinterpret_cast<uint64_t>(b[i].Aliasing.pResourceAfter));
    }
    if (on() && b) {   // the back buffers' states this list sets (Captured: indirect compute proofs)
        List* s = nullptr;
        for (UINT i = 0; i < n; ++i) {
            uint64_t image = 0;
            if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) image = reinterpret_cast<uint64_t>(b[i].Transition.pResource);
            else if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                const uint64_t a0 = reinterpret_cast<uint64_t>(b[i].Aliasing.pResourceBefore);
                const uint64_t a1 = reinterpret_cast<uint64_t>(b[i].Aliasing.pResourceAfter);
                image = is_back(a0) ? a0 : a1;
            }
            if (!is_back(image)) continue;
            if (!s) s = list_of(cl);
            List::BackState* st = s->back_state(image, true);
            st->epoch = g.arm_epoch.load();
            if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
                (b[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) ||
                (b[i].Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && b[i].Transition.Subresource != 0)) {
                st->valid = false;
                continue;
            }
            st->state = uint32_t(b[i].Transition.StateAfter);
            st->valid = true;
        }
    }
    if (on() && b && g.chain.load(std::memory_order_relaxed)) {
        List* s = list_of(cl);
        const uint32_t kWrites = uint32_t(D3D12_RESOURCE_STATE_RENDER_TARGET) | uint32_t(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
                                 uint32_t(D3D12_RESOURCE_STATE_COPY_DEST) | uint32_t(D3D12_RESOURCE_STATE_RESOLVE_DEST);
        for (UINT i = 0; i < n; ++i) {
            if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || (b[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY))
                continue;
            const D3D12_RESOURCE_STATES st = b[i].Transition.StateBefore;
            ID3D12Resource* r = b[i].Transition.pResource;
            if (!r) continue;
            const uint64_t image = reinterpret_cast<uint64_t>(r);
            if (!(uint32_t(st) & kWrites)) {   // (the input side: the state this list leaves a candidate in)
                List::Cand* c = s->cand(image, false);
                if (!c && (b[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) == 0 && chain_candidate(r)) c = s->cand(image, true);
                if (c) { c->state = uint32_t(b[i].Transition.StateAfter); c->state_known = (b[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) == 0; }
                continue;
            }
            List::Cand* c = s->cand(image, false);
            uint8_t last = c ? c->last : 0;
            if (!c) {
                if (!(st & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) || !chain_candidate(r)) continue;
                last = kScene;
            }
            if (c || last == kScene) mark_written(image);   // (a candidate: a write state left)
            if (last == kScene) chain_copy(cl, *s, image, st);
            if (!c) c = s->cand(image, true);
            c->last = 0;
            c->state = uint32_t(b[i].Transition.StateAfter);
            c->state_known = (b[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) == 0;
        }
    }
    real_barrier(cl, n, b);
}
// ID3D12GraphicsCommandList7::Barrier (enhanced barriers, slot 80): its layouts are not read; a list that calls it
// keeps no back buffer state of its own (nothing proven by state in it from then on).
using PFN_Barrier7 = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const void*);
PFN_Barrier7 real_barrier7;
void STDMETHODCALLTYPE hk_barrier7(ID3D12GraphicsCommandList* cl, UINT n, const void* groups) {
    if (on()) for (List::BackState& st : list_of(cl)->back_states) st.valid = false;
    real_barrier7(cl, n, groups);
}
void executed(UINT n, ID3D12CommandList* const* lists, ID3D12CommandQueue* queue) {
    if (!g.lists_hooked || t_own || !lists) return;
    std::lock_guard<std::mutex> lk(g.m);
    for (UINT i = 0; i < n; ++i) {
        const auto it = g.lists.find(static_cast<void*>(lists[i]));
        trace().add("submit-list", reinterpret_cast<uint64_t>(lists[i]),
            it == g.lists.end() ? 0 : it->second.trace_generation, reinterpret_cast<uint64_t>(queue), i, n);
        if (it == g.lists.end() || it->second.recs.empty()) continue;
        // its records stay until the list is reset: executed again, its copies run again
        if (g.frame.size() < 16384) g.frame.insert(g.frame.end(), it->second.recs.begin(), it->second.recs.end());
    }
}
// Only where NR Control's queue hook is not there (it calls d3d12_executed after the call, through the .asi)
void STDMETHODCALLTYPE hk_execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    executed(n, lists, q);
    real_execute(q, n, lists);
    if (!t_own && lists) {
        std::vector<nr::gpu::Ticket> tickets;
        for (UINT i=0;i<n;++i) if (auto t=nr::gpu::find(lists[i])) tickets.push_back(std::move(t));
        if (!tickets.empty()) {
            ID3D12Device* device=nullptr; ID3D12Fence* fence=nullptr;
            if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&device))) && device) {
                device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)); device->Release();
            }
            if (fence && SUCCEEDED(q->Signal(fence,1))) {
                const auto owner=std::shared_ptr<ID3D12Fence>(fence,[](auto* f){f->Release();});
                for (const auto& t:tickets) t->submitted([owner]{const auto v=owner->GetCompletedValue();return v!=UINT64_MAX && v>=1;});
            } else {
                if (fence) fence->Release();
                for (const auto& t:tickets) t->submitted({});
            }
        }
    }
}

// `object`'s vtable slot hooked with `detour` (once: a function hooked already is left as it is).
bool hook(void* object, int slot, void* detour, void** real, const char* name) {
    // Once per method as well. The bridge rewrites the command lists' vtables (nr_pe_d3d12split.cpp), so a later list
    // can hold another function in this slot; its calls still reach the one hooked first, and a second hook would
    // overwrite *real with a way back into the first - a loop that ends in a stack overflow.
    if (*real) return true;
    void* fn = (*reinterpret_cast<void***>(object))[slot];
    MH_STATUS st = MH_CreateHook(fn, detour, real);
    if (st == MH_OK) st = MH_EnableHook(fn);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        log("[nr] HUD FIX capture: %s could not be hooked (%s)", name, MH_StatusToString(st));
        return false;
    }
    return true;
}
template <class Fn>
bool hook_method(void* object, int slot, Fn detour, Fn* real, const char* name) {
    if (*real) return true;
    const MH_STATUS st = nr::hudcap::MethodHook<Fn>::install_raw_method(object, slot, detour, real);
    if (st != MH_OK) {
        log("[nr] HUD FIX capture: %s could not be hooked (%s)", name, MH_StatusToString(st));
        return false;
    }
    return true;
}
template <class F> void* as(F f) { return reinterpret_cast<void*>(f); }

using PFN_CreateCbv = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateUav = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);


void STDMETHODCALLTYPE hk_create_cbv(PFN_CreateCbv real_create_cbv, ID3D12Device* d,const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE h) {
    real_create_cbv(d,desc,h);
    if (g_views_on.load(std::memory_order_relaxed)) erase_srv(h.ptr);
}
void STDMETHODCALLTYPE hk_create_uav(PFN_CreateUav real_create_uav, ID3D12Device* d,ID3D12Resource* r,ID3D12Resource* counter,const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE h) {
    real_create_uav(d,r,counter,desc,h);
    if (!g_views_on.load(std::memory_order_relaxed)) return;
    if (r) g_uav_views.add(reinterpret_cast<uint64_t>(r));   // (a compute pass can write `r` only through such a view)
    erase_srv(h.ptr);
}
using PFN_CreateRoot = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
// Root signatures may precede the swap chain. Follow their creation without selecting a
// capture device (a probe/bridged device must not become the game's capture device).
PFN_CreateRoot real_create_root[8]{};
std::atomic<uint64_t> root_seen{0}, root_saved{0}, root_failed{0};
template<int Slot>
HRESULT STDMETHODCALLTYPE hk_create_root(ID3D12Device* d, UINT node, const void* bytes, SIZE_T size, REFIID iid, void** out) {
    const HRESULT hr=real_create_root[Slot](d,node,bytes,size,iid,out);
    if (SUCCEEDED(hr) && out && *out && !t_own) {
        ID3D12RootSignature* root=nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&root))) && root) {
            ++root_seen;
            const bool parsed=nr::hudcap::attach_root_layout(root,bytes,size);
            if (parsed) ++root_saved;
            else if (++root_failed <= 4)
                log("[nr] HUD FIX root metadata: parsing or storing a created root failed (%llu bytes)", (unsigned long long)size);
            trace().add(parsed ? "root-layout" : "root-layout-failed",0,0,reinterpret_cast<uint64_t>(root));
            root->Release();
        }
    }
    return hr;
}

bool hook_roots(ID3D12Device* device) {
    static std::mutex lock;
    static std::vector<void*> targets;
    std::lock_guard<std::mutex> guard(lock);
    void* fn=(*reinterpret_cast<void***>(device))[16];
    if (std::find(targets.begin(),targets.end(),fn)!=targets.end()) return true;
    const PFN_CreateRoot detours[]={hk_create_root<0>,hk_create_root<1>,hk_create_root<2>,hk_create_root<3>,
        hk_create_root<4>,hk_create_root<5>,hk_create_root<6>,hk_create_root<7>};
    if (targets.size()>=8) return false;
    const auto slot=targets.size();
    auto st=MH_CreateHook(fn,as(detours[slot]),reinterpret_cast<void**>(&real_create_root[slot]));
    if (st==MH_OK) {
        st=MH_EnableHook(fn);
        if (st!=MH_OK) MH_RemoveHook(fn);
    }
    if (st!=MH_OK) {
        log("[nr] HUD FIX root metadata: CreateRootSignature hook failed (%s)",MH_StatusToString(st));
        return false;
    }
    targets.push_back(fn);
    return true;
}

template<class Fn>
bool metadata_hook(void* object,int slot,typename nr::hudcap::MethodHook<Fn>::Handler handler,const char* name) {
    const auto status=nr::hudcap::MethodHook<Fn>::install(object,slot,handler);
    if(status!=MH_OK) log("[nr] HUD FIX metadata: %s hook failed (%s)",name,MH_StatusToString(status));
    return status==MH_OK;
}
bool hook_metadata(ID3D12Device* device);
using CreateDeviceFn=HRESULT(WINAPI*)(IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**);
CreateDeviceFn real_create_device[4]{};
template<int Slot>
HRESULT WINAPI hk_capture_device(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** out) {
    const HRESULT hr=real_create_device[Slot](adapter,level,iid,out);
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12Device* d=nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&d))) && d) {
            // (hooked before the device is handed to its creator: every UAV view it will make is seen)
            if (hook_metadata(d)) {
                std::lock_guard<std::mutex> lk(g_watched_m);
                if (std::find(g_watched_devices.begin(), g_watched_devices.end(), d) == g_watched_devices.end())
                    g_watched_devices.push_back(d);
            }
            log("[nr] HUD FIX metadata: roots, pipelines and views followed before the device's swap chain");
            d->Release();
        }
    }
    return hr;
}

// HUD FIX's view hooks on `device`'s implementation (once; true: all installed): CBV / UAV / SRV creation and the
// descriptor copies; the bookkeeping on from now.
bool hook_views(ID3D12Device* device) {
    std::lock_guard<std::mutex> lk(g_views_m);
    MH_Initialize();
    bool ok = metadata_hook<PFN_CreateCbv>(device, 17, &hk_create_cbv, "CreateConstantBufferView");
    ok = metadata_hook<PFN_CreateUav>(device, 19, &hk_create_uav, "CreateUnorderedAccessView") && ok;
    ok = metadata_hook<PFN_CreateSrv>(device, 18, &hk_create_srv, "CreateShaderResourceView") && ok;
    // the descriptor copies (render-target views and the views above): also only while HUD FIX wants the capture
    ok = metadata_hook<PFN_CopyDescriptors>(device, 23, &hk_copy_descriptors, "CopyDescriptors") && ok;
    ok = metadata_hook<PFN_CopyDescriptorsSimple>(device, 24, &hk_copy_descriptors_simple, "CopyDescriptorsSimple") && ok;
    g_views_hooked.store(true);
    g_views_on.store(true);
    return ok;
}
// (true: its UAV view creations are followed: HUD FIX wanted the capture as the device was made)
bool hook_metadata(ID3D12Device* device) {
    MH_Initialize();
    bool ok = hook_roots(device);
    ok = metadata_hook<PFN_CreateSignature>(device, 41, &hk_create_signature, "CreateCommandSignature") && ok;
    ok = metadata_hook<PFN_CreateGraphics>(device, 10, &hk_create_graphics, "CreateGraphicsPipelineState") && ok;
    // the views (CBV / SRV / UAV creation): only while HUD FIX wants the capture (hook_views)
    const int wanted = g_wanted.load() >= 0 ? g_wanted.load() : (g_wanted_query && g_wanted_query()) ? 1 : 0;
    const bool uav_ok = wanted == 1 && hook_views(device);
    ok = metadata_hook<PFN_CreateRtv>(device, 20, &hk_create_rtv, "CreateRenderTargetView") && ok;
    // ID3D12Device2's CreatePipelineState (slot 47) and a pipeline library's loads (ID3D12Device1::CreatePipelineLibrary,
    // slot 44: one made empty for its vtable; LoadGraphicsPipeline 9, ID3D12PipelineLibrary1::LoadPipeline 13)
    static const GUID kDevice2 = {0x30baa41e, 0xb15b, 0x475c, {0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28}};
    static const GUID kDevice1 = {0x77acce80, 0x638e, 0x4e65, {0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e}};
    static const GUID kLibrary = {0xc64226a8, 0x9201, 0x46af, {0xb4, 0xcc, 0x53, 0xfb, 0x9f, 0xf7, 0x41, 0x4f}};
    static const GUID kLibrary1 = {0x80eabf42, 0x2568, 0x4e5e, {0xbd, 0x82, 0xc3, 0x7f, 0x86, 0x96, 0x1d, 0xc3}};
    IUnknown* d2 = nullptr;
    if (SUCCEEDED(device->QueryInterface(kDevice2, reinterpret_cast<void**>(&d2))) && d2) {
        ok = metadata_hook<PFN_CreateStream>(d2, 47, &hk_create_stream, "CreatePipelineState") && ok;
        d2->Release();
    }
    IUnknown* d1 = nullptr;
    if (SUCCEEDED(device->QueryInterface(kDevice1, reinterpret_cast<void**>(&d1))) && d1) {
        using PFN_CreateLibrary = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, const void*, SIZE_T, REFIID, void**);
        auto create = reinterpret_cast<PFN_CreateLibrary>((*reinterpret_cast<void***>(d1))[44]);
        IUnknown* lib = nullptr;
        if (SUCCEEDED(create(d1, nullptr, 0, kLibrary, reinterpret_cast<void**>(&lib))) && lib) {
            ok = metadata_hook<PFN_LoadGraphics>(lib, 9, &hk_load_graphics, "LoadGraphicsPipeline") && ok;
            IUnknown* lib1 = nullptr;
            if (SUCCEEDED(lib->QueryInterface(kLibrary1, reinterpret_cast<void**>(&lib1))) && lib1) {
                ok = metadata_hook<PFN_LoadStream>(lib1, 13, &hk_load_stream, "LoadPipeline") && ok;
                lib1->Release();
            }
            lib->Release();
        }
        d1->Release();
    }
    log("[nr] HUD FIX capture: the device's render-target views and pipelines are followed%s", ok ? "" : " (in part: above)");
    return uav_ok;
}

void hook_device(ID3D12Device* device) {
    if (g.device_hooked) return;
    g.device_hooked=true;
    g.rtv_step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g.srv_step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    hook_metadata(device);
}

bool hook_lists(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (g.lists_hooked) return true;
    static bool tried = false;   // once: a list it could not hook stays so (said once)
    if (tried) return false;
    tried = true;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* l = nullptr;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) || !alloc) return false;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&l))) || !l) {
        alloc->Release();
        return false;
    }
    bool ok = hook_method(l, 10, &hk_reset, &real_reset, "Reset") &&
              hook_method(l, 12, &hk_draw, &real_draw, "DrawInstanced") &&
              hook_method(l, 13, &hk_draw_indexed, &real_draw_indexed, "DrawIndexedInstanced") &&
              hook_method(l, 16, &hk_copy_region, &real_copy_region, "CopyTextureRegion") &&
              hook_method(l, 17, &hk_copy_resource, &real_copy_resource, "CopyResource") &&
              hook_method(l, 19, &hk_resolve, &real_resolve, "ResolveSubresource") &&
              hook_method(l, 21, &hk_viewports, &real_viewports, "RSSetViewports") &&
              hook_method(l, 22, &hk_scissors, &real_scissors, "RSSetScissorRects") &&
              hook_method(l, 25, &hk_set_pipeline, &real_set_pipeline, "SetPipelineState") &&
              hook_method(l, 26, &hk_barrier, &real_barrier, "ResourceBarrier") &&
              hook_method(l, 28, &hk_set_heaps, &real_set_heaps, "SetDescriptorHeaps") &&
              hook_method(l, 30, &hk_set_root_sig, &real_set_root_sig, "SetGraphicsRootSignature") &&
              hook_method(l, 32, &hk_set_table, &real_set_table, "SetGraphicsRootDescriptorTable") &&
              hook_method(l, 27, &hk_execute_bundle, &real_execute_bundle, "ExecuteBundle") &&
              hook_method(l, 46, &hk_set_targets, &real_set_targets, "OMSetRenderTargets") &&
              hook_method(l, 48, &hk_clear_target, &real_clear_target, "ClearRenderTargetView") &&
              hook_method(l, 59, &hk_execute_indirect, &real_execute_indirect, "ExecuteIndirect");
    if (trace().enabled) {
        const bool diagnostic_hooks = hook_method(l, 14, &hk_trace_dispatch, &real_trace_dispatch, "HUD trace Dispatch") &&
            hook_method(l, 29, &hk_trace_root, &real_trace_root, "HUD trace ComputeRoot") &&
            hook_method(l, 31, &hk_trace_table, &real_trace_table, "HUD trace ComputeTable");
        if (!diagnostic_hooks) log("[nr] HUD trace: compute hooks incomplete");
    }
    static const GUID kList4 = {0x8754318e, 0xd3a9, 0x4541, {0x98, 0xcf, 0x64, 0x5b, 0x50, 0xdc, 0x48, 0x74}};
    IUnknown* l4 = nullptr;
    if (SUCCEEDED(l->QueryInterface(kList4, reinterpret_cast<void**>(&l4))) && l4) {
        ok = hook_method(l4, 68, &hk_begin_pass, &real_begin_pass, "BeginRenderPass") &&
             hook_method(l4, 69, &hk_end_pass, &real_end_pass, "EndRenderPass") && ok;
        l4->Release();
    }
    {   // (a device without enhanced barriers: no list can call them)
        static const GUID kList7 = {0xdd171223, 0x8b61, 0x4769, {0x90, 0xe3, 0x16, 0x0c, 0xcd, 0xe4, 0xe2, 0xc1}};
        IUnknown* l7 = nullptr;
        if (SUCCEEDED(l->QueryInterface(kList7, reinterpret_cast<void**>(&l7))) && l7) {
            if (!hook_method(l7, 80, &hk_barrier7, &real_barrier7, "Barrier (enhanced)")) {
                g.enhanced_unhooked.store(true);
                log("[nr] HUD FIX capture: enhanced barriers are not followed: no back buffer state is proven from a list");
            }
            l7->Release();
        }
    }
    if (queue) {
        if (!nr::pe::menu::queue_hooked())
            ok = hook_method(queue, 10, &hk_execute, &real_execute, "ExecuteCommandLists") && ok;
    } else {
        ok = false;
        log("[nr] HUD FIX capture: the swap chain's queue was not found");
    }
    l->Release();
    alloc->Release();
    g.lists_hooked = ok;
    log("[nr] HUD FIX capture: the game's command lists are followed%s", ok ? "" : " - NOT: it does not run");
    return ok;
}

bool ensure_hudless(ID3D12Device* device, DXGI_FORMAT format, uint32_t w, uint32_t h) {
    ID3D12Resource* cur = g.hudless.load();
    if (cur) {
        const D3D12_RESOURCE_DESC d = cur->GetDesc();
        ID3D12Device* of = nullptr;
        const bool same = SUCCEEDED(cur->GetDevice(IID_PPV_ARGS(&of))) && of == device;
        if (of) of->Release();
        if (same && d.Width == w && d.Height == h && d.Format == format) return true;
        g.hudless.store(nullptr, std::memory_order_release);
        std::lock_guard<std::mutex> lk(g.m);
        g.retired.push_back({cur, g.presents});
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = format; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&r))) || !r) {
        log("[nr] HUD FIX capture: its image (format %u, %ux%u) could not be made", unsigned(format), w, h);
        return false;
    }
    g.hudless.store(r, std::memory_order_release);
    return true;
}


// ---- frame generation's HUD-less frame (D3D12) ----------------------------------------------------------------------
// The FidelityFX API's frame-generation configure (ffx_framegeneration.h, ffx_api.h, ffx_api_types.h: FidelityFX SDK,
// MIT), XeSS-FG's resource tag (xefg_swapchain_d3d12.h, Intel, MIT) and Streamline's tags (sl_core_types.h, NVIDIA,
// MIT): only the members read, in their layout.
struct FfxHeader { uint64_t type; FfxHeader* next; };
struct FfxResource {
    void* resource;
    uint32_t type, format, width, height, depth, mips, flags, usage;
    uint32_t state;
};
struct FfxConfigureFg {   // ffxConfigureDescFrameGeneration
    FfxHeader header;
    void* swap_chain;
    void* present_callback;
    void* present_context;
    void* fg_callback;
    void* fg_context;
    bool enabled, async;
    FfxResource hudless;
    uint32_t flags;
    bool only_generated;
    int32_t rect[4];
    uint64_t frame_id;
};
constexpr uint64_t kFfxConfigureFg = 0x00020002u;   // FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION
struct FfxDistortion { FfxHeader header; FfxResource field; };
constexpr uint64_t kFfxDistortion = 0x00020008u;    // ..._FRAMEGENERATION_REGISTERDISTORTIONRESOURCE (chained to it)
// ffxDispatchDescFrameGenerationPrepare (deprecated, still what OptiScaler and many games call) and its V2 (the camera
// inline), ffx_framegeneration.h; the camera of the first one in a chained ffxDispatchDescFrameGenerationPrepareCameraInfo
struct FfxPrepare {
    FfxHeader header;
    uint64_t frame_id;
    uint32_t flags;
    void* command_list;
    uint32_t render[2];
    float jitter[2], mv_scale[2];
    float dt;
    bool reset;            // V1: unused_reset
    float near_, far_, fov, meters;
    FfxResource depth, motion;
};
struct FfxPrepareV2 { FfxPrepare base; float camera[12]; };
struct FfxCameraInfo { FfxHeader header; float camera[12]; };
constexpr uint64_t kFfxPrepare = 0x00020004u, kFfxCameraInfo = 0x0002000au, kFfxPrepareV2 = 0x0002000cu;
struct FfxUiResource { FfxHeader header; FfxResource ui; uint32_t flags; };   // ...SwapChainRegisterUiResourceDX12
constexpr uint64_t kFfxUiResource = 0x00030002u;
static_assert(sizeof(FfxResource) == 48 && sizeof(FfxConfigureFg) == 144 && sizeof(FfxPrepare) == 184 &&
              sizeof(FfxPrepareV2) == 232 && sizeof(FfxCameraInfo) == 64 && sizeof(FfxUiResource) == 72,
              "FidelityFX API layouts (ffx_framegeneration.h, ffx_api_framegeneration_dx12.h)");
struct XefgResource {      // xefg_swapchain_d3d12_resource_data_t
    uint32_t type, validity;   // type 0 HUDLESS_COLOR; validity 0 UNTIL_NEXT_PRESENT, 1 ONLY_NOW
    uint32_t base[2], size[2];
    ID3D12Resource* resource;
    D3D12_RESOURCE_STATES state;
};
struct SlBase { void* next; uint8_t type[16]; size_t version; };
struct SlResource { SlBase base; char type; void* native; void* memory; void* view; uint32_t state; };
struct SlTag { SlBase base; SlResource* resource; uint32_t type; int lifecycle; uint32_t extent[4]; };   // lifecycle 0 only now

D3D12_RESOURCE_STATES d3d12_state_of_ffx(uint32_t s) {
    D3D12_RESOURCE_STATES r = D3D12_RESOURCE_STATE_COMMON;
    if (s & (1u << 1)) r |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (s & (1u << 2)) r |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (s & (1u << 3)) r |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (s & (1u << 4)) r |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (s & (1u << 5)) r |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (s & (1u << 6)) r |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if (s & (1u << 8)) r |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    return r;   // PRESENT and COMMON: 0
}
// Formats CopyResource takes as a pair: the same typeless group.
int format_group(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 1;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 2;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return 3;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return 4;
    case DXGI_FORMAT_R11G11B10_FLOAT: return 5;
    default: return 100 + int(f);
    }
}

bool (*g_game_caller)(void*) = nullptr;   // set_game_caller
struct FgState {
    std::atomic<bool> armed{false};
    std::atomic<bool> list_copies{false};   // copies on the hand-over's command list (HUD FIX takes it)
    std::atomic<int> in_present{0};
    std::mutex m;
    ID3D12Resource* copy = nullptr;   // ours
    uint32_t w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // the last hand-over
    uint64_t at = ~0ull;              // g.presents then (this frame's: the presents so far)
    ID3D12Resource* destination = nullptr;
    D3D12_RESOURCE_STATES destination_state = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* game = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool copied = false, until_present = false, substitute = false, hudless = true;
    const char* source = "";
    // XeSS pairing: a local Present epoch alone is not a provider frame identity.
    void* xess_context = nullptr;
    uint32_t xess_frame = 0;
    bool xess_full_frame = false;
    struct Ui {
        void* context = nullptr; uint32_t frame = 0; uint64_t at = ~0ull;
        ID3D12Resource* image = nullptr;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        std::shared_ptr<ChainSnapshot> keep;
    } ui;
    // AddRef'd game resources live until both the CPU reader and its GPU recording finish.
    // A bounded pool; no age-based retirement and no copies on the game's lists for UI.
    std::vector<std::shared_ptr<ChainSnapshot>> ui_leases;
    std::string refused;              // the last one not taken: why
    // a hand-over seen while not armed (HUD FIX in another mode): its time (GetTickCount64), so NR Control can tell
    // whether the method could run before it is picked
    std::atomic<uint64_t> offered_ms{0};
    HMODULE hooked_ffx[6] = {};    // the FidelityFX modules followed (one detour slot each: a game's copy and OptiScaler's)
    bool hooked_sl = false;
    HMODULE hooked_xefg[4] = {};   // the libxess_fg.dll modules whose tags are followed (one detour slot each)
    uint64_t looked = 0;
    std::atomic<uint64_t> dlssg_ms{0};   // the last DLSS-G evaluate (through NGX) the Present layers ran at
} gf;

// A hand-over of `r` (in `state`; `list`: the game's command list it was made on, null none) by `source`.
void fg_note(void* caller, ID3D12Resource* r, D3D12_RESOURCE_STATES state, ID3D12GraphicsCommandList* list, bool until_present,
             bool substitute, const char* source, bool hudless = true,
             void* xess_context = nullptr, uint32_t xess_frame = 0, bool xess_full_frame = false,
             ID3D12Resource* destination = nullptr, D3D12_RESOURCE_STATES dest_state = D3D12_RESOURCE_STATE_COMMON) {
    if (!r || t_own) return;
    // While the Present layers run at DLSS-G's evaluate (DLSS-G through NGX) the HUD-less frame frame generation reads
    // is the one that evaluate hands over (Streamline's copy, fg_note_dlssg): the game's own hand-overs (its Streamline
    // tag) are not taken. Made on the game's threads, one could land between that evaluate's hand-over and the layers,
    // which then wrote NR's change into the game's next HUD-less frame instead: one generated frame without NR, the
    // next with it twice.
    if (caller) {
        const uint64_t d = gf.dlssg_ms.load();
        if (d && GetTickCount64() - d < 1000) return;
    }
    // OptiScaler's own frame generation's (its lists, its threads): not taken (caller null: NGX's DLSS-G evaluate, the game's)
    if (caller && g_game_caller && !g_game_caller(caller)) {
        static std::atomic<int> told{0};
        if (told.fetch_add(1) < 2)
            log("[nr] HUD FIX (frame generation): %s's HUD-less frame handed over by OptiScaler's own frame generation (or "
                "another library) is not taken; only the game's own hand-overs are", source);
        return;
    }
    if (!gf.armed.load(std::memory_order_relaxed)) {   // only seen (not during the Present: OptiScaler's own)
        if (!t_in_present && hudless) gf.offered_ms.store(GetTickCount64());   // (HUD FIX's method: HUD-less ones)
        return;
    }
    std::lock_guard<std::mutex> lk(gf.m);
    // The declared HUD-less source and the interpolation's presentColor can be different textures (e.g.
    // after a menu changes the render path). A same-frame unlabelled presentColor must not downgrade it.
    if (!hudless && gf.at == g.presents && (gf.hudless || gf.game == r)) {
        trace().add("fg-keep-hudless",0,0,reinterpret_cast<uint64_t>(gf.game),reinterpret_cast<uint64_t>(r));
        return;
    }
    // Inside a Present (on this thread) the hand-overs are OptiScaler's own frame generation's - but not DLSS-G's own
    // (caller null: its evaluate through NGX). This thread's only: a game whose render thread hands its frame generation
    // the HUD-less frame while its presenting thread sits in a Present (RE Engine; Streamline's presenting thread most of
    // the time) had it refused on every frame the two overlapped - taken, refused, taken, by the timing.
    if (caller && t_in_present) { gf.refused = std::string(source) + "'s is handed over during the Present (OptiScaler's own frame generation): the capture serves it"; return; }
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    ID3D12Device* rd = nullptr, *cd = nullptr;
    if (gf.copy) { r->GetDevice(IID_PPV_ARGS(&rd)); gf.copy->GetDevice(IID_PPV_ARGS(&cd)); }
    const bool same_device = rd && rd == cd;
    if (rd) rd->Release();
    if (cd) cd->Release();
    if (!same_device) { gf.refused = std::string(source) + "'s is of another device"; return; }
    if (d.Width != gf.w || d.Height != gf.h || format_group(d.Format) != format_group(gf.format) || !gf.copy) {
        gf.refused = std::string(source) + "'s is not of the back buffer's size and format";
        return;
    }
    trace().add(hudless ? "fg-hudless" : "fg-present-color",0,0,reinterpret_cast<uint64_t>(r),g.presents);
    gf.at = g.presents;
    gf.xess_context = xess_context; gf.xess_frame = xess_frame; gf.xess_full_frame = xess_full_frame;
    gf.destination = destination ? destination : r;
    gf.destination_state = destination ? dest_state : state;
    gf.game = r;
    gf.state = state;
    gf.source = source;
    gf.until_present = until_present;
    gf.substitute = substitute && until_present;
    gf.hudless = hudless;
    gf.copied = false;
    gf.refused.clear();
    if (list && reinterpret_cast<uintptr_t>(list) > 0xFFFF && gf.list_copies.load()) {   // copied now, on the game's list (XeSS-FG's fake list: 1)
        ++t_own;
        transition(list, r, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(list, gf.copy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(gf.copy, r);
        transition(list, gf.copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(list, r, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        --t_own;
        gf.copied = true;
    } else if (!until_present) {
        gf.at = ~0ull;
        gf.refused = std::string(source) + "'s is valid only when handed over, without a command list to copy it on";
    }
}

using PFN_FfxConfigure = int32_t (*)(void*, const FfxHeader*);
// Slots 0..5: the FidelityFX modules' exports; 6..11: OptiScaler's way past its own hook of the same export (ffx_entry)
constexpr int kFfxSlots = 6;
PFN_FfxConfigure real_ffx_configure[2 * kFfxSlots];
int g_ffx_slot_name[2 * kFfxSlots];   // kFfxLibrary's index of the module in that slot
// The export of slot I is OptiScaler's hook's too, its way past it followed (slot I + kFfxSlots): every call reaching the
// library passes there - the even frames' hand-over is made there only (never twice)
std::atomic<bool> g_ffx_inner[kFfxSlots];
template <int I> bool ffx_delays() { return I >= kFfxSlots || !g_ffx_inner[I % kFfxSlots].load(std::memory_order_relaxed); }
// The caller of the export a call past OptiScaler's hook came in at (this thread, the call in progress): the game's call
// OptiScaler hands on is the game's there too (its HUD-less frame taken, the game the driver); OptiScaler's own calls have
// none (OptiScaler is their caller).
thread_local void* t_ffx_outer = nullptr;
template <int I> void* ffx_caller(void* here) { return I >= kFfxSlots && t_ffx_outer ? t_ffx_outer : here; }
template <int I> struct FfxOuter {
    void* was;
    explicit FfxOuter(void* caller) : was(t_ffx_outer) { if (I < kFfxSlots && g_ffx_inner[I].load(std::memory_order_relaxed)) t_ffx_outer = caller; }
    ~FfxOuter() { t_ffx_outer = was; }
};
// The caller's module, for the frame-generation lines below.
std::string module_of(void* address) {
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &m) || !m || !GetModuleFileNameW(m, path, MAX_PATH))
        return "?";
    const wchar_t* base = std::wcsrchr(path, L'\\');
    return nr::u8::narrow(base ? base + 1 : path);
}
const char* const kFfxLibrary[3] = {"amd_fidelityfx_dx12.dll", "amd_fidelityfx_loader_dx12.dll",
                                    "amd_fidelityfx_framegeneration_dx12.dll"};
// The FidelityFX swapchain submits its public interpolation list at Present.
 // A dispatch on that list reads its presentColor after the Present layers have
 // supplied the shown frame. Other, application-submitted lists have different ordering.
using PFN_FfxQuery=int32_t (*)(void*,FfxHeader*);
PFN_FfxQuery real_ffx_query[kFfxSlots]{};
std::mutex interpolation_mutex;
std::unordered_map<void*,uint64_t> interpolation_lists;
std::atomic<uint64_t> interpolation_frame{0};
template<int I> int32_t ffx_query(void* context,FfxHeader* desc) {
    const int32_t rc=real_ffx_query[I](context,desc);
    if (!rc && desc && desc->type==0x00030003u) {
        struct Query { FfxHeader header; void** result; };
        void** out=reinterpret_cast<Query*>(desc)->result;
        if(out && *out) {
            std::lock_guard<std::mutex> guard(interpolation_mutex);
            const uint64_t tick=interpolation_frame.load(std::memory_order_relaxed);
            for(auto it=interpolation_lists.begin();it!=interpolation_lists.end();)
                if(it->second+2<tick)it=interpolation_lists.erase(it);else ++it;
            interpolation_lists[*out]=tick;
        }
    }
    return rc;
}
std::atomic<uint64_t> app_colour_at{0};
// The game's last frame-generation configure gave no frame-generation callback: it dispatches frame generation itself
// (ffx_dispatch takes its presentColor then; through a callback the library dispatches inside its own Present).
std::atomic<bool> g_ffx_app_dispatch{false};

// ---- NRFrameReuse's even frames with frame generation: whose calls carry a frame's inputs ----------------------------
// While the even frames run every frame is shown `lag` frames late, and frame generation must be handed the inputs of the
// frame it is shown with (xd below: XeSS frame generation's tags and constants; fd: FSR frame generation's configure and
// prepare through the FidelityFX API). Those are the calls of whoever drives frame generation: OptiScaler's own frame
// generation while it calls a library (it also takes a game's own calls over and makes them again itself - so they are
// delayed once, at OptiScaler's), else the game. A library's calls into another (FidelityFX's loader into its
// frame-generation DLL) are not delayed again: the outer call was. Frame generation driven by anything else (a DLSS-G
// to FSR library calling FidelityFX itself, FSR 3.0 linked into the game) is not followed: the even frames do not run
// then (fg_followed).
bool (*g_opti_caller)(void*) = nullptr;   // set_opti_caller
namespace fgd {
std::atomic<uint64_t> opti_ms{0};         // last evidence that OptiScaler drives FG: a library call or its emulated input context
std::atomic<uint64_t> game_ms{0};         // the game's last such call, wherever it went (GetTickCount64)
std::atomic<bool> capture{false};
std::atomic<uint32_t> lag{0};             // frames late while the even frames run, else 0
std::atomic<uint64_t> frames{0};          // the Present layers' recordings (fg_delay_present): a frame count
// the driver's last calls carrying a frame's inputs, by frames: XeSS's constants, FSR's prepare dispatch
std::atomic<uint64_t> xefg_at{0}, ffx_at{0};
std::atomic<const char*> unfollowed_reason{nullptr};
std::atomic<uint64_t> unfollowed_at{0};   // a call it cannot follow, by frames (why: `why`, said once each)
// the driver's last call into a frame-generation library (GetTickCount64): those calls carry its inputs and are delayed
// there - a DLSS-G evaluate (dd below) is not delayed again then
std::atomic<uint64_t> library_ms{0};
std::mutex why_m;
std::unordered_set<std::string> said;
// A call by the driver whose inputs are delayed.
// Inputs supplied before the next Present use the stage's committed pending
// output. An output provider called inside this Present reads its actual age.
uint32_t input_lag() {
    if (!capture.load(std::memory_order_relaxed))return 0;
    return uint32_t(t_in_present?nr::pe::Session::frames_delay():nr::pe::Session::next_frames_delay());
}
bool driver(void* caller, void* xefg_context = nullptr) {
    const uint64_t now = GetTickCount64();
    const bool output = g_opti_caller && g_opti_caller(caller);
    if (output) {
        opti_ms.store(now,std::memory_order_relaxed);
        const auto input = game_ms.load(std::memory_order_relaxed);
        if (input && now-input<2000) return false;
    } else {
        if (!g_game_caller || !g_game_caller(caller)) return false;
        game_ms.store(now,std::memory_order_relaxed);
    }
    library_ms.store(now,std::memory_order_relaxed);
    return true;
}
// Follow the game's public API at its entry, before OptiScaler can consume
// the call. A forwarded call through its inner thunk is the same input.
// With only upscaler input, the provider's actual library entry owns delay.
template<int I> bool entry(void* caller) {
    if (g_game_caller && g_game_caller(caller))
        return I<kFfxSlots || !t_ffx_outer;
    return ffx_delays<I>();
}
uint64_t frame_now() { return frames.load(std::memory_order_relaxed) + (t_in_present ? 0u : 1u); }   // (its Present's)
void unfollowed(const char* why) {
    unfollowed_reason.store(why,std::memory_order_relaxed);
    unfollowed_at.store(std::max<uint64_t>(1, frame_now()), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(why_m);
    if (said.insert(why).second)
        log("[nr] frame reuse's even frames cannot follow frame generation: %s "
            "(said once)", why);
}
// The ring's copy of `like` (same description: CopyResource takes the pair), resting in `state`.
bool ring_resource(ID3D12Resource*& res, D3D12_RESOURCE_STATES& rest, ID3D12Resource* like, D3D12_RESOURCE_STATES state,
                   const char* what) {
    const D3D12_RESOURCE_DESC want = like->GetDesc();
    if (res) {
        const D3D12_RESOURCE_DESC have = res->GetDesc();
        const bool same = have.Dimension == want.Dimension && have.Width == want.Width && have.Height == want.Height &&
                          have.DepthOrArraySize == want.DepthOrArraySize && have.MipLevels == want.MipLevels &&
                          have.Format == want.Format && have.SampleDesc.Count == want.SampleDesc.Count &&
                          have.SampleDesc.Quality == want.SampleDesc.Quality && have.Layout == want.Layout &&
                          have.Flags == want.Flags;
        if (same && rest == state) return true;
        res->Release();
        res = nullptr;
    }
    ID3D12Device* d = nullptr;
    if (FAILED(like->GetDevice(IID_PPV_ARGS(&d))) || !d) return false;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = want;
    const HRESULT hr = d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res));
    d->Release();
    if (FAILED(hr) || !res) {
        res = nullptr;
        static std::atomic<int> told{0};
        if (told.fetch_add(1) < 2)
            log("[nr] even frames with frame generation: a copy of its %s input could not be made (0x%08x)", what,
                unsigned(hr));
        return false;
    }
    rest = state;
    return true;
}
void copy_into(ID3D12GraphicsCommandList* l, ID3D12Resource* from, D3D12_RESOURCE_STATES from_state, ID3D12Resource* to,
               D3D12_RESOURCE_STATES to_state) {
    ++t_own;
    transition(l, from, from_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(l, to, to_state, D3D12_RESOURCE_STATE_COPY_DEST);
    l->CopyResource(to, from);
    transition(l, to, D3D12_RESOURCE_STATE_COPY_DEST, to_state);
    transition(l, from, D3D12_RESOURCE_STATE_COPY_SOURCE, from_state);
    --t_own;
}
}  // namespace fgd

// FSR frame generation (FidelityFX API) while the even frames run: four frames of its depth, motion vectors (copied on the
// prepare dispatch's command list), HUD-less frame and UI (valid until the present: copied at the Present layers'
// recording, or - handed over during the Present, by OptiScaler - on the prepare dispatch's list that follows), and the
// prepare's numbers; frame `id` is handed the copies and numbers of frame `id - lag` (frame IDs: the prepare and the
// configure carry the same one). A frame whose frame before has none is handed over as given (V2: its reset set).
namespace fd {
constexpr uint32_t kRing = 4;
enum { kDepth, kMotion, kHudless, kUi, kDistortion, kTypes };
const char* const kName[kTypes] = {"depth", "motion vector", "HUD-less", "UI", "distortion"};
struct Slot {
    ID3D12Resource* res = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;   // where it rests (the state the frame's own was handed in)
    FfxResource description{};
    uint32_t ffx_state = 0;                                      // that state as the caller wrote it
    uint64_t id = 0;
    bool valid = false;
};
struct Numbers {
    uint64_t id = 0;
    bool valid = false;
    uint32_t flags=0;bool reset=false, has_camera=false;
    uint32_t render[2] = {};
    float jitter[2] = {}, mv_scale[2] = {}, dt = 0, near_ = 0, far_ = 0, fov = 0, meters = 0, camera[12] = {};
};
struct Pending { ID3D12Resource* game; D3D12_RESOURCE_STATES state; uint32_t type, ring; };
struct Ctx {
    Slot slots[kTypes][kRing];
    Numbers numbers[kRing];
    std::vector<Pending> pending;   // valid until the present: copied at the Present layers' recording or the next prepare
};
std::mutex m;
std::unordered_map<void*, Ctx> ctxs;   // by the context's handle (the UI's: the swap chain context's)
bool hooked = false;
uint32_t off_frames = 0;
void release(Ctx& c) {
    for (auto& t : c.slots) for (auto& sl : t) if (sl.res) { sl.res->Release(); sl.res = nullptr; sl.valid = false; }
    for (auto& p : c.pending) if (p.game) p.game->Release();
    c.pending.clear();
}
void flush(Ctx& c, ID3D12GraphicsCommandList* l) {
    for (const auto& p : c.pending) {
        const Slot& to = c.slots[p.type][p.ring];
        if (l && to.res) fgd::copy_into(l, p.game, p.state, to.res, to.state);
        p.game->Release();
    }
    c.pending.clear();
}
// Frame `id`'s resource of `type` into the ring (now on `list`, else pending) and, when the ring has frame `id - lag`'s,
// `r` handed that one instead. Returns whether it was.
bool resource(Ctx& c, uint32_t type, uint64_t id, FfxResource& r, ID3D12GraphicsCommandList* list) {
    if (!r.resource) return false;
    auto* game = static_cast<ID3D12Resource*>(r.resource);
    const D3D12_RESOURCE_STATES state = d3d12_state_of_ffx(r.state);
    Slot& now = c.slots[type][id % kRing];
    if (fgd::ring_resource(now.res, now.state, game, state, kName[type])) {
        if (list) fgd::copy_into(list, game, state, now.res, now.state);
        else { game->AddRef(); c.pending.push_back({game, state, type, uint32_t(id % kRing)}); }
        now.id = id; now.valid = true; now.ffx_state = r.state; now.description=r;
    } else {
        now.valid = false;
    }
    const uint32_t k = fgd::input_lag();
    const Slot& before = c.slots[type][(id + kRing - k) % kRing];
    if (!k || !before.res || !before.valid || before.id + k != id) return false;
    r=before.description;
    r.resource = before.res;
    r.state = before.ffx_state;
    static std::atomic<uint32_t> told{0};
    if (!(told.fetch_or(1u << type) & (1u << type)))
        log("[nr] even frames with FSR frame generation: its %s input is handed over %u frame(s) later (frame %llu: the "
            "copy of %llu, %s)", kName[type], k, (unsigned long long)id, (unsigned long long)before.id,
            list ? "copied on the prepare's command list" : "copied before the next Present's layers");
    return true;
}
}  // namespace fd

// DLSS-G through NGX while the even frames run (nr_asi_main.cpp hk_evaluate, the Present layers at its evaluate): a
// provider behind OptiScaler's NGX export that calls no frame-generation library - Nukem's dlssg-to-fsr3 (OptiScaler's
// default replacement: FSR 3 inside that DLL), Artur's DLSS Enabler, a custom one - is handed the inputs of the frame shown:
// four frames of the evaluate's depth, motion vectors and HUD-less frame (Streamline's copies, in COPY_DEST at the evaluate:
// the state OptiScaler's own replacement and Nukem's take them in), copied on the evaluate's command list; frame `id - lag`'s
// handed over (its numbers: nr_asi_main.cpp). OptiScaler's own replacement (FFX) calls FidelityFX: delayed there (fd).
namespace dd {
constexpr uint32_t kRing = 4;
struct Slot {
    ID3D12Resource* res = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    uint64_t id = 0;
    bool valid = false;
};
std::mutex m;
Slot slots[kDlssgInputs][kRing];
uint32_t off_frames = 0;
bool any = false;
std::atomic<uint64_t> seen_ms{0};   // the last evaluate whose inputs are delayed here (fg_followed)
const char* const kName[kDlssgInputs] = {"depth", "motion vector", "HUD-less", "distortion"};
// Copies no longer used go after 8 more Presents and 2 s (an evaluate's list may not have run yet when the even frames
// stop or the size changes: the ring is the provider's input on it)
struct Retired { ID3D12Resource* res; uint32_t presents; ULONGLONG ms; };
std::vector<Retired> retired;
void retire(ID3D12Resource*& r) {
    if (r) retired.push_back({r, 8, GetTickCount64() + 2000});
    r = nullptr;
}
void tick() {   // (dd::m held; once a Present)
    const ULONGLONG now = GetTickCount64();
    retired.erase(std::remove_if(retired.begin(), retired.end(), [now](Retired& e) {
                      if (e.presents) --e.presents;
                      if (e.presents || now < e.ms) return false;
                      e.res->Release();
                      return true;
                  }), retired.end());
}
void release() {
    for (auto& t : slots) for (Slot& s : t) { retire(s.res); s = Slot{}; }
    any = false;
}
// The slot's copy of `like` (its description), resting in COPY_DEST; a slot of another description retired first
bool slot_for(Slot& s, ID3D12Resource* like, const char* what) {
    if (s.res) {
        const D3D12_RESOURCE_DESC have = s.res->GetDesc(), want = like->GetDesc();
        if (std::memcmp(&have, &want, sizeof have) == 0) return true;
        retire(s.res);
        s.valid = false;
    }
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12Resource* made = nullptr;
    if (!fgd::ring_resource(made, state, like, D3D12_RESOURCE_STATE_COPY_DEST, what)) return false;
    s.res = made;
    s.state = state;
    return true;
}
}  // namespace dd

// FFX's public loader can forward the descriptor through a second hooked export.
// That export already sees the shown-frame descriptor. Keep the original game's
// HUD-less input registered for the Present's composition.
struct ForwardedFfx {
    inline static thread_local const FfxHeader* current = nullptr;
    const FfxHeader* previous;
    explicit ForwardedFfx(const FfxHeader& desc) : previous(current) { current=&desc; }
    ~ForwardedFfx() { current=previous; }
    static bool contains(const FfxHeader* desc) {
        if (!desc || !current || current->type != desc->type) return false;
        if (desc==current) return true;
        if (desc->type==kFfxConfigureFg) {
            const auto& a=*reinterpret_cast<const FfxConfigureFg*>(current);
            const auto& b=*reinterpret_cast<const FfxConfigureFg*>(desc);
            return a.frame_id==b.frame_id && a.swap_chain==b.swap_chain && a.hudless.resource==b.hudless.resource;
        }
        if (desc->type==kFfxPrepare || desc->type==kFfxPrepareV2) {
            const auto& a=*reinterpret_cast<const FfxPrepare*>(current);
            const auto& b=*reinterpret_cast<const FfxPrepare*>(desc);
            return a.frame_id==b.frame_id && a.command_list==b.command_list &&
                   a.depth.resource==b.depth.resource && a.motion.resource==b.motion.resource;
        }
        return false;
    }
};
template <int I> int32_t ffx_configure(void* ctx, const FfxHeader* desc) {
    if (ForwardedFfx::contains(desc)) return real_ffx_configure[I](ctx,desc);
    void* const caller = ffx_caller<I>(__builtin_return_address(0));
    FfxOuter<I> outer(__builtin_return_address(0));
    // the even frames: the HUD-less frame (the frame generation configure first in the chain) and the UI (the swap
    // chain's register) of the frame shown now, copies
    if (desc && ctx && !t_own && (desc->type == kFfxConfigureFg || desc->type == kFfxUiResource) && fgd::entry<I>(caller) &&
        fgd::driver(caller)) {
        if (desc->type == kFfxConfigureFg)
            for (const FfxHeader* h = desc->next ? desc->next->next : nullptr; h; h = h->next)
                if (h->type == kFfxDistortion && reinterpret_cast<const FfxResource*>(h + 1)->resource)
                    fgd::unfollowed(nr::frame_reuse::distortion_chain);
    }
    if (desc && ctx && fgd::capture.load(std::memory_order_relaxed) && !t_own && !t_dlssg_handed && fgd::entry<I>(caller) &&
        (desc->type == kFfxConfigureFg || desc->type == kFfxUiResource) && fgd::driver(caller)) {
        void* const handle = *static_cast<void**>(ctx);
        if (desc->type == kFfxConfigureFg) {
            FfxConfigureFg use = *reinterpret_cast<const FfxConfigureFg*>(desc);
            FfxDistortion distortion{};
            const bool has_field=use.enabled && use.header.next && use.header.next->type==kFfxDistortion;
            bool delayed=false, delayed_field=false;
            {
                std::lock_guard<std::mutex> lock(fd::m);
                auto& ctx_data=fd::ctxs[handle];
                if (use.enabled && use.hudless.resource)
                    delayed=fd::resource(ctx_data,fd::kHudless,use.frame_id,use.hudless,nullptr);
                if (has_field) {
                    distortion=*reinterpret_cast<const FfxDistortion*>(use.header.next);
                    delayed_field=fd::resource(ctx_data,fd::kDistortion,use.frame_id,distortion.field,nullptr);
                    use.header.next=&distortion.header;
                }
            }
            if (delayed || delayed_field) {
                if (g_game_caller && g_game_caller(caller)) g_ffx_app_dispatch.store(use.enabled && !use.fg_callback);
                const auto& current=reinterpret_cast<const FfxConfigureFg*>(desc)->hudless;
                if (current.resource)
                    fg_note(caller,static_cast<ID3D12Resource*>(current.resource),d3d12_state_of_ffx(current.state),
                            nullptr,true,true,"FSR frame generation",true,nullptr,0,false,
                            static_cast<ID3D12Resource*>(use.hudless.resource),d3d12_state_of_ffx(use.hudless.state));
                ForwardedFfx forwarded(use.header);
                return real_ffx_configure[I](ctx,&use.header);
            }
        } else {
            FfxUiResource use = *reinterpret_cast<const FfxUiResource*>(desc);
            bool delayed = false;
            if (use.ui.resource) {
                std::lock_guard<std::mutex> lock(fd::m);
                delayed = fd::resource(fd::ctxs[handle], fd::kUi, fgd::frame_now(), use.ui, nullptr);
            }
            if (delayed) { ForwardedFfx forwarded(use.header); return real_ffx_configure[I](ctx, &use.header); }
        }
    }
    for (const FfxHeader* h = desc; h; h = h->next)
        if (h->type == kFfxConfigureFg) {
            const auto* c = reinterpret_cast<const FfxConfigureFg*>(h);
            if (g_game_caller && g_game_caller(caller))
                g_ffx_app_dispatch.store(c->enabled && !c->fg_callback);
            static std::atomic<int> told{0};
            if (told.fetch_add(1) < 3)   // what the game hands frame generation (how its generated frames are made)
                log("[nr] frame generation (FSR, %s from %s): configure enabled %d, async %d, swap chain %p, present "
                    "callback %p, frame-generation callback %p, HUD-less %p (format %u, %ux%u)", kFfxLibrary[g_ffx_slot_name[I]],
                    module_of(caller).c_str(), c->enabled ? 1 : 0, c->async ? 1 : 0, c->swap_chain,
                    c->present_callback, c->fg_callback, c->hudless.resource, c->hudless.format, c->hudless.width,
                    c->hudless.height);
            if (c->enabled && c->hudless.resource)
                fg_note(caller, static_cast<ID3D12Resource*>(c->hudless.resource),
                        d3d12_state_of_ffx(c->hudless.state), nullptr, true, true, "FSR frame generation");
            break;
        }
    return real_ffx_configure[I](ctx, desc);
}
// ffxDispatchDescFrameGeneration (ffx_framegeneration.h): a game that runs frame generation itself (no
// frame-generation callback) records it on the swap chain's interpolation command list, from presentColor
struct FfxDispatchFg {
    FfxHeader header;
    void* command_list;
    FfxResource present_color;
    FfxResource outputs[4];
    uint32_t generated;
};
constexpr uint64_t kFfxDispatchFg = 0x00020003u;   // FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION
using PFN_FfxDispatch = int32_t (*)(void*, const FfxHeader*);
PFN_FfxDispatch real_ffx_dispatch[2 * kFfxSlots];
// The prepare dispatch (V1, with or without its camera chained first, or V2) by the driver: this frame's depth and vectors
// copied into the ring on its list, and frame `id - lag`'s copies and numbers handed over in their place (`use`, `camera`:
// copies of the caller's descriptions). false: the ring has nothing of that frame (handed over as given; V2: reset).
bool ffx_prepare(void* handle, FfxPrepare& use, float* camera) {
    auto* list = static_cast<ID3D12GraphicsCommandList*>(use.command_list);
    std::lock_guard<std::mutex> lock(fd::m);
    if (t_in_present)   // OptiScaler's: what it handed over during this Present (its configure just before) goes first
        for (auto& [h, c] : fd::ctxs) fd::flush(c, list);
    fd::Ctx& c = fd::ctxs[handle];
    const uint64_t id = use.frame_id;
    fd::Numbers& now = c.numbers[id % fd::kRing];
    now.id = id; now.valid = true; now.flags=use.flags; now.reset=use.reset; now.has_camera=camera!=nullptr;
    std::memcpy(now.render, use.render, sizeof now.render);
    std::memcpy(now.jitter, use.jitter, sizeof now.jitter);
    std::memcpy(now.mv_scale, use.mv_scale, sizeof now.mv_scale);
    now.dt = use.dt; now.near_ = use.near_; now.far_ = use.far_; now.fov = use.fov; now.meters = use.meters;
    if (camera) std::memcpy(now.camera, camera, sizeof now.camera);
    const bool depth = fd::resource(c, fd::kDepth, id, use.depth, list);
    const bool motion = fd::resource(c, fd::kMotion, id, use.motion, list);
    const uint32_t k = fgd::input_lag();
    if (!k) return true; // The current originals stay with the provider while its history fills.
    const fd::Numbers& before = c.numbers[(id + fd::kRing - k) % fd::kRing];
    if (!depth || !motion || !before.valid || before.id + k != id) {   // as given: this frame's own
        if (depth || motion) {   // (one swapped, the other not: both back to this frame's)
            fd::Slot& d = c.slots[fd::kDepth][id % fd::kRing];
            fd::Slot& m = c.slots[fd::kMotion][id % fd::kRing];
            if (depth && d.res) { use.depth.resource = d.res; use.depth.state = d.ffx_state; }
            if (motion && m.res) { use.motion.resource = m.res; use.motion.state = m.ffx_state; }
        }
        return false;
    }
    use.flags=before.flags; use.reset=before.reset;
    std::memcpy(use.render, before.render, sizeof use.render);
    std::memcpy(use.jitter, before.jitter, sizeof use.jitter);
    std::memcpy(use.mv_scale, before.mv_scale, sizeof use.mv_scale);
    use.dt = before.dt; use.near_ = before.near_; use.far_ = before.far_; use.fov = before.fov; use.meters = before.meters;
    if (camera && before.has_camera) std::memcpy(camera, before.camera, sizeof before.camera);
    static std::atomic<int> told{0};
    if (!told.fetch_add(1))
        log("[nr] even frames with FSR frame generation: its prepare is handed frame %llu's inputs and numbers at frame %llu "
            "(%u frame(s) later)%s", (unsigned long long)before.id, (unsigned long long)id, k,
            camera ? ", the camera too" : "");
    return true;
}
template <int I> int32_t ffx_dispatch(void* ctx, const FfxHeader* desc) {
    if (ForwardedFfx::contains(desc)) return real_ffx_dispatch[I](ctx,desc);
    void* const caller = ffx_caller<I>(__builtin_return_address(0));
    FfxOuter<I> outer(__builtin_return_address(0));
    if (desc && ctx && (desc->type == kFfxPrepare || desc->type == kFfxPrepareV2) && !t_own && fgd::entry<I>(caller) &&
        fgd::driver(caller)) {
        fgd::ffx_at.store(fgd::frame_now(), std::memory_order_relaxed);
        const bool chained = desc->type == kFfxPrepare && desc->next && desc->next->type == kFfxCameraInfo;
        if (desc->type == kFfxPrepare && !chained)
            for (const FfxHeader* h = desc->next; h; h = h->next)
                if (h->type == kFfxCameraInfo) fgd::unfollowed(nr::frame_reuse::ffx_chain);
        if (fgd::capture.load(std::memory_order_relaxed) && !t_dlssg_handed) {   // (DLSS-G's evaluate handed them already)
            void* const handle = *static_cast<void**>(ctx);
            if (desc->type == kFfxPrepareV2) {
                FfxPrepareV2 use = *reinterpret_cast<const FfxPrepareV2*>(desc);
                if (!ffx_prepare(handle, use.base, use.camera)) use.base.reset = true;
                ForwardedFfx forwarded(use.base.header);
                return real_ffx_dispatch[I](ctx, &use.base.header);
            }
            FfxPrepare use = *reinterpret_cast<const FfxPrepare*>(desc);
            FfxCameraInfo camera{};
            if (chained) { camera = *reinterpret_cast<const FfxCameraInfo*>(desc->next); use.header.next = &camera.header; }
            if (!ffx_prepare(handle, use, chained ? camera.camera : nullptr)) use.reset=true;
            ForwardedFfx forwarded(use.header);
            return real_ffx_dispatch[I](ctx, &use.header);
        }
    }
    if (desc && desc->type == kFfxDispatchFg) {
        const auto* d = reinterpret_cast<const FfxDispatchFg*>(desc);
        static std::atomic<int> told{0};
        if (told.fetch_add(1) < 3)
            log("[nr] frame generation (FSR, %s from %s): dispatched by the game on list %p from presentColor %p (format %u, "
                "%ux%u, state 0x%x) into %p, %u generated", kFfxLibrary[g_ffx_slot_name[I]], module_of(caller).c_str(),
                d->command_list, d->present_color.resource, d->present_color.format, d->present_color.width,
                d->present_color.height, d->present_color.state, d->outputs[0].resource, d->generated);
        // The frame the generated frames are made from, when it is not the back buffer (d3d12_present checks): read when
        // the swap chain presents, so the Present layers' change is put into it as into a HUD-less frame (fg_delta), or
        // the generated frames are shown without NR
        if (g_ffx_app_dispatch.load() && fgd::entry<I>(caller) && fgd::driver(caller)) {
            std::lock_guard<std::mutex> guard(interpolation_mutex);
            const auto found=interpolation_lists.find(d->command_list);
            if (found!=interpolation_lists.end() && found->second==interpolation_frame.load(std::memory_order_relaxed)) {
                app_colour_at.store(std::max<uint64_t>(1,fgd::frame_now()),std::memory_order_relaxed);
                static bool said=false;
                if(!said){said=true;log("[nr] FSR application dispatch: its colour is consumed on the swapchain interpolation list at Present");}
            } else fgd::unfollowed(nr::frame_reuse::dispatch_colour);
        }
        if (d->present_color.resource && g_ffx_app_dispatch.load())
            fg_note(caller, static_cast<ID3D12Resource*>(d->present_color.resource),
                    d3d12_state_of_ffx(d->present_color.state), nullptr, true, true, "FSR frame generation", false);
    }
    return real_ffx_dispatch[I](ctx, desc);
}
// Only the lifetime and full-frame contract exercised by the XeSS game is admitted here, in any format with an alpha
// (ui_alpha_format; until 11-11 R8G8B8A8 with an R8G8B8A8_UNORM swap chain only: BGRA8 and 10-bit SDR swap chains fell
// to the RGB difference). ONLY_NOW, subrect and array/MSAA sources keep the RGB-difference fallback; an HDR back buffer
// too (the Present layers decide that: the UI is blended in the game's own space, not in PQ / scRGB).
void xess_ui_note(void* caller, void* context, uint32_t frame, const XefgResource& tag) {
    if (t_own || t_in_present || (caller && g_game_caller && !g_game_caller(caller))) return;
    std::lock_guard<std::mutex> lock(gf.m);
    gf.ui = {}; // an explicit null, invalid or mismatched replacement must not leave an old UI tag active
    gf.ui_leases.erase(std::remove_if(gf.ui_leases.begin(), gf.ui_leases.end(),
        [](const auto& p) { return p.use_count() == 1 && p->ready(); }), gf.ui_leases.end());
    if (!gf.armed.load() || !tag.resource || tag.validity != 0 || !gf.copy ||
        tag.base[0] || tag.base[1] ||
        tag.size[0] != gf.w || tag.size[1] != gf.h) return;
    const auto d = tag.resource->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize != 1 || d.MipLevels != 1 ||
        d.SampleDesc.Count != 1 || d.Width != gf.w || d.Height != gf.h ||
        ui_alpha_format(d.Format) == DXGI_FORMAT_UNKNOWN) return;
    ID3D12Device *a=nullptr,*b=nullptr;
    tag.resource->GetDevice(IID_PPV_ARGS(&a)); gf.copy->GetDevice(IID_PPV_ARGS(&b));
    const bool same = a && a == b;
    if (a) a->Release(); if (b) b->Release(); if (!same) return;
    std::shared_ptr<ChainSnapshot> lease;
    for (const auto& p : gf.ui_leases) if (p->dst == tag.resource) { lease = p; break; }
    if (!lease) {
        if (gf.ui_leases.size() >= 16) return; // pending GPU readers never evicted to make room
        lease = std::make_shared<ChainSnapshot>();
        tag.resource->AddRef(); lease->dst = tag.resource;
        gf.ui_leases.push_back(lease);
    }
    gf.ui.context=context; gf.ui.frame=frame; gf.ui.at=g.presents; gf.ui.image=tag.resource;
    gf.ui.state=tag.state; gf.ui.keep=std::move(lease);
}
// Observe tags before filtering for HUD-less. This records availability, NOT alpha correctness,
// same-frame colour agreement or resource ownership. Streamline tokens are opaque pointers.
// Sample only three calls per type and then once per ten seconds; counts include skipped calls.
// Numeric SL 68 is deliberately ambiguous: its meaning changed between SDK versions.
static nr::hudcap::InputInventory fg_inventory;
bool input_inventory_enabled() {
    static const bool enabled=[] { const char* e=std::getenv("NR_HUD_INPUT_LOG");return e && std::strcmp(e,"1")==0; }();
    return enabled;
}
const char* xefg_tag_name(uint32_t type) {
    switch (type) {
    case 0: return "HUDLessColor"; case 1: return "Depth"; case 2: return "MotionVector";
    case 3: return "UI"; case 4: return "Backbuffer-region"; default: return "unknown";
    }
}
const char* sl_tag_name(uint32_t type) {
    switch (type) {
    case 2: return "HUDLessColor"; case 23: return "UIColorAndAlpha";
    case 68: return "UIAlpha-old-or-Responsivity-new"; case 69: return "UIAlpha-new";
    default: return "other";
    }
}
// ---- NRFrameReuse's even frames with XeSS frame generation: its inputs as many frames later ---------------------------
// The even frames show every frame `lag` frames later (1; 2 with the two-sided rebuild: the frame's NR answer in the back
// buffer of the frame `lag` after it). XeSS frame generation is then handed that frame's HUD-less frame, depth, motion
// vectors and UI too - copies of ours, four frames of each, made from what the game tags (on the command list it tags
// them on, or at the Present when valid until it) - and that frame's constants (xefgSwapChainTagFrameConstants), so the
// frames it generates sit between the frames shown. The copies run while the even frames do (xefg_delay); a frame whose
// frame `lag` before has no copy (the even frames just began) is handed over as the game tagged it, history reset.
namespace xd {
constexpr uint32_t kTypes = 4, kRing = 4;   // HUDLESS_COLOR, DEPTH, MOTION_VECTOR, UI; frames (lag 2 + this one + one read)
struct Slot {
    ID3D12Resource* res = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;   // where it rests (the state the game tags its own in)
    uint32_t base[2]{},size[2]{};
    uint32_t id = 0;
    bool valid = false;   // holds frame `id`'s (or will, by the time frame generation reads it)
};
struct Pending { ID3D12Resource* game; D3D12_RESOURCE_STATES state; uint32_t type, ring; bool at_proxy; };
constexpr size_t kConstants = 152;   // xefg_swapchain_frame_constant_data_t (packed): 2 x float4x4, 4 floats, uint, float
constexpr size_t kResetAt = 144;     // its resetHistory
struct Ctx {
    Slot slots[kTypes][kRing];
    uint8_t constants[kRing][kConstants] = {};
    uint32_t constant_id[kRing] = {};
    bool constant_valid[kRing] = {};
    bool reset = false;              // a frame handed over undelayed: its constants reset frame generation's history
    std::vector<Pending> pending;    // copies at the Present (resources valid until it, tagged without a command list)
};
std::mutex m;
std::unordered_map<void*, Ctx> ctxs;
std::unordered_set<void*> observed_proxies;
std::atomic<unsigned> proxy_pending{0};
std::atomic<bool> on{false};
std::atomic<uint32_t> lag{1};   // frames late (1 or 2)
uint32_t off_presents = 0;   // Presents since the even frames stopped: the copies go after a few (frame generation may
                             // still read the last one handed over)
bool hooked_tags = false, hooked_constants = false;
void release(Ctx& c) {
    for (auto& t : c.slots) for (auto& sl : t) if (sl.res) { sl.res->Release(); sl.res = nullptr; sl.valid = false; }
    for (auto& p : c.pending) {if(p.at_proxy)--proxy_pending;if(p.game)p.game->Release();}
    c.pending.clear();
}
// The ring's resource for `type`, frame `id`, like the game's `g` (same description: CopyResource takes the pair).
Slot* slot(Ctx& c, uint32_t type, uint32_t id, ID3D12Resource* g, D3D12_RESOURCE_STATES state) {
    Slot& sl = c.slots[type][id % kRing];
    const D3D12_RESOURCE_DESC want = g->GetDesc();
    if (sl.res) {
        const D3D12_RESOURCE_DESC have = sl.res->GetDesc();
        const bool same = have.Dimension == want.Dimension && have.Width == want.Width && have.Height == want.Height &&
                          have.DepthOrArraySize == want.DepthOrArraySize && have.MipLevels == want.MipLevels &&
                          have.Format == want.Format && have.SampleDesc.Count == want.SampleDesc.Count &&
                          have.SampleDesc.Quality == want.SampleDesc.Quality && have.Layout == want.Layout &&
                          have.Flags == want.Flags;
        if (!same || sl.state != state) { sl.res->Release(); sl.res = nullptr; sl.valid = false; }
    }
    if (!sl.res) {
        ID3D12Device* d = nullptr;
        if (FAILED(g->GetDevice(IID_PPV_ARGS(&d))) || !d) return nullptr;
        D3D12_HEAP_PROPERTIES heap{};heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = want;
        const HRESULT hr = d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&sl.res));
        d->Release();
        if (FAILED(hr) || !sl.res) {
            sl.res = nullptr;
            static std::atomic<int> told{0};
            if (told.fetch_add(1) < 2)
                log("[nr] even frames with XeSS frame generation: a copy of its %s input could not be made (0x%08x): "
                    "that input is handed over as the game tagged it", type == 0 ? "HUD-less" : type == 1 ? "depth" :
                    type == 2 ? "motion vector" : "UI", unsigned(hr));
            return nullptr;
        }
        sl.state = state;
    }
    return &sl;
}
void copy(ID3D12GraphicsCommandList* l, ID3D12Resource* from, D3D12_RESOURCE_STATES from_state, const Slot& to) {
    ++t_own;
    transition(l, from, from_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(l, to.res, to.state, D3D12_RESOURCE_STATE_COPY_DEST);
    l->CopyResource(to.res, from);
    transition(l, to.res, D3D12_RESOURCE_STATE_COPY_DEST, to.state);
    transition(l, from, D3D12_RESOURCE_STATE_COPY_SOURCE, from_state);
    --t_own;
}
// A tag of frame `id` while the even frames run: this frame's resource copied into the ring (now on `list`, else at the
// Present), and the frame before's handed over in its place - `use` changed - when the ring has it.
void tag(void* sc, ID3D12GraphicsCommandList* list, uint32_t id, const XefgResource& r, XefgResource* use) {
    if (r.type >= kTypes || !r.resource) return;
    std::lock_guard<std::mutex> lock(m);
    Ctx& c = ctxs[sc];
    if (Slot* now = slot(c, r.type, id, r.resource, r.state)) {
        if (list) copy(list, r.resource, r.state, *now);
        else { r.resource->AddRef(); c.pending.push_back({r.resource, r.state, r.type, id % kRing,t_in_present!=0});if(t_in_present)++proxy_pending; }
        std::copy_n(r.base,2,now->base);std::copy_n(r.size,2,now->size);
        now->id = id; now->valid = true;
    }
    const uint32_t k = fgd::input_lag();
    if (!k) return;
    const Slot& before = c.slots[r.type][(id + kRing - k) % kRing];
    if (before.res && before.valid && before.id + k == id) {
        std::copy_n(before.base,2,use->base);std::copy_n(before.size,2,use->size);
        use->resource = before.res; use->state = before.state;
        use->validity = 0;   // UNTIL_NEXT_PRESENT: ours, kept until frame generation has read it
        static std::atomic<uint32_t> said{0};
        if (!(said.fetch_or(1u << r.type) & (1u << r.type)))
            log("[nr] even frames with XeSS frame generation: its %s input is handed over %u frame(s) later (presentId %u: "
                "the copy of %u, %s)", r.type == 0 ? "HUD-less" : r.type == 1 ? "depth" : r.type == 2 ? "motion vector" : "UI",
                k, id, before.id, list ? "copied on the tag's command list" : "copied at the Present");
    } else {
        c.reset = true;      // the frame before has none: handed over as tagged, frame generation's history reset
    }
}
void flush(Ctx& c,ID3D12GraphicsCommandList* list,bool proxy_only) {
    for(auto it=c.pending.begin();it!=c.pending.end();) {
        if(it->at_proxy!=proxy_only){++it;continue;}
        auto& slot=c.slots[it->type][it->ring];
        if(list && slot.res) copy(list,it->game,it->state,slot);
        if(it->at_proxy)--proxy_pending;
        it->game->Release();it=c.pending.erase(it);
    }
}
}  // namespace xd

using PFN_XefgTag = int32_t (*)(void*, ID3D12CommandList*, uint32_t, const XefgResource*);
// Every libxess_fg.dll of the process (a game ships its own; OptiScaler brings another for its own frame generation, and
// which one loads first differs by game): each its own detour (slot) and original
constexpr int kXefgSlots = 4;
PFN_XefgTag real_xefg_tag[kXefgSlots];
using PFN_XefgConstants = int32_t (*)(void*, uint32_t, const void*);
PFN_XefgConstants real_xefg_constants[kXefgSlots];
// xefgSwapChainTagFrameConstants: this frame's kept; while the even frames run the frame before's handed over (this
// frame's with its history reset when the frame before has none, or a resource of this frame went over undelayed).
int32_t xefg_constants(PFN_XefgConstants real, void* caller, void* sc, uint32_t id, const void* constants) {
    // (the driver's constants once a frame: XeSS frame generation runs, its inputs from calls that can be followed)
    const bool driver = constants && !t_own && !t_dlssg_handed && fgd::driver(caller, sc);
    if (driver) fgd::xefg_at.store(fgd::frame_now(), std::memory_order_relaxed);
    if (!driver || !xd::on.load(std::memory_order_relaxed)) return real(sc, id, constants);
    uint8_t use[xd::kConstants];
    {
        std::lock_guard<std::mutex> lock(xd::m);
        xd::Ctx& c = xd::ctxs[sc];
        const uint32_t k = fgd::input_lag();
        const uint32_t now = id % xd::kRing, before = (id + xd::kRing - k) % xd::kRing;
        std::memcpy(c.constants[now], constants, xd::kConstants);
        c.constant_id[now] = id; c.constant_valid[now] = true;
        const bool have = c.constant_valid[before] && c.constant_id[before] + k == id;
        std::memcpy(use, have ? c.constants[before] : c.constants[now], xd::kConstants);
        if (k && (!have || c.reset)) { const uint32_t one = 1; std::memcpy(use + xd::kResetAt, &one, 4); }
        static std::atomic<int> said{0};
        if (have && said.fetch_add(1) == 0)
            log("[nr] even frames with XeSS frame generation: its frame constants are handed over %u frame(s) later "
                "(presentId %u: those of %u)", k, id, id - k);
        c.reset = false;
    }
    return real(sc, id, use);
}
template <int I> int32_t xefg_constants_at(void* sc, uint32_t id, const void* constants) {
    return xefg_constants(real_xefg_constants[I], __builtin_return_address(0), sc, id, constants);
}
int32_t xefg_tag(PFN_XefgTag real, void* caller, void* sc, ID3D12CommandList* game_r_list, uint32_t id,
                 const XefgResource* game_r) {
    // NRFrameReuse's even frames: the frame before's copy in place of this frame's resource (xd above)
    XefgResource delayed{};
    const XefgResource* r = game_r;
    ID3D12CommandList* list = game_r_list;
    if (game_r && game_r->type < xd::kTypes && game_r->resource && !t_own && !t_dlssg_handed && fgd::driver(caller, sc)) {
        ID3D12GraphicsCommandList* gl = nullptr;
        if (list && reinterpret_cast<uintptr_t>(list) > 0xFFFF && SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&gl))) && gl)
            gl->Release();   // (the caller holds it)
        // one valid until the present handed over during it (OptiScaler's own frame generation): its copy would be made
        // at the next Present's layers, from what the resource holds then
        bool can_copy=gl || game_r->validity==0;
        if(!gl && t_in_present) {
            std::lock_guard<std::mutex> guard(xd::m);
            can_copy=can_copy && xd::observed_proxies.count(sc);
        }
        if(!can_copy)fgd::unfollowed(nr::frame_reuse::xe_list);
        else if (xd::on.load(std::memory_order_relaxed)) {
            delayed = *game_r;
            xd::tag(sc, gl, id, *game_r, &delayed);
            r = &delayed;
        }
    }
    if (r && r->type == 0 && r->resource) {
        ID3D12GraphicsCommandList* gl = nullptr;
        if (list && reinterpret_cast<uintptr_t>(list) > 0xFFFF && SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&gl))) && gl) gl->Release();
        const auto d = r->resource->GetDesc();
        const bool full = r->validity == 0 && !r->base[0] && !r->base[1] && r->size[0] == d.Width && r->size[1] == d.Height &&
                          d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 &&
                          d.MipLevels == 1 && d.SampleDesc.Count == 1;
        fg_note(caller, game_r->resource, game_r->state, gl, game_r->validity == 0, game_r->validity == 0,
                "XeSS frame generation", true, sc, id, full, r->resource, r->state);
    }
    // Copy metadata while ONLY_NOW resources are still valid; never dereference them after the API returns.
    nr::hudcap::InputInventory::Count count;
    const bool sampled = input_inventory_enabled() && r && fg_inventory.sample(0, r->type, r->resource != nullptr, GetTickCount64(), count);
    XefgResource meta{};
    D3D12_RESOURCE_DESC image{};
    bool described = false;
    const auto observation = g.chain_stamp.load();
    if (sampled) {
        meta = *r;
        // BACKBUFFER's resource pointer is ignored by XeSS; only its region is meaningful.
        if (meta.resource && meta.type <= 3) { image = meta.resource->GetDesc(); described = true; }
    }
    // Snapshot the description before the real API; ONLY_NOW contents are not read after it.
    const XefgResource tagged = r ? *r : XefgResource{};
    const int32_t result = real(sc, list, id, r);
    if (r && tagged.type == 3) {
        XefgResource ui = *game_r;
        if (result != 0) ui.resource = nullptr;
        xess_ui_note(caller, sc, id, ui);
    }
    if (r && tagged.type == 0 && (!tagged.resource || result != 0)) {
        std::lock_guard<std::mutex> lock(gf.m);
        if (gf.xess_context == sc && gf.xess_frame == id) gf.xess_full_frame = false;
    }
    if (sampled)
        log("[nr] HUD inputs XeSS: type=%u(%s) presentId=%u context=%p observation=%llu caller=%s "
            "resource=%p validity=%u region=%u,%u+%ux%u state=0x%x list=%p described=%d DXGI=%u image=%llux%u "
            "result=%d calls=%llu populated=%llu overflow=%d; metadata only",
            meta.type, xefg_tag_name(meta.type), id, sc, (unsigned long long)observation,
            module_of(caller).c_str(), meta.resource, meta.validity,
            meta.base[0], meta.base[1], meta.size[0], meta.size[1], unsigned(meta.state), list,
            described ? 1 : 0, unsigned(image.Format), (unsigned long long)image.Width, image.Height, result,
            (unsigned long long)count.calls, (unsigned long long)count.populated, count.overflow ? 1 : 0);
    return result;
}
template <int I> int32_t xefg_tag_at(void* sc, ID3D12CommandList* list, uint32_t id, const XefgResource* r) {
    return xefg_tag(real_xefg_tag[I], __builtin_return_address(0), sc, list, id, r);
}
void sl_inventory(void* caller, const void* frame, const void* vp, const SlTag* tags, uint32_t n, void* cmd) {
    if (!input_inventory_enabled()) return;
    for (uint32_t i = 0; tags && i < n; ++i) {
        const SlTag& t = tags[i];
        const SlResource* r = t.resource;
        nr::hudcap::InputInventory::Count count;
        if (!fg_inventory.sample(1, t.type, r && r->native, GetTickCount64(), count)) continue;
        // No GetDesc/QI here: a tagged native handle can be Vulkan, D3D11 or D3D12.
        log("[nr] HUD inputs Streamline: type=%u(%s) token=%p viewport=%p observation=%llu caller=%s "
            "resource=%p nativeType=%d lifecycle=%d region=%u,%u+%ux%u state=0x%x list=%p "
            "calls=%llu populated=%llu overflow=%d; offered only, token opaque, API/format unverified",
            t.type, sl_tag_name(t.type), frame, vp, (unsigned long long)g.chain_stamp.load(),
            module_of(caller).c_str(), r ? r->native : nullptr, r ? int(r->type) : -1, t.lifecycle,
            t.extent[0], t.extent[1], t.extent[2], t.extent[3], r ? r->state : 0, cmd,
            (unsigned long long)count.calls, (unsigned long long)count.populated, count.overflow ? 1 : 0);
    }
}
void sl_tags(void* caller, const SlTag* tags, uint32_t n, void* cmd) {
    for (uint32_t i = 0; tags && i < n; ++i) {
        const SlTag& t = tags[i];
        if (t.type != 2 || !t.resource || !t.resource->native) continue;   // kBufferTypeHUDLessColor
        ID3D12GraphicsCommandList* gl = nullptr;
        IUnknown* u = static_cast<IUnknown*>(cmd);
        if (u && reinterpret_cast<uintptr_t>(u) > 0xFFFF && SUCCEEDED(u->QueryInterface(IID_PPV_ARGS(&gl))) && gl) gl->Release();
        // lifecycle 0 valid only now, 1 until the present, 2 until its evaluate: only 1 is read at the Present; the others
        // are copied on their command list (refused without one)
        fg_note(caller, static_cast<ID3D12Resource*>(t.resource->native), D3D12_RESOURCE_STATES(t.resource->state), gl, t.lifecycle == 1,
                t.lifecycle == 1, "DLSS frame generation (Streamline)");
    }
}
using PFN_SlSetTag = int (*)(const void*, const SlTag*, uint32_t, void*);
using PFN_SlSetTagForFrame = int (*)(const void*, const void*, const SlTag*, uint32_t, void*);
PFN_SlSetTag real_sl_set_tag;
PFN_SlSetTagForFrame real_sl_set_tag_frame;
int sl_set_tag(const void* vp, const SlTag* tags, uint32_t n, void* cmd) {
    sl_inventory(__builtin_return_address(0), nullptr, vp, tags, n, cmd);
    sl_tags(__builtin_return_address(0), tags, n, cmd);
    return real_sl_set_tag(vp, tags, n, cmd);
}
int sl_set_tag_frame(const void* f, const void* vp, const SlTag* tags, uint32_t n, void* cmd) {
    sl_inventory(__builtin_return_address(0), f, vp, tags, n, cmd);
    sl_tags(__builtin_return_address(0), tags, n, cmd);
    return real_sl_set_tag_frame(f, vp, tags, n, cmd);
}

bool hook_code(void* fn, void* detour, void** real) {
    MH_STATUS st = MH_CreateHook(fn, detour, real);
    if (st == MH_OK) st = MH_EnableHook(fn);
    if (st != MH_OK) { log("[nr] frame generation: code at %p could not be hooked (%s)", fn, MH_StatusToString(st)); return false; }
    return true;
}
// OptiScaler hooks a FidelityFX module's exports itself (Detours: its input hooks, the game's calls) and makes its own calls
// - its frame generation's - through the hook's trampoline, the export's first instructions moved, past the export: a
// hook of the export does not see them. The trampoline of an export hooked that way (x64 Detours 4, its
// _DETOUR_TRAMPOLINE: rbCode[30] cbCode ... pbRemain pbDetour rbCodeIn[8]): the export begins with a jmp to rbCodeIn,
// which jumps through pbDetour into OptiScaler, int3s after that jmp up to pbRemain; rbCode holds the moved
// instructions and a jmp back to pbRemain. Null when the export is not hooked that way (each fact checked).
void* ffx_entry(FARPROC f) {
    const auto* a = reinterpret_cast<const uint8_t*>(f);
    if (!a || a[0] != 0xE9 || !g_opti_caller) return nullptr;
    const uint8_t* in = a + 5 + *reinterpret_cast<const int32_t*>(a + 1);   // rbCodeIn: jmp [rip-14] = [pbDetour]
    MEMORY_BASIC_INFORMATION mi{};
    if (!VirtualQuery(in - 88, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
        reinterpret_cast<const uint8_t*>(mi.BaseAddress) + mi.RegionSize < in + 14)
        return nullptr;
    if (in[0] != 0xFF || in[1] != 0x25 || *reinterpret_cast<const int32_t*>(in + 2) != -14) return nullptr;
    void* const detour = *reinterpret_cast<void* const*>(in - 8);
    const uint8_t* remain = *reinterpret_cast<const uint8_t* const*>(in - 16);
    size_t pad = 0;
    while (pad < 25 && a[5 + pad] == 0xCC) ++pad;
    const uint8_t* code = in - 88;
    const uint8_t cb = code[30];
    if (!g_opti_caller(detour) || remain != a + 5 + pad || cb < 5 || cb > 24 || code[cb] != 0xFF || code[cb + 1] != 0x25)
        return nullptr;
    return const_cast<uint8_t*>(code);
}
bool hook_export(HMODULE m, const char* name, void* detour, void** real) {
    void* fn = reinterpret_cast<void*>(GetProcAddress(m, name));
    if (!fn) return false;
    MH_STATUS st = MH_CreateHook(fn, detour, real);
    if (st == MH_OK) st = MH_EnableHook(fn);
    if (st != MH_OK) { log("[nr] HUD FIX (frame generation): %s could not be hooked (%s)", name, MH_StatusToString(st)); return false; }
    return true;
}
// The frame-generation libraries as they come (looked for once a second of Presents): each hooked once.
void fg_hook_libraries() {
    if (g.presents - gf.looked < 60 && gf.looked) return;
    gf.looked = g.presents;
    MH_Initialize();
    static const wchar_t* const ffx[3] = {L"amd_fidelityfx_dx12.dll", L"amd_fidelityfx_loader_dx12.dll",
                                          L"amd_fidelityfx_framegeneration_dx12.dll"};
    static void* const ffx_detour[2 * kFfxSlots] = {
        as(&ffx_configure<0>), as(&ffx_configure<1>), as(&ffx_configure<2>), as(&ffx_configure<3>), as(&ffx_configure<4>),
        as(&ffx_configure<5>), as(&ffx_configure<6>), as(&ffx_configure<7>), as(&ffx_configure<8>), as(&ffx_configure<9>),
        as(&ffx_configure<10>), as(&ffx_configure<11>)};
    static void* const query_detour[kFfxSlots]={as(&ffx_query<0>),as(&ffx_query<1>),as(&ffx_query<2>),
        as(&ffx_query<3>),as(&ffx_query<4>),as(&ffx_query<5>)};
    static void* const ffx_dispatch_detour[2 * kFfxSlots] = {
        as(&ffx_dispatch<0>), as(&ffx_dispatch<1>), as(&ffx_dispatch<2>), as(&ffx_dispatch<3>), as(&ffx_dispatch<4>),
        as(&ffx_dispatch<5>), as(&ffx_dispatch<6>), as(&ffx_dispatch<7>), as(&ffx_dispatch<8>), as(&ffx_dispatch<9>),
        as(&ffx_dispatch<10>), as(&ffx_dispatch<11>)};
    // every FidelityFX module of these names loaded (a game's copy and OptiScaler's, each by its own path)
    HMODULE mods[1024];
    DWORD bytes = 0;
    bool any = false;
    for (const wchar_t* n : ffx) any = any || GetModuleHandleW(n);
    if (any && K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &bytes))
        for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 1024; ++i) {
            wchar_t path[MAX_PATH] = {};
            if (!GetModuleFileNameW(mods[i], path, MAX_PATH)) continue;
            const wchar_t* base = std::wcsrchr(path, L'\\');
            int name = -1;
            for (int k = 0; k < 3; ++k) if (_wcsicmp(base ? base + 1 : path, ffx[k]) == 0) name = k;
            if (name < 0) continue;
            int slot = -1;
            for (int k = 0; k < kFfxSlots; ++k) {
                if (gf.hooked_ffx[k] == mods[i]) { slot = -2; break; }
                if (!gf.hooked_ffx[k] && slot == -1) slot = k;
            }
            if (slot < 0) continue;   // followed already, or no slot left
            gf.hooked_ffx[slot] = mods[i];
            g_ffx_slot_name[slot] = g_ffx_slot_name[slot + kFfxSlots] = name;
            // (OptiScaler's own calls, when it hooked the export itself first: their way past its hook, before ours)
            void* const own_c = ffx_entry(GetProcAddress(mods[i], "ffxConfigure"));
            void* const own_d = ffx_entry(GetProcAddress(mods[i], "ffxDispatch"));
            hook_export(mods[i],"ffxQuery",query_detour[slot],reinterpret_cast<void**>(&real_ffx_query[slot]));
            const bool c = hook_export(mods[i], "ffxConfigure", ffx_detour[slot], reinterpret_cast<void**>(&real_ffx_configure[slot]));
            const bool d = hook_export(mods[i], "ffxDispatch", ffx_dispatch_detour[slot], reinterpret_cast<void**>(&real_ffx_dispatch[slot]));
            if (c && d) fd::hooked = true;
            if (c) log("[nr] HUD FIX (frame generation): %ls's ffxConfigure followed", path);
            if (d) log("[nr] frame generation: %ls's ffxDispatch followed", path);
            if (own_c && own_d && hook_code(own_c, ffx_detour[slot + kFfxSlots], reinterpret_cast<void**>(&real_ffx_configure[slot + kFfxSlots])) &&
                hook_code(own_d, ffx_dispatch_detour[slot + kFfxSlots], reinterpret_cast<void**>(&real_ffx_dispatch[slot + kFfxSlots]))) {
                g_ffx_inner[slot] = true;
                log("[nr] frame generation: %ls's ffxConfigure and ffxDispatch hooked by OptiScaler before: its own calls followed "
                    "past its hook too", path);
            }
        }
    {   // every libxess_fg.dll loaded (by its own name: a game's copy and OptiScaler's are both)
        static void* const tag_detour[kXefgSlots] = {as(&xefg_tag_at<0>), as(&xefg_tag_at<1>), as(&xefg_tag_at<2>), as(&xefg_tag_at<3>)};
        static void* const constants_detour[kXefgSlots] = {as(&xefg_constants_at<0>), as(&xefg_constants_at<1>),
                                                           as(&xefg_constants_at<2>), as(&xefg_constants_at<3>)};
        HMODULE mods[1024];
        DWORD bytes = 0;
        if (GetModuleHandleW(L"libxess_fg.dll") && K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &bytes))
            for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 1024; ++i) {
                wchar_t path[MAX_PATH] = {};
                if (!GetModuleFileNameW(mods[i], path, MAX_PATH)) continue;
                const wchar_t* base = std::wcsrchr(path, L'\\');
                if (_wcsicmp(base ? base + 1 : path, L"libxess_fg.dll") != 0) continue;
                int slot = -1;
                for (int k = 0; k < kXefgSlots; ++k) {
                    if (gf.hooked_xefg[k] == mods[i]) { slot = -2; break; }
                    if (!gf.hooked_xefg[k] && slot == -1) slot = k;
                }
                if (slot < 0) continue;   // followed already, or no slot left
                gf.hooked_xefg[slot] = mods[i];
                const bool tags = hook_export(mods[i], "xefgSwapChainD3D12TagFrameResource", tag_detour[slot],
                                              reinterpret_cast<void**>(&real_xefg_tag[slot]));
                const bool constants = hook_export(mods[i], "xefgSwapChainTagFrameConstants", constants_detour[slot],
                                                   reinterpret_cast<void**>(&real_xefg_constants[slot]));
                if (tags) xd::hooked_tags = true;
                if (constants) xd::hooked_constants = true;
                if (tags) log("[nr] HUD FIX (frame generation): %ls's resource tags followed%s", path,
                              constants ? " (and its frame constants)" : "");
            }
    }
    if (!gf.hooked_sl)
        if (HMODULE m = GetModuleHandleW(L"sl.interposer.dll")) {
            gf.hooked_sl = true;
            const bool a = hook_export(m, "slSetTag", as(&sl_set_tag), reinterpret_cast<void**>(&real_sl_set_tag));
            const bool b = hook_export(m, "slSetTagForFrame", as(&sl_set_tag_frame), reinterpret_cast<void**>(&real_sl_set_tag_frame));
            if (a || b) log("[nr] HUD FIX (frame generation): Streamline's resource tags followed");
        }
}

}  // namespace

void fg_delay(int frames, bool capture) {
    const bool on = capture || frames > 0;
    fgd::capture.store(on,std::memory_order_relaxed);
    const uint32_t k = on ? uint32_t(std::min(frames, 2)) : 0u;
    if (on) xd::lag.store(k);
    xd::on.store(on);
    fgd::lag.store(k, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(xd::m);
        if (on) xd::off_presents = 0;
        else if (!xd::ctxs.empty() && ++xd::off_presents >= 4) {   // (the last one handed over read by then)
            for (auto& [sc, c] : xd::ctxs) xd::release(c);
            xd::ctxs.clear();
        }
    }
    {
        std::lock_guard<std::mutex> lock(dd::m);
        if (on) dd::off_frames = 0;
        else if (dd::any && ++dd::off_frames >= 4) dd::release();
        dd::tick();
    }
    std::lock_guard<std::mutex> lock(fd::m);
    if (on) fd::off_frames = 0;
    else if (!fd::ctxs.empty() && ++fd::off_frames >= 4) {
        for (auto& [h, c] : fd::ctxs) fd::release(c);
        fd::ctxs.clear();
    }
}
bool dlssg_delay_here() {
    const uint64_t l = fgd::library_ms.load(std::memory_order_relaxed);
    return !l || GetTickCount64() - l > 1000;
}
void dlssg_seen() { dd::seen_ms.store(GetTickCount64(), std::memory_order_relaxed); }
void dlssg_unfollowed(const char* why) { fgd::unfollowed(why); }
uint32_t fg_lag() { return fgd::capture.load(std::memory_order_relaxed)?uint32_t(nr::pe::Session::next_frames_delay()):0; }
void dlssg_reset_ring() {
    std::lock_guard<std::mutex> lock(dd::m);
    for (auto& t : dd::slots) for (dd::Slot& s : t) s.valid = false;
}
DlssgHanded::DlssgHanded(bool handed) : on(handed) { if (on) ++t_dlssg_handed; }
DlssgHanded::~DlssgHanded() { if (on) --t_dlssg_handed; }
ID3D12Resource* dlssg_input(uint32_t type, uint64_t id, uint32_t k, ID3D12Resource* game, ID3D12GraphicsCommandList* list,
                            bool* copied) {
    if (copied) *copied = false;
    if (type >= kDlssgInputs || !game || !list) return nullptr;
    std::lock_guard<std::mutex> lock(dd::m);
    dd::Slot& now = dd::slots[type][id % dd::kRing];
    if (dd::slot_for(now, game, dd::kName[type])) {
        fgd::copy_into(list, game, D3D12_RESOURCE_STATE_COPY_DEST, now.res, now.state);
        now.id = id; now.valid = true;
        dd::any = true;
        if (copied) *copied = true;
    } else {
        now.valid = false;
    }
    const dd::Slot& before = dd::slots[type][(id + dd::kRing - k) % dd::kRing];
    if (!k || !before.res || !before.valid || before.id + k != id) return nullptr;
    static std::atomic<uint32_t> told{0};
    if (!(told.fetch_or(1u << type) & (1u << type)))
        log("[nr] even frames with DLSS-G through NGX: its %s input is handed over %u frame(s) later (frame %llu: the copy "
            "of %llu, copied on the evaluate's command list)", dd::kName[type], k, (unsigned long long)id,
            (unsigned long long)before.id);
    return before.res;
}
bool fg_not_started() {
    const bool hooked = fd::hooked || (xd::hooked_tags && xd::hooked_constants);
    return hooked && !fgd::unfollowed_at.load(std::memory_order_relaxed) && !g_ffx_app_dispatch.load() &&
           !fgd::ffx_at.load(std::memory_order_relaxed) && !fgd::xefg_at.load(std::memory_order_relaxed) &&
           !dd::seen_ms.load(std::memory_order_relaxed);
}
bool fg_followed(bool xefg_proxy, const char** reason) {
    *reason=nr::frame_reuse::fg_unsynced;
    const uint64_t f = fgd::frames.load(std::memory_order_relaxed), u = fgd::unfollowed_at.load(std::memory_order_relaxed);
    if (u && f < u + 2) {
        if (auto* why=fgd::unfollowed_reason.load(std::memory_order_relaxed)) *reason=why;
        return false;
    }   // a call it cannot follow in the last 120 frames
    if (g_ffx_app_dispatch.load()) {
        const auto colour=app_colour_at.load(std::memory_order_relaxed);
        if(!colour || f>colour+1) { *reason=nr::frame_reuse::dispatch_colour; return false; }
    }
    const bool xefg_hooks = xd::hooked_tags && xd::hooked_constants;
    const uint64_t x = fgd::xefg_at.load(std::memory_order_relaxed), a = fgd::ffx_at.load(std::memory_order_relaxed);
    // DLSS-G through NGX whose evaluate is handed the inputs here (dlssg_seen): within the last second (its evaluates are
    // the Present layers' frames then; the frame count above does not move at them)
    const uint64_t g = dd::seen_ms.load(std::memory_order_relaxed);
    return (xefg_hooks && (xefg_proxy || (x && f <= x + 2))) || (fd::hooked && a && f <= a + 2) ||
           (g && GetTickCount64() - g < 1000);
}
void fg_delay_present(ID3D12GraphicsCommandList* list) {
    interpolation_frame.fetch_add(1,std::memory_order_relaxed);
    fgd::frames.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(xd::m);
        for(auto& item:xd::ctxs)xd::flush(item.second,list,false);
    }
    std::lock_guard<std::mutex> lock(fd::m);
    for (auto& [h, c] : fd::ctxs) fd::flush(c, list);
}
void xefg_proxy_observed(void* context,bool observed) {
    std::lock_guard<std::mutex> guard(xd::m);
    if(observed)xd::observed_proxies.insert(context);else xd::observed_proxies.erase(context);
}
bool xefg_any_pending() {return xd::proxy_pending.load(std::memory_order_relaxed)!=0;}
bool xefg_pending(void* context) {
    std::lock_guard<std::mutex> guard(xd::m);
    auto it=xd::ctxs.find(context);
    return it!=xd::ctxs.end() && std::any_of(it->second.pending.begin(),it->second.pending.end(),[](const auto& p){return p.at_proxy;});
}
void xefg_flush_proxy(void* context,ID3D12GraphicsCommandList* list) {
    std::lock_guard<std::mutex> guard(xd::m);
    auto it=xd::ctxs.find(context);if(it!=xd::ctxs.end())xd::flush(it->second,list,true);
}
void xefg_delay_forget(void* context) {
    std::lock_guard<std::mutex> lock(xd::m);
    xd::observed_proxies.erase(context);
    if (const auto it = xd::ctxs.find(context); it != xd::ctxs.end()) { xd::release(it->second); xd::ctxs.erase(it); }
}

// The private object follows Release as well as Reset. Keeping a COM reference to the command list itself
// would prevent its release and could retain every recording forever.
nr::gpu::Ticket d3d12_recording(ID3D12GraphicsCommandList* list) {
    static const GUID key = {0x709ae383, 0x15a8, 0x4e89, {0xb5, 0x8c, 0x3a, 0x6a, 0xde, 0x2d, 0xa1, 0x7f}};
    if (auto t = nr::gpu::find(list)) return t;
    if (!hook_method(list, 10, &hk_reset, &real_reset, "Reset")) return {};
    struct Lifetime final : IUnknown {
        std::atomic<ULONG> refs{1}; const void* key; nr::gpu::Ticket ticket; bool attached=false;
        Lifetime(const void* k, nr::gpu::Ticket t) : key(k), ticket(std::move(t)) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** p) override {
            if (!p) return E_POINTER;
            *p = nullptr;
            if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
            *p = this; AddRef(); return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
        ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --refs; if (!n) delete this; return n; }
        ~Lifetime() {
            // Replacing this private object after Reset must not clear the NEW recording's state.
            const bool current=nr::gpu::find(key)==ticket;
            nr::gpu::forget(key, ticket);
            if (current && attached) {
                std::lock_guard<std::mutex> lock(g.m);
                auto it=g.lists.find(const_cast<void*>(key));
                if (it!=g.lists.end()) it->second.reset();
            }
        }
    };
    auto ticket = nr::gpu::recording(list);
    auto* owned = new Lifetime(list, ticket);
    const HRESULT hr = list->SetPrivateDataInterface(key, owned);
    owned->attached=SUCCEEDED(hr);
    owned->Release();
    if (FAILED(hr)) return {};
    return ticket;
}

void d3d12_executed(unsigned count, ID3D12CommandList* const* lists, ID3D12CommandQueue* queue) { executed(count, lists, queue); }

void d3d12_use_chain(const Frame& frame) {
    if (frame.chain_keep) std::static_pointer_cast<ChainSnapshot>(frame.chain_keep)->touch(nr::gpu::current());
    if (frame.fg.ui_keep) std::static_pointer_cast<ChainSnapshot>(frame.fg.ui_keep)->touch(nr::gpu::current());
}

void chain_checked(uint64_t image, uint32_t hud, uint32_t total) {
    if (!image || !total) return;
    std::lock_guard<std::mutex> lk(g.chain_m);
    for (const auto& x : g.refused)   // (a result of a frame before its refusal: the read-back runs behind)
        if (x.image == image && x.until > g.presents) return;
    trace().add("count-result", 0, 0, image, hud, total);
    const bool fits = uint64_t(hud) * 4 <= uint64_t(total);
    const double share = 100.0 * double(hud) / double(total);
    const auto say = [&](const char* what) {
        if (g.said++ < 40) log("[nr] HUD FIX chain: image %p %s (the HUD %.1f %% of the frame)", reinterpret_cast<void*>(image),
                               what, share);
    };
    State::Check* c = nullptr;
    for (State::Check& x : g.checks)
        if (x.image == image) c = &x;
    if (!c) {   // a new one in the place of the longest unchecked
        c = &g.checks[0];
        for (State::Check& x : g.checks)
            if (x.at < c->at) c = &x;
        *c = {};
        c->image = image;
        // Preserve short retry treatment even if the four-entry check cache churned.
        for (const auto& x : g.refused)
            if (x.image == image && x.strikes == 0) c->fitted = true;
    }
    c->at = g.presents;
    if (c->trusted) {
        if (fits) c->bad = 0;
        else if (++c->bad >= 3) { c->trusted = false; c->ok = 0; c->bad = 0; say("no longer fits the presented frame: checked again"); }
        return;
    }
    if (fits) {
        c->fitted = true;
        c->misfits = 0;
        if (++c->ok >= 15) { c->trusted = true; say("fits the presented frame: NR runs on it"); }
        return;
    }
    c->ok = 0;
    // The GPU check already uses the frame itself for a mismatch. A once-fitting
    // source gets a short, renewable cooldown, so an ESC menu can recover. Clearing
    // chain_keys alone no longer affects admission (they are diagnostic only).
    if (++c->misfits < 30) return;
    if (c->fitted) {
        g.refused.erase(std::remove_if(g.refused.begin(),g.refused.end(),
            [image](const auto& x){return x.image==image;}),g.refused.end());
        g.refused.push_back({image,g.presents+60,0});
        if (g.refused.size()>16) g.refused.erase(g.refused.begin());
        for (uint64_t& k : g.chain_keys) if (k == image) k = 0;
        c->misfits = 0;
        say("no longer fits the presented frame: trying other images for 60 presents");
        return;
    }
    // not the frame before its HUD (or a menu over most of it): refused for 10 s, another image is tried; refused again
    // after that, for twice as long each time (5 min at most)
    uint32_t strikes = 1;
    for (auto it = g.refused.begin(); it != g.refused.end();)
        if (it->image == image) { strikes = std::min(it->strikes + 1, 6u); it = g.refused.erase(it); } else ++it;
    g.refused.push_back({image, g.presents + (600ull << (strikes - 1)), strikes});
    if (g.refused.size() > 16) g.refused.erase(g.refused.begin());
    for (uint64_t& k : g.chain_keys) if (k == image) k = 0;
    *c = {};
    say(strikes == 1 ? "does not fit the presented frame: refused for 10 s" : "does not fit the presented frame again: refused for longer");
}

Own::Own() { ++t_own; }
Own::~Own() { --t_own; }
void set_game_caller(bool (*game)(void* caller)) { g_game_caller = game; }
void set_opti_caller(bool (*opti)(void* caller)) { g_opti_caller = opti; }
void set_capture_wanted_query(bool (*query)()) { g_wanted_query = query; }
void set_capture_wanted(bool wanted) {
    const int now = wanted ? 1 : 0;
    if (g_wanted.exchange(now) == now) return;
    if (wanted) {
        if (g_views_on.load()) return;
        ID3D12Device* device = g.device.load();
        if (!device) return;   // (installed with the device's hooks: hook_metadata asks g_wanted)
        const bool fresh = !g_views_hooked.load();
        hook_views(device);
        log("[nr] HUD FIX: its view bookkeeping starts now (switched on in a running game): views the game made before "
            "are not known%s - for all of them, start the game with HUD FIX on", fresh ? "" : " (or while it was off)");
    } else {
        std::lock_guard<std::mutex> lk(g_views_m);
        if (!g_views_on.exchange(false)) return;
        g_views.clear();
        {   // (UAV views made while off are not seen: no device counts as followed from now on)
            std::lock_guard<std::mutex> w(g_watched_m);
            g_watched_devices.clear();
        }
        for (auto& b : g.back_watched) b.store(false);
        log("[nr] HUD FIX off: its view bookkeeping stops (no cost from it until HUD FIX is on again)");
    }
}
bool ffx_app_dispatch() { return g_ffx_app_dispatch.load(); }
PresentCall::PresentCall() { ++gf.in_present; ++t_in_present; }
PresentCall::~PresentCall() { --gf.in_present; --t_in_present; }

void fg_note_dlssg(ID3D12GraphicsCommandList* list, ID3D12Resource* hudless, bool layers_here, ID3D12Resource* destination) {
    // layers_here: the Present layers run in this evaluate, on its list, before frame generation reads the frame - it is
    // read (and may be substituted) as one valid until the present
    if (layers_here) gf.dlssg_ms.store(GetTickCount64());
    if (hudless && list)
        fg_note(nullptr, hudless, D3D12_RESOURCE_STATE_COPY_DEST, list, layers_here, layers_here, "DLSS frame generation (NGX)", true, nullptr, 0, false, destination, D3D12_RESOURCE_STATE_COPY_DEST);
}

void d3d12_device(ID3D12Device* device, bool armed_now) {
    (void)armed_now;   // the device's creations are followed at once either way (cheap: they come at load and resize)
    if (!device) return;
    g.device.store(device);
    hook_device(device);
}

void d3d12_module(HMODULE module) {
    if (!module) return;
    void* fn=reinterpret_cast<void*>(GetProcAddress(module,"D3D12CreateDevice"));
    if (!fn) return;
    static std::mutex lock;
    static std::vector<void*> targets;
    std::lock_guard<std::mutex> guard(lock);
    if (std::find(targets.begin(),targets.end(),fn)!=targets.end() || targets.size()>=4) return;
    const CreateDeviceFn detours[]={hk_capture_device<0>,hk_capture_device<1>,hk_capture_device<2>,hk_capture_device<3>};
    const auto slot=targets.size();
    auto st=MH_CreateHook(fn,as(detours[slot]),reinterpret_cast<void**>(&real_create_device[slot]));
    if (st==MH_OK) {
        st=MH_EnableHook(fn);
        if (st!=MH_OK) MH_RemoveHook(fn);
    }
    if (st==MH_OK) targets.push_back(fn);
    else log("[nr] HUD FIX root metadata: D3D12CreateDevice hook failed (%s)",MH_StatusToString(st));
}

namespace {
void store_backs(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d))) return;
    for (UINT i = 0; i < 8; ++i) {
        ID3D12Resource* b = nullptr;
        uint64_t v = 0;
        bool watched = false;
        if (i < d.BufferCount && SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&b))) && b) {
            v = reinterpret_cast<uint64_t>(b);
            if (g.backs[i].load() == v) watched = g.back_watched[i].load();
            else {
                ID3D12Device* dev = nullptr;
                if (SUCCEEDED(b->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
                    std::lock_guard<std::mutex> lk(g_watched_m);
                    watched = std::find(g_watched_devices.begin(), g_watched_devices.end(), dev) != g_watched_devices.end();
                    dev->Release();
                }
            }
            b->Release();
        }
        g.back_watched[i].store(watched);
        g.backs[i].store(v);
    }
}
}  // namespace

void d3d12_backs(IDXGISwapChain* sc) {
    if (sc && g.armed.load()) store_backs(sc);
}

Frame d3d12_present(ID3D12Device* device, IDXGISwapChain* sc, ID3D12CommandQueue* queue, ID3D12Resource* back,
                    DXGI_FORMAT format, uint32_t w, uint32_t h, bool armed, bool fg_armed, bool fg_list_copies) {
    TracePresent trace_scope;
    trace().add("present", 0, 0, reinterpret_cast<uint64_t>(sc), reinterpret_cast<uint64_t>(back),
        reinterpret_cast<uint64_t>(queue), (uint64_t(w)<<32)|h);
    Frame f;
    gf.list_copies.store(fg_list_copies);
    {   // frame generation's HUD-less frame: this frame's hand-over (since the last Present), and its copy made ready
        std::lock_guard<std::mutex> lk(gf.m);
        if (fg_armed && device) {
            bool own_back = false;   // dispatched from the back buffer itself: the layers' change is in it already
            if (gf.at == g.presents && !gf.hudless && sc) {
                DXGI_SWAP_CHAIN_DESC d{};
                if (gf.game == back) own_back = true;
                else if (SUCCEEDED(sc->GetDesc(&d)))
                    for (UINT i = 0; i < d.BufferCount && i < 8 && !own_back; ++i) {
                        ID3D12Resource* b = nullptr;
                        if (SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&b))) && b) { own_back = b == gf.game; b->Release(); }
                    }
            }
            if (gf.at == g.presents && !own_back) {
                f.fg.seen = true;
                f.fg.destination = gf.destination; f.fg.destination_state = gf.destination_state;
                f.fg.copy = gf.copy; f.fg.copied = gf.copied; f.fg.game = gf.game; f.fg.state = gf.state;
                f.fg.substitute = gf.substitute; f.fg.source = gf.source; f.fg.hudless = gf.hudless;
                // Both tags must belong to this same provider frame and CPU Present epoch.
                if (gf.hudless && gf.xess_full_frame && gf.xess_context && gf.ui.image &&
                    gf.ui.context == gf.xess_context && gf.ui.frame == gf.xess_frame && gf.ui.at == gf.at) {
                    f.fg.ui = gf.ui.image; f.fg.ui_state = gf.ui.state; f.fg.ui_keep = gf.ui.keep;
                    f.fg.ui_frame = gf.xess_frame;
                }
                if (!gf.hudless) f.fg.why = "the game hands its frame generation no HUD-less frame (it is dispatched from a frame of its own)";
            } else if (own_back) {
                f.fg.why = "the game's frame generation is dispatched from the back buffer itself";
            } else {
                f.fg.why = !gf.refused.empty() ? gf.refused : "the game hands its frame generation no HUD-less frame (none this frame)";
            }
            trace().add("fg-select",0,0,reinterpret_cast<uint64_t>(f.fg.game),f.fg.seen,f.fg.hudless,g.presents);
            gf.refused.clear();
            bool other_device = false;
            if (gf.copy) {
                ID3D12Device* of = nullptr;
                other_device = FAILED(gf.copy->GetDevice(IID_PPV_ARGS(&of))) || of != device;
                if (of) of->Release();
            }
            if (!gf.copy || other_device || gf.w != w || gf.h != h || gf.format != format) {
                if (gf.copy) { std::lock_guard<std::mutex> lk2(g.m); g.retired.push_back({gf.copy, g.presents}); gf.copy = nullptr; }
                D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC rd{};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1;
                rd.MipLevels = 1; rd.Format = format; rd.SampleDesc.Count = 1; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                           nullptr, IID_PPV_ARGS(&gf.copy)))) gf.copy = nullptr;
                gf.w = w; gf.h = h; gf.format = format;
                if (f.fg.seen) { f.fg.seen = false; f.fg.ui = nullptr; f.fg.ui_keep.reset(); f.fg.why = "its image is being made"; }
            }
        } else if (gf.armed.load()) {
            gf.at = ~0ull;
        }
        gf.ui = {}; // consumed or missed: never carry UI over an ESC/menu gap
        gf.ui_leases.erase(std::remove_if(gf.ui_leases.begin(), gf.ui_leases.end(),
            [](const auto& p) { return p.use_count() == 1 && p->ready(); }), gf.ui_leases.end());
        gf.armed.store(fg_armed && device);
        const uint64_t offered = gf.offered_ms.load();
        f.fg.offered = offered && GetTickCount64() - offered <= 2000;
    }
    // followed in every mode: NR Control greys the method from the start where no hand-over comes (cheap: the
    // libraries' entry points hooked once; while not armed a hand-over only notes its time)
    if (device) fg_hook_libraries();
    ++g.presents;
    if (armed && g.presents % 120 == 0) {
        log("[nr] HUD FIX input discovery (since last report): %llu final draws, %llu missing root layouts, "
            "%llu missing heaps, %llu SRV probes", (unsigned long long)g.input_draws.exchange(0),
            (unsigned long long)g.input_no_root.exchange(0), (unsigned long long)g.input_no_heap.exchange(0),
            (unsigned long long)g.input_probes.exchange(0));
        log("[nr] HUD FIX root metadata (session): %llu created, %llu saved, %llu failed",
            (unsigned long long)root_seen.load(),(unsigned long long)root_saved.load(),(unsigned long long)root_failed.load());
    }
    // earlier copies' images (g.retired) are kept: a list the game recorded with one may still run (it is not freed
    // without a proof that none can; each resize retires one or two)
    const bool was = g.armed.load();
    if (!armed) {
        if (was) {
            g.armed.store(false);
            std::lock_guard<std::mutex> lk(g.m);
            g.frame.clear();
        }
        return f;
    }
    if (!g.device_hooked) { g.late = true; hook_device(device); }
    if (!hook_lists(device, queue)) {
        f.result = Result::kMissed;
        f.why = "the game's command lists could not be followed (see the log)";
        f.broken = true;
        return f;
    }
    // the swap chain's back buffers (at DLSS-G's evaluate none in hand: those its Presents saw, d3d12_backs)
    if (sc) store_backs(sc);
    if (g.back_w.load() != w || g.back_h.load() != h || g.format != format) {   // the chain's copies go (a new size)
        std::lock_guard<std::mutex> lk(g.chain_m);
        for (State::ChainDst& e : g.chain_dst) {
            if (e.owner) chain_retire(std::move(e.owner));
            e = {};
        }
        for (uint64_t& k : g.chain_keys) k = 0;
        for (State::Check& c : g.checks) c = {};
        g.written.clear();   // (the old images' addresses: freed in the game's resize, maybe reused)
        g.refused.clear();
    }
    g.back_w.store(w);
    g.back_h.store(h);
    g.format = format;
    g.back_group.store(format_group(format));
    g.chain_stamp.store(g.presents);
    const bool image = ensure_hudless(device, format, w, h);
    if (!was) g.arm_epoch.fetch_add(1);
    g.armed.store(true);
    std::vector<Rec> recs;
    {
        std::lock_guard<std::mutex> lk(g.m);
        recs.swap(g.frame);
    }
    g.copies.store(0);
    const bool foreign = g.foreign.exchange(false);
    if (!was) {
        f.result = Result::kMissed;
        f.why = "it starts with the next frame";
        // the chain capture from this frame on as well (until 600 Presents show no frame it serves)
        g.chain.store(true);
        g.clean_at = g.presents;
        return f;
    }
    // An indirect compute dispatch while a back buffer was bound: no write on it when that back buffer never had a UAV
    // view (its device's every view creation followed; the views made up to now, after the lists' submission).
    {
        size_t kept = 0;
        for (size_t i = 0; i < recs.size(); ++i) {
            Rec& rec = recs[i];
            if (rec.kind == kUnknown && rec.how == kWhyComputeView) {
                bool watched = false;
                for (int k = 0; k < 8; ++k)
                    if (g.backs[k].load() == rec.back) watched = g.back_watched[k].load();
                if (nr::hudcap::compute_cannot_write_by_views(watched, g_uav_views.maybe(rec.back))) { ++indirect_skipped; ++indirect_by_view; continue; }
                if (watched) ++indirect_view_seen; else ++indirect_unwatched;
                rec.how = kWhyComputeUav;
            }
            if (kept != i) recs[kept] = std::move(rec);
            ++kept;
        }
        recs.resize(kept);
    }
    // Count only records submitted for the actual presented buffer, not unrelated recordings.
    uint32_t rejected[7]={}; uint32_t reasons=0;
    for (const auto& rec:recs) if (rec.kind==kUnknown && rec.back==reinterpret_cast<uint64_t>(back)) {
        const auto reason=rec.how<7 ? rec.how : 0;
        ++rejected[reason]; reasons|=1u<<reason;
    }
    static uint32_t last_reasons=0;
    if (reasons && (reasons!=last_reasons || g.presents%120==0))
        log("[nr] HUD FIX Captured rejected writes: unknown PSO %u, unknown indirect signature %u, indirect draw %u, "
            "indirect compute on UAV-capable target %u, bundle %u, other %u; signatures %llu/%llu; "
            "non-writing indirect observations %llu (session); indirect compute on the back buffer (session): no write "
            "proven by its state %llu, by no UAV view %llu; not proven: a UAV view of it was made %llu, its device's views "
            "not all followed %llu", rejected[1],rejected[2],rejected[3],rejected[4],rejected[5],rejected[0],
            (unsigned long long)signature_saved.load(),(unsigned long long)signature_seen.load(),(unsigned long long)indirect_skipped.load(),
            (unsigned long long)indirect_by_state.load(),(unsigned long long)indirect_by_view.load(),
            (unsigned long long)indirect_view_seen.load(),(unsigned long long)indirect_unwatched.load());
    if (!reasons && (last_reasons || g.presents%120==0))
        log("[nr] HUD FIX Captured: no rejected writes this frame; signatures %llu/%llu; non-writing indirect observations %llu "
            "(session; on the back buffer by its state %llu, by no UAV view %llu)",
            (unsigned long long)signature_saved.load(),(unsigned long long)signature_seen.load(),(unsigned long long)indirect_skipped.load(),
            (unsigned long long)indirect_by_state.load(),(unsigned long long)indirect_by_view.load());
    last_reasons=reasons;
    if (g.presents%120==0) {   // a pool that only grows: copies whose use never finishes (ChainSnapshot::done)
        size_t waiting = 0, spare = 0;
        {
            std::lock_guard<std::mutex> lk(g.chain_m);
            (void)chain_prune(g.chain_stamp.load(), false);
            for (const auto& r : g.chain_retired) (r.use_count() == 1 && r->ready() ? spare : waiting)++;
        }
        log("[nr] HUD FIX chain snapshots: %llu made (session), %zu set aside until the GPU is done with them, %zu spare",
            (unsigned long long)g_chain_made.load(), waiting, spare);
    }
    const nr::hudcap::Verdict v = nr::hudcap::verdict(recs, reinterpret_cast<uint64_t>(back), foreign,
                                                      image ? reinterpret_cast<uint64_t>(g.hudless.load()) : 0, f.why);
    f.result = v == nr::hudcap::Verdict::kCaptured ? Result::kCaptured
               : v == nr::hudcap::Verdict::kClean ? Result::kClean : Result::kMissed;
    if (f.result == Result::kCaptured) f.hudless = g.hudless.load();
    // The chain capture: armed while the back buffer's last write is a scene write (its copies from the next frame on);
    // off again after 10 s without such a frame. Its pick (nr_hud_capture.hpp chain_picks) is this frame's HUD-less copy.
    g.captured_run = v == nr::hudcap::Verdict::kCaptured ? g.captured_run + 1 : 0;
    if (v == nr::hudcap::Verdict::kClean) {
        g.clean_at = g.presents;
        g.chain.store(true);
        if (!g.chain_said) {
            g.chain_said = true;
            log("[nr] HUD FIX chain: the game draws nothing over its back buffer after the final pass; the images of its "
                "size are followed for the frame before its HUD");
        }
        {
            std::string cwhy;
            // the copies before the final pass: a trusted image's (checked against the presented frame before), else the
            // last one made where an image's writes ended in a scene write, else the final pass's first input (a refused
            // image is not copied for a while)
            const std::vector<nr::hudcap::ChainCopy> picks = nr::hudcap::chain_picks(recs, reinterpret_cast<uint64_t>(back), cwhy);
            nr::hudcap::ChainCopy pick{};
            {
                std::lock_guard<std::mutex> lk(g.chain_m);
                for (const nr::hudcap::ChainCopy& c : picks)
                    for (const State::Check& k : g.checks)
                        if (k.trusted && k.image == c.image) pick = c;
                if (!pick.dst)
                    for (const nr::hudcap::ChainCopy& c : picks)
                        if (c.how == 0) pick = c;
                if (!pick.dst)
                    for (const nr::hudcap::ChainCopy& c : picks)
                        if (c.how == 1) { pick = c; break; }
            }
            trace().add("pick", 0, 0, pick.image, pick.dst, pick.how, picks.size());
            const uint8_t how = pick.how;
            {   // the diagnostic line, on a change (40 at most): what the final pass reads, the copies, the pick
                std::string d;
                char b[160];
                {
                    std::lock_guard<std::mutex> lk(g.diag_m);
                    d = "the final pass reads " + std::to_string(g.final_inputs.size()) + " image(s) of the back buffer's size";
                    for (const auto& x : g.final_inputs) { std::snprintf(b, sizeof b, " %p (format %u)", reinterpret_cast<void*>(x.first), x.second); d += b; }
                    g.final_inputs.clear();
                }
                std::snprintf(b, sizeof b, "; copies: %u at images' ends / before overlays, %u of the final pass's inputs; ",
                              g.copies_write.exchange(0), g.copies_input.exchange(0));
                d += b;
                if (pick.dst) { std::snprintf(b, sizeof b, "picked %p (%s)", reinterpret_cast<void*>(pick.image), how ? "an input of the final pass" : "its end of writes / before an overlay"); d += b; }
                else d += "no pick: " + cwhy;
                if (d != g.said_diag && g.diag_lines < 40) { ++g.diag_lines; log("[nr] HUD FIX chain: %s", d.c_str()); }
                g.said_diag = d;
            }
            std::lock_guard<std::mutex> lk(g.chain_m);
            if (pick.dst) {
                f.result = Result::kCaptured;
                f.hudless = reinterpret_cast<ID3D12Resource*>(pick.dst);
                f.chain = true;
                f.chain_image = pick.image;
                f.chain_keep = pick.keep;
                for (const State::Check& c : g.checks) f.chain_trusted = f.chain_trusted || (c.image == pick.image && c.trusted);
                // (NR_TEST_CHAIN_TRUST=1, a test hook: every pick trusted at once - no check)
                static const bool trust_all = [] { const char* e = std::getenv("NR_TEST_CHAIN_TRUST"); return e && e[0] == '1'; }();
                if (trust_all) f.chain_trusted = true;
                f.why.clear();
                g.misses = 0;
                bool keyed = false;   // the picked images (four at most: a game may alternate two or three)
                for (uint64_t k : g.chain_keys) keyed = keyed || k == pick.image;
                if (!keyed) {
                    for (int i = 3; i > 0; --i) g.chain_keys[i] = g.chain_keys[i - 1];
                    g.chain_keys[0] = pick.image;
                }
            } else {
                f.why = "the final pass's input: " + cwhy;
                // A missing source remains visible; do not hold a previous frame's snapshot to hide a miss.
                // Candidate admission is always open, independently of these recent diagnostic addresses.
                ++g.misses;
                bool had_keys = false;
                for (uint64_t& k : g.chain_keys) { had_keys = had_keys || k != 0; k = 0; }
                if (had_keys && g.miss_reports++ < 8)
                    log("[nr] HUD FIX chain: the selected source has no usable copy (%s); search stays open "
                        "without restricting subsequent candidates", cwhy.c_str());
            }
        }
    } else if (g.chain.load() && (g.presents - g.clean_at > 600 || g.captured_run >= 60)) {
        g.chain.store(false);
        if (g.chain_said) log("[nr] HUD FIX chain: the game draws over its back buffer after its final pass again; the chain is not followed");
        g.chain_said = false;
    }
    if (g.late && f.result != Result::kCaptured)
        f.why += " (HUD FIX was switched on after the game made its pipelines and views: restart the game with it on)";
    return f;
}


// ---- D3D11 ----------------------------------------------------------------------------------------------------------
namespace {

struct State11 {
    std::atomic<bool> armed{false};
    ID3D11DeviceContext* imm = nullptr;
    uint64_t back_buffer = 0;   // the swap chain's buffer
    uint32_t w = 0, h = 0;
    ID3D11Texture2D* hudless = nullptr;
    bool hooked = false;
    // the immediate context's bindings and this frame's writes (the context's thread)
    uint64_t back = 0;
    bool dsv = false;
    ID3D11DepthStencilState* ds = nullptr;
    ID3D11BlendState* bs = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    D3D11_VIEWPORT vp{};
    D3D11_RECT sc{};
    uint8_t last = 0;
    std::vector<Rec> recs;
    uint32_t places = 0, last_place = 0;
    bool hooks_ok = false;
    void unbind() { back = 0; dsv = false; ds = nullptr; bs = nullptr; rs = nullptr; vp = {}; sc = {}; }
} g11;

bool on11(ID3D11DeviceContext* ctx) { return g11.armed.load(std::memory_order_relaxed) && !t_own && ctx == g11.imm; }

void write11(ID3D11DeviceContext* ctx, uint8_t kind) {
    if (kind == kOverlay && g11.last == kScene) {
        const uint32_t place = g11.places++;
        if (place >= g11.last_place && g11.hudless) {   // the last frame's last place: one copy a frame
            ++t_own;
            ctx->CopyResource(g11.hudless, reinterpret_cast<ID3D11Resource*>(g11.back_buffer));
            --t_own;
            g11.recs.push_back({kCopy, g11.back_buffer, reinterpret_cast<uint64_t>(g11.hudless)});
        }
    }
    if (g11.recs.size() >= nr::hudcap::kMaxRecs) {
        g11.recs.clear();
        g11.recs.push_back({kLost, g11.back_buffer});
    }
    g11.recs.push_back({kind, g11.back_buffer});
    g11.last = kind;
}

uint8_t draw_kind11(uint32_t vertices, uint32_t instances, bool indirect) {
    nr::hudcap::Draw d;
    d.dsv = g11.dsv;
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;   // a null state: the default (depth on)
    if (g11.ds) g11.ds->GetDesc(&dd);
    d.depth = dd.DepthEnable ? 1 : 0;
    D3D11_BLEND_DESC bd{};
    if (g11.bs) g11.bs->GetDesc(&bd);
    d.blend = bd.RenderTarget[0].BlendEnable ? 1 : 0;
    D3D11_RASTERIZER_DESC rd{};
    if (g11.rs) g11.rs->GetDesc(&rd);
    d.scissor_on = rd.ScissorEnable != FALSE;
    d.vp[0] = g11.vp.TopLeftX; d.vp[1] = g11.vp.TopLeftY; d.vp[2] = g11.vp.Width; d.vp[3] = g11.vp.Height;
    d.sc[0] = g11.sc.left; d.sc[1] = g11.sc.top; d.sc[2] = g11.sc.right; d.sc[3] = g11.sc.bottom;
    d.vertices = vertices; d.instances = instances; d.indirect = indirect;
    return nr::hudcap::draw_kind(d, g11.w, g11.h);
}
void draw11(ID3D11DeviceContext* ctx, uint32_t vertices, uint32_t instances, bool indirect) {
    if (on11(ctx) && g11.back) write11(ctx, draw_kind11(vertices, instances, indirect));
}
bool is_back11(ID3D11Resource* r) { return r && reinterpret_cast<uint64_t>(r) == g11.back_buffer; }
uint64_t back_of(ID3D11RenderTargetView* v) {
    if (!v) return 0;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return 0;
    const uint64_t at = reinterpret_cast<uint64_t>(r);
    r->Release();
    return at == g11.back_buffer ? at : 0;
}

using PFN11_DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN11_Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN11_DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN11_DrawInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN11_SetTargets = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using PFN11_SetTargetsUav = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
                                                     UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using PFN11_Blend = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT*, UINT);
using PFN11_Depth = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
using PFN11_DrawAuto = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using PFN11_DrawIndirect = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PFN11_Raster = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RasterizerState*);
using PFN11_Viewports = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using PFN11_Scissors = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
using PFN11_CopyRegion = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT,
                                                  const D3D11_BOX*);
using PFN11_Copy = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using PFN11_Update = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using PFN11_Clear = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*);
using PFN11_Resolve = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
using PFN11_Execute = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using PFN11_ClearState = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);

PFN11_DrawIndexed r11_draw_indexed;
PFN11_Draw r11_draw;
PFN11_DrawIndexedInstanced r11_draw_indexed_instanced;
PFN11_DrawInstanced r11_draw_instanced;
PFN11_SetTargets r11_set_targets;
PFN11_SetTargetsUav r11_set_targets_uav;
PFN11_Blend r11_blend;
PFN11_Depth r11_depth;
PFN11_DrawAuto r11_draw_auto;
PFN11_DrawIndirect r11_draw_indexed_indirect, r11_draw_indirect;
PFN11_Raster r11_raster;
PFN11_Viewports r11_viewports;
PFN11_Scissors r11_scissors;
PFN11_CopyRegion r11_copy_region;
PFN11_Copy r11_copy;
PFN11_Update r11_update;
PFN11_Clear r11_clear;
PFN11_Resolve r11_resolve;
PFN11_Execute r11_execute;
PFN11_ClearState r11_clear_state;

void STDMETHODCALLTYPE k11_draw_indexed(ID3D11DeviceContext* c, UINT n, UINT s, INT b) { draw11(c, n, 1, false); r11_draw_indexed(c, n, s, b); }
void STDMETHODCALLTYPE k11_draw(ID3D11DeviceContext* c, UINT n, UINT s) { draw11(c, n, 1, false); r11_draw(c, n, s); }
void STDMETHODCALLTYPE k11_draw_indexed_instanced(ID3D11DeviceContext* c, UINT n, UINT i, UINT s, INT b, UINT si) {
    draw11(c, n, i, false);
    r11_draw_indexed_instanced(c, n, i, s, b, si);
}
void STDMETHODCALLTYPE k11_draw_instanced(ID3D11DeviceContext* c, UINT n, UINT i, UINT s, UINT si) {
    draw11(c, n, i, false);
    r11_draw_instanced(c, n, i, s, si);
}
void STDMETHODCALLTYPE k11_draw_auto(ID3D11DeviceContext* c) { draw11(c, 0, 0, true); r11_draw_auto(c); }
void STDMETHODCALLTYPE k11_draw_indexed_indirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
    draw11(c, 0, 0, true);
    r11_draw_indexed_indirect(c, b, o);
}
void STDMETHODCALLTYPE k11_draw_indirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
    draw11(c, 0, 0, true);
    r11_draw_indirect(c, b, o);
}
void targets11(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!on11(c)) return;
    g11.back = 0;
    g11.dsv = dsv != nullptr;
    for (UINT i = 0; rtvs && i < n && i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (const uint64_t b = back_of(rtvs[i])) { g11.back = b; break; }
}
void STDMETHODCALLTYPE k11_set_targets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    targets11(c, n, rtvs, dsv);
    r11_set_targets(c, n, rtvs, dsv);
}
void STDMETHODCALLTYPE k11_set_targets_uav(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                                           UINT us, UINT un, ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) targets11(c, n, rtvs, dsv);
    r11_set_targets_uav(c, n, rtvs, dsv, us, un, uavs, counts);
}
void STDMETHODCALLTYPE k11_blend(ID3D11DeviceContext* c, ID3D11BlendState* s, const FLOAT* f, UINT m) {
    if (on11(c)) g11.bs = s;
    r11_blend(c, s, f, m);
}
void STDMETHODCALLTYPE k11_depth(ID3D11DeviceContext* c, ID3D11DepthStencilState* s, UINT r) {
    if (on11(c)) g11.ds = s;
    r11_depth(c, s, r);
}
void STDMETHODCALLTYPE k11_raster(ID3D11DeviceContext* c, ID3D11RasterizerState* s) {
    if (on11(c)) g11.rs = s;
    r11_raster(c, s);
}
void STDMETHODCALLTYPE k11_viewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
    if (on11(c) && n && v) g11.vp = v[0];
    r11_viewports(c, n, v);
}
void STDMETHODCALLTYPE k11_scissors(ID3D11DeviceContext* c, UINT n, const D3D11_RECT* r) {
    if (on11(c) && n && r) g11.sc = r[0];
    r11_scissors(c, n, r);
}
bool whole11(UINT x, UINT y, ID3D11Resource* src, const D3D11_BOX* box) {
    if (x || y) return false;
    if (box) return box->right - box->left >= g11.w && box->bottom - box->top >= g11.h;
    ID3D11Texture2D* t = nullptr;
    if (!src || FAILED(src->QueryInterface(IID_PPV_ARGS(&t))) || !t) return false;
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    t->Release();
    return d.Width >= g11.w && d.Height >= g11.h;
}
void STDMETHODCALLTYPE k11_copy_region(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src,
                                       UINT ss, const D3D11_BOX* box) {
    if (on11(c) && is_back11(dst)) write11(c, whole11(x, y, src, box) ? kScene : kOverlay);
    r11_copy_region(c, dst, ds, x, y, z, src, ss, box);
}
void STDMETHODCALLTYPE k11_copy(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (on11(c) && is_back11(dst)) write11(c, kScene);
    r11_copy(c, dst, src);
}
void STDMETHODCALLTYPE k11_update(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void* data,
                                  UINT pitch, UINT depth) {
    if (on11(c) && is_back11(dst))
        write11(c, !box || (box->left == 0 && box->top == 0 && box->right >= g11.w && box->bottom >= g11.h) ? kScene : kOverlay);
    r11_update(c, dst, sub, box, data, pitch, depth);
}
void STDMETHODCALLTYPE k11_clear(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT* colour) {
    if (on11(c) && back_of(v)) write11(c, kScene);
    r11_clear(c, v, colour);
}
void STDMETHODCALLTYPE k11_resolve(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, ID3D11Resource* src, UINT ss, DXGI_FORMAT f) {
    if (on11(c) && is_back11(dst)) write11(c, kScene);
    r11_resolve(c, dst, ds, src, ss, f);
}
void STDMETHODCALLTYPE k11_execute(ID3D11DeviceContext* c, ID3D11CommandList* l, BOOL restore) {
    r11_execute(c, l, restore);
    if (on11(c)) {
        // a deferred context's work: its writes on the back buffer are not seen - after it the frame is not trusted
        g11.recs.push_back({kUnknown, g11.back_buffer});
        g11.last = kOverlay;
        if (!restore) g11.unbind();   // the context's state cleared after it
    }
}
void STDMETHODCALLTYPE k11_clear_state(ID3D11DeviceContext* c) {
    r11_clear_state(c);
    if (on11(c)) g11.unbind();
}

bool hook11(ID3D11DeviceContext* ctx) {
    if (g11.hooked) return g11.hooks_ok;
    g11.hooked = true;
    MH_Initialize();
    const struct { int slot; void* detour; void** real; const char* name; } hooks[] = {
        {12, as(&k11_draw_indexed), reinterpret_cast<void**>(&r11_draw_indexed), "DrawIndexed"},
        {13, as(&k11_draw), reinterpret_cast<void**>(&r11_draw), "Draw"},
        {20, as(&k11_draw_indexed_instanced), reinterpret_cast<void**>(&r11_draw_indexed_instanced), "DrawIndexedInstanced"},
        {21, as(&k11_draw_instanced), reinterpret_cast<void**>(&r11_draw_instanced), "DrawInstanced"},
        {33, as(&k11_set_targets), reinterpret_cast<void**>(&r11_set_targets), "OMSetRenderTargets"},
        {34, as(&k11_set_targets_uav), reinterpret_cast<void**>(&r11_set_targets_uav), "OMSetRenderTargetsAndUnorderedAccessViews"},
        {35, as(&k11_blend), reinterpret_cast<void**>(&r11_blend), "OMSetBlendState"},
        {36, as(&k11_depth), reinterpret_cast<void**>(&r11_depth), "OMSetDepthStencilState"},
        {38, as(&k11_draw_auto), reinterpret_cast<void**>(&r11_draw_auto), "DrawAuto"},
        {39, as(&k11_draw_indexed_indirect), reinterpret_cast<void**>(&r11_draw_indexed_indirect), "DrawIndexedInstancedIndirect"},
        {40, as(&k11_draw_indirect), reinterpret_cast<void**>(&r11_draw_indirect), "DrawInstancedIndirect"},
        {43, as(&k11_raster), reinterpret_cast<void**>(&r11_raster), "RSSetState"},
        {44, as(&k11_viewports), reinterpret_cast<void**>(&r11_viewports), "RSSetViewports"},
        {45, as(&k11_scissors), reinterpret_cast<void**>(&r11_scissors), "RSSetScissorRects"},
        {46, as(&k11_copy_region), reinterpret_cast<void**>(&r11_copy_region), "CopySubresourceRegion"},
        {47, as(&k11_copy), reinterpret_cast<void**>(&r11_copy), "CopyResource"},
        {48, as(&k11_update), reinterpret_cast<void**>(&r11_update), "UpdateSubresource"},
        {50, as(&k11_clear), reinterpret_cast<void**>(&r11_clear), "ClearRenderTargetView"},
        {57, as(&k11_resolve), reinterpret_cast<void**>(&r11_resolve), "ResolveSubresource"},
        {58, as(&k11_execute), reinterpret_cast<void**>(&r11_execute), "ExecuteCommandList"},
        {110, as(&k11_clear_state), reinterpret_cast<void**>(&r11_clear_state), "ClearState"},
    };
    bool ok = true;
    for (const auto& k : hooks) ok = hook(ctx, k.slot, k.detour, k.real, k.name) && ok;
    log("[nr] HUD FIX capture (D3D11): the immediate context is followed%s", ok ? "" : " - NOT in full (above): it does not run");
    g11.hooks_ok = ok;
    return ok;
}

}  // namespace

Frame11 d3d11_present(ID3D11Device* device, ID3D11Texture2D* back, DXGI_FORMAT format, uint32_t w, uint32_t h, bool armed) {
    Frame11 f;
    const bool was = g11.armed.load();
    if (!armed || !device || !back) {
        if (was) { g11.armed.store(false); g11.recs.clear(); g11.last = 0; g11.places = 0; }
        return f;
    }
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (!ctx) { f.result = Result::kMissed; f.why = "the game's context was not found"; return f; }
    ctx->Release();   // the device holds it
    if (g11.imm != ctx) {   // another device: its own copy image
        g11.imm = ctx;
        g11.unbind();
        if (g11.hudless) { g11.hudless->Release(); g11.hudless = nullptr; }
    }
    if (!hook11(ctx)) {
        f.result = Result::kMissed;
        f.why = "the game's D3D11 context could not be followed (see the log)";
        f.broken = true;
        return f;
    }
    g11.back_buffer = reinterpret_cast<uint64_t>(static_cast<ID3D11Resource*>(back));
    g11.w = w; g11.h = h;
    bool image = false;
    if (g11.hudless) {
        D3D11_TEXTURE2D_DESC d{};
        g11.hudless->GetDesc(&d);
        if (d.Width == w && d.Height == h && d.Format == format) image = true;
        else { g11.hudless->Release(); g11.hudless = nullptr; }   // D3D11 keeps it while the GPU uses it
    }
    if (!image) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = format; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        image = SUCCEEDED(device->CreateTexture2D(&d, nullptr, &g11.hudless)) && g11.hudless;
        if (!image) { g11.hudless = nullptr; log("[nr] HUD FIX capture (D3D11): its image (format %u) could not be made", unsigned(format)); }
    }
    g11.armed.store(true);
    std::vector<Rec> recs;
    recs.swap(g11.recs);
    g11.last = 0;
    g11.last_place = g11.places ? g11.places - 1 : 0;
    g11.places = 0;
    if (!was) { f.result = Result::kMissed; f.why = "it starts with the next frame"; return f; }
    const nr::hudcap::Verdict v =
        nr::hudcap::verdict(recs, g11.back_buffer, false, image ? reinterpret_cast<uint64_t>(g11.hudless) : 0, f.why);
    f.result = v == nr::hudcap::Verdict::kCaptured ? Result::kCaptured
               : v == nr::hudcap::Verdict::kClean ? Result::kClean : Result::kMissed;
    if (f.result == Result::kCaptured) f.hudless = g11.hudless;
    return f;
}


// ---- Vulkan ---------------------------------------------------------------------------------------------------------
// Every hooked function twice where the Vulkan loader hands two addresses (its export, and what its vkGetDeviceProcAddr gives
// when that is another one), as the .asi's queue hooks: a detour instance each, its own original.
namespace {

struct RpInfo {   // a render pass: its first subpass's colour attachments, each one's load op and initial layout
    std::vector<uint32_t> colour;
    std::vector<VkAttachmentLoadOp> load;
    std::vector<VkImageLayout> initial;
    bool depth = false;
};
struct FbInfo {
    VkRenderPass rp = VK_NULL_HANDLE;
    std::vector<VkImageView> views;   // empty: imageless (the views come with vkCmdBeginRenderPass)
};
struct VkPipeFacts {
    int8_t depth = -1;   // -2: dynamic (vkCmdSetDepthTestEnable)
    int8_t blend = -1;
    bool dyn_vp = true, dyn_sc = true;
    VkViewport vp{};
    VkRect2D sc{};
};
struct VkList {
    uint64_t back = 0;
    bool dsv = false, in_pass = false;
    VkPipeline pipe = VK_NULL_HANDLE;
    int8_t dyn_depth = -1;
    VkViewport vp{};
    VkRect2D sc{};
    uint8_t last = 0;
    std::vector<Rec> recs;
    void reset() { back = 0; dsv = in_pass = false; pipe = VK_NULL_HANDLE; dyn_depth = -1; vp = {}; sc = {}; last = 0; recs.clear(); }
};
struct StateVk {
    std::atomic<bool> armed{false};
    std::atomic<VkDevice> device{VK_NULL_HANDLE};
    std::atomic<uint64_t> backs[16];
    std::atomic<uint32_t> w{0}, h{0};
    std::atomic<uint64_t> hudless{0};
    std::atomic<uint32_t> copies{0};
    std::atomic<bool> foreign{false};   // a secondary command buffer or a next subpass on a back buffer: not followed
    bool created_hooked = false, cmds_hooked = false;
    std::shared_mutex m_obj;   // views, framebuffers, render passes, pipelines
    std::unordered_map<uint64_t, uint64_t> views;   // view -> image
    std::unordered_map<uint64_t, VkImageUsageFlags> usage;   // image -> its usage
    std::unordered_set<uint64_t> reordered;                  // views whose channels are not in their own order
    std::unordered_map<uint64_t, FbInfo> fbs;
    std::unordered_map<uint64_t, RpInfo> rps;
    std::unordered_map<uint64_t, VkPipeFacts> pipes;
    std::mutex m;   // lists' map, frame
    std::unordered_map<VkCommandBuffer, VkList> lists;   // never erased
    std::vector<Rec> frame;
} gv;

bool is_back_vk(uint64_t img) {
    if (!img) return false;
    for (const auto& b : gv.backs)
        if (b.load(std::memory_order_relaxed) == img) return true;
    return false;
}
VkList* vlist(VkCommandBuffer cb) {
    thread_local VkCommandBuffer last_cb = VK_NULL_HANDLE;
    thread_local VkList* last_state = nullptr;
    if (cb == last_cb) return last_state;
    std::lock_guard<std::mutex> lk(gv.m);
    VkList* s = &gv.lists[cb];
    last_cb = cb;
    last_state = s;
    return s;
}
bool on_vk() { return gv.armed.load(std::memory_order_relaxed) && !t_own; }
uint64_t image_of_view(VkImageView v) {
    std::shared_lock<std::shared_mutex> lk(gv.m_obj);
    const auto it = gv.views.find(reinterpret_cast<uint64_t>(v));
    return it == gv.views.end() ? 0 : it->second;
}

void vk_write(VkCommandBuffer cb, VkList& s, uint64_t b, uint8_t kind) {
    if (s.recs.size() >= nr::hudcap::kMaxRecs) {
        s.recs.clear();
        s.recs.push_back({kLost, b});
    }
    s.recs.push_back({kind, b});
    s.last = kind;
    (void)cb;
}
// The copy before a render pass on back buffer `b`, which is in `layout` then (outside any pass).
void vk_copy_before(VkCommandBuffer cb, VkList& s, uint64_t b, VkImageLayout layout) {
    const VkImage hl = reinterpret_cast<VkImage>(gv.hudless.load(std::memory_order_acquire));
    if (!hl || layout == VK_IMAGE_LAYOUT_UNDEFINED || s.last == kOverlay ||
        gv.copies.fetch_add(1, std::memory_order_relaxed) >= nr::hudcap::kMaxCopies)
        return;
    ++t_own;
    const VkImage back = reinterpret_cast<VkImage>(b);
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier in[2]{};
    for (auto& x : in) { x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; x.subresourceRange = all; }
    in[0].image = back; in[0].oldLayout = layout; in[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    in[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; in[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    in[1].image = hl; in[1].oldLayout = in[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    in[1].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; in[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, in);
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {gv.w.load(), gv.h.load(), 1};
    vkCmdCopyImage(cb, back, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hl, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    VkImageMemoryBarrier out[2] = {in[0], in[1]};
    out[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; out[0].newLayout = layout;
    out[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; out[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    out[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; out[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, out);
    --t_own;
    s.recs.push_back({kCopy, b, reinterpret_cast<uint64_t>(hl)});
}
uint8_t vk_draw_kind(const VkList& s, uint32_t vertices, uint32_t instances, bool indirect) {
    VkPipeFacts f;
    {
        std::shared_lock<std::shared_mutex> lk(gv.m_obj);
        const auto it = gv.pipes.find(reinterpret_cast<uint64_t>(s.pipe));
        if (it != gv.pipes.end()) f = it->second;
    }
    nr::hudcap::Draw d;
    d.dsv = s.dsv;
    d.depth = f.depth == -2 ? s.dyn_depth : f.depth;
    d.blend = f.blend;
    d.scissor_on = true;   // Vulkan always cuts by its scissor
    VkViewport vp = f.dyn_vp ? s.vp : f.vp;
    if (vp.height < 0) { vp.y += vp.height; vp.height = -vp.height; }   // a flipped viewport
    const VkRect2D sc = f.dyn_sc ? s.sc : f.sc;
    d.vp[0] = vp.x; d.vp[1] = vp.y; d.vp[2] = vp.width; d.vp[3] = vp.height;
    d.sc[0] = sc.offset.x; d.sc[1] = sc.offset.y; d.sc[2] = sc.offset.x + int32_t(sc.extent.width);
    d.sc[3] = sc.offset.y + int32_t(sc.extent.height);
    d.vertices = vertices; d.instances = instances; d.indirect = indirect;
    return nr::hudcap::draw_kind(d, gv.w.load(std::memory_order_relaxed), gv.h.load(std::memory_order_relaxed));
}
void vk_draw(VkCommandBuffer cb, uint32_t vertices, uint32_t instances, bool indirect) {
    if (!on_vk()) return;
    VkList* s = vlist(cb);
    if (s->back) vk_write(cb, *s, s->back, vk_draw_kind(*s, vertices, instances, indirect));
}
// A transfer into back buffer `img` covering `x, y, w x h` of it: a scene write when whole.
void vk_transfer(VkCommandBuffer cb, VkImage img, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (!on_vk() || !is_back_vk(reinterpret_cast<uint64_t>(img))) return;
    const bool whole = x == 0 && y == 0 && w >= gv.w.load() && h >= gv.h.load();
    VkList* s = vlist(cb);
    vk_write(cb, *s, reinterpret_cast<uint64_t>(img), whole ? kScene : kOverlay);
}

// creation
PFN_vkCreateImageView rv_create_view[2];
PFN_vkCreateFramebuffer rv_create_fb[2];
PFN_vkCreateRenderPass rv_create_rp[2];
PFN_vkCreateRenderPass2 rv_create_rp2[2], rv_create_rp2_khr[2];
PFN_vkCreateGraphicsPipelines rv_create_pipes[2];
PFN_vkCreateImage rv_create_image[2];
template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_create_view(VkDevice d, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a,
                                                             VkImageView* out) {
    const VkResult r = rv_create_view[I](d, ci, a, out);
    if (r == VK_SUCCESS && ci && out && d == gv.device.load()) {
        const auto own = [](VkComponentSwizzle s, VkComponentSwizzle self) { return s == VK_COMPONENT_SWIZZLE_IDENTITY || s == self; };
        const bool reordered = !own(ci->components.r, VK_COMPONENT_SWIZZLE_R) || !own(ci->components.g, VK_COMPONENT_SWIZZLE_G) ||
                               !own(ci->components.b, VK_COMPONENT_SWIZZLE_B) || !own(ci->components.a, VK_COMPONENT_SWIZZLE_A);
        const uint64_t v = reinterpret_cast<uint64_t>(*out);
        std::unique_lock<std::shared_mutex> lk(gv.m_obj);
        gv.views[v] = reinterpret_cast<uint64_t>(ci->image);
        if (reordered) gv.reordered.insert(v); else gv.reordered.erase(v);
    }
    return r;
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_create_image(VkDevice d, const VkImageCreateInfo* ci, const VkAllocationCallbacks* a,
                                                              VkImage* out) {
    const VkResult r = rv_create_image[I](d, ci, a, out);
    if (r == VK_SUCCESS && ci && out && d == gv.device.load()) {
        std::unique_lock<std::shared_mutex> lk(gv.m_obj);
        gv.usage[reinterpret_cast<uint64_t>(*out)] = ci->usage;   // a handle made again is written again
    }
    return r;
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_create_fb(VkDevice d, const VkFramebufferCreateInfo* ci, const VkAllocationCallbacks* a,
                                                           VkFramebuffer* out) {
    const VkResult r = rv_create_fb[I](d, ci, a, out);
    if (r == VK_SUCCESS && ci && out && d == gv.device.load()) {
        FbInfo f;
        f.rp = ci->renderPass;
        if (!(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && ci->pAttachments)
            f.views.assign(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
        std::unique_lock<std::shared_mutex> lk(gv.m_obj);
        gv.fbs[reinterpret_cast<uint64_t>(*out)] = std::move(f);
    }
    return r;
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_create_rp(VkDevice d, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks* a,
                                                           VkRenderPass* out) {
    const VkResult r = rv_create_rp[I](d, ci, a, out);
    if (r == VK_SUCCESS && ci && out && d == gv.device.load()) {
        RpInfo p;
        for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
            p.load.push_back(ci->pAttachments[i].loadOp);
            p.initial.push_back(ci->pAttachments[i].initialLayout);
        }
        if (ci->subpassCount) {
            const VkSubpassDescription& sp = ci->pSubpasses[0];
            for (uint32_t i = 0; i < sp.colorAttachmentCount; ++i) p.colour.push_back(sp.pColorAttachments[i].attachment);
            p.depth = sp.pDepthStencilAttachment && sp.pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED;
        }
        std::unique_lock<std::shared_mutex> lk(gv.m_obj);
        gv.rps[reinterpret_cast<uint64_t>(*out)] = std::move(p);
    }
    return r;
}
void remember_rp2(VkDevice d, const VkRenderPassCreateInfo2* ci, VkRenderPass* out) {
    if (!ci || !out || d != gv.device.load()) return;
    RpInfo p;
    for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
        p.load.push_back(ci->pAttachments[i].loadOp);
        p.initial.push_back(ci->pAttachments[i].initialLayout);
    }
    if (ci->subpassCount) {
        const VkSubpassDescription2& sp = ci->pSubpasses[0];
        for (uint32_t i = 0; i < sp.colorAttachmentCount; ++i) p.colour.push_back(sp.pColorAttachments[i].attachment);
        p.depth = sp.pDepthStencilAttachment && sp.pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED;
    }
    std::unique_lock<std::shared_mutex> lk(gv.m_obj);
    gv.rps[reinterpret_cast<uint64_t>(*out)] = std::move(p);
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_create_rp2(VkDevice d, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a,
                                                            VkRenderPass* out) {
    const VkResult r = rv_create_rp2[I](d, ci, a, out);
    if (r == VK_SUCCESS) remember_rp2(d, ci, out);
    return r;
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_create_rp2_khr(VkDevice d, const VkRenderPassCreateInfo2* ci,
                                                                const VkAllocationCallbacks* a, VkRenderPass* out) {
    const VkResult r = rv_create_rp2_khr[I](d, ci, a, out);
    if (r == VK_SUCCESS) remember_rp2(d, ci, out);
    return r;
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_create_pipes(VkDevice d, VkPipelineCache cache, uint32_t n,
                                                              const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks* a,
                                                              VkPipeline* out) {
    const VkResult r = rv_create_pipes[I](d, cache, n, ci, a, out);
    if ((r == VK_SUCCESS || r == VK_PIPELINE_COMPILE_REQUIRED) && ci && out && d == gv.device.load()) {
        std::unique_lock<std::shared_mutex> lk(gv.m_obj);
        for (uint32_t i = 0; i < n; ++i) {
            if (!out[i]) continue;
            const VkGraphicsPipelineCreateInfo& c = ci[i];
            VkPipeFacts f;
            bool dyn_depth = false, dyn_blend = false;
            f.dyn_vp = f.dyn_sc = false;
            if (c.pDynamicState)
                for (uint32_t k = 0; k < c.pDynamicState->dynamicStateCount; ++k) {
                    const VkDynamicState ds = c.pDynamicState->pDynamicStates[k];
                    if (ds == VK_DYNAMIC_STATE_VIEWPORT || ds == VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT) f.dyn_vp = true;
                    if (ds == VK_DYNAMIC_STATE_SCISSOR || ds == VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT) f.dyn_sc = true;
                    if (ds == VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE) dyn_depth = true;
                    if (ds == VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT) dyn_blend = true;
                }
            f.depth = dyn_depth ? -2 : (c.pDepthStencilState ? (c.pDepthStencilState->depthTestEnable ? 1 : 0) : 0);
            f.blend = dyn_blend || !c.pColorBlendState || !c.pColorBlendState->attachmentCount
                          ? int8_t(dyn_blend ? -1 : 0) : int8_t(c.pColorBlendState->pAttachments[0].blendEnable ? 1 : 0);
            if (c.pViewportState) {
                if (!f.dyn_vp && c.pViewportState->pViewports && c.pViewportState->viewportCount) f.vp = c.pViewportState->pViewports[0];
                if (!f.dyn_sc && c.pViewportState->pScissors && c.pViewportState->scissorCount) f.sc = c.pViewportState->pScissors[0];
            }
            gv.pipes[reinterpret_cast<uint64_t>(out[i])] = f;
        }
    }
    return r;
}

// commands
PFN_vkBeginCommandBuffer rv_begin_cb[2];
PFN_vkCmdBeginRenderPass rv_begin_rp[2];
PFN_vkCmdBeginRenderPass2 rv_begin_rp2[2], rv_begin_rp2_khr[2];
PFN_vkCmdEndRenderPass rv_end_rp[2];
PFN_vkCmdEndRenderPass2 rv_end_rp2[2], rv_end_rp2_khr[2];
PFN_vkCmdBeginRendering rv_begin_r[2], rv_begin_r_khr[2];
PFN_vkCmdEndRendering rv_end_r[2], rv_end_r_khr[2];
PFN_vkCmdBindPipeline rv_bind_pipe[2];
PFN_vkCmdSetViewport rv_set_vp[2];
PFN_vkCmdSetScissor rv_set_sc[2];
PFN_vkCmdSetDepthTestEnable rv_set_depth[2], rv_set_depth_ext[2];
PFN_vkCmdDraw rv_draw[2];
PFN_vkCmdDrawIndexed rv_draw_indexed[2];
PFN_vkCmdDrawIndirect rv_draw_indirect[2];
PFN_vkCmdDrawIndexedIndirect rv_draw_indexed_indirect[2];
PFN_vkCmdDrawIndirectCount rv_draw_count[2], rv_draw_indexed_count[2];
PFN_vkCmdCopyImage rv_copy_image[2];
PFN_vkCmdBlitImage rv_blit_image[2];
PFN_vkCmdCopyBufferToImage rv_copy_buffer[2];
PFN_vkCmdClearColorImage rv_clear_colour[2];
PFN_vkCmdResolveImage rv_resolve[2];
PFN_vkCmdExecuteCommands rv_execute_commands[2];
PFN_vkCmdClearAttachments rv_clear_attachments[2];
PFN_vkCmdNextSubpass rv_next_subpass[2];
PFN_vkCmdNextSubpass2 rv_next_subpass2[2], rv_next_subpass2_khr[2];

template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_begin_cb(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info) {
    const VkResult result = rv_begin_cb[I](cb, info);
    if (result == VK_SUCCESS) {
        nr::gpu::reset(cb);
        if (gv.cmds_hooked && !t_own) vlist(cb)->reset();
    }
    return result;
}
// A render pass begins on `views` with `rp`'s attachments (or, dynamic rendering, each attachment's own load op and
// layout): the copy before it where it keeps a back buffer's contents.
void vk_pass(VkCommandBuffer cb, uint64_t img, VkAttachmentLoadOp load, VkImageLayout layout, bool dsv) {
    VkList* s = vlist(cb);
    s->back = 0;
    s->dsv = dsv;
    if (!is_back_vk(img)) { s->in_pass = true; return; }
    if (load == VK_ATTACHMENT_LOAD_OP_LOAD) vk_copy_before(cb, *s, img, layout);
    s->in_pass = true;
    s->back = img;
    if (load != VK_ATTACHMENT_LOAD_OP_LOAD) vk_write(cb, *s, img, kScene);   // cleared or left undefined: a scene start
}
void vk_begin_rp(VkCommandBuffer cb, const VkRenderPassBeginInfo* info) {
    if (!on_vk() || !info) return;
    FbInfo fb;
    RpInfo rp;
    {
        std::shared_lock<std::shared_mutex> lk(gv.m_obj);
        const auto f = gv.fbs.find(reinterpret_cast<uint64_t>(info->framebuffer));
        const auto p = gv.rps.find(reinterpret_cast<uint64_t>(info->renderPass));
        if (f == gv.fbs.end() || p == gv.rps.end()) { VkList* s = vlist(cb); s->back = 0; s->in_pass = true; return; }
        fb = f->second;
        rp = p->second;
    }
    std::vector<VkImageView> views = fb.views;
    if (views.empty())   // imageless: VkRenderPassAttachmentBeginInfo
        for (const VkBaseInStructure* n = static_cast<const VkBaseInStructure*>(info->pNext); n; n = n->pNext)
            if (n->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
                const auto* a = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(n);
                views.assign(a->pAttachments, a->pAttachments + a->attachmentCount);
            }
    for (uint32_t c : rp.colour) {
        if (c == VK_ATTACHMENT_UNUSED || c >= views.size() || c >= rp.load.size()) continue;
        const uint64_t img = image_of_view(views[c]);
        if (!is_back_vk(img)) continue;
        vk_pass(cb, img, rp.load[c], rp.initial[c], rp.depth);
        return;
    }
    vk_pass(cb, 0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_UNDEFINED, rp.depth);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_begin_rp(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, VkSubpassContents c) {
    vk_begin_rp(cb, info);
    rv_begin_rp[I](cb, info, c);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_begin_rp2(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* sb) {
    vk_begin_rp(cb, info);
    rv_begin_rp2[I](cb, info, sb);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_begin_rp2_khr(VkCommandBuffer cb, const VkRenderPassBeginInfo* info,
                                                             const VkSubpassBeginInfo* sb) {
    vk_begin_rp(cb, info);
    rv_begin_rp2_khr[I](cb, info, sb);
}
void vk_end(VkCommandBuffer cb) {
    if (!on_vk()) return;
    VkList* s = vlist(cb);
    s->in_pass = false;
    s->back = 0;
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_end_rp(VkCommandBuffer cb) { vk_end(cb); rv_end_rp[I](cb); }
template <int I> VKAPI_ATTR void VKAPI_CALL kv_end_rp2(VkCommandBuffer cb, const VkSubpassEndInfo* e) { vk_end(cb); rv_end_rp2[I](cb, e); }
template <int I> VKAPI_ATTR void VKAPI_CALL kv_end_rp2_khr(VkCommandBuffer cb, const VkSubpassEndInfo* e) { vk_end(cb); rv_end_rp2_khr[I](cb, e); }
void vk_begin_rendering(VkCommandBuffer cb, const VkRenderingInfo* info) {
    if (!on_vk() || !info) return;
    const bool dsv = info->pDepthAttachment && info->pDepthAttachment->imageView;
    for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
        const VkRenderingAttachmentInfo& a = info->pColorAttachments[i];
        const uint64_t img = image_of_view(a.imageView);
        if (!is_back_vk(img)) continue;
        vk_pass(cb, img, a.loadOp, a.imageLayout, dsv);
        return;
    }
    vk_pass(cb, 0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_UNDEFINED, dsv);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_begin_r(VkCommandBuffer cb, const VkRenderingInfo* info) { vk_begin_rendering(cb, info); rv_begin_r[I](cb, info); }
template <int I> VKAPI_ATTR void VKAPI_CALL kv_begin_r_khr(VkCommandBuffer cb, const VkRenderingInfo* info) { vk_begin_rendering(cb, info); rv_begin_r_khr[I](cb, info); }
template <int I> VKAPI_ATTR void VKAPI_CALL kv_end_r(VkCommandBuffer cb) { vk_end(cb); rv_end_r[I](cb); }
template <int I> VKAPI_ATTR void VKAPI_CALL kv_end_r_khr(VkCommandBuffer cb) { vk_end(cb); rv_end_r_khr[I](cb); }
template <int I> VKAPI_ATTR void VKAPI_CALL kv_bind_pipe(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (on_vk() && bp == VK_PIPELINE_BIND_POINT_GRAPHICS) vlist(cb)->pipe = p;
    rv_bind_pipe[I](cb, bp, p);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_set_vp(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
    if (on_vk() && first == 0 && n && v) vlist(cb)->vp = v[0];
    rv_set_vp[I](cb, first, n, v);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_set_sc(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r) {
    if (on_vk() && first == 0 && n && r) vlist(cb)->sc = r[0];
    rv_set_sc[I](cb, first, n, r);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_set_depth(VkCommandBuffer cb, VkBool32 on) {
    if (on_vk()) vlist(cb)->dyn_depth = on ? 1 : 0;
    rv_set_depth[I](cb, on);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_set_depth_ext(VkCommandBuffer cb, VkBool32 on) {
    if (on_vk()) vlist(cb)->dyn_depth = on ? 1 : 0;
    rv_set_depth_ext[I](cb, on);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_draw(VkCommandBuffer cb, uint32_t v, uint32_t i, uint32_t fv, uint32_t fi) {
    vk_draw(cb, v, i, false);
    rv_draw[I](cb, v, i, fv, fi);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_draw_indexed(VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t fi, int32_t vo, uint32_t fin) {
    vk_draw(cb, n, i, false);
    rv_draw_indexed[I](cb, n, i, fi, vo, fin);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_draw_indirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    vk_draw(cb, 0, 0, true);
    rv_draw_indirect[I](cb, b, o, n, s);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_draw_indexed_indirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    vk_draw(cb, 0, 0, true);
    rv_draw_indexed_indirect[I](cb, b, o, n, s);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_draw_count(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co,
                                                          uint32_t m, uint32_t s) {
    vk_draw(cb, 0, 0, true);
    rv_draw_count[I](cb, b, o, cbuf, co, m, s);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_draw_indexed_count(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf,
                                                                  VkDeviceSize co, uint32_t m, uint32_t s) {
    vk_draw(cb, 0, 0, true);
    rv_draw_indexed_count[I](cb, b, o, cbuf, co, m, s);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_copy_image(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                                          uint32_t n, const VkImageCopy* r) {
    for (uint32_t k = 0; k < n && r; ++k)
        vk_transfer(cb, dst, r[k].dstOffset.x, r[k].dstOffset.y, r[k].extent.width, r[k].extent.height);
    rv_copy_image[I](cb, src, sl, dst, dl, n, r);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_blit_image(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                                          uint32_t n, const VkImageBlit* r, VkFilter f) {
    for (uint32_t k = 0; k < n && r; ++k) {
        const VkOffset3D a = r[k].dstOffsets[0], b = r[k].dstOffsets[1];
        vk_transfer(cb, dst, std::min(a.x, b.x), std::min(a.y, b.y), uint32_t(std::abs(b.x - a.x)), uint32_t(std::abs(b.y - a.y)));
    }
    rv_blit_image[I](cb, src, sl, dst, dl, n, r, f);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_copy_buffer(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, uint32_t n,
                                                           const VkBufferImageCopy* r) {
    for (uint32_t k = 0; k < n && r; ++k)
        vk_transfer(cb, dst, r[k].imageOffset.x, r[k].imageOffset.y, r[k].imageExtent.width, r[k].imageExtent.height);
    rv_copy_buffer[I](cb, src, dst, dl, n, r);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_clear_colour(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue* c,
                                                            uint32_t n, const VkImageSubresourceRange* r) {
    vk_transfer(cb, img, 0, 0, gv.w.load(), gv.h.load());
    rv_clear_colour[I](cb, img, l, c, n, r);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_resolve(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                                       uint32_t n, const VkImageResolve* r) {
    for (uint32_t k = 0; k < n && r; ++k)
        vk_transfer(cb, dst, r[k].dstOffset.x, r[k].dstOffset.y, r[k].extent.width, r[k].extent.height);
    rv_resolve[I](cb, src, sl, dst, dl, n, r);
}

// Attachments cleared inside a pass on a back buffer: a scene write when one rect covers it all, else an overlay (a UI
// panel's background).
template <int I> VKAPI_ATTR void VKAPI_CALL kv_clear_attachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment* a,
                                                                 uint32_t nr, const VkClearRect* r) {
    if (on_vk()) {
        VkList* s = vlist(cb);
        bool colour = false, whole = false;
        for (uint32_t i = 0; a && i < na; ++i) colour |= (a[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
        for (uint32_t i = 0; r && i < nr; ++i)
            whole |= r[i].rect.offset.x <= 0 && r[i].rect.offset.y <= 0 && r[i].rect.extent.width >= gv.w.load() &&
                     r[i].rect.extent.height >= gv.h.load();
        if (s->back && colour) vk_write(cb, *s, s->back, whole ? kScene : kOverlay);
    }
    rv_clear_attachments[I](cb, na, a, nr, r);
}

// Work this capture does not follow inside a pass on a back buffer: the frame is not trusted.
void vk_unfollowed(VkCommandBuffer cb) {
    if (!on_vk()) return;
    VkList* s = vlist(cb);
    if (s->back) vk_write(cb, *s, s->back, kUnknown);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_execute_commands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer* b) {
    for (uint32_t i = 0; i < n; ++i) nr::gpu::inherit(cb, b[i]);
    vk_unfollowed(cb);
    rv_execute_commands[I](cb, n, b);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_next_subpass(VkCommandBuffer cb, VkSubpassContents c) {
    vk_unfollowed(cb);
    rv_next_subpass[I](cb, c);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_next_subpass2(VkCommandBuffer cb, const VkSubpassBeginInfo* b, const VkSubpassEndInfo* e) {
    vk_unfollowed(cb);
    rv_next_subpass2[I](cb, b, e);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_next_subpass2_khr(VkCommandBuffer cb, const VkSubpassBeginInfo* b,
                                                                 const VkSubpassEndInfo* e) {
    vk_unfollowed(cb);
    rv_next_subpass2_khr[I](cb, b, e);
}

std::mutex lifetime_mutex;
std::unordered_map<VkCommandBuffer, VkCommandPool> lifetime_pools;
PFN_vkAllocateCommandBuffers rv_allocate_cb[2]{};
PFN_vkFreeCommandBuffers rv_free_cb[2]{};
PFN_vkResetCommandBuffer rv_reset_cb[2]{};
PFN_vkResetCommandPool rv_reset_pool[2]{};
PFN_vkDestroyCommandPool rv_destroy_pool[2]{};
template<int I> VKAPI_ATTR VkResult VKAPI_CALL kv_allocate_cb(VkDevice d, const VkCommandBufferAllocateInfo* a, VkCommandBuffer* cb) {
    const VkResult r = rv_allocate_cb[I](d, a, cb);
    if (r == VK_SUCCESS) { std::lock_guard<std::mutex> guard(lifetime_mutex);
        for (uint32_t i = 0; i < a->commandBufferCount; ++i) lifetime_pools[cb[i]] = a->commandPool; }
    return r;
}
template<int I> VKAPI_ATTR void VKAPI_CALL kv_free_cb(VkDevice d, VkCommandPool p, uint32_t n, const VkCommandBuffer* cb) {
    rv_free_cb[I](d, p, n, cb);
    std::lock_guard<std::mutex> guard(lifetime_mutex);
    for (uint32_t i = 0; i < n; ++i) { nr::gpu::reset(cb[i]); lifetime_pools.erase(cb[i]); }
}
template<int I> VKAPI_ATTR VkResult VKAPI_CALL kv_reset_cb(VkCommandBuffer cb, VkCommandBufferResetFlags f) {
    const VkResult r = rv_reset_cb[I](cb, f); if (r == VK_SUCCESS) nr::gpu::reset(cb); return r;
}
void reset_pool_recordings(VkCommandPool pool, bool destroy) {
    std::lock_guard<std::mutex> guard(lifetime_mutex);
    for (auto i = lifetime_pools.begin(); i != lifetime_pools.end();) {
        if (i->second != pool) { ++i; continue; }
        nr::gpu::reset(i->first);
        if (destroy) i = lifetime_pools.erase(i); else ++i;
    }
}
template<int I> VKAPI_ATTR VkResult VKAPI_CALL kv_reset_pool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) {
    const VkResult r = rv_reset_pool[I](d, p, f); if (r == VK_SUCCESS) reset_pool_recordings(p, false); return r;
}
template<int I> VKAPI_ATTR void VKAPI_CALL kv_destroy_pool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks* a) {
    rv_destroy_pool[I](d, p, a); reset_pool_recordings(p, true);
}

// `name` in the Vulkan loader hooked with detours [0] (its export) and [1] (its vkGetDeviceProcAddr's, when another).
bool hook_vk(VkDevice device, const char* name, void* d0, void** r0, void* d1, void** r1) {
    HMODULE m = GetModuleHandleW(L"vulkan-1.dll");
    if (!m) return false;
    const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(reinterpret_cast<void*>(GetProcAddress(m, "vkGetDeviceProcAddr")));
    void* exported = reinterpret_cast<void*>(GetProcAddress(m, name));
    void* asked = gdpa && device ? reinterpret_cast<void*>(gdpa(device, name)) : nullptr;
    void* targets[2] = {exported, asked && asked != exported ? asked : nullptr};
    void* detours[2] = {d0, d1};
    void** reals[2] = {r0, r1};
    bool any = false;
    for (int i = 0; i < 2; ++i) {
        if (!targets[i]) continue;
        MH_STATUS st = MH_CreateHook(targets[i], detours[i], reals[i]);
        if (st == MH_OK || (st == MH_ERROR_ALREADY_CREATED && *reals[i])) st = MH_EnableHook(targets[i]);
        if (st == MH_OK || st == MH_ERROR_ENABLED) any = true;
        else log("[nr] HUD FIX capture (Vulkan): %s could not be hooked (%s)", name, MH_StatusToString(st));
    }
    return any;
}
#define NR_VK_HOOK(device, name, k, r) \
    hook_vk(device, name, as(&k<0>), reinterpret_cast<void**>(&r[0]), as(&k<1>), reinterpret_cast<void**>(&r[1]))

}  // namespace

bool vk_lifetime(VkDevice device) {
    bool ok = NR_VK_HOOK(device, "vkBeginCommandBuffer", kv_begin_cb, rv_begin_cb);
    ok = NR_VK_HOOK(device, "vkAllocateCommandBuffers", kv_allocate_cb, rv_allocate_cb) && ok;
    ok = NR_VK_HOOK(device, "vkFreeCommandBuffers", kv_free_cb, rv_free_cb) && ok;
    ok = NR_VK_HOOK(device, "vkResetCommandBuffer", kv_reset_cb, rv_reset_cb) && ok;
    ok = NR_VK_HOOK(device, "vkResetCommandPool", kv_reset_pool, rv_reset_pool) && ok;
    ok = NR_VK_HOOK(device, "vkDestroyCommandPool", kv_destroy_pool, rv_destroy_pool) && ok;
    ok = NR_VK_HOOK(device, "vkCmdExecuteCommands", kv_execute_commands, rv_execute_commands) && ok;
    return ok;
}

void vk_device(VkDevice device, bool translator) {
    if (device) vk_lifetime(device);
    if (translator || !device) return;
    if (gv.created_hooked) {   // another native device: its creations from now on (the hooks are the loader's own)
        std::unique_lock<std::shared_mutex> lk(gv.m_obj);
        gv.views.clear(); gv.fbs.clear(); gv.rps.clear(); gv.pipes.clear(); gv.usage.clear(); gv.reordered.clear();
        gv.device.store(device);
        return;
    }
    gv.created_hooked = true;
    gv.device.store(device);
    MH_Initialize();
    bool ok = NR_VK_HOOK(device, "vkCreateImageView", kv_create_view, rv_create_view);
    ok = NR_VK_HOOK(device, "vkCreateImage", kv_create_image, rv_create_image) && ok;
    ok = NR_VK_HOOK(device, "vkCreateFramebuffer", kv_create_fb, rv_create_fb) && ok;
    ok = NR_VK_HOOK(device, "vkCreateRenderPass", kv_create_rp, rv_create_rp) && ok;
    NR_VK_HOOK(device, "vkCreateRenderPass2", kv_create_rp2, rv_create_rp2);
    NR_VK_HOOK(device, "vkCreateRenderPass2KHR", kv_create_rp2_khr, rv_create_rp2_khr);
    ok = NR_VK_HOOK(device, "vkCreateGraphicsPipelines", kv_create_pipes, rv_create_pipes) && ok;
    log("[nr] HUD FIX capture (Vulkan): the device's images, image views, framebuffers, render passes and pipelines are "
        "followed%s", ok ? "" : " (in part: above)");
}

bool vk_image_usage(VkImage image, VkImageUsageFlags* usage) {
    std::shared_lock<std::shared_mutex> lk(gv.m_obj);
    const auto it = gv.usage.find(reinterpret_cast<uint64_t>(image));
    if (it == gv.usage.end()) return false;
    *usage = it->second;
    return true;
}
bool vk_view_reordered(VkImageView view) {
    std::shared_lock<std::shared_mutex> lk(gv.m_obj);
    return gv.reordered.count(reinterpret_cast<uint64_t>(view)) != 0;
}

void vk_submitted(uint32_t n, const VkCommandBuffer* buffers) {
    if (!gv.cmds_hooked || t_own || !buffers) return;
    std::lock_guard<std::mutex> lk(gv.m);
    for (uint32_t i = 0; i < n; ++i) {
        const auto it = gv.lists.find(buffers[i]);
        if (it == gv.lists.end() || it->second.recs.empty()) continue;
        // its records stay until the buffer is begun again: submitted again, its copies run again
        if (gv.frame.size() < 16384) gv.frame.insert(gv.frame.end(), it->second.recs.begin(), it->second.recs.end());
    }
}
void vk_submitted2(uint32_t n, const VkSubmitInfo2* submits) {
    if (!gv.cmds_hooked || t_own || !submits) return;
    std::vector<VkCommandBuffer> cbs;
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t k = 0; k < submits[i].commandBufferInfoCount; ++k) cbs.push_back(submits[i].pCommandBufferInfos[k].commandBuffer);
    vk_submitted(uint32_t(cbs.size()), cbs.data());
}

FrameVk vk_present(VkDevice device, const std::vector<VkImage>& images, VkImage image, VkImage hudless, uint32_t w, uint32_t h,
                   bool armed) {
    FrameVk f;
    const bool was = gv.armed.load();
    if (!armed || !device) {
        if (was) {
            gv.armed.store(false);
            std::lock_guard<std::mutex> lk(gv.m);
            gv.frame.clear();
        }
        return f;
    }
    if (!gv.created_hooked || gv.device.load() != device) {
        f.result = Result::kMissed;
        f.why = "the game's Vulkan device was not seen being made";
        f.broken = true;
        return f;
    }
    if (!gv.cmds_hooked) {
        gv.cmds_hooked = true;
        bool ok = NR_VK_HOOK(device, "vkBeginCommandBuffer", kv_begin_cb, rv_begin_cb);
        ok = NR_VK_HOOK(device, "vkCmdBeginRenderPass", kv_begin_rp, rv_begin_rp) && ok;
        NR_VK_HOOK(device, "vkCmdBeginRenderPass2", kv_begin_rp2, rv_begin_rp2);
        NR_VK_HOOK(device, "vkCmdBeginRenderPass2KHR", kv_begin_rp2_khr, rv_begin_rp2_khr);
        ok = NR_VK_HOOK(device, "vkCmdEndRenderPass", kv_end_rp, rv_end_rp) && ok;
        NR_VK_HOOK(device, "vkCmdEndRenderPass2", kv_end_rp2, rv_end_rp2);
        NR_VK_HOOK(device, "vkCmdEndRenderPass2KHR", kv_end_rp2_khr, rv_end_rp2_khr);
        NR_VK_HOOK(device, "vkCmdBeginRendering", kv_begin_r, rv_begin_r);
        NR_VK_HOOK(device, "vkCmdBeginRenderingKHR", kv_begin_r_khr, rv_begin_r_khr);
        NR_VK_HOOK(device, "vkCmdEndRendering", kv_end_r, rv_end_r);
        NR_VK_HOOK(device, "vkCmdEndRenderingKHR", kv_end_r_khr, rv_end_r_khr);
        ok = NR_VK_HOOK(device, "vkCmdBindPipeline", kv_bind_pipe, rv_bind_pipe) && ok;
        ok = NR_VK_HOOK(device, "vkCmdSetViewport", kv_set_vp, rv_set_vp) && ok;
        ok = NR_VK_HOOK(device, "vkCmdSetScissor", kv_set_sc, rv_set_sc) && ok;
        NR_VK_HOOK(device, "vkCmdSetDepthTestEnable", kv_set_depth, rv_set_depth);
        NR_VK_HOOK(device, "vkCmdSetDepthTestEnableEXT", kv_set_depth_ext, rv_set_depth_ext);
        ok = NR_VK_HOOK(device, "vkCmdDraw", kv_draw, rv_draw) && ok;
        ok = NR_VK_HOOK(device, "vkCmdDrawIndexed", kv_draw_indexed, rv_draw_indexed) && ok;
        NR_VK_HOOK(device, "vkCmdDrawIndirect", kv_draw_indirect, rv_draw_indirect);
        NR_VK_HOOK(device, "vkCmdDrawIndexedIndirect", kv_draw_indexed_indirect, rv_draw_indexed_indirect);
        NR_VK_HOOK(device, "vkCmdDrawIndirectCount", kv_draw_count, rv_draw_count);
        NR_VK_HOOK(device, "vkCmdDrawIndexedIndirectCount", kv_draw_indexed_count, rv_draw_indexed_count);
        NR_VK_HOOK(device, "vkCmdCopyImage", kv_copy_image, rv_copy_image);
        NR_VK_HOOK(device, "vkCmdBlitImage", kv_blit_image, rv_blit_image);
        NR_VK_HOOK(device, "vkCmdCopyBufferToImage", kv_copy_buffer, rv_copy_buffer);
        NR_VK_HOOK(device, "vkCmdClearColorImage", kv_clear_colour, rv_clear_colour);
        NR_VK_HOOK(device, "vkCmdResolveImage", kv_resolve, rv_resolve);
        NR_VK_HOOK(device, "vkCmdExecuteCommands", kv_execute_commands, rv_execute_commands);
        NR_VK_HOOK(device, "vkCmdClearAttachments", kv_clear_attachments, rv_clear_attachments);
        NR_VK_HOOK(device, "vkCmdNextSubpass", kv_next_subpass, rv_next_subpass);
        NR_VK_HOOK(device, "vkCmdNextSubpass2", kv_next_subpass2, rv_next_subpass2);
        NR_VK_HOOK(device, "vkCmdNextSubpass2KHR", kv_next_subpass2_khr, rv_next_subpass2_khr);
        log("[nr] HUD FIX capture (Vulkan): the game's command buffers are followed%s", ok ? "" : " (in part: above)");
    }
    for (size_t i = 0; i < 16; ++i) gv.backs[i].store(i < images.size() ? reinterpret_cast<uint64_t>(images[i]) : 0);
    gv.w.store(w);
    gv.h.store(h);
    gv.hudless.store(reinterpret_cast<uint64_t>(hudless), std::memory_order_release);
    gv.armed.store(true);
    std::vector<Rec> recs;
    {
        std::lock_guard<std::mutex> lk(gv.m);
        recs.swap(gv.frame);
    }
    gv.copies.store(0);
    if (!was) { f.result = Result::kMissed; f.why = "it starts with the next frame"; return f; }
    const nr::hudcap::Verdict v = nr::hudcap::verdict(recs, reinterpret_cast<uint64_t>(image), gv.foreign.exchange(false),
                                                      reinterpret_cast<uint64_t>(hudless), f.why);
    f.result = v == nr::hudcap::Verdict::kCaptured ? Result::kCaptured
               : v == nr::hudcap::Verdict::kClean ? Result::kClean : Result::kMissed;
    if (f.result == Result::kMissed && f.why == "the scene was written again after the copy")
        f.why = "the HUD is drawn in the scene's own render pass (inside which no copy can be made)";
    return f;
}

int copy_group(DXGI_FORMAT f) { return format_group(f); }
DXGI_FORMAT ui_alpha_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

}  // namespace nr::asi::hudcap
