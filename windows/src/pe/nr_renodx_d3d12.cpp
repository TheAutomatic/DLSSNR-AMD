// The RenoDX stage on the native D3D12 ASD route (nr_renodx_d3d12.hpp). The host logic is nr_renodx.cpp's
// (0.0.8.1, the Vulkan stage) line for line where it decides what the shaders are given; the shaders are RenoDX's
// own D3D12 ones (windows/shaders/renodx *.cs_5_1.hlsl), compiled at runtime with d3dcompiler_47 as they are,
// RENODX_VULKAN not defined. The frame is always taken whole - the colour copied in, the answer stored into the
// output - which is the Vulkan stage's copied path (its in-place reading and storing are scheduling only, "the same
// values"); the network runs between the encode and the resolve through the backend's ASD network over linear
// buffers, its guides resampled onto the network's extent (to_motion / to_depth, the frame path's own shaders).
#include "nr_renodx_d3d12.hpp"
#include "renodx/codec_gain.hpp"   // the codec's divisors and anchors (dlssnr.hpp's, as nr_renodx.cpp includes it)
namespace nr::asi::native {
#include "nr_fast_shaders.hpp"   // kFastShaders: the Passes (fast) programs, shared with the wilsjo2 stage
}
#include "nr_d3d12_asd.hpp"
#include "nr_pe_log.hpp"
#include "nr_d3d12_capture.hpp"
#include <cstdarg>
#include <cmath>
#include <fstream>
#include <iterator>

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace nr::pe::asd::renodx12 {
using Settings = nr::renodx::Settings;   // RenoDX's own (nr_renodx.hpp)
namespace {

void release(IUnknown* p) { if (p) p->Release(); }

// ---- the inline HLSL: the stage's own plumbing (register layout: t0.. for reads, u0.. for writes, b0 the constants)

// Preprocess on the RenoDX path (windows/shaders/renodx-prep/renodx_prep.comp + prep_math.glsl, ours) as D3D12 HLSL,
// the same modes and the same math: mode 0 meters `a`, mode 1 runs forward(a -> b), mode 2 takes the change back out
// of the answer (RESTORE puts P back into a). The flags are renodx_prep.comp's.
const char kPrepShader[] = R"hlsl(
cbuffer PrepC : register(b0) {
    uint p_width;
    uint p_height;
    uint p_mode;
    uint p_flags;
    uint p_curve;
    float p_bias;
    float p_contrast;
    float p_saturation;
    float p_k;
    float p_cap;
    float p_dt;
    float p_divisor;
};

RWStructuredBuffer<float4> prep_state : register(u0);
RWTexture2D<float4> prep_a : register(u1);
RWTexture2D<float4> prep_b : register(u2);
RWTexture2D<float4> prep_c : register(u3);
Texture2D<float4> prep_scale : register(t0);

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

bool prep_finite_val(float x) { return (asuint(x) & 0x7fffffffu) < 0x7f800000u; }
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

// never set here (renodx_prep.comp's knee: the identity)
float3 prep_knee(float3 d) { return d; }
float3 prep_knee_inv(float3 d) { return d; }
bool prep_unknee() { return false; }
bool prep_linear_frame() { return (p_flags & 16u) != 0u; }
float3 prep_light(float3 P) { return prep_linear_frame() ? max(P, 0.0f) : prep_dec(P); }
float3 prep_reencode(float3 v) { return prep_linear_frame() ? v : prep_enc(v); }

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
float3 prep_gt(float3 x) { return float3(prep_gt1(x.r), prep_gt1(x.g), prep_gt1(x.b)); }
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
float3 prep_gt_inv(float3 y) { return float3(prep_gt_inv1(y.r), prep_gt_inv1(y.g), prep_gt_inv1(y.b)); }

float3 prep_aces(float3 x) { return (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f); }
float3 prep_aces_inv(float3 y) {
    y = min(y, 0.9999f);
    const float3 qa = 2.51f - 2.43f * y, qb = 0.03f - 0.59f * y, qc = -0.14f * y;
    return max((-qb + sqrt(max(qb * qb - 4.0f * qa * qc, 0.0f))) / (2.0f * qa), 0.0f);
}

// GLSL's AGX_IN * c and AGX_OUT * c are column-times-vector; these give the same product (prep_math.glsl's listing,
// triple by triple, as the columns they are there).
float3 agx_in(float3 c) {
    return c.x * float3(0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f) +
           c.y * float3(0.0784335999999992f, 0.878468636469772f, 0.0784336f) +
           c.z * float3(0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f);
}
float3 agx_out(float3 c) {
    return c.x * float3(1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f) +
           c.y * float3(-0.0980208811401368f, 1.15190312990417f, -0.0980434501171241f) +
           c.z * float3(-0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f);
}
// the inverse of a 3x3 by its adjugate (GLSL's inverse() on AGX_OUT / AGX_IN)
float3x3 prep_inv3(float3x3 m) {
    const float3x3 c = float3x3(
        m[1].y * m[2].z - m[1].z * m[2].y, m[1].z * m[2].x - m[1].x * m[2].z, m[1].x * m[2].y - m[1].y * m[2].x,
        m[2].y * m[0].z - m[2].z * m[0].y, m[2].z * m[0].x - m[2].x * m[0].z, m[2].x * m[0].y - m[2].y * m[0].x,
        m[0].y * m[1].z - m[0].z * m[1].y, m[0].z * m[1].x - m[0].x * m[1].z, m[0].x * m[1].y - m[0].y * m[1].x);
    const float det = m[0].x * c[0].x + m[0].y * c[0].y + m[0].z * c[0].z;
    const float inv_det = 1.0f / (abs(det) < 1e-20f ? (det < 0.0f ? -1e-20f : 1e-20f) : det);
    return float3x3(c[0] * inv_det, c[1] * inv_det, c[2] * inv_det);
}
float3x3 agx_in_mat() {
    return float3x3(0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f,
                    0.0784335999999992f, 0.878468636469772f, 0.0784336f,
                    0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f);
}
float3x3 agx_out_mat() {
    return float3x3(1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f,
                    -0.0980208811401368f, 1.15190312990417f, -0.0980434501171241f,
                    -0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f);
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
    float3 v = agx_in(c);
    v = clamp(log2(max(v, 1e-10f)), kAgxMin, kAgxMax);
    v = (v - kAgxMin) / (kAgxMax - kAgxMin);
    v = float3(prep_agx_sig(v.r), prep_agx_sig(v.g), prep_agx_sig(v.b));
    return pow(max(mul(agx_out_mat(), v), 0.0f), 2.2f);
}
float3 prep_agx_inv(float3 y) {
    float3 v = mul(prep_inv3(agx_out_mat()), pow(max(y, 0.0f), 1.0f / 2.2f));
    v = float3(prep_agx_sig_inv(v.r), prep_agx_sig_inv(v.g), prep_agx_sig_inv(v.b));
    v = exp2(v * (kAgxMax - kAgxMin) + kAgxMin);
    return mul(prep_inv3(agx_in_mat()), v);
}

float3 prep_curve(float3 x) {
    switch (p_curve) {
    case 1u: return prep_neutral(x * p_k);
    case 2u: return prep_reinhard(x * p_k);
    case 3u: return prep_hable(x * p_k);
    case 4u: return prep_gt(x * p_k);
    case 5u: return prep_aces(x * p_k);
    case 6u: return prep_agx(x * p_k);
    default: return prep_unknee() ? prep_knee(x) : x;
    }
}
float3 prep_curve_inv(float3 y) {
    switch (p_curve) {
    case 1u: return prep_neutral_inv(y) / p_k;
    case 2u: return prep_reinhard_inv(y) / p_k;
    case 3u: return prep_hable_inv(y) / p_k;
    case 4u: return prep_gt_inv(y) / p_k;
    case 5u: return prep_aces_inv(y) / p_k;
    case 6u: return prep_agx_inv(y) / p_k;
    default: return prep_unknee() ? prep_knee_inv(y) : y;
    }
}

float prep_gain() { return (p_flags & 1u) != 0u ? prep_state[0].z : exp2(p_bias); }

float3 prep_forward(float3 x) {
    x *= prep_gain();
    if (p_contrast != 1.0f) x = 0.18f * pow(max(x, 0.0f) / 0.18f, p_contrast);
    if (p_saturation != 1.0f) {
        const float l = dot(x, kPrepLuma);
        x = max(l + p_saturation * (x - l), 0.0f);
    }
    return prep_curve(x);
}
float3 prep_backward(float3 y) {
    float3 x = prep_curve_inv(y);
    if (p_saturation != 1.0f) {
        const float l = dot(x, kPrepLuma);
        x = l + (x - l) / p_saturation;
    }
    if (p_contrast != 1.0f) x = 0.18f * pow(max(x, 0.0f) / 0.18f, 1.0f / p_contrast);
    return x / prep_gain();
}

// The half an rgba16f store keeps of v on RDNA4 (renodx_prep.comp's half_rtz).
float prep_half1(float v) {
    return abs(v) >= 6.103515625e-05f ? asfloat(asuint(v) & 0xffffe000u)
                                      : trunc(v * 16777216.0f) * 5.9604644775390625e-08f;
}
float3 prep_half(float3 v) { return float3(prep_half1(v.r), prep_half1(v.g), prep_half1(v.b)); }

groupshared uint s_prep_hist[128];

[numthreads(16, 16, 1)]
void prep_main(uint3 id : SV_DispatchThreadID, uint t : SV_GroupIndex) {
    if (p_mode == 0u) {
        if (t < 128u) s_prep_hist[t] = 0u;
        GroupMemoryBarrierWithGroupSync();
        const uint sx = max(1u, p_width / 160u), sy = max(1u, p_height / 90u);
        const uint nx = (p_width + sx - 1u) / sx, ny = (p_height + sy - 1u) / sy;
        for (uint i = t; i < nx * ny; i += 256u) {
            const int2 xy = int2((i % nx) * sx, (i / nx) * sy);
            const float3 P = prep_a[xy].rgb;
            if (!prep_finite(P)) continue;
            const float l = max(dot(prep_light(P), kPrepLuma), 1.5e-5f);
            InterlockedAdd(s_prep_hist[uint(clamp((log2(l) + 16.0f) * 6.4f, 0.0f, 127.0f))], 1u);
        }
        GroupMemoryBarrierWithGroupSync();
        if (t != 0u) return;
        uint n = 0u;
        for (uint b = 0u; b < 128u; ++b) n += s_prep_hist[b];
        const bool valid = prep_state[0].y == 1.0f && (p_flags & 8u) == 0u;
        float s = valid ? prep_state[0].x : 0.0f;
        if (n >= 64u) {
            const float lo = kLow * float(n), hi = kHigh * float(n);
            float below = 0.0f, sum = 0.0f, weight = 0.0f;
            for (uint b = 0u; b < 128u; ++b) {
                const float c = float(s_prep_hist[b]);
                const float from = max(below, lo), to = min(below + c, hi);
                if (to > from) { sum += (to - from) * ((float(b) + 0.5f) / 6.4f - 16.0f); weight += to - from; }
                below += c;
            }
            const float target = clamp(kLog2Key - sum / max(weight, 1.0f), kMinGain, kMaxGain);
            const float speed = target < s ? kSpeedBrighter : kSpeedDarker;
            s = valid ? s + (target - s) * (1.0f - exp(-clamp(p_dt, 0.0f, 0.25f) * speed)) : target;
        }
        s = clamp(s, kMinGain, kMaxGain);
        prep_state[0] = float4(s, 1.0f, exp2(s + p_bias), 0.0f);
        return;
    }
    const uint2 pixel = id.xy;
    if (pixel.x >= p_width || pixel.y >= p_height) return;
    if (p_mode == 1u) {
        const float4 P = prep_a[pixel];
        if (prep_linear_frame()) {
            float3 x = prep_light(P.rgb);
            if ((p_flags & 32u) != 0u)
                x /= p_divisor > 0.0f ? p_divisor : max(prep_scale.Load(int3(0, 0, 0)).r, 1e-8f);
            prep_b[pixel] = prep_finite(P.rgb) ? float4(prep_forward(x), P.a) : P;
            return;
        }
        if ((p_flags & 128u) != 0u) {
            prep_b[pixel] = prep_finite(P.rgb) ? float4(prep_enc(prep_forward(prep_light(P.rgb))), P.a) : P;
            return;
        }
        prep_b[pixel] = P;
        if (!prep_finite(P.rgb)) return;
        prep_a[pixel] = float4(prep_enc(prep_forward(prep_light(P.rgb))), P.a);
        return;
    }
    const float4 nout = prep_c[pixel];
    const float4 P = prep_b[pixel];
    float4 nin;
    if ((p_flags & 256u) != 0u) {
        nin = prep_finite(P.rgb) ? float4(prep_half(prep_enc(prep_forward(prep_light(P.rgb)))), P.a) : P;
    } else {
        nin = prep_a[pixel];
    }
    float3 M = P.rgb;
    if (any(nout.rgb != nin.rgb) && prep_finite(nout.rgb) && prep_finite(P.rgb)) {
        const float3 din = prep_dec(nin.rgb), dout = prep_dec(nout.rgb);
        const float3 p = prep_light(P.rgb);
        float3 d;
        if ((p_flags & 64u) != 0u) {
            d = p * (clamp(max(dout, 1.0f / 1024.0f) / max(din, 1.0f / 1024.0f), 0.25f, 4.0f) - 1.0f);
        } else {
            d = prep_backward(dout) - prep_backward(din);
            const float3 lim = p_cap * abs(dout - din) / prep_gain();
            d = clamp(d, -lim, lim);
        }
        M = max(P.rgb + prep_reencode(max(p + d, 0.0f)) - prep_reencode(p), 0.0f);
        if (!prep_finite(M)) M = P.rgb;
    }
    prep_c[pixel] = float4(M, nout.a);
    if ((p_flags & 4u) != 0u) prep_a[pixel] = P;
}
)hlsl";

// NR Control's debug views (renodx_debug.comp), over the picture the stage hands the frame.
const char kDebugShader[] = R"hlsl(
Texture2D<float4> DebugProxy : register(t0);
Texture2D<float4> DebugNeural : register(t1);
RWTexture2D<float4> DebugOutput : register(u0);

cbuffer DebugC : register(b0) {
    uint2 d_size;
    uint2 d_net;
    uint d_view;
};

[numthreads(16, 16, 1)]
void debug_main(uint3 id : SV_DispatchThreadID) {
    const uint2 pixel = id.xy;
    if (pixel.x >= d_size.x || pixel.y >= d_size.y) return;
    const uint2 ns = max(d_net, uint2(1u, 1u));
    const int2 q = int2(min(uint2(((float2(pixel) + 0.5f) * float2(ns)) / float2(d_size)), ns - 1u));
    const float3 p = DebugProxy.Load(int3(q, 0)).rgb;
    const float3 n = DebugNeural.Load(int3(q, 0)).rgb;
    const float3 v = d_view == 1u ? p : d_view == 2u ? n : clamp(0.5f + 20.0f * (n - p), 0.0f, 1.0f);
    DebugOutput[pixel] = float4(v, 1.0f);
}
)hlsl";

// The plumbing: proxy texture <-> linear buffer (the network's buffers), the stored picture into the output, and the
// game's guides onto the network's extent (nr_d3d12_asd_backend.cpp kShaders' to_motion / to_depth, the same mapping).
const char kAuxShaders[] = R"hlsl(
cbuffer AuxC : register(b0) {
    uint a_w;
    uint a_h;
    uint a_what;
    uint a_pad0;
    float4 a_region;
};

Texture2D<float4> aux_tex : register(t0);
SamplerState aux_linear_clamp : register(s0);
StructuredBuffer<float4> aux_buf : register(t0);
RWStructuredBuffer<float4> aux_out : register(u0);
RWTexture2D<float4> aux_dst : register(u0);

[numthreads(8, 8, 1)]
void tex_to_buf(uint3 id : SV_DispatchThreadID) {
    if (id.x < a_w && id.y < a_h) aux_out[id.y * a_w + id.x] = aux_tex.Load(int3(id.xy, 0));
}

[numthreads(8, 8, 1)]
void buf_to_tex(uint3 id : SV_DispatchThreadID) {
    if (id.x >= a_w || id.y >= a_h) return;
    aux_dst[id.xy] = aux_buf[id.y * a_w + id.x];
}

[numthreads(8, 8, 1)]
void copy_out(uint3 id : SV_DispatchThreadID) {
    if (id.x >= a_w || id.y >= a_h) return;
    aux_dst[id.xy] = aux_tex.Load(int3(id.xy, 0));
}

[numthreads(8, 8, 1)]
void to_motion(uint3 id : SV_DispatchThreadID) {
    if (id.x >= a_w || id.y >= a_h) return;
    const float2 uv = (float2(id.xy) + 0.5f) / float2(a_w, a_h);
    aux_out[id.y * a_w + id.x] = float4(aux_tex.SampleLevel(aux_linear_clamp, uv * a_region.xy + a_region.zw, 0).xy, 0, 0);
}

[numthreads(8, 8, 1)]
void to_depth(uint3 id : SV_DispatchThreadID) {
    if (id.x >= a_w || id.y >= a_h) return;
    const float2 s = floor((float2(id.xy) + 0.5f) * a_region.xy / float2(a_w, a_h));
    const int2 t = min(int2(s), int2(a_region.xy) - 1);
    aux_out[id.y * a_w + id.x] = float4(aux_tex.Load(int3(t + int2(a_region.zw), 0)).r, 0, 0, 0);
}
)hlsl";

DXGI_FORMAT srv_format_of(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
        default: return f;
    }
}
DXGI_FORMAT uav_format_of(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return f;
    }
}

