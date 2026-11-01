// The wilsjo2 stage on the native D3D12 route (M5 v2): its programs - the Vulkan stage's own SPIR-V as DXBC
// (windows/build/build_wilsjo2_d3d12.sh) - as D3D12 compute pipelines the driver makes, recorded into the game's list.
// nr_asi_wilsjo2.cpp's route; a header so the offline test (nr_wilsjo2_d3d12_test.cpp) runs the very same class.
#pragma once

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
    {w2::kHudEstimate, "hudfix_estimate", false}, {w2::kHudCompose, "hudfix_compose", false},
};
using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                        UINT, UINT, ID3DBlob**, ID3DBlob**);
inline ID3DBlob* compile_cs_hlsl(const char* source, size_t bytes, const char* entry, std::string* why) {
    static PFN_D3DCompile fn = [] {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        return m ? reinterpret_cast<PFN_D3DCompile>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile"))) : nullptr;
    }();
    if (!fn) {
        if (why) *why = "d3dcompiler_47.dll is missing";
        return nullptr;
    }
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = fn(source, bytes, nullptr, nullptr, nullptr, entry, "cs_5_0", 1u << 15, 0, &blob, &errors);
    if (FAILED(hr) || !blob) {
        if (why) *why = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "D3DCompile failed";
        if (errors) errors->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    return blob;
}

inline constexpr char kHalfCopyShader[] = R"hlsl(
Texture2D<float4> g_t0 : register(t0);
RWTexture2D<float4> g_u0 : register(u4);
ByteAddressBuffer g_cb : register(t6);

float3 fast_hybrid_encode(float3 v) {
    v = max(v, 0.0f);
    float m = max(v.r, max(v.g, v.b));
    if (m <= 1e-6f) return v;
    float e = (m - 0.75f) / 0.25f;
    float y = m <= 0.75f ? m : 0.75f + 0.25f * (e * rsqrt(e * e + 1.0f));
    return v * (y / m);
}

float3 fast_hybrid_decode(float3 y) {
    y = max(y, 0.0f);
    float m = max(y.r, max(y.g, y.b));
    if (m <= 1e-6f) return y;
    float u = min((m - 0.75f) / 0.25f, 0.999999f);
    float x = m <= 0.75f ? m : 0.75f + 0.25f * (u * rsqrt(max(1.0f - u * u, 1e-8f)));
    return y * (x / m);
}

[numthreads(8, 8, 1)]
void half_copy(uint3 id : SV_DispatchThreadID) {
    uint width = g_cb.Load(8);
    uint height = g_cb.Load(12);
    if (id.x >= width || id.y >= height) return;
    float4 v = g_t0.Load(int3(int2(id.xy), 0));
    uint dbg = g_cb.Load(24);
    uint cmp = g_cb.Load(52);
    uint way = max(dbg, cmp);
    if (way == 1u) v.rgb = fast_hybrid_encode(v.rgb);
    else if (way == 2u) v.rgb = fast_hybrid_decode(v.rgb);
    g_u0[id.xy] = v;
}
)hlsl";

inline constexpr char kFastShaders[] = R"hlsl(
Texture2D<float4> Box_InA : register(t0);
Texture2D<float4> Box_InB : register(t1);
RWTexture2D<float4> Box_OutA : register(u4);
RWTexture2D<float4> Box_OutB : register(u5);

cbuffer BoxC : register(b0) {
    uint2 b_low;
    uint b_radius;
    uint b_coef;
    float b_eps;
};

groupshared float4 s_box_a[24][16];
groupshared float4 s_box_b[24][16];

[numthreads(16, 16, 1)]
void st_box(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    int2 org = int2(gid.xy) * 16;
    int2 lid = int2(gtid.xy);
    int r = int(b_radius);
    int col = org.x + lid.x;
    for (int k = lid.y; k < 16 + 2 * r; k += 16) {
        int row = org.y - r + k;
        if (row < 0 || row >= int(b_low.y) || col >= int(b_low.x)) continue;
        float4 A = 0.0f, B = 0.0f; float c = 0.0f;
        for (int i = -r; i <= r; ++i) {
            int2 t = int2(col + i, row);
            if (t.x < 0 || t.x >= int(b_low.x)) continue;
            A += Box_InA.Load(int3(t, 0));
            B += Box_InB.Load(int3(t, 0));
            c += 1.0f;
        }
        s_box_a[k][lid.x] = A / c;
        s_box_b[k][lid.x] = B / c;
    }
    GroupMemoryBarrierWithGroupSync();
    int2 q = org + lid;
    if (q.x >= int(b_low.x) || q.y >= int(b_low.y)) return;
    float4 sa = 0.0f, sb = 0.0f; float c = 0.0f;
    for (int i = -r; i <= r; ++i) {
        int row = q.y + i;
        if (row < 0 || row >= int(b_low.y)) continue;
        sa += s_box_a[lid.y + r + i][lid.x];
        sb += s_box_b[lid.y + r + i][lid.x];
        c += 1.0f;
    }
    precise float4 A = sa / c, B = sb / c;
    if (b_coef != 0u) {
        float mI = A.x;
        float vI = max(A.y - mI * mI, 0.0f);
        float3 mp = float3(A.z, A.w, B.x);
        float3 mIp = B.yzw;
        float3 a = (mIp - mI * mp) / (vI + b_eps);
        Box_OutA[q] = float4(a, mI);
        Box_OutB[q] = float4(mp - a * mI, vI);
    } else {
        Box_OutA[q] = A;
        Box_OutB[q] = B;
    }
}

