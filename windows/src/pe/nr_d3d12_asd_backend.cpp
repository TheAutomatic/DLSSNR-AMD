// The native D3D12 ASD backend's frame path (nr_d3d12_asd_backend.hpp): HLSL copy-in, the network, HLSL writeback, on
// the game's command list. Everything that can fail is checked before the first command is recorded, so a false
// return leaves the list as it came and the 0.0.8.1 path runs the frame.
#include "nr_d3d12_asd_backend.hpp"
#include "nr_d3d12_capture.hpp"
#include "nr_d3d12_asd.hpp"
#include "nr_d3d12_asd_net.hpp"
#include "nr_pe_d3d12split.hpp"
#include "nr_renodx_d3d12.hpp"   // the RenoDX stage on this route (M5 item 6)
#include "nr_pe_log.hpp"
#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nr::pe::asd {
namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// The network until M4's drops in: the output is the input.
class PlaceholderCopyNetwork final : public D3d12AsdNetwork {
public:
    const char* name() const override { return "PLACEHOLDER copy (input -> output; the M4 network is not in yet)"; }
    bool create(ID3D12Device*, const char**) override { return true; }
    bool record(ID3D12GraphicsCommandList* cmd, const LinearFrame& in, const LinearFrame& out, const char** why) override {
        if (in.bytes != out.bytes) { *why = "the linear buffers differ in size"; return false; }
        D3D12_RESOURCE_BARRIER b[2]{};
        for (auto& x : b) {
            x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            x.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        b[0].Transition.pResource = in.buffer;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[1].Transition.pResource = out.buffer;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cmd->ResourceBarrier(2, b);
        cmd->CopyBufferRegion(out.buffer, 0, in.buffer, 0, in.bytes);
        for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        cmd->ResourceBarrier(2, b);
        return true;
    }
};

using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                        UINT, UINT, ID3DBlob**, ID3DBlob**);
const char kShaders[] =
    "cbuffer C : register(b0) { uint g_w; uint g_h; uint g_tint; uint g_pad; float4 g_region; };\n"
    "Texture2D<float4> src_tex : register(t0);\n"
    "SamplerState linear_clamp : register(s0);\n"
    "RWTexture2D<float4> dst_tex : register(u0);\n"
    "StructuredBuffer<float4> lin_src : register(t1);\n"
    "RWStructuredBuffer<float4> lin_dst : register(u1);\n"
    "[numthreads(8, 8, 1)] void to_linear(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x < g_w && id.y < g_h) lin_dst[id.y * g_w + id.x] = src_tex.Load(int3(id.xy, 0));\n"
    "}\n"
    "[numthreads(8, 8, 1)] void from_linear(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= g_w || id.y >= g_h) return;\n"
    "    float4 v = lin_src[id.y * g_w + id.x];\n"
    "    if (g_tint) v.g *= 0.75;\n"   // DebugTint: a magenta cast, to see that this path wrote the frame
    "    dst_tex[id.xy] = v;\n"
    "}\n"
    // The engine's motion onto the frame: at each pixel's centre, through a linear clamp-to-edge sampler, where the
    // runtime's temporal kernels sample the guide in place (mv_direct: uv * region.xy + region.zw, the subrect's share
    // of the allocation and its base); the kernels then read this field at the same pixel centres (region 1, 0).
    "[numthreads(8, 8, 1)] void to_motion(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= g_w || id.y >= g_h) return;\n"
    "    const float2 uv = (float2(id.xy) + 0.5) / float2(g_w, g_h);\n"
    "    lin_dst[id.y * g_w + id.x] = float4(src_tex.SampleLevel(linear_clamp, uv * g_region.xy + g_region.zw, 0).xy, 0, 0);\n"
    "}\n"
    // The engine's depth into the temporal path's linear buffer: texel for texel what a NEAREST blit
    // of the subrect picks (runtime_depth.comp), stored as RGBA32F with depth in .r.
    "[numthreads(8, 8, 1)] void to_depth(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= g_w || id.y >= g_h) return;\n"
    "    const float2 s = floor((float2(id.xy) + 0.5) * g_region.xy / float2(g_w, g_h));\n"
    "    const int2 t = min(int2(s), int2(g_region.xy) - 1);\n"
    "    lin_dst[id.y * g_w + id.x] = float4(src_tex.Load(int3(t + int2(g_region.zw), 0)).r, 0, 0, 0);\n"
    "}\n";

ID3DBlob* compile(const char* entry, std::string* why) { return compile_cs(kShaders, sizeof kShaders - 1, entry, why); }
const char kPrepShaders[] = R"hlsl(
cbuffer PrepC : register(b0) {
    uint p_width;
    uint p_height;
    uint p_flags;
    uint p_curve;
    float p_bias;
    float p_contrast;
    float p_saturation;
    float p_k;
    float p_cap;
    float p_dt;
    uint p_rmode;
    uint p_kind;
};

RWStructuredBuffer<float4> lin_in_rw : register(u0);
RWStructuredBuffer<float4> lin_out_rw : register(u1);
RWStructuredBuffer<float4> lin_prep_p : register(u2);
RWStructuredBuffer<float4> prep_state : register(u3);

static const float3 kPrepLuma = float3(0.2126f, 0.7152f, 0.0722f);
static const float kPrepCap = 4.0f;
static const float kLog2Key = -2.4739f;
static const float kLow = 0.10f;
static const float kHigh = 0.90f;
static const float kMinGain = -4.0f;
static const float kMaxGain = 8.0f;
static const float kSpeedBrighter = 3.0f;
static const float kSpeedDarker = 1.0f;
static const float kAgxMin = -12.47393f;
static const float kAgxMax = 4.026069f;
static const float kHA = 0.15f, kHB = 0.50f, kHC = 0.10f, kHD = 0.20f, kHE = 0.02f, kHF = 0.30f;

bool prep_finite_val(float x) {
    return (asuint(x) & 0x7fffffffu) < 0x7f800000u;
}