// RenoDX's own files, read from the installed shaders folder (their #includes resolved there).
std::string read_text(const std::wstring& path) {
    std::string out;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buf[16384];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

struct FolderInclude final : ID3DInclude {
    std::vector<std::wstring> dirs;
    STDMETHOD(Open)(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
        const std::string n(name);
        std::wstring w(n.begin(), n.end());
        for (const std::wstring& d : dirs) {
            std::string text = read_text(d + L"\\" + w);
            if (text.empty()) continue;
            auto* copy = new char[text.size()];
            std::memcpy(copy, text.data(), text.size());
            *data = copy;
            *bytes = UINT(text.size());
            return S_OK;
        }
        return E_FAIL;
    }
    STDMETHOD(Close)(LPCVOID data) override { delete[] static_cast<const char*>(data); return S_OK; }
};

ID3DBlob* compile_hlsl(const char* source, size_t bytes, const char* entry, std::string* why, FolderInclude* inc,
                       const D3D_SHADER_MACRO* macros = nullptr) {
    using PFN = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                 UINT, UINT, ID3DBlob**, ID3DBlob**);
    static PFN fn = [] {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        return m ? reinterpret_cast<PFN>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile"))) : nullptr;
    }();
    if (!fn) { *why = "d3dcompiler_47.dll (D3DCompile) is not available"; return nullptr; }
    ID3DBlob *code = nullptr, *errors = nullptr;
    // Test-only: D3DCOMPILE_SKIP_OPTIMIZATION (bit 2) instead of level-3, to see whether the codec's ULP differences
    // against the SPIR-V build come from the optimizer's reassociation. The product compiles at level 3.
    const UINT flags = (std::getenv("NR_TEST_CODEC_NOOPT") ? (1u << 2) : 0u) | (1u << 15);
    const HRESULT hr = fn(source, bytes, entry, macros, inc, entry, "cs_5_1", flags, 0, &code, &errors);
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

// ---- look_stage.hpp's host side, verbatim from nr_renodx.cpp (device-independent) ------------------------
namespace look {
constexpr uint32_t kFlagBands = 1u << 0, kFlagTemporal = 1u << 1, kFlagMotion = 1u << 2,
                   kFlagTemporalDetail = 1u << 3, kFlagTransport = 1u << 4, kFlagRefLinear = 1u << 5,
                   kFlagHistoryValid = 1u << 6, kFlagRangeConfidence = 1u << 7;
constexpr float kHistoryMaxGapSeconds = 0.25f;
constexpr uint32_t kUpsampleAuto = 0, kUpsampleEdgeAware = 1, kUpsampleClassic = 2, kUpsampleAreaInput = 3;
constexpr float kAreaEncode = 1.f;
constexpr float kDetailStabilityAlpha = 0.15f;

struct Constants {
    uint32_t size[2], nr_size[2], full_size[2], flags, mode;
    float strength, brighten, darken, max_brighten, max_darken, colour, hue, max_colour, shadows, midtones,
        highlights, tone, detail, halo;
    uint32_t radius;
    float alpha, motion_scale[2];
    uint32_t motion_base[2], motion_grid[2];
    float gf_eps, halo_bright;
    uint32_t min_radius, mid_radius, padding;
};

struct Settings {
    float strength = 1.f, brighten = 1.f, darken = 1.f, max_brighten = 0.f, max_darken = 0.f, colour = 1.f,
          hue = 1.f, max_colour = 0.f, shadows = 1.f, midtones = 1.f, highlights = 1.f, tone = 1.f, detail = 1.f,
          halo = 0.f, halo_bright = 0.f, detail_radius = 1.f;
    uint32_t stabilize = 0;
    float stabilize_ms = 60.f;
    bool stabilize_detail = false;
    float stabilize_alpha = 0.f;
    uint32_t upsample = 0;
};

bool gains_neutral(const Settings& s) {
    return s.strength == 1.f && s.brighten == 1.f && s.darken == 1.f && s.max_brighten <= 0.f && s.max_darken <= 0.f &&
           s.colour == 1.f && s.hue == 1.f && s.max_colour <= 0.f && s.shadows == 1.f && s.midtones == 1.f &&
           s.highlights == 1.f && s.tone == 1.f && s.detail == 1.f && s.halo <= 0.f && s.halo_bright <= 0.f;
}
bool bands_wanted(const Settings& s) {
    return s.tone != 1.f || s.detail != 1.f || s.halo > 0.f || s.halo_bright > 0.f ||
           (s.stabilize != 0 && s.stabilize_alpha <= 0.f);
}
bool dispatched(const Settings& s, bool transport) { return !gains_neutral(s) || s.stabilize != 0 || transport; }
bool transport_wanted(const Settings& s, uint32_t nw, uint32_t nh, uint32_t fw, uint32_t fh) {
    return s.upsample != kUpsampleClassic && nw <= fw && nh <= fh && (nw != fw || nh != fh);
}

struct Frame {
    uint32_t nr_width = 0, nr_height = 0, full_width = 0, full_height = 0;
    bool reference_linear = false, transport = false;
    float frame_seconds = 1.f / 60.f;
    bool history_valid = false, motion = false;
    float motion_scale[2] = {0.f, 0.f};
    uint32_t motion_base[2] = {0, 0}, motion_grid[2] = {1, 1};
};

uint32_t band_radius(const Settings& s, uint32_t nr_height) {
    const float half_pixels = s.detail_radius * 0.01f * float(nr_height) * 0.5f;
    return std::clamp(uint32_t(std::lround(half_pixels)), 1u, 64u);
}


Constants make_constants(const Settings& s, const Frame& f) {
    Constants c{};
    c.size[0] = f.nr_width; c.size[1] = f.nr_height;
    c.nr_size[0] = f.nr_width; c.nr_size[1] = f.nr_height;
    c.full_size[0] = f.full_width; c.full_size[1] = f.full_height;
    const bool temporal = s.stabilize != 0;
    c.flags = (bands_wanted(s) ? kFlagBands : 0u) | (temporal ? kFlagTemporal : 0u) |
              (temporal && s.stabilize == 2u && f.motion ? kFlagMotion : 0u) |
              (temporal && s.stabilize_detail ? kFlagTemporalDetail : 0u) | (f.transport ? kFlagTransport : 0u) |
              (f.reference_linear ? kFlagRefLinear : 0u) | (temporal && f.history_valid ? kFlagHistoryValid : 0u) |
              (temporal && s.stabilize_alpha > 0.f ? kFlagRangeConfidence : 0u);
    c.strength = s.strength; c.brighten = s.brighten; c.darken = s.darken;
    c.max_brighten = s.max_brighten; c.max_darken = s.max_darken;
    c.colour = s.colour; c.hue = s.hue; c.max_colour = s.max_colour;
    c.shadows = s.shadows; c.midtones = s.midtones; c.highlights = s.highlights;
    c.tone = s.tone; c.detail = s.detail; c.halo = s.halo; c.halo_bright = s.halo_bright;
    c.radius = band_radius(s, f.nr_height);
    c.min_radius = std::min(c.radius * 4u, 48u);
    c.mid_radius = std::min(c.radius * 6u, 64u);
    c.alpha = s.stabilize_alpha > 0.f ? s.stabilize_alpha
                                      : 1.f - std::exp(-f.frame_seconds / std::max(s.stabilize_ms * 1e-3f, 1e-3f));
    c.motion_scale[0] = f.motion_scale[0]; c.motion_scale[1] = f.motion_scale[1];
    c.motion_base[0] = f.motion_base[0]; c.motion_base[1] = f.motion_base[1];
    c.motion_grid[0] = std::max(f.motion_grid[0], 1u); c.motion_grid[1] = std::max(f.motion_grid[1], 1u);
    c.gf_eps = 0.01f;
    return c;
}

Settings snapshot(const nr::renodx::Settings& s, bool auto_engages) {
    Settings out{};
    out.upsample = s.look_upsample;
    if (s.look_mode) {
        out.strength = s.look_strength; out.brighten = s.look_brighten; out.darken = s.look_darken;
        out.max_brighten = s.look_max_brighten; out.max_darken = s.look_max_darken;
        out.colour = s.look_colour; out.hue = s.look_hue; out.max_colour = s.look_max_colour;
        out.shadows = s.look_shadows; out.midtones = s.look_midtones; out.highlights = s.look_highlights;
        out.tone = s.look_tone; out.detail = s.look_detail; out.halo = s.look_halo; out.halo_bright = s.look_halo_bright;
        out.detail_radius = s.look_detail_radius;
        out.stabilize = s.look_stabilize; out.stabilize_ms = s.look_stabilize_ms;
        out.stabilize_detail = s.look_stabilize_detail;
    }
    if (out.stabilize == 0u && (s.detail_stability == 2u || (s.detail_stability == 0u && auto_engages))) {
        out.stabilize = 2u;
        out.stabilize_detail = true;
        out.stabilize_alpha = kDetailStabilityAlpha;
    }
    return out;
}
}  // namespace look

// The legacy cbuffer (legacy_common.hlsli's CodecConstants, 20 dwords).
struct LegacyConstants {
    uint32_t size[2], source_size[2], source_base[2], proxy_size[2], neural_size[2];
    float paper_white_scale, transfer_strength, color_strength;
    uint32_t hdr;
    float diffuse_white_nits;
    float unread;
    float padding[4];
};
static_assert(sizeof(LegacyConstants) == 80);

// Preprocess's push block (renodx_prep.comp's, 12 dwords).
struct PrepPush {
    uint32_t w, h, mode, flags, curve;
    float bias, contrast, saturation, k, cap;
    float dt;
    float divisor;
};
enum : uint32_t { kPrepAuto = 1, kPrepRestore = 4, kPrepReset = 8, kPrepLinear = 16, kPrepNormalise = 32,
                  kPrepRatio = 64, kPrepApart = 128, kPrepRecompute = 256 };
constexpr float kPrepAnchor[7] = {1.0f, 1.0f, 1.2195122f, 2.9275228f, 1.0052344f, 0.7231708f, 0.8083602f};

// RenoDX's SettingEffect (nr_renodx.cpp's history_settings_differ / recreate_settings_differ, verbatim).
bool history_settings_differ(const Settings& a, const Settings& b) {
    auto same3 = [](const auto& x, const auto& y) { return x[0] == y[0] && x[1] == y[1] && x[2] == y[2]; };
    return a.passes_mode != b.passes_mode || a.fast_method != b.fast_method ||
           std::memcmp(a.fast_passes, b.fast_passes, sizeof a.fast_passes) != 0 ||
           std::memcmp(a.fast_halo_limit, b.fast_halo_limit, sizeof a.fast_halo_limit) != 0 ||
           std::memcmp(a.fast_fine, b.fast_fine, sizeof a.fast_fine) != 0 ||
           a.fast_edit_strength != b.fast_edit_strength ||
           a.auto_mask != b.auto_mask || a.chained_history != b.chained_history || a.codec_mode != b.codec_mode ||
           a.depth_mode != b.depth_mode || a.feed_mode != b.feed_mode || a.intensity != b.intensity ||
           a.local_structure != b.local_structure || a.local_tone != b.local_tone ||
           a.mvec_scale_x != b.mvec_scale_x || a.mvec_scale_y != b.mvec_scale_y ||
           a.norm_governor != b.norm_governor || a.norm_slew_stops != b.norm_slew_stops ||
           a.pq_calibration != b.pq_calibration || a.paper_white_scale != b.paper_white_scale ||
           !same3(a.pass_auto_mask, b.pass_auto_mask) || !same3(a.pass_follow, b.pass_follow) ||
           !same3(a.pass_intensity, b.pass_intensity) || !same3(a.pass_local_tone, b.pass_local_tone) ||
           !same3(a.pass_skin_structure, b.pass_skin_structure) ||
           !same3(a.pass_local_structure, b.pass_local_structure) || !same3(a.pass_style, b.pass_style) ||
           a.present_encoding != b.present_encoding || a.present_guides != b.present_guides ||
           a.present_white_nits != b.present_white_nits || a.skin_independent != b.skin_independent ||
           a.skin_structure != b.skin_structure || a.style != b.style;
}
bool recreate_settings_differ(const Settings& a, const Settings& b) {
    return a.hook_point != b.hook_point || a.linear_unit_nits != b.linear_unit_nits || a.passes != b.passes ||
           a.unlock_passes != b.unlock_passes || a.source_encoding != b.source_encoding;
}

uint32_t bits(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; }

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// InferHdrMode / FrameHdrMode on DXGI formats (nr_renodx.cpp's Vulkan ones).
uint8_t infer_hdr_mode_dxgi(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return 1;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return 2;
        default:
            return 0;
    }
}
uint8_t frame_hdr_mode_dxgi(DXGI_FORMAT f, bool hdr_flag) {
    if (hdr_flag) return std::max<uint8_t>(infer_hdr_mode_dxgi(f), 1u);
    if (f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R10G10B10A2_TYPELESS) return 0;
    return infer_hdr_mode_dxgi(f);
}

// PassTransferStrength / PassColorStrength (nr_renodx.cpp's strengths): pass 1 the global pair, passes 2.. their
// own (4 and later: 4's).
void strengths(const Settings& s, uint32_t pass, float* transfer, float* color) {
    if (pass == 0) { *transfer = s.transfer_strength; *color = s.color_strength; return; }
    const uint32_t i = std::min(pass, 3u) - 1u;
    *transfer = s.pass_transfer[i];
    *color = s.pass_color[i];
}

