#pragma once
// The Passes (fast) programs (windows/shaders/renodx-fast, ours, shared by every RenoDX Stage) as D3D12 compute
// HLSL: the same math as the GLSL ones, compiled at runtime with d3dcompiler_47 (cs_5_0). Shared by the wilsjo2
// stage on the native D3D12 route (nr_asi_wilsjo2_d3d12.hpp) and the RenoDX stage on it (nr_renodx_d3d12.cpp).
// Register layout: texture binding N -> tN, image binding N -> uN; the constants in cbuffer b0 (<= 24 dwords).

#include <cstdint>


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

