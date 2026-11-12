#include "nr_path_u8.hpp"
// The real network of the D3D12 passthrough (nr_d3d12_passthrough_net.hpp). Same steps, same order, same names as
// nr::Runtime runs its network (windows/src/core/nr_runtime.cpp): build, retarget, controls / patch_push, record. What
// differs is only what D3D12 and the ELF pipelines need:
//   - a dispatch is the pipeline made from its kernel's ACO record (nr_d3d12_passthrough_elf.hpp) with a 4 KB table per
//     dispatch - buffer and image descriptors, the push block at 512, the grid at 640 - bound as the root UAV (the
//     ELF's prefix loads the kernel's descriptor pointer, push constants and grid from it), where the Vulkan runtime
//     binds a descriptor set and pushes constants;
//   - the images are linear buffers with RDNA4 image descriptors written here: tex_in = the frame, surf0 = the
//     answer (RGBA32F at the frame's extent, the backend's), the temporal path's flow0 = the engine's motion resampled
//     onto the frame by the backend, the two history images (RGBA16F or RGBA32F: [Network] History Precision), the
//     depth; surf1 and a missing depth are 1x1 stand-ins, as the runtime's (the post block never writes surf1:
//     post_no_out1);
//   - the activation arena's "zero once" is a fresh committed resource (D3D12 zeroes those);
//   - the seed-0 noise table is a buffer of its own at the pre kernels' binding 5, made once frames keep seed 0
//     (a 256-byte stand-in until then), as Runtime::Impl::make_noise;
//   - the post block restores the frame's alpha (and the 8-bit rounding) itself, as the runtime's (no alpha pass);
//   - the temporal params buffer travels with the frame's tables (one upload, one copy).
#include "nr_d3d12_passthrough_net.hpp"
#include "nr_d3d12_passthrough.hpp"
#include "nr_d3d12_passthrough_elf.hpp"
#include "tinlayout.hpp"
#include "nr_ini_network.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_d3d12split.hpp"
#include "nr_vram_ledger.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace nr::pe::d3d12_passthrough {
namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Reuse the runtime's use set, with the strict D3D12 completion query instead of its timed fallback.
void touch_recording(nr::gpu::Uses& uses, const nr::gpu::Ticket& rec) {
    auto& tickets = uses.tickets;
    tickets.erase(std::remove_if(tickets.begin(), tickets.end(), [](const auto& t) {
        return split::completed(t);
    }), tickets.end());
    if (rec && std::find(tickets.begin(), tickets.end(), rec) == tickets.end()) tickets.push_back(rec);
}
bool recordings_done(const nr::gpu::Uses& uses) {
    return std::all_of(uses.tickets.begin(), uses.tickets.end(), [](const auto& t) {
        return split::completed(t);
    });
}


std::atomic<NetPlanner> g_planner{nullptr};
std::atomic<NetControlsPatch> g_patch{nullptr};
std::atomic<NetModelQuery> g_model_query{nullptr};

std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(n > 0 ? size_t(n - 1) : 0);
    return w;
}
bool slurp(const std::wstring& path, std::vector<uint8_t>* out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->resize(n > 0 ? size_t(n) : 0);
    const bool ok = n >= 0 && std::fread(out->data(), 1, out->size(), f) == out->size();
    std::fclose(f);
    return ok;
}
// The record's name: FNV-1a 64 of the kernel's SPIR-V (as windows/data/aco/records names its files).
uint64_t fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// ---- a kernel's pipeline: its ACO record wrapped into a PAL ELF (nr_d3d12_passthrough_elf.hpp)
// ------------------------
struct KernElf { uint32_t dims[3]{}; std::string stem; std::vector<uint8_t> elf_bytes; std::set<uint32_t> e4m3;
                 std::vector<uint32_t> aliases; };
// RDNA3's stand-ins: the storage-buffer bindings that hold e4m3 data (nrvk.hpp's e4m3_view_bindings, the same rule: the
// block's name *E4M3*, *Pair4, Act, Act4 - bound to the FP16 twin) and the alias bindings (nrvk.hpp kAliasBase = 16 + k:
// a raw-word view of the buffer at binding k, bound to that buffer itself), in binding order.
constexpr uint32_t kAliasBase = 16;
void rdna3_bindings(const std::vector<uint8_t>& spv, std::set<uint32_t>* e4m3, std::vector<uint32_t>* aliases) {
    const size_t words = spv.size() / 4;
    const auto at = [&](size_t i) { uint32_t v = 0; std::memcpy(&v, spv.data() + 4 * i, 4); return v; };
    std::map<uint32_t, std::string> names;
    std::map<uint32_t, uint32_t> pointee, var_type, binding;
    for (size_t i = 5; i < words;) {
        const uint32_t op = at(i) & 0xFFFFu, wc = at(i) >> 16;
        if (!wc || i + wc > words) break;
        if (op == 5 && wc >= 3) names[at(i + 1)] = std::string(reinterpret_cast<const char*>(spv.data() + 4 * (i + 2)),
                                                               strnlen(reinterpret_cast<const char*>(spv.data() + 4 * (i + 2)), (wc - 2) * 4));
        else if (op == 32 && wc == 4 && at(i + 2) == 12) pointee[at(i + 1)] = at(i + 3);      // OpTypePointer StorageBuffer
        else if (op == 59 && wc >= 4 && at(i + 3) == 12) var_type[at(i + 2)] = at(i + 1);     // OpVariable StorageBuffer
        else if (op == 71 && wc >= 4 && at(i + 2) == 33) binding[at(i + 1)] = at(i + 3);      // OpDecorate Binding
        i += wc;
    }
    std::set<uint32_t> alias_set;
    for (const auto& v : var_type) {
        const auto b = binding.find(v.first);
        const auto p = pointee.find(v.second);
        if (b == binding.end() || p == pointee.end()) continue;
        if (b->second >= kAliasBase) { alias_set.insert(b->second); continue; }
        const std::string& n = names[p->second];
        if (n.find("E4M3") != std::string::npos || n.find("Pair4") != std::string::npos || n == "Act" || n == "Act4")
            e4m3->insert(b->second);
    }
    aliases->assign(alias_set.begin(), alias_set.end());
}

// ---- the binding table of a kernel: nr_graph.cpp's kernel creation ------------------------------------------------
// NOISE: the seed-0 noise table (binding 5 of the pre kernels, image_input.glsl; g_noisefield writes it).
enum Res { ACT, WGT, NOISE, PARAMS };
// TEX_IN, FLOW, HIST, DEPTH: combined image + sampler (48 bytes); SURF0, SURF1, HIST_STORE: storage images (32).
enum Img { TEX_IN, SURF0, SURF1, FLOW, HIST, HIST_STORE, DEPTH };
struct Family { std::vector<Res> buffers; std::vector<Img> images; };
bool family_of(std::string k, Family* f) {
    if (k.size() > 6 && k.compare(k.size() - 6, 6, "_grade") == 0) k.resize(k.size() - 6);
    auto w = [&](std::initializer_list<Res> b) { f->buffers.assign(b); };
    // nr_runtime.cpp's temporal pre / post: {act, act, wgt, wgt, wgt, noise_buf, params} {tex_in, flow0_sampled,
    // hist(c), depth}; {act, act, wgt, wgt, wgt, params} {surf0, hist_store[c ^ 1], tex_in, flow0_sampled, hist(c)}.
    if (k.rfind("temporal_pre", 0) == 0) { w({ACT, ACT, WGT, WGT, WGT, NOISE, PARAMS}); f->images = {TEX_IN, FLOW, HIST, DEPTH}; }
    else if (k.rfind("temporal_post", 0) == 0) { w({ACT, ACT, WGT, WGT, WGT, PARAMS}); f->images = {SURF0, HIST_STORE, TEX_IN, FLOW, HIST}; }
    else if (k.rfind("fswindsp", 0) == 0) w({ACT, ACT, WGT, WGT, WGT, ACT});
    else if (k == "fswinimagepreds32" || k == "fswinimagepreds32nh") { w({ACT, ACT, WGT, WGT, WGT, NOISE}); f->images = {TEX_IN}; }
    else if (k == "fswinimagepost32") { w({ACT, ACT, WGT, WGT, WGT}); f->images = {SURF0, SURF1, TEX_IN}; }
    else if (k.rfind("fswinfusedup", 0) == 0) w({ACT, ACT, WGT, WGT, WGT});
    else if (k == "fswinblendpost32") w({ACT, ACT, WGT, WGT, WGT});
    else if (k.rfind("fswin", 0) == 0) w({ACT, ACT, WGT, WGT, WGT, ACT});
    else if (k == "decups") w({ACT, ACT, WGT});
    else if (k == "vitattn") w({ACT, WGT, ACT});
    else if (k.rfind("attn", 0) == 0) w({ACT, ACT, WGT, WGT, ACT});
    else if (k == "ffwd3" || k == "ffwd3w") w({ACT, ACT, ACT, WGT, WGT, ACT});
    else if (k.rfind("gemm", 0) == 0) w({ACT, ACT, ACT, WGT, WGT, ACT, WGT, ACT});
    else if (k == "noisefield") w({NOISE});
    // (the RDNA3 lowering's own: nr_graph.cpp's kernel creation)
    else if (k == "repack" || k == "upsview" || k.rfind("pool", 0) == 0) w({ACT});
    else if (k == "qkvnorm") w({ACT, ACT, WGT});
    else return false;
    return true;
}