// A depth texture's SRV view (nr_d3d12_asd_backend.cpp's depth_view_format).
DXGI_FORMAT depth_srv_format_of(DXGI_FORMAT format) {
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

// The history pass k of a feature's stack runs on (pass 0: the feature's own) - nr_pe_session.cpp's stack_feature.
// Pass 1's name on a route whose frames carry none (feature 0), when it stacks; no game feature is numbered so.
uint64_t stack_feature(uint64_t feature, uint32_t k) {
    return k == 0 ? feature : (uint64_t(1) << 62) | (feature << 8) | k;
}
constexpr uint64_t kUnnamedStack = 0xFFFFFFu;

// ---- RenoDX host logic for the v6 codec (nr_renodx.cpp, verbatim where it decides what the shaders are given) ----
namespace codec = ::renodx::dlss5::codec;

codec::SourceUnits interpret_source_units(const Settings& s, uint8_t hdr_mode) {
    codec::SourceUnits units;
    uint8_t choice = 0;
    if (s.source_encoding != 0u) choice = uint8_t(s.source_encoding);
    else if (hdr_mode == 2u) choice = 3u;
    else if (hdr_mode == 1u) choice = 2u;
    switch (choice) {
        case 1u: units.encoding = codec::Encoding::Sdr; break;
        case 2u: units.encoding = codec::Encoding::Linear; break;
        case 3u: units.encoding = codec::Encoding::Pq; units.absolute = true; units.unit_nits = 10000.f; break;
        default: units.encoding = codec::Encoding::Linear; break;
    }
    if (units.encoding == codec::Encoding::Linear && s.linear_unit_nits > 0.f) {
        units.absolute = true;
        units.unit_nits = s.linear_unit_nits;
    }
    return units;
}

uint32_t v6_encoding_constant(const codec::SourceUnits& u) {
    switch (u.encoding) {
        case codec::Encoding::Pq: return u.bt2020 ? 4u : 2u;
        case codec::Encoding::Sdr: return 3u;
        default: return 1u;
    }
}

float v6_dark_gate(const codec::SourceUnits& u) {
    return u.absolute && u.unit_nits > 0.f ? 1.f / u.unit_nits : 0.005f;
}

bool frame_display_codec(const Settings& s, uint8_t hdr_mode, const codec::SourceUnits& u) {
    if (hdr_mode == 0) return false;
    if (s.codec_mode == 3u) return true;
    if (s.codec_mode == 2u) return !(u.absolute && u.unit_nits > 0.f);
    return false;
}

bool frame_pedestal_removal(const Settings& s, uint8_t hdr_mode, const codec::SourceUnits& u) {
    if (!frame_display_codec(s, hdr_mode, u)) return true;
    if (s.display_pedestal == 1u) return true;
    return u.absolute && u.unit_nits > 0.f;
}

float frame_codec_divisor(const Settings& s, uint8_t hdr_mode, const codec::SourceUnits& u) {
    if (hdr_mode == 0) return 1.f;
    if (!u.absolute || !(u.unit_nits > 0.f)) return 0.f;
    if (frame_display_codec(s, hdr_mode, u))
        return codec::DisplayAnchorLinear(u, s.proxy_anchor_nits, s.diffuse_white_nits);
    if (s.codec_mode == 1u) return codec::AnchoredDivisor(s.proxy_anchor_nits, u);
    return u.encoding == codec::Encoding::Pq
        ? codec::ClassicPqDivisor(s.diffuse_white_nits, s.paper_white_scale, s.pq_calibration)
        : codec::ClassicLinearDivisor(s.paper_white_scale);
}

// The frame's snapshots (ProcessInline): one divisor, encoding, gate and cap.
struct Frame6 {
    uint8_t hdr_mode{};
    codec::SourceUnits units;
    float divisor{}, cap{}, gate{};
    uint32_t encoding{};
};

// cbuffer CodecConstants of v6_common.hlsli (V6CodecConstants).
struct V6Constants {
    uint32_t width, height, source_width, source_height, source_base_x, source_base_y;
    uint32_t proxy_width, proxy_height, neural_width, neural_height;
    float divisor, pedestal_cap, transfer_strength, color_strength;
    uint32_t encoding;
    float dark_gate;
    uint32_t curve;
    float pedestal_gate, slew_stops, snap_now, chroma_clamp_stops, release_stops;
    uint32_t governor_mode, transfer_mode;
    float highlight_guard_stops;
    uint32_t dark_gate_mode;
    uint32_t padding[2];
};
static_assert(sizeof(V6Constants) == 28 * 4);
// The _prep builds' cbuffer (v6_common.hlsli under NR_PREP_RESOLVE): dwords 26 / 27 carry [Preprocess]'s curve
// and its input scale, then its contrast and saturation.
struct V6PrepConstants {
    V6Constants v;
    float contrast, saturation;
    float floor;   // prep_floor()
};
static_assert(sizeof(V6PrepConstants) == 31 * 4);

V6Constants make_v6(const Settings& s, const Frame6& f, uint32_t w, uint32_t h, uint32_t sw, uint32_t sh,
                    uint32_t pw, uint32_t ph, uint32_t nw, uint32_t nh, float transfer, float color, float chroma,
                    bool ratio_resolve = false) {
    V6Constants c{};
    c.width = w; c.height = h; c.source_width = sw; c.source_height = sh;
    c.proxy_width = pw; c.proxy_height = ph; c.neural_width = nw; c.neural_height = nh;
    c.divisor = f.divisor; c.pedestal_cap = f.cap;
    c.transfer_strength = transfer; c.color_strength = color;
    c.encoding = f.encoding; c.dark_gate = f.gate;
    c.curve = ratio_resolve || frame_display_codec(s, f.hdr_mode, f.units) ? 1u : s.neural_floor_guard ? 2u : 0u;
    c.pedestal_gate = frame_pedestal_removal(s, f.hdr_mode, f.units) ? 1.f : 0.f;
    c.chroma_clamp_stops = chroma;
    c.transfer_mode = s.transfer_mode;
    c.highlight_guard_stops = s.style_highlight_guard;
    c.dark_gate_mode = s.display_pedestal;
    return c;
}

// PrepBackward(float3(1/1024)).y of prep_resolve.hlsli on the CPU: the resolve's floor behind [Preprocess]'s curve,
// the same for every pixel (nr_renodx.cpp's prep_floor, verbatim).
float prep_floor(uint32_t curve, float k, float contrast, float saturation) {
    const float y = 1.0f / 1024.0f;
    float v[3] = {y, y, y};
    auto each = [&](float (*f)(float)) { for (float& c : v) c = f(c) / k; };
    switch (curve) {
        case 1: {   // neutral: below its start the identity (1/1024 always is)
            for (float& c : v) c /= k;
            break;
        }
        case 2: each([](float c) { c = std::min(c, 0.9999f); return c / (1.0f - c); }); break;
        case 3: each([](float c) {
            const float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
            auto h1 = [&](float x) { return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F; };
            const float t = std::min(c * h1(11.2f) + E / F, 0.999f);
            const float qa = A * (1.0f - t), qb = C * B - B * t, qc = D * E - D * F * t;
            return std::max((-qb + std::sqrt(std::max(qb * qb - 4.0f * qa * qc, 0.0f))) / (2.0f * qa), 0.0f);
        }); break;
        case 4: each([](float c) {   // gt's toe (1/1024 < m): Newton on t = x / m, as gt_inv1
            const float m = 0.22f, cc = 1.33f;
            float t = std::pow(std::max(c, 0.0f) / m, 1.0f / cc);
            for (int i = 0; i < 4; ++i) {
                const float s = t * t * (3.0f - 2.0f * t), ds = 6.0f * t * (1.0f - t), tc = std::pow(t, cc);
                const float g = tc * (1.0f - s) + t * s - c / m;
                const float dg = cc * std::pow(std::max(t, 1e-20f), cc - 1.0f) * (1.0f - s) - tc * ds + s + t * ds;
                t = std::clamp(t - g / std::max(dg, 1e-6f), 0.0f, 1.0f);
            }
            return m * t;
        }); break;
        case 5: each([](float c) {
            c = std::min(c, 0.9999f);
            const float qa = 2.51f - 2.43f * c, qb = 0.03f - 0.59f * c, qc = -0.14f * c;
            return std::max((-qb + std::sqrt(std::max(qb * qb - 4.0f * qa * qc, 0.0f))) / (2.0f * qa), 0.0f);
        }); break;
        case 6: {   // agx: inverse(AGX_OUT) = AGX_IN, the sigmoid's inverse, exp2, inverse(AGX_IN) = AGX_OUT
            static const float in[3][3] = {{0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f},
                                           {0.0784335999999992f, 0.878468636469772f, 0.0784336f},
                                           {0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f}};
            static const float out[3][3] = {{1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f},
                                            {-0.0980208811401368f, 1.15190312990417f, -0.0980434501171241f},
                                            {-0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f}};
            auto sig = [](float x) { const float x2 = x * x, x4 = x2 * x2;
                return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f; };
            auto dsig = [](float x) { const float x2 = x * x, x4 = x2 * x2;
                return 93.0f * x4 * x - 200.7f * x4 + 127.84f * x2 * x - 20.604f * x2 + 0.8596f * x + 0.1191f; };
            const float g = std::pow(y, 1.0f / 2.2f);
            float u[3] = {}, w[3] = {};
            for (int col = 0; col < 3; ++col) for (int r = 0; r < 3; ++r) u[r] += in[col][r] * g;
            for (float& c : u) {
                c = std::clamp(c, sig(0.0f), sig(1.0f));
                float lo = 0.0f, hi = 1.0f;
                for (int i = 0; i < 8; ++i) { const float mid = 0.5f * (lo + hi); if (sig(mid) < c) lo = mid; else hi = mid; }
                float x = 0.5f * (lo + hi);
                for (int i = 0; i < 3; ++i) x = std::clamp(x - (sig(x) - c) / dsig(x), lo, hi);
                c = std::exp2(x * (4.026069f + 12.47393f) - 12.47393f);
            }
            for (int col = 0; col < 3; ++col) for (int r = 0; r < 3; ++r) w[r] += out[col][r] * u[col];
            for (int i = 0; i < 3; ++i) v[i] = w[i] / k;
            break;
        }
        case 7: {   // v6_encode's toe; the floor is below the shoulder
            const float b = y - 0.014f - 0.001f;
            const float root = std::sqrt(b * b + 4.0f * y * 0.001f);
            for (float& c : v) c = 2.0f * y * 0.001f / (root - b);
            break;
        }
        // 8: the classic codec's shoulder is the identity at the floor.
        default: break;
    }
    if (saturation != 1.0f) {
        const float l = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
        for (float& c : v) c = l + (c - l) / saturation;
    }
    if (contrast != 1.0f)
        for (float& c : v) c = 0.18f * std::pow(std::max(c, 0.0f) / 0.18f, 1.0f / contrast);
    return std::max(v[1], 1e-8f);
}

// FeedSource: where a relative-HDR frame's NR input scale comes from.
enum Feed : uint32_t { kFeedV1 = 0, kFeedTexture = 1, kFeedFixed = 2 };

// The governor's per-stream state (NormStream) and PlanFrameScale.
struct NormStream {
    bool primed = false;
    uint32_t feed = kFeedV1;
    int64_t last_ns = 0, last_reset_ns = 0;
};
constexpr int64_t kNormResetQuietNs = 250'000'000;

struct ScalePlan {
    uint32_t governor[5] = {};   // slew_stops .. governor_mode
    bool guarded = false;        // Auto's texture feed, range-guarded against v1
    bool v2_snap = false;        // v6_exposure_scale's SnapNow
};

// SelectFeed: v2 needs positive evidence (a readable exposure texture on a stream without AutoExposure).
uint32_t select_feed(const Settings& s, uint8_t hdr_mode, const codec::SourceUnits& u, bool readable, bool auto_flag) {
    if (hdr_mode == 0 || u.absolute || u.encoding == codec::Encoding::Sdr || s.feed_mode == 1u) return kFeedV1;
    if (s.feed_mode == 2u) return readable ? kFeedTexture : kFeedFixed;
    return readable && !auto_flag ? kFeedTexture : kFeedV1;
}

ScalePlan plan_frame_scale(const Settings& s, NormStream& stream, bool frame_reset, uint32_t feed, bool epoch) {
    ScalePlan plan;
    const uint32_t mode = s.norm_governor == 2u && s.norm_cut_aware ? 3u : s.norm_governor;
    const bool stable = mode >= 2u, cut_aware = mode >= 3u;
    const int64_t now = now_ns();
    bool reset_snap = !stable && frame_reset;
    if (cut_aware && frame_reset) {
        reset_snap = stream.last_reset_ns == 0 || now - stream.last_reset_ns >= kNormResetQuietNs;
        stream.last_reset_ns = now;
    }
    // The reset-epoch snap of modes 0/1: RenoDX's epoch advances on every history reset it requests - here `epoch`
    // (recreate or history_reset of the frame before).
    const bool prime = !stream.primed, feed_change = stream.primed && stream.feed != feed;
    const bool snap = prime || reset_snap || feed_change || (!stable && epoch);
    stream.primed = true;
    stream.feed = feed;
    const float frame_seconds = stream.last_ns != 0
        ? std::clamp((now - stream.last_ns) / 1e9f, cut_aware ? 0.f : 1.f / 240.f, 0.1f)
        : 1.f / 60.f;
    stream.last_ns = now;
    const float attack = stable ? s.norm_attack_stops * frame_seconds : s.norm_slew_stops;
    const float release = stable ? s.norm_release_stops * frame_seconds : 0.f;
    plan.governor[0] = bits(mode == 0u ? 0.f : attack);
    plan.governor[1] = bits(snap ? 1.f : 0.f);
    plan.governor[3] = bits(release);
    plan.governor[4] = mode;
    plan.guarded = feed == kFeedTexture && s.feed_mode == 0u;
    plan.v2_snap = prime || feed_change || (reset_snap && !cut_aware);
    return plan;
}


}  // namespace

uint8_t frame_hdr_mode(DXGI_FORMAT format, bool hdr_flag) { return frame_hdr_mode_dxgi(format, hdr_flag); }
struct Stage::Impl {
    ID3D12Device* device = nullptr;
    Config cfg;
    Settings settings;
    uint8_t hdr = 0;
    bool broken = false;

    enum P { kEncodeL, kDecodeL, kReduceL, kReduceLU, kApplyL, kDecodeReduceL, kCompose, kUpsample, kBand,
             // the v6 codec's (HDR), RenoDX's own files as well
             kLinearize, kAutoscale, kExposureScale, kEncode6, kResolve6, kReduce6, kReduce6U, kCommit6,
             kCommitExposure, kResolveCommit6, kPrepEncode,
             // the resolve when [Preprocess] made the proxy (NR_PREP_RESOLVE builds of v6_resolve & co)
             kResolve6Prep, kResolveCommit6Prep, kReduce6UPrep,
             kDebug,
             // the aux root signature (kFastShaders', the prep port, the plumbing)
             kStFbIn, kStPrep, kStBox, kStApply, kPrep, kTexToBuf, kBufToTex, kCopyOut, kToMotion, kToDepth,
             kCount };
    bool codec_sig(P p) const { return p <= kDebug; }
    ID3D12RootSignature* rs_codec = nullptr;
    ID3D12RootSignature* rs_aux = nullptr;
    ID3D12PipelineState* pso[kCount] = {};
    ID3D12DescriptorHeap* heap = nullptr;          // shader-visible, the dispatches' views
    ID3D12DescriptorHeap* clear_heap = nullptr;    // one descriptor, for the prep state's zeroing clear
    UINT inc = 0, slot = 0;
    static constexpr UINT kSlots = 4096;   // the first heap: one dispatch a slot (20 descriptors); a frame uses tens
    UINT slots = kSlots;                   // this heap's slots (grows when a frame would outrun completion)
    UINT kClearSlot = kSlots * 20;          // the clear descriptors at the end of this heap
    // Each slot carries the frame that last wrote it. A slot is rewritten only when the caller says that frame has
    // finished on the GPU; otherwise the frame fails to the 0.0.8.1 path rather than overwriting descriptors a
    // recorded dispatch still reads.
    std::vector<uint64_t> slot_frame;
    static constexpr uint64_t kNoFrame = ~uint64_t(0);   // a slot never written (frame 0 is a real frame)
    uint64_t frame_no = 0;
    uint64_t pinned = 0;   // the caller's frame number for this record (its own completion watermark's unit)
    std::function<bool(uint64_t)> completed;

    // The stage's own surfaces (nr_renodx.cpp's images; `decoded` in the frame's format when it takes a typed UAV
    // store, else RGBA16F stored through float - the Vulkan stage's blit fallback, as a shader store).
    struct Tex {
        ID3D12Resource* res = nullptr;
        uint32_t w = 0, h = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };
    Tex original, proxy, neural, look_out, decoded, decoded_alt, block_mean, up_proxy, up_neural, motion, neutral_mv;
    Tex band_a, band_b, band_max_a, band_max_b;
    Tex prep_keep;              // Preprocess's (SDR): the network's input kept apart, or P
    ID3D12Resource* trace = nullptr;      // the look programs' trace slot, never read
    ID3D12Resource* prep_state = nullptr;
    bool prep_state_fresh = false;
    ID3D12Resource* lin_proxy = nullptr, * lin_neural = nullptr, * lin_mv = nullptr, * lin_depth = nullptr;
    bool decoded_storage = false;
    bool prep_was_on = false;
    Preprocess prep_last{};
    int64_t prep_metered_ns = 0;
    bool area_input = false, area_known = false, history_reset = false, recreate = false;
    struct History { uint32_t written = 0; bool valid = false, whole_edit = false; int64_t last_ns = 0; };
    struct PassLook { ID3D12Resource* history[2] = {}; History hist; };
    std::vector<PassLook> looks;
    uint32_t logged_passes = 1;
    // Passes (fast)'s images (the guided filter's quarters, the fine part, the feedback's add and history).
    Tex st_fineh, st_fbadd, st_d0, st_d1, st_t0, st_t1, st_g0, st_g1, st_f0[2], st_f1[2];
    uint32_t st_cur = 0;
    bool st_valid = false;
    bool net_failed_logged = false;
    Stage::Facts facts;
    // The v6 codec's (HDR): its work surfaces (RGBA16F at the frame's extent; work_b made when a stack first
    // needs it), the 1x1 scale and commit, Preprocess's forward-half image, the exposure texel's copy.
    Tex work0, work_a, work_b, scale, commit, work_p, exposure_1x1;
    bool commit_cleared = false;
    codec::SourceUnits units;
    NormStream norm;
    uint32_t feed = kFeedV1;
    uint32_t logged_feed = ~0u;
    Frame6 f6;