float3 st_dec(float3 c) {
    return lerp(c / 12.92f, pow(max((c + 0.055f) / 1.055f, 1e-8f), 2.4f), step(0.04045f, c));
}
float3 st_enc(float3 l) {
    l = clamp(l, 0.0f, 1.0f);
    return lerp(l * 12.92f, 1.055f * pow(max(l, 1e-8f), 1.0f / 2.4f) - 0.055f, step(0.0031308f, l));
}
float3 st_lab(float3 s) {
    float3 l = st_dec(max(s, 0.0f));
    float3 lms = float3(dot(l, float3(0.4122214708f, 0.5363325363f, 0.0514459929f)),
                        dot(l, float3(0.2119034982f, 0.6806995451f, 0.1073969566f)),
                        dot(l, float3(0.0883024619f, 0.2817188376f, 0.6299787005f)));
    float3 q = sign(lms) * pow(max(abs(lms), 1e-12f), 1.0f / 3.0f);
    return float3(dot(q, float3(0.2104542553f, 0.7936177850f, -0.0040720468f)),
                  dot(q, float3(1.9779984951f, -2.4285922050f, 0.4505937099f)),
                  dot(q, float3(0.0259040371f, 0.7827717662f, -0.8086757660f)));
}
float3 st_lin(float3 lab) {
    float3 q = float3(lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z,
                      lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z,
                      lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z);
    float3 lms = q * q * q;
    return float3(dot(lms, float3(4.0767416621f, -3.3077115913f, 0.2309699292f)),
                  dot(lms, float3(-1.2684380046f, 2.6097574011f, -0.3413193965f)),
                  dot(lms, float3(-0.0041960863f, -0.7034186147f, 1.7076147010f)));
}
bool st_in(float3 l) { return all(l >= -1e-6f) && all(l <= 1.0f + 1e-6f); }
float st_soft(float anchor, float extra) {
    float a = clamp(anchor, 1e-4f, 1.0f - 1e-4f);
    return extra < 0.0f ? a * exp(extra / a) : 1.0f - (1.0f - a) * exp(-extra / (1.0f - a));
}

Texture2D<float4> Prep_P : register(t0);
Texture2D<float4> Prep_N : register(t1);
RWTexture2D<float2> Prep_Fine : register(u4);
RWTexture2D<float4> Prep_OutA : register(u5);
RWTexture2D<float4> Prep_OutB : register(u6);

cbuffer PrepC : register(b0) {
    uint2 pr_size;
    uint2 pr_low;
    uint pr_src;
};

groupshared float s_prep_dl[20][20];
groupshared float s_prep_ln[20][20];
groupshared float s_prep_lp[20][20];
groupshared float3 s_prep_v[16][16];

[numthreads(16, 16, 1)]
void st_prep(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gindex : SV_GroupIndex) {
    int2 org = int2(gid.xy) * 16;
    int2 lid = int2(gtid.xy);
    for (uint i = gindex; i < 400u; i += 256u) {
        int2 t = clamp(org - 2 + int2(i % 20u, i / 20u), int2(0, 0), int2(pr_size) - 1);
        float ln = st_lab(Prep_N.Load(int3(t, 0)).rgb).x;
        float lp = st_lab(Prep_P.Load(int3(t, 0)).rgb).x;
        s_prep_dl[i / 20u][i % 20u] = ln - lp;
        s_prep_ln[i / 20u][i % 20u] = ln;
        s_prep_lp[i / 20u][i % 20u] = lp;
    }
    int2 px = org + lid;
    bool inside = (px.x < int(pr_size.x) && px.y < int(pr_size.y));
    if (pr_src != 0u && inside) {
        s_prep_v[lid.y][lid.x] = Prep_N.Load(int3(px, 0)).rgb - Prep_P.Load(int3(px, 0)).rgb;
    }
    GroupMemoryBarrierWithGroupSync();
    if (inside) {
        static const float w[5] = {1.0f, 4.0f, 6.0f, 4.0f, 1.0f};
        float s = 0.0f;
        for (int y = -2; y <= 2; ++y)
            for (int x = -2; x <= 2; ++x)
                s += w[x + 2] * w[y + 2] * s_prep_dl[lid.y + 2 + y][lid.x + 2 + x];
        Prep_Fine[px] = float2(s_prep_dl[lid.y + 2][lid.x + 2] - s / 256.0f, s_prep_ln[lid.y + 2][lid.x + 2]);
    }
    if (lid.x < 4 && lid.y < 4) {
        uint2 q = gid.xy * 4u + uint2(lid);
        if (q.x < pr_low.x && q.y < pr_low.y) {
            float4 A = 0.0f, B = 0.0f; float c = 0.0f;
            for (uint y = 0u; y < 4u; ++y)
                for (uint x = 0u; x < 4u; ++x) {
                    uint2 p = q * 4u + uint2(x, y);
                    if (p.x >= pr_size.x || p.y >= pr_size.y) continue;
                    uint2 l = p - uint2(org);
                    float I = s_prep_lp[l.y + 2u][l.x + 2u];
                    float3 v = pr_src == 0u ? float3(s_prep_dl[l.y + 2u][l.x + 2u], 0.0f, 0.0f) : s_prep_v[l.y][l.x];
                    A += float4(I, I * I, v.x, v.y);
                    B += float4(v.z, I * v.x, I * v.y, I * v.z);
                    c += 1.0f;
                }
            Prep_OutA[q] = A / max(c, 1.0f);
            Prep_OutB[q] = B / max(c, 1.0f);
        }
    }
}

float3 st_gfq_fn(Texture2D<float4> CA, Texture2D<float4> CB, float2 pos, float I, uint2 low, out float mI, out float vI) {
    float2 q = clamp(pos / 4.0f - 0.5f, 0.0f, float2(low) - 1.0f);
    int2 a = int2(floor(q)), b = min(a + 1, int2(low) - 1);
    float2 f = q - float2(a);
    float4 A = lerp(lerp(CA.Load(int3(a, 0)), CA.Load(int3(b.x, a.y, 0)), f.x),
                    lerp(CA.Load(int3(a.x, b.y, 0)), CA.Load(int3(b, 0)), f.x), f.y);
    float4 B = lerp(lerp(CB.Load(int3(a, 0)), CB.Load(int3(b.x, a.y, 0)), f.x),
                    lerp(CB.Load(int3(a.x, b.y, 0)), CB.Load(int3(b, 0)), f.x), f.y);
    mI = A.w; vI = B.w;
    return A.rgb * I + B.rgb;
}

Texture2D<float4> Fb_CA : register(t0);
Texture2D<float4> Fb_Motion : register(t1);
Texture2D<float4> Fb_CB : register(t2);
RWTexture2D<float4> Fb_Proxy : register(u4);
RWTexture2D<float4> Fb_Add : register(u5);

cbuffer FbC : register(b0) {
    uint2 fb_size;
    uint2 fb_low;
    uint2 fb_mbase;
    uint2 fb_mgrid;
    float2 fb_mscale;
    float fb_w;
    uint fb_flags;
    uint fb_block;
    float fb_cstop;
    float fb_darkcol;
};

