#include "nr_path_u8.hpp"
// The real network of the native D3D12 ASD backend (nr_d3d12_asd_net.hpp). Same steps, same order, same names as
// nr::Runtime runs its network (nr_runtime.cpp; Mochi's linux/src/core/nr_runtime.cpp is the same file): build,
// retarget, controls / patch_push, record. What differs is only what D3D12 and the ASD carriers need:
//   - a dispatch is the carrier pipeline of its kernel (the PSDB's ACO machine code for it) with a 4 KB table per
//     dispatch - buffer and image descriptors, the push block at 512, the grid at 640 - bound as the carrier's root
//     UAV, its tag as the root constant (the lab's M4 executor, amd-d3d12-aco-lab/src/m4_graph.hpp), where the Vulkan
//     runtime binds a descriptor set and pushes constants;
//   - the images are the backend's linear RGBA32F buffers (tex_in = the frame, surf0 = the answer, surf1 of ours;
//     the temporal path's flow0 = the engine's motion resampled onto the frame by the backend, the two history
//     images, the depth) - every image RGBA32F at the frame's extent, read through the runtime's samplers;
//   - the activation arena's "zero once" is a fresh committed resource (D3D12 zeroes those);
//   - the alpha pass is HLSL on the linear buffers (runtime_alpha.comp's arithmetic);
//   - the temporal params buffer travels with the frame's tables (one upload, one copy).
#include "nr_d3d12_asd_net.hpp"
#include "nr_d3d12_capture.hpp"
#include "nr_d3d12_asd.hpp"
#include "nr_ini_network.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_d3d12split.hpp"

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