bool prep_finite(float3 v) {
    return prep_finite_val(v.x) && prep_finite_val(v.y) && prep_finite_val(v.z);
}

float3 prep_enc(float3 v) {
    v = clamp(v, 0.0f, 1.0f);
    return lerp(v * 12.92f, 1.055f * pow(max(v, 1e-8f), 1.0f / 2.4f) - 0.055f, step(0.0031308f, v));
}

float3 prep_dec(float3 v) {
    v = clamp(v, 0.0f, 1.0f);
    return lerp(v / 12.92f, pow((v + 0.055f) / 1.055f, 2.4f), step(0.04045f, v));
}

float3 prep_knee_soft(float3 d) {
    float l = dot(d, kPrepLuma);
    if (l > 0.75f) d *= (0.75f + 0.25f * (1.0f - exp(-(l - 0.75f) / 0.25f))) / l;
    float peak = max(d.r, max(d.g, d.b));
    if (peak > 1.0f) d /= peak;
    return d;
}

float3 prep_knee_soft_inv(float3 d) {
    float l = dot(d, kPrepLuma);
    if (l <= 0.75f) return d;
    float lc = min(l, 0.75f + 0.25f * (1.0f - 1e-6f));
    return d * ((0.75f - 0.25f * log(1.0f - (lc - 0.75f) / 0.25f)) / l);
}

float3 prep_neutwo_encode(float3 v) {
    v = max(v, 0.0f);
    float m = max(v.r, max(v.g, v.b));
    return m <= 1e-6f ? v : v * rsqrt(m * m + 1.0f);
}

float3 prep_neutwo_decode(float3 y) {
    y = max(y, 0.0f);
    float m = min(max(y.r, max(y.g, y.b)), 0.999999f);
    return m <= 1e-6f ? y : y * rsqrt(max(1.0f - m * m, 1e-8f));
}

float prep_hybrid_f(float m) {
    if (m <= 0.75f) return m;
    float e = (m - 0.75f) / 0.25f;
    return 0.75f + 0.25f * (e * rsqrt(e * e + 1.0f));
}

float prep_hybrid_inv_f(float y) {
    if (y <= 0.75f) return y;
    float u = min((y - 0.75f) / 0.25f, 0.999999f);
    return 0.75f + 0.25f * (u * rsqrt(max(1.0f - u * u, 1e-8f)));
}

float3 prep_hybrid_encode(float3 v) {
    v = max(v, 0.0f);
    float m = max(v.r, max(v.g, v.b));
    return m <= 1e-6f ? v : v * (prep_hybrid_f(m) / m);
}

float3 prep_hybrid_decode(float3 y) {
    y = max(y, 0.0f);
    float m = max(y.r, max(y.g, y.b));
    return m <= 1e-6f ? y : y * (prep_hybrid_inv_f(m) / m);
}

float3 prep_knee(float3 d, uint rmode) {
    if (rmode != 0u) return clamp(rmode >= 3u ? prep_hybrid_encode(d) : prep_neutwo_encode(d), 0.0f, 1.0f);
    return prep_knee_soft(d);
}

float3 prep_knee_inv(float3 d, uint rmode) {
    if (rmode != 0u) return rmode >= 3u ? prep_hybrid_decode(d) : prep_neutwo_decode(d);
    return prep_knee_soft_inv(d);
}

float3 prep_neutral(float3 c) {
    const float start = 0.76f, d = 0.24f, desat = 0.15f;
    const float peak = max(c.r, max(c.g, c.b));
    if (peak < start) return c;
    const float np = 1.0f - d * d / (peak + d - start);
    c *= np / peak;
    const float g = 1.0f - 1.0f / (desat * (peak - np) + 1.0f);
    return lerp(c, float3(np, np, np), g);
}

float3 prep_neutral_inv(float3 y) {
    const float start = 0.76f, d = 0.24f, desat = 0.15f;
    const float np = max(y.r, max(y.g, y.b));
    if (np < start) return y;
    const float peak = d * d / max(1.0f - np, 1e-6f) - d + start;
    const float g = 1.0f - 1.0f / (desat * (peak - np) + 1.0f);
    return (y - g * np) / (1.0f - g) * (peak / np);
}

float3 prep_reinhard(float3 x) { return x / (1.0f + x); }
float3 prep_reinhard_inv(float3 y) { y = min(y, 0.9999f); return y / (1.0f - y); }

float prep_hable1(float x) {
    return ((x * (kHA * x + kHC * kHB) + kHD * kHE) / (x * (kHA * x + kHB) + kHD * kHF)) - kHE / kHF;
}

float3 prep_hable(float3 x) {
    const float w = prep_hable1(11.2f);
    return float3(prep_hable1(x.r), prep_hable1(x.g), prep_hable1(x.b)) / w;
}

float prep_hable_inv1(float y) {
    const float t = min(y * prep_hable1(11.2f) + kHE / kHF, 0.999f);
    const float qa = kHA * (1.0f - t), qb = kHC * kHB - kHB * t, qc = kHD * kHE - kHD * kHF * t;
    return max((-qb + sqrt(max(qb * qb - 4.0f * qa * qc, 0.0f))) / (2.0f * qa), 0.0f);
}

float3 prep_hable_inv(float3 y) {
    return float3(prep_hable_inv1(y.r), prep_hable_inv1(y.g), prep_hable_inv1(y.b));
}

float prep_gt1(float x) {
    const float P = 1.0f, a = 1.0f, m = 0.22f, l = 0.4f, c = 1.33f, b = 0.0f;
    const float l0 = ((P - m) * l) / a, S0 = m + l0, S1 = m + a * l0;
    const float C2 = (a * P) / (P - S1), CP = -C2 / P;
    const float w0 = 1.0f - smoothstep(0.0f, m, x);
    const float w2 = step(m + l0, x);
    const float w1 = 1.0f - w0 - w2;
    const float T = m * pow(max(x, 0.0f) / m, c) + b;
    const float S = P - (P - S1) * exp(CP * (x - S0));
    const float L = m + a * (x - m);
    return T * w0 + L * w1 + S * w2;
}