[numthreads(16, 16, 1)]
void st_fb_in(uint3 id : SV_DispatchThreadID) {
    uint2 px = id.xy;
    if (px.x >= fb_size.x || px.y >= fb_size.y) return;
    float4 p = Fb_Proxy[px];
    float3 np = p.rgb;
    float conf = 0.0f;
    if ((fb_flags & 1u) != 0u) {
        float2 prev = float2(px) + 0.5f;
        if ((fb_flags & 2u) != 0u) {
            uint2 t = fb_mbase + min(uint2((float2(px) + 0.5f) * float2(fb_mgrid) / float2(fb_size)), fb_mgrid - 1u);
            prev += Fb_Motion.Load(int3(int2(t), 0)).xy * fb_mscale;
        }
        if ((fb_flags & 16u) != 0u) {
            if (all(prev >= 0.0f) && all(prev <= float2(fb_size))) {
                float I = st_lab(p.rgb).x;
                float mI, vI;
                float3 q = st_gfq_fn(Fb_CA, Fb_CB, prev, I, fb_low, mI, vI);
                float z = (I - mI) * (I - mI) / (vI + 0.0004f);
                conf = exp(-0.125f * z);
                float3 add = fb_w * conf * q;
                if (fb_darkcol > 0.0f) {
                    float3 X = st_lab(p.rgb);
                    float3 Y = st_lab(saturate(p.rgb + add));
                    add = st_enc(st_lin(float3(Y.x, X.yz + smoothstep(0.0f, fb_darkcol, X.x) * (Y.yz - X.yz)))) - p.rgb;
                }
                float s = 1.0f;
                if (add.x > 1e-7f) s = min(s, max(1.0f - p.x, 0.0f) / add.x);
                else if (add.x < -1e-7f) s = min(s, max(p.x, 0.0f) / -add.x);
                if (add.y > 1e-7f) s = min(s, max(1.0f - p.y, 0.0f) / add.y);
                else if (add.y < -1e-7f) s = min(s, max(p.y, 0.0f) / -add.y);
                if (add.z > 1e-7f) s = min(s, max(1.0f - p.z, 0.0f) / add.z);
                else if (add.z < -1e-7f) s = min(s, max(p.z, 0.0f) / -add.z);
                np = p.rgb + s * add;
            }
        }
    }
    Fb_Proxy[px] = float4(np, p.a);
    Fb_Add[px] = float4(np - p.rgb, conf);
}

Texture2D<float4> Apply_Fine : register(t1);
Texture2D<float4> Apply_CA : register(t2);
Texture2D<float4> Apply_CB : register(t3);
Texture2D<float4> Apply_FineH : register(t5);
RWTexture2D<float4> Apply_P : register(u0);
RWTexture2D<float4> Apply_N : register(u4);
RWTexture2D<float4> Apply_OutA : register(u6);
RWTexture2D<float4> Apply_OutB : register(u7);

cbuffer ApplyC : register(b0) {
    uint2 ap_size;
    uint2 ap_low;
    float ap_k;
    uint ap_mode;
    float ap_halo;
    float ap_chroma;
    uint ap_satkeep;
    float ap_fine;
    float ap_base;
    float ap_rho;
    float ap_lam;
    float ap_dcol;
    float ap_cfloor;
    float ap_ring;
    float ap_ccap;
    float ap_nohalo;
    uint ap_restored;
    float ap_ocap;
    float ap_darkcol;
};

struct Ans { float3 O; float co; float2 uo; };
Ans make_answer(float3 O) {
    Ans a;
    a.O = O;
    a.co = length(O.yz);
    a.uo = a.co > 0.0f ? O.yz / a.co : float2(0.0f, 0.0f);
    return a;
}

float3 at7(Ans a, float amp, float2 dab, float f, float t, uint mode, float ccap, float rho, uint satkeep) {
    float3 O = a.O;
    float L = st_soft(O.x, (t - 1.0f) * amp);
    float g = mode >= 7u ? pow(max(L / max(O.x, 1e-4f), 1e-8f), asfloat(satkeep)) : 1.0f;
    if (mode >= 8u) g = min(g, 1.0f);
    float tc = t;
    if (ccap > 1.0f) {
        float e = (t - 1.0f) / (ccap - 1.0f);
        tc = 1.0f + (t - 1.0f) / pow(max(1.0f + e * e * e * e, 1e-8f), 0.25f);
    }
    float2 ab = (O.yz + (tc - 1.0f) * f * dab) * g;
    float c = length(ab);
    if (rho > 0.0f && a.co > 0.005f && c > 1e-6f) {
        float2 u = ab / c;
        float d = atan2(u.x * a.uo.y - u.y * a.uo.x, dot(u, a.uo));
        float r = rho * d;
        float sr = sin(r), cr = cos(r);
        ab = c * float2(u.x * cr - u.y * sr, u.x * sr + u.y * cr);
    }
    return float3(L, ab);
}

float3 ov_search(Ans A, float amp, float2 dab, float f, float k, uint mode, float ccap, float rho, uint satkeep) {
    float lo = 1.0f, hi = k;
    for (int i = 0; i < 12; ++i) {
        float m = 0.5f * (lo + hi);
        if (st_in(st_lin(at7(A, amp, dab, f, m, mode, ccap, rho, satkeep)))) lo = m; else hi = m;
    }
    return st_lin(at7(A, amp, dab, f, lo, mode, ccap, rho, satkeep));
}

static const uint kQueue = 128u, kInline = 0xffffffffu, kDone = 0xfffffffeu;
groupshared float4 s_qa[128], s_qb[128];
groupshared float2 s_qc[128];
groupshared uint s_qn;
groupshared float4 s_iv[16][16];

struct Ov { float2 dab; float f, amp; float3 l; uint slot; };

Ov ov_begin(float3 X, Ans A, float amp_in, float k, float chroma, float cfloor, uint mode, float ccap, float rho, uint satkeep) {
    Ov v;
    float3 O = A.O;
    v.dab = O.yz - X.yz;
    float rel = length(v.dab) / (length(X.yz) + 0.01f) / chroma;
    float fl = cfloor * smoothstep(0.4f, 0.65f, O.x);
    v.f = fl + (1.0f - fl) / (1.0f + rel * rel);
    v.amp = amp_in > -1e8f ? amp_in : O.x - X.x;
    v.l = st_lin(at7(A, v.amp, v.dab, v.f, k, mode, ccap, rho, satkeep));
    v.slot = kDone;
    if (!st_in(v.l)) {
        InterlockedAdd(s_qn, 1u, v.slot);
        if (v.slot < kQueue) {
            s_qa[v.slot] = float4(O, v.amp);
            s_qb[v.slot] = float4(A.uo, v.dab);
            s_qc[v.slot] = float2(v.f, A.co);
        } else {
            v.slot = kInline;
        }
    }
    return v;
}

