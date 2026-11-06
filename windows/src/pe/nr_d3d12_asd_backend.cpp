// The native D3D12 ASD backend's frame path (nr_d3d12_asd_backend.hpp), on the game's command list: the RenoDX
// processing's frames through its D3D12 stage around the network; the network calls of the wilsjo2 processing's D3D12
// flow (nr_asi_wilsjo2.cpp) as HLSL copy-in, the network, HLSL writeback. Everything that can fail is checked before
// the first command is recorded, so a false return leaves the list as it came and the Vulkan bridge runs the frame.
#include "nr_d3d12_asd_backend.hpp"
#include "nr_d3d12_asd.hpp"
#include "nr_d3d12_asd_net.hpp"
#include "nr_pe_d3d12split.hpp"
#include "nr_renodx_d3d12.hpp"   // the RenoDX stage on this route
#include "nr_pe_log.hpp"
#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace nr::pe::asd {
namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
// Uses retains the original recording set; D3D12 retirement requires actual completion,
// rather than Recording::ready()'s time/submission fallback for the bridge.
bool recordings_done(const nr::gpu::Uses& uses) {
    return std::all_of(uses.tickets.begin(), uses.tickets.end(), [](const auto& t) {
        return nr::pe::split::completed(t);
    });
}

using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                        UINT, UINT, ID3DBlob**, ID3DBlob**);
const char kShaders[] =
    "cbuffer C : register(b0) { uint g_w; uint g_h; uint g_pad0; uint g_pad1; float4 g_region; };\n"
    "Texture2D<float4> src_tex : register(t0);\n"
    "SamplerState linear_clamp : register(s0);\n"
    "RWTexture2D<float4> dst_tex : register(u0);\n"
    "StructuredBuffer<uint2> lin_src : register(t1);\n"
    "RWStructuredBuffer<uint2> lin_dst : register(u1);\n"
    "[numthreads(8, 8, 1)] void to_linear(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x < g_w && id.y < g_h) {\n"
    "        uint4 h = f32tof16(src_tex.Load(int3(id.xy, 0)));\n"
    "        lin_dst[id.y * g_w + id.x] = uint2(h.x | (h.y << 16), h.z | (h.w << 16));\n"
    "    }\n"
    "}\n"
    "[numthreads(8, 8, 1)] void from_linear(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= g_w || id.y >= g_h) return;\n"
    "    uint2 p = lin_src[id.y * g_w + id.x];\n"
    "    dst_tex[id.xy] = f16tof32(uint4(p.x & 0xffffu, p.x >> 16, p.y & 0xffffu, p.y >> 16));\n"
    "}\n"
    "[numthreads(8, 8, 1)] void to_motion(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= g_w || id.y >= g_h) return;\n"
    "    const float2 uv = (float2(id.xy) + 0.5) / float2(g_w, g_h);\n"
    "    uint2 h = f32tof16(src_tex.SampleLevel(linear_clamp, uv * g_region.xy + g_region.zw, 0).xy);\n"
    "    lin_dst[id.y * g_w + id.x] = uint2(h.x | (h.y << 16), 0u);\n"
    "}\n"
    "[numthreads(8, 8, 1)] void to_depth(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= g_w || id.y >= g_h) return;\n"
    "    const float2 s = floor((float2(id.xy) + 0.5) * g_region.xy / float2(g_w, g_h));\n"
    "    const int2 t = min(int2(s), int2(g_region.xy) - 1);\n"
    "    uint idx = (id.y * g_w + id.x) * 2;\n"
    "    lin_dst[idx] = uint2(asuint(src_tex.Load(int3(t + int2(g_region.zw), 0)).r), 0u);\n"
    "    lin_dst[idx + 1] = uint2(0u, 0u);\n"
    "}\n";

ID3DBlob* compile(const char* entry, std::string* why) { return compile_cs(kShaders, sizeof kShaders - 1, entry, why); }

}  // namespace

ID3DBlob* compile_cs(const char* source, size_t bytes, const char* entry, std::string* why) {
    static PFN_D3DCompile fn = [] {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        return m ? reinterpret_cast<PFN_D3DCompile>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile"))) : nullptr;
    }();
    if (!fn) { *why = "d3dcompiler_47.dll (D3DCompile) is not available"; return nullptr; }
    ID3DBlob *code = nullptr, *errors = nullptr;
    const HRESULT hr = fn(source, bytes, "nr_d3d12_asd", nullptr, nullptr, entry, "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        *why = std::string("the ") + entry + " shader did not compile";
        if (errors) { *why += ": "; *why += static_cast<const char*>(errors->GetBufferPointer()); }
        release(errors);
        release(code);
        return nullptr;
    }
    release(errors);
    return code;
}

namespace {

// A view format of the resource's: typeless -> its UNORM / FLOAT member; an sRGB format (no typed UAV) -> unknown.
DXGI_FORMAT view_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;      // the motion guide's
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_UNKNOWN;
    default: return f;
    }
}
DXGI_FORMAT depth_view_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16_FLOAT:
        return DXGI_FORMAT_R16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}


