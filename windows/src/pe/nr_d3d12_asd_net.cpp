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
#include "nr_d3d12_asd.hpp"
#include "nr_pe_log.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace nr::pe::asd {
namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

std::atomic<NetPlanner> g_planner{nullptr};
std::atomic<NetControlsPatch> g_patch{nullptr};

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
constexpr uint32_t kRing = 16;   // tables of 16 frames in the upload ring before one is written again
constexpr uint32_t kParamsBytes = 256;   // the temporal params (12 floats), after the frame's tables
constexpr uint32_t kMaxFeatures = 8, kMaxExtents = 4;

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
    ID3D12Resource *table = nullptr, *ring = nullptr;
    uint8_t* ring_map = nullptr;
    uint32_t tables = 0;                          // per frame: noise jobs + dispatches
    uint64_t slot_bytes = 0;                      // one frame's ring slot: the tables, then the params
    bool retargeted = false;
    bool table_uav = false;                       // the table buffer's state: UNORDERED_ACCESS (else COPY_DEST)
    uint32_t ring_slot = 0;
    uint64_t used = 0;
    std::map<uint64_t, std::shared_ptr<FeatureState>> features;
    ~Built() {
        for (auto& [n, k] : kern) release(k.pso);
        release(t_pre.pso); release(t_post.pso); release(t_grade.pso);
        if (ring && ring_map) ring->Unmap(0, nullptr);
        release(act); release(wgt); release(wgt_upload); release(surf1); release(depth); release(table); release(ring);
    }
};

// What the build thread and the evaluate share.
struct Shared {
    std::mutex m;
    bool building = false;
    uint32_t bw = 0, bh = 0;
    std::shared_ptr<Built> done;                  // built, not yet taken by ready()
    std::map<std::pair<uint32_t, uint32_t>, std::string> failed;   // extents that cannot run (not built again)
};