void ov_drain(uint gindex, float k, uint mode, float ccap, float rho, uint satkeep) {
    GroupMemoryBarrierWithGroupSync();
    uint n = min(s_qn, kQueue);
    for (uint i = gindex; i < n; i += 256u) {
        float4 a = s_qa[i], b = s_qb[i]; float2 c = s_qc[i];
        Ans A; A.O = a.xyz; A.co = c.y; A.uo = b.xy;
        s_qa[i].xyz = ov_search(A, a.w, b.zw, c.x, k, mode, ccap, rho, satkeep);
    }
    GroupMemoryBarrierWithGroupSync();
}

float3 ov_end(Ov v, Ans A, float3 o, float k, uint mode, float ccap, float rho, uint satkeep) {
    float3 l = v.l;
    if (v.slot == kInline) l = ov_search(A, v.amp, v.dab, v.f, k, mode, ccap, rho, satkeep);
    else if (v.slot != kDone) l = s_qa[v.slot].xyz;
    return st_in(l) ? st_enc(l) : o;
}

float3 st_dadd(float3 n, float3 Ln, float3 add, float darkcol, float dcol) {
    if (darkcol > 0.0f) {
        float3 N0 = Ln;
        float3 Y0 = st_lab(saturate(n + add));
        float cs = (dcol >= 1.0f ? 1.0f : max(dcol, 0.0f)) * smoothstep(0.0f, darkcol, N0.x);
        return st_enc(st_lin(float3(Y0.x, N0.yz + cs * (Y0.yz - N0.yz)))) - n;
    }
    if (dcol >= 1.0f) return add;
    float3 N = Ln;
    float3 Y = st_lab(saturate(n + add));
    if (dcol < 0.0f) {
        float2 ab = N.yz * (Y.x / max(N.x, 1e-3f)) - dcol * (Y.yz - N.yz);
        return st_enc(st_lin(float3(Y.x, ab))) - n;
    }
    return st_enc(st_lin(float3(Y.x, N.yz + dcol * (Y.yz - N.yz)))) - n;
}

float st_fit(float3 base, float3 add) {
    float s = 1.0f;
    if (add.x > 1e-7f) s = min(s, max(1.0f - base.x, 0.0f) / add.x);
    else if (add.x < -1e-7f) s = min(s, max(base.x, 0.0f) / -add.x);
    if (add.y > 1e-7f) s = min(s, max(1.0f - base.y, 0.0f) / add.y);
    else if (add.y < -1e-7f) s = min(s, max(base.y, 0.0f) / -add.y);
    if (add.z > 1e-7f) s = min(s, max(1.0f - base.z, 0.0f) / add.z);
    else if (add.z < -1e-7f) s = min(s, max(base.z, 0.0f) / -add.z);
    return s;
}

float3 st_nohalo(float3 o, float3 alt, float nohalo) {
    if (nohalo > 1.5f) return st_lab(o).x > st_lab(alt).x ? alt : o;
    return lerp(o, alt, clamp(nohalo, 0.0f, 1.0f));
}

float3 st_relight(float3 lab, float Lb, float extra) {
    float3 l = st_lin(float3(st_soft(Lb, extra), lab.yz));
    if (st_in(l)) return st_enc(l);
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 12; ++i) {
        float m = 0.5f * (lo + hi);
        if (st_in(st_lin(float3(st_soft(Lb, m * extra), lab.yz)))) lo = m; else hi = m;
    }
    return st_enc(st_lin(float3(st_soft(Lb, lo * extra), lab.yz)));
}

float3 st_fb_ring(float3 o, float3 Ln, int2 px, Texture2D<float4> fineH, uint2 size) {
    float r0 = 1e9f;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            int2 t_coord = clamp(px + int2(x, y), int2(0, 0), int2(size) - 1);
            r0 = min(r0, fineH.Load(int3(t_coord, 0)).g);
        }
    }
    float3 O = st_lab(o);
    float floor_l = min(Ln.x, r0);
    if (O.x >= floor_l) return o;
    float3 l = st_lin(float3(floor_l, O.yz));
    return st_in(l) ? st_enc(l) : o;
}

float3 ocap_tail(uint2 px, float3 p, float3 o, Texture2D<float4> fine_tex, float ocap, uint mode) {
    if (ocap != 0.0f && (mode == 6u || mode == 8u)) {
        float3 own = p - fine_tex.Load(int3(int2(px), 0)).rgb;
        float3 Xo = st_lab(own);
        float3 Y = st_lab(o);
        float2 dab = Y.yz - Xo.yz;
        float c0 = length(Xo.yz);
        float2 u0 = c0 > 1e-4f ? Xo.yz / c0 : float2(0.0f, 0.0f);
        float2 desat = ocap < 0.0f ? min(dot(dab, u0), 0.0f) * u0 : float2(0.0f, 0.0f);
        float2 rest = dab - desat;
        float lim = abs(ocap) * (c0 + 0.01f);
        float m = length(rest);
        if (m > 1e-6f) {
            float e = m / lim;
            float mo = m / pow(max(1.0f + e * e * e * e, 1e-8f), 0.25f);
            float3 l = st_lin(float3(Y.x, Xo.yz + desat + rest * (mo / m)));
            if (st_in(l)) o = st_enc(l);
        }
    }
    return o;
}

float st_h_fn(float x) {
    return abs(x) >= 6.103515625e-05f ? asfloat(asuint(x) & 0xffffe000u) : trunc(x * 16777216.0f) * 5.9604644775390625e-08f;
}
float3 st_h_vec3(float3 x) { return float3(st_h_fn(x.r), st_h_fn(x.g), st_h_fn(x.b)); }

float3 st_edit_fn(float3 nE, float3 pE, float k) {
    float3 p = st_dec(saturate(pE)), n = st_dec(saturate(nE));
    float3 e = log2(max(n, 1.0f / 1024.0f)) - log2(max(p, 1.0f / 1024.0f));
    float t = smoothstep(1.0f / 1024.0f, 1.0f / 64.0f, max(p.x, max(p.y, p.z)));
    return st_enc(n * exp2(t * (k - 1.0f) * e));
}