struct Backend {
    ID3D12Device* device = nullptr;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState *to_linear = nullptr, *from_linear = nullptr, *to_motion = nullptr, *to_depth = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    UINT increment = 0, slot = 0;
    static constexpr UINT kSlots = 2048;   // the first heap: four descriptors a frame, 512 segments
    // A segment is rewritten only when the GPU has finished the frame that last used it (the net's completion
    // watermark, refresh_executed). Running out of finished segments grows the heap - it never overwrites one still
    // in flight, which would make a recorded dispatch read descriptors another frame wrote.
    UINT segments = kSlots / 4;
    std::vector<int64_t> segment_owner;    // per segment: the frame that last used it (-1: never)
    ID3D12Resource *lin_in = nullptr, *lin_out = nullptr, *lin_mv = nullptr, *lin_depth = nullptr;
    ID3D12Resource* neutral_mv = nullptr;
    uint32_t w = 0, h = 0;
    // A shared_ptr, not a unique_ptr: a reader off the game's thread (the heartbeat, NR Control's status) copies
    //    the pointer under `owner_lock` and uses it outside the lock, so a device swap cannot free it under a reader.
    std::shared_ptr<D3d12AsdNetwork> network;
    bool broken = false;   // something of the device's could not be made: the Vulkan bridge from now on
    // Set while a device generation is being dropped: a stage's destructor calls retire() for the surfaces it still
    // holds, and those must be released here, not queued into the next generation's list (their watermark is gone).
    bool discarding = false;
    std::string broken_why;
    uint64_t tick = 0;
    // The backend's own completion signal: a frame number copied into a readback buffer by the same command list that
    // used the objects, so a retired stage or buffer is released only after the GPU ran past it. An evaluate count is
    // not evidence: a stack records many frames into one list that may still be unsubmitted.
    static constexpr uint32_t kDoneSlots = 128, kTimeRing = 16;
    ID3D12QueryHeap* queries = nullptr;
    ID3D12Resource* stamps = nullptr;
    uint8_t* stamps_map = nullptr;
    double hz = 0.0;
    nr::gpu::Ticket stamp_uses[kTimeRing] = {};
    float stage_ring[32] = {};
    size_t stage_ring_at = 0;
    ID3D12Resource* token = nullptr;     // upload: the frame numbers to echo
    uint64_t* token_map = nullptr;
    uint32_t token_slots = 0;            // growable: a slot is rewritten only after its holder executed
    ID3D12Resource* done = nullptr;      // readback: what the GPU echoed
    uint64_t* done_map = nullptr;
    uint64_t frames = 0;                 // frames recorded by this backend
    int64_t executed = -1;               // newest frame the GPU finished
    // The recording ticket of the frame being recorded: every resource that frame makes is retired by it, so the
    // resource's own recording is its completion evidence (not a global "newest frame finished" assumption).
    std::shared_ptr<nr::gpu::Recording> booking;
    // Every recording this generation made (one per recorded frame/list), kept so the generation is released only
    // when all of them are strictly done - not by the newest one, and not by a token maximum.
    struct FrameRecording {
        int64_t frame;                                // the backend frame number this recording belongs to
        uint64_t feature;                             // the game feature the frame belonged to
        std::shared_ptr<nr::gpu::Recording> rec;
    };
    std::vector<FrameRecording> recordings;
    // The evidence a retired object waits for: this generation's unfinished recordings at the moment it is retired,
    // conservatively - the frame being recorded is not proof by itself, an earlier recording of the same generation may
    // still be unsubmitted and run after a newer one finished (nr::gpu::Uses, the same owner-less use set the Vulkan
    // path retires with).
    nr::gpu::Uses uses_now() const {
        nr::gpu::Uses u;
        for (const auto& r : recordings)
            if (r.rec && !nr::pe::split::completed(r.rec)) u.tickets.push_back(r.rec);
        if (booking && std::find(u.tickets.begin(), u.tickets.end(), booking) == u.tickets.end())
            u.tickets.push_back(booking);
        return u;
    }
    // A grown-away token buffer, released by the recording of the frame that last read it (a predecessor of the token
    // block is read by the same list that copies this frame's number, so the frame's own recording is the evidence).
    struct RetiredToken {
        int64_t frame;
        nr::gpu::Uses uses;
        ID3D12Resource* p;
    };
    std::vector<RetiredToken> token_retired;
    // A value the GPU has not written yet reads as ~0, and only a slot whose holder matches its own index is a
    // completion: a frame number ahead of the newest record is a torn read, not evidence.
    void refresh_executed() {
        if (!done_map) return;
        const uint64_t max_f = frames ? frames - 1 : 0;
        for (uint32_t s = 0; s < kDoneSlots; ++s) {
            const uint64_t v = done_map[s];
            if (v != ~uint64_t(0) && v <= max_f && (v % kDoneSlots) == s && int64_t(v) > executed)
                executed = int64_t(v);
        }
    }
    struct Retired {
        int64_t frame;                                   // the backend's frame number that last used it
        nr::gpu::Uses uses;                               // the recordings that may still run past it
        IUnknown* p;
    };
    std::vector<Retired> retired;
    // The RenoDX route's stages, one a feature (nr_pe_session.cpp's RenoDXEntry: its working resolution's debounce
    // and its stage, keyed by everything a change of takes a new one).
    struct RenodxEntry {
        std::unique_ptr<renodx12::Stage> stage;
        uint32_t stack = 0;
        uint32_t w = 0, h = 0, nw = 0, nh = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        int hdr = 0, present = 0;
        float scale = 1.0f;
        bool follow = false;
        uint32_t applied_w = 0, applied_h = 0, pending_w = 0, pending_h = 0, pending_frames = 0;
    };
    std::map<uint64_t, RenodxEntry> renodx;
    // A replaced stage (an extent change) is released once the recording of the frame that last used it is done - the
    // same evidence as `retired`, not the backend's newest-frame watermark: a stack records many frames into one list
    // that may still be unsubmitted, and a frame number says nothing about whether the GPU ran past that stage's work.
    struct RetiredStage {
        int64_t frame;
        nr::gpu::Uses uses;
        const char* why;   // why it was replaced: a key change, a failed set(), or the feature being released
        std::unique_ptr<renodx12::Stage> stage;
    };
    std::vector<RetiredStage> renodx_retired;
};
Backend g;
std::atomic<float> g_stage_avg_ms{0.f}, g_last_fast_passes{0.f};
std::atomic<uint32_t> g_last_nr_w{0}, g_last_nr_h{0};
std::atomic<bool> g_last_fast_fallback{false};
void note_stage_gpu(double ms) {
    if (g.stage_ring_at == 1 && ms < double(g.stage_ring[0]) * 0.75) g.stage_ring_at = 0;
    g.stage_ring[g.stage_ring_at % 32] = float(ms);
    ++g.stage_ring_at;
    const size_t n = std::min<size_t>(g.stage_ring_at, 32);
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += g.stage_ring[i];
    const float avg = float(s / double(n));
    g_stage_avg_ms.store(avg);

}
// Guards `g.device` and `g.network` against a reader that does not run inside evaluate (the heartbeat's device-removed
// poll, NR Control's status): the readers take a reference under it, the evaluate/drop_device path swaps them under it.
// Recursive because the writers already call one another (release_generation -> drop_device).
std::recursive_mutex g_owner_lock;
// Device generations that were swapped while work of theirs could still be in flight. Each is released only when its
// own completion evidence (its Recording/ticket, or its token watermark) says the GPU ran past its last frame.
std::vector<std::unique_ptr<Backend>> g_old_generations;