constexpr uint32_t kTableBytes = 4096, kTablePushOff = 512, kTableGridOff = 640;
constexpr uint32_t kRing = 16;        // upload slots a buffer; more buffers are added while every slot is held
constexpr uint32_t kDoneSlots = 128;  // frame numbers the GPU lands in the readback buffer, a ring of these
constexpr uint32_t kParamsBytes = 256;   // the temporal params (12 floats), after the frame's tables
constexpr uint32_t kMaxFeatures = 64, kHardMaxFeatures = 128, kMaxExtents = 4;

using elf::put_buf;
// A linear image over a buffer, rows of `pitch` texels (lin_pitch: a multiple of 128 bytes). `format`: the RDNA4 image
// format, 63 32_32_32_32_FLOAT (RGBA32F) or 57 16_16_16_16_FLOAT (RGBA16F); RADV's own descriptors for linear images of
// the two formats differ in that field alone (vkGetDescriptorEXT, gfx1201). The pitch - 1 in word 4: its low 14 bits
// there (DEPTH_GFX12), the rest in PITCH_MSB_GFX12 - pitches above 16384 texels are not made (ready() refuses them).
constexpr uint32_t kFormatRgba32f = 63, kFormatRgba16f = 57;
void put_image(uint32_t* d, uint64_t va, uint32_t width, uint32_t height, uint32_t format, uint32_t pitch) {
    const uint32_t wm1 = width - 1, hm1 = height - 1;
    d[0] = uint32_t(va >> 8);
    d[1] = uint32_t((va >> 40) & 0xffu) | (format << 17) | ((wm1 & 3u) << 30);
    d[2] = (wm1 >> 2) | (hm1 << 14);
    d[3] = 0x90000facu;
    d[4] = pitch - 1u;
    d[5] = 4u << 20;
    d[6] = 1u << 15;
    d[7] = 0;
}
// RDNA3 (gfx11, RADV's linear images there, drm-shim phoenix / navi31): the format at bit 20 (RDNA4: 17), no pitch
// word (the rows are 256-byte aligned: lin_pitch), words 6/7 zero.
void put_image_gfx11(uint32_t* d, uint64_t va, uint32_t width, uint32_t height, uint32_t format) {
    const uint32_t wm1 = width - 1, hm1 = height - 1;
    d[0] = uint32_t(va >> 8);
    d[1] = uint32_t((va >> 40) & 0xffu) | (format << 20) | ((wm1 & 3u) << 30);
    d[2] = (wm1 >> 2) | (hm1 << 14);
    d[3] = 0x90000facu;
    d[4] = 0;
    d[5] = 4u << 20;
    d[6] = 0;
    d[7] = 0;
}
// nrvk's sampler (NEAREST, MIRRORED_REPEAT: tex_in) and the runtime's linear_sampler (LINEAR, mip NEAREST,
// CLAMP_TO_EDGE: flow0_sampled, the history, the depth), as the driver writes a sampler: clamp x/y/z, trunc_coord for
// a point sampler only, the xy mag / min filter, mip point.
void put_mirror_nearest_sampler(uint32_t* d) { d[0] = 0x08000049u; d[1] = 0; d[2] = 0x04000000u; d[3] = 0; }
void put_linear_clamp_sampler(uint32_t* d) { d[0] = 0x00000092u; d[1] = 0; d[2] = 0x04500000u; d[3] = 0; }
// RDNA3's (RADV gfx1103 / gfx1100): the linear sampler's word 0 carries bit 27 there; the nearest one is the same.
void put_linear_clamp_sampler_gfx11(uint32_t* d) { d[0] = 0x08000092u; d[1] = 0; d[2] = 0x04500000u; d[3] = 0; }

ID3D12Resource* buffer(ID3D12Device* device, D3D12_HEAP_TYPE type, uint64_t bytes, D3D12_RESOURCE_STATES state, bool uav) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes ? bytes : 256; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}
void uav_barrier(ID3D12GraphicsCommandList* cmd) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = nullptr;   // every UAV access: the next dispatch reads what this one wrote, as compute_barrier
    cmd->ResourceBarrier(1, &b);
}
void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmd->ResourceBarrier(1, &b);
}

// The temporal state of one feature (Runtime::Impl::FeatureState): its two history images (the post block writes the
// next frame's into the other, t.pingpong), the latch, the seed counter, the input domain its history was made under.
// D3D12 zeroes a new committed resource: a fresh feature's history is cleared as the runtime clears it.
struct FeatureState {
    ID3D12Resource* hist[2]{};
    bool f16 = false;              // the history images are RGBA16F (else RGBA32F)
    uint32_t hcur = 0;
    bool latch = false;
    uint32_t seed = 0;
    uint32_t domain = 0;
    uint64_t used = 0;
    int64_t last_frame = -1;
    nr::gpu::Uses gpu;
    ~FeatureState() { release(hist[0]); release(hist[1]); }
};

// One lowered network at one extent, everything it runs with (the Vulkan runtime's session + GraphSlot).
struct Built {
    uint32_t w = 0, h = 0;
    nr::D3d12NetPlan plan;
    std::chrono::steady_clock::time_point last{};   // its last record (the stale-history rule on a switch back)
    int hist_f16 = -1;                                // the history precision its features were made in (-1: none yet)
    // RDNA3: the bindings that read an FP16 twin (e4m3 views: the arena's and the weights' twins) and the alias
    // bindings (kAliasBase + k: raw-word views of buffer k), from the stand-in's interface
    struct Kern { ID3D12PipelineState* pso = nullptr; uint32_t tag = 0; Family fam; std::set<uint32_t> e4m3;
                  std::vector<uint32_t> aliases; };
    std::map<std::string, Kern> kern;
    Kern t_pre, t_post, t_grade;                  // the temporal pre / post (t_grade.pso null: no Style grade)
    Kern t_post16, t_grade16;                     // the RGBA16F history's post (t_post16.pso null: none)
    // stand: 256 bytes, the 1x1 surf1 and the missing depth (as the runtime's 1x1 images). noise: the seed-0 noise table,
    // the stand-in until noise_real (made once frames keep seed 0, make_noise).
    ID3D12Resource *act = nullptr, *wgt = nullptr, *wgt_upload = nullptr, *stand = nullptr, *noise = nullptr;
    // RDNA3 (the Vulkan runtime's fp16_twin): the arena's FP16 twin (twice its bytes, zero) and the weights' (each e4m3
    // byte as an f16), what its e4m3 views read
    bool rdna3 = false;
    ID3D12Resource *act_e = nullptr, *wgt_e = nullptr, *wgt_e_upload = nullptr;
    bool noise_real = false, want_noise = false;
    uint32_t seed0_run = 0;                       // consecutive recordings whose pre block ran seed 0
    // The recording that initialized this arena. If discarded, rebuild the Built: the original upload is already
    // retired and the recorded table/weight states cannot be repaired by clearing retargeted alone.
    nr::gpu::Ticket initialization;
    ID3D12Resource* table = nullptr;
    // The upload slots: ring buffers of kRing slots each, grown while every slot of every buffer is still held.
    // A slot is written by the CPU and read by the CopyBufferRegion of the same record(); its last 8 bytes carry
    // the record's frame number, and a second recorded copy lands that number in `copy_map`. A slot is handed
    // out again only once its number is seen there: the GPU has then executed this record's copy (and every
    // earlier one in the queue), however many records are still waiting in unsubmitted command lists.
    struct Upload {
        ID3D12Resource* buf = nullptr;
        uint8_t* map = nullptr;
        std::vector<uint64_t> owner;   // per slot: the frame that holds it, ~0ull = never used
        nr::gpu::Ticket uses[kRing];   // each buffer owns its slots, including grown blocks and other extents
    };
    std::vector<Upload> upload;
    // The execution completion token: this record's frame number, copied back once the model's dispatches have
    // run. A slot of it is written again only once the GPU has executed the record that last held it, so a copy
    // still waiting in an unsubmitted list never reads a newer number; a stack deep enough to outrun the block
    // gets a larger one, and the old block stays alive until the last record that reads it has run.
    ID3D12Resource* exec_token = nullptr;
    uint64_t* exec_token_map = nullptr;
    uint64_t token_slots = 0;
    ID3D12Resource* done = nullptr;       // readback buffer: [0..kDoneSlots) for copy completion, [kDoneSlots..2*kDoneSlots) for execution completion
    uint64_t* copy_map = nullptr;
    uint64_t* exec_map = nullptr;
    int64_t copied = -1;                  // newest frame where parameter copy to GPU has finished (upload slots recyclable)
    int64_t executed = -1;                // newest frame where all model dispatches have finished on GPU (resources releasable)
    int64_t last_frame = -1;              // newest frame recorded into this Built; -1: none
    nr::gpu::Uses gpu;
    void refresh_copied(uint64_t max_f) {
        if (!copy_map) return;
        for (uint32_t s = 0; s < kDoneSlots; ++s) {
            const uint64_t v = copy_map[s];
            if (v != ~uint64_t(0) && v <= max_f && (v % kDoneSlots) == s && int64_t(v) > copied)
                copied = int64_t(v);
        }
    }
    void refresh_executed(uint64_t max_f) {
        if (!exec_map) return;
        for (uint32_t s = 0; s < kDoneSlots; ++s) {
            const uint64_t v = exec_map[s];
            if (v != ~uint64_t(0) && v <= max_f && (v % kDoneSlots) == s && int64_t(v) > executed)
                executed = int64_t(v);
        }
    }
    uint32_t tables = 0;                          // per frame: the noise jobs (the frame that makes the table) + dispatches
    uint64_t slot_bytes = 0;                      // one frame's ring slot: the tables, then the params
    bool retargeted = false;
    bool table_uav = false;                       // the table buffer's state: UNORDERED_ACCESS (else COPY_DEST)
    uint64_t used = 0;
    int net = -1;
    std::map<uint64_t, std::shared_ptr<FeatureState>> features;
    ~Built() {
        for (auto& [n, k] : kern) release(k.pso);
        release(t_pre.pso); release(t_post.pso); release(t_grade.pso); release(t_post16.pso); release(t_grade16.pso);
        for (auto& u : upload) {
            if (u.map) u.buf->Unmap(0, nullptr);
            release(u.buf);
        }
        if (exec_token_map) exec_token->Unmap(0, nullptr);
        release(exec_token);
        if (copy_map) done->Unmap(0, nullptr);
        release(done);
        release(act); release(wgt); release(wgt_upload); release(stand); release(noise); release(table);
        release(act_e); release(wgt_e); release(wgt_e_upload);
    }
};