float3 strengthened(uint2 px, float4 n, float3 p, Ans ans, uint2 ap_size, uint2 ap_low, float ap_k, uint ap_mode,
                    float ap_halo, float ap_chroma, uint ap_satkeep, float ap_fine, float ap_base, float ap_rho,
                    float ap_dcol, float ap_cfloor, float ap_ring, float ap_ccap, float ap_nohalo, float ap_ocap,
                    float ap_darkcol) {
    float3 X = st_lab(p);
    float mI, vI;
    float3 o = n.rgb;
    if (ap_mode == 9u) {
        float3 O = ans.O;
        float dL = O.x - X.x;
        float B = st_gfq_fn(Apply_CA, Apply_CB, float2(px) + 0.5f, X.x, ap_low, mI, vI).r;
        float F = Apply_FineH.Load(int3(int2(px), 0)).r;
        float amp = ap_base * B + ap_fine * F + (ap_nohalo > 0.5f ? 0.0f : ap_halo) * (dL - B - F);
        float2 dab = O.yz - X.yz;
        float rel = length(dab) / (length(X.yz) + 0.01f) / ap_chroma;
        float fl = ap_cfloor * smoothstep(0.4f, 0.65f, O.x);
        float f = fl + (1.0f - fl) / (1.0f + rel * rel);
        float tt = ap_k;
        float3 lab = at7(ans, amp, dab, f, ap_k, ap_mode, ap_ccap, ap_rho, ap_satkeep);
        float3 l = st_lin(lab);
        if (!st_in(l)) {
            float lo = 1.0f, hi = ap_k;
            for (int i = 0; i < 12; ++i) {
                float m = 0.5f * (lo + hi);
                if (st_in(st_lin(at7(ans, amp, dab, f, m, ap_mode, ap_ccap, ap_rho, ap_satkeep)))) lo = m; else hi = m;
            }
            tt = lo; lab = at7(ans, amp, dab, f, lo, ap_mode, ap_ccap, ap_rho, ap_satkeep); l = st_lin(lab);
        }
        if (ap_ring > 0.0f && ap_fine != 1.0f) {
            float r0 = 1e9f, r1 = -1e9f;
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x) {
                    int2 t_coord = clamp(int2(px) + int2(x, y), int2(0, 0), int2(ap_size) - 1);
                    float v = Apply_FineH.Load(int3(t_coord, 0)).g;
                    r0 = min(r0, v); r1 = max(r1, v);
                }
            float lnf = st_soft(ans.O.x, (tt - 1.0f) * (amp - (ap_fine - 1.0f) * F));
            lab.x = ap_ring > 1.5f ? max(lab.x, min(lnf, r0)) : clamp(lab.x, min(lnf, r0), max(lnf, r1));
            float3 l2 = st_lin(lab);
            if (st_in(l2)) l = l2;
        }
        if (st_in(l)) o = st_enc(l);
    } else if (ap_mode == 6u) {
        float3 q = st_gfq_fn(Apply_CA, Apply_CB, float2(px) + 0.5f, X.x, ap_low, mI, vI);
        float3 add = st_dadd(n.rgb, ans.O, ap_k * ((n.rgb - p) - q), ap_darkcol, ap_dcol);
        o = n.rgb + st_fit(n.rgb, add) * add;
        if (ap_nohalo > 0.0f) {
            float fF = ap_k * Apply_FineH.Load(int3(int2(px), 0)).r;
            o = st_nohalo(o, fF != 0.0f ? st_relight(ans.O, ans.O.x, fF) : n.rgb, ap_nohalo);
        }
        if (ap_nohalo > 0.0f && ap_ring > 0.0f) o = st_fb_ring(o, ans.O, int2(px), Apply_FineH, ap_size);
    }
    return ocap_tail(px, p, o, Apply_Fine, ap_ocap, ap_mode);
}