float3 prep_gt(float3 x) {
    return float3(prep_gt1(x.r), prep_gt1(x.g), prep_gt1(x.b));
}

float prep_gt_inv1(float y) {
    const float P = 1.0f, a = 1.0f, m = 0.22f, l = 0.4f, c = 1.33f;
    const float l0 = ((P - m) * l) / a, S0 = m + l0, S1 = m + a * l0;
    const float C2 = (a * P) / (P - S1);
    if (y >= prep_gt1(64.0f)) return 64.0f;
    if (y >= S0) return S0 - log(max((P - y) / (P - S1), 1e-30f)) / C2;
    if (y >= m) return y;
    float t = pow(max(y, 0.0f) / m, 1.0f / c);
    [unroll] for (int i = 0; i < 4; ++i) {
        const float s = t * t * (3.0f - 2.0f * t), ds = 6.0f * t * (1.0f - t), tc = pow(t, c);
        const float g = tc * (1.0f - s) + t * s - y / m;
        const float dg = c * pow(max(t, 1e-20f), c - 1.0f) * (1.0f - s) - tc * ds + s + t * ds;
        t = clamp(t - g / max(dg, 1e-6f), 0.0f, 1.0f);
    }
    return m * t;
}

float3 prep_gt_inv(float3 y) {
    return float3(prep_gt_inv1(y.r), prep_gt_inv1(y.g), prep_gt_inv1(y.b));
}