// An upload or readback buffer of `bytes` in `state` (the completion pair).
ID3D12Resource* heap_buffer(ID3D12Device* device, D3D12_HEAP_TYPE type, uint64_t bytes, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

// The frame the object was last used in: the one about to be recorded (a replaced stage was used by the frame before,
// so this is one frame of margin, never less).
void retire(IUnknown* p) {
    if (!p) return;
    if (g.discarding) { p->Release(); return; }
    g.retired.push_back({int64_t(g.frames), g.uses_now(), p});
}
void release_generation(Backend& b);

// Has this backend frame's own recording finished? Frames without a Recording (the harness's early frames, a device
// the split never hooked) fall back to the token watermark.
bool frame_done(int64_t frame) {
    for (const auto& r : g.recordings)
        if (r.frame == frame && r.rec) return nr::pe::split::completed(r.rec);
    g.refresh_executed();
    return g.executed >= frame;
}

// A generation is finished when every recording it made is strictly ready (sealed, every submission answered). A
// generation that made none (the offline harness, whose split returns nothing) keeps the token watermark.
bool generation_done(Backend& b) {
    for (const auto& rec : b.recordings) {
        if (!rec.rec) continue;
        if (!nr::pe::split::completed(rec.rec)) return false;
    }
    if (!b.recordings.empty()) return true;
    b.refresh_executed();
    return b.frames == 0 || b.executed >= int64_t(b.frames) - 1;
}

void sweep() {
    ++g.tick;
    // Previous generations: released only when their own evidence has passed their last recorded frame.
    for (size_t i = 0; i < g_old_generations.size();) {
        Backend& b = *g_old_generations[i];
        if (generation_done(b)) { release_generation(b); g_old_generations.erase(g_old_generations.begin() + long(i)); }
        else ++i;
    }
    // A recording whose every submission answered no longer gates the generation: drop it, so the set does not
    // accumulate one entry per frame for the life of the device.
    for (size_t i = 0; i < g.recordings.size();) {
        const bool dropped = g.recordings[i].rec && g.recordings[i].rec->never_submitted();
        if (dropped) release_renodx_feature(g.recordings[i].feature);   // closed, never executed: history ends
        if (!g.recordings[i].rec || dropped || nr::pe::split::completed(g.recordings[i].rec))
            g.recordings.erase(g.recordings.begin() + long(i));
        else ++i;
    }
    g.refresh_executed();
    auto it = std::remove_if(g.retired.begin(), g.retired.end(), [](const auto& r) {
        // An owner released by its own recording's ticket once that recording is sealed and its GPU work finished;
        // a resource retired without a ticket keeps the backend's own watermark.
        if (!r.uses.tickets.empty()) {
            if (!recordings_done(r.uses)) return false;
        } else if (g.executed < r.frame) {
            return false;
        }
        r.p->Release();
        return true;
    });
    g.retired.erase(it, g.retired.end());
    // The same evidence as `retired`: the frame's own recording when there is one, the watermark otherwise.
    auto frame_evidence = [](int64_t frame, const nr::gpu::Uses& uses) {
        if (!uses.tickets.empty()) return recordings_done(uses);
        return g.executed >= frame;
    };
    auto rs = std::remove_if(g.renodx_retired.begin(), g.renodx_retired.end(), [&](const auto& r) {
        if (!frame_evidence(r.frame, r.uses)) return false;
        // A replaced stage's own destruction: one line, so an extent change can be seen to retire the old stage rather
        // than merely producing output for the new one.
        // The evidence, not the frame number: how many recordings the retirement waited on and that all of them are
        // done (each is sealed and every submission has answered).
        nr::pe::log("[asd] a replaced RenoDX stage was released (retired at frame %lld, reason %s, %zu recording(s) done, "
                    "stage %p)", (long long)r.frame, r.why ? r.why : "?", r.uses.tickets.size(),
                    static_cast<void*>(r.stage.get()));
        return true;
    });
    g.renodx_retired.erase(rs, g.renodx_retired.end());
    auto rt = std::remove_if(g.token_retired.begin(), g.token_retired.end(), [&](auto& r) {
        if (!frame_evidence(r.frame, r.uses)) return false;
        r.p->Release();
        return true;
    });
    g.token_retired.erase(rt, g.token_retired.end());
}

// Recorded at the end of every frame, after everything that uses a stage: the GPU copies this frame's number back,
// and sweep() releases what an older frame used.
void mark_frame(ID3D12GraphicsCommandList* cmd) {
    if (!g.token || !g.token_map || !g.done) return;
    const uint64_t frame = g.frames;
    // Grow the token block before rewriting a slot that the GPU has not finished: a stack records many frames into
    // one still-unsubmitted list, and reusing a slot would make the earlier frame's copy land in a later frame's
    // number, so sweep() would believe work is done that has not run.
    if (frame >= g.token_slots && int64_t(frame - g.token_slots) > g.executed) {
        const uint64_t slots = uint64_t(g.token_slots) * 2;
        ID3D12Resource* grown = heap_buffer(g.device, D3D12_HEAP_TYPE_UPLOAD, slots * 8, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* tm = nullptr;
        if (!grown || FAILED(grown->Map(0, nullptr, &tm)) || !tm) {
            release(grown);
            return;   // no completion signal this frame: retirement simply waits (never less safe)
        }
        g.token_retired.push_back({int64_t(g.frames), g.uses_now(), g.token});   // the last record that reads the old block
        g.token = grown;
        g.token_map = static_cast<uint64_t*>(tm);
        g.token_slots = uint32_t(slots);
    }
    // The upload block grows; the readback keeps kDoneSlots entries (copies run in queue order, so a readback slot
    // is rewritten only after the copy into it has run).
    const uint64_t up = (frame % g.token_slots) * 8;
    const uint64_t rb = (frame % Backend::kDoneSlots) * 8;
    g.token_map[up / 8] = frame;
    cmd->CopyBufferRegion(g.done, rb, g.token, up, 8);
    ++g.frames;
}

ID3D12Resource* linear_buffer(ID3D12Device* device, uint64_t bytes);
std::wstring widen(const char* u8);




bool build(ID3D12Device* device, std::string* why) {
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    D3D12_ROOT_PARAMETER p[5]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 8;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable.NumDescriptorRanges = 1;
    p[2].DescriptorTable.pDescriptorRanges = &ranges[1];
    p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[3].Descriptor.ShaderRegister = 1;
    p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[4].Descriptor.ShaderRegister = 1;
    for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 5;
    rd.pParameters = p;
    // to_motion's sampler: the runtime's linear_sampler (LINEAR, mip NEAREST, CLAMP_TO_EDGE).
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = 0.0f;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rd.NumStaticSamplers = 1;
    rd.pStaticSamplers = &ss;
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    release(errors);
    if (FAILED(hr) || !blob) { *why = "D3D12SerializeRootSignature failed"; return false; }
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rs));
    release(blob);
    if (FAILED(hr)) { *why = "CreateRootSignature failed"; return false; }
    for (auto [entry, pso] : {std::pair{"to_linear", &g.to_linear}, std::pair{"from_linear", &g.from_linear},
                              std::pair{"to_motion", &g.to_motion}, std::pair{"to_depth", &g.to_depth}}) {
        ID3DBlob* code = compile(entry, why);
        if (!code) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g.rs;
        pd.CS.pShaderBytecode = code->GetBufferPointer();
        pd.CS.BytecodeLength = code->GetBufferSize();
        hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(pso));
        release(code);
        if (FAILED(hr)) { *why = std::string("CreateComputePipelineState(") + entry + ") failed"; return false; }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = Backend::kSlots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap)))) { *why = "CreateDescriptorHeap failed"; return false; }
    g.increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // The ACO network, lowered by the planner the .asi sets (nr_asi_main.cpp).
    if (!net_planner()) { *why = "no network planner"; return false; }
    {
        std::lock_guard<std::recursive_mutex> l(g_owner_lock);
        g.network = make_aco_network(folder());
    }
    const char* nwhy = "";
    if (!g.network->create(device, &nwhy)) { *why = std::string("the network: ") + nwhy; return false; }
    // The completion pair: a frame number the GPU echoes back once the commands that used a retired object have run.
    g.token = heap_buffer(device, D3D12_HEAP_TYPE_UPLOAD, Backend::kDoneSlots * 8, D3D12_RESOURCE_STATE_GENERIC_READ);
    g.done = heap_buffer(device, D3D12_HEAP_TYPE_READBACK, Backend::kDoneSlots * 8, D3D12_RESOURCE_STATE_COPY_DEST);
    g.token_slots = Backend::kDoneSlots;
    if (!g.token || !g.done) { *why = "the backend's completion buffers could not be made"; return false; }
    {
        void* m = nullptr;
        if (FAILED(g.token->Map(0, nullptr, &m)) || !m) { *why = "the backend's token buffer could not be mapped"; return false; }
        g.token_map = static_cast<uint64_t*>(m);
        m = nullptr;
        if (FAILED(g.done->Map(0, nullptr, &m)) || !m) { *why = "the backend's readback could not be mapped"; return false; }
        g.done_map = static_cast<uint64_t*>(m);
        std::memset(g.done_map, 0xFF, Backend::kDoneSlots * 8);
        D3D12_QUERY_HEAP_DESC qh{}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = 2 * Backend::kTimeRing;
        ID3D12CommandQueue* q = nullptr; D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; UINT64 hz = 0;
        if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) && q) { q->GetTimestampFrequency(&hz); q->Release(); }
        if (hz && SUCCEEDED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&g.queries)))) {
            g.stamps = heap_buffer(device, D3D12_HEAP_TYPE_READBACK, 16 * Backend::kTimeRing, D3D12_RESOURCE_STATE_COPY_DEST);
            void* sm = nullptr;
            if (g.stamps && SUCCEEDED(g.stamps->Map(0, nullptr, &sm)) && sm) {
                g.stamps_map = static_cast<uint8_t*>(sm); g.hz = double(hz); std::memset(g.stamps_map, 0, 16 * Backend::kTimeRing);
            }
        }
    }
    note_network(g.network->name());
    renodx12::prewarm_shaders(widen(nr::pe::module_folder()) + L"\\dlssnr-amd\\shaders\\renodx");
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd_mv{};
    rd_mv.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd_mv.Width = 1; rd_mv.Height = 1; rd_mv.DepthOrArraySize = 1; rd_mv.MipLevels = 1;
    rd_mv.Format = DXGI_FORMAT_R16G16_FLOAT;
    rd_mv.SampleDesc.Count = 1;
    // Neutral (0, 0) motion buffer for no-MV frames: D3D12_HEAP_FLAG_NONE without
    // D3D12_HEAP_FLAG_CREATE_NOT_ZEROED guarantees zero-initialisation (0.0f, 0.0f).
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd_mv,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                               nullptr, IID_PPV_ARGS(&g.neutral_mv)))) {
        *why = "CreateCommittedResource(neutral_mv) failed"; return false;
    }
    return true;
}