    // The resource states of ours (external ones - the frame, the guides - are transitioned by the record itself).
    std::map<ID3D12Resource*, D3D12_RESOURCE_STATES> states;
    D3D12_RESOURCE_STATES& state_of(ID3D12Resource* r) { return states.emplace(r, D3D12_RESOURCE_STATE_COMMON).first->second; }
    void ensure(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES want) {
        if (!r) return;
        auto [it, fresh] = states.emplace(r, D3D12_RESOURCE_STATE_COMMON);
        (void)fresh;
        if (it->second == want) return;
        // No COMMON shortcut: D3D12 promotes a resource out of COMMON for reads and copies (SRV, COPY_SOURCE,
        // COPY_DEST), but *not* for UNORDERED_ACCESS - a texture written as a UAV from COMMON is a state mismatch the
        // driver answers with DXGI_ERROR_INVALID_CALL, which is what this route kept dying of. Buffers follow the same
        // rules for promotion; what a buffer does not need is the texture-side per-state bookkeeping. The transition
        // below names whatever the table holds, so a stage-made resource starts at COMMON and an external one at the
        // state its caller declared.
        transition(cmd, r, it->second, want);   // journaled like every other barrier
        it->second = want;
    }
    void uav_barrier(ID3D12GraphicsCommandList* cmd) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cmd->ResourceBarrier(1, &b);
    }
    // The stage's own command journal (nr-diag\run.txt): what it recorded for a frame, in order. A barrier whose
    // StateBefore is not the resource's real state, or a copy outside a resource, is an invalid call the driver answers
    // by removing the device, and nothing else in the log says which one it was.
    bool journal = false;
    uint32_t frames_recorded = 0;
    void jlog(const char* fmt, ...) {
        if (!journal) return;
        char line[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(line, sizeof line, fmt, ap);
        va_end(ap);
        nr::pe::capture::schedule(line);
    }

    void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                    D3D12_RESOURCE_STATES to) {
        jlog("renodx barrier %p %u -> %u%s", static_cast<void*>(r), static_cast<unsigned>(from), static_cast<unsigned>(to),
             from == to ? " (skipped: already there)" : "");
        // A transition to the state the resource is already in is not the no-op it looks like: D3D12 rejects the
        // barrier and the driver answers the list with DXGI_ERROR_INVALID_CALL (the frame's last command here).
        if (from == to) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        cmd->ResourceBarrier(1, &b);
        states[r] = to;   // the table follows every explicit transition, external resources included
    }

    Tex make_tex(uint32_t w, uint32_t h, DXGI_FORMAT f, bool uav, std::string* why) {
        Tex t;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = f; rd.SampleDesc.Count = 1;
        if (uav) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                   nullptr, IID_PPV_ARGS(&t.res))) || !t.res) {
            if (why) *why = "a stage texture could not be made";
            return {};
        }
        t.w = w; t.h = h; t.format = f;
        states[t.res] = D3D12_RESOURCE_STATE_COMMON;
        return t;
    }
    // A host-visible buffer for a test upload (the proxy replay): mapped by the caller, never read on the GPU.
    ID3D12Resource* upload_buffer(ID3D12Device* dev, uint64_t bytes) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* r = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, IID_PPV_ARGS(&r)))) return nullptr;
        return r;
    }
    // This frame's slot budget, from its configuration (a pass is tens of dispatches: the stage's encode/decode,
    // look and fast programs), grown here before the frame's first command. Not an admission cap: the heap simply
    // keeps the frames' slots larger than any one frame needs.
    UINT frame_budget = 128;
    UINT frame_used = 0;
    bool reserve_frame_slots() {
        for (int tries = 0; tries < 12; ++tries) {
            if (slot_frame.size() != slots) slot_frame.assign(slots, kNoFrame);
            // The run must be within one wrap: a heap smaller than the budget cannot carry a frame.
            bool free_run = slots >= frame_budget;
            for (UINT i = 0; i < frame_budget && free_run; ++i) {
                const UINT at = (slot + i) % slots;
                if (slot_frame[at] != kNoFrame && !completed(slot_frame[at])) free_run = false;
            }
            if (free_run) return true;
            if (!grow_slots()) return false;
        }
        return false;
    }
    // A new shader-visible heap of twice the slots, its clear descriptors made, the old one retired through the
    // caller's watermark (a recorded dispatch may still read it).
    bool grow_slots() {
        const UINT grown = slots * 2;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = grown * 20 + 2;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12DescriptorHeap* nh = nullptr;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&nh))) || !nh) return false;
        const UINT ni = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ID3D12DescriptorHeap* old = heap;
        const UINT old_slots = slots;
        heap = nh;
        inc = ni;
        slots = grown;
        kClearSlot = grown * 20;
        // The prep state's clear descriptors of the new heap, as creation made them.
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Buffer.NumElements = 1;
        d.Buffer.StructureByteStride = 16;
        if (prep_state) device->CreateUnorderedAccessView(prep_state, nullptr, &d, main_cpu(kClearSlot));
        slot_frame.assign(slots, kNoFrame);
        slot = 0;
        nr::pe::log("[nr] RenoDX stage: descriptor heap grown to %u slots (was %u; the old one retires on completion)",
                    grown, old_slots);
        if (old) {
            if (retire) retire(old);
        }
        return true;
    }
    ID3D12Resource* make_buffer(uint64_t bytes, std::string* why) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* r = nullptr;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                   nullptr, IID_PPV_ARGS(&r))) || !r) {
            if (why) *why = "a stage buffer could not be made";
            return nullptr;
        }
        states[r] = D3D12_RESOURCE_STATE_COMMON;
        return r;
    }
    // A replaced surface may still be read by a recorded frame another list has not run yet, so the release goes
    // through the caller's completion watermark when it gave one; `states` forgets it either way.
    std::function<void(IUnknown*)> retire;
    // False when a surface this frame needed could not be made: the frame must not be reported as processed (its
    // output may not have been written). A frame with NR off is a legitimate copy-through and stays true.
    bool record_ok_ = true;
    bool net_ran_ = false;   // the network was actually called and answered for this frame
    ID3D12Resource* replay_buf_ = nullptr;   // test-only: the uploaded network input (NR_TEST_PROXY_REPLAY)
    uint32_t replay_frame_ = 0;              // which frame's file the replay takes next
    void drop(Tex& t) {
        if (!t.res) return;
        states.erase(t.res);
        if (retire) retire(t.res); else release(t.res);
        t = {};
    }
    void drop_buf(ID3D12Resource*& r) {
        if (!r) return;
        states.erase(r);
        if (retire) retire(r); else release(r);
        r = nullptr;
    }

    // One descriptor slot: a texture (SRV or UAV) or a buffer (structured float4 or raw uints, read or written).
    struct View {
        ID3D12Resource* res = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        bool write = false, buffer = false, raw = false;
        uint64_t bytes = 0;
    };
    static View tv(const Tex& t, bool write) {
        View v;
        v.res = t.res; v.fmt = write ? uav_format_of(t.format) : srv_format_of(t.format); v.write = write;
        return v;
    }
    static View bv(ID3D12Resource* r, uint64_t bytes, bool raw, bool write) {
        View v;
        v.res = r; v.buffer = true; v.raw = raw; v.bytes = bytes; v.write = write;
        return v;
    }
    // HDR resolves pass k into the work images, SDR into the decoded ones.
    Tex& stack_out(uint32_t pass) { return hdr ? ((pass & 1u) ? work_b : work_a) : ((pass & 1u) ? decoded_alt : decoded); }

    // ClearUnorderedAccessView* reads its GPU handle from the heap bound right now and its CPU handle from a heap the
    // driver reads beside it (D3D12's signature: ViewGPUHandleInCurrentHeap, ViewCPUHandle). The dispatch slots rotate,
    // so the clears get two slots of their own after them: a recorded clear must keep the view it was given.
    // The prep state's clear descriptors sit after the dispatch slots of this heap (a member: a grown heap moves them).
    D3D12_CPU_DESCRIPTOR_HANDLE main_cpu(UINT i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * inc;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE main_gpu(UINT i) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += UINT64(i) * inc;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE clear_cpu(UINT i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = clear_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * inc;
        return h;
    }
    void bind_heap(ID3D12GraphicsCommandList* cmd) const {
        ID3D12DescriptorHeap* heaps[1] = {heap};
        cmd->SetDescriptorHeaps(1, heaps);
    }
    // One dispatch. `srv` / `uav` hold the register-numbered slots (t0.. / u0..); a null slot takes the first
    // non-null one's view (a binding the stage leaves out is one the program never reads, and D3D12 never
    // validates an unaccessed descriptor).
    // NR_STAGE_DISPATCH_LIMIT=<n>: record only the first n dispatches of a run (0/unset: all). Bisecting which one
    // loses the device offline, without the game and without guessing.
    static uint32_t dispatch_limit() {
        static const uint32_t v = [] {
            char b[16] = {};
            const DWORD n = GetEnvironmentVariableA("NR_STAGE_DISPATCH_LIMIT", b, sizeof b);
            return n && n < sizeof b ? uint32_t(std::atoi(b)) : 0u;
        }();
        return v;
    }
    uint32_t dispatches_done = 0;
    void dispatch(ID3D12GraphicsCommandList* cmd, P p, View srv[8], View uav[14], const void* push,
                  uint32_t dwords, uint32_t gx, uint32_t gy) {
        // NR_STAGE_ONLY=<program>: record only that program's dispatches (bisecting a single one in isolation).
        static const int only = [] {
            char b[16] = {};
            const DWORD n = GetEnvironmentVariableA("NR_STAGE_ONLY", b, sizeof b);
            return n && n < sizeof b ? std::atoi(b) : -1;
        }();
        if (only >= 0 && int(p) != only) {
            jlog("renodx dispatch skipped (only %d): program %u", only, static_cast<unsigned>(p));
            return;
        }
        const uint32_t limit = dispatch_limit();
        if (limit && dispatches_done >= limit) {
            jlog("renodx dispatch skipped (limit %u): program %u", limit, static_cast<unsigned>(p));
            return;
        }
        ++dispatches_done;
        const bool codec = codec_sig(p);
        const UINT n_srv = codec ? 6 : 8, n_uav = codec ? 14 : 8;
        // The slot first, on the heap record() reserved the frame's whole budget on: no growth happens here, so no
        // dispatch can be dropped or written into a heap that was replaced after it was bound.
        if (frame_used++ >= frame_budget) {
            record_ok_ = false;
            nr::pe::log("[nr] RenoDX stage: a frame needs more than its %u-slot budget; failing it", frame_budget);
            return;
        }
        UINT take = slot % slots;
        slot_frame[take] = pinned;
        slot = (slot + 1) % slots;
        bind_heap(cmd);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(take) * 20 * inc;
        gpu.ptr += UINT64(take) * 20 * inc;
        const auto fill = [&](View* views, UINT n, bool write) {
            View first{};
            for (UINT i = 0; i < n; ++i)
                if (views[i].res) { first = views[i]; break; }
            for (UINT i = 0; i < n; ++i) {
                View v = views[i].res ? views[i] : first;
                if (!v.res) continue;
                if (v.buffer) {
                    ensure(cmd, v.res, write ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                             : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
                    if (v.write) {
                        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
                        d.Format = DXGI_FORMAT_UNKNOWN;
                        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                        if (v.raw) {
                            d.Buffer.NumElements = UINT(v.bytes / 4);
                            d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                        } else {
                            d.Buffer.NumElements = UINT(v.bytes / 16);
                            d.Buffer.StructureByteStride = 16;
                        }
                        h.ptr += SIZE_T(n_srv + i) * inc;
                        device->CreateUnorderedAccessView(v.res, nullptr, &d, h);
                    } else {
                        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
                        d.Format = DXGI_FORMAT_UNKNOWN;
                        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        if (v.raw) {
                            d.Buffer.NumElements = UINT(v.bytes / 4);
                            d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                        } else {
                            d.Buffer.NumElements = UINT(v.bytes / 16);
                            d.Buffer.StructureByteStride = 16;
                        }
                        h.ptr += SIZE_T(i) * inc;
                        device->CreateShaderResourceView(v.res, &d, h);
                    }
                    continue;
                }
                ensure(cmd, v.res, write ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                         : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (v.write) {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
                    d.Format = v.fmt;
                    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
                    h.ptr += SIZE_T(n_srv + i) * inc;
                    device->CreateUnorderedAccessView(v.res, nullptr, &d, h);
                } else {
                    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
                    d.Format = v.fmt;
                    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    d.Texture2D.MipLevels = 1;
                    D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
                    h.ptr += SIZE_T(i) * inc;
                    device->CreateShaderResourceView(v.res, &d, h);
                }
            }
        };
        fill(srv, n_srv, false);
        fill(uav, n_uav, true);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
        gpu_uav.ptr += UINT64(n_srv) * inc;
        cmd->SetComputeRootSignature(codec ? rs_codec : rs_aux);
        cmd->SetPipelineState(pso[p]);
        cmd->SetComputeRoot32BitConstants(0, dwords, push, 0);
        cmd->SetComputeRootDescriptorTable(1, gpu);
        cmd->SetComputeRootDescriptorTable(2, gpu_uav);
        if (journal) {
            for (UINT i = 0; i < n_srv; ++i)
                if (srv[i].res) {
                    const D3D12_RESOURCE_DESC d = srv[i].res->GetDesc();
                    jlog("  srv%u res %p buffer %d bytes %llu fmt %d | res fmt %d %llux%u flags 0x%X state %u", i,
                         static_cast<void*>(srv[i].res), srv[i].buffer ? 1 : 0,
                         static_cast<unsigned long long>(srv[i].bytes), static_cast<int>(srv[i].fmt),
                         static_cast<int>(d.Format), static_cast<unsigned long long>(d.Width), static_cast<unsigned>(d.Height),
                         static_cast<unsigned>(d.Flags),
                         static_cast<unsigned>(states.count(srv[i].res) ? states[srv[i].res] : 0));
                }
            for (UINT i = 0; i < n_uav; ++i)
                if (uav[i].res) {
                    const D3D12_RESOURCE_DESC d = uav[i].res->GetDesc();
                    jlog("  uav%u res %p buffer %d bytes %llu fmt %d | res fmt %d %llux%u flags 0x%X state %u", i,
                         static_cast<void*>(uav[i].res), uav[i].buffer ? 1 : 0,
                         static_cast<unsigned long long>(uav[i].bytes), static_cast<int>(uav[i].fmt),
                         static_cast<int>(d.Format), static_cast<unsigned long long>(d.Width), static_cast<unsigned>(d.Height),
                         static_cast<unsigned>(d.Flags),
                         static_cast<unsigned>(states.count(uav[i].res) ? states[uav[i].res] : 0));
                }
        }
        jlog("renodx dispatch p %u pso %p rs %p srv %u uav %u grid %ux%u push %u dwords", static_cast<unsigned>(p),
             static_cast<void*>(pso[p]), static_cast<void*>(codec ? rs_codec : rs_aux), n_srv, n_uav, gx, gy, dwords);
        cmd->Dispatch(gx, gy, 1);
        uav_barrier(cmd);
    }

    bool typed_uav(DXGI_FORMAT f) const {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
        fs.Format = f;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs))) return false;
        return (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
               (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    }

    bool build(std::string* why);
    void run_look(ID3D12GraphicsCommandList* cmd, uint32_t pass, Tex& ref, const look::Settings& ls, bool motion_copied,
                  const Frame& fr, bool reset, bool& shaped, bool& transport);
    void copy_frame(ID3D12GraphicsCommandList* cmd, const Frame& f);
    uint32_t record(ID3D12GraphicsCommandList* cmd, const Frame& f, const Frame::Net& net);

    // Passes (fast)'s workset (nr_renodx.cpp's ensure_st, the images only - the programs are built at start).
    void ensure_st(std::string* why) {
        const uint32_t nw = cfg.nr_width, nh = cfg.nr_height;
        const uint32_t lw = (nw + 3) / 4, lh = (nh + 3) / 4;
        if (st_fineh.res && (st_fineh.w != nw || st_fineh.h != nh)) {
            for (Tex* t : {&st_fineh, &st_fbadd, &st_d0, &st_d1, &st_t0, &st_t1, &st_g0, &st_g1, &st_f0[0], &st_f0[1],
                           &st_f1[0], &st_f1[1]})
                drop(*t);
            st_valid = false;
        }
        if (!st_fineh.res) {
            st_fineh = make_tex(nw, nh, DXGI_FORMAT_R16G16_FLOAT, true, why);
            for (Tex* t : {&st_d0, &st_d1, &st_t0, &st_t1, &st_g0, &st_g1})
                *t = make_tex(lw, lh, DXGI_FORMAT_R32G32B32A32_FLOAT, true, why);
            st_valid = false;
        }
    }
    void ensure_st_feedback(std::string* why) {
        const uint32_t nw = cfg.nr_width, nh = cfg.nr_height;
        const uint32_t lw = (nw + 3) / 4, lh = (nh + 3) / 4;
        if (st_fbadd.res) return;
        st_fbadd = make_tex(nw, nh, DXGI_FORMAT_R16G16B16A16_FLOAT, true, why);
        for (Tex* t : {&st_f0[0], &st_f0[1], &st_f1[0], &st_f1[1]})
            *t = make_tex(lw, lh, DXGI_FORMAT_R32G32B32A32_FLOAT, true, why);
        st_valid = false;
    }
    // The guided filter's rest (st_gf): the box mean, then the coefficients of the means.
    void st_gf(ID3D12GraphicsCommandList* cmd, Tex& outa, Tex& outb, uint32_t radius, float eps) {
        const uint32_t nw = cfg.nr_width, nh = cfg.nr_height;
        struct { uint32_t low[2], radius, coef; float eps; } g{{(nw + 3) / 4, (nh + 3) / 4}, radius, 1u, eps};
        const uint32_t gx = (g.low[0] + 15) / 16, gy = (g.low[1] + 15) / 16;
        View srv[8] = {};
        srv[0] = tv(st_d0, false); srv[1] = tv(st_d1, false);
        View uav[14] = {};
        uav[4] = tv(outa, true); uav[5] = tv(outb, true);
        dispatch(cmd, kStBox, srv, uav, &g, sizeof g / 4, gx, gy);
        g.coef = 0u;
        srv[0] = tv(st_t0, false); srv[1] = tv(st_t1, false);
        uav[4] = tv(outa, true); uav[5] = tv(outb, true);
        dispatch(cmd, kStBox, srv, uav, &g, sizeof g / 4, gx, gy);
    }
};

bool Stage::Impl::build(std::string* why) {
    // The codec root signature (RenoDX's own layout): 40 root constants at b0, reads t0..t5, writes u0..u13.
    {
        D3D12_DESCRIPTOR_RANGE rsrv{}, ruav{};
        rsrv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rsrv.NumDescriptors = 6;
        ruav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ruav.NumDescriptors = 14;
        D3D12_ROOT_PARAMETER p[3]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants.ShaderRegister = 0;
        p[0].Constants.Num32BitValues = 40;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1;
        p[1].DescriptorTable.pDescriptorRanges = &rsrv;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 1;
        p[2].DescriptorTable.pDescriptorRanges = &ruav;
        for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 3; rd.pParameters = p;
        ID3DBlob *blob = nullptr, *errors = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        release(errors);
        if (FAILED(hr) || !blob) { *why = "renodx: D3D12SerializeRootSignature(codec) failed"; return false; }
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs_codec));
        release(blob);
        if (FAILED(hr)) { *why = "renodx: CreateRootSignature(codec) failed"; return false; }
    }
    // The aux one (kFastShaders' layout): the same constants, reads t0..t7, writes u0..u7, one linear-clamp sampler.
    {
        D3D12_DESCRIPTOR_RANGE rsrv{}, ruav{};
        rsrv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rsrv.NumDescriptors = 8;
        ruav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ruav.NumDescriptors = 8;
        D3D12_ROOT_PARAMETER p[3]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants.ShaderRegister = 0;
        p[0].Constants.Num32BitValues = 40;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1;
        p[1].DescriptorTable.pDescriptorRanges = &rsrv;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 1;
        p[2].DescriptorTable.pDescriptorRanges = &ruav;
        for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC ss{};
        ss.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;   // to_motion's sampler: the frame path's linear clamp
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.MaxLOD = 0.0f;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 3; rd.pParameters = p;
        rd.NumStaticSamplers = 1; rd.pStaticSamplers = &ss;
        ID3DBlob *blob = nullptr, *errors = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        release(errors);
        if (FAILED(hr) || !blob) { *why = "renodx: D3D12SerializeRootSignature(aux) failed"; return false; }
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs_aux));
        release(blob);
        if (FAILED(hr)) { *why = "renodx: CreateRootSignature(aux) failed"; return false; }
    }
    // RenoDX's own programs, from the installed folder (their #include resolved there, and beside it for the prep
    // resolve's - the HDR builds).
    FolderInclude includer;
    includer.dirs = {cfg.shaders, cfg.shaders + L"\\renodx-prep"};
    struct File { P p; const char* name; const char* macro; };
    const File files[] = {
        {kEncodeL, "legacy_encode", nullptr}, {kDecodeL, "legacy_decode", nullptr},
        {kReduceL, "legacy_reduce", nullptr}, {kReduceLU, "legacy_reduce_unshaped", nullptr},
        {kApplyL, "legacy_apply", nullptr}, {kDecodeReduceL, "legacy_decode_reduce", nullptr},
        {kCompose, "look_compose", nullptr}, {kUpsample, "look_upsample", nullptr}, {kBand, "look_band", nullptr},
    };
    const File files6[] = {
        {kLinearize, "v6_linearize", nullptr}, {kAutoscale, "v6_autoscale", nullptr},
        {kExposureScale, "v6_exposure_scale", nullptr}, {kEncode6, "v6_encode", nullptr},
        {kResolve6, "v6_resolve", nullptr}, {kReduce6, "v6_pedestal_reduce", nullptr},
        {kReduce6U, "v6_pedestal_reduce_unshaped", nullptr}, {kCommit6, "v6_commit", nullptr},
        {kCommitExposure, "v6_commit_exposure", nullptr}, {kResolveCommit6, "v6_resolve_commit", nullptr},
        {kPrepEncode, "renodx-prep\\prep_encode", nullptr},   // ours (renodx_prep_encode.comp, as HLSL)
        // the resolve when [Preprocess] made the proxy (v6_common.hlsli under NR_PREP_RESOLVE)
        {kResolve6Prep, "v6_resolve", "NR_PREP_RESOLVE"},
        {kResolveCommit6Prep, "v6_resolve_commit", "NR_PREP_RESOLVE"},
        {kReduce6UPrep, "v6_pedestal_reduce_unshaped", "NR_PREP_RESOLVE"},
    };
    const auto make_file_pso = [&](const File& f) -> bool {
        const std::string text = read_text(cfg.shaders + L"\\" +
            std::wstring(f.name, f.name + std::strlen(f.name)) + (f.p == kPrepEncode ? L".hlsl" : L".cs_5_1.hlsl"));
        if (text.empty()) { *why = std::string("renodx: ") + f.name + " is missing"; return false; }
        // Probe builds (both routes compile this same file): NR_ENCODE_PROBE_IMMEDIATE stores the value entering the
        // encode rather than the encoded one, so the same boundary can be compared on both sides.
        const D3D_SHADER_MACRO macros[] = {{f.macro, "1"}, {nullptr, nullptr}};
        const D3D_SHADER_MACRO probe[] = {{"NR_ENCODE_PROBE_IMMEDIATE", "1"}, {nullptr, nullptr}};
        const bool probing = f.p == kEncode6 && std::getenv("NR_ENCODE_PROBE_IMMEDIATE") != nullptr;
        ID3DBlob* code = compile_hlsl(text.data(), text.size(), "main", why, &includer,
                                      probing ? probe : (f.macro ? macros : nullptr));
        if (!code) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs_codec;
        pd.CS.pShaderBytecode = code->GetBufferPointer();
        pd.CS.BytecodeLength = code->GetBufferSize();
        const HRESULT hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso[f.p]));
        release(code);
        if (FAILED(hr) || !pso[f.p]) { *why = std::string("renodx: ") + f.name + " does not make a pipeline"; return false; }
        return true;
    };
    for (const File& f : files)
        if (!make_file_pso(f)) return false;
    if (hdr)
        for (const File& f : files6)
            if (!make_file_pso(f)) return false;
    // The inline ones: the debug views on the codec signature, the rest on the aux one.
    {
        ID3DBlob* code = compile_hlsl(kDebugShader, sizeof kDebugShader - 1, "debug_main", why, nullptr);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        if (code) {
            pd.pRootSignature = rs_codec;
            pd.CS.pShaderBytecode = code->GetBufferPointer();
            pd.CS.BytecodeLength = code->GetBufferSize();
            if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso[kDebug])))) pso[kDebug] = nullptr;
            release(code);
        }
        if (!pso[kDebug]) return false;
        std::string err;
        const struct { P p; const char* entry; } aux[] = {
            {kStFbIn, "st_fb_in"}, {kStPrep, "st_prep"}, {kStBox, "st_box"}, {kStApply, "st_apply"},
            {kPrep, "prep_main"}, {kTexToBuf, "tex_to_buf"}, {kBufToTex, "buf_to_tex"},
            {kCopyOut, "copy_out"}, {kToMotion, "to_motion"}, {kToDepth, "to_depth"},
        };
        for (const auto& a : aux) {
            ID3DBlob* blob = nullptr;
            if (a.p == kPrep) blob = compile_hlsl(kPrepShader, sizeof kPrepShader - 1, a.entry, &err, nullptr);
            else if (a.p >= kStFbIn && a.p <= kStApply)
                blob = compile_hlsl(::nr::asi::native::kFastShaders,
                                    std::strlen(::nr::asi::native::kFastShaders), a.entry, &err, nullptr);
            else blob = compile_hlsl(kAuxShaders, sizeof kAuxShaders - 1, a.entry, &err, nullptr);
            if (!blob) { *why = "renodx: " + err; return false; }
            D3D12_COMPUTE_PIPELINE_STATE_DESC ad{};
            ad.pRootSignature = rs_aux;
            ad.CS.pShaderBytecode = blob->GetBufferPointer();
            ad.CS.BytecodeLength = blob->GetBufferSize();
            const HRESULT hr = device->CreateComputePipelineState(&ad, IID_PPV_ARGS(&pso[a.p]));
            release(blob);
            if (FAILED(hr) || !pso[a.p]) { *why = std::string("renodx: ") + a.entry + " does not make a pipeline"; return false; }
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    // Test-only: a small first heap (NR_STAGE_SLOT_TEST=<n>) makes the completion-driven growth run in a short test;
    // the product always starts at kSlots.
    if (const char* ts = std::getenv("NR_STAGE_SLOT_TEST"); ts && ts[0]) {
        const long n = std::strtol(ts, nullptr, 10);
        if (n > 0 && n < 1 << 20) { slots = UINT(n); kClearSlot = slots * 20; }
    }
    hd.NumDescriptors = slots * 20 + 2;   // + the two clear slots (kClearSlot)
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) { *why = "renodx: the descriptor heap failed"; return false; }
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hd.NumDescriptors = 2;   // one per cleared resource, beside the GPU ones
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&clear_heap)))) { *why = "renodx: the clear heap failed"; return false; }
    Impl::inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    const uint32_t W = cfg.width, H = cfg.height, NW = cfg.nr_width, NH = cfg.nr_height;
    const DXGI_FORMAT frame = cfg.frame_format;
    original = make_tex(W, H, frame, false, why);
    decoded_storage = typed_uav(frame);
    decoded = make_tex(W, H, decoded_storage ? frame : DXGI_FORMAT_R16G16B16A16_FLOAT, true, why);
    // Probe only (NR_ENCODE_PROBE_F32): the network's proxy in RGBA32F, so the value entering the encode is read
    // back without the half store. The product keeps RGBA16F (this is a test switch, not a precision change).
    const bool probe_f32 = std::getenv("NR_ENCODE_PROBE_F32") != nullptr;
    proxy = make_tex(NW, NH, probe_f32 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, true, why);
    neural = make_tex(NW, NH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, why);
    look_out = make_tex(NW, NH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, why);
    block_mean = make_tex((W + 31) / 32, (H + 31) / 32, DXGI_FORMAT_R32G32B32A32_FLOAT, true, why);
    if (NW < W || NH < H) {
        up_proxy = make_tex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true, why);
        up_neural = make_tex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true, why);
    }
    if (!original.res || !decoded.res || !proxy.res || !neural.res || !look_out.res || !block_mean.res ||
        ((NW < W || NH < H) && (!up_proxy.res || !up_neural.res))) {
        *why = "renodx: a stage texture could not be made";
        return false;
    }
    trace = make_buffer(16 * 1024, why);
    lin_proxy = make_buffer(uint64_t(NW) * NH * 16, why);
    lin_neural = make_buffer(uint64_t(NW) * NH * 16, why);
    lin_mv = make_buffer(uint64_t(NW) * NH * 16, why);
    lin_depth = make_buffer(uint64_t(NW) * NH * 16, why);
    if (!trace || !lin_proxy || !lin_neural || !lin_mv || !lin_depth) return false;
    neutral_mv = make_tex(1, 1, DXGI_FORMAT_R16G16_FLOAT, false, why);   // zero: the heap guarantees it
    if (!neutral_mv.res) return false;
    looks.resize(1);
    if (hdr) {
        // The v6 codec's surfaces: the work images RGBA16F like the proxy, the scale and the commit 1x1 RGBA32F.
        work0 = make_tex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true, why);
        work_a = make_tex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true, why);
        scale = make_tex(1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, true, why);
        commit = make_tex(1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, true, why);
        if (!work0.res || !work_a.res || !scale.res || !commit.res) {
            *why = "renodx: a v6 surface could not be made";
            return false;
        }
    }
    nr::pe::log("[nr] RenoDX stage (D3D12): %ux%u (network %ux%u), frame format %u%s", W, H, NW, NH,
                unsigned(frame), decoded_storage ? "" : ", result through RGBA16F");
    return true;
}