float3 prep_aces(float3 x) {
    return (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
}

float3 prep_aces_inv(float3 y) {
    y = min(y, 0.9999f);
    const float3 qa = 2.51f - 2.43f * y, qb = 0.03f - 0.59f * y, qc = -0.14f * y;
    return max((-qb + sqrt(max(qb * qb - 4.0f * qa * qc, 0.0f))) / (2.0f * qa), 0.0f);
}

float3 prep_agx_in(float3 c) {
    return c.x * float3(0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f) +
           c.y * float3(0.0784335999999992f, 0.878468636469772f, 0.0784336f) +
           c.z * float3(0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f);
}

float3 prep_agx_out(float3 c) {
    return c.x * float3(1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f) +
           c.y * float3(-0.0980208811401368f, 1.15190312990417f, -0.0980434501171241f) +
           c.z * float3(-0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f);
}

float prep_agx_sig(float x) {
    const float x2 = x * x, x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
}

float prep_agx_dsig(float x) {
    const float x2 = x * x, x4 = x2 * x2;
    return 93.0f * x4 * x - 200.7f * x4 + 127.84f * x2 * x - 20.604f * x2 + 0.8596f * x + 0.1191f;
}

float prep_agx_sig_inv(float y) {
    y = clamp(y, prep_agx_sig(0.0f), prep_agx_sig(1.0f));
    float lo = 0.0f, hi = 1.0f;
    [unroll] for (int i = 0; i < 8; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (prep_agx_sig(mid) < y) lo = mid; else hi = mid;
    }
    float x = 0.5f * (lo + hi);
    [unroll] for (int j = 0; j < 3; ++j) x = clamp(x - (prep_agx_sig(x) - y) / prep_agx_dsig(x), lo, hi);
    return x;
}

float3 prep_agx(float3 c) {
    float3 v = prep_agx_in(c);
    v = clamp(log2(max(v, 1e-10f)), kAgxMin, kAgxMax);
    v = (v - kAgxMin) / (kAgxMax - kAgxMin);
    v = float3(prep_agx_sig(v.r), prep_agx_sig(v.g), prep_agx_sig(v.b));
    return pow(max(prep_agx_out(v), 0.0f), 2.2f);
}

float3 prep_agx_inv(float3 y) {
    float3 v = prep_agx_in(pow(max(y, 0.0f), 1.0f / 2.2f));
    v = float3(prep_agx_sig_inv(v.r), prep_agx_sig_inv(v.g), prep_agx_sig_inv(v.b));
    v = exp2(v * (kAgxMax - kAgxMin) + kAgxMin);
    return prep_agx_out(v);
}

bool prep_unknee(uint flags) { return (flags & 2u) != 0u; }

float3 prep_curve(float3 x, uint curve_id, float k, uint flags, uint rmode) {
    switch (curve_id) {
    case 1u: return prep_neutral(x * k);
    case 2u: return prep_reinhard(x * k);
    case 3u: return prep_hable(x * k);
    case 4u: return prep_gt(x * k);
    case 5u: return prep_aces(x * k);
    case 6u: return prep_agx(x * k);
    default: return prep_unknee(flags) ? prep_knee(x, rmode) : x;
    }
}

float3 prep_curve_inv(float3 y, uint curve_id, float k, uint flags, uint rmode) {
    switch (curve_id) {
    case 1u: return prep_neutral_inv(y) / k;
    case 2u: return prep_reinhard_inv(y) / k;
    case 3u: return prep_hable_inv(y) / k;
    case 4u: return prep_gt_inv(y) / k;
    case 5u: return prep_aces_inv(y) / k;
    case 6u: return prep_agx_inv(y) / k;
    default: return prep_unknee(flags) ? prep_knee_inv(y, rmode) : y;
    }
}

float3 prep_light(float3 P, uint flags, uint rmode, uint kind) {
    if (kind == 2u) return P;
    return prep_unknee(flags) ? prep_knee_inv(prep_dec(P), rmode) : prep_dec(P);
}

float3 prep_reencode(float3 v, uint flags, uint rmode, uint kind) {
    if (kind == 2u) return v;
    return prep_unknee(flags) ? prep_enc(prep_knee(v, rmode)) : prep_enc(v);
}

float3 prep_forward_math(float3 x, float gain, float contrast, float saturation, uint curve_id, float k, uint flags, uint rmode) {
    x *= gain;
    if (contrast != 1.0f) x = 0.18f * pow(max(x, 0.0f) / 0.18f, contrast);
    if (saturation != 1.0f) {
        float l = dot(x, kPrepLuma);
        x = max(l + saturation * (x - l), 0.0f);
    }
    return prep_curve(x, curve_id, k, flags, rmode);
}

float3 prep_backward_math(float3 y, float gain, float contrast, float saturation, uint curve_id, float k, uint flags, uint rmode) {
    float3 x = prep_curve_inv(y, curve_id, k, flags, rmode);
    if (saturation != 1.0f) {
        float l = dot(x, kPrepLuma);
        x = l + (x - l) / saturation;
    }
    if (contrast != 1.0f) x = 0.18f * pow(max(x, 0.0f) / 0.18f, 1.0f / contrast);
    return x / max(gain, 1e-6f);
}

groupshared uint s_hist[128];

[numthreads(256, 1, 1)]
void prep_meter(uint t : SV_GroupIndex) {
    if (t < 128u) s_hist[t] = 0u;
    GroupMemoryBarrierWithGroupSync();

    const uint sx = max(1u, p_width / 160u);
    const uint sy = max(1u, p_height / 90u);
    const uint nx = (p_width + sx - 1u) / sx;
    const uint ny = (p_height + sy - 1u) / sy;
    const uint total = nx * ny;

    for (uint i = t; i < total; i += 256u) {
        const uint2 xy = uint2((i % nx) * sx, (i / nx) * sy);
        if (xy.x < p_width && xy.y < p_height) {
            const float3 P = lin_in_rw[xy.y * p_width + xy.x].rgb;
            if (prep_finite(P)) {
                const float l = max(dot(prep_light(P, p_flags, p_rmode, p_kind), kPrepLuma), 1.5e-5f);
                const uint bin = uint(clamp((log2(l) + 16.0f) * 6.4f, 0.0f, 127.0f));
                InterlockedAdd(s_hist[bin], 1u);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (t != 0u) return;

    uint n = 0u;
    for (uint b = 0u; b < 128u; ++b) n += s_hist[b];

    const float4 cur_st = prep_state[0];
    const bool valid = (cur_st.y == 1.0f) && ((p_flags & 8u) == 0u);
    float s = valid ? cur_st.x : 0.0f;
    float target = s;

    if (n >= 64u) {
        const float lo = kLow * float(n);
        const float hi = kHigh * float(n);
        float below = 0.0f, sum = 0.0f, weight = 0.0f;
        for (uint b = 0u; b < 128u; ++b) {
            const float c = float(s_hist[b]);
            const float from_rank = max(below, lo);
            const float to_rank = min(below + c, hi);
            if (to_rank > from_rank) {
                sum += (to_rank - from_rank) * ((float(b) + 0.5f) / 6.4f - 16.0f);
                weight += to_rank - from_rank;
            }
            below += c;
        }
        target = clamp(kLog2Key - sum / max(weight, 1.0f), kMinGain, kMaxGain);
        const float speed = target < s ? kSpeedBrighter : kSpeedDarker;
        s = valid ? s + (target - s) * (1.0f - exp(-clamp(p_dt, 0.0f, 0.25f) * speed)) : target;
    }
    s = clamp(s, kMinGain, kMaxGain);
    prep_state[0] = float4(s, 1.0f, exp2(s + p_bias), target);
}

[numthreads(8, 8, 1)]
void prep_forward(uint3 id : SV_DispatchThreadID) {
    if (id.x >= p_width || id.y >= p_height) return;
    const uint idx = id.y * p_width + id.x;
    const float4 P = lin_in_rw[idx];
    lin_prep_p[idx] = P;
    if (!prep_finite(P.rgb)) {
        lin_in_rw[idx] = float4(0.0f, 0.0f, 0.0f, P.a);
        return;
    }
    const float gain = (p_flags & 1u) != 0u ? prep_state[0].z : exp2(p_bias);
    const float3 p = prep_light(P.rgb, p_flags, p_rmode, p_kind);
    const float3 fwd = prep_forward_math(p, gain, p_contrast, p_saturation, p_curve, p_k, p_flags, p_rmode);
    lin_in_rw[idx] = float4(prep_enc(fwd), P.a);
}

[numthreads(8, 8, 1)]
void prep_back(uint3 id : SV_DispatchThreadID) {
    if (id.x >= p_width || id.y >= p_height) return;
    const uint idx = id.y * p_width + id.x;
    const float4 nin = lin_in_rw[idx];
    const float4 nout = lin_out_rw[idx];
    const float4 P = lin_prep_p[idx];
    float3 M = P.rgb;
    if (any(nout.rgb != nin.rgb) && prep_finite(nout.rgb) && prep_finite(P.rgb)) {
        const float3 din = prep_dec(nin.rgb);
        const float3 dout = prep_dec(nout.rgb);
        const float3 p = prep_light(P.rgb, p_flags, p_rmode, p_kind);
        const float gain = (p_flags & 1u) != 0u ? prep_state[0].z : exp2(p_bias);
        float3 d;
        if ((p_flags & 16u) != 0u) {
            d = p * (clamp(max(dout, 1.0f / 1024.0f) / max(din, 1.0f / 1024.0f), 0.25f, 4.0f) - 1.0f);
        } else {
            d = prep_backward_math(dout, gain, p_contrast, p_saturation, p_curve, p_k, p_flags, p_rmode) -
                prep_backward_math(din, gain, p_contrast, p_saturation, p_curve, p_k, p_flags, p_rmode);
            const float3 lim = p_cap * abs(dout - din) / max(gain, 1e-6f);
            d = clamp(d, -lim, lim);
        }
        M = max(P.rgb + prep_reencode(max(p + d, 0.0f), p_flags, p_rmode, p_kind) - prep_reencode(p, p_flags, p_rmode, p_kind), 0.0f);
        if (!prep_finite(M)) M = P.rgb;
    }
    lin_out_rw[idx] = float4(M, nout.a);
}
)hlsl";

ID3DBlob* compile_prep(const char* entry, std::string* why) { return compile_cs(kPrepShaders, sizeof kPrepShaders - 1, entry, why); }

constexpr float kPreprocessAnchor[7] = {1.0f, 1.0f, 1.2195122f, 2.9275228f, 1.0052344f, 0.7231708f, 0.8083602f};

struct PrepConstants {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t flags = 0;
    uint32_t curve = 0;
    float bias = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float k = 1.0f;
    float cap = 4.0f;
    float dt = 0.0f;
    uint32_t rmode = 0;
    uint32_t kind = 0;
    uint32_t pad[4] = {};
};

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
    ID3D12RootSignature* rs_prep = nullptr;
    ID3D12PipelineState *prep_meter = nullptr, *prep_forward = nullptr, *prep_back = nullptr;
    ID3D12Resource *lin_prep_p = nullptr, *prep_state = nullptr;
    bool prep_was_on = false;
    uint32_t prep_last_curve = 0;
    int prep_last_exposure = 0;
    float prep_last_bias = 0.0f, prep_last_contrast = 1.0f, prep_last_saturation = 1.0f;
    int prep_last_inverse = 1;
    uint32_t prep_last_rmode = 0;
    uint32_t prep_last_kind = 0;
    std::chrono::steady_clock::time_point prep_metered{};
    uint32_t w = 0, h = 0;
    std::unique_ptr<D3d12AsdNetwork> network;
    bool broken = false;   // something of the device's could not be made: the 0.0.8.1 path from now on
    // Set while a device generation is being dropped: a stage's destructor calls retire() for the surfaces it still
    // holds, and those must be released here, not queued into the next generation's list (their watermark is gone).
    bool discarding = false;
    std::string broken_why;
    uint64_t tick = 0;
    // The backend's own completion signal: a frame number copied into a readback buffer by the same command list that
    // used the objects, so a retired stage or buffer is released only after the GPU ran past it. An evaluate count is
    // not evidence: a stack records many frames into one list that may still be unsubmitted.
    static constexpr uint32_t kDoneSlots = 128;
    ID3D12Resource* token = nullptr;     // upload: the frame numbers to echo
    uint64_t* token_map = nullptr;
    uint32_t token_slots = 0;            // growable: a slot is rewritten only after its holder executed
    ID3D12Resource* done = nullptr;      // readback: what the GPU echoed
    uint64_t* done_map = nullptr;
    uint64_t frames = 0;                 // frames recorded by this backend
    int64_t executed = -1;               // newest frame the GPU finished
    std::vector<std::pair<int64_t, ID3D12Resource*>> token_retired;   // grown-away token buffers
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
    std::vector<std::pair<int64_t, IUnknown*>> retired;   // frame that last used it
    // The RenoDX route's stages, one a feature (nr_pe_session.cpp's RenoDXEntry: its working resolution's debounce
    // and its stage, keyed by everything a change of takes a new one).
    struct RenodxEntry {
        std::unique_ptr<renodx12::Stage> stage;
        uint32_t w = 0, h = 0, nw = 0, nh = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        int hdr = 0, present = 0;
        float scale = 1.0f;
        bool follow = false;
        uint32_t applied_w = 0, applied_h = 0, pending_w = 0, pending_h = 0, pending_frames = 0;
    };
    std::map<uint64_t, RenodxEntry> renodx;
    // A replaced stage (an extent change) is released once the frames in flight are past it, like `retired`.
    std::vector<std::pair<int64_t, std::unique_ptr<renodx12::Stage>>> renodx_retired;
};
Backend g;

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
    g.retired.emplace_back(int64_t(g.frames), p);
}
void sweep() {
    ++g.tick;
    g.refresh_executed();
    auto it = std::remove_if(g.retired.begin(), g.retired.end(), [](const auto& r) {
        if (g.executed < r.first) return false;
        r.second->Release();
        return true;
    });
    g.retired.erase(it, g.retired.end());
    auto rs = std::remove_if(g.renodx_retired.begin(), g.renodx_retired.end(), [](const auto& r) {
        return g.executed >= r.first;
    });
    g.renodx_retired.erase(rs, g.renodx_retired.end());
    auto rt = std::remove_if(g.token_retired.begin(), g.token_retired.end(), [](const auto& r) {
        if (g.executed < r.first) return false;
        r.second->Release();
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
        g.token_retired.emplace_back(int64_t(g.frames), g.token);   // the last record that reads the old block
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

ID3D12Resource* linear_buffer(ID3D12Device* device, uint64_t bytes);   // (defined below drop_device)


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
    // The ACO network when the .asi has set its planner (nr_asi_main.cpp); a test without one keeps the placeholder.
    if (net_planner()) g.network = make_aco_network(folder());
    else g.network = std::make_unique<PlaceholderCopyNetwork>();
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
    }
    note_network(g.network->name());
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
    D3D12_ROOT_PARAMETER rp[5]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.RegisterSpace = 0;
    rp[0].Constants.Num32BitValues = 16;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].Descriptor.ShaderRegister = 0;
    rp[1].Descriptor.RegisterSpace = 0;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[2].Descriptor.ShaderRegister = 1;
    rp[2].Descriptor.RegisterSpace = 0;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[3].Descriptor.ShaderRegister = 2;
    rp[3].Descriptor.RegisterSpace = 0;
    rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[4].Descriptor.ShaderRegister = 3;
    rp[4].Descriptor.RegisterSpace = 0;
    for (auto& x : rp) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd_prep{};
    rd_prep.NumParameters = 5;
    rd_prep.pParameters = rp;
    blob = nullptr; errors = nullptr;
    hr = D3D12SerializeRootSignature(&rd_prep, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    release(errors);
    if (FAILED(hr) || !blob) { *why = "D3D12SerializeRootSignature(prep) failed"; return false; }
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rs_prep));
    release(blob);
    if (FAILED(hr)) { *why = "CreateRootSignature(prep) failed"; return false; }
    for (auto [entry, pso] : {std::pair{"prep_meter", &g.prep_meter},
                              std::pair{"prep_forward", &g.prep_forward},
                              std::pair{"prep_back", &g.prep_back}}) {
        ID3DBlob* code = compile_prep(entry, why);
        if (!code) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g.rs_prep;
        pd.CS.pShaderBytecode = code->GetBufferPointer();
        pd.CS.BytecodeLength = code->GetBufferSize();
        hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(pso));
        release(code);
        if (FAILED(hr)) { *why = std::string("CreateComputePipelineState(") + entry + ") failed"; return false; }
    }
    g.prep_state = linear_buffer(device, 64);
    if (!g.prep_state) { *why = "linear_buffer(prep_state) failed"; return false; }
    return true;
}