namespace nr::pe::asd {
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
// The lab's record name: FNV-1a 64 of the kernel's SPIR-V (amd-d3d12-aco-lab/src/model_elf.hpp fnv_lab).
uint64_t fnv_lab(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// ---- the carriers (net-carriers.bin, written by the lab's m5-build next to the PSDB it made) --------------------------
struct Carrier { uint32_t tag = 0, dims[3]{}; std::string stem, src; std::vector<uint8_t> dxil; };
struct Carriers {
    std::vector<uint8_t> rs;
    std::map<std::string, Carrier> by_stem;   // the "root" record first, as the M4 executor resolves
};
bool load_carriers(const std::wstring& path, Carriers* c, std::string* why) {
    std::vector<uint8_t> b;
    if (!slurp(path, &b)) { *why = "net-carriers.bin is missing beside the PSDB"; return false; }
    size_t at = 0;
    auto u32 = [&](uint32_t* v) { if (at + 4 > b.size()) return false; std::memcpy(v, b.data() + at, 4); at += 4; return true; };
    auto bytes = [&](std::vector<uint8_t>* v) {
        uint32_t n = 0;
        if (!u32(&n) || at + n > b.size()) return false;
        v->assign(b.begin() + at, b.begin() + at + n); at += n; return true;
    };
    auto str = [&](std::string* s) { std::vector<uint8_t> v; if (!bytes(&v)) return false; s->assign(v.begin(), v.end()); return true; };
    if (b.size() < 8 || std::memcmp(b.data(), "NRCAR001", 8) != 0) { *why = "net-carriers.bin: not NRCAR001"; return false; }
    at = 8;
    uint32_t count = 0;
    if (!bytes(&c->rs) || !u32(&count)) { *why = "net-carriers.bin: truncated"; return false; }
    for (uint32_t i = 0; i < count; ++i) {
        Carrier k;
        if (!u32(&k.tag) || !u32(&k.dims[0]) || !u32(&k.dims[1]) || !u32(&k.dims[2]) || !str(&k.stem) || !str(&k.src) ||
            !bytes(&k.dxil)) { *why = "net-carriers.bin: truncated record"; return false; }
        auto it = c->by_stem.find(k.stem);
        if (it == c->by_stem.end() || (it->second.src != "root" && k.src == "root")) c->by_stem[k.stem] = std::move(k);
    }
    return true;
}

// ---- the binding table of a kernel: nr_graph.cpp's kernel creation (the lab's family_of) ---------------------------
enum Res { ACT, WGT, PARAMS };
// TEX_IN, FLOW, HIST, DEPTH: combined image + sampler (48 bytes); SURF0, SURF1, HIST_STORE: storage images (32).
enum Img { TEX_IN, SURF0, SURF1, FLOW, HIST, HIST_STORE, DEPTH };
struct Family { std::vector<Res> buffers; std::vector<Img> images; };
bool family_of(std::string k, Family* f) {
    if (k.size() > 6 && k.compare(k.size() - 6, 6, "_grade") == 0) k.resize(k.size() - 6);
    auto w = [&](std::initializer_list<Res> b) { f->buffers.assign(b); };
    // nr_runtime.cpp's temporal pre / post: {act, act, wgt, wgt, wgt, act, params} {tex_in, flow0_sampled, hist(c),
    // depth}; {act, act, wgt, wgt, wgt, params} {surf0, hist_store[c ^ 1], tex_in, flow0_sampled, hist(c)}.
    if (k.rfind("temporal_pre", 0) == 0) { w({ACT, ACT, WGT, WGT, WGT, ACT, PARAMS}); f->images = {TEX_IN, FLOW, HIST, DEPTH}; }
    else if (k.rfind("temporal_post", 0) == 0) { w({ACT, ACT, WGT, WGT, WGT, PARAMS}); f->images = {SURF0, HIST_STORE, TEX_IN, FLOW, HIST}; }
    else if (k.rfind("fswindsp", 0) == 0) w({ACT, ACT, WGT, WGT, WGT, ACT});
    else if (k == "fswinimagepreds32" || k == "fswinimagepreds32nh") { w({ACT, ACT, WGT, WGT, WGT, ACT}); f->images = {TEX_IN}; }
    else if (k == "fswinimagepost32") { w({ACT, ACT, WGT, WGT, WGT}); f->images = {SURF0, SURF1, TEX_IN}; }
    else if (k.rfind("fswinfusedup", 0) == 0) w({ACT, ACT, WGT, WGT, WGT});
    else if (k == "fswinblendpost32") w({ACT, ACT, WGT, WGT, WGT});
    else if (k.rfind("fswin", 0) == 0) w({ACT, ACT, WGT, WGT, WGT, ACT});
    else if (k == "decups") w({ACT, ACT, WGT});
    else if (k == "vitattn") w({ACT, WGT, ACT});
    else if (k.rfind("attn", 0) == 0) w({ACT, ACT, WGT, WGT, ACT});
    else if (k == "ffwd3" || k == "ffwd3w") w({ACT, ACT, ACT, WGT, WGT, ACT});
    else if (k.rfind("gemm", 0) == 0) w({ACT, ACT, ACT, WGT, WGT, ACT, WGT, ACT});
    else if (k == "noisefield") w({WGT});
    else return false;
    return true;
}

constexpr uint32_t kTableBytes = 4096, kTablePushOff = 512, kTableGridOff = 640;
constexpr uint32_t kRing = 16;        // upload slots a buffer; more buffers are added while every slot is held
constexpr uint32_t kDoneSlots = 128;  // frame numbers the GPU lands in the readback buffer, a ring of these
constexpr uint32_t kParamsBytes = 256;   // the temporal params (12 floats), after the frame's tables
constexpr uint32_t kMaxFeatures = 64, kHardMaxFeatures = 128, kMaxExtents = 4;

void put_buf(uint32_t* d, uint64_t va, uint64_t bytes) {
    d[0] = uint32_t(va);
    d[1] = uint32_t((va >> 32) & 0xffffu);
    d[2] = uint32_t(bytes);
    d[3] = 0x30014facu;
}
void put_rgba32f_image(uint32_t* d, uint64_t va, uint32_t width, uint32_t height) {
    const uint32_t wm1 = width - 1, hm1 = height - 1;
    d[0] = uint32_t(va >> 8);
    d[1] = uint32_t((va >> 40) & 0xffu) | (63u << 17) | ((wm1 & 3u) << 30);
    d[2] = (wm1 >> 2) | (hm1 << 14);
    d[3] = 0x90000facu;
    d[4] = wm1;
    d[5] = 4u << 20;
    d[6] = 1u << 15;
    d[7] = 0;
}
// nrvk's sampler (NEAREST, MIRRORED_REPEAT: tex_in) and the runtime's linear_sampler (LINEAR, mip NEAREST,
// CLAMP_TO_EDGE: flow0_sampled, the history, the depth), as the driver writes a sampler: clamp x/y/z, trunc_coord for
// a point sampler only, the xy mag / min filter, mip point.
void put_mirror_nearest_sampler(uint32_t* d) { d[0] = 0x08000049u; d[1] = 0; d[2] = 0x04000000u; d[3] = 0; }
void put_linear_clamp_sampler(uint32_t* d) { d[0] = 0x00000092u; d[1] = 0; d[2] = 0x04500000u; d[3] = 0; }

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

// runtime_alpha.comp (windows/shaders/passes): the frame's alpha back onto the answer, the 8-bit rounding for an
// 8-bit output.
const char kAlpha[] =
    "cbuffer C : register(b0) { uint g_w; uint g_h; uint g_u8; uint g_pad; };\n"
    "RWStructuredBuffer<float4> result : register(u0);\n"
    "RWStructuredBuffer<float4> original : register(u1);\n"
    "[numthreads(8, 8, 1)] void alpha(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= g_w || id.y >= g_h) return;\n"
    "    const uint i = id.y * g_w + id.x;\n"
    "    float3 rgb = result[i].rgb;\n"
    "    if (g_u8 != 0) { precise float3 scaled = saturate(rgb) * 255.0; rgb = (floor(scaled) + step(0.5, frac(scaled))) / 255.0; }\n"
    "    result[i] = float4(rgb, original[i].a);\n"
    "}\n";

// The temporal state of one feature (Runtime::Impl::FeatureState): its two history images (the post block writes the
// next frame's into the other, t.pingpong), the latch, the seed counter, the input domain its history was made under.
// D3D12 zeroes a new committed resource: a fresh feature's history is cleared as the runtime clears it.
struct FeatureState {
    ID3D12Resource* hist[2]{};
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
    struct Kern { ID3D12PipelineState* pso = nullptr; uint32_t tag = 0; Family fam; };
    std::map<std::string, Kern> kern;
    Kern t_pre, t_post, t_grade;                  // the temporal pre / post (t_grade.pso null: no Style grade)
    ID3D12Resource *act = nullptr, *wgt = nullptr, *wgt_upload = nullptr, *surf1 = nullptr, *depth = nullptr;
    // The recording that initialized this arena. If discarded, rebuild the Built: the original upload is already
    // retired and the recorded table/weight states cannot be repaired by clearing retargeted alone.
    nr::gpu::Ticket initialization;
    ID3D12Resource* table = nullptr;
    // Debug: the noise field the plan precomputes into wgt's tail (readback, filled on the first frame).
    ID3D12Resource* noise_dump = nullptr;
    // Debug: the activation arena's fixed prefix after the temporal pre block (a layout-free comparison).
    ID3D12Resource* pre_dump = nullptr;
    uint64_t pre_dump_bytes = 0;
    bool pre_dump_written = false;
    ID3D12Resource* wgt_dump = nullptr;   // the weight blob [0, weights_end) (N_R_NET_WGT_DUMP)
    uint64_t wgt_dump_bytes = 0;
    bool wgt_dump_written = false;
    ID3D12Resource* kern_dump = nullptr;   // the act prefix after the first named kernel (NR_NET_KERN_DUMP)
    uint64_t kern_dump_bytes = 0;
    bool kern_dump_written = false;
    uint64_t noise_dump_bytes = 0;
    bool noise_dump_written = false;
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
    uint32_t tables = 0;                          // per frame: noise jobs + dispatches
    uint64_t slot_bytes = 0;                      // one frame's ring slot: the tables, then the params
    bool retargeted = false;
    bool table_uav = false;                       // the table buffer's state: UNORDERED_ACCESS (else COPY_DEST)
    uint64_t used = 0;
    int net = -1;
    std::map<uint64_t, std::shared_ptr<FeatureState>> features;
    ~Built() {
        for (auto& [n, k] : kern) release(k.pso);
        release(t_pre.pso); release(t_post.pso); release(t_grade.pso);
        for (auto& u : upload) {
            if (u.map) u.buf->Unmap(0, nullptr);
            release(u.buf);
        }
        if (exec_token_map) exec_token->Unmap(0, nullptr);
        release(exec_token);
        if (copy_map) done->Unmap(0, nullptr);
        release(done);
        release(act); release(wgt); release(wgt_upload); release(surf1); release(depth); release(table);
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
std::shared_ptr<Built> build(ID3D12Device* device, const Carriers& carriers, ID3D12RootSignature* rs, uint32_t w,
                             uint32_t h, int want_net, std::string* why) {
    const auto t0 = std::chrono::steady_clock::now();
    NetPlanner planner = g_planner.load();
    if (!planner) { *why = "no network planner"; return nullptr; }
    auto b = std::make_shared<Built>();
    b->w = w; b->h = h;
    // The kernel's record: FNV of its SPIR-V (g_<kern>.spv in the shaders the graph was lowered against; the temporal
    // kernels' own files).
    auto resolve_file = [&](const std::string& path, const Carrier** out) {
        std::vector<uint8_t> spv;
        if (!slurp(wide(path), &spv)) return false;
        char stem[32];
        std::snprintf(stem, sizeof stem, "%016llx", (unsigned long long)fnv_lab(spv.data(), spv.size()));
        auto it = carriers.by_stem.find(stem);
        if (it == carriers.by_stem.end()) return false;
        *out = &it->second;
        return true;
    };
    auto resolve = [&](const std::string& kern, const Carrier** out) {
        return resolve_file(b->plan.spv_dir + "/g_" + kern + ".spv", out);
    };
    auto stem_of = [](const std::string& path) {
        const size_t s = path.find_last_of("/\\"), e = path.rfind(".spv");
        return path.substr(s == std::string::npos ? 0 : s + 1, e == std::string::npos ? std::string::npos : e - (s == std::string::npos ? 0 : s + 1));
    };
    std::map<std::string, const Carrier*> found;
    const Carrier *c_pre = nullptr, *c_post = nullptr, *c_grade = nullptr;
    std::string missing;
    for (int attempt = 0; attempt < 2; ++attempt) {
        // CC_SMALL_FP8_EXPERIMENTAL first, as the runtime picks it; a kernel the PSDB lacks: CC_SMALL_FP8 (the
        // runtime's own "anything missing: the small model").
        const bool try_experimental = (want_net == nr::kSmallFp8Experimental && attempt == 0);
        const int plan_model = (want_net == nr::kSmallFp8Experimental && attempt == 1) ? nr::kSmallFp8 : want_net;
        if (!planner(w, h, plan_model, try_experimental, &b->plan, why)) return nullptr;
        found.clear(); missing.clear();
        c_pre = c_post = c_grade = nullptr;
        const auto& t = b->plan.temporal;
        for (size_t i = 0; i < b->plan.disp.size(); ++i) {
            const auto& d = b->plan.disp[i];
            if (int(i) == t.pre || int(i) == t.post || found.count(d.kern)) continue;   // replaced by the temporal kernels
            const Carrier* c = nullptr;
            if (!resolve(d.kern, &c)) { missing = d.kern; break; }
            found[d.kern] = c;
        }
        if (missing.empty() && !b->plan.noise.empty()) {
            const Carrier* c = nullptr;
            if (resolve("noisefield", &c)) found["noisefield"] = c; else missing = "noisefield";
        }
        if (missing.empty() && !resolve_file(t.pre_spv, &c_pre)) missing = stem_of(t.pre_spv);
        if (missing.empty() && !resolve_file(t.post_spv, &c_post)) missing = stem_of(t.post_spv);
        if (missing.empty() && !t.post_grade_spv.empty() && !resolve_file(t.post_grade_spv, &c_grade))
            missing = stem_of(t.post_grade_spv);
        if (missing.empty()) break;
        if (b->plan.model.rfind("CC_SMALL_FP8_EXPERIMENTAL", 0) != 0) break;
        nr::pe::log("[asd] the %s network: kernel %s has no record in the PSDB; CC_SMALL_FP8 instead", b->plan.model.c_str(),
                    missing.c_str());
    }
    if (!missing.empty()) { *why = "kernel " + missing + " has no record in the PSDB (" + b->plan.model + ")"; return nullptr; }
    if (b->plan.temporal.pre < 0 || b->plan.temporal.post < 0) { *why = "the plan has no temporal pre / post block"; return nullptr; }
    b->net = nr::network_choice(b->plan.model.substr(0, b->plan.model.find(' ')));
    if (b->net < 0) b->net = want_net >= 0 ? want_net : nr::kSmallFp8;
    auto make = [&](const std::string& kern, const Carrier& c, Built::Kern* k) {
        if (!family_of(kern, &k->fam)) { *why = "no binding table for kernel " + kern; return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs;
        pd.CS.pShaderBytecode = c.dxil.data();
        pd.CS.BytecodeLength = c.dxil.size();
        if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&k->pso)))) {
            *why = "CreateComputePipelineState failed for kernel " + kern; return false;
        }
        k->tag = c.tag;
        return true;
    };
    for (const auto& [kern, c] : found)
        if (!make(kern, *c, &b->kern[kern])) return nullptr;
    if (!make(stem_of(b->plan.temporal.pre_spv), *c_pre, &b->t_pre) ||
        !make(stem_of(b->plan.temporal.post_spv), *c_post, &b->t_post) ||
        (c_grade && !make(stem_of(b->plan.temporal.post_grade_spv), *c_grade, &b->t_grade)))
        return nullptr;
    // The network memory (retarget): the arena zero (a new committed resource), the weights to upload once.
    const uint64_t wbytes = b->plan.weights.size();
    b->act = buffer(device, D3D12_HEAP_TYPE_DEFAULT, b->plan.act_total, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    b->wgt = buffer(device, D3D12_HEAP_TYPE_DEFAULT, wbytes, D3D12_RESOURCE_STATE_COPY_DEST, true);
    b->wgt_upload = buffer(device, D3D12_HEAP_TYPE_UPLOAD, wbytes, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    b->surf1 = buffer(device, D3D12_HEAP_TYPE_DEFAULT, uint64_t(w) * h * 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    // t.depth: dummy fallback when depth is not provided (the pre block reads have_depth 0), bound as a valid image.
    b->depth = buffer(device, D3D12_HEAP_TYPE_DEFAULT, uint64_t(w) * h * 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    b->tables = uint32_t(b->plan.noise.size() + b->plan.disp.size());
    b->slot_bytes = uint64_t(b->tables) * kTableBytes + kParamsBytes;
    b->table = buffer(device, D3D12_HEAP_TYPE_DEFAULT, b->slot_bytes, D3D12_RESOURCE_STATE_COPY_DEST, true);
    if (!b->act || !b->wgt || !b->wgt_upload || !b->surf1 || !b->depth || !b->table) {
        *why = "the network memory could not be made"; return nullptr;
    }
    capture::name(b->act, L"nr:act");
    capture::name(b->wgt, L"nr:wgt");
    capture::name(b->wgt_upload, L"nr:wgt_upload");
    capture::name(b->surf1, L"nr:surf1");
    capture::name(b->depth, L"nr:depth");
    capture::name(b->table, L"nr:table");
    capture::name(b->done, L"nr:done");
    capture::name(b->exec_token, L"nr:exec_token");
    {
        char line[256];
        std::snprintf(line, sizeof line, "network %s %ux%u (working %ux%u), %zu dispatches, %zu kernels, weights %llu B, arena %llu B",
                      b->plan.model.c_str(), w, h, b->plan.work_w, b->plan.work_h, b->plan.disp.size(), b->kern.size(),
                      static_cast<unsigned long long>(wbytes), static_cast<unsigned long long>(b->plan.act_total));
        capture::note("network", line);
    }
    for (ID3D12Resource* r : {b->act, b->wgt, b->surf1, b->depth, b->table})
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
    nr::pe::log("[asd] network %s for %ux%u (working %ux%u): %zu dispatches, %zu kernels + temporal %s / %s%s, %zu noise jobs, "
                "weights %llu B, arena %llu B, built in %.2fs%s%s", b->plan.model.c_str(), w, h, b->plan.work_w, b->plan.work_h,
                b->plan.disp.size(), b->kern.size(), stem_of(b->plan.temporal.pre_spv).c_str(),
                stem_of(b->plan.temporal.post_spv).c_str(), b->t_grade.pso ? " (+ Style grade)" : "", b->plan.noise.size(),
                (unsigned long long)wbytes, (unsigned long long)b->plan.act_total, s, b->plan.note.empty() ? "" : "; ",
                b->plan.note.c_str());
    return b;
}

class AcoNetwork final : public D3d12AsdNetwork {
public:
    explicit AcoNetwork(std::wstring folder) : folder_(std::move(folder)), shared_(std::make_shared<Shared>()) {}
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
        release(alpha_pso_); release(alpha_rs_); release(rs_);
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

    bool write_capture(const std::string& dir, std::string* why) override {
        for (auto& [key, b] : nets_) {
            if (!b || !b->noise_dump || !b->noise_dump_written) continue;
            void* m = nullptr;
            if (FAILED(b->noise_dump->Map(0, nullptr, &m)) || !m) { *why = "the noise capture could not be mapped"; return false; }
            if (b->pre_dump && b->pre_dump_written) {
                void* pm = nullptr;
                if (SUCCEEDED(b->pre_dump->Map(0, nullptr, &pm)) && pm) {
                    const char* lo2 = std::getenv("NR_NET_PRE_OFF");
                    const std::string ppath = dir + "\\asd_pre" + (lo2 ? "_off" + std::string(lo2) : "") + ".bin";
                    if (FILE* pf = nr::u8::fopen(ppath.c_str(), "wb")) {
                        std::fwrite(pm, 1, size_t(b->pre_dump_bytes), pf);
                        std::fclose(pf);
                        char msg[192];
                        std::snprintf(msg, sizeof msg, "pre dump written: %s (%llu bytes)", ppath.c_str(),
                                      static_cast<unsigned long long>(b->pre_dump_bytes));
                        capture::schedule(msg);
                    }
                    b->pre_dump->Unmap(0, nullptr);
                }
            }
            if (b->wgt_dump && b->wgt_dump_written) {
                void* wm = nullptr;
                if (SUCCEEDED(b->wgt_dump->Map(0, nullptr, &wm)) && wm) {
                    const std::string wpath = dir + "\\asd_wgt.bin";
                    if (FILE* wf = nr::u8::fopen(wpath.c_str(), "wb")) {
                        std::fwrite(wm, 1, size_t(b->wgt_dump_bytes), wf);
                        std::fclose(wf);
                    }
                    b->wgt_dump->Unmap(0, nullptr);
                }
            }
            if (b->kern_dump && b->kern_dump_written) {
                void* km = nullptr;
                if (SUCCEEDED(b->kern_dump->Map(0, nullptr, &km)) && km) {
                    const std::string kpath = dir + "\\asd_kern.bin";
                    if (FILE* kf = nr::u8::fopen(kpath.c_str(), "wb")) {
                        std::fwrite(km, 1, size_t(b->kern_dump_bytes), kf);
                        std::fclose(kf);
                    }
                    b->kern_dump->Unmap(0, nullptr);
                }
            }
            const std::string path = dir + "\\asd_noise.bin";
            FILE* f = nr::u8::fopen(path.c_str(), "wb");
            const bool ok = f && std::fwrite(m, 1, size_t(b->noise_dump_bytes), f) == size_t(b->noise_dump_bytes);
            if (f) std::fclose(f);
            b->noise_dump->Unmap(0, nullptr);
            if (!ok) { *why = "the noise capture could not be written"; return false; }
            const auto& j = b->plan.noise[0];
            FILE* meta = nr::u8::fopen((dir + "\\asd_noise.txt").c_str(), "w");
            if (meta) {
                std::fprintf(meta, "file=%s\ndtype=f16x4\noff=%u\nsize=%llu\nwidth=%u\nheight=%u\nseed=%u\nnoise=%g\n"
                                   "weights_end=%llu\nact_total=%llu\n",
                             path.c_str(), j.off, static_cast<unsigned long long>(b->noise_dump_bytes), j.width, j.height,
                             j.seed, double(j.noise), static_cast<unsigned long long>(b->plan.weights_end),
                             static_cast<unsigned long long>(b->plan.act_total));
                std::fclose(meta);
            }
            return true;
        }
        *why = "no capture was recorded (NR_NET_NOISE_DUMP unset or no noise job)";
        return false;
    }

    bool create(ID3D12Device* device, const char** why) override {
        static std::string w;
        auto carriers = std::make_shared<Carriers>();
        if (!load_carriers(folder_ + L"\\net-carriers.bin", carriers.get(), &w)) { *why = w.c_str(); return false; }
        carriers_ = carriers;
        if (FAILED(device->CreateRootSignature(0, carriers_->rs.data(), carriers_->rs.size(), IID_PPV_ARGS(&rs_)))) {
            *why = "the carriers' root signature could not be made"; return false;
        }
        // The alpha pass: constants, the answer, the frame.
        D3D12_ROOT_PARAMETER p[3]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants.Num32BitValues = 4;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[1].Descriptor.ShaderRegister = 0;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[2].Descriptor.ShaderRegister = 1;
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 3;
        rd.pParameters = p;
        ID3DBlob *blob = nullptr, *errors = nullptr;
        const HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        release(errors);
        if (FAILED(hr) || !blob) { *why = "the alpha pass's root signature did not serialize"; return false; }
        const HRESULT hr2 = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&alpha_rs_));
        release(blob);
        if (FAILED(hr2)) { *why = "the alpha pass's root signature could not be made"; return false; }
        ID3DBlob* code = compile_cs(kAlpha, sizeof kAlpha - 1, "alpha", &w);
        if (!code) { *why = w.c_str(); return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = alpha_rs_;
        pd.CS.pShaderBytecode = code->GetBufferPointer();
        pd.CS.BytecodeLength = code->GetBufferSize();
        const HRESULT hr3 = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&alpha_pso_));
        release(code);
        if (FAILED(hr3)) { *why = "the alpha pass's pipeline could not be made"; return false; }
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
        if (!stamps_map_) nr::pe::log("[asd] the network's GPU time cannot be measured (no timestamp queries)");
        device->AddRef();
        device_ = device;
        name_ = "the ACO network (building)";
        return true;
    }

    bool ready(ID3D12Device*, uint32_t w, uint32_t h, std::string* why) override {
        sweep();
        {
            std::lock_guard<std::recursive_mutex> l(state_m_);
            current_shared_.reset();
            current_ = nullptr;
        }
        building_ = false;
        if (w % 8) { *why = "width " + std::to_string(w) + " is not a multiple of 8 (the network's RGBA32F image rows)"; return false; }
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
            // PSDB record). The result is registered under what it IS and under what was ASKED (shared_->bw/bh/
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
                nr::pe::log("[asd] network ACTIVE for %ux%u: %s (frames of this extent now run on the ASD network)", w, h, name_.c_str());
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
            nr::pe::log("[asd] building the %s network for %ux%u (until it is done these frames pass through with no NR; "
                        "the bridge is not used)", nr::network_codename(want), w, h);
            std::thread([sh = shared_, dev = device_, rs = rs_, c = carriers_, w, h, want] {
                std::string e;
                std::shared_ptr<Built> b;
                try { b = build(dev, *c, rs, w, h, want, &e); } catch (const std::exception& x) { e = x.what(); }
                if (!b) nr::pe::log("[asd] ERROR: the network for %ux%u cannot run: %s (the 0.0.8.1 path)", w, h, e.c_str());
                {
                    std::lock_guard<std::mutex> l(sh->m);
                    if (b) sh->done = std::move(b); else sh->failed[{w, h, want}] = e;
                    sh->building = false;
                }
                rs->Release();
                dev->Release();
            }).detach();
        }
        *why = std::string("the ") + nr::network_codename(shared_->bnet) + " network for " + std::to_string(shared_->bw) + "x" + std::to_string(shared_->bh) + " is building";
        building_ = true;   // v2.6: it will run these frames (no bridge meanwhile)
        return false;
    }
    bool building() const override { return building_; }
    AsdNetStatus status() const override {
        AsdNetStatus s;
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
        return s;
    }
    void controls(const nr::D3d12NetControls& c) override { controls_ = c; }
    void temporal(const NetTemporal& t) override { temporal_ = t; }
    bool motion_without_depth() const override {
        if (current_) return current_->plan.temporal.motion_without_depth;
        if (!nets_.empty()) return nets_.begin()->second->plan.temporal.motion_without_depth;
        return true;
    }

    bool record(ID3D12GraphicsCommandList* cmd, const LinearFrame& in, const LinearFrame& out, const char** why) override {
        const auto c0 = std::chrono::steady_clock::now();
        Built* b = current_;
        if (!b || b->w != in.width || b->h != in.height) { *why = "no network for this extent"; return false; }
        if ((in.va & 255) || (out.va & 255) || (temporal_.motion_va & 255) || (temporal_.depth_va & 255)) { *why = "a linear buffer is not 256-byte aligned"; return false; }
        const auto& tp = b->plan.temporal;
        // This frame's feature (Runtime::record_all): its state, made on its first frame.
        FeatureState* fs = nullptr;
        if (auto it = b->features.find(temporal_.feature); it != b->features.end() && it->second) {
            fs = it->second.get();
        } else {
            // No age-based eviction: a feature's history stays until the caller says that feature is gone
            // (release_feature), as the session's entry.stack does - an "inactive" window would silently reset the
            // history of a feature that still exists.
            auto fresh = std::make_shared<FeatureState>();
            for (auto& hh : fresh->hist)
                hh = buffer(device_, D3D12_HEAP_TYPE_DEFAULT, uint64_t(b->w) * b->h * 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            if (!fresh->hist[0] || !fresh->hist[1]) { *why = "the history images could not be made"; return false; }
            fs = fresh.get();
            b->features[temporal_.feature] = std::move(fresh);
        }
        fs->used = tick_;
        // The gate and the seed, the runtime's rule (nr_runtime.cpp lines 2420-2440 and 2626-2630):
        // with motion: the first-frame latch, DLSSNR.Reset, the input domain; the pre block's seed
        // a counter zeroed by any of them (frames with seed 0 use the precomputed noise field).
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
        // [D3D12ASD] NetTemporal / NetNoiseEveryFrame (dlssnr-amd-asd.ini, read once; v2.4: default 1 = the runtime's
        // rule above, which the bridge's network (nr_runtime.cpp) runs for a wilsjo2 layer: the history gate and a seed
        // that moves every frame). 0 turns either off - a diagnosis only (v2.3's default, not the cause of the flicker).
        static const int ini_temporal = GetPrivateProfileIntW(L"D3D12ASD", L"NetTemporal", 1, ini_path().c_str());
        static const int ini_noise = GetPrivateProfileIntW(L"D3D12ASD", L"NetNoiseEveryFrame", 1, ini_path().c_str());
        static std::once_flag said;
        std::call_once(said, [] {
            nr::pe::log("[asd] network temporal mode (dlssnr-amd-asd.ini): NetTemporal %d (%s), NetNoiseEveryFrame %d (%s)",
                        ini_temporal, ini_temporal ? "the history, the runtime's rule" : "off: no history (diagnosis)",
                        ini_noise, ini_noise ? "a new seed every frame, the runtime's rule" : "off: seed 0 (diagnosis)");
        });
        if (!ini_temporal) gate = false;
        if (!ini_noise) pre_seed = 0;
        // diagnostics (wrong picture), the runtime's own two: price the per-frame noise and the history
        if (std::getenv("NR_DIAG_SEED0")) pre_seed = 0;
        if (std::getenv("NR_DIAG_TGATE0")) gate = false;

        const uint64_t act_va = b->act->GetGPUVirtualAddress(), wgt_va = b->wgt->GetGPUVirtualAddress();
        const uint64_t s1_va = b->surf1->GetGPUVirtualAddress(), depth_va = b->depth->GetGPUVirtualAddress();
        const uint64_t table_va = b->table->GetGPUVirtualAddress();
        const uint64_t params_va = table_va + uint64_t(b->tables) * kTableBytes;
        const uint64_t hist_va = fs->hist[fs->hcur]->GetGPUVirtualAddress(), store_va = fs->hist[fs->hcur ^ 1u]->GetGPUVirtualAddress();
        NetControlsPatch patch = g_patch.load();
        const bool grade = b->t_grade.pso && controls_.style != 0;

        // retarget: the weights with their tail once, then the noise field's jobs (their tables first in the ring).
        // Recorded only after this record has a slot: a record that declines below must not leave the weight copy
        // in the list, or the next one would transition b->wgt from COPY_DEST when it is already UNORDERED_ACCESS.
        const bool first = !b->retargeted;
        // The tables of this frame: [noise jobs (first frame)] [every dispatch] [the params].
        const uint32_t n_noise = first ? uint32_t(b->plan.noise.size()) : 0;
        const uint32_t base = uint32_t(b->plan.noise.size());   // the dispatches' tables keep their place
        // An upload slot of this record's own. The CPU writes it here and the CopyBufferRegion below reads it
        // when the list runs; its last 8 bytes carry this record's frame number, which a second recorded copy
        // lands in the readback buffer. A slot is only taken again once that number has landed (copied: this
        // record's copy, and every earlier one in the queue, has run) - so the many networks a multi-pass stack
        // records into one not-yet-submitted command list each keep their own tables, however many there are.
        // The parameters are all the sequence reads from it, so a later dispatch of the same frame does not
        // keep the slot held: only the record that reads it does.
        const uint64_t frame = frames_;
        // Diagnostics: a PIX marker per dispatch (DRED's breadcrumbs keep the last one reached), the schedule of the
        // first frames in the run folder, and an optional limit for bisecting which dispatch faults.
        const bool diag = capture::on();
        const bool sched = diag && frame < 3;
        const uint32_t limit = uint32_t(std::max(0, asd::dispatch_limit()));
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
            static bool said = false;
            if (!said) {
                said = true;
                nr::pe::log("[asd] the network's upload slots are all in flight and no more could be made; "
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
        }
        const uint64_t slot_off = uint64_t(slot_i) * (b->slot_bytes + 8);
        uint8_t* ring = slot_buf->map + slot_off;
        std::memcpy(ring + b->slot_bytes, &frame, 8);
        const uint64_t token_off = (frame % b->token_slots) * 8;
        b->exec_token_map[token_off / 8] = frame;
        std::vector<const Built::Kern*> kern(b->tables, nullptr);
        auto image_va = [&](Img m) {
            switch (m) {
            case TEX_IN: return in.va;
            case SURF0: return out.va;
            case SURF1: return s1_va;
            case FLOW: return temporal_.motion_va;
            case HIST: return hist_va;
            case HIST_STORE: return store_va;
            case DEPTH: return temporal_.depth_va ? temporal_.depth_va : depth_va;
            }
            return in.va;
        };
        auto table = [&](uint32_t i, const Built::Kern& k, const std::vector<uint8_t>& push, uint32_t gx, uint32_t gy,
                         uint32_t gz) {
            uint8_t* t = ring + uint64_t(i) * kTableBytes;
            std::memset(t, 0, kTableBytes);
            size_t at = 0;
            for (Res r : k.fam.buffers) {
                if (r == PARAMS) put_buf(reinterpret_cast<uint32_t*>(t + at), params_va, 48);
                else put_buf(reinterpret_cast<uint32_t*>(t + at), r == ACT ? act_va : wgt_va,
                             r == ACT ? b->plan.act_total : b->wgt->GetDesc().Width);
                at += 16;
            }
            for (Img m : k.fam.images) {
                auto* d = reinterpret_cast<uint32_t*>(t + at);
                put_rgba32f_image(d, image_va(m), b->w, b->h);
                if (m == TEX_IN) { put_mirror_nearest_sampler(d + 8); at += 48; }
                else if (m == FLOW || m == HIST || m == DEPTH) { put_linear_clamp_sampler(d + 8); at += 48; }
                else at += 32;
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
        for (size_t i = 0; i < b->plan.disp.size(); ++i) {
            nr::D3d12NetPlan::Dispatch d = b->plan.disp[i];
            if (patch) patch(d, controls_, grade);    // controls -> patch_push
            if (d.act_at >= 0 && size_t(d.act_at) + 8 <= d.push.size()) std::memcpy(d.push.data() + d.act_at, &act_va, 8);   // put_act_address
            const Built::Kern* k = nullptr;
            if (int(i) == tp.pre) {
                k = &b->t_pre;
                // This frame's noise seed, computed in the shader: the precomputed field is seed 0's.
                if (pre_seed && d.push.size() == tp.pre_push_bytes) {
                    const uint32_t no_field = 0;
                    std::memcpy(d.push.data() + tp.seed_at, &pre_seed, 4);
                    std::memcpy(d.push.data() + tp.noise_field_at, &no_field, 4);
                }
            } else if (int(i) == tp.post) {
                k = grade ? &b->t_grade : &b->t_post;
            } else {
                k = &b->kern.at(d.kern);
            }
            if (limit && i >= limit) break;
            table(base + uint32_t(i), *k, d.push, d.gx, d.gy, d.gz);
            if (diag) {
                wchar_t mark[128];
                std::swprintf(mark, 128, L"NR: ACO %hs [%zu]", d.kern.c_str(), i);
                capture::mark(cmd, mark);
            }
            if (sched) {
                // The whole block: 128 B carries the tone/structure/skin/noise fields past byte 96.
                char push_hex[3 * 128 + 1] = {};
                const size_t pn = std::min<size_t>(d.push.size(), 128);
                for (size_t p = 0; p < pn; ++p)
                    std::snprintf(push_hex + p * 3, 4, "%02x ", static_cast<unsigned char>(d.push[p]));
                char line[760];
                std::snprintf(line, sizeof line,
                              "frame %llu dispatch %zu %s grid %ux%ux%u push[%zu] %s| in 0x%llX out 0x%llX act 0x%llX wgt 0x%llX",
                              static_cast<unsigned long long>(frame), i, d.kern.c_str(), d.gx, d.gy, d.gz, d.push.size(),
                              push_hex, static_cast<unsigned long long>(in.va), static_cast<unsigned long long>(out.va),
                              static_cast<unsigned long long>(act_va), static_cast<unsigned long long>(wgt_va));
                capture::schedule(line);
            }
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
        const uint32_t lo = first ? 0 : base;
        if (sched) {
            char l[256];
            std::snprintf(l, sizeof l, "frame %llu barrier table %s->COPY_DEST (uav before: %d)",
                          static_cast<unsigned long long>(frame), b->table_uav ? "UAV" : "COPY_DEST", b->table_uav ? 1 : 0);
            capture::schedule(l);
        }
        if (b->table_uav) transition(cmd, b->table, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(b->table, uint64_t(lo) * kTableBytes, slot_buf->buf, slot_off + uint64_t(lo) * kTableBytes,
                              b->slot_bytes - uint64_t(lo) * kTableBytes);
        if (sched) {
            char l[256];
            std::snprintf(l, sizeof l, "frame %llu CopyBufferRegion table+%llu <- upload+%llu, %llu bytes (table %llu, slot %llu)",
                          static_cast<unsigned long long>(frame), static_cast<unsigned long long>(uint64_t(lo) * kTableBytes),
                          static_cast<unsigned long long>(slot_off + uint64_t(lo) * kTableBytes),
                          static_cast<unsigned long long>(b->slot_bytes - uint64_t(lo) * kTableBytes),
                          static_cast<unsigned long long>(b->slot_bytes), static_cast<unsigned long long>(b->slot_bytes + 8));
            capture::schedule(l);
        }
        transition(cmd, b->table, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        b->table_uav = true;
        // The slot's frame number back where the CPU can see it: once it lands, this record's copy has run.
        cmd->CopyBufferRegion(b->done, uint64_t(frame % kDoneSlots) * 8, slot_buf->buf, slot_off + b->slot_bytes, 8);

        // A CPU call count cannot prove that ResolveQueryData has run. Keep the same Recording that protects
        // the frame's resources; read and reuse a query pair only after all of that recording's submissions finish.
        uint32_t ts = kRing;
        if (stamps_map_ && !limit) {
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

        // record: every dispatch in plan order, a barrier after each.
        if (diag) capture::mark(cmd, L"NR: ACO network");
        cmd->SetComputeRootSignature(rs_);
        // With DispatchLimit only the first ones have a table and a kernel: the loop stops where they stop.
        if (!b->noise_dump && !b->plan.noise.empty() && std::getenv("NR_NET_NOISE_DUMP")) {
            const auto& j = b->plan.noise[0];
            uint64_t nbytes = uint64_t(j.width) * j.height * 8;
            const uint64_t from = uint64_t(j.off) * 4; // NoiseJob::off is an absolute uint32 index in wgt.
            const uint64_t have = b->wgt->GetDesc().Width;
            if (from > have || nbytes > have - from) {
                *why = "noise capture region is outside the weight buffer";
                return false;
            }
            char msg[192];
            std::snprintf(msg, sizeof msg, "noise dump: wgt %llu bytes, region %llu+%llu (%ux%u)",
                          static_cast<unsigned long long>(have), static_cast<unsigned long long>(from),
                          static_cast<unsigned long long>(nbytes), j.width, j.height);
            capture::schedule(msg);
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = nbytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                           nullptr, IID_PPV_ARGS(&b->noise_dump))) && b->noise_dump) {
                b->noise_dump_bytes = nbytes;
            }
        }
        if (!b->wgt_dump && std::getenv("NR_NET_WGT_DUMP") && b->wgt && b->plan.weights_end) {
            const uint64_t nbytes = b->plan.weights_end;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = nbytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                           nullptr, IID_PPV_ARGS(&b->wgt_dump))) && b->wgt_dump) {
                b->wgt_dump_bytes = nbytes;
                transition(cmd, b->wgt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                cmd->CopyBufferRegion(b->wgt_dump, 0, b->wgt, 0, nbytes);
                transition(cmd, b->wgt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                b->wgt_dump_written = true;
            }
        }
        if (!b->kern_dump && std::getenv("NR_NET_KERN_DUMP") && b->act) {
            const char* lo = std::getenv("NR_NET_REGION_OFF");
            const char* ln = std::getenv("NR_NET_REGION_LEN");
            const uint64_t roff = lo && lo[0] ? std::strtoull(lo, nullptr, 0) : 0;
            uint64_t nbytes = ln && ln[0] ? std::strtoull(ln, nullptr, 0) : (64ull << 20);
            const uint64_t cap = b->act->GetDesc().Width;
            if (roff >= cap) nbytes = 0;
            else if (nbytes > cap - roff) nbytes = cap - roff;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = nbytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                           nullptr, IID_PPV_ARGS(&b->kern_dump))) && b->kern_dump)
                b->kern_dump_bytes = nbytes;
        }
        if (!b->pre_dump && std::getenv("NR_NET_PRE_DUMP") && b->act) {
            const char* lo = std::getenv("NR_NET_PRE_OFF");
            const char* ln = std::getenv("NR_NET_PRE_LEN");
            const uint64_t roff = lo && lo[0] ? std::strtoull(lo, nullptr, 0) : 0;
            uint64_t nbytes = ln && ln[0] ? std::strtoull(ln, nullptr, 0) : (64ull << 20);
            const uint64_t cap = b->act->GetDesc().Width;
            if (roff >= cap) nbytes = 0;
            else if (nbytes > cap - roff) nbytes = cap - roff;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = nbytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                           nullptr, IID_PPV_ARGS(&b->pre_dump))) && b->pre_dump) {
                b->pre_dump_bytes = nbytes;
                { char m4[160]; std::snprintf(m4, sizeof m4, "pre dump: region %llu+%llu", static_cast<unsigned long long>(roff),
                                              static_cast<unsigned long long>(nbytes)); capture::schedule(m4); }
                char msg[128];
                std::snprintf(msg, sizeof msg, "pre dump: act prefix %llu bytes", static_cast<unsigned long long>(nbytes));
                capture::schedule(msg);
            }
        }
        const uint32_t hi = limit ? std::min<uint32_t>(b->tables, base + limit) : b->tables;
        for (uint32_t i = lo; i < hi; ++i) {
            const Built::Kern& k = *kern[i];
            const uint8_t* t = ring + uint64_t(i) * kTableBytes;
            uint32_t grid[3];
            std::memcpy(grid, t + kTableGridOff, sizeof grid);
            const uint32_t tag = k.tag + 1;
            if (sched) {
                const char* kn = "temporal";
                for (const auto& [nm, kk] : b->kern) if (&kk == &k) { kn = nm.c_str(); break; }
                char l[320];
                std::snprintf(l, sizeof l,
                              "frame %llu [%u] pso %p rs %p rootUAV 0x%llX (table base 0x%llX, %u bytes) tag %u grid %ux%ux%u kern %s",
                              static_cast<unsigned long long>(frame), i, static_cast<void*>(k.pso), static_cast<void*>(rs_),
                              static_cast<unsigned long long>(table_va + uint64_t(i) * kTableBytes),
                              static_cast<unsigned long long>(table_va), kTableBytes, tag, grid[0], grid[1], grid[2], kn);
                capture::schedule(l);
            }
            cmd->SetPipelineState(k.pso);
            cmd->SetComputeRoot32BitConstants(0, 1, &tag, 0);
            cmd->SetComputeRootUnorderedAccessView(1, table_va + uint64_t(i) * kTableBytes);
            cmd->Dispatch(grid[0], grid[1], grid[2]);
            uav_barrier(cmd);
            if (b->kern_dump && !b->kern_dump_written && b->act) {
                const char* want = std::getenv("NR_NET_KERN_DUMP");
                const char* knm = "temporal";
                for (const auto& [nm, kk] : b->kern) if (&kk == &k) { knm = nm.c_str(); break; }
                if (want && want[0] && std::strcmp(knm, want) == 0) {
                    const char* lo = std::getenv("NR_NET_REGION_OFF");
                    const uint64_t roff = lo && lo[0] ? std::strtoull(lo, nullptr, 0) : 0;
                    transition(cmd, b->act, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    cmd->CopyBufferRegion(b->kern_dump, 0, b->act, roff, b->kern_dump_bytes);
                    transition(cmd, b->act, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    b->kern_dump_written = true;
                    { char m2[160]; std::snprintf(m2, sizeof m2, "kern dump: %s region %llu+%llu", want,
                        static_cast<unsigned long long>(roff), static_cast<unsigned long long>(b->kern_dump_bytes));
                      capture::schedule(m2); }
                }
            }
            const int plan_i = int(i) - int(base);   // the table index includes the noise dispatches
            if (!b->pre_dump_written && plan_i == tp.pre && plan_i >= 0 && size_t(plan_i) < b->plan.disp.size() &&
                b->plan.disp[size_t(plan_i)].push.size() >= 8) {
                const uint8_t* pp = b->plan.disp[size_t(plan_i)].push.data();
                uint32_t p0 = 0, p1 = 0;
                std::memcpy(&p0, pp, 4);
                std::memcpy(&p1, pp + 4, 4);
                char m3[160];
                std::snprintf(m3, sizeof m3, "pre push head: %u %u (push %zu bytes) table %u plan %d",
                              p0, p1, b->plan.disp[size_t(plan_i)].push.size(), i, plan_i);
                capture::schedule(m3);
            }
            if (b->pre_dump && !b->pre_dump_written && plan_i == tp.pre && b->act) {
                const char* lo = std::getenv("NR_NET_PRE_OFF");
                const uint64_t roff = lo && lo[0] ? std::strtoull(lo, nullptr, 0) : 0;
                // NR_NET_PRE_SURF=1: the same dump from surf1 instead of the act arena (where the pre block's
                // output may live on this route).
                ID3D12Resource* src = std::getenv("NR_NET_PRE_SURF") ? b->surf1 : b->act;
                const uint64_t cap = src ? src->GetDesc().Width : 0;
                uint64_t len = b->pre_dump_bytes;
                if (roff >= cap) len = 0; else if (len > cap - roff) len = cap - roff;
                if (src && len) {
                    transition(cmd, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    cmd->CopyBufferRegion(b->pre_dump, 0, src, roff, len);
                    transition(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    b->pre_dump_bytes = len;
                }
                b->pre_dump_written = true;
            }
        }
        // Debug capture: the precomputed noise field, as the noise kernel left it in wgt's tail.
        if (b->noise_dump && !b->noise_dump_written) {
            transition(cmd, b->wgt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(b->noise_dump, 0, b->wgt, uint64_t(b->plan.noise[0].off) * 4,
                                  b->noise_dump_bytes);
            transition(cmd, b->wgt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            b->noise_dump_written = true;
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
        }
        if (diag) capture::mark(cmd, limit ? L"NR: alpha (skipped: DispatchLimit)" : L"NR: alpha");
        // The alpha pass (runtime_alpha.comp): NR_POST_ALPHA is 0, so the post block left the alpha to it.
        if (limit) {   // bisecting: the frame stays as the network left it
            ++frames_;
            return true;
        }
        const UINT c[4] = {b->w, b->h, controls_.round_u8 ? 1u : 0u, 0u};
        if (sched) {
            char l[320];
            std::snprintf(l, sizeof l,
                          "frame %llu alpha pso %p rs %p consts %ux%u uav0 0x%llX uav1 0x%llX grid %ux%ux1",
                          static_cast<unsigned long long>(frame), static_cast<void*>(alpha_pso_),
                          static_cast<void*>(alpha_rs_), c[0], c[1], static_cast<unsigned long long>(out.va),
                          static_cast<unsigned long long>(in.va), (b->w + 7) / 8, (b->h + 7) / 8);
            capture::schedule(l);
        }
        cmd->SetComputeRootSignature(alpha_rs_);
        cmd->SetPipelineState(alpha_pso_);
        cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
        cmd->SetComputeRootUnorderedAccessView(1, out.va);
        cmd->SetComputeRootUnorderedAccessView(2, in.va);
        cmd->Dispatch((b->w + 7) / 8, (b->h + 7) / 8, 1);
        uav_barrier(cmd);
        if (timed) {
            cmd->EndQuery(queries_, D3D12_QUERY_TYPE_TIMESTAMP, ts * 2 + 1);
            cmd->ResolveQueryData(queries_, D3D12_QUERY_TYPE_TIMESTAMP, ts * 2, 2, stamps_, uint64_t(ts) * 16);
        }
        // The model execution completion marker: recorded AFTER all compute dispatches,
        // alpha pass, and UAV barriers. Once it lands in exec_map, the GPU has completely finished
        // executing the entire model for this frame. Only then may retired networks, activations,
        // weights, and feature history buffers be freed by sweep().
        cmd->CopyBufferRegion(b->done, (kDoneSlots + uint64_t(frame % kDoneSlots)) * 8,
                              b->exec_token, token_off, 8);
        // After the frame (record_all): with motion, the latch is set (reset suppresses consumption,
        // it does not un-write the history), and the post block wrote this frame's history into the other
        // image (flip hcur). Without motion (temporal == nullptr in 0.0.8.1), latch and hcur are untouched.
        if (temporal_.have_motion) {
            fs->latch = true;
            fs->hcur ^= 1u;
        }
        ++frames_;
        if (temporal_.reset) ++resets_;
        const double cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        cpu_sum_ += cpu_ms;
        if (frames_ <= 3)
            nr::pe::log("[asd] frame %llu on the ASD network: %ux%u, feature %llu, history gate %d, seed %u, reset %d, MVecScale "
                        "%.6g %.6g, %zu dispatches (+%u noise), Style %d%s, record %.3f ms CPU", (unsigned long long)frames_, b->w,
                        b->h, (unsigned long long)temporal_.feature, gate ? 1 : 0, pre_seed, temporal_.reset ? 1 : 0,
                        temporal_.motion_scale_x, temporal_.motion_scale_y, b->plan.disp.size(), n_noise, controls_.style,
                        grade ? " (graded)" : "", cpu_ms);
        if (frames_ % 300 == 0) {
            nr::pe::log("[asd] timing: frames %llu-%llu on the ASD network: GPU %s, CPU record avg %.3f ms; %ux%u, %zu feature(s), "
                        "%llu reset(s), history gate %d", (unsigned long long)(frames_ - 299), (unsigned long long)frames_,
                        gpu_text().c_str(), cpu_sum_ / 300.0, b->w, b->h, b->features.size(), (unsigned long long)resets_,
                        gate ? 1 : 0);
            cpu_sum_ = 0; resets_ = 0;
        }
        return true;
    }

private:
    void note_gpu(double ms) {
        ++gpu_n_; gpu_sum_ += ms;
        gpu_min_ = gpu_n_ == 1 ? ms : std::min(gpu_min_, ms);
        gpu_max_ = std::max(gpu_max_, ms);
        if (++gpu_seen_ <= 3) nr::pe::log("[asd] timing: one ASD network frame took %.3f ms on the GPU (network + alpha pass)", ms);
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

    std::wstring folder_;
    std::string name_ = "the ACO network";
    std::shared_ptr<const Carriers> carriers_;   // shared with a build thread still running when the device goes
    ID3D12Device* device_ = nullptr;
    ID3D12RootSignature* rs_ = nullptr;
    ID3D12RootSignature* alpha_rs_ = nullptr;
    ID3D12PipelineState* alpha_pso_ = nullptr;
    std::shared_ptr<Shared> shared_;
    std::map<std::tuple<uint32_t, uint32_t, int>, std::shared_ptr<Built>> nets_;
    std::shared_ptr<Built> current_shared_;
    // `current_`/`last_want_`/`nets_` are written by the game's thread and read by the heartbeat's status line: a
    // reader copies the small fields under this lock, so a network the game drops cannot be freed under it.
    mutable std::recursive_mutex state_m_;
    Built* current_ = nullptr;                   // ready()'s answer for this evaluate
    bool building_ = false;                      // v2.6: ready() said no only because a network is building
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
    double gpu_sum_ = 0, gpu_min_ = 0, gpu_max_ = 0, cpu_sum_ = 0;
};

}  // namespace

void set_net_planner(NetPlanner planner, NetControlsPatch patch, NetModelQuery model_query) {
    g_planner = planner; g_patch = patch; g_model_query = model_query;
}
NetPlanner net_planner() { return g_planner.load(); }

std::unique_ptr<D3d12AsdNetwork> make_aco_network(const std::wstring& folder) {
    return std::make_unique<AcoNetwork>(folder);
}

}  // namespace nr::pe::asd