// build: the lowering and everything of the device's it needs. On the build thread.
std::shared_ptr<Built> build(ID3D12Device* device, const Carriers& carriers, ID3D12RootSignature* rs, uint32_t w,
                             uint32_t h, std::string* why) {
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
        if (!planner(w, h, attempt == 0, &b->plan, why)) return nullptr;
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
    if (!b->plan.temporal.motion_without_depth) {
        *why = "dlssnr-amd.ini [Network] Motion Without Depth = 0: the depth input is not in the ASD path yet";
        return nullptr;
    }
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
    // t.depth: never written with Motion Without Depth (the pre block reads have_depth 0), bound as a valid image.
    b->depth = buffer(device, D3D12_HEAP_TYPE_DEFAULT, uint64_t(w) * h * 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    b->tables = uint32_t(b->plan.noise.size() + b->plan.disp.size());
    b->slot_bytes = uint64_t(b->tables) * kTableBytes + kParamsBytes;
    b->table = buffer(device, D3D12_HEAP_TYPE_DEFAULT, b->slot_bytes, D3D12_RESOURCE_STATE_COPY_DEST, true);
    b->ring = buffer(device, D3D12_HEAP_TYPE_UPLOAD, b->slot_bytes * kRing, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    if (!b->act || !b->wgt || !b->wgt_upload || !b->surf1 || !b->depth || !b->table || !b->ring) {
        *why = "the network memory could not be made"; return nullptr;
    }
    for (ID3D12Resource* r : {b->act, b->wgt, b->surf1, b->depth, b->table})
        if ((r->GetGPUVirtualAddress() >> 48) != 0) { *why = "a buffer address above 48 bits"; return nullptr; }
    void* m = nullptr;
    if (FAILED(b->wgt_upload->Map(0, nullptr, &m)) || !m) { *why = "the weights upload could not be mapped"; return nullptr; }
    if (wbytes) std::memcpy(m, b->plan.weights.data(), wbytes);
    b->wgt_upload->Unmap(0, nullptr);
    std::vector<uint8_t>().swap(b->plan.weights);
    if (FAILED(b->ring->Map(0, nullptr, &m)) || !m) { *why = "the table ring could not be mapped"; return nullptr; }
    b->ring_map = static_cast<uint8_t*>(m);
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
        nets_.clear();
        retired_.clear();
        for (auto& r : retire_upload_) r.second->Release();
        if (stamps_map_) stamps_->Unmap(0, nullptr);
        release(stamps_); release(queries_);
        release(alpha_pso_); release(alpha_rs_); release(rs_);
        if (device_) device_->Release();
    }
    const char* name() const override { return name_.c_str(); }

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
        // The network's GPU time: two timestamps a frame (a ring of kRing frames), read back kRing frames later. The
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
        current_ = nullptr;
        if (w % 8) { *why = "width " + std::to_string(w) + " is not a multiple of 8 (the network's RGBA32F image rows)"; return false; }
        const auto key = std::make_pair(w, h);
        if (auto it = nets_.find(key); it != nets_.end()) {
            current_ = it->second.get();
            current_->used = tick_;
            return true;
        }
        std::lock_guard<std::mutex> lock(shared_->m);
        if (shared_->done && shared_->done->w == w && shared_->done->h == h) {
            // Several extents at once (NR before and after the upscaler): one network each, the least recently used
            // dropped beyond kMaxExtents, as the session keeps a runtime per extent.
            if (nets_.size() >= kMaxExtents) {
                auto lru = nets_.begin();
                for (auto it = nets_.begin(); it != nets_.end(); ++it) if (it->second->used < lru->second->used) lru = it;
                retired_.emplace_back(tick_ + 64, std::move(lru->second));
                nets_.erase(lru);
            }
            auto& b = nets_[key];
            b = std::move(shared_->done);
            b->used = tick_;
            current_ = b.get();
            name_ = "the ACO network, " + b->plan.model + ", " + std::to_string(b->plan.disp.size()) + " dispatches, temporal";
            nr::pe::log("[asd] network ACTIVE for %ux%u: %s (frames of this extent now run on the ASD network)", w, h, name_.c_str());
            return true;
        }
        const auto f = shared_->failed.find(key);
        if (f != shared_->failed.end()) { *why = f->second; return false; }
        if (!shared_->building) {
            shared_->building = true;
            shared_->bw = w; shared_->bh = h;
            shared_->done.reset();
            device_->AddRef();
            rs_->AddRef();
            nr::pe::log("[asd] building the network for %ux%u (the 0.0.8.1 path runs these frames until it is done)", w, h);
            std::thread([sh = shared_, dev = device_, rs = rs_, c = carriers_, w, h] {
                std::string e;
                std::shared_ptr<Built> b;
                try { b = build(dev, *c, rs, w, h, &e); } catch (const std::exception& x) { e = x.what(); }
                if (!b) nr::pe::log("[asd] ERROR: the network for %ux%u cannot run: %s (the 0.0.8.1 path)", w, h, e.c_str());
                {
                    std::lock_guard<std::mutex> l(sh->m);
                    if (b) sh->done = std::move(b); else sh->failed[{w, h}] = e;
                    sh->building = false;
                }
                rs->Release();
                dev->Release();
            }).detach();
        }
        *why = "the network for " + std::to_string(shared_->bw) + "x" + std::to_string(shared_->bh) + " is building";
        return false;
    }

    void controls(const nr::D3d12NetControls& c) override { controls_ = c; }
    void temporal(const NetTemporal& t) override { temporal_ = t; }

    bool record(ID3D12GraphicsCommandList* cmd, const LinearFrame& in, const LinearFrame& out, const char** why) override {
        const auto c0 = std::chrono::steady_clock::now();
        Built* b = current_;
        if (!b || b->w != in.width || b->h != in.height) { *why = "no network for this extent"; return false; }
        if ((in.va & 255) || (out.va & 255) || (temporal_.motion_va & 255)) { *why = "a linear buffer is not 256-byte aligned"; return false; }
        const auto& tp = b->plan.temporal;
        // This frame's feature (Runtime::record_all): its state, made on its first frame.
        FeatureState* fs = nullptr;
        {
            auto& slot = b->features[temporal_.feature];
            if (!slot) {
                if (b->features.size() > kMaxFeatures) {
                    auto lru = b->features.end();
                    for (auto it = b->features.begin(); it != b->features.end(); ++it)
                        if (it->second && (lru == b->features.end() || it->second->used < lru->second->used)) lru = it;
                    if (lru != b->features.end()) { retired_features_.emplace_back(tick_ + 64, std::move(lru->second)); b->features.erase(lru); }
                }
                auto& fresh = b->features[temporal_.feature];
                fresh = std::make_shared<FeatureState>();
                for (auto& hh : fresh->hist)
                    hh = buffer(device_, D3D12_HEAP_TYPE_DEFAULT, uint64_t(b->w) * b->h * 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
                if (!fresh->hist[0] || !fresh->hist[1]) { b->features.erase(temporal_.feature); *why = "the history images could not be made"; return false; }
                fs = fresh.get();
            } else {
                fs = slot.get();
            }
        }
        fs->used = tick_;
        // The gate and the seed, the runtime's rule: the first-frame latch, DLSSNR.Reset, the input domain; the pre
        // block's seed a counter zeroed by any of them (frames with seed 0 use the precomputed noise field).
        const bool domain_changed = fs->latch && fs->domain != controls_.reversible;
        bool gate = fs->latch && !temporal_.reset && !domain_changed;
        if (!fs->latch || temporal_.reset || domain_changed) fs->seed = 0;
        fs->domain = controls_.reversible;
        uint32_t pre_seed = fs->seed++;
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
        const bool first = !b->retargeted;
        if (first) {
            cmd->CopyBufferRegion(b->wgt, 0, b->wgt_upload, 0, b->wgt->GetDesc().Width);
            transition(cmd, b->wgt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        // The tables of this frame: [noise jobs (first frame)] [every dispatch] [the params].
        const uint32_t n_noise = first ? uint32_t(b->plan.noise.size()) : 0;
        const uint32_t base = uint32_t(b->plan.noise.size());   // the dispatches' tables keep their place
        const uint64_t slot_off = uint64_t(b->ring_slot) * b->slot_bytes;
        uint8_t* ring = b->ring_map + slot_off;
        std::vector<const Built::Kern*> kern(b->tables, nullptr);
        auto image_va = [&](Img m) {
            switch (m) {
            case TEX_IN: return in.va;
            case SURF0: return out.va;
            case SURF1: return s1_va;
            case FLOW: return temporal_.motion_va;
            case HIST: return hist_va;
            case HIST_STORE: return store_va;
            case DEPTH: return depth_va;
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
            table(base + uint32_t(i), *k, d.push, d.gx, d.gy, d.gz);
        }
        {
            // params[12] (record_all): the gate, MVecScale, the history weight, the history's extent, no depth, and the
            // motion field covering the frame (the backend resampled the guide's region onto it).
            const float hs = controls_.history_strength >= 0.f ? std::min(std::max(controls_.history_strength, 0.f), 1.f) : 1.0f;
            const float params[12] = {gate ? 1.0f : 0.0f, temporal_.motion_scale_x, temporal_.motion_scale_y,
                                      hs * tp.blend_scale, float(b->w), float(b->h), 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f};
            std::memset(ring + uint64_t(b->tables) * kTableBytes, 0, kParamsBytes);
            std::memcpy(ring + uint64_t(b->tables) * kTableBytes, params, sizeof params);
        }
        const uint32_t lo = first ? 0 : base;
        if (b->table_uav) transition(cmd, b->table, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(b->table, uint64_t(lo) * kTableBytes, b->ring, slot_off + uint64_t(lo) * kTableBytes,
                              b->slot_bytes - uint64_t(lo) * kTableBytes);
        transition(cmd, b->table, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        b->table_uav = true;
        b->ring_slot = (b->ring_slot + 1) % kRing;

        // The GPU time of the frame kRing frames ago in this timestamp slot, then this frame's.
        const uint32_t ts = uint32_t(frames_ % kRing);
        if (stamps_map_ && frames_ >= kRing) {
            uint64_t t2[2];
            std::memcpy(t2, stamps_map_ + ts * 16, 16);
            if (t2[1] > t2[0]) note_gpu(double(t2[1] - t2[0]) * 1000.0 / hz_);
        }
        if (stamps_map_) cmd->EndQuery(queries_, D3D12_QUERY_TYPE_TIMESTAMP, ts * 2);

        // record: every dispatch in plan order, a barrier after each.
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
            uav_barrier(cmd);
        }
        if (first) {
            b->retargeted = true;
            retire_upload_.emplace_back(tick_ + 64, b->wgt_upload);
            b->wgt_upload = nullptr;
        }
        // The alpha pass (runtime_alpha.comp): NR_POST_ALPHA is 0, so the post block left the alpha to it.
        const UINT c[4] = {b->w, b->h, controls_.round_u8 ? 1u : 0u, 0u};
        cmd->SetComputeRootSignature(alpha_rs_);
        cmd->SetPipelineState(alpha_pso_);
        cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
        cmd->SetComputeRootUnorderedAccessView(1, out.va);
        cmd->SetComputeRootUnorderedAccessView(2, in.va);
        cmd->Dispatch((b->w + 7) / 8, (b->h + 7) / 8, 1);
        uav_barrier(cmd);
        if (stamps_map_) {
            cmd->EndQuery(queries_, D3D12_QUERY_TYPE_TIMESTAMP, ts * 2 + 1);
            cmd->ResolveQueryData(queries_, D3D12_QUERY_TYPE_TIMESTAMP, ts * 2, 2, stamps_, uint64_t(ts) * 16);
        }
        // After the frame (record_all): the latch set (reset suppresses consumption, it does not un-write the
        // history), the post block wrote this frame's history into the other image.
        fs->latch = true;
        fs->hcur ^= 1u;
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
            if (retired_[i].first <= tick_) retired_.erase(retired_.begin() + long(i)); else ++i;
        for (size_t i = 0; i < retired_features_.size();)
            if (retired_features_[i].first <= tick_) retired_features_.erase(retired_features_.begin() + long(i)); else ++i;
        for (size_t i = 0; i < retire_upload_.size();)
            if (retire_upload_[i].first <= tick_) { retire_upload_[i].second->Release(); retire_upload_.erase(retire_upload_.begin() + long(i)); }
            else ++i;
    }

    std::wstring folder_;
    std::string name_ = "the ACO network";
    std::shared_ptr<const Carriers> carriers_;   // shared with a build thread still running when the device goes
    ID3D12Device* device_ = nullptr;
    ID3D12RootSignature* rs_ = nullptr;
    ID3D12RootSignature* alpha_rs_ = nullptr;
    ID3D12PipelineState* alpha_pso_ = nullptr;
    std::shared_ptr<Shared> shared_;
    std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<Built>> nets_;
    Built* current_ = nullptr;                   // ready()'s answer for this evaluate
    std::vector<std::pair<uint64_t, std::shared_ptr<Built>>> retired_;
    std::vector<std::pair<uint64_t, std::shared_ptr<FeatureState>>> retired_features_;
    std::vector<std::pair<uint64_t, ID3D12Resource*>> retire_upload_;
    uint64_t tick_ = 0;
    nr::D3d12NetControls controls_;
    NetTemporal temporal_;
    ID3D12QueryHeap* queries_ = nullptr;
    ID3D12Resource* stamps_ = nullptr;
    uint8_t* stamps_map_ = nullptr;
    double hz_ = 0;
    uint64_t frames_ = 0, resets_ = 0, gpu_seen_ = 0;
    unsigned gpu_n_ = 0;
    double gpu_sum_ = 0, gpu_min_ = 0, gpu_max_ = 0, cpu_sum_ = 0;
};

}  // namespace

void set_net_planner(NetPlanner planner, NetControlsPatch patch) { g_planner = planner; g_patch = patch; }
NetPlanner net_planner() { return g_planner.load(); }

std::unique_ptr<D3d12AsdNetwork> make_aco_network(const std::wstring& folder) {
    return std::make_unique<AcoNetwork>(folder);
}

}  // namespace nr::pe::asd