// RunLookStage (nr_renodx.cpp's run_look, the parts the SDR path uses): the bands, the temporal filter on the
// pass's own history, every gain into look_out (N'), then, below the frame's extent, P_up and N'_up.
void Stage::Impl::run_look(ID3D12GraphicsCommandList* cmd, uint32_t pass, Tex& ref, const look::Settings& ls,
                           bool motion_copied, const Frame& fr, bool reset, bool& shaped_out, bool& transport_out) {
    const uint32_t W = cfg.width, H = cfg.height, NW = cfg.nr_width, NH = cfg.nr_height;
    transport_out = up_proxy.res && look::transport_wanted(ls, NW, NH, W, H);
    shaped_out = look::dispatched(ls, transport_out);
    PassLook& pl = looks[pass];
    const bool temporal = shaped_out && ls.stabilize != 0;
    if (!temporal) {
        pl.hist.valid = false;
        if (pl.history[0]) { drop_buf(pl.history[0]); drop_buf(pl.history[1]); }
    }
    if (!shaped_out) return;
    if (look::bands_wanted(ls) && !band_a.res) {
        const uint32_t bw = (NW + 1) / 2, bh = (NH + 1) / 2;
        std::string why;
        band_a = make_tex(bw, bh, DXGI_FORMAT_R32G32B32A32_FLOAT, true, &why);
        band_b = make_tex(bw, bh, DXGI_FORMAT_R32G32B32A32_FLOAT, true, &why);
        band_max_a = make_tex(bw, bh, DXGI_FORMAT_R32G32_FLOAT, true, &why);
        band_max_b = make_tex(bw, bh, DXGI_FORMAT_R32G32_FLOAT, true, &why);
        if (!band_a.res || !band_b.res || !band_max_a.res || !band_max_b.res) return;
    }
    if (temporal && !pl.history[0]) {
        pl.history[0] = make_buffer(uint64_t(NW) * NH * 12, nullptr);
        pl.history[1] = make_buffer(uint64_t(NW) * NH * 12, nullptr);
        if (!pl.history[0] || !pl.history[1]) { pl.history[0] = pl.history[1] = nullptr; return; }
        pl.hist = {};
    }
    const int64_t now = now_ns();
    look::Frame lf;
    lf.nr_width = NW; lf.nr_height = NH; lf.full_width = W; lf.full_height = H;
    lf.reference_linear = hdr != 0;
    lf.transport = transport_out;
    const bool whole_edit = ls.stabilize_alpha > 0.f;
    if (temporal) {
        const float gap = pl.hist.last_ns != 0 ? (now - pl.hist.last_ns) / 1e9f : 0.f;
        const bool restart = !pl.hist.valid || reset || pl.hist.whole_edit != whole_edit ||
                             (!whole_edit && gap > look::kHistoryMaxGapSeconds);
        lf.history_valid = !restart;
        lf.frame_seconds = pl.hist.last_ns != 0 ? std::clamp(gap, 1.f / 240.f, look::kHistoryMaxGapSeconds) : 1.f / 60.f;
        if (ls.stabilize == 2u && motion_copied) {
            // DLSS motion vectors scaled to the NR grid: texel * MVScale * NR / motion extent.
            lf.motion = true;
            lf.motion_scale[0] = fr.motion_scale_x * settings.mvec_scale_x * float(NW);
            lf.motion_scale[1] = fr.motion_scale_y * settings.mvec_scale_y * float(NH);
            lf.motion_grid[0] = fr.motion_w;
            lf.motion_grid[1] = fr.motion_h;
        }
    }
    look::Settings shaping = ls;
    if (!temporal) shaping.stabilize = 0;
    look::Constants lc = look::make_constants(shaping, lf);
    Tex& mv = motion.res ? motion : original;
    const uint64_t hist_bytes = uint64_t(NW) * NH * 12;
    // The look set [ref, P, N, ref] and the writes at u0.. (binding - 4); the trace buffer at u7, the history at
    // u9 / u10 (look_common.hlsli's ByteAddressBuffers on this path).
    const auto look_set = [&](Tex& n, ID3D12Resource* hist_out, ID3D12Resource* hist_in, View (&srv)[8],
                              View (&uav)[14]) {
        srv[0] = tv(ref, false); srv[1] = tv(proxy, false); srv[2] = tv(n, false); srv[3] = tv(ref, false);
        srv[4] = tv(mv, false);
        uav[0] = tv(look_out, true);
        uav[1] = tv(band_a, true); uav[2] = tv(band_b, true);
        uav[3] = tv(band_max_a, true); uav[4] = tv(band_max_b, true);
        uav[5] = tv(up_proxy, true); uav[6] = tv(up_neural, true);
        uav[7] = bv(trace, 16 * 1024, true, true);
        if (hist_out) { uav[9] = bv(hist_out, hist_bytes, true, true); uav[10] = bv(hist_in, hist_bytes, true, true); }
    };
    // RecordCompose: the five band passes at half the network's extent, then the compose.
    ID3D12Resource* hist_out = temporal ? pl.history[pl.hist.written ^ 1u] : nullptr;
    ID3D12Resource* hist_in = temporal ? pl.history[pl.hist.written] : nullptr;
    if (lc.flags & look::kFlagBands) {
        look::Constants band = lc;
        band.size[0] = (NW + 1) / 2; band.size[1] = (NH + 1) / 2;
        View srv[8] = {}, uav[14] = {};
        look_set(neural, hist_out, hist_in, srv, uav);
        for (uint32_t mode = 0; mode < 5; ++mode) {
            band.mode = mode;
            dispatch(cmd, kBand, srv, uav, &band, sizeof band / 4, (band.size[0] + 7) / 8, (band.size[1] + 7) / 8);
        }
    }
    lc.mode = 0;
    {
        View srv[8] = {}, uav[14] = {};
        look_set(neural, hist_out, hist_in, srv, uav);
        dispatch(cmd, kCompose, srv, uav, &lc, sizeof lc / 4, (NW + 15) / 16, (NH + 15) / 16);
    }
    if (transport_out) {   // RecordUpsample: P_up and N'_up at the frame's extent
        look::Constants up = lc;
        up.size[0] = W; up.size[1] = H;
        View srv[8] = {}, uav[14] = {};
        look_set(look_out, nullptr, nullptr, srv, uav);
        dispatch(cmd, kUpsample, srv, uav, &up, sizeof up / 4, (W + 15) / 16, (H + 15) / 16);
    }
    if (temporal) {   // FinishLookPass
        pl.hist.written ^= 1u;
        pl.hist.valid = true;
        pl.hist.whole_edit = whole_edit;
        pl.hist.last_ns = now;
    }
}

// The frame as it came: colour into the output (a byte copy; the same image: nothing at all).

// The float formats v6_exposure_scale reads (.r), as RenoDX's Vulkan path lists them.
static bool exposure_format_readable(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return true;
        default:
            return false;
    }
}