void release_generation(Backend& b);

void drop_device() {
    std::lock_guard<std::recursive_mutex> owner(g_owner_lock);
    // A device is swapped while a frame of ours may still be queued: the whole generation is handed over and released
    // by its own evidence, never here (a pointer change is not proof that the GPU is idle).
    if (g.device && (g.frames > 0 || !g.recordings.empty()) && !generation_done(g)) {
        g_old_generations.push_back(std::make_unique<Backend>(std::move(g)));
        g = Backend{};
        return;
    }
    g.discarding = true;   // the destructors below release their own surfaces here, not into the next generation
    g.renodx.clear();
    g.renodx_retired.clear();
    retire(g.rs); retire(g.to_linear); retire(g.from_linear); retire(g.to_motion); retire(g.to_depth); retire(g.heap);
    retire(g.lin_in); retire(g.lin_out); retire(g.lin_mv); retire(g.lin_depth);
    retire(g.neutral_mv);
    if (g.token_map) g.token->Unmap(0, nullptr);
    if (g.done_map) g.done->Unmap(0, nullptr);
    release(g.token); release(g.done);
    if (g.stamps_map) g.stamps->Unmap(0, nullptr);
    release(g.stamps); release(g.queries); g.stamps_map = nullptr; g.hz = 0.0;
    for (auto& u : g.stamp_uses) u.reset();
    for (auto& r : g.token_retired) r.p->Release();
    // The lists belong to this device: nothing of it is in flight once drop_device is called (the same reason the
    // resources above are released here), and a later device must not inherit watermarks from this one.
    for (auto& r : g.retired) r.p->Release();
    g.retired.clear();
    g.token_retired.clear();
    g.token_map = nullptr; g.done_map = nullptr; g.token_slots = 0; g.frames = 0; g.executed = -1;
    g.segments = Backend::kSlots / 4;
    g.segment_owner.clear();
    g.slot = 0;
    g.rs = nullptr; g.to_linear = g.from_linear = g.to_motion = g.to_depth = nullptr; g.heap = nullptr;
    g.lin_in = g.lin_out = g.lin_mv = g.lin_depth = nullptr;
    g.neutral_mv = nullptr;
    g.discarding = false;
    g.w = g.h = 0;
    g.network.reset();
    g.renodx.clear();   // its stages own nothing of the device's that outlives it
    if (g.device) g.device->Release();
    g.device = nullptr;
    g.broken = false;
}

// Releases everything of a generation whose work has finished: the immediate body above, run on that generation.
void release_generation(Backend& b) {
    std::lock_guard<std::recursive_mutex> owner(g_owner_lock);   // `g` itself is swapped here
    Backend keep = std::move(g);
    g = std::move(b);
    drop_device();          // the immediate-release body, on this generation
    g.discarding = false;
    g = std::move(keep);
}

ID3D12Resource* linear_buffer(ID3D12Device* device, uint64_t bytes) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

bool typed_uav(ID3D12Device* device, DXGI_FORMAT f) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = f;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs))) return false;
    return (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
}

void transition(D3D12_RESOURCE_BARRIER& b, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    b = D3D12_RESOURCE_BARRIER{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
}

bool fallback(const std::string& why) {
    note_backend(("not run (the Vulkan bridge): " + why).c_str(), false);
    return false;
}


std::wstring widen(const char* u8) {
    std::wstring out;
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, nullptr, 0);
    if (n > 0) { out.resize(size_t(n) - 1); MultiByteToWideChar(CP_UTF8, 0, u8, -1, out.data(), n); }
    return out;
}

// The RenoDX route's frame (nr_pe_session.cpp's run_renodx, on the ASD route): the stage around the network, one a
// feature, the network through this backend's own (g.network) over its linear buffers.
uint64_t stack_feature(uint64_t feature, uint32_t k) {
    return k == 0 ? feature : (uint64_t(1) << 62) | (feature << 8) | k;
}
constexpr uint64_t kUnnamedStack = 0xFFFFFFu;