void drop_device() {
    g.discarding = true;   // the destructors below release their own surfaces here, not into the next generation
    g.renodx.clear();
    g.renodx_retired.clear();
    retire(g.rs); retire(g.to_linear); retire(g.from_linear); retire(g.to_motion); retire(g.to_depth); retire(g.heap);
    retire(g.lin_in); retire(g.lin_out); retire(g.lin_mv); retire(g.lin_depth);
    retire(g.neutral_mv);
    retire(g.rs_prep); retire(g.prep_meter); retire(g.prep_forward); retire(g.prep_back);
    retire(g.lin_prep_p); retire(g.prep_state);
    if (g.token_map) g.token->Unmap(0, nullptr);
    if (g.done_map) g.done->Unmap(0, nullptr);
    release(g.token); release(g.done);
    for (auto& r : g.token_retired) r.second->Release();
    // The lists belong to this device: nothing of it is in flight once drop_device is called (the same reason the
    // resources above are released here), and a later device must not inherit watermarks from this one.
    for (auto& r : g.retired) r.second->Release();
    g.retired.clear();
    g.token_retired.clear();
    g.token_map = nullptr; g.done_map = nullptr; g.token_slots = 0; g.frames = 0; g.executed = -1;
    g.segments = Backend::kSlots / 4;
    g.segment_owner.clear();
    g.slot = 0;
    g.rs = nullptr; g.to_linear = g.from_linear = g.to_motion = g.to_depth = nullptr; g.heap = nullptr;
    g.lin_in = g.lin_out = g.lin_mv = g.lin_depth = nullptr;
    g.neutral_mv = nullptr;
    g.rs_prep = nullptr; g.prep_meter = g.prep_forward = g.prep_back = nullptr;
    g.lin_prep_p = g.prep_state = nullptr;
    g.discarding = false;
    g.prep_was_on = false;
    g.prep_last_curve = 0;
    g.prep_last_exposure = 0;
    g.prep_last_bias = 0.0f;
    g.prep_last_contrast = 1.0f;
    g.prep_last_saturation = 1.0f;
    g.prep_last_inverse = 1;
    g.prep_last_rmode = 0;
    g.prep_last_kind = 0;
    g.w = g.h = 0;
    g.network.reset();
    g.renodx.clear();   // its stages own nothing of the device's that outlives it
    if (g.device) g.device->Release();
    g.device = nullptr;
    g.broken = false;
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
    note_backend(("not run (the 0.0.8.1 path): " + why).c_str(), false);
    return false;
}