void Stage::Impl::copy_frame(ID3D12GraphicsCommandList* cmd, const Frame& f) {
    if (f.colour == f.output) {
        if (f.colour_state != f.output_state)
            transition(cmd, f.colour, f.colour_state, f.output_state);
        return;
    }
    D3D12_RESOURCE_BARRIER b[2]{};
    for (auto& x : b) {
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    b[0].Transition.pResource = f.colour;
    b[0].Transition.StateBefore = f.colour_state;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = f.output;
    b[1].Transition.StateBefore = f.output_state;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    {
        const D3D12_RESOURCE_DESC cd = f.colour->GetDesc(), od = f.output->GetDesc();
        jlog("renodx frame copy: colour %p %llux%u fmt %u state %u -> output %p %llux%u fmt %u state %u",
             static_cast<void*>(f.colour), static_cast<unsigned long long>(cd.Width), static_cast<unsigned>(cd.Height),
             static_cast<unsigned>(cd.Format), static_cast<unsigned>(f.colour_state), static_cast<void*>(f.output),
             static_cast<unsigned long long>(od.Width), static_cast<unsigned>(od.Height), static_cast<unsigned>(od.Format),
             static_cast<unsigned>(f.output_state));
    }
    cmd->ResourceBarrier(2, b);
    states[f.colour] = D3D12_RESOURCE_STATE_COPY_SOURCE;
    states[f.output] = D3D12_RESOURCE_STATE_COPY_DEST;
    cmd->CopyResource(f.output, f.colour);
    for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    cmd->ResourceBarrier(2, b);
    states[f.colour] = f.colour_state;
    states[f.output] = f.output_state;
}

uint32_t Stage::Impl::record(ID3D12GraphicsCommandList* cmd, const Frame& f, const Frame::Net& net) {
    record_ok_ = true;
    net_ran_ = false;
    ++frame_no;
    // Every slot this frame may use is made free before its first command: a frame that would outrun completion
    // grows the heap here, and a frame that cannot is not recorded at all (rather than losing a dispatch halfway).
    {
        // The frame's own stack (Stage::set updates `settings`, not the construction snapshot): the passes it runs,
        // and per pass the most dispatches any branch of this stage issues: encode (1), tex_to_buf (1), buf_to_tex
        // (1), decode (legacy 1 + black-level 2 | v6 1 + resolve 1), look (compose 1, upsample 1 when it upsamples,
        // band 2 when it bands), preprocess (forward 1, inverse 1, meter 1), fast (prep 1, fb_in 1, apply 1) - 16 at
        // the most in the current code, so 32 is a factor-two margin; plus the fixed per-frame programs (copy_in,
        // copy_out, commit, pedestal, the clears) counted in the 64.
        const uint32_t passes = std::max(1u, std::min<uint32_t>(stack_passes(settings), nr::renodx::kMaxUnlockedPasses));
        frame_budget = 64 + 32 * passes;
        frame_used = 0;
    }
    if (completed && !reserve_frame_slots()) {
        record_ok_ = false;
        nr::pe::log("[nr] RenoDX stage: no slot budget for this frame (fails before any command)");
        return 0;
    }
    const Settings& s = settings;
    const uint32_t W = cfg.width, H = cfg.height, NW = cfg.nr_width, NH = cfg.nr_height;
    const uint32_t gx = (W + 15) / 16, gy = (H + 15) / 16, nx = (NW + 15) / 16, ny = (NH + 15) / 16;
    const uint32_t passes = stack_passes(s);

    if (!s.nr_enabled) {
        copy_frame(cmd, f);
        recreate = true;
        return 0;
    }
    if (recreate) history_reset = true;
    // The frame's resources are not this stage's: they arrive in the states OptiScaler's evaluate declared. Register
    // those, or the first ensure() would name COMMON after a resource that is already in UNORDERED_ACCESS (in place,
    // colour and output are the same texture, and its UAV state is what the copy below restores).
    // Every frame: the caller declares these again, and after a submission a promotion the driver decayed is not a
    // state to trust. (A pointer the caller no longer uses simply keeps its last entry; harmless and bounded.)
    if (f.colour) states[f.colour] = f.colour_state;
    if (f.output) states[f.output] = f.output_state;
    if (f.motion) states[f.motion] = f.motion_state;
    if (f.depth) states[f.depth] = f.depth_state;
    if (f.exposure) states[f.exposure] = f.exposure_state;
    journal = false;
    if (nr::pe::capture::on() && frames_recorded < 2) {
        journal = true;
        ++frames_recorded;
        const D3D12_RESOURCE_DESC cd = f.colour->GetDesc(), od = f.output->GetDesc();
        jlog("renodx frame %u: %ux%u network %ux%u passes %u hdr %d; colour %llux%u fmt %u state %u; output %llux%u fmt %u "
             "state %u", frames_recorded, W, H, NW, NH, passes, hdr ? 1 : 0,
             static_cast<unsigned long long>(cd.Width), static_cast<unsigned>(cd.Height), static_cast<unsigned>(cd.Format),
             static_cast<unsigned>(f.colour_state), static_cast<unsigned long long>(od.Width),
             static_cast<unsigned>(od.Height),
             static_cast<unsigned>(od.Format), static_cast<unsigned>(f.output_state));
    }

    // The stack's own images and look histories, as this frame's depth needs them.
    if (hdr && passes > 1 && !work_b.res) {
        std::string why;
        work_b = make_tex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true, &why);
        if (!work_b.res) { record_ok_ = false; return 0; }
    }
    if (!hdr && passes > 1 && !decoded_alt.res) {
        std::string why;
        decoded_alt = make_tex(W, H, decoded.format, true, &why);
        if (!decoded_alt.res) { record_ok_ = false; return 0; }
    }
    while (looks.size() > passes) {
        drop_buf(looks.back().history[0]);
        drop_buf(looks.back().history[1]);
        looks.pop_back();
    }
    looks.resize(passes);
    if (passes != logged_passes) {
        logged_passes = passes;
        nr::pe::log("[nr] RenoDX stage (D3D12): %u model passes a frame", passes);
    }
    {
        const bool area = s.look_upsample == look::kUpsampleAreaInput;
        if (area_known && area != area_input) history_reset = true;
        area_input = area; area_known = true;
    }

    // The v6 codec's frame snapshots (ProcessInline): one divisor, encoding, gate and cap. The Present path's own
    // divisor, from the HDR diffuse white; Anchored (1) and Display (3) keep their anchors.
    const bool v6 = hdr != 0;
    f6.hdr_mode = hdr;
    f6.units = units;
    f6.divisor = v6 ? frame_codec_divisor(s, hdr, units) : 0.f;
    if (cfg.present && v6 && s.codec_mode != 1u && s.codec_mode != 3u)
        f6.divisor = codec::PresentDivisor(units, f.present_white_nits, s.paper_white_scale, s.pq_calibration);
    f6.encoding = v6_encoding_constant(units);
    f6.gate = v6_dark_gate(units);
    f6.cap = codec::BlackPedestalCap(units);

    // Preprocess this frame (renodx_prep.comp's push block; RESET when it has just come on). On HDR its forward
    // half runs before RenoDX's encode (the running image -> work_p); on SDR it runs on the proxy, around the
    // network.
    const Preprocess& pp = f.preprocess;
    const bool prep_on = pp.active();
    if (prep_on != prep_was_on || (prep_on && !(pp == prep_last))) history_reset = true;
    PrepPush prep_push{};
    if (prep_on) {
        std::string why;
        if (hdr) {
            if (!work_p.res) work_p = make_tex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true, &why);
            if (!work_p.res) { record_ok_ = false; return 0; }
        } else {
            if (!prep_keep.res) prep_keep = make_tex(NW, NH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, &why);
            if (!prep_keep.res) { record_ok_ = false; return 0; }
        }
        if (!prep_state) {
            prep_state = make_buffer(16, &why);
            if (!prep_state) { record_ok_ = false; return 0; }
            prep_state_fresh = true;
        }
        if (prep_state_fresh) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
            d.Format = DXGI_FORMAT_UNKNOWN;
            d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            d.Buffer.NumElements = 1;
            d.Buffer.StructureByteStride = 16;
            device->CreateUnorderedAccessView(prep_state, nullptr, &d, main_cpu(kClearSlot));
            device->CreateUnorderedAccessView(prep_state, nullptr, &d, clear_cpu(0));
            ensure(cmd, prep_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);   // the clear writes it
            bind_heap(cmd);                                                   // the GPU handle's heap
            const UINT zero[4] = {};
            jlog("renodx clear prep_state %p uav slot %u (gpu handle in the bound heap)", static_cast<void*>(prep_state), kClearSlot);
            cmd->ClearUnorderedAccessViewUint(main_gpu(kClearSlot), clear_cpu(0), prep_state, zero, 0, nullptr);
            uav_barrier(cmd);
            prep_state_fresh = false;
        }
        const uint32_t curve = uint32_t(std::clamp(pp.curve, 0, 6));
        const int64_t now = now_ns();
        prep_push = {hdr ? W : NW, hdr ? H : NH, 0u,
                     (pp.exposure == 1 ? uint32_t(kPrepAuto) : 0u) | (hdr ? uint32_t(kPrepLinear) : 0u) |
                         (prep_was_on ? 0u : uint32_t(kPrepReset)) | (pp.inverse ? 0u : uint32_t(kPrepRatio)),
                     curve, pp.exposure == 0 ? 0.0f : std::clamp(pp.bias_ev, -8.0f, 8.0f),
                     std::clamp(pp.contrast, 0.5f, 2.0f), std::clamp(pp.saturation, 0.05f, 2.0f), kPrepAnchor[curve],
                     4.0f, prep_was_on ? float(now - prep_metered_ns) / 1e9f : 0.0f, 0.0f};
        prep_metered_ns = now;
    } else if (prep_keep.res || work_p.res) {   // off: its images go
        drop(prep_keep);
        drop(work_p);
    }
    prep_was_on = prep_on;
    prep_last = pp;
    facts = Stage::Facts{};
    facts.hdr_mode = hdr;
    facts.absolute = units.absolute;
    facts.prep_exposure = prep_on && pp.exposure != 0;
    // Everything a frame may need that can fail to be made is made before any command of this frame's pass is
    // recorded: a creation failure then returns with the caller's list as it came (his states came back in
    // copy_frame, nothing is bound, no half frame), instead of after bindings the next caller would inherit.
    const bool have_motion = f.motion && f.motion_w && f.motion_h;
    const look::Settings ls = look::snapshot(s, f.after_upscale && have_motion);
    const uint32_t fast_slot = std::min(s.fast_method, 2u);
    const bool fast = s.passes_mode == 1u && (s.fast_method == 3u ? s.fast_edit_strength != 1.f : s.fast_passes[fast_slot] > 1.f);
    const bool fast_motion = have_motion && f.fast_motion_available;
    const bool fast_history = s.fast_method == 1u || s.fast_method == 2u;
    const bool fast_feedback = fast && fast_history && fast_motion;
    facts.fast_passes = s.passes_mode == 1u ? (s.fast_method == 3u ? s.fast_edit_strength : s.fast_passes[fast_slot]) : 0.f;
    facts.motion = fast_motion;
    {
        std::string why;
        const bool motion_wanted = (ls.stabilize == 2u && have_motion && look::dispatched(ls, false)) || fast_feedback;
        if (motion_wanted && have_motion) {
            const D3D12_RESOURCE_DESC md = f.motion->GetDesc();
            if (motion.res && (motion.w != f.motion_w || motion.h != f.motion_h || motion.format != md.Format)) drop(motion);
            if (!motion.res) motion = make_tex(f.motion_w, f.motion_h, md.Format, false, &why);
            if (!motion.res) { record_ok_ = false; return 0; }
        }
        if (hdr && f.exposure) {
            const DXGI_FORMAT ef = f.exposure->GetDesc().Format;
            const bool readable = exposure_format_readable(ef);
            if (readable && select_feed(s, hdr, units, readable, f.auto_exposure || !f.create_flags_known) == kFeedTexture) {
                if (!exposure_1x1.res || exposure_1x1.format != ef) {
                    drop(exposure_1x1);
                    exposure_1x1 = make_tex(1, 1, ef, false, &why);
                }
                if (!exposure_1x1.res) { record_ok_ = false; return 0; }
            }
        }
    }

    // HDR with Preprocess: the resolve's ratio is taken behind Preprocess's curve (the _prep programs); with its
    // Inverse off, in the picture the network was shown (RenoDX's own resolve).
    const bool prep_exposure = prep_on && pp.exposure != 0;
    const bool ratio_resolve = prep_on && hdr && !frame_display_codec(s, hdr, units);
    // Without Preprocess's exposure (HDR) RenoDX's own exposure stays: Preprocess's forward half divides by
    // RenoDX's divisor (or its same-frame scale) and the encode then divides by 1.
    const bool prep_normalise = prep_on && hdr && !prep_exposure;
    if (prep_normalise) {
        prep_push.flags |= kPrepNormalise;
        prep_push.divisor = f6.divisor > 0.f ? f6.divisor : 0.f;
    }
    if (prep_exposure && hdr) f6.divisor = 1.f;
    // Curve = none keeps v6_encode. Tell the inverse which codec it applied, rather than treating it as identity.
    // Internal curve IDs 7 / 8 are the display toe+shoulder / classic shoulder, not Preprocess menu choices.
    const uint32_t prep_inverse_curve = prep_push.curve != 0u ? prep_push.curve
        : frame_display_codec(s, hdr, units) ? 7u : 8u;
    const float floor_behind = ratio_resolve ? prep_floor(prep_inverse_curve, prep_push.k, prep_push.contrast,
                                                          prep_push.saturation) : 0.f;
    const auto prep_constants = [&](const V6Constants& v) {
        V6PrepConstants c{v, prep_push.contrast, prep_push.saturation, floor_behind};
        c.v.padding[0] = prep_inverse_curve;
        std::memcpy(&c.v.padding[1], &prep_push.k, sizeof(float));
        return c;
    };
    // SDR Preprocess with the network's input in its own image (NR_PREP_APART, the Vulkan stage's default).
    static const bool apart_on = [] { const char* e = std::getenv("NR_PREP_APART"); return !(e && e[0] == '0'); }();
    const bool prep_apart = apart_on && prep_on && !hdr;
    // The prep scale (binding 4): RenoDX's same-frame scale texel on HDR, an image of the stage's that is never
    // read on SDR.
    const View prep_scale_view = units.absolute ? tv(work0, false) : tv(scale, false);
    // Preprocess's forward half (HDR): the pass's running image -> work_p, its meter first on the frame.
    const auto prep_forward_hdr = [&](Tex& from, bool meter) {
        View srv[8] = {};
        srv[0] = prep_scale_view;
        View uav[14] = {};
        uav[0] = bv(prep_state, 16, false, true);
        uav[1] = tv(from, true);
        uav[2] = tv(work_p, true);
        uav[3] = tv(work_p, true);
        PrepPush push = prep_push;
        if (meter && pp.exposure == 1) {
            PrepPush m = push;
            dispatch(cmd, kPrep, srv, uav, &m, sizeof m / 4, 1, 1);
        }
        push.mode = 1;
        dispatch(cmd, kPrep, srv, uav, &push, sizeof push / 4, gx, gy);
    };


    // Passes (fast) this frame (nr_renodx.cpp's planning): the decisions above, only their line.
    facts.fast_fallback = fast && fast_history && !fast_motion;
    if (!fast && st_fineh.res) {   // Passes (fast) off: its images go (nr_renodx.cpp's retire)
        for (Tex* t : {&st_fineh, &st_fbadd, &st_d0, &st_d1, &st_t0, &st_t1, &st_g0, &st_g1, &st_f0[0], &st_f0[1],
                       &st_f1[0], &st_f1[1]})
            drop(*t);
        st_valid = false;
    }

    // The game's motion vectors, for Detail stability: the subrect copied to the origin of an image of ours
    // (the Vulkan stage's copy; its in-place read is scheduling only).
    bool motion_copied = false;
    const bool motion_wanted = (ls.stabilize == 2u && have_motion && look::dispatched(ls, false)) || fast_feedback;
    if (motion_wanted && have_motion) {
        const D3D12_RESOURCE_DESC md = f.motion->GetDesc();
        transition(cmd, f.motion, f.motion_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ensure(cmd, motion.res, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = f.motion;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        dst.pResource = motion.res;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        // The caller reports the region; a box outside the source, or an empty one, is not a copy D3D12 accepts.
        const uint32_t mx = std::min(f.motion_x, uint32_t(md.Width)), my = std::min(f.motion_y, uint32_t(md.Height));
        const uint32_t mw = std::min(f.motion_w, uint32_t(md.Width) - mx), mh = std::min(f.motion_h, uint32_t(md.Height) - my);
        D3D12_BOX box{};
        box.left = mx;
        box.top = my;
        box.right = mx + mw;
        box.bottom = my + mh;
        box.back = 1;
        if (!mw || !mh) {
            // An empty or out-of-bounds region is not a copy D3D12 accepts: skip it and let the look run without motion.
            static bool said = false;
            if (!said) { said = true; nr::pe::log("[nr] RenoDX stage (D3D12): the motion region is outside the guide; the look runs without motion"); }
        } else {
            jlog("renodx motion copy: box %u,%u..%u,%u of source %llux%u into %ux%u", box.left, box.top, box.right,
                 box.bottom, static_cast<unsigned long long>(md.Width), static_cast<unsigned>(md.Height), motion.w, motion.h);
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
            motion_copied = true;
        }
        ensure(cmd, motion.res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(cmd, f.motion, D3D12_RESOURCE_STATE_COPY_SOURCE, f.motion_state);
    }
    const bool recreate_epoch = recreate || history_reset;   // the governor's reset-epoch snap (PlanFrameScale)
    const bool reset_all = f.reset || history_reset;
    if (reset_all || !fast_feedback) st_valid = false;
    history_reset = false;
    recreate = false;

    // The network's guides, onto the network's extent (once a frame): the motion through the frame path's own
    // linear sample, the depth texel for texel (to_motion / to_depth).
    {
        struct { uint32_t w, h, what, pad; float region[4]; } c{NW, NH, 0, 0, {1.f, 1.f, 0.f, 0.f}};
        if (have_motion) {
            const D3D12_RESOURCE_DESC md = f.motion->GetDesc();
            const float mtw = float(md.Width), mth = float(md.Height);
            c.region[0] = float(f.motion_w) / mtw;
            c.region[1] = float(f.motion_h) / mth;
            c.region[2] = float(f.motion_x) / mtw;
            c.region[3] = float(f.motion_y) / mth;
            View srv[8] = {};
            srv[0].res = f.motion;
            srv[0].fmt = srv_format_of(md.Format);
            srv[0].write = false;
            View uav[14] = {};
            uav[0] = bv(lin_mv, uint64_t(NW) * NH * 16, false, true);
            // the guide is an external resource: its state around the dispatch, by hand
            transition(cmd, f.motion, f.motion_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            states[f.motion] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;   // (what the dispatch's ensure holds)
            dispatch(cmd, kToMotion, srv, uav, &c, 4, (NW + 7) / 8, (NH + 7) / 8);
            transition(cmd, f.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.motion_state);
            states[f.motion] = f.motion_state;
        } else {
            View srv[8] = {};
            srv[0] = tv(neutral_mv, false);
            View uav[14] = {};
            uav[0] = bv(lin_mv, uint64_t(NW) * NH * 16, false, true);
            dispatch(cmd, kToMotion, srv, uav, &c, 4, (NW + 7) / 8, (NH + 7) / 8);
        }
    }
    const bool net_wants_depth = have_motion && f.depth && !f.motion_without_depth;
    if (net_wants_depth) {
        struct { uint32_t w, h, what, pad; float region[4]; } c{NW, NH, 0, 0, {1.f, 1.f, 0.f, 0.f}};
        const D3D12_RESOURCE_DESC dd = f.depth->GetDesc();
        if (f.depth_x >= dd.Width || f.depth_y >= dd.Height) {
            c.region[0] = float(dd.Width);
            c.region[1] = float(dd.Height);
            c.region[2] = 0.f;
            c.region[3] = 0.f;
        } else {
            const uint32_t max_w = uint32_t(dd.Width) - f.depth_x;
            const uint32_t max_h = uint32_t(dd.Height) - f.depth_y;
            c.region[0] = float(f.depth_w ? std::min(f.depth_w, max_w) : max_w);
            c.region[1] = float(f.depth_h ? std::min(f.depth_h, max_h) : max_h);
            c.region[2] = float(f.depth_x);
            c.region[3] = float(f.depth_y);
        }
        View srv[8] = {};
        srv[0].res = f.depth;
        srv[0].fmt = depth_srv_format_of(dd.Format);
        srv[0].write = false;
        View uav[14] = {};
        uav[0] = bv(lin_depth, uint64_t(NW) * NH * 16, false, true);
        transition(cmd, f.depth, f.depth_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        dispatch(cmd, kToDepth, srv, uav, &c, 4, (NW + 7) / 8, (NH + 7) / 8);
        transition(cmd, f.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.depth_state);
    }

    // 1. The frame into `original` (its own bits).
    transition(cmd, f.colour, f.colour_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ensure(cmd, original.res, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = f.colour;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        dst.pResource = original.res;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_BOX box{};
        box.right = W;
        box.bottom = H;
        box.back = 1;
        jlog("renodx original copy: %u x %u from a %llux%u source into %ux%u", W, H,
             static_cast<unsigned long long>(f.colour->GetDesc().Width),
             static_cast<unsigned>(f.colour->GetDesc().Height), original.w, original.h);
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    transition(cmd, f.colour, D3D12_RESOURCE_STATE_COPY_SOURCE, f.colour_state);
    ensure(cmd, original.res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (v6) {
        // 2a. linearize: original -> work0, the linear source everything after reads.
        const V6Constants lin = make_v6(s, f6, W, H, W, H, W, H, W, H, 0.f, 0.f, 0.f);
        View srv[8] = {};
        srv[0] = srv[1] = srv[2] = srv[3] = tv(original, false);
        View uav[14] = {};
        uav[0] = tv(work0, true);
        dispatch(cmd, kLinearize, srv, uav, &lin, sizeof lin / 4, gx, gy);
        // with Preprocess's exposure the frame is metered after its forward half; without it before (RenoDX's scale)
        if (prep_on && !prep_normalise) prep_forward_hdr(work0, true);
        Tex& metered = (prep_on && !prep_normalise) ? work_p : work0;
        feed = kFeedV1;
        if (!units.absolute && !prep_exposure) {
            // 2b. this frame's scale texel: the v1 meter and the governor (feed v1).
            if (!commit_cleared) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
                d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                device->CreateUnorderedAccessView(commit.res, nullptr, &d, main_cpu(kClearSlot + 1));
                device->CreateUnorderedAccessView(commit.res, nullptr, &d, clear_cpu(1));
                ensure(cmd, commit.res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);   // the clear writes it
                bind_heap(cmd);                                                   // the GPU handle's heap
                const float zero[4] = {};
                jlog("renodx clear commit %p uav slot %u", static_cast<void*>(commit.res), kClearSlot + 1);
                cmd->ClearUnorderedAccessViewFloat(main_gpu(kClearSlot + 1), clear_cpu(1), commit.res, zero, 0, nullptr);
                uav_barrier(cmd);
                commit_cleared = true;
            }
            // The float formats v6_exposure_scale reads (.r), as RenoDX's Vulkan path lists them.
            const bool readable = exposure_format_readable(f.exposure ? f.exposure->GetDesc().Format : DXGI_FORMAT_UNKNOWN);
            feed = select_feed(s, hdr, units, readable, f.auto_exposure || !f.create_flags_known);
            facts.feed = feed == kFeedV1 ? 0 : feed == kFeedTexture ? 1 : 2;
            facts.exposure_texture = !f.exposure ? 0 : readable ? 1 : 2;
            if (feed != logged_feed) {   // SelectFeed's line, once per decision
                logged_feed = feed;
                nr::pe::log("[nr] RenoDX stage (D3D12): NR feed %s (NRFeedMode=%u; exposure texture %s, auto "
                            "exposure %s, pre-exposure %.4g, exposure scale %.4g)",
                            feed == kFeedV1 ? "v1 (the content meter)" : feed == kFeedTexture ? "v2 (the game's exposure)"
                                                                                              : "v2 (fixed 1)",
                            s.feed_mode, !f.exposure ? "none" : readable ? "readable" : "unreadable",
                            !f.create_flags_known ? "unknown" : f.auto_exposure ? "1" : "0", f.pre_exposure,
                            f.exposure_scale);
            }
            const ScalePlan plan = plan_frame_scale(s, norm, f.reset, feed, recreate_epoch);
            if (feed == kFeedV1 || plan.guarded) {
                V6Constants meter = make_v6(s, f6, 1, 1, W, H, 1, 1, 1, 1, 0.f, 0.f, 0.f);
                meter.divisor = 0.f; meter.pedestal_cap = 0.f; meter.encoding = 1u; meter.dark_gate = 0.f;
                std::memcpy(&meter.slew_stops, plan.governor, sizeof plan.governor);
                View msrv[8] = {};
                msrv[0] = msrv[1] = msrv[2] = msrv[3] = tv(metered, false);
                View muav[14] = {};
                muav[0] = tv(scale, true);
                muav[1] = tv(commit, true);
                dispatch(cmd, kAutoscale, msrv, muav, &meter, sizeof meter / 4, 1, 1);
            }
            if (feed != kFeedV1) {
                const bool texture = feed == kFeedTexture && readable;
                if (texture) {
                    // Texel (0,0), the one v6_exposure_scale reads, into the stage's own image.
                    transition(cmd, f.exposure, f.exposure_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    ensure(cmd, exposure_1x1.res, D3D12_RESOURCE_STATE_COPY_DEST);
                    D3D12_TEXTURE_COPY_LOCATION esrc{}, edst{};
                    esrc.pResource = f.exposure;
                    esrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    esrc.SubresourceIndex = 0;
                    edst.pResource = exposure_1x1.res;
                    edst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    edst.SubresourceIndex = 0;
                    D3D12_BOX ebox{};
                    ebox.right = ebox.bottom = ebox.back = 1;
                    cmd->CopyTextureRegion(&edst, 0, 0, 0, &esrc, &ebox);
                    ensure(cmd, exposure_1x1.res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    transition(cmd, f.exposure, D3D12_RESOURCE_STATE_COPY_SOURCE, f.exposure_state);
                }
                // v6_exposure_scale's own cbuffer view: dwords 0..6. The fixed feed reads no texture: the metered
                // image fills the slot, as RenoDX's own sets do.
                const uint32_t feed_constants[7] = {texture ? uint32_t(kFeedTexture) : uint32_t(kFeedFixed),
                                                    bits(f.pre_exposure > 0.f ? f.pre_exposure : 1.f),
                                                    bits(f.exposure_scale > 0.f ? f.exposure_scale : 1.f),
                                                    bits(plan.v2_snap ? 1.f : 0.f), plan.guarded ? 1u : 0u, W, H};
                View esrv[8] = {};
                esrv[0] = texture ? tv(exposure_1x1, false) : tv(metered, false);
                esrv[1] = tv(metered, false);
                esrv[2] = texture ? tv(exposure_1x1, false) : tv(metered, false);
                esrv[3] = texture ? tv(exposure_1x1, false) : tv(metered, false);
                View euav[14] = {};
                euav[0] = tv(scale, true);
                euav[1] = tv(commit, true);
                dispatch(cmd, kExposureScale, esrv, euav, feed_constants, 7, 1, 1);
            }
        }
        if (prep_normalise) prep_forward_hdr(work0, false);   // after RenoDX's scale, which it divides by
    }


    const float nfast = std::clamp(s.fast_passes[fast_slot], 1.f, 8.f);
    const uint32_t segment = std::min(uint32_t(nfast), 3u) - 1u;
    const float kt[] = {1.f, 1.77f, 2.36f, 2.94f}, wt[] = {0.f, 0.5f, 0.67f, 0.75f};
    const float part = nfast - float(segment + 1u);
    const float fast_k = nfast > 4.f ? 2.94f + 0.58f * (nfast - 4.f) : kt[segment] + part * (kt[segment + 1] - kt[segment]);
    const float fast_w = nfast > 4.f ? std::min(0.8f, 1.f - 1.f / nfast) : wt[segment] + part * (wt[segment + 1] - wt[segment]);

    int32_t executed = -1;
    // The last pass's resolve and the commit in one dispatch when the commit takes no pedestal
    // (v6_resolve_commit; the recorded RDNA4 fixtures matched the two-dispatch output).
    static const bool fuse_off = [] { const char* e = std::getenv("NR_RENODX_FUSE"); return e && e[0] == '0'; }();
    const bool fuse = v6 && !fuse_off && !frame_pedestal_removal(s, hdr, units);
    bool fused = false;
    float final_transfer = 0.f, final_color = 0.f;
    bool final_shaped = false;
    uint32_t ran = 0;
    bool net_ok = true;
    for (uint32_t k = 0; k < passes && net_ok; ++k) {
        float transfer = 1.f, color = 1.f;
        strengths(settings, k, &transfer, &color);
        Tex& ref = k == 0 ? (v6 ? work0 : original) : stack_out(k - 1);
        Tex& write = stack_out(k);
        if (v6 && transfer <= 0.f && color <= 0.f) {
            // An explicit zero-strength pass carries the running image across exactly.
            ensure(cmd, ref.res, D3D12_RESOURCE_STATE_COPY_SOURCE);
            ensure(cmd, write.res, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(write.res, ref.res);
            uav_barrier(cmd);
            ensure(cmd, write.res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            executed = int32_t(k);
            final_transfer = final_color = 0.f;
            final_shaped = false;
            continue;
        }
        // 3a. encode: the running image -> the proxy at the network's extent.
        if (v6) {
            if (prep_on && k > 0) prep_forward_hdr(ref, false);
            if (prep_on && pp.curve != 0) {
                // Preprocess's curve in place of RenoDX's: the same footprint and sRGB, no toe or shoulder.
                const uint32_t push[4] = {NW, NH, W, H};
                View srv[8] = {};
                srv[0] = tv(work_p, false);
                View uav[14] = {};
                uav[0] = tv(proxy, true);
                dispatch(cmd, kPrepEncode, srv, uav, push, 4, nx, ny);
            } else {
                V6Constants enc = make_v6(s, f6, NW, NH, W, H, NW, NH, NW, NH, transfer, color, 0.f);
                if (prep_normalise) enc.divisor = 1.f;   // Preprocess's forward half divided already
                View srv[8] = {};
                srv[0] = srv[1] = srv[2] = tv(prep_on ? work_p : ref, false);
                srv[3] = units.absolute ? tv(work0, false) : tv(scale, false);
                View uav[14] = {};
                uav[0] = tv(proxy, true);
                dispatch(cmd, kEncode6, srv, uav, &enc, sizeof enc / 4, nx, ny);
            }
            // Probe (NR_ENCODE_PROBE_F32): the value entering the encode is this run's only output. Capture it and
            // stop here, so the RGBA32F proxy never enters the f16 network path (which would record an invalid
            // descriptor) - the frame takes the not-run path and is copied through by the caller.
            if (std::getenv("NR_ENCODE_PROBE_F32") && f.capture_in) {
                uav_barrier(cmd);   // the encode's UAV writes, visible before the copy reads them
                ensure(cmd, proxy.res, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION di{}, si{};
                di.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; di.pResource = f.capture_in;
                di.PlacedFootprint.Footprint = {proxy.format, NW, NH, 1, NW * 16};
                si.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; si.pResource = proxy.res; si.SubresourceIndex = 0;
                cmd->CopyTextureRegion(&di, 0, 0, 0, &si, nullptr);
                ensure(cmd, proxy.res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                uav_barrier(cmd);
            }
            if (std::getenv("NR_ENCODE_PROBE_F32")) return 0;   // not run: the caller copies the frame through
        } else {
            LegacyConstants c{};
            c.size[0] = NW; c.size[1] = NH; c.source_size[0] = W; c.source_size[1] = H;
            c.proxy_size[0] = c.neural_size[0] = NW; c.proxy_size[1] = c.neural_size[1] = NH;
            c.paper_white_scale = s.paper_white_scale; c.transfer_strength = transfer; c.color_strength = color;
            c.hdr = 0; c.diffuse_white_nits = s.diffuse_white_nits;
            if (ls.upsample == look::kUpsampleAreaInput && up_proxy.res && look::transport_wanted(ls, NW, NH, W, H))
                c.padding[0] = look::kAreaEncode;
            View srv[8] = {};
            srv[0] = srv[1] = srv[2] = srv[3] = tv(ref, false);
            View uav[14] = {};
            uav[0] = tv(proxy, true);
            dispatch(cmd, kEncodeL, srv, uav, &c, sizeof c / 4, nx, ny);
        }
        if (prep_on && !v6) {   // the network's input in place (mode 1), the meter once a frame
            View srv[8] = {};
            srv[0] = tv(block_mean, false);   // RenoDX's scale (binding 4): never read on SDR
            View uav[14] = {};
            uav[0] = bv(prep_state, 16, false, true);
            uav[1] = tv(proxy, true);         // a
            uav[2] = tv(prep_keep, true);     // b
            uav[3] = tv(neural, true);        // c (mode 1 never reads it)
            PrepPush push = prep_push;
            if (prep_apart) push.flags |= kPrepApart;
            if (k == 0 && pp.exposure == 1) {
                PrepPush meter = push;
                dispatch(cmd, kPrep, srv, uav, &meter, sizeof meter / 4, 1, 1);
            }
            push.mode = 1;
            dispatch(cmd, kPrep, srv, uav, &push, sizeof push / 4, nx, ny);
        }
        // Work in the input the network actually reads, including SDR Preprocess APART.
        Tex& fast_input = prep_apart ? prep_keep : proxy;
        if (fast) {
            std::string why;
            ensure_st(&why);
            if (fast_feedback) ensure_st_feedback(&why);
        }
        if (fast_feedback && motion_copied) {
            struct { uint32_t size[2], low[2], mbase[2], mgrid[2]; float mscale[2], w; uint32_t flags, block;
                     float cstop, darkcol; }
                fp{{NW, NH}, {(NW + 3) / 4, (NH + 3) / 4}, {}, {f.motion_w, f.motion_h},
                   {f.motion_scale_x * s.mvec_scale_x * float(NW), f.motion_scale_y * s.mvec_scale_y * float(NH)}, fast_w,
                   16u | 2u | (st_valid ? 1u : 0u), 4u, 0.f, 0.45f};
            View srv[8] = {};
            srv[0] = tv(st_f0[st_cur], false);
            srv[1] = tv(motion, false);
            srv[2] = tv(st_f1[st_cur], false);
            View uav[14] = {};
            uav[4] = tv(fast_input, true);
            uav[5] = tv(st_fbadd, true);
            dispatch(cmd, kStFbIn, srv, uav, &fp, sizeof fp / 4, nx, ny);
        }
        // 3b. the network: the input into a linear buffer, the ASD network, its answer back into `neural`.
        {
            struct { uint32_t w, h, what, pad; float region[4]; } c{NW, NH, 0, 0, {1.f, 1.f, 0.f, 0.f}};
            View srv[8] = {};
            srv[0] = tv(fast_input, false);
            View uav[14] = {};
            uav[0] = bv(lin_proxy, uint64_t(NW) * NH * 16, false, true);
            dispatch(cmd, kTexToBuf, srv, uav, &c, 8, (NW + 7) / 8, (NH + 7) / 8);
        }
        // Test-only: replay another route's network input instead of this route's encode, so a difference in the
        // answer can be attributed to the codec or to the network. The file is the f16 proxy (W*H*8) another route
        // recorded; each value is widened to float exactly, which is what a reader of that image sees.
        if (const char* replay = std::getenv("NR_TEST_PROXY_REPLAY"); replay && replay[0]) {
            // A directory replays a sequence: vulkan_proxy_f<N>.bin for the N-th recorded frame; a file replays that
            // one proxy every frame.
            std::string replay_path = replay;
            const bool is_dir = replay_path.back() == '\\' || replay_path.back() == '/';
            if (is_dir) {
                char nm[64];
                std::snprintf(nm, sizeof nm, "vulkan_proxy_f%u.bin", replay_frame_);
                replay_path += nm;
            }
            if (is_dir || !replay_buf_) {
                const uint64_t bytes = uint64_t(NW) * NH * 8;
                std::vector<uint8_t> f16(bytes);
                std::ifstream in(replay_path, std::ios::binary);
                if (in && in.read(reinterpret_cast<char*>(f16.data()), std::streamsize(bytes))) {
                    if (!replay_buf_) replay_buf_ = upload_buffer(device, uint64_t(NW) * NH * 16);
                    if (replay_buf_) {
                        void* m = nullptr;
                        if (SUCCEEDED(replay_buf_->Map(0, nullptr, &m)) && m) {
                            auto* dst = static_cast<float*>(m);
                            const auto* src = reinterpret_cast<const uint16_t*>(f16.data());
                            for (size_t i = 0; i < size_t(NW) * NH * 4; ++i) {
                                const uint16_t h = src[i];
                                const uint32_t sign = (h & 0x8000u) ? 0x80000000u : 0u;
                                const uint32_t e = (h >> 10) & 0x1Fu, f = h & 0x3FFu;
                                float v;
                                if (e == 0) {
                                    v = std::ldexp(float(f), -24);          // 0 or subnormal, |v| < 2^-14
                                    if (sign) v = -v;
                                } else if (e == 31) {
                                    const uint32_t bits = sign | 0x7F800000u | (f << 13);
                                    std::memcpy(&v, &bits, 4);
                                } else {
                                    const uint32_t bits = sign | ((e + 112u) << 23) | (f << 13);
                                    std::memcpy(&v, &bits, 4);
                                }
                                dst[i] = v;
                            }
                            replay_buf_->Unmap(0, nullptr);
                            jlog("renodx proxy replay: %s (%llu bytes f16)", replay_path.c_str(),
                                 static_cast<unsigned long long>(bytes));
                        }
                    }
                }
            }
            ++replay_frame_;
            if (replay_buf_) {
                ensure(cmd, lin_proxy, D3D12_RESOURCE_STATE_COPY_DEST);
                cmd->CopyBufferRegion(lin_proxy, 0, replay_buf_, 0, uint64_t(NW) * NH * 16);
                ensure(cmd, lin_proxy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                uav_barrier(cmd);
            }
        }
        // Pass k's controls, RenoDX's (record_renodx.cpp's pass_controls): pass 1's, or pass k's once its
        // "Same as pass 1" is off; with NRSkinIndependent and the Character mask on, Structure at most 1 and a
        // stock Skin sent as 1 (SentSkinSteering).
        const bool reset = reset_all || (k > 0 && !s.chained_history);
        nr::D3d12NetControls ck = f.base_controls;
        ck.history_strength = 1.f;
        {
            const uint32_t ki = std::min(k, 3u);
            const bool own = ki > 0 && !s.pass_follow[ki - 1];
            ck.intensity = ki == 0 ? s.intensity : s.pass_intensity[ki - 1];
            ck.style = int(own ? s.pass_style[ki - 1] : s.style);
            ck.local_tone = own ? s.pass_local_tone[ki - 1] : s.local_tone;
            ck.local_structure = own ? s.pass_local_structure[ki - 1] : s.local_structure;
            ck.skin_structure = own ? s.pass_skin_structure[ki - 1] : s.skin_structure;
            ck.automatic_mask = own ? s.pass_auto_mask[ki - 1] : s.auto_mask;
            ck.seed_fixed = !s.noise_every_frame;
            if (s.skin_independent && ck.automatic_mask) {
                ck.local_structure = std::min(ck.local_structure, 1.0f);
                if (ck.skin_structure < 0.0f) ck.skin_structure = 1.0f;
            }
        }
        if (f.capture_in || f.capture_out || std::getenv("NR_NET_PRE_DUMP") || std::getenv("NR_NET_KERN_DUMP")) {
            // debug: the model controls this pass sends
            nr::pe::log("[nr] stage controls: pass %u intensity %g style %d tone %g structure %g skin %g mask %d "
                        "(settings auto_mask %d skin_independent %d)",
                        k, double(ck.intensity), ck.style, double(ck.local_tone), double(ck.local_structure),
                        double(ck.skin_structure), ck.automatic_mask ? 1 : 0, s.auto_mask ? 1 : 0,
                        s.skin_independent ? 1 : 0);
        }
        // RenoDX's guides: NRDepthMode overrides the game's depth direction, NRMVecScaleX / Y scale its motion
        // vectors (the network's and the look's).
        NetTemporal nt;
        nt.motion_va = lin_mv->GetGPUVirtualAddress();
        nt.motion_scale_x = f.motion_scale_x * s.mvec_scale_x;
        nt.motion_scale_y = f.motion_scale_y * s.mvec_scale_y;
        nt.depth_va = net_wants_depth ? lin_depth->GetGPUVirtualAddress() : 0;
        nt.have_depth = net_wants_depth;
        nt.depth_inverted = f.depth_inverted;
        if (s.depth_mode == 1u) nt.depth_inverted = false;
        if (s.depth_mode == 2u) nt.depth_inverted = true;
        nt.reset = reset;
        nt.feature = (cfg.feature || passes > 1) ? stack_feature(cfg.feature ? cfg.feature : kUnnamedStack, k) : 0;
        nt.have_motion = true;
        // The net reaches these by raw address: lin_proxy and the guides are read (UAV), lin_neural is written (UAV).
        // A real barrier each frame entry - the table is not evidence that the GPU moved.
        // The guides the net will read, after the stage resampled them.
        if (f.capture_mv) {
            ensure(cmd, lin_mv, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(f.capture_mv, 0, lin_mv, 0, uint64_t(NW) * NH * 16);
            ensure(cmd, lin_mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (f.capture_depth) {
            ensure(cmd, lin_depth, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(f.capture_depth, 0, lin_depth, 0, uint64_t(NW) * NH * 16);
            ensure(cmd, lin_depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        ensure(cmd, lin_proxy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ensure(cmd, lin_neural, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ensure(cmd, lin_mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ensure(cmd, lin_depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        uav_barrier(cmd);   // the guides and lin_proxy are read by the net by raw address
        if (f.capture_in_raw) {   // the float4 buffer the network actually reads
            ensure(cmd, lin_proxy, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(f.capture_in_raw, 0, lin_proxy, 0, uint64_t(NW) * NH * 16);
            ensure(cmd, lin_proxy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            uav_barrier(cmd);
        }
        if (f.capture_in) {   // the proxy texture the network is fed from (RGBA16F, as the Vulkan stage's proxy)
            ensure(cmd, proxy.res, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION di{}, si{};
            di.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; di.pResource = f.capture_in;
            di.PlacedFootprint.Footprint = {proxy.format, NW, NH, 1, NW * (proxy.format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 16u : 8u)};
            si.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; si.pResource = proxy.res; si.SubresourceIndex = 0;
            cmd->CopyTextureRegion(&di, 0, 0, 0, &si, nullptr);
            ensure(cmd, proxy.res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            uav_barrier(cmd);
        }
        const bool net_ok_here = net(k, nt.feature, ck, nt, lin_proxy, lin_neural);
        net_ran_ = net_ran_ || net_ok_here;
        if (!net_ok_here) {
            if (!net_failed_logged) {
                net_failed_logged = true;
                nr::pe::log("[nr] RenoDX stage (D3D12): the network recorded nothing; the frame as it came");
            }
            net_ok = false;
            break;
        }
        {
            // The net's writes are UAV (the entry's ensure said so): order them against the read, then move the state.
            uav_barrier(cmd);
            if (f.capture_raw) {   // the network's answer as its buffer holds it
                ensure(cmd, lin_neural, D3D12_RESOURCE_STATE_COPY_SOURCE);
                cmd->CopyBufferRegion(f.capture_raw, 0, lin_neural, 0, uint64_t(NW) * NH * 16);
                ensure(cmd, lin_neural, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                uav_barrier(cmd);
            }
            ensure(cmd, lin_neural, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            struct { uint32_t w, h, what, pad; float region[4]; } c{NW, NH, 0, 0, {1.f, 1.f, 0.f, 0.f}};
            View srv[8] = {};
            srv[0] = bv(lin_neural, uint64_t(NW) * NH * 16, false, false);
            View uav[14] = {};
            uav[0] = tv(neural, true);
            dispatch(cmd, kBufToTex, srv, uav, &c, 8, (NW + 7) / 8, (NH + 7) / 8);
            if (f.capture_out) {   // the answer texture the codec reads, after the network's buffer is put in it
                ensure(cmd, neural.res, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION di{}, si{};
                di.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; di.pResource = f.capture_out;
                di.PlacedFootprint.Footprint = {proxy.format, NW, NH, 1, NW * (proxy.format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 16u : 8u)};
                si.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; si.pResource = neural.res; si.SubresourceIndex = 0;
                cmd->CopyTextureRegion(&di, 0, 0, 0, &si, nullptr);
                ensure(cmd, neural.res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                uav_barrier(cmd);
            }
        }
        ++ran;   // the network ran this pass
        if (fast) {
            const bool fb = fast_feedback && motion_copied;
            struct { uint32_t size[2], low[2]; float k; uint32_t mode; float halo, chroma; uint32_t satkeep;
                     float fine, base, rho, lam, dcol, cfloor, ring, ccap, nohalo; uint32_t restored; float ocap, darkcol; }
                ap{{NW, NH}, {(NW + 3) / 4, (NH + 3) / 4}, fast_k, fb ? (s.fast_method == 2u ? 8u : 6u) : 9u,
                   fb ? nfast - 1.f : (s.fast_halo_limit[fast_slot] ? 0.f : 1.f), fb ? 0.6f : 0.3f, bits(0.5f),
                   fb ? 1.f : 2.f, 1.f, 0.3f, 0.f, 0.5f, fb ? 0.f : 0.6f, 2.f, fb ? 0.f : 2.5f,
                   fb ? (s.fast_halo_limit[fast_slot] ? 1.f : 0.5f) : 0.f, 0u, fb ? -0.5f : 0.f, fb ? 0.45f : 0.f};
            if (fb && s.fast_method == 1u) ap.k = nfast - 1.f;
            const float fine = s.fast_fine[fast_slot];
            if (fine != 1.f) {
                if (!fb) ap.fine *= fine;
                else if (s.fast_method == 1u) ap.k *= fine;
                else { ap.halo *= fine; ap.fine = fine; }
            }
            ap.restored = fb ? 1u : 0u;
            const bool edit = s.fast_method == 3u;   // Edit strength: the apply alone
            if (edit) { ap.mode = 10u; ap.k = std::clamp(s.fast_edit_strength, 0.f, 4.f); }
            const uint32_t pp5[5] = {NW, NH, (NW + 3) / 4, (NH + 3) / 4, fb ? 1u : 0u};
            if (!edit) {
                View srv[8] = {};
                srv[0] = tv(fast_input, false);   // P
                srv[1] = tv(neural, false);       // N
                View uav[14] = {};
                uav[4] = tv(st_fineh, true);      // Fine (rg16f)
                uav[5] = tv(st_d0, true);
                uav[6] = tv(st_d1, true);
                dispatch(cmd, kStPrep, srv, uav, pp5, 5, nx, ny);
                st_gf(cmd, st_g0, st_g1, fb ? 2u : 4u, fb ? 1e9f : 1e-3f);
            }
            {
                View srv[8] = {};
                srv[1] = tv(fb ? st_fbadd : st_fineh, false);   // Fine
                srv[2] = tv(st_g0, false);                      // CA
                srv[3] = tv(st_g1, false);                      // CB
                srv[5] = tv(st_fineh, false);                   // FineH
                View uav[14] = {};
                uav[0] = tv(fast_input, true);                  // P (written when restored)
                uav[4] = tv(neural, true);                      // N
                uav[6] = tv(st_d0, true);                       // OutA
                uav[7] = tv(st_d1, true);                       // OutB
                dispatch(cmd, kStApply, srv, uav, &ap, sizeof ap / 4, nx, ny);
            }
            if (fb) {
                st_gf(cmd, st_f0[st_cur ^ 1u], st_f1[st_cur ^ 1u], 2u, 1e9f);
                st_cur ^= 1u;
                st_valid = true;
            }
        }
        // SDR with Preprocess: transfer the edit back through the bounded inverse, restore P to the input
        // (mode 2, RESTORE). Not on the last pass while NR Control's debug view shows this layer.
        const bool debug_raw = s.debug_view >= 1u && s.debug_view <= 3u && k + 1 == passes;
        if (prep_on && !v6 && !debug_raw) {
            PrepPush back = prep_push;
            back.mode = 2;
            back.flags = (back.flags & ~uint32_t(kPrepReset)) | (prep_apart ? 0u : uint32_t(kPrepRestore));
            Tex& nin = prep_apart ? prep_keep : proxy;
            Tex& kept = prep_apart ? proxy : prep_keep;
            View srv[8] = {};
            srv[0] = tv(block_mean, false);
            View uav[14] = {};
            uav[0] = bv(prep_state, 16, false, true);
            uav[1] = tv(nin, true);
            uav[2] = tv(kept, true);
            uav[3] = tv(neural, true);
            dispatch(cmd, kPrep, srv, uav, &back, sizeof back / 4, nx, ny);
        }
        // 3c. the look: Adjust NR's look, Detail stability, edge-aware upsampling.
        bool shaped = false, transport = false;
        run_look(cmd, k, ref, ls, motion_copied, f, reset, shaped, transport);
        Tex* rp = transport ? &up_proxy : &proxy;
        Tex* rn = transport ? &up_neural : (shaped ? &look_out : &neural);
        const uint32_t pw = transport ? W : NW, ph = transport ? H : NH;
        // 3d. resolve onto the running image.
        if (v6 && fuse && k + 1 == passes) {
            // The last pass's resolve and the commit in one dispatch when the commit takes no pedestal.
            const V6Constants res = make_v6(s, f6, W, H, W, H, pw, ph, pw, ph, transfer, color,
                                            s.chroma_clamp_stops, ratio_resolve);
            View srv[8] = {};
            srv[0] = tv(ref, false); srv[1] = tv(*rp, false); srv[2] = tv(*rn, false);
            srv[3] = units.absolute ? tv(work0, false) : tv(scale, false);
            View uav[14] = {};
            uav[0] = tv(decoded, true);
            const V6PrepConstants pres = prep_constants(res);
            dispatch(cmd, ratio_resolve ? kResolveCommit6Prep : kResolveCommit6, srv, uav,
                     ratio_resolve ? static_cast<const void*>(&pres) : static_cast<const void*>(&res),
                     ratio_resolve ? sizeof pres / 4 : sizeof res / 4, gx, gy);
            fused = true;
        } else if (v6) {
            const V6Constants res = make_v6(s, f6, W, H, W, H, pw, ph, pw, ph, transfer, color,
                                            s.chroma_clamp_stops, ratio_resolve);
            View srv[8] = {};
            srv[0] = tv(ref, false); srv[1] = tv(*rp, false); srv[2] = tv(*rn, false);
            srv[3] = units.absolute ? tv(work0, false) : tv(scale, false);
            View uav[14] = {};
            uav[0] = tv(write, true);
            const V6PrepConstants pres = prep_constants(res);
            dispatch(cmd, ratio_resolve ? kResolve6Prep : kResolve6, srv, uav,
                     ratio_resolve ? static_cast<const void*>(&pres) : static_cast<const void*>(&res),
                     ratio_resolve ? sizeof pres / 4 : sizeof res / 4, gx, gy);
        } else {
            // SDR's copied path: decode -> [reduce -> apply], into `write`.
            LegacyConstants c{};
            c.size[0] = W; c.size[1] = H; c.source_size[0] = W; c.source_size[1] = H;
            c.proxy_size[0] = c.neural_size[0] = pw; c.proxy_size[1] = c.neural_size[1] = ph;
            c.paper_white_scale = s.paper_white_scale; c.transfer_strength = transfer; c.color_strength = color;
            c.hdr = 0; c.diffuse_white_nits = s.diffuse_white_nits;
            View srv[8] = {};
            srv[0] = tv(ref, false); srv[1] = tv(*rp, false); srv[2] = tv(*rn, false); srv[3] = tv(ref, false);
            View uav[14] = {};
            uav[0] = tv(write, true);
            dispatch(cmd, kDecodeL, srv, uav, &c, sizeof c / 4, gx, gy);
            if (s.codec_mode != 0u) {   // RunBlackLevelRestore: the reduce, then the apply
                c.proxy_size[0] = c.neural_size[0] = NW; c.proxy_size[1] = c.neural_size[1] = NH;
                View rsrv[8] = {};
                rsrv[0] = tv(ref, false); rsrv[1] = tv(proxy, false); rsrv[2] = tv(neural, false);
                rsrv[3] = tv(ref, false);
                View ruav[14] = {};
                ruav[0] = tv(write, true);
                ruav[1] = tv(block_mean, true);
                dispatch(cmd, shaped ? kReduceLU : kReduceL, rsrv, ruav, &c, sizeof c / 4, (W + 31) / 32, (H + 31) / 32);
                LegacyConstants a = c;
                a.padding[0] = s.sdr_black_restore == 1u ? 2.f : 0.061f;
                a.padding[1] = float(s.sdr_black_restore);
                ruav[1] = tv(block_mean, true);   // the apply reads the means back (a UAV load, as its declare)
                dispatch(cmd, kApplyL, rsrv, ruav, &a, sizeof a / 4, gx, gy);
            }
        }
        executed = int32_t(k);
        final_transfer = transfer;
        final_color = color;
        final_shaped = shaped;
    }

    // 4. The last pass's result back into the frame's encoding (HDR: the pedestal block means and the commit),
    // into `decoded`; the copy-out below hands it to the output.
    if (v6 && net_ok && !fused) {
        Tex& work_final = executed >= 0 ? stack_out(uint32_t(executed)) : work_a;
        if (frame_pedestal_removal(s, hdr, units)) {
            const bool unshaped = executed == 0 && final_shaped;
            const V6Constants red = make_v6(s, f6, W, H, W, H, NW, NH, NW, NH, final_transfer, final_color,
                                            s.chroma_clamp_stops, ratio_resolve);
            if (unshaped) {   // a single shaped pass measures NR's own lift, unshaped
                const V6PrepConstants pred = prep_constants(red);
                View srv[8] = {};
                srv[0] = tv(work0, false); srv[1] = tv(proxy, false); srv[2] = tv(neural, false);
                srv[3] = units.absolute ? tv(work0, false) : tv(scale, false);
                View uav[14] = {};
                uav[0] = tv(block_mean, true);
                dispatch(cmd, ratio_resolve ? kReduce6UPrep : kReduce6U, srv, uav,
                         ratio_resolve ? static_cast<const void*>(&pred) : static_cast<const void*>(&red),
                         ratio_resolve ? sizeof pred / 4 : sizeof red / 4, (W + 31) / 32, (H + 31) / 32);
            } else {
                View srv[8] = {};
                srv[0] = tv(work_final, false); srv[1] = tv(work0, false);
                srv[2] = units.absolute ? tv(work0, false) : tv(scale, false);
                srv[3] = tv(work0, false);
                View uav[14] = {};
                uav[0] = tv(block_mean, true);
                dispatch(cmd, kReduce6, srv, uav, &red, sizeof red / 4, (W + 31) / 32, (H + 31) / 32);
            }
        }
        const V6Constants com = make_v6(s, f6, W, H, W, H, W, H, W, H, final_transfer, final_color, 0.f);
        const bool v2 = !units.absolute && feed != kFeedV1;
        View srv[8] = {};
        srv[0] = tv(work_final, false); srv[1] = tv(work0, false);
        srv[2] = v2 ? tv(scale, false) : (units.absolute ? tv(work0, false) : tv(scale, false));
        srv[3] = tv(work0, false);
        View uav[14] = {};
        uav[0] = tv(decoded, true);
        uav[1] = tv(block_mean, true);
        dispatch(cmd, v2 ? kCommitExposure : kCommit6, srv, uav, &com, sizeof com / 4, gx, gy);
    }

    if (!net_ok) {   // the network recorded nothing: the frame as it came
        copy_frame(cmd, f);
        return 0;
    }
    Tex& result = v6 ? decoded : (executed < 0 ? decoded : stack_out(uint32_t(executed)));
    // NR Control's debug view of this layer, over what goes to the frame.
    if (s.debug_view >= 1u && s.debug_view <= 3u) {
        const uint32_t dv[5] = {W, H, NW, NH, s.debug_view};
        Tex& shown_in = prep_on && prep_apart ? prep_keep : proxy;
        View srv[8] = {};
        srv[0] = tv(shown_in, false);
        srv[1] = tv(neural, false);
        View uav[14] = {};
        uav[0] = tv(result, true);
        dispatch(cmd, kDebug, srv, uav, dv, 5, gx, gy);
    }
    // 4. The result into the output: a store through float (bit-exact where the formats match).
    {
        struct { uint32_t w, h, what, pad; float region[4]; } c{W, H, 0, 0, {1.f, 1.f, 0.f, 0.f}};
        View srv[8] = {};
        srv[0] = tv(result, false);
        View uav[14] = {};
        uav[0].res = f.output;
        uav[0].fmt = uav_format_of(f.output->GetDesc().Format);
        uav[0].write = true;
        const bool output_is_uav = f.output_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if (!output_is_uav) transition(cmd, f.output, f.output_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(cmd, kCopyOut, srv, uav, &c, 8, (W + 7) / 8, (H + 7) / 8);
        if (!output_is_uav) transition(cmd, f.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, f.output_state);
    }
    return ran;
}


std::unique_ptr<Stage> Stage::make(const Config& c, std::string* why) {
    auto s = std::unique_ptr<Stage>(new Stage());
    s->impl_ = std::make_unique<Impl>();
    Impl& m = *s->impl_;
    m.device = c.device;
    m.cfg = c;
    m.settings = c.settings;
    m.hdr = c.present ? (c.present == 2 ? uint8_t(2) : c.present == 3 ? uint8_t(1) : uint8_t(0))
                      : frame_hdr_mode_dxgi(c.frame_format, c.hdr_flag);
    // present_path.hpp: the colour space decides, units are absolute (PQ's 10000 nits, scRGB's 80).
    if (c.present == 2) m.units = {codec::Encoding::Pq, true, 10000.f, true};
    else if (c.present == 3) m.units = {codec::Encoding::Linear, true, 80.f, false};
    else m.units = interpret_source_units(c.settings, m.hdr);
    if (!m.build(why)) return nullptr;
    return s;
}

Stage::~Stage() {
    Impl& m = *impl_;
    for (Impl::Tex* t : {&m.original, &m.proxy, &m.neural, &m.look_out, &m.decoded, &m.decoded_alt, &m.block_mean,
                   &m.up_proxy, &m.up_neural, &m.motion, &m.neutral_mv, &m.band_a, &m.band_b, &m.band_max_a,
                   &m.band_max_b, &m.prep_keep, &m.st_fineh, &m.st_fbadd, &m.st_d0, &m.st_d1, &m.st_t0, &m.st_t1,
                   &m.st_g0, &m.st_g1, &m.st_f0[0], &m.st_f0[1], &m.st_f1[0], &m.st_f1[1]})
        m.drop(*t);
    m.drop_buf(m.trace);
    m.drop_buf(m.prep_state);
    m.drop_buf(m.lin_proxy);
    m.drop_buf(m.lin_neural);
    m.drop_buf(m.lin_mv);
    m.drop_buf(m.lin_depth);
    for (auto& pl : m.looks) { m.drop_buf(pl.history[0]); m.drop_buf(pl.history[1]); }
    for (auto& p : m.pso) release(p);
    release(m.rs_codec);
    release(m.rs_aux);
    release(m.heap);
    release(m.clear_heap);
}

bool Stage::set(const Settings& s) {
    Impl& m = *impl_;
    if (recreate_settings_differ(m.settings, s)) m.recreate = true;
    else if (history_settings_differ(m.settings, s)) m.history_reset = true;
    if (m.cfg.present) {   // the Present path's units are its colour space's
        m.settings = s;
        return true;
    }
    const codec::SourceUnits units = interpret_source_units(s, m.hdr);
    // Relative <-> absolute units (NRLinearUnitNits, NRSourceEncoding): a new stage next frame.
    if (units.absolute != m.units.absolute) return false;
    m.settings = s;
    m.units = units;
    return true;
}

bool Stage::record_ok() const { return impl_ ? impl_->record_ok_ : false; }
bool Stage::net_ran() const { return impl_ ? impl_->net_ran_ : false; }
void Stage::set_completed(std::function<bool(uint64_t)> f) { impl_->completed = std::move(f); }
void Stage::set_frame(uint64_t f) { impl_->pinned = f; }

uint32_t Stage::record(ID3D12GraphicsCommandList* cmd, const Frame& f, const Frame::Net& net) {
    return impl_->record(cmd, f, net);
}

uint8_t Stage::hdr_mode() const { return impl_->hdr; }
Stage::Facts Stage::facts() const { return impl_->facts; }

void Stage::set_retire(std::function<void(IUnknown*)> f) {
    if (impl_) impl_->retire = std::move(f);
}

}  // namespace nr::pe::asd::renodx12