bool renodx_frame(const nr::dlssnr::D3d12BackendFrame& f, D3D12_RESOURCE_STATES color_state,
                  D3D12_RESOURCE_STATES output_state) {
    if (!f.renodx_settings) return fallback("the RenoDX route without its frame (no settings)");
    const nr::renodx::Settings& s = *f.renodx_settings;
    const D3D12_RESOURCE_DESC sd = f.color->GetDesc(), dd = f.output->GetDesc();
    if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        sd.DepthOrArraySize != 1 || dd.DepthOrArraySize != 1 || sd.SampleDesc.Count != 1 || dd.SampleDesc.Count != 1)
        return fallback("the RenoDX route: colour or output is not a single-sampled 2D texture");
    const DXGI_FORMAT sf = view_format(sd.Format), df = view_format(dd.Format);
    if (sf == DXGI_FORMAT_UNKNOWN || df == DXGI_FORMAT_UNKNOWN) return fallback("the RenoDX route: an sRGB format");
    if (!(dd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || !typed_uav(f.device, df))
        return fallback("the RenoDX route: the output's format has no typed UAV store");
    const uint32_t w = std::min<uint64_t>({f.width, sd.Width, dd.Width});
    const uint32_t h = std::min<uint64_t>({f.height, sd.Height, dd.Height});
    if (!w || !h) return fallback("the RenoDX route: an empty extent");

    // The network's extent (nr_pe_session.cpp's renodx_extent: RenoDX's ComputeNrWorkingResolution, a size the
    // game's guides change to taken once it has held kContractDebounceFrames).
    Backend::RenodxEntry& e = g.renodx[f.feature];
    // Session::record_renodx: only passes removed from the stack surrender their histories. The surviving passes
    // keep their latch/seed; re-added passes start fresh instead of reviving the history from an earlier stack.
    const uint32_t passes = renodx::stack_passes(s);
    const uint64_t named = f.feature ? f.feature : kUnnamedStack;
    if (g.network) {
        for (uint32_t k = passes; k < e.stack; ++k) g.network->release_feature(stack_feature(named, k));
        if (!f.feature && passes == 1 && e.stack > 1) g.network->release_feature(named);
    }
    e.stack = passes;
    uint32_t nw = 0, nh = 0;
    renodx::working_resolution(s, w, h, f.motion_w, f.motion_h, &nw, &nh);
    const bool settings_changed = e.scale != s.resolution_scale || e.follow != s.follow_input_res || e.w != w ||
                                  e.h != h;
    e.scale = s.resolution_scale;
    e.follow = s.follow_input_res;
    if (e.applied_w == 0 || settings_changed || (nw == e.applied_w && nh == e.applied_h)) {
        e.applied_w = nw; e.applied_h = nh; e.pending_frames = 0;
    } else {
        if (nw != e.pending_w || nh != e.pending_h) { e.pending_w = nw; e.pending_h = nh; e.pending_frames = 0; }
        if (++e.pending_frames >= renodx::kContractDebounceFrames) { e.applied_w = nw; e.applied_h = nh; e.pending_frames = 0; }
        else { nw = e.applied_w; nh = e.applied_h; }
    }

    std::string why;
    if (!g.network->ready(f.device, nw, nh, &why)) {
        if (g.network->building() && f.waiting) {
            *f.waiting = 1;
            note_backend(("not run yet: the network: " + why + " (frames pass through with no NR; the bridge is not used)").c_str(), false);
            return false;
        }
        return fallback("the RenoDX route: the network: " + why);
    }
    note_network(g.network->name());

    if (!e.stage || e.w != w || e.h != h || e.nw != nw || e.nh != nh || e.format != sf ||
        e.hdr != f.renodx_hdr_flag || e.present != f.renodx_present) {
        renodx12::Stage::Config c;
        c.device = f.device;
        c.shaders = widen(nr::pe::module_folder()) + L"\\dlssnr-amd\\shaders\\renodx";
        c.width = w; c.height = h;
        c.nr_width = nw; c.nr_height = nh;
        c.frame_format = sf;
        c.hdr_flag = f.renodx_hdr_flag != 0;
        c.present = uint32_t(f.renodx_present);
        c.settings = s;
        c.feature = f.feature;
        // Only an object that existed: on the first make there is nothing to retire, and a null entry would be logged
        // as a replaced stage later.
        if (e.stage) g.renodx_retired.push_back({int64_t(g.frames), g.uses_now(), "key", std::move(e.stage)});   // released once its frame's recording is done
        auto made = renodx12::Stage::make(c, &why);
        if (made) made->set_retire([](IUnknown* r) { retire(r); });   // released on the backend's watermark
        if (made) {
            made->set_completed([](uint64_t f) { return frame_done(int64_t(f)); });
        }
        if (!made) return fallback("the RenoDX route: " + why);
        e.stage = std::move(made);
        e.w = w; e.h = h; e.nw = nw; e.nh = nh; e.format = sf;
        e.hdr = f.renodx_hdr_flag; e.present = f.renodx_present;
        nr::pe::log("[nr] the RenoDX route (D3D12): feature #%u at %ux%u (network %ux%u)", unsigned(f.feature), w, h, nw, nh);
    }
    if (!e.stage->set(s)) {
        // Relative <-> absolute units (NRLinearUnitNits, NRSourceEncoding): a stage made with the new settings
        // takes the frame now (the Vulkan route retires the old one and waits a frame; the bridge is never used here).
        if (e.stage) g.renodx_retired.push_back({int64_t(g.frames), g.uses_now(), "set", std::move(e.stage)});
        renodx12::Stage::Config c;
        c.device = f.device;
        c.shaders = widen(nr::pe::module_folder()) + L"\\dlssnr-amd\\shaders\\renodx";
        c.width = w; c.height = h;
        c.nr_width = nw; c.nr_height = nh;
        c.frame_format = sf;
        c.hdr_flag = f.renodx_hdr_flag != 0;
        c.present = uint32_t(f.renodx_present);
        c.settings = s;
        c.feature = f.feature;
        e.stage = renodx12::Stage::make(c, &why);
        if (e.stage) e.stage->set_retire([](IUnknown* r) { retire(r); });
        if (e.stage) e.stage->set_completed([](uint64_t f) { return frame_done(int64_t(f)); });
        if (!e.stage) return fallback("the RenoDX route: " + why);
    }

    renodx12::Frame rf;
    rf.colour = f.color;
    rf.colour_state = color_state;
    rf.output = f.output;
    rf.output_state = output_state;
    rf.reset = f.reset != 0;
    rf.after_upscale = f.renodx_after_upscale != 0;
    rf.fast_motion_available = f.renodx_fast_motion != 0;
    rf.motion = f.motion;
    rf.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    rf.motion_x = f.motion_x; rf.motion_y = f.motion_y;
    rf.motion_w = f.motion_w ? f.motion_w : (f.motion ? uint32_t(f.motion->GetDesc().Width) : 0);   // 0: the whole texture
    rf.motion_h = f.motion_h ? f.motion_h : (f.motion ? f.motion->GetDesc().Height : 0);
    rf.motion_scale_x = f.motion_scale_x;
    rf.motion_scale_y = f.motion_scale_y;
    rf.depth = f.depth;
    rf.depth_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    rf.depth_x = f.depth_x; rf.depth_y = f.depth_y;
    rf.depth_w = f.depth_w; rf.depth_h = f.depth_h;
    rf.depth_inverted = f.depth_inverted != 0;
    rf.exposure = f.exposure;
    rf.exposure_state = static_cast<D3D12_RESOURCE_STATES>(f.exposure_state);
    rf.pre_exposure = f.pre_exposure;
    rf.exposure_scale = f.exposure_scale;
    rf.auto_exposure = f.auto_exposure != 0;
    rf.create_flags_known = f.create_flags_known != 0;
    rf.preprocess.enabled = f.preprocess != 0;
    rf.preprocess.exposure = f.prep_exposure;
    rf.preprocess.bias_ev = f.prep_bias_ev;
    rf.preprocess.curve = f.prep_curve;
    rf.preprocess.contrast = f.prep_contrast;
    rf.preprocess.saturation = f.prep_saturation;
    rf.preprocess.inverse = f.prep_inverse != 0;
    rf.present_white_nits = f.renodx_white_nits;
    rf.motion_without_depth = g.network->motion_without_depth();
    rf.base_controls.style = f.style;
    rf.base_controls.intensity = f.intensity;
    rf.base_controls.local_tone = f.local_tone;
    rf.base_controls.local_structure = f.local_structure;
    rf.base_controls.skin_structure = f.skin_structure;
    rf.base_controls.automatic_mask = f.automatic_mask != 0;
    rf.base_controls.round_u8 = df == DXGI_FORMAT_R8G8B8A8_UNORM || df == DXGI_FORMAT_B8G8R8A8_UNORM ||
                                df == DXGI_FORMAT_B8G8R8X8_UNORM;
    rf.base_controls.reversible = f.reversible_mode >= 0 && f.reversible_mode <= 4 ? uint32_t(f.reversible_mode) : 0u;
    rf.base_controls.history_strength = f.history_strength;
    rf.base_controls.noise_every_frame = f.noise_every_frame != 0;

    const uint64_t bytes = uint64_t(nw) * nh * 8;
    const char* nwhy = "NR is disabled";
    const auto net = [&](uint32_t, uint64_t, const nr::D3d12NetControls& controls, NetTemporal nt,
                         ID3D12Resource* in, ID3D12Resource* out) {
        nt.motion_f16 = true;
        g.network->controls(controls);
        g.network->temporal(nt);
        nwhy = "";
        const LinearFrame lin{in, in->GetGPUVirtualAddress(), nw, nh, bytes};
        const LinearFrame lout{out, out->GetGPUVirtualAddress(), nw, nh, bytes};
        return g.network->record(f.cmd, lin, lout, &nwhy);
    };
    const auto state = nr::pe::split::snapshot(f.cmd);
    if (!state) return fallback("the RenoDX route: no snapshot of the command list's state");
    e.stage->set_frame(g.frames);
    uint32_t ts = Backend::kTimeRing;
    if (g.stamps_map) {
        for (uint32_t i = 0; i < Backend::kTimeRing; ++i) {
            if (g.stamp_uses[i] ? nr::pe::split::completed(g.stamp_uses[i]) : !g.booking) {
                uint64_t t2[2];
                std::memcpy(t2, g.stamps_map + i * 16, 16);
                if (t2[0] && t2[1] > t2[0]) {
                    note_stage_gpu(double(t2[1] - t2[0]) * 1000.0 / g.hz);
                    std::memset(g.stamps_map + i * 16, 0, 16);
                }
                g.stamp_uses[i].reset();
            }
            if (!g.stamp_uses[i] && ts == Backend::kTimeRing) ts = i;
        }
        if (!g.booking) ts = uint32_t(g.frames % Backend::kTimeRing);
    }
    const bool timed = g.stamps_map && ts < Backend::kTimeRing;
    if (timed) {
        g.stamp_uses[ts] = g.booking;
        std::memset(g.stamps_map + ts * 16, 0, 16);
        f.cmd->EndQuery(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, ts * 2);
    }
    const uint32_t ran = e.stage->record(f.cmd, rf, net);
    if (timed) {
        f.cmd->EndQuery(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, ts * 2 + 1);
        f.cmd->ResolveQueryData(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, ts * 2, 2, g.stamps, uint64_t(ts) * 16);
    }
    g_last_nr_w.store(nw); g_last_nr_h.store(nh);
    const auto facts = e.stage->facts();
    g_last_fast_passes.store(facts.fast_passes);
    g_last_fast_fallback.store(facts.fast_fallback);
    nr::pe::split::restore(f.cmd, state);
    // 0 passes is a legitimate copy-through only when the stage says it made everything it needed; otherwise the
    // output may not have been written at all and the caller must take its own path.
    if (!e.stage->record_ok()) {
        // The frame is dropped after part of it was recorded: its history can no longer be trusted (the original
        // Runtime's rule for a dropped recording), so this feature's state goes rather than a stale latch surviving.
        release_renodx_feature(f.feature);
        return fallback("the RenoDX stage could not make a surface this frame needs");
    }
    char line[200];
    const bool net_ok = e.stage->net_ran();
    if (net_ok)
        std::snprintf(line, sizeof line, "ran the RenoDX stage (D3D12) %ux%u, network %ux%u: %u pass%s", w, h, nw, nh,
                      ran, ran == 1 ? "" : "es");
    else
        std::snprintf(line, sizeof line,
                      "ran the RenoDX stage (D3D12) %ux%u, network %ux%u: %u pass%s, the network recorded nothing (%s)",
                      w, h, nw, nh, ran, ran == 1 ? "" : "es", nwhy);
    note_backend_produced(ran > 0);   // what Stage::record returns, the same ran the Vulkan path reports
    note_backend(line, true);
    return true;
}
}  // namespace