[numthreads(16, 16, 1)]
void st_apply(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gindex : SV_GroupIndex) {
    uint2 px = gid.xy * 16u + gtid.xy;
    bool inside = (px.x < ap_size.x && px.y < ap_size.y);
    bool hybrid = (ap_mode == 8u);
    if (hybrid) {
        if (gindex == 0u) s_qn = 0u;
        GroupMemoryBarrierWithGroupSync();
    }
    float4 n = 0.0f, p = 0.0f;
    float3 o = 0.0f;
    Ans ans = make_answer(float3(0.0f, 0.0f, 0.0f));
    Ov v1 = {float2(0, 0), 0.0f, 0.0f, float3(0, 0, 0), kDone};
    Ov v2 = {float2(0, 0), 0.0f, 0.0f, float3(0, 0, 0), kDone};
    float3 ofb = 0.0f;
    float4 fa = 0.0f;
    if (inside) {
        n = Apply_N[px];
        p = Apply_P[px];
        ans = make_answer(st_lab(n.rgb));
        if (hybrid) {
            float3 X = st_lab(p.rgb);
            float mI, vI;
            fa = Apply_Fine.Load(int3(int2(px), 0));
            float3 q = st_gfq_fn(Apply_CA, Apply_CB, float2(px) + 0.5f, X.x, ap_low, mI, vI);
            float3 pp = p.rgb - fa.rgb, Xp = st_lab(pp);
            float3 add = st_dadd(n.rgb, ans.O, ap_halo * ((n.rgb - p.rgb) - q), ap_darkcol, ap_dcol);
            ofb = n.rgb + st_fit(n.rgb, add) * add;
            v1 = ov_begin(Xp, ans, -1e9f, ap_k, ap_chroma, ap_cfloor, ap_mode, ap_ccap, ap_rho, ap_satkeep);
            if (ap_nohalo > 0.0f) {
                float F = Apply_FineH.Load(int3(int2(px), 0)).r, fF = ap_halo * F;
                ofb = st_nohalo(ofb, fF != 0.0f ? st_relight(ans.O, ans.O.x, fF) : n.rgb, ap_nohalo);
                if (ap_ring > 0.0f) ofb = st_fb_ring(ofb, ans.O, int2(px), Apply_FineH, ap_size);
                float Bl = st_lab(saturate(pp + q)).x - Xp.x;
                v2 = ov_begin(Xp, ans, Bl + ap_fine * F, ap_k, ap_chroma, ap_cfloor, ap_mode, ap_ccap, ap_rho, ap_satkeep);
            }
        } else {
            o = (ap_mode == 10u) ? st_edit_fn(n.rgb, p.rgb, ap_k)
                                  : strengthened(px, n, p.rgb, ans, ap_size, ap_low, ap_k, ap_mode, ap_halo,
                                                 ap_chroma, ap_satkeep, ap_fine, ap_base, ap_rho, ap_dcol,
                                                 ap_cfloor, ap_ring, ap_ccap, ap_nohalo, ap_ocap, ap_darkcol);
         }
     }
    if (hybrid) {
        ov_drain(gindex, ap_k, ap_mode, ap_ccap, ap_rho, ap_satkeep);
        if (inside) {
            float3 oov = ov_end(v1, ans, n.rgb, ap_k, ap_mode, ap_ccap, ap_rho, ap_satkeep);
            if (ap_nohalo > 0.0f) oov = st_nohalo(oov, ov_end(v2, ans, n.rgb, ap_k, ap_mode, ap_ccap, ap_rho, ap_satkeep), ap_nohalo);
            o = ocap_tail(px, p.rgb, lerp(oov, ofb, saturate(fa.a)), Apply_Fine, ap_ocap, ap_mode);
        }
    }
    if (inside) {
        Apply_N[px] = float4(o, n.a);
        if (ap_restored != 0u) {
            float3 back = p.rgb - Apply_Fine.Load(int3(int2(px), 0)).rgb;
            Apply_P[px] = float4(back, p.a);
            float3 pk = st_h_vec3(back);
            s_iv[gtid.y][gtid.x] = float4(st_lab(pk).x, st_h_vec3(o) - pk);
        }
    }
    if (ap_restored == 0u) return;
    GroupMemoryBarrierWithGroupSync();
    if (gtid.x >= 4u || gtid.y >= 4u) return;
    uint2 q = gid.xy * 4u + gtid.xy;
    if (q.x >= ap_low.x || q.y >= ap_low.y) return;
    float4 A = 0.0f, B = 0.0f; float c = 0.0f;
    for (uint y = 0u; y < 4u; ++y) {
        for (uint x = 0u; x < 4u; ++x) {
            uint2 t = q * 4u + uint2(x, y);
            if (t.x >= ap_size.x || t.y >= ap_size.y) continue;
            float4 iv = s_iv[t.y - gid.y * 16u][t.x - gid.x * 16u];
            float I = iv.x; float3 v = iv.yzw;
            A += float4(I, I * I, v.x, v.y);
            B += float4(v.z, I * v.x, I * v.y, I * v.z);
            c += 1.0f;
        }
    }
    Apply_OutA[q] = A / max(c, 1.0f);
    Apply_OutB[q] = B / max(c, 1.0f);
}
)hlsl";

class Stage12 final : public w2::Stage::Backend {
public:
    static constexpr UINT kSlots = 4096;
    static constexpr UINT kStride = 256;   // the Vulkan stage's push-constant block
    static_assert(sizeof(w2::Constants) <= kStride, "the constants fit the block");   // dispatches before a slot (6 descriptors, 256 bytes) is reused
    using Ensure = std::function<void(ID3D12Resource*, D3D12_RESOURCE_STATES)>;