// What the build thread and the evaluate share.
struct Shared {
    std::mutex m;
    bool building = false;
    uint32_t bw = 0, bh = 0;
    int bnet = -1;
    std::chrono::steady_clock::time_point build_started{};
    std::shared_ptr<Built> done;                  // built, not yet taken by ready()
    std::map<std::tuple<uint32_t, uint32_t, int>, std::string> failed;   // extents & models that cannot run (not built again)
};

// build: the lowering and everything of the device's it needs. On the build thread.
std::shared_ptr<Built> build(ID3D12Device* device, ID3D12RootSignature* rs, uint32_t w,
                             uint32_t h, int want_net, std::string* why) {
    const auto t0 = std::chrono::steady_clock::now();
    NetPlanner planner = g_planner.load();
    if (!planner) { *why = "no network planner"; return nullptr; }
    auto b = std::make_shared<Built>();
    b->w = w; b->h = h;
    switch (arch()) {   // the lowering of the GPU's architecture (nr_gpu_arch.hpp), never one by default
    case nr::arch::GpuArch::rdna4: b->rdna3 = false; break;
    case nr::arch::GpuArch::rdna3: b->rdna3 = true; break;
    case nr::arch::GpuArch::unknown: case nr::arch::GpuArch::rdna1: case nr::arch::GpuArch::rdna2:
        *why = std::string("no network for this GPU (") + nr::arch::name(arch()) + ")";
        return nullptr;
    }
    // The kernel's record: FNV of its SPIR-V (g_<kern>.spv in the shaders the graph was lowered against; the temporal
    // kernels' own files).
    std::map<std::string, KernElf> elfs;
    const std::wstring aco = aco_folder();
    auto resolve_file = [&](const std::string& path, const KernElf** out) {
        std::vector<uint8_t> spv;
        if (!slurp(wide(path), &spv)) return false;
        std::string stem, err;
        KernElf k;
        // RDNA3: the gfx11 machine the driver took (the first kernel tries them: gfx1100 first)
        const uint32_t mach = b->rdna3 ? (gfx11_mach() ? gfx11_mach() : uint32_t(elf::kGfx11Mach[0])) : 0u;
        if (b->rdna3) rdna3_bindings(spv, &k.e4m3, &k.aliases);
        if (!elf::load_kernel_elf(b->plan.spv_dir, aco, spv, &k.elf_bytes, k.dims, &stem, &err, mach)) {
            nr::pe::log("[d3d12] %s: %s", path.c_str(), err.c_str());
            return false;
        }
        k.stem = stem;
        auto [it, _] = elfs.emplace(stem, std::move(k));
        *out = &it->second;
        return true;
    };
    auto resolve = [&](const std::string& kern, const KernElf** out) {
        return resolve_file(b->plan.spv_dir + "/g_" + kern + ".spv", out);
    };
    auto stem_of = [](const std::string& path) {
        const size_t s = path.find_last_of("/\\"), e = path.rfind(".spv");
        return path.substr(s == std::string::npos ? 0 : s + 1, e == std::string::npos ? std::string::npos : e - (s == std::string::npos ? 0 : s + 1));
    };
    std::map<std::string, const KernElf*> found;
    const KernElf *c_pre = nullptr, *c_post = nullptr, *c_grade = nullptr, *c_post16 = nullptr, *c_grade16 = nullptr;
    std::string missing;
    for (int attempt = 0; attempt < 2; ++attempt) {
        // CC_SMALL_FP8_EXPERIMENTAL first, as the runtime picks it; a kernel without a record: CC_SMALL_FP8 (the
        // runtime's own "anything missing: the small model").
        const bool try_experimental = (want_net == nr::kSmallFp8Experimental && attempt == 0);
        const int plan_model = (want_net == nr::kSmallFp8Experimental && attempt == 1) ? nr::kSmallFp8 : want_net;
        if (!planner(w, h, plan_model, try_experimental, &b->plan, why)) return nullptr;
        found.clear(); missing.clear();
        c_pre = c_post = c_grade = c_post16 = c_grade16 = nullptr;
        const auto& t = b->plan.temporal;
        for (size_t i = 0; i < b->plan.disp.size(); ++i) {
            const auto& d = b->plan.disp[i];
            if (int(i) == t.pre || int(i) == t.post || found.count(d.kern)) continue;   // replaced by the temporal kernels
            const KernElf* c = nullptr;
            if (!resolve(d.kern, &c)) { missing = d.kern; break; }
            found[d.kern] = c;
        }
        if (missing.empty() && !b->plan.noise.empty()) {
            const KernElf* c = nullptr;
            if (resolve("noisefield", &c)) found["noisefield"] = c; else missing = "noisefield";
        }
        if (missing.empty() && !resolve_file(t.pre_spv, &c_pre)) missing = stem_of(t.pre_spv);
        if (missing.empty() && !resolve_file(t.post_spv, &c_post)) missing = stem_of(t.post_spv);
        if (missing.empty() && !t.post_grade_spv.empty() && !resolve_file(t.post_grade_spv, &c_grade))
            missing = stem_of(t.post_grade_spv);
        // The RGBA16F history's posts: both, or the history stays RGBA32F (as the runtime's h16_ok)
        if (missing.empty() && !t.post16_spv.empty() && resolve_file(t.post16_spv, &c_post16) &&
            !t.post16_grade_spv.empty() && !resolve_file(t.post16_grade_spv, &c_grade16))
            c_post16 = nullptr;
        if (missing.empty()) break;
        if (b->plan.model.rfind("CC_SMALL_FP8_EXPERIMENTAL", 0) != 0) break;
        nr::pe::log("[d3d12] the %s network: kernel %s has no ACO record; CC_SMALL_FP8 instead", b->plan.model.c_str(),
                    missing.c_str());
    }
    if (!missing.empty()) { *why = "kernel " + missing + " has no ACO record (" + b->plan.model + ")"; return nullptr; }
    if (b->plan.temporal.pre < 0 || b->plan.temporal.post < 0) { *why = "the plan has no temporal pre / post block"; return nullptr; }
    // The table in a buffer of its own (the network shaders' manifest says noise_buffer) is what this side binds
    if (!b->plan.noise.empty() && !b->plan.noise_in_buffer) {
        *why = "the shader folder keeps the noise table in the weights (not the shaders the ACO records were made from)"; return nullptr;
    }
    b->net = nr::network_choice(b->plan.model.substr(0, b->plan.model.find(' ')));
    if (b->net < 0) b->net = want_net >= 0 ? want_net : nr::kSmallFp8;
    else if (nr::network_optimized(b->net)) {
        if (b->plan.model.find("(layers: quality)") != std::string::npos) b->net = nr::kSilverAardwoldOptimizedFp8;
        else if (b->plan.model.find("(layers: performance)") != std::string::npos) b->net = nr::kSilverAardwoldOptimizedPerformance;
        else if (b->plan.model.find("(layers: ultra performance)") != std::string::npos) b->net = nr::kSilverAardwoldOptimizedUltra;
        else if (nr::network_optimized(want_net)) b->net = want_net;
    }
    auto make = [&](const std::string& kern, const KernElf& c, Built::Kern* k) {
        if (!family_of(kern, &k->fam)) { *why = "no binding table for kernel " + kern; return false; }
        std::string err;
        uint32_t mach = 0;
        if (!elf::create_pso_from_elf(device, c.elf_bytes.data(), c.elf_bytes.size(), c.dims, &k->pso, &err, &mach)) {
            *why = "CreateComputePipelineFromElf failed for kernel " + kern + ": " + err; return false;
        }
        if (b->rdna3 && !gfx11_mach() && mach) {
            set_gfx11_mach(mach);
            nr::pe::log("[d3d12] RDNA3: the driver takes the gfx11 machine 0x%02x", unsigned(mach));
        }
        k->e4m3 = c.e4m3;
        k->aliases = c.aliases;
        return true;
    };
    for (const auto& [kern, c] : found)
        if (!make(kern, *c, &b->kern[kern])) return nullptr;
    if (!make(stem_of(b->plan.temporal.pre_spv), *c_pre, &b->t_pre) ||
        !make(stem_of(b->plan.temporal.post_spv), *c_post, &b->t_post) ||
        (c_grade && !make(stem_of(b->plan.temporal.post_grade_spv), *c_grade, &b->t_grade)) ||
        (c_post16 && !make(stem_of(b->plan.temporal.post16_spv), *c_post16, &b->t_post16)) ||
        (c_grade16 && !make(stem_of(b->plan.temporal.post16_grade_spv), *c_grade16, &b->t_grade16)))
        return nullptr;
    // The network memory (retarget): the arena zero (a new committed resource), the weights to upload once.
    const uint64_t wbytes = b->plan.weights.size();
    {
        nr::vram::Scope s_act(nr::vram::kActivations, b->net);
        b->act = buffer(device, D3D12_HEAP_TYPE_DEFAULT, b->plan.act_total, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        nr::vram::track_d3d12(device, b->act, nr::vram::kActivations);
    }
    {
        nr::vram::Scope s_wgt(nr::vram::kWeights, b->net);
        b->wgt = buffer(device, D3D12_HEAP_TYPE_DEFAULT, wbytes, D3D12_RESOURCE_STATE_COPY_DEST, true);
        nr::vram::track_d3d12(device, b->wgt, nr::vram::kWeights);
    }
    b->wgt_upload = buffer(device, D3D12_HEAP_TYPE_UPLOAD, wbytes, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    if (b->rdna3) {   // the FP16 twins (nr_graph.cpp / reload_network on RDNA3): the arena's zero, the weights' converted
        {
            nr::vram::Scope s_act(nr::vram::kActivations, b->net);
            b->act_e = buffer(device, D3D12_HEAP_TYPE_DEFAULT, b->plan.act_total * 2, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            nr::vram::track_d3d12(device, b->act_e, nr::vram::kActivations);
        }
        {
            nr::vram::Scope s_wgt(nr::vram::kWeights, b->net);
            b->wgt_e = buffer(device, D3D12_HEAP_TYPE_DEFAULT, wbytes * 2, D3D12_RESOURCE_STATE_COPY_DEST, true);
            nr::vram::track_d3d12(device, b->wgt_e, nr::vram::kWeights);
        }
        b->wgt_e_upload = buffer(device, D3D12_HEAP_TYPE_UPLOAD, wbytes * 2, D3D12_RESOURCE_STATE_GENERIC_READ, false);
        if (!b->act_e || !b->wgt_e || !b->wgt_e_upload) { *why = "the RDNA3 FP16 twins could not be made"; return nullptr; }
        void* em = nullptr;
        if (FAILED(b->wgt_e_upload->Map(0, nullptr, &em)) || !em) { *why = "the FP16 weights upload could not be mapped"; return nullptr; }
        uint16_t to_f16[256];
        for (int i = 0; i < 256; ++i) to_f16[i] = tin::f_to_f16(tin::e4m3_to_f(uint8_t(i)));
        auto* o = static_cast<uint16_t*>(em);
        for (uint64_t i = 0; i < wbytes; ++i) o[i] = to_f16[b->plan.weights[size_t(i)]];
        b->wgt_e_upload->Unmap(0, nullptr);
    }
    b->stand = buffer(device, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    b->noise = buffer(device, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    b->tables = uint32_t(b->plan.noise.size() + b->plan.disp.size());
    b->slot_bytes = uint64_t(b->tables) * kTableBytes + kParamsBytes;
    {
        nr::vram::Scope s_buf(nr::vram::kUnset, b->net);
        b->table = buffer(device, D3D12_HEAP_TYPE_DEFAULT, b->slot_bytes, D3D12_RESOURCE_STATE_COPY_DEST, true);
        nr::vram::track_d3d12(device, b->table, nr::vram::kUnset);
    }
    if (!b->act || !b->wgt || !b->wgt_upload || !b->stand || !b->noise || !b->table) {
        *why = "the network memory could not be made"; return nullptr;
    }
    for (ID3D12Resource* r : {b->act, b->wgt, b->stand, b->noise, b->table})
        if ((r->GetGPUVirtualAddress() >> 48) != 0) { *why = "a buffer address above 48 bits"; return nullptr; }
    {
        void* wm = nullptr;
        if (FAILED(b->wgt_upload->Map(0, nullptr, &wm)) || !wm) { *why = "the weights upload could not be mapped"; return nullptr; }
        if (wbytes) std::memcpy(wm, b->plan.weights.data(), wbytes);
        b->wgt_upload->Unmap(0, nullptr);
    }
    std::vector<uint8_t>().swap(b->plan.weights);
    {
        Built::Upload u;
        u.buf = buffer(device, D3D12_HEAP_TYPE_UPLOAD, (b->slot_bytes + 8) * kRing, D3D12_RESOURCE_STATE_GENERIC_READ, false);
        u.owner.assign(kRing, ~uint64_t(0));
        if (!u.buf) { *why = "the network memory could not be made"; return nullptr; }
        void* m = nullptr;
        if (FAILED(u.buf->Map(0, nullptr, &m)) || !m) { *why = "the table upload could not be mapped"; return nullptr; }
        u.map = static_cast<uint8_t*>(m);
        b->upload.push_back(std::move(u));
    }
    b->exec_token = buffer(device, D3D12_HEAP_TYPE_UPLOAD, kDoneSlots * 8, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    if (!b->exec_token) { *why = "the execution completion token buffer could not be made"; return nullptr; }
    b->token_slots = kDoneSlots;
    {
        void* m = nullptr;
        if (FAILED(b->exec_token->Map(0, nullptr, &m)) || !m) { *why = "the execution completion token could not be mapped"; return nullptr; }
        b->exec_token_map = static_cast<uint64_t*>(m);
    }
    b->done = buffer(device, D3D12_HEAP_TYPE_READBACK, kDoneSlots * 16, D3D12_RESOURCE_STATE_COPY_DEST, false);
    if (!b->done) { *why = "the completion readback could not be made"; return nullptr; }
    {
        void* m = nullptr;
        if (FAILED(b->done->Map(0, nullptr, &m)) || !m) { *why = "the completion readback could not be mapped"; return nullptr; }
        b->copy_map = static_cast<uint64_t*>(m);
        b->exec_map = b->copy_map + kDoneSlots;
        std::memset(b->copy_map, 0xFF, kDoneSlots * 16);   // never a frame number
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    nr::pe::log("[d3d12] network %s for %ux%u (working %ux%u): %zu dispatches, %zu kernels + temporal %s / %s%s, %zu noise jobs, "
                "weights %llu B, arena %llu B, built in %.2fs%s%s", b->plan.model.c_str(), w, h, b->plan.work_w, b->plan.work_h,
                b->plan.disp.size(), b->kern.size(), stem_of(b->plan.temporal.pre_spv).c_str(),
                stem_of(b->plan.temporal.post_spv).c_str(),
                b->t_post16.pso ? (b->t_grade.pso ? " (+ Style grade, + RGBA16F history)" : " (+ RGBA16F history)")
                                : b->t_grade.pso ? " (+ Style grade)" : "", b->plan.noise.size(),
                (unsigned long long)wbytes, (unsigned long long)b->plan.act_total, s, b->plan.note.empty() ? "" : "; ",
                b->plan.note.c_str());
    return b;
}

class AcoNetwork final : public D3d12PassthroughNetwork {
public:
    AcoNetwork() : shared_(std::make_shared<Shared>()) {}
    ~AcoNetwork() override {
        {
            std::lock_guard<std::recursive_mutex> l(state_m_);
            current_shared_.reset();
            current_ = nullptr;
            nets_.clear();
        }
        retired_.clear();
        for (auto& r : retire_upload_) if (r.upload) r.upload->Release();
        if (stamps_map_) stamps_->Unmap(0, nullptr);
        release(stamps_); release(queries_);
        release(rs_);
        if (device_) device_->Release();
    }
    const char* name() const override { return name_.c_str(); }

    // Runtime::release_feature releases exactly this history id. The session/backend owns stack expansion.
    void release_feature(uint64_t feature) override {
        plain_seeds_.erase(feature);
        std::set<const Built*> seen;
        for (auto& [key, b] : nets_) {
            if (!b || !seen.insert(b.get()).second) continue;
            auto it = b->features.find(feature);
            if (it == b->features.end()) continue;
            RetiredUse r;
            r.net = b;
            r.frame = it->second ? it->second->last_frame : -1;
            if (it->second) r.uses = it->second->gpu;
            r.hold = std::move(it->second);
            retired_features_.push_back(std::move(r));
            b->features.erase(it);
        }
    }

    bool create(ID3D12Device* device, const char** why) override {
        static std::string w;
        if (!elf::create_carrier_root_signature(device, 1, &rs_, &w)) { *why = w.c_str(); return false; }
        // The network's GPU time: two timestamps per measured recording, read back after its completion. The
        // tick rate from a queue of our own (the device's timestamp frequency; nothing is submitted to it).
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 2 * kRing;
        if (SUCCEEDED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&queries_)))) {
            stamps_ = buffer(device, D3D12_HEAP_TYPE_READBACK, 16 * kRing, D3D12_RESOURCE_STATE_COPY_DEST, false);
            ID3D12CommandQueue* q = nullptr;
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            UINT64 hz = 0;
            if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) && q) { q->GetTimestampFrequency(&hz); q->Release(); }
            void* m = nullptr;
            if (stamps_ && hz && SUCCEEDED(stamps_->Map(0, nullptr, &m)) && m) { stamps_map_ = static_cast<uint8_t*>(m); hz_ = double(hz); }
        }
        if (!stamps_map_) nr::pe::log("[d3d12] the network's GPU time cannot be measured (no timestamp queries)");
        device->AddRef();
        device_ = device;
        name_ = "the ACO network (building)";
        return true;
    }

    bool ready(ID3D12Device*, uint32_t w, uint32_t h, std::string* why) override {
        sweep();
        Built* const was = current_;   // the network the last ready() settled on (the stale-history rule's "switch")
        {
            std::lock_guard<std::recursive_mutex> l(state_m_);
            current_shared_.reset();
            current_ = nullptr;
        }
        building_ = false;
        // Any width (lin_pitch pads the rows, as RADV's linear images); RDNA4's pitch word holds at most 16384 texels
        if (!w || !h || lin_pitch(w, 8) > 16384u) {
            *why = "a network extent of " + std::to_string(w) + " x " + std::to_string(h) + " (rows of at most 16384 texels)";
            return false;
        }
        NetModelQuery query = g_model_query.load();
        const int q = query ? query() : -1;
        const int want = q >= 0 ? q : (last_want_ >= 0 ? last_want_ : nr::kSmallFp8);
        {
            std::lock_guard<std::recursive_mutex> l(state_m_);
            last_want_ = want;
        }
        const auto key = std::make_tuple(w, h, want);
        if (auto it = nets_.find(key); it != nets_.end() && it->second->initialization &&
            it->second->initialization->never_submitted()) {
            const auto dropped = it->second;
            RetiredNet retired;
            retired.net = dropped;
            retired_.push_back(std::move(retired));
            {
                std::lock_guard<std::recursive_mutex> l(state_m_);
                for (auto alias = nets_.begin(); alias != nets_.end();) {
                    if (alias->second == dropped) alias = nets_.erase(alias);
                    else ++alias;
                }
            }
        }
        if (auto it = nets_.find(key); it != nets_.end()) {
            std::lock_guard<std::recursive_mutex> l(state_m_);
            // Taken up again after serving nothing for 250 ms: its features start afresh, as after a Reset
            // (Session::select_runtime's kStaleHistory -> Runtime::forget_history); layers taking turns every frame
            // keep theirs.
            if (was != it->second.get() && it->second->last != std::chrono::steady_clock::time_point{} &&
                std::chrono::steady_clock::now() - it->second->last > std::chrono::milliseconds(250)) {
                for (auto& fe : it->second->features)
                    if (fe.second) fe.second->latch = false;
                nr::pe::log("[d3d12] network %ux%u taken up again: its history starts afresh", it->second->w, it->second->h);
            }
            current_shared_ = it->second;
            current_ = current_shared_.get();
            current_->used = tick_;
            name_ = "the ACO network, " + current_->plan.model + ", " + std::to_string(current_->plan.disp.size()) + " dispatches, temporal";
            return true;
        }
        std::lock_guard<std::mutex> lock(shared_->m);
        if (shared_->done) {
            auto done = std::move(shared_->done);
            // The build may have fallen back (CC_SMALL_FP8_EXPERIMENTAL -> CC_SMALL_FP8 when a kernel has no
            // ACO record). The result is registered under what it IS and under what was ASKED (shared_->bw/bh/
            // bnet, still the request this build answered), so the next ask of either finds it instead of
            // starting the same build again every frame.
            const auto done_key = std::make_tuple(done->w, done->h, done->net);
            const auto ask_key = std::make_tuple(shared_->bw, shared_->bh, shared_->bnet);
            if (nets_.size() >= kMaxExtents) {
                auto lru = nets_.begin();
                for (auto it = nets_.begin(); it != nets_.end(); ++it) if (it->second->used < lru->second->used) lru = it;
                RetiredNet rn;
                rn.net = std::move(lru->second);
                retired_.push_back(std::move(rn));
                {
                    std::lock_guard<std::recursive_mutex> l(state_m_);
                    nets_.erase(lru);
                }
            }
            auto& b = nets_[done_key];
            b = std::move(done);
            b->used = tick_;
            if (ask_key != done_key) {   // a second name for the same net, not another extent
                auto& alias = nets_[ask_key];
                alias = b;
                alias->used = tick_;
            }
            if (done_key == key || ask_key == key) {
                std::lock_guard<std::recursive_mutex> l(state_m_);
                current_shared_ = b;
                current_ = current_shared_.get();
                name_ = "the ACO network, " + b->plan.model + ", " + std::to_string(b->plan.disp.size()) + " dispatches, temporal";
                nr::pe::log("[d3d12] network ACTIVE for %ux%u: %s (frames of this extent now run on the D3D12 network)", w, h, name_.c_str());
                return true;
            }
        }
        const auto f = shared_->failed.find(key);
        if (f != shared_->failed.end()) { *why = f->second; return false; }
        if (!shared_->building) {
            shared_->building = true;
            shared_->bw = w; shared_->bh = h; shared_->bnet = want;
            shared_->build_started = std::chrono::steady_clock::now();
            shared_->done.reset();
            device_->AddRef();
            rs_->AddRef();
            nr::pe::log("[d3d12] building the %s network for %ux%u (until it is done these frames pass through with no NR; "
                        "the bridge is not used)", nr::network_label(want), w, h);
            std::thread([sh = shared_, dev = device_, rs = rs_, w, h, want] {
                std::string e;
                std::shared_ptr<Built> b;
                try { b = build(dev, rs, w, h, want, &e); } catch (const std::exception& x) { e = x.what(); }
                if (!b) nr::pe::log("[d3d12] ERROR: the network for %ux%u cannot run: %s (the Vulkan bridge)", w, h, e.c_str());
                {
                    std::lock_guard<std::mutex> l(sh->m);
                    if (b) sh->done = std::move(b); else sh->failed[{w, h, want}] = e;
                    sh->building = false;
                }
                rs->Release();
                dev->Release();
            }).detach();
        }
        *why = std::string("the ") + nr::network_label(shared_->bnet) + " network for " + std::to_string(shared_->bw) + "x" + std::to_string(shared_->bh) + " is building";
        building_ = true;   // it will run these frames (no bridge meanwhile)
        return false;
    }
    bool building() const override { return building_; }
    PassthroughNetStatus status() const override {
        PassthroughNetStatus s;
        {
            std::lock_guard<std::recursive_mutex> l(state_m_);
            s.running_net = current_ ? current_->net : -1;
            s.want_net = last_want_;
        }
        std::lock_guard<std::mutex> l(shared_->m);
        s.building = shared_->building;
        s.building_net = shared_->building ? shared_->bnet : -1;
        if (shared_->building) {
            s.building_seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - shared_->build_started).count();
        }
        s.gpu_ms = gpu_avg_ms_.load();
        return s;
    }
    void controls(const nr::D3d12NetControls& c) override { controls_ = c; }
    void history_f16(bool on) override {   // NR_HIST16 in the environment wins (Runtime::set_history_f16)
        if (std::getenv("NR_HIST16")) return;
        f16_set_ = true; f16_want_ = on;
    }
    void style_grade(bool on) override { grade_set_ = true; grade_want_ = on; }
    void temporal(const NetTemporal& t) override { temporal_ = t; }
    uint32_t history_texel_bytes() const override {   // (record's own rule for the precision below)
        const Built* b = current_;
        if (!b) return 0;
        return (f16_set_ ? f16_want_ : b->plan.temporal.history_f16) && b->t_post16.pso ? 8u : 16u;
    }
    bool motion_without_depth() const override {
        if (current_) return current_->plan.temporal.motion_without_depth;
        if (!nets_.empty()) return nets_.begin()->second->plan.temporal.motion_without_depth;
        return true;
    }

    bool record(ID3D12GraphicsCommandList* cmd, const LinearFrame& in, const LinearFrame& out, const char** why) override {
        const auto c0 = std::chrono::steady_clock::now();
        Built* b = current_;
        if (!b || b->w != in.width || b->h != in.height) { *why = "no network for this extent"; return false; }
        b->last = c0;
        if ((in.va & 255) || (out.va & 255) || (temporal_.motion_va & 255) || (temporal_.depth_va & 255) || (temporal_.history_va & 255)) { *why = "a linear buffer is not 256-byte aligned"; return false; }
        const auto& tp = b->plan.temporal;
        // [Network] History Precision as the caller last said (history_f16), the plan's ini value before that; RGBA16F
        // only when the folder has its posts (the runtime's h16_ok).
        const bool f16 = (f16_set_ ? f16_want_ : tp.history_f16) && b->t_post16.pso;
        // A change of the precision is a cut for every feature of this network, not only the one recording now
        // (Runtime::Impl::switch_history clears them all): one left in the old format would otherwise come back
        // with a history from before the switch.
        if (b->hist_f16 >= 0 && b->hist_f16 != int(f16)) {
            for (auto it = b->features.begin(); it != b->features.end();) {
                RetiredUse r;
                r.net = current_shared_;
                if (it->second) { r.frame = it->second->last_frame; r.uses = it->second->gpu; }
                r.hold = std::move(it->second);
                retired_features_.push_back(std::move(r));
                it = b->features.erase(it);
            }
            nr::pe::log("[d3d12] history images %s ([Network] History Precision %s): every feature starts afresh",
                        f16 ? "RGBA16F" : "RGBA32F", f16 ? "16" : "32");
        }
        b->hist_f16 = int(f16);
        // This frame's feature (Runtime::record_all): its state, made on its first frame. A feature whose history is
        // in the other format starts afresh (Runtime::Impl::switch_history: a switch is a cut, as a network switch is).
        FeatureState* fs = nullptr;
        if (auto it = b->features.find(temporal_.feature); it != b->features.end() && it->second && it->second->f16 == f16) {
            fs = it->second.get();
        } else {
            if (it != b->features.end()) {
                RetiredUse r;
                r.net = current_shared_;
                if (it->second) { r.frame = it->second->last_frame; r.uses = it->second->gpu; }
                r.hold = std::move(it->second);
                retired_features_.push_back(std::move(r));
                b->features.erase(it);
                nr::pe::log("[d3d12] history images %s ([Network] History Precision %s): the history starts afresh",
                            f16 ? "RGBA16F" : "RGBA32F", f16 ? "16" : "32");
            }
            // No age-based eviction: a feature's history stays until the caller says that feature is gone
            // (release_feature), as the session's entry.stack does - an "inactive" window would silently reset the
            // history of a feature that still exists.
            auto fresh = std::make_shared<FeatureState>();
            fresh->f16 = f16;
            {
                nr::vram::Scope s_hist(nr::vram::kUnset, b->net);
                for (auto& hh : fresh->hist) {
                    hh = buffer(device_, D3D12_HEAP_TYPE_DEFAULT,
                                uint64_t(lin_pitch(b->w, f16 ? 8 : 16)) * b->h * (f16 ? 8 : 16),   // (rows of lin_pitch texels)
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
                    nr::vram::track_d3d12(device_, hh, nr::vram::kUnset);
                }
            }
            if (!fresh->hist[0] || !fresh->hist[1]) { *why = "the history images could not be made"; return false; }
            fs = fresh.get();
            b->features[temporal_.feature] = std::move(fresh);
        }
        fs->used = tick_;
        // The gate and the seed, the runtime's rule (Runtime::record_all):
        // with motion: the first-frame latch, DLSSNR.Reset, the input domain; the pre block's seed
        // a counter zeroed by any of them (frames with seed 0 use the noise table once it is made).
        // without motion: gate is false (no history accumulation); the seed is 0 unless
        // Controls::noise_every_frame, then a separate per-feature counter across no-MV frames.
        bool gate = false;
        uint32_t pre_seed = 0;
        if (temporal_.have_motion) {
            const bool domain_changed = fs->latch && fs->domain != controls_.reversible;
            gate = fs->latch && !temporal_.reset && !domain_changed;
            if (!fs->latch || temporal_.reset || domain_changed) fs->seed = 0;
            fs->domain = controls_.reversible;
            pre_seed = fs->seed++;
            if (controls_.seed_fixed) pre_seed = 0;
        } else {
            uint32_t& seed = temporal_.feature ? plain_seeds_[temporal_.feature] : plain_seed_;
            if (temporal_.reset) seed = 0;
            if (controls_.noise_every_frame && !controls_.seed_fixed) pre_seed = seed++;
            else seed = 0;
        }

        const uint64_t act_va = b->act->GetGPUVirtualAddress(), wgt_va = b->wgt->GetGPUVirtualAddress();
        // RDNA3's FP16 twins (0 on RDNA4)
        const uint64_t act_e_va = b->act_e ? b->act_e->GetGPUVirtualAddress() : 0;
        const uint64_t wgt_e_va = b->wgt_e ? b->wgt_e->GetGPUVirtualAddress() : 0;
        const uint64_t stand_va = b->stand->GetGPUVirtualAddress();
        const uint64_t table_va = b->table->GetGPUVirtualAddress();
        const uint64_t params_va = table_va + uint64_t(b->tables) * kTableBytes;
        // (Passes (fast) Interleaved: the pass's history as it made it, read in place of the feature's; the post block still
        // stores into the feature's other image, the next frame's history)
        const uint64_t hist_va = temporal_.history_va ? temporal_.history_va : fs->hist[fs->hcur]->GetGPUVirtualAddress();
        const uint64_t store_va = fs->hist[fs->hcur ^ 1u]->GetGPUVirtualAddress();
        NetControlsPatch patch = g_patch.load();
        // [Network] Style Grade as the caller last said (style_grade), the plan's ini value before that (the Vulkan
        // runtime's style_grade && style_grade_on)
        const bool grade = b->t_grade.pso && controls_.style != 0 && !controls_.no_grade &&
                           (grade_set_ ? grade_want_ : b->plan.style_grade);

        // retarget: the weights with their tail once. Recorded only after this record has a slot: a record that
        // declines below must not leave the weight copy in the list, or the next one would transition b->wgt from
        // COPY_DEST when it is already UNORDERED_ACCESS.
        const bool first = !b->retargeted;
        // The noise table, once the recordings before asked for it (make_noise): this record makes it before anything
        // reads binding 5, with the noise jobs' tables first in the ring.
        const bool make_noise = b->want_noise && !b->noise_real && !b->plan.noise.empty();
        ID3D12Resource* noise_made = nullptr;
        if (make_noise) {
            noise_made = buffer(device_, D3D12_HEAP_TYPE_DEFAULT, b->plan.noise_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            if (!noise_made) b->want_noise = false;   // (no memory for it: seed 0 keeps computing the noise, the same bytes)
        }
        // The tables of this frame: [noise jobs (the frame that makes the table)] [every dispatch] [the params].
        const uint32_t n_noise = noise_made ? uint32_t(b->plan.noise.size()) : 0;
        const uint32_t base = uint32_t(b->plan.noise.size());   // the dispatches' tables keep their place
        // An upload slot of this record's own. The CPU writes it here and the CopyBufferRegion below reads it
        // when the list runs; its last 8 bytes carry this record's frame number, which a second recorded copy
        // lands in the readback buffer. A slot is only taken again once that number has landed (copied: this
        // record's copy, and every earlier one in the queue, has run) - so the many networks a multi-pass stack
        // records into one not-yet-submitted command list each keep their own tables, however many there are.
        const uint64_t frame = frames_;
        b->refresh_copied(frame);
        // This record's execution token, into a slot whose previous holder the GPU has executed. A stack of many
        // passes records that many frames into one still-unsubmitted list, so the block grows when the CPU has
        // outrun it; the old one is retired behind the completion of the last record that reads it. Done before
        // anything of this record is claimed, so a failure here leaves the stage exactly as it was.
        if (frame >= b->token_slots && int64_t(frame - b->token_slots) > b->executed && b->exec_token) {
            const uint64_t slots = b->token_slots * 2;
            ID3D12Resource* grown = buffer(device_, D3D12_HEAP_TYPE_UPLOAD, slots * 8, D3D12_RESOURCE_STATE_GENERIC_READ, false);
            void* tm = nullptr;
            if (!grown || FAILED(grown->Map(0, nullptr, &tm)) || !tm) {
                release(grown);
                release(noise_made);
                *why = "the execution completion token could not be made";
                return false;
            }
            RetiredUse rt;
            rt.net = current_shared_;
            rt.upload = b->exec_token;
            rt.uses = b->gpu;
            rt.frame = b->last_frame;   // the last record that reads the old block (-1: none did)
            retire_upload_.push_back(std::move(rt));
            b->exec_token = grown;
            b->exec_token_map = static_cast<uint64_t*>(tm);
            b->token_slots = slots;
        }
        Built::Upload* slot_buf = nullptr;
        uint32_t slot_i = 0;
        const nr::gpu::Ticket record_ticket = split::book(cmd);
        for (auto& u : b->upload) {
            for (uint32_t i = 0; i < kRing; ++i) {
                const bool free = u.uses[i] ? split::completed(u.uses[i])
                                                  : (u.owner[i] == ~uint64_t(0) ||
                                                     (b->copied >= 0 && u.owner[i] <= uint64_t(b->copied)));
                if (free) { slot_buf = &u; slot_i = i; break; }
            }
            if (slot_buf) break;
        }
        if (!slot_buf) {   // every slot still in flight: another buffer of them, however many it takes
            Built::Upload u;
            u.buf = buffer(device_, D3D12_HEAP_TYPE_UPLOAD, (b->slot_bytes + 8) * kRing, D3D12_RESOURCE_STATE_GENERIC_READ, false);
            if (u.buf) {
                void* m = nullptr;
                if (SUCCEEDED(u.buf->Map(0, nullptr, &m)) && m) {
                    u.map = static_cast<uint8_t*>(m);
                    u.owner.assign(kRing, ~uint64_t(0));
                    b->upload.push_back(std::move(u));
                    slot_buf = &b->upload.back();
                    slot_i = 0;
                } else {
                    release(u.buf);
                }
            }
        }
        if (slot_buf) slot_buf->uses[slot_i] = record_ticket;   // this slot's writer, for the next record's reuse

        if (!slot_buf) {   // could not grow either (out of memory): no slot may be taken - reusing a held one
                           // would corrupt a copy the GPU has not read; this record declines, the frame passes
            release(noise_made);
            static bool said = false;
            if (!said) {
                said = true;
                nr::pe::log("[d3d12] the network's upload slots are all in flight and no more could be made; "
                            "this evaluate is not recorded");
            }
            return false;
        }
        b->last_frame = int64_t(frame);
        if (first) b->initialization = record_ticket;
        touch_recording(b->gpu, record_ticket);
        if (fs) {
            fs->last_frame = int64_t(frame);
            touch_recording(fs->gpu, record_ticket);
        }
        slot_buf->owner[slot_i] = frame;
        if (first) {
            cmd->CopyBufferRegion(b->wgt, 0, b->wgt_upload, 0, b->wgt->GetDesc().Width);
            transition(cmd, b->wgt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (b->wgt_e) {
                cmd->CopyBufferRegion(b->wgt_e, 0, b->wgt_e_upload, 0, b->wgt_e->GetDesc().Width);
                transition(cmd, b->wgt_e, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        }
        if (noise_made) {   // the stand-in goes behind every recording that bound it (this one included)
            RetiredUse r;
            r.net = current_shared_;
            r.upload = b->noise;
            r.uses = b->gpu;
            r.frame = int64_t(frame);
            retire_upload_.push_back(std::move(r));
            b->noise = noise_made;
            b->noise_real = true;
            b->want_noise = false;
            nr::pe::log("[d3d12] the noise table made (%llu bytes): frames keep seed 0", (unsigned long long)b->plan.noise_bytes);
        }
        const uint64_t noise_va = b->noise->GetGPUVirtualAddress();
        const uint64_t noise_size = b->noise->GetDesc().Width;
        const uint64_t slot_off = uint64_t(slot_i) * (b->slot_bytes + 8);
        uint8_t* ring = slot_buf->map + slot_off;
        std::memcpy(ring + b->slot_bytes, &frame, 8);
        const uint64_t token_off = (frame % b->token_slots) * 8;
        b->exec_token_map[token_off / 8] = frame;
        std::vector<const Built::Kern*> kern(b->tables, nullptr);
        const uint32_t hist_format = f16 ? kFormatRgba16f : kFormatRgba32f;
        auto table = [&](uint32_t i, const Built::Kern& k, const std::vector<uint8_t>& push, uint32_t gx, uint32_t gy,
                         uint32_t gz) {
            uint8_t* t = ring + uint64_t(i) * kTableBytes;
            std::memset(t, 0, kTableBytes);
            size_t at = 0;
            const auto put_res = [&](uint32_t* d, Res r, bool twin) {   // twin: RDNA3's e4m3 view (the arena's / weights' twin)
                if (r == PARAMS) put_buf(d, params_va, 48);
                else if (r == NOISE) put_buf(d, noise_va, noise_size);
                else if (twin && r == ACT) put_buf(d, act_e_va, b->plan.act_total * 2);
                else if (twin && r == WGT) put_buf(d, wgt_e_va, b->wgt_e->GetDesc().Width);
                else put_buf(d, r == ACT ? act_va : wgt_va, r == ACT ? b->plan.act_total : b->wgt->GetDesc().Width);
            };
            for (size_t bi = 0; bi < k.fam.buffers.size(); ++bi) {
                put_res(reinterpret_cast<uint32_t*>(t + at), k.fam.buffers[bi], b->rdna3 && k.e4m3.count(uint32_t(bi)) != 0);
                at += 16;
            }
            for (Img m : k.fam.images) {
                auto* d = reinterpret_cast<uint32_t*>(t + at);
                const uint32_t in_format = in.bytes == uint64_t(lin_pitch(b->w, 8)) * b->h * 8 ? kFormatRgba16f : kFormatRgba32f;
                const uint32_t out_format = out.bytes == uint64_t(lin_pitch(b->w, 8)) * b->h * 8 ? kFormatRgba16f : kFormatRgba32f;
                const auto put_img = [&](uint32_t* dd, uint64_t va, uint32_t iw, uint32_t ih, uint32_t fmt) {
                    if (b->rdna3) put_image_gfx11(dd, va, iw, ih, fmt);   // (its rows: 256-byte aligned, no pitch word)
                    else put_image(dd, va, iw, ih, fmt, lin_pitch(iw, fmt == kFormatRgba16f ? 8u : 16u));
                };
                const uint32_t mv_format = temporal_.motion_f16 ? kFormatRgba16f : kFormatRgba32f;
                switch (m) {
                case TEX_IN: put_img(d, in.va, b->w, b->h, in_format); break;
                case SURF0: put_img(d, out.va, b->w, b->h, out_format); break;
                case SURF1: put_img(d, stand_va, 1, 1, kFormatRgba32f); break;
                case FLOW: put_img(d, temporal_.motion_va, b->w, b->h, mv_format); break;
                case HIST: put_img(d, hist_va, b->w, b->h, hist_format); break;
                case HIST_STORE: put_img(d, store_va, b->w, b->h, hist_format); break;
                case DEPTH:
                    if (temporal_.depth_va) put_img(d, temporal_.depth_va, b->w, b->h, kFormatRgba32f);
                    else put_img(d, stand_va, 1, 1, kFormatRgba32f);
                    break;
                }
                if (m == TEX_IN) { put_mirror_nearest_sampler(d + 8); at += 48; }
                else if (m == FLOW || m == HIST || m == DEPTH) {
                    if (b->rdna3) put_linear_clamp_sampler_gfx11(d + 8); else put_linear_clamp_sampler(d + 8);
                    at += 48;
                } else at += 32;
            }
            // RDNA3's alias bindings, after the images in binding order: a raw-word view of the buffer at binding k,
            // the buffer itself (never its twin - nrvk.hpp)
            for (uint32_t a : k.aliases) {
                const uint32_t bk = a - kAliasBase;
                if (bk < k.fam.buffers.size()) put_res(reinterpret_cast<uint32_t*>(t + at), k.fam.buffers[bk], false);
                at += 16;
            }
            std::memcpy(t + kTablePushOff, push.data(), std::min<size_t>(push.size(), kTableGridOff - kTablePushOff));
            const uint32_t grid[3] = {gx, gy, gz};
            std::memcpy(t + kTableGridOff, grid, sizeof grid);
            kern[i] = &k;
        };
        for (uint32_t i = 0; i < n_noise; ++i) {
            const auto& j = b->plan.noise[i];
            std::vector<uint8_t> push(20);
            std::memcpy(push.data(), &j, 20);
            table(i, b->kern.at("noisefield"), push, (j.width + 7) / 8, (j.height + 7) / 8, 1);
        }
        bool pre_seed0 = false;   // this record's pre block ran seed 0 without the table (the runtime's seed0_run)
        for (size_t i = 0; i < b->plan.disp.size(); ++i) {
            nr::D3d12NetPlan::Dispatch d = b->plan.disp[i];
            if (patch) patch(d, controls_, grade);    // controls -> patch_push
            if (d.act_at >= 0 && size_t(d.act_at) + 8 <= d.push.size()) std::memcpy(d.push.data() + d.act_at, &act_va, 8);   // put_act_address
            const Built::Kern* k = nullptr;
            if (int(i) == tp.pre) {
                k = &b->t_pre;
                // This frame's noise seed, computed in the shader; seed 0 reads the table when there is one, any other
                // seed computes (the plan's push says no table: --lazy-images).
                if (d.push.size() == tp.pre_push_bytes && (pre_seed || (b->noise_real && !b->plan.noise.empty()))) {
                    const uint32_t field = pre_seed ? 0u : b->plan.noise.front().off;
                    if (pre_seed) std::memcpy(d.push.data() + tp.seed_at, &pre_seed, 4);
                    std::memcpy(d.push.data() + tp.noise_field_at, &field, 4);
                }
                pre_seed0 = !pre_seed && !b->noise_real && !b->plan.noise.empty();
            } else if (int(i) == tp.post) {
                k = f16 ? (grade ? &b->t_grade16 : &b->t_post16) : (grade ? &b->t_grade : &b->t_post);
            } else {
                k = &b->kern.at(d.kern);
            }
            table(base + uint32_t(i), *k, d.push, d.gx, d.gy, d.gz);
        }
        if (!b->noise_real && !b->plan.noise.empty()) {   // Runtime::record_all: two recordings in a row with seed 0
            b->seed0_run = pre_seed0 ? b->seed0_run + 1u : 0u;
            if (b->seed0_run >= 2u) b->want_noise = true;
        }
        {
            // params[12] (record_all): the gate, MVecScale, the history weight, the history's extent, depth flags, and the
            // motion field covering the frame (the backend resampled the guide's region onto it).
            const float hs = controls_.history_strength >= 0.f ? std::min(std::max(controls_.history_strength, 0.f), 1.f) : 1.0f;
            const bool have_depth = temporal_.have_depth && !b->plan.temporal.motion_without_depth;
            const float params[12] = {gate ? 1.0f : 0.0f, temporal_.motion_scale_x, temporal_.motion_scale_y,
                                      hs * tp.blend_scale, float(b->w), float(b->h),
                                      have_depth ? 1.0f : 0.0f,
                                      have_depth && temporal_.depth_inverted ? 1.0f : 0.0f,
                                      1.0f, 1.0f, 0.0f, 0.0f};
            std::memset(ring + uint64_t(b->tables) * kTableBytes, 0, kParamsBytes);
            std::memcpy(ring + uint64_t(b->tables) * kTableBytes, params, sizeof params);
        }
        const uint32_t lo = n_noise ? 0 : base;
        if (b->table_uav) transition(cmd, b->table, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(b->table, uint64_t(lo) * kTableBytes, slot_buf->buf, slot_off + uint64_t(lo) * kTableBytes,
                              b->slot_bytes - uint64_t(lo) * kTableBytes);
        transition(cmd, b->table, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        b->table_uav = true;
        // The slot's frame number back where the CPU can see it: once it lands, this record's copy has run.
        cmd->CopyBufferRegion(b->done, uint64_t(frame % kDoneSlots) * 8, slot_buf->buf, slot_off + b->slot_bytes, 8);

        // A CPU call count cannot prove that ResolveQueryData has run. Keep the same Recording that protects
        // the frame's resources; read and reuse a query pair only after all of that recording's submissions finish.
        uint32_t ts = kRing;
        if (stamps_map_) {
            for (uint32_t i = 0; i < kRing; ++i) {
                if (stamp_uses_[i] && split::completed(stamp_uses_[i])) {
                    uint64_t t2[2];
                    std::memcpy(t2, stamps_map_ + i * 16, 16);
                    if (t2[1] > t2[0]) note_gpu(double(t2[1] - t2[0]) * 1000.0 / hz_);
                    stamp_uses_[i].reset();
                }
                if (!stamp_uses_[i] && ts == kRing) ts = i;
            }
        }
        const auto recording = ts < kRing ? split::book(cmd) : nr::gpu::Ticket{};
        const bool timed = bool(recording);
        if (timed) {
            stamp_uses_[ts] = recording;
            // A recording reset without execution has no timestamp result. Do not count a previous use's pair.
            std::memset(stamps_map_ + ts * 16, 0, 16);
            cmd->EndQuery(queries_, D3D12_QUERY_TYPE_TIMESTAMP, ts * 2);
        }

        // record: the noise table's jobs (when made now), then every dispatch in plan order, a barrier after each.
        cmd->SetComputeRootSignature(rs_);
        for (uint32_t i = lo; i < b->tables; ++i) {
            const Built::Kern& k = *kern[i];
            const uint8_t* t = ring + uint64_t(i) * kTableBytes;
            uint32_t grid[3];
            std::memcpy(grid, t + kTableGridOff, sizeof grid);
            const uint32_t tag = k.tag + 1;
            cmd->SetPipelineState(k.pso);
            cmd->SetComputeRoot32BitConstants(0, 1, &tag, 0);
            cmd->SetComputeRootUnorderedAccessView(1, table_va + uint64_t(i) * kTableBytes);
            cmd->Dispatch(grid[0], grid[1], grid[2]);
            const bool nobar = i >= base && (i - base) < b->plan.disp.size() && b->plan.disp[i - base].no_barrier_after;
            if (!nobar) uav_barrier(cmd);
        }
        if (first) {
            b->retargeted = true;
            RetiredUse r;
            r.net = current_shared_;
            r.upload = b->wgt_upload;
            r.uses = b->gpu;
            r.frame = int64_t(frame);
            retire_upload_.push_back(std::move(r));
            b->wgt_upload = nullptr;
            if (b->wgt_e_upload) {
                RetiredUse e;
                e.net = current_shared_;
                e.upload = b->wgt_e_upload;
                e.uses = b->gpu;
                e.frame = int64_t(frame);
                retire_upload_.push_back(std::move(e));
                b->wgt_e_upload = nullptr;
            }
        }
        if (timed) {
            cmd->EndQuery(queries_, D3D12_QUERY_TYPE_TIMESTAMP, ts * 2 + 1);
            cmd->ResolveQueryData(queries_, D3D12_QUERY_TYPE_TIMESTAMP, ts * 2, 2, stamps_, uint64_t(ts) * 16);
        }
        cmd->CopyBufferRegion(b->done, (kDoneSlots + uint64_t(frame % kDoneSlots)) * 8,
                              b->exec_token, token_off, 8);
        if (temporal_.have_motion) {
            fs->latch = true;
            fs->hcur ^= 1u;
        }
        ++frames_;
        if (temporal_.reset) ++resets_;
        const double cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        cpu_sum_ += cpu_ms;
        if (frames_ <= 3)
            nr::pe::log("[d3d12] frame %llu on the D3D12 network: %ux%u, feature %llu, history gate %d (%s), seed %u, reset %d, "
                        "MVecScale %.6g %.6g, %zu dispatches (+%u noise), Style %d%s, record %.3f ms CPU",
                        (unsigned long long)frames_, b->w, b->h, (unsigned long long)temporal_.feature, gate ? 1 : 0,
                        f16 ? "RGBA16F" : "RGBA32F", pre_seed, temporal_.reset ? 1 : 0, temporal_.motion_scale_x,
                        temporal_.motion_scale_y, b->plan.disp.size(), n_noise, controls_.style, grade ? " (graded)" : "", cpu_ms);
        if (frames_ % 300 == 0) {
            nr::pe::log("[d3d12] timing: frames %llu-%llu on the D3D12 network: GPU %s, CPU record avg %.3f ms; %ux%u, %zu feature(s), "
                        "%llu reset(s), history gate %d", (unsigned long long)(frames_ - 299), (unsigned long long)frames_,
                        gpu_text().c_str(), cpu_sum_ / 300.0, b->w, b->h, b->features.size(), (unsigned long long)resets_,
                        gate ? 1 : 0);
            cpu_sum_ = 0; resets_ = 0;
        }
        return true;
    }

private:
    void note_gpu(double ms) {
        gpu_ring_[gpu_ring_at_ % 32] = float(ms);
        ++gpu_ring_at_;
        const size_t n = std::min<size_t>(gpu_ring_at_, 32);
        double s = 0;
        for (size_t i = 0; i < n; ++i) s += gpu_ring_[i];
        gpu_avg_ms_.store(float(s / double(n)));
        ++gpu_n_; gpu_sum_ += ms;
        gpu_min_ = gpu_n_ == 1 ? ms : std::min(gpu_min_, ms);
        gpu_max_ = std::max(gpu_max_, ms);
        if (++gpu_seen_ <= 3) nr::pe::log("[d3d12] timing: one D3D12 network frame took %.3f ms on the GPU", ms);
    }
    std::string gpu_text() {
        char t[96];
        if (!gpu_n_) std::snprintf(t, sizeof t, "not measured");
        else std::snprintf(t, sizeof t, "avg %.3f ms (min %.3f, max %.3f, %u samples)", gpu_sum_ / gpu_n_, gpu_min_, gpu_max_, gpu_n_);
        gpu_n_ = 0; gpu_sum_ = gpu_min_ = gpu_max_ = 0;
        return t;
    }
    void sweep() {
        ++tick_;
        for (size_t i = 0; i < retired_.size();)
            if (retired_[i].done()) retired_.erase(retired_.begin() + long(i)); else ++i;
        for (size_t i = 0; i < retired_features_.size();)
            if (retired_features_[i].done()) retired_features_.erase(retired_features_.begin() + long(i)); else ++i;
        for (size_t i = 0; i < retire_upload_.size();)
            if (retire_upload_[i].done()) {
                if (retire_upload_[i].upload) retire_upload_[i].upload->Release();
                retire_upload_.erase(retire_upload_.begin() + long(i));
            } else ++i;
    }

    std::string name_ = "the ACO network";
    ID3D12Device* device_ = nullptr;
    ID3D12RootSignature* rs_ = nullptr;
    std::shared_ptr<Shared> shared_;
    std::map<std::tuple<uint32_t, uint32_t, int>, std::shared_ptr<Built>> nets_;
    std::shared_ptr<Built> current_shared_;
    // `current_`/`last_want_`/`nets_` are written by the game's thread and read by the heartbeat's status line: a
    // reader copies the small fields under this lock, so a network the game drops cannot be freed under it.
    mutable std::recursive_mutex state_m_;
    Built* current_ = nullptr;                   // ready()'s answer for this evaluate
    bool building_ = false;                      // ready() said no only because a network is building
    int last_want_ = -1;
    // Each resource keeps all recordings that used it. A later queue's completion cannot retire an earlier
    // recording that remains executable or whose submissions have not all finished.
    struct RetiredNet {
        std::shared_ptr<Built> net;
        bool done() { return !net || recordings_done(net->gpu); }
    };
    struct RetiredUse {
        std::shared_ptr<Built> net;
        std::shared_ptr<void> hold;
        ID3D12Resource* upload = nullptr;
        int64_t frame = -1;
        nr::gpu::Uses uses;   // snapshot of every recording that used this actual resource
        bool done() { return recordings_done(uses); }
    };
    std::vector<RetiredNet> retired_;
    std::vector<RetiredUse> retired_features_;
    std::vector<RetiredUse> retire_upload_;
    uint64_t tick_ = 0;
    nr::D3d12NetControls controls_;
    bool f16_set_ = false, f16_want_ = true;   // [Network] History Precision from the caller (history_f16)
    bool grade_set_ = false, grade_want_ = true;   // [Network] Style Grade from the caller (style_grade)
    NetTemporal temporal_;
    uint32_t plain_seed_ = 0;
    std::map<uint64_t, uint32_t> plain_seeds_;
    ID3D12QueryHeap* queries_ = nullptr;
    ID3D12Resource* stamps_ = nullptr;
    uint8_t* stamps_map_ = nullptr;
    nr::gpu::Ticket stamp_uses_[kRing];
    double hz_ = 0;
    uint64_t frames_ = 0, resets_ = 0, gpu_seen_ = 0;
    unsigned gpu_n_ = 0;
    std::atomic<float> gpu_avg_ms_{0.f};
    float gpu_ring_[32] = {};
    size_t gpu_ring_at_ = 0;
    double gpu_sum_ = 0, gpu_min_ = 0, gpu_max_ = 0, cpu_sum_ = 0;
};

}  // namespace

void set_net_planner(NetPlanner planner, NetControlsPatch patch, NetModelQuery model_query) {
    g_planner = planner; g_patch = patch; g_model_query = model_query;
}
NetPlanner net_planner() { return g_planner.load(); }

std::unique_ptr<D3d12PassthroughNetwork> make_aco_network() {
    return std::make_unique<AcoNetwork>();
}

}  // namespace nr::pe::d3d12_passthrough