bool wilsjo2_ready(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, uint32_t w, uint32_t h, std::string* why,
                   bool* building) {
    if (building) *building = false;
    std::string w_;
    std::string& y = why ? *why : w_;
    if (!enabled()) { y = "ASD is off (dlssnr-amd-asd.ini Enabled = 0)"; return false; }
    if (!device || !cmd) { y = "no device or command list"; return false; }
    // A list state not known yet (the hooks came after its Reset), a recording blocked this moment (render pass,
    // query, predicate, bundle, too many cuts) and a canary not run / still running are waits, not the bridge; only the
    // cut off for good and a canary MISS / FAIL take the bridge
    if (!nr::pe::split::prepare(cmd, &y)) {
        if (building && !nr::pe::split::disabled()) *building = true;
        y = "the command list's state is not known (" + y + ")";
        return false;
    }
    on_device(device);   // the canary, once
    const Canary c = canary();
    if (c != Canary::kHit) {
        if (building && (c == Canary::kNotRun || c == Canary::kRunning)) *building = true;
        y = std::string("canary ") + canary_name(c);
        return false;
    }
    if (g.device != device) {
        drop_device();
        device->AddRef();
        {
            std::lock_guard<std::recursive_mutex> l(g_owner_lock);
            g.device = device;
        }
        if (!build(device, &y)) { g.broken = true; g.broken_why = y; }
    }
    if (g.broken) { y = g.broken_why; return false; }
    if (!g.network->ready(device, w, h, &y)) {
        if (building) *building = g.network->building();
        y = "the network: " + y;
        return false;
    }
    note_network(g.network->name());
    return true;
}