    ~Stage12() override {
        for (auto* p : fast_pso_) if (p) p->Release();
        if (rs_fast_) rs_fast_->Release();
        for (auto* p : pso_) if (p) p->Release();
        for (auto* c : copies_) c->Release();
        if (upload_) upload_->Release();
        if (heap_) heap_->Release();
        if (rs_) rs_->Release();
    }
    // On a thread of ours: the programs of `folder` (wilsjo2_<name>.dxbc). False: why.
    bool make(ID3D12Device* device, const std::wstring& folder, std::string* why) {
        device_ = device;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 4;   // t0..t3
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        ranges[1].BaseShaderRegister = 4;   // u4, u5 (the Vulkan bindings 4, 5)
        D3D12_ROOT_PARAMETER p[3]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[0].DescriptorTable.NumDescriptorRanges = 1;
        p[0].DescriptorTable.pDescriptorRanges = &ranges[0];
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1;
        p[1].DescriptorTable.pDescriptorRanges = &ranges[1];
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[2].Descriptor.ShaderRegister = 6;   // the constants (ByteAddressBuffer cb : register(t6))
        for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // Spec 1.2 (the Vulkan stage's sampler): bilinear, mip-linear, clamp to the edge on all three axes, no LOD clamp
        D3D12_STATIC_SAMPLER_DESC ss[4]{};
        for (UINT i = 0; i < 4; ++i) {
            ss[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            ss[i].MaxAnisotropy = 1;
            ss[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            ss[i].MinLOD = 0.0f;
            ss[i].MaxLOD = D3D12_FLOAT32_MAX;
            ss[i].ShaderRegister = i;
            ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 3; rd.pParameters = p;
        rd.NumStaticSamplers = 4; rd.pStaticSamplers = ss;
        ID3DBlob *blob = nullptr, *errors = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        if (errors) errors->Release();
        if (FAILED(hr) || !blob) { *why = "D3D12SerializeRootSignature failed"; return false; }
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs_));
        blob->Release();
        if (FAILED(hr)) { *why = "CreateRootSignature failed"; return false; }
        // Half-copy pipeline (mode 9)
        {
            std::string err;
            ID3DBlob* code = compile_cs_hlsl(kHalfCopyShader, sizeof kHalfCopyShader - 1, "half_copy", &err);
            if (code) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
                pd.pRootSignature = rs_;
                pd.CS.pShaderBytecode = code->GetBufferPointer();
                pd.CS.BytecodeLength = code->GetBufferSize();
                if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_[w2::kHalfCopy])))) {
                    pso_[w2::kHalfCopy] = nullptr;
                }
                code->Release();
            }
            if (!pso_[w2::kHalfCopy]) {
                NR_W2D3D12_LOG("[asd] wilsjo2 on D3D12: half_copy HLSL compile failed: %s", err.c_str());
            }
        }
        // Root signature for fast programs
        {
            D3D12_ROOT_PARAMETER rp_fast[3]{};
            rp_fast[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp_fast[0].Constants.ShaderRegister = 0;
            rp_fast[0].Constants.RegisterSpace = 0;
            rp_fast[0].Constants.Num32BitValues = 24;
            rp_fast[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_DESCRIPTOR_RANGE r_srv{};
            r_srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            r_srv.NumDescriptors = 8;
            r_srv.BaseShaderRegister = 0;
            r_srv.RegisterSpace = 0;
            r_srv.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            rp_fast[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp_fast[1].DescriptorTable.NumDescriptorRanges = 1;
            rp_fast[1].DescriptorTable.pDescriptorRanges = &r_srv;
            rp_fast[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_DESCRIPTOR_RANGE r_uav{};
            r_uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            r_uav.NumDescriptors = 8;
            r_uav.BaseShaderRegister = 0;
            r_uav.RegisterSpace = 0;
            r_uav.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            rp_fast[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp_fast[2].DescriptorTable.NumDescriptorRanges = 1;
            rp_fast[2].DescriptorTable.pDescriptorRanges = &r_uav;
            rp_fast[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_STATIC_SAMPLER_DESC ss_fast{};
            ss_fast.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ss_fast.AddressU = ss_fast.AddressV = ss_fast.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            ss_fast.MaxAnisotropy = 1;
            ss_fast.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            ss_fast.MinLOD = 0.0f;
            ss_fast.MaxLOD = D3D12_FLOAT32_MAX;
            ss_fast.ShaderRegister = 0;
            ss_fast.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC rd_fast{};
            rd_fast.NumParameters = 3;
            rd_fast.pParameters = rp_fast;
            rd_fast.NumStaticSamplers = 1;
            rd_fast.pStaticSamplers = &ss_fast;

            blob = nullptr; errors = nullptr;
            hr = D3D12SerializeRootSignature(&rd_fast, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
            if (errors) errors->Release();
            if (SUCCEEDED(hr) && blob) {
                device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs_fast_));
                blob->Release();
            }

            if (rs_fast_) {
                const char* const names[] = {"st_fb_in", "st_prep", "st_box", "st_apply"};
                for (uint32_t i = 0; i < nr::fast::kProgramCount; ++i) {
                    std::string err;
                    ID3DBlob* code = compile_cs_hlsl(kFastShaders, sizeof kFastShaders - 1, names[i], &err);
                    if (code) {
                        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
                        pd.pRootSignature = rs_fast_;
                        pd.CS.pShaderBytecode = code->GetBufferPointer();
                        pd.CS.BytecodeLength = code->GetBufferSize();
                        if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&fast_pso_[i])))) {
                            fast_pso_[i] = nullptr;
                        }
                        code->Release();
                    }
                    if (!fast_pso_[i]) {
                        NR_W2D3D12_LOG("[asd] wilsjo2 on D3D12: %s HLSL compile failed: %s", names[i], err.c_str());
                    }
                }
            }
        }
        std::map<std::string, ID3D12PipelineState*> made;
        for (const ProgramFile& pf : kPrograms) {
            auto it = made.find(pf.name);
            if (it == made.end()) {
                const std::wstring path = folder + L"\\wilsjo2_" + std::wstring(pf.name, pf.name + std::strlen(pf.name)) + L".dxbc";
                std::vector<char> code;
                if (FILE* f = _wfopen(path.c_str(), L"rb")) {
                    char buf[65536];
                    size_t n;
                    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) code.insert(code.end(), buf, buf + n);
                    std::fclose(f);
                }
                ID3D12PipelineState* pso = nullptr;
                if (!code.empty()) {
                    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
                    pd.pRootSignature = rs_;
                    pd.CS.pShaderBytecode = code.data();
                    pd.CS.BytecodeLength = code.size();
                    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) pso = nullptr;
                }
                it = made.emplace(pf.name, pso).first;
                if (!pso) {
                    if (pf.required) { *why = std::string("wilsjo2_") + pf.name + ".dxbc " + (code.empty() ? "is missing" : "does not make a pipeline"); return false; }
                    NR_W2D3D12_LOG("[asd] wilsjo2 on D3D12: wilsjo2_%s.dxbc %s (its feature takes the bridge)", pf.name,
                                code.empty() ? "is missing" : "does not make a pipeline");
                }
            }
            if (it->second) {
                if (pso_[pf.mode]) pso_[pf.mode]->Release();
                it->second->AddRef();
                pso_[pf.mode] = it->second;
            }
        }
        for (auto& [name, pso] : made) if (pso) pso->Release();
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kSlots * 16;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) { *why = "CreateDescriptorHeap failed"; return false; }
        inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = uint64_t(kSlots) * kStride; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&upload_)))) { *why = "the constants' upload buffer could not be made"; return false; }
        void* m = nullptr;
        if (FAILED(upload_->Map(0, nullptr, &m)) || !m) { *why = "the constants' upload buffer could not be mapped"; return false; }
        mapped_ = static_cast<uint8_t*>(m);
        return true;
    }
    // A recording on `cmd` (its state saved by the caller): our heap and root signature bound; `ensure` puts a resource
    // in a state (the route's tracking).
    void begin(ID3D12GraphicsCommandList* cmd, Ensure ensure) {
        cmd_ = cmd;
        ensure_ = std::move(ensure);
        ID3D12DescriptorHeap* heaps[] = {heap_};
        cmd_->SetDescriptorHeaps(1, heaps);
        cmd_->SetComputeRootSignature(rs_);
    }
    void end() { cmd_ = nullptr; ensure_ = nullptr; }
    bool has_fast() const override {
        return fast_pso_[0] && fast_pso_[1] && fast_pso_[2] && fast_pso_[3] && pso_[w2::kHalfCopy];
    }
    bool has(w2::Mode mode) const override { return mode < kModes && pso_[mode]; }
    void record(const w2::Dispatch& d) override {
        const uint32_t mode = d.constants.mode;
        if (!cmd_) throw std::runtime_error("wilsjo2 stage (D3D12): recorded outside a recording");
        if (mode >= kModes || !pso_[mode]) throw std::runtime_error("wilsjo2 stage (D3D12): no program for mode " + std::to_string(mode));
        if (!d.t0 || !d.u0) throw std::runtime_error("wilsjo2 stage (D3D12): a dispatch without t0 or u0");
        // Spec 6.3: a read slot without a resource is given t0's, a missing u1 is given u0's.
        w2::Image read_images[4] = {d.t0, d.t1 ? d.t1 : d.t0, d.t2 ? d.t2 : d.t0, d.t3 ? d.t3 : d.t0};
        const w2::Image* reads[4] = {&read_images[0], &read_images[1], &read_images[2], &read_images[3]};
        const w2::Image* writes[2] = {&d.u0, d.u1 ? &d.u1 : &d.u0};
        // An image read and written by one dispatch (the resolve in place: the frame at t0 / t2 and u0): read through a
        // copy of it made just before - what each invocation reads in Vulkan's GENERAL layout, the image as it was; one
        // resource cannot be a shader resource and an unordered-access one at once in D3D12.
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
        const UINT slot = slot_;
        slot_ = (slot_ + 1) % kSlots;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(slot) * 16 * inc_;
        gpu.ptr += UINT64(slot) * 16 * inc_;
        for (UINT i = 0; i < 4; ++i) {
            ID3D12Resource* r = resource(*reads[i]);
            const D3D12_RESOURCE_DESC rd = r->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = srv_format(rd.Format);
            if (v.Format == DXGI_FORMAT_UNKNOWN || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.SampleDesc.Count != 1)
                throw std::runtime_error("wilsjo2 stage (D3D12): an image it cannot read (format " + std::to_string(int(rd.Format)) + ")");
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (reads[i]->layer != 0 || reads[i]->mip >= rd.MipLevels)
                throw std::runtime_error("wilsjo2 stage (D3D12): a read of an array layer or a missing mip level");
            v.Texture2D.MostDetailedMip = reads[i]->mip;
            v.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
            h.ptr += SIZE_T(i) * inc_;
            device_->CreateShaderResourceView(r, &v, h);
        }
        for (UINT i = 0; i < 2; ++i) {
            ID3D12Resource* r = resource(*writes[i]);
            const D3D12_RESOURCE_DESC rd = r->GetDesc();
            D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
            v.Format = uav_format(rd.Format);
            if (v.Format == DXGI_FORMAT_UNKNOWN || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                throw std::runtime_error("wilsjo2 stage (D3D12): an image it cannot write (format " + std::to_string(int(rd.Format)) + ")");
            v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
            h.ptr += SIZE_T(4 + i) * inc_;
            device_->CreateUnorderedAccessView(r, nullptr, &v, h);
        }
        std::memcpy(mapped_ + uint64_t(slot) * kStride, &d.constants, sizeof(w2::Constants));
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
        gpu_uav.ptr += UINT64(4) * inc_;
        cmd_->SetComputeRootSignature(rs_);
        cmd_->SetPipelineState(pso_[mode]);
        cmd_->SetComputeRootDescriptorTable(0, gpu);
        cmd_->SetComputeRootDescriptorTable(1, gpu_uav);
        cmd_->SetComputeRootShaderResourceView(2, upload_->GetGPUVirtualAddress() + uint64_t(slot) * kStride);
        // Spec 6.4: 8x8 groups over Width x Height; the meter one group a tile, Width x Height groups.
        if (mode == w2::kMeter) cmd_->Dispatch(d.constants.width, d.constants.height, 1);
        else if (mode == w2::kPrepMeter) cmd_->Dispatch(1, 1, 1);   // one 16 x 16 group over the whole image
        else cmd_->Dispatch((d.constants.width + 7) / 8, (d.constants.height + 7) / 8, 1);
        // the Vulkan stage's barrier after every dispatch: its writes seen by whatever comes next
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
        if (program >= nr::fast::kProgramCount || !fast_pso_[program] || !rs_fast_)
            throw std::runtime_error("wilsjo2 stage (D3D12): fast program " + std::to_string(program) + " not ready");

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

        const UINT slot = slot_;
        slot_ = (slot_ + 1) % kSlots;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(slot) * 16 * inc_;
        gpu.ptr += UINT64(slot) * 16 * inc_;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_srv = cpu;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav = cpu;
        cpu_uav.ptr += SIZE_T(8) * inc_;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv = gpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu;
        gpu_uav.ptr += UINT64(8) * inc_;

        for (uint32_t i = 0; i < count; ++i) {
            uint32_t b = images[i].binding;
            if (b >= 8) continue;
            ID3D12Resource* r = resource(images[i].image);
            if (!r) continue;
            const D3D12_RESOURCE_DESC rd = r->GetDesc();
            if (is_uav(program, b)) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
                v.Format = uav_format(rd.Format);
                if (v.Format == DXGI_FORMAT_UNKNOWN || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                    !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                    throw std::runtime_error("wilsjo2 stage (D3D12): fast image cannot be UAV written (format " + std::to_string(int(rd.Format)) + ")");
                v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                D3D12_CPU_DESCRIPTOR_HANDLE h = cpu_uav;
                h.ptr += SIZE_T(b) * inc_;
                device_->CreateUnorderedAccessView(r, nullptr, &v, h);
            } else {
                D3D12_SHADER_RESOURCE_VIEW_DESC v{};
                v.Format = srv_format(rd.Format);
                if (v.Format == DXGI_FORMAT_UNKNOWN || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.SampleDesc.Count != 1)
                    throw std::runtime_error("wilsjo2 stage (D3D12): fast image cannot be SRV read (format " + std::to_string(int(rd.Format)) + ")");
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                v.Texture2D.MostDetailedMip = images[i].image.mip;
                v.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE h = cpu_srv;
                h.ptr += SIZE_T(b) * inc_;
                device_->CreateShaderResourceView(r, &v, h);
            }
        }

        cmd_->SetComputeRootSignature(rs_fast_);
        cmd_->SetPipelineState(fast_pso_[program]);
        if (push && bytes) {
            cmd_->SetComputeRoot32BitConstants(0, (bytes + 3) / 4, push, 0);
        }
        cmd_->SetComputeRootDescriptorTable(1, gpu_srv);
        cmd_->SetComputeRootDescriptorTable(2, gpu_uav);
        cmd_->Dispatch(gx, gy, 1);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;
        cmd_->ResourceBarrier(1, &b);
        ++dispatches_;
    }

private:
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
    static constexpr uint32_t kModes = 128;
    ID3D12Device* device_ = nullptr;
    ID3D12RootSignature* rs_ = nullptr;
    ID3D12PipelineState* pso_[kModes]{};
    ID3D12RootSignature* rs_fast_ = nullptr;
    ID3D12PipelineState* fast_pso_[nr::fast::kProgramCount]{};
    ID3D12DescriptorHeap* heap_ = nullptr;
    ID3D12Resource* upload_ = nullptr;
    uint8_t* mapped_ = nullptr;
    UINT inc_ = 0, slot_ = 0;
    uint64_t dispatches_ = 0;
    ID3D12GraphicsCommandList* cmd_ = nullptr;
    Ensure ensure_;
};

}  // namespace nr::asi::native