std::wstring widen(const char* u8) {
    std::wstring out;
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, nullptr, 0);
    if (n > 0) { out.resize(size_t(n) - 1); MultiByteToWideChar(CP_UTF8, 0, u8, -1, out.data(), n); }
    return out;
}

// The RenoDX route's frame (nr_pe_session.cpp's run_renodx, on the ASD route): the stage around the network, one a
// feature, the network through this backend's own (g.network) over its linear buffers. HDR still takes the 0.0.8.1
// path (the v6 codec is item 6b).
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
        g.renodx_retired.emplace_back(int64_t(g.frames), std::move(e.stage));   // released once the GPU ran past this frame
        auto made = renodx12::Stage::make(c, &why);
        if (made) made->set_retire([](ID3D12Resource* r) { retire(r); });   // released on the backend's watermark
        if (made) {
            made->set_completed([](uint64_t f) { g.refresh_executed(); return g.executed >= int64_t(f); });
        }
        if (!made) return fallback("the RenoDX route: " + why);
        e.stage = std::move(made);
        e.w = w; e.h = h; e.nw = nw; e.nh = nh; e.format = sf;
        e.hdr = f.renodx_hdr_flag; e.present = f.renodx_present;
        nr::pe::log("[nr] the RenoDX route (D3D12): feature #%u at %ux%u (network %ux%u)", unsigned(f.feature), w, h, nw, nh);
    }
    if (!e.stage->set(s)) {
        // Relative <-> absolute units (NRLinearUnitNits, NRSourceEncoding): a stage made with the new settings
        // takes the frame now (0.0.8.1 retires the old one and waits a frame; the bridge is never used here).
        g.renodx_retired.emplace_back(int64_t(g.frames), std::move(e.stage));
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
        if (e.stage) e.stage->set_retire([](ID3D12Resource* r) { retire(r); });
        if (e.stage) e.stage->set_completed([](uint64_t f) { g.refresh_executed(); return g.executed >= int64_t(f); });
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
    rf.capture_in = f.capture_in;      // the boundary capture is the test's; the stage only copies when set
    rf.capture_out = f.capture_out;
    rf.capture_raw = f.capture_raw;
    rf.capture_in_raw = f.capture_in_raw;
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

    const uint64_t bytes = uint64_t(nw) * nh * 16;
    const auto net = [&](uint32_t, uint64_t, const nr::D3d12NetControls& controls, const NetTemporal& nt,
                         ID3D12Resource* in, ID3D12Resource* out) {
        g.network->controls(controls);
        g.network->temporal(nt);
        const char* nwhy = "";
        const LinearFrame lin{in, in->GetGPUVirtualAddress(), nw, nh, bytes};
        const LinearFrame lout{out, out->GetGPUVirtualAddress(), nw, nh, bytes};
        return g.network->record(f.cmd, lin, lout, &nwhy);
    };
    const auto state = nr::pe::split::snapshot(f.cmd);
    if (!state) return fallback("the RenoDX route: no snapshot of the command list's state");
    e.stage->set_frame(g.frames);
    const uint32_t ran = e.stage->record(f.cmd, rf, net);
    nr::pe::split::restore(f.cmd, state);
    // 0 passes is a legitimate copy-through only when the stage says it made everything it needed; otherwise the
    // output may not have been written at all and the caller must take its own path.
    if (!e.stage->record_ok())
        return fallback("the RenoDX stage could not make a surface this frame needs");
    char line[160];
    std::snprintf(line, sizeof line, "ran the RenoDX stage (D3D12) %ux%u, network %ux%u: %u pass%s", w, h, nw, nh,
                  ran, ran == 1 ? "" : "es");
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
    // v2.7: a list state not known yet (the hooks came after its Reset), a recording blocked this moment (render pass,
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
        g.device = device;
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
    // Stages of the 0.0.8.1 path this backend does not run (that path takes the frame whole) - except the RenoDX
    // route, whose stage runs here on the ASD route (M5 item 6).
    const auto color_state = static_cast<D3D12_RESOURCE_STATES>(f.color_state);
    const auto output_state = static_cast<D3D12_RESOURCE_STATES>(f.output_state);
    const bool renodx = f.renodx_route != 0;

    sweep();
    std::string why;
    // v2.7: as wilsjo2_ready - passing states wait (the frame as it came, no NR, no bridge); lasting ones take the bridge
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
        g.device = f.device;
        capture::adopt(f.device);
        if (!build(f.device, &why)) { g.broken = true; g.broken_why = why; }
    }
    // Whatever the debug layer said since the last frame: drained here so the first error is in the log before a
    // game that dies on the GPU takes the rest of the run with it.
    capture::drain();
    if (g.broken) return fallback(g.broken_why);

    if (renodx) {
        const bool ran = renodx_frame(f, color_state, output_state);
        if (ran) mark_frame(f.cmd);
        return ran;
    }

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
        retire(g.lin_in); retire(g.lin_out); retire(g.lin_mv); retire(g.lin_depth); retire(g.lin_prep_p);
        const uint64_t bytes = uint64_t(w) * h * 16;
        g.lin_in = linear_buffer(f.device, bytes);
        g.lin_out = linear_buffer(f.device, bytes);
        g.lin_mv = linear_buffer(f.device, bytes);
        g.lin_depth = linear_buffer(f.device, bytes);
        g.lin_prep_p = linear_buffer(f.device, bytes);
        if (!g.lin_in || !g.lin_out || !g.lin_mv || !g.lin_depth || !g.lin_prep_p) {
            retire(g.lin_in); retire(g.lin_out); retire(g.lin_mv); retire(g.lin_depth); retire(g.lin_prep_p);
            g.lin_in = g.lin_out = g.lin_mv = g.lin_depth = g.lin_prep_p = nullptr;
            g.w = g.h = 0;
            return fallback("the linear buffers could not be made");
        }
        g.w = w; g.h = h;
    }
    const bool prep_on = (f.preprocess != 0);
    PrepConstants pc{};
    bool prep_reset = false;
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
            // v2.6 (Kenny): while ASD's network builds, no bridge - the model passes the frame through with no NR
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

        const uint32_t curve = uint32_t(std::clamp(f.prep_curve, 0, 6));
        const bool is_float = (sf == DXGI_FORMAT_R16G16B16A16_FLOAT || sf == DXGI_FORMAT_R11G11B10_FLOAT ||
                               sf == DXGI_FORMAT_R32G32B32A32_FLOAT);
        const uint32_t kind = is_float ? 0u : 1u;
        const uint32_t rmode = f.reversible_mode >= 0 && f.reversible_mode <= 4 ? uint32_t(f.reversible_mode) : 0u;

        float prep_dt = 0.0f;
        if (prep_on) {
            const bool prep_changed = (!g.prep_was_on ||
                                       curve != g.prep_last_curve ||
                                       f.prep_exposure != g.prep_last_exposure ||
                                       f.prep_bias_ev != g.prep_last_bias ||
                                       f.prep_contrast != g.prep_last_contrast ||
                                       f.prep_saturation != g.prep_last_saturation ||
                                       f.prep_inverse != g.prep_last_inverse ||
                                       rmode != g.prep_last_rmode ||
                                       kind != g.prep_last_kind);
            prep_reset = prep_changed;
            const auto now = std::chrono::steady_clock::now();
            if (g.prep_was_on && !prep_reset && f.prep_exposure == 1) {
                prep_dt = std::chrono::duration<float>(now - g.prep_metered).count();
            }
            g.prep_metered = now;
            g.prep_was_on = true;
            g.prep_last_curve = curve;
            g.prep_last_exposure = f.prep_exposure;
            g.prep_last_bias = f.prep_bias_ev;
            g.prep_last_contrast = f.prep_contrast;
            g.prep_last_saturation = f.prep_saturation;
            g.prep_last_inverse = f.prep_inverse;
            g.prep_last_rmode = rmode;
            g.prep_last_kind = kind;

            const uint32_t flags = (f.prep_exposure == 1 ? 1u : 0u) |
                                   (kind == 0u ? 2u : 0u) |
                                   (prep_reset ? 8u : 0u) |
                                   (f.prep_inverse ? 0u : 16u);
            pc.width = w;
            pc.height = h;
            pc.flags = flags;
            pc.curve = curve;
            pc.bias = f.prep_exposure == 0 ? 0.0f : std::clamp(f.prep_bias_ev, -8.0f, 8.0f);
            pc.contrast = std::clamp(f.prep_contrast, 0.5f, 2.0f);
            pc.saturation = std::clamp(f.prep_saturation, 0.05f, 2.0f);
            pc.k = kPreprocessAnchor[curve];
            pc.cap = 4.0f;
            pc.dt = prep_dt;
            pc.rmode = rmode;
            pc.kind = kind;
        } else {
            if (g.prep_was_on) {
                prep_reset = true;
                g.prep_was_on = false;
            }
        }

        NetTemporal nt;
        nt.motion_va = g.lin_mv->GetGPUVirtualAddress();
        nt.motion_scale_x = have_motion ? f.motion_scale_x : 1.0f;
        nt.motion_scale_y = have_motion ? f.motion_scale_y : 1.0f;
        nt.depth_va = have_depth ? g.lin_depth->GetGPUVirtualAddress() : 0;
        nt.have_depth = have_depth;
        nt.depth_inverted = f.depth_inverted != 0;
        nt.reset = (f.reset != 0) || prep_reset;
        nt.feature = f.feature;
        nt.have_motion = have_motion;
        g.network->temporal(nt);
    }
    const auto state = nr::pe::split::snapshot(f.cmd);
    if (!state) return fallback("no snapshot of the command list's state");

    // The descriptors: the colour's SRV, the output's UAV. A segment whose previous frame has not finished is not
    // rewritten: the heap grows instead (D3D12 does not keep a descriptor's resource alive for its GPU use).
    g.refresh_executed();
    if (g.segment_owner.empty()) g.segment_owner.assign(g.segments, -1);
    UINT seg = g.slot / 4;
    if (g.segment_owner[seg] >= 0 && g.segment_owner[seg] > g.executed) {
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
    const LinearFrame in{g.lin_in, g.lin_in->GetGPUVirtualAddress(), w, h, uint64_t(w) * h * 16};
    const LinearFrame out{g.lin_out, g.lin_out->GetGPUVirtualAddress(), w, h, in.bytes};
    UINT constants[8] = {w, h, debug_tint() ? 1u : 0u, 0u, 0u, 0u, 0u, 0u};
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
    if (prep_on) {
        transition(b[n++], g.lin_prep_p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(b[n++], g.prep_state, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
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

    if (prep_on) {
        cmd->SetComputeRootSignature(g.rs_prep);
        cmd->SetComputeRoot32BitConstants(0, 16, &pc, 0);
        cmd->SetComputeRootUnorderedAccessView(1, g.lin_in->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(2, g.lin_out->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(3, g.lin_prep_p->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(4, g.prep_state->GetGPUVirtualAddress());

        if (f.prep_exposure == 1) {
            cmd->SetPipelineState(g.prep_meter);
            cmd->Dispatch(1, 1, 1);
            b[0] = D3D12_RESOURCE_BARRIER{};
            b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b[0].UAV.pResource = nullptr;
            cmd->ResourceBarrier(1, b);
        }

        cmd->SetPipelineState(g.prep_forward);
        cmd->Dispatch(gx, gy, 1);
        b[0] = D3D12_RESOURCE_BARRIER{};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b[0].UAV.pResource = nullptr;
        cmd->ResourceBarrier(1, b);
    }

    // the network (both in UNORDERED_ACCESS, and left so)
    const char* nwhy = "";
    const bool net_ok = g.network->record(cmd, in, out, &nwhy);

    if (net_ok && prep_on) {
        b[0] = D3D12_RESOURCE_BARRIER{};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b[0].UAV.pResource = nullptr;
        cmd->ResourceBarrier(1, b);

        cmd->SetComputeRootSignature(g.rs_prep);
        cmd->SetComputeRoot32BitConstants(0, 16, &pc, 0);
        cmd->SetComputeRootUnorderedAccessView(1, g.lin_in->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(2, g.lin_out->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(3, g.lin_prep_p->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(4, g.prep_state->GetGPUVirtualAddress());

        cmd->SetPipelineState(g.prep_back);
        cmd->Dispatch(gx, gy, 1);

        b[0] = D3D12_RESOURCE_BARRIER{};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b[0].UAV.pResource = nullptr;
        cmd->ResourceBarrier(1, b);
    }

    // The network binds its own root signature: the writeback's again.
    cmd->SetComputeRootSignature(g.rs);
    cmd->SetComputeRoot32BitConstants(0, 8, constants, 0);
    cmd->SetComputeRootDescriptorTable(1, gpu);
    cmd->SetComputeRootDescriptorTable(2, gpu_uav);

    ID3D12Resource* copy_back = (prep_on && !net_ok) ? g.lin_prep_p : g.lin_in;
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
    if (prep_on && net_ok)
        transition(b[n++], g.lin_prep_p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (prep_on && !net_ok)
        transition(b[n++], g.lin_in, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    transition(b[n++], g.lin_out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    transition(b[n++], g.lin_mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (have_depth)
        transition(b[n++], g.lin_depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (prep_on)
        transition(b[n++], g.prep_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    cmd->ResourceBarrier(n, b);
    nr::pe::split::restore(cmd, state);

    mark_frame(f.cmd);
    char line[200];
    std::snprintf(line, sizeof line, "ran %ux%u%s: %s", w, h, in_place ? " in place" : "",
                  net_ok ? g.network->name() : (std::string("the network recorded nothing (") + nwhy + "): input copied back").c_str());
    note_backend(line, true);
    return true;
}
bool write_network_capture(const std::string& dir, std::string* why) {
    if (!g.network) { *why = "no network"; return false; }
    return g.network->write_capture(dir, why);
}

HRESULT device_removed_reason() {
    ID3D12Device* d = g.device;
    return d ? d->GetDeviceRemovedReason() : S_OK;
}
AsdNetStatus backend_net_status() {
    return g.network ? g.network->status() : AsdNetStatus{};
}


}  // namespace nr::pe::asd