bool evaluate_backend(const nr::dlssnr::D3d12BackendFrame& f) {
    if (!f.device || !f.cmd || !f.color || !f.output) return false;
    const auto color_state = static_cast<D3D12_RESOURCE_STATES>(f.color_state);
    const auto output_state = static_cast<D3D12_RESOURCE_STATES>(f.output_state);
    const bool renodx = f.renodx_route != 0;

    sweep();
    std::string why;
    // As wilsjo2_ready: passing states wait (the frame as it came, no NR, no bridge); lasting ones take the bridge
    const auto wait = [&](const std::string& y) {
        *f.waiting = 1;
        note_backend(("not run yet: " + y + " (frames pass through with no NR; the bridge is not used)").c_str(), false);
        return false;
    };
    if (!nr::pe::split::prepare(f.cmd, &why)) {
        if (f.waiting && !nr::pe::split::disabled()) return wait("the command list's state is not known (" + why + ")");
        return fallback("the command list's state is not known (" + why + ")");
    }
    on_device(f.device);   // the canary, once
    const Canary c = canary();
    if (c != Canary::kHit) {
        if (f.waiting && (c == Canary::kNotRun || c == Canary::kRunning)) return wait(std::string("canary ") + canary_name(c));
        return fallback(std::string("canary ") + canary_name(c));
    }

    if (g.device != f.device) {
        drop_device();
        f.device->AddRef();
        {
            std::lock_guard<std::recursive_mutex> l(g_owner_lock);
            g.device = f.device;
        }
        if (!build(f.device, &why)) { g.broken = true; g.broken_why = why; }
    }
    if (g.broken) return fallback(g.broken_why);
    g.network->history_f16(f.history_f16 != 0);   // [Network] History Precision, live

    if (renodx) {
        g.booking = nr::pe::split::book(f.cmd);
        if (g.booking) g.recordings.push_back({int64_t(g.frames), f.feature, g.booking});
        const bool ran = renodx_frame(f, color_state, output_state);
        // Marked even when the stage reports failure: a failed frame may have recorded some of its dispatches, and
        // those commands still hold internal resources that must not be released until the GPU ran past them.
        mark_frame(f.cmd);
        return ran;
    }

    // The network call of the wilsjo2 flow on D3D12: Preprocess is the flow's own there (wilsjo2_prep, prep_outside);
    // one that asks the network's runtime for it is not this path's.
    if (f.preprocess) return fallback("Preprocess in the network's runtime (the wilsjo2 flow runs its own)");
    const D3D12_RESOURCE_DESC sd = f.color->GetDesc(), dd = f.output->GetDesc();
    if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        sd.DepthOrArraySize != 1 || dd.DepthOrArraySize != 1 || sd.SampleDesc.Count != 1 || dd.SampleDesc.Count != 1)
        return fallback("colour or output is not a single-sampled 2D texture");
    const DXGI_FORMAT sf = view_format(sd.Format), df = view_format(dd.Format);
    if (sf == DXGI_FORMAT_UNKNOWN || df == DXGI_FORMAT_UNKNOWN) return fallback("an sRGB colour or output format");
    const bool have_motion = f.motion != nullptr;
    ID3D12Resource* motion_res = f.motion;
    D3D12_RESOURCE_DESC md{};
    DXGI_FORMAT mf = DXGI_FORMAT_UNKNOWN;
    if (have_motion) {
        md = f.motion->GetDesc();
        mf = view_format(md.Format);
        if (md.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || md.SampleDesc.Count != 1 || mf == DXGI_FORMAT_UNKNOWN || !md.Width ||
            !md.Height)
            return fallback("the motion guide is not a single-sampled 2D texture");
    } else {
        if (!g.neutral_mv) return fallback("the neutral motion texture is missing");
        motion_res = g.neutral_mv;
        md = g.neutral_mv->GetDesc();
        mf = DXGI_FORMAT_R16G16_FLOAT;
    }
    if (!(dd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || !typed_uav(f.device, df))
        return fallback("the output's format has no typed UAV store");
    const uint32_t w = std::min<uint64_t>({f.width, sd.Width, dd.Width}), h = std::min<uint32_t>({f.height, sd.Height, dd.Height});
    if (!w || !h) return fallback("an empty extent");
    if (w != g.w || h != g.h) {
        retire(g.lin_in); retire(g.lin_out); retire(g.lin_mv); retire(g.lin_depth);
        const uint64_t bytes8 = uint64_t(w) * h * 8;
        const uint64_t bytes16 = uint64_t(w) * h * 16;
        g.lin_in = linear_buffer(f.device, bytes8);
        g.lin_out = linear_buffer(f.device, bytes8);
        g.lin_mv = linear_buffer(f.device, bytes8);
        g.lin_depth = linear_buffer(f.device, bytes16);
        if (!g.lin_in || !g.lin_out || !g.lin_mv || !g.lin_depth) {
            retire(g.lin_in); retire(g.lin_out); retire(g.lin_mv); retire(g.lin_depth);
            g.lin_in = g.lin_out = g.lin_mv = g.lin_depth = nullptr;
            g.w = g.h = 0;
            return fallback("the linear buffers could not be made");
        }
        g.w = w; g.h = h;
    }
    const bool wants_depth = have_motion && f.depth && !g.network->motion_without_depth();
    DXGI_FORMAT depth_view = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_DESC depth_desc{};
    if (wants_depth) {
        depth_desc = f.depth->GetDesc();
        if (depth_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && depth_desc.SampleDesc.Count == 1 &&
            !(depth_desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) {
            depth_view = depth_view_format(depth_desc.Format);
        }
    }
    const bool have_depth = depth_view != DXGI_FORMAT_UNKNOWN;
    if (wants_depth && !have_depth) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            nr::pe::log("[asd] the depth buffer (DXGI format %u) cannot be read; running without depth",
                        unsigned(depth_desc.Format));
        }
    }
    if (have_depth) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            nr::pe::log("[asd] depth read from guide (DXGI format %u -> %u)", unsigned(depth_desc.Format),
                        unsigned(depth_view));
        }
    }
    {
        std::string nwhy;
        if (!g.network->ready(f.device, w, h, &nwhy)) {
            // While ASD's network builds, no bridge: the model passes the frame through with no NR
            if (g.network->building() && f.waiting) {
                *f.waiting = 1;
                note_backend(("not run yet: " + nwhy + " (frames pass through with no NR; the bridge is not used)").c_str(), false);
                return false;
            }
            return fallback("the network: " + nwhy);
        }
        note_network(g.network->name());   // "(building)" until a network is active, then the model
        nr::D3d12NetControls nc;
        nc.style = f.style;
        nc.intensity = f.intensity;
        nc.local_tone = f.local_tone;
        nc.local_structure = f.local_structure;
        nc.skin_structure = f.skin_structure;
        nc.automatic_mask = f.automatic_mask != 0;
        nc.round_u8 = df == DXGI_FORMAT_R8G8B8A8_UNORM || df == DXGI_FORMAT_B8G8R8A8_UNORM || df == DXGI_FORMAT_B8G8R8X8_UNORM;
        nc.reversible = f.reversible_mode >= 0 && f.reversible_mode <= 4 ? uint32_t(f.reversible_mode) : 0u;   // reversible_of
        nc.history_strength = f.history_strength;
        nc.noise_every_frame = f.noise_every_frame != 0;
        g.network->controls(nc);


        NetTemporal nt;
        nt.motion_va = g.lin_mv->GetGPUVirtualAddress();
        nt.motion_scale_x = have_motion ? f.motion_scale_x : 1.0f;
        nt.motion_scale_y = have_motion ? f.motion_scale_y : 1.0f;
        nt.depth_va = have_depth ? g.lin_depth->GetGPUVirtualAddress() : 0;
        nt.have_depth = have_depth;
        nt.depth_inverted = f.depth_inverted != 0;
        nt.reset = f.reset != 0;
        nt.feature = f.feature;
        nt.have_motion = have_motion;
        nt.motion_f16 = true;
        g.network->temporal(nt);
    }
    const auto state = nr::pe::split::snapshot(f.cmd);
    if (!state) return fallback("no snapshot of the command list's state");

    // The descriptors: the colour's SRV, the output's UAV. A segment whose previous frame has not finished is not
    // rewritten: the heap grows instead (D3D12 does not keep a descriptor's resource alive for its GPU use).
    g.refresh_executed();
    if (g.segment_owner.empty()) g.segment_owner.assign(g.segments, -1);
    UINT seg = g.slot / 4;
    if (g.segment_owner[seg] >= 0 && !frame_done(g.segment_owner[seg])) {
        const UINT grown = g.segments * 2;
        ID3D12DescriptorHeap* nh = nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = grown * 4;   // the SRV and UAV of each segment, as the first heap laid them out
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(f.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&nh))) || !nh)
            return fallback("no descriptor segment whose GPU use has finished");
        retire(g.heap);
        g.heap = nh;
        g.segments = grown;
        g.segment_owner.assign(g.segments, -1);
        g.slot = 0;
        seg = 0;
        char msg[160];
        std::snprintf(msg, sizeof msg, "descriptor heap grown to %u segments (completion protection)", grown);
        nr::pe::log("[asd] %s", msg);
    }
    g.segment_owner[seg] = int64_t(g.frames);
    const UINT slot = seg * 4;
    g.slot = (seg + 1) * 4 % (g.segments * 4);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(slot) * g.increment;
    gpu.ptr += UINT64(slot) * g.increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = sf;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    f.device->CreateShaderResourceView(f.color, &srv, cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = df;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav = cpu;
    cpu_uav.ptr += g.increment;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
    gpu_uav.ptr += g.increment;
    f.device->CreateUnorderedAccessView(f.output, nullptr, &uav, cpu_uav);
    // The motion guide's SRV (to_motion).
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_mv = cpu;
    cpu_mv.ptr += SIZE_T(2) * g.increment;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_mv = gpu;
    gpu_mv.ptr += UINT64(2) * g.increment;
    srv.Format = mf;
    f.device->CreateShaderResourceView(motion_res, &srv, cpu_mv);
    // The depth guide's SRV (to_depth).
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_depth = gpu;
    gpu_depth.ptr += UINT64(3) * g.increment;
    if (have_depth) {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_depth = cpu;
        cpu_depth.ptr += SIZE_T(3) * g.increment;
        srv.Format = depth_view;
        f.device->CreateShaderResourceView(f.depth, &srv, cpu_depth);
    }

    const bool in_place = f.color == f.output;
    const LinearFrame in{g.lin_in, g.lin_in->GetGPUVirtualAddress(), w, h, uint64_t(w) * h * 8};
    const LinearFrame out{g.lin_out, g.lin_out->GetGPUVirtualAddress(), w, h, in.bytes};
    UINT constants[8] = {w, h, 0u, 0u, 0u, 0u, 0u, 0u};
    UINT mv_constants[8] = {w, h, 0u, 0u, 0u, 0u, 0u, 0u};
    UINT depth_constants[8] = {w, h, 0u, 0u, 0u, 0u, 0u, 0u};
    if (have_motion) {
        // nr_runtime.cpp's params[8..11] for a guide sampled in place: the subrect's share of the allocation, its base.
        const float mtw = float(md.Width), mth = float(md.Height);
        const float mw = float(f.motion_w ? f.motion_w : uint32_t(md.Width)), mh = float(f.motion_h ? f.motion_h : md.Height);
        const float region[4] = {mw / mtw, mh / mth, float(f.motion_x) / mtw, float(f.motion_y) / mth};
        std::memcpy(mv_constants + 4, region, sizeof region);
    } else {
        const float region[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        std::memcpy(mv_constants + 4, region, sizeof region);
    }
    if (have_depth) {
        float region[4];
        if (f.depth_x >= depth_desc.Width || f.depth_y >= depth_desc.Height) {
            region[0] = float(depth_desc.Width);
            region[1] = float(depth_desc.Height);
            region[2] = 0.0f;
            region[3] = 0.0f;
        } else {
            const uint32_t max_w = uint32_t(depth_desc.Width) - f.depth_x;
            const uint32_t max_h = depth_desc.Height - f.depth_y;
            region[0] = float(f.depth_w ? std::min(f.depth_w, max_w) : max_w);
            region[1] = float(f.depth_h ? std::min(f.depth_h, max_h) : max_h);
            region[2] = float(f.depth_x);
            region[3] = float(f.depth_y);
        }
        std::memcpy(depth_constants + 4, region, sizeof region);
    }
    const UINT gx = (w + 7) / 8, gy = (h + 7) / 8;
    auto* cmd = f.cmd;
    D3D12_RESOURCE_BARRIER b[10];

    // copy-in: colour -> lin_in, motion -> lin_mv, depth -> lin_depth
    int n = 0;
    transition(b[n++], g.lin_in, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(b[n++], g.lin_out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(b[n++], g.lin_mv, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (have_depth)
        transition(b[n++], g.lin_depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (color_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        transition(b[n++], f.color, color_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(n, b);
    ID3D12DescriptorHeap* heaps[] = {g.heap};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(g.rs);
    cmd->SetPipelineState(g.to_linear);
    cmd->SetComputeRoot32BitConstants(0, 8, constants, 0);
    cmd->SetComputeRootDescriptorTable(1, gpu);
    cmd->SetComputeRootDescriptorTable(2, gpu_uav);
    cmd->SetComputeRootUnorderedAccessView(4, in.va);
    cmd->Dispatch(gx, gy, 1);
    cmd->SetPipelineState(g.to_motion);
    cmd->SetComputeRoot32BitConstants(0, 8, mv_constants, 0);
    cmd->SetComputeRootDescriptorTable(1, gpu_mv);
    cmd->SetComputeRootUnorderedAccessView(4, g.lin_mv->GetGPUVirtualAddress());
    cmd->Dispatch(gx, gy, 1);
    if (have_depth) {
        cmd->SetPipelineState(g.to_depth);
        cmd->SetComputeRoot32BitConstants(0, 8, depth_constants, 0);
        cmd->SetComputeRootDescriptorTable(1, gpu_depth);
        cmd->SetComputeRootUnorderedAccessView(4, g.lin_depth->GetGPUVirtualAddress());
        cmd->Dispatch(gx, gy, 1);
    }
    b[0] = D3D12_RESOURCE_BARRIER{};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b[0].UAV.pResource = nullptr;   // lin_in, lin_mv and lin_depth
    cmd->ResourceBarrier(1, b);


    // the network (both in UNORDERED_ACCESS, and left so)
    const char* nwhy = "";
    const bool net_ok = g.network->record(cmd, in, out, &nwhy);


    // The network binds its own root signature: the writeback's again.
    cmd->SetComputeRootSignature(g.rs);
    cmd->SetComputeRoot32BitConstants(0, 8, constants, 0);
    cmd->SetComputeRootDescriptorTable(1, gpu);
    cmd->SetComputeRootDescriptorTable(2, gpu_uav);

    ID3D12Resource* copy_back = g.lin_in;
    const D3D12_GPU_VIRTUAL_ADDRESS copy_back_va = copy_back->GetGPUVirtualAddress();

    // writeback: lin_out -> output (the input copied straight back when the network recorded nothing)
    n = 0;
    transition(b[n++], g.lin_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (in_place) transition(b[n++], f.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, output_state);
    else if (color_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        transition(b[n++], f.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, color_state);
    if (!net_ok) transition(b[n++], copy_back, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(n, b);
    cmd->SetPipelineState(g.from_linear);
    cmd->SetComputeRootShaderResourceView(3, net_ok ? out.va : copy_back_va);
    cmd->Dispatch(gx, gy, 1);
    n = 0;
    b[n] = D3D12_RESOURCE_BARRIER{};
    b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b[n++].UAV.pResource = f.output;
    cmd->ResourceBarrier(n, b);
    n = 0;
    transition(b[n++], copy_back, net_ok ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    transition(b[n++], g.lin_out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    transition(b[n++], g.lin_mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (have_depth)
        transition(b[n++], g.lin_depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    cmd->ResourceBarrier(n, b);
    nr::pe::split::restore(cmd, state);

    // The same recording evidence as the RenoDX route: a generation's completeness is every frame it recorded,
    // ordinary and RenoDX alike - one branch alone must not stand for the generation.
    g.booking = nr::pe::split::book(f.cmd);
    if (g.booking) g.recordings.push_back({int64_t(g.frames), f.feature, g.booking});
    mark_frame(f.cmd);
    char line[200];
    std::snprintf(line, sizeof line, "ran %ux%u%s: %s", w, h, in_place ? " in place" : "",
                  net_ok ? g.network->name() : (std::string("the network recorded nothing (") + nwhy + "): input copied back").c_str());
    note_backend_produced(net_ok);
    note_backend(line, true);
    return true;
}
void release_renodx_feature(uint64_t feature) {
    if (auto it = g.renodx.find(feature); it != g.renodx.end()) {
        if (it->second.stage) g.renodx_retired.push_back({int64_t(g.frames), g.uses_now(), "feature", std::move(it->second.stage)});
        g.renodx.erase(it);
    }
    if (g.network) {
        const uint64_t named = feature ? feature : kUnnamedStack;
        g.network->release_feature(feature);
        if (!feature) g.network->release_feature(named);
        for (uint32_t k = 1; k < renodx::kMaxUnlockedPasses; ++k)
            g.network->release_feature(stack_feature(named, k));
    }
}


HRESULT device_removed_reason() {
    ID3D12Device* d = nullptr;
    {
        std::lock_guard<std::recursive_mutex> l(g_owner_lock);
        d = g.device;
        if (d) d->AddRef();
    }
    if (!d) return S_OK;
    const HRESULT hr = d->GetDeviceRemovedReason();
    d->Release();
    return hr;
}
AsdNetStatus backend_net_status() {
    std::shared_ptr<D3d12AsdNetwork> n;
    {
        std::lock_guard<std::recursive_mutex> l(g_owner_lock);
        n = g.network;
    }
    AsdNetStatus s = n ? n->status() : AsdNetStatus{};
    s.stage_ms = g_stage_avg_ms.load();
    s.nr_w = g_last_nr_w.load();
    s.nr_h = g_last_nr_h.load();
    s.fast_passes = g_last_fast_passes.load();
    s.fast_fallback = g_last_fast_fallback.load();
    return s;
}


}  // namespace nr::pe::asd
