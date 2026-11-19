// The RenoDX stage on the D3D12 passthrough (nr_renodx_d3d12.hpp): host orchestration mirrors nr_renodx.cpp,
// while outer compute shaders run as pre-captured PAL ELFs from d3d12_outer.nra via mapped upload slots (zero
// SetDescriptorHeaps calls and zero d3dcompiler_47 dependency).
#include "nr_renodx_d3d12.hpp"
#include "nr_d3d12_direct_state.hpp"
#include "nr_d3d12_il.hpp"
#include "nr_d3d12_outer_elf.hpp"
#include "renodx/codec_gain.hpp"   // the codec's divisors and anchors (dlssnr.hpp's, as nr_renodx.cpp includes it)
#include "nr_d3d12_passthrough.hpp"
#include "nr_pe_log.hpp"
#include "nr_vram_ledger.hpp"
#include <cstdarg>
#include <cmath>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nr::pe::d3d12_passthrough::renodx12 {
using Settings = nr::renodx::Settings;   // RenoDX's own (nr_renodx.hpp)
namespace {

#ifdef NR_TOOLS_BUILD
std::atomic<uint32_t> g_test_fail_dispatch_at{0};
#endif

void release(IUnknown* p) { if (p) p->Release(); }

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
           a.fast_edit_strength != b.fast_edit_strength || a.fast_interleaved_passes != b.fast_interleaved_passes ||
           a.fast_interleaved_preset != b.fast_interleaved_preset ||
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

// NR_RENODX_FUSE=0 keeps the last resolve and the commit apart (record()'s fuse; read once)
bool fuse_off_env() {
    static const bool off = [] { const char* e = std::getenv("NR_RENODX_FUSE"); return e && e[0] == '0'; }();
    return off;
}

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// InferHdrMode / FrameHdrMode on DXGI formats (nr_renodx.cpp's Vulkan ones).
uint8_t infer_hdr_mode_dxgi(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
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

// A depth texture's SRV view (nr_d3d12_passthrough_backend.cpp's depth_view_format).
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
    if (s.display_pedestal == 0u) return false;   // NRDisplayPedestal Off (the default): never, whatever the codec
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
    c.dark_gate_mode = s.display_pedestal == 3u ? 0u : s.display_pedestal;   // the shaders' modes: 0 auto, 1 always, 2 smooth
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
             kEncode6Lin, kResolve6Lin, kResolveCommit6Lin,
             kDecodeUpL,          // look_upsample + legacy_decode (SDR, the network below the frame's extent)
             kResolveCommit6Up,   // look_upsample + v6_resolve_commit (HDR, the same; nr_renodx.cpp's)
             kDebug,
             // the aux root signature (Passes (fast), Preprocess, the plumbing)
             kStFbIn, kStPrep, kStBox, kStApply, kPrep, kTexToBuf, kBufToTex, kCopyOut, kToMotion, kToDepth,
             kSdrEncodeLin, kSdrDecodeLin,
             kCount };
    bool codec_sig(P p) const { return p <= kDebug; }
    ID3D12RootSignature* rs_outer = nullptr;
    d3d12_outer::OuterKernel kernels[kCount]{};
    d3d12_outer::OuterHeapPool outer_pool;
    ID3D12Resource* zero_upload = nullptr;   // 512-byte zeroed UPLOAD buffer for CopyBufferRegion / CopyTextureRegion clears
    nr::gpu::Ticket current_ticket;
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
    bool lin_mv_dirty = false;
    bool decoded_storage = false;
    bool decoded_load = false;
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
    bool cmd_touched_ = false;
    bool rs_outer_bound_ = false;
    Stage::Facts facts;
    // The v6 codec's (HDR): its work surfaces (RGBA16F at the frame's extent; work_b made when a stack first
    // needs it), the 1x1 scale and commit, Preprocess's forward-half image, the exposure texel's copy.
    Tex work0, work_a, work_b, scale, commit, work_p, exposure_1x1;
    int up_fused_logged = -1;
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
        transition(cmd, r, it->second, want);
        it->second = want;
    }
    void uav_barrier(ID3D12GraphicsCommandList* cmd) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cmd->ResourceBarrier(1, &b);
        cmd_touched_ = true;
    }

    void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                    D3D12_RESOURCE_STATES to) {
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
        cmd_touched_ = true;
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
        nr::vram::track_d3d12(device, t.res, nr::vram::kRenoDX);
        return t;
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
        nr::vram::track_d3d12(device, r, nr::vram::kRenoDX);
        return r;
    }
    // A replaced surface may still be read by a recorded frame another list has not run yet, so the release goes
    // through the caller's completion watermark when it gave one; `states` forgets it either way.
    std::function<void(IUnknown*)> retire;
    // False when a surface this frame needed could not be made: the frame must not be reported as processed (its
    // output may not have been written). A frame with NR off is a legitimate copy-through and stays true.
    bool record_ok_ = true;
    bool net_ran_ = false;   // the network was actually called and answered for this frame
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

    static uint32_t dispatch_limit() {
        static const uint32_t v = [] {
            wchar_t b[16] = {};
            const DWORD n = GetEnvironmentVariableW(L"NR_STAGE_DISPATCH_LIMIT", b, 16);
            return n && n < 16 ? uint32_t(std::wcstoul(b, nullptr, 10)) : 0u;
        }();
        return v;
    }
    uint32_t dispatches_done = 0;
    uint32_t frame_dispatches_ = 0;
    void dispatch(ID3D12GraphicsCommandList* cmd, P p, View srv[8], View uav[14], const void* push,
                  uint32_t dwords, uint32_t gx, uint32_t gy, bool barrier = true) {
        if (!record_ok_) return;
        static const int only = [] {
            wchar_t b[16] = {};
            const DWORD n = GetEnvironmentVariableW(L"NR_STAGE_ONLY", b, 16);
            return n && n < 16 ? std::stoi(std::wstring(b)) : -1;
        }();
        if (only >= 0 && int(p) != only) return;
        const uint32_t limit = dispatch_limit();
        if (limit && dispatches_done >= limit) return;
        ++dispatches_done;
        ++frame_dispatches_;
#ifdef NR_TOOLS_BUILD
        {
            const uint32_t target_idx = g_test_fail_dispatch_at.load(std::memory_order_relaxed);
            if (target_idx > 0u && frame_dispatches_ == target_idx) {
                record_ok_ = false;
                return;
            }
        }
#endif

        const d3d12_outer::OuterKernel& k = kernels[p];
        if (!k.pso || !rs_outer) {
            record_ok_ = false;
            nr::pe::log("[nr] RenoDX stage: outer kernel %d is missing", int(p));
            return;
        }
        std::string slot_err;
        d3d12_outer::OuterSlot s = outer_pool.acquire(current_ticket, &slot_err, k.desc_zero_bytes);
        if (!s.cpu) {
            record_ok_ = false;
            nr::pe::log("[nr] RenoDX stage: %s", slot_err.empty() ? "no outer descriptor slot" : slot_err.c_str());
            return;
        }

        const bool glsl = (p == kPrepEncode) || (p >= kDebug && p <= kPrep);
        const bool aux_hlsl = p >= kTexToBuf;
        const uint32_t uav_shift = glsl ? 0u : (aux_hlsl ? 8u : 6u);
        const UINT n_srv = codec_sig(p) ? 6u : 8u;
        const UINT n_uav = codec_sig(p) ? 14u : 8u;

        for (UINT i = 0; i < n_srv; ++i) {
            if (!srv[i].res) continue;
            ensure(cmd, srv[i].res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (const auto* ob = k.find_binding(i)) {
                if (srv[i].buffer) s.write_buf(ob, srv[i].res, srv[i].bytes);
                else s.write_srv_tex(device, ob, srv[i].res, srv[i].fmt, 0, 1);
            }
        }
        for (UINT j = 0; j < n_uav; ++j) {
            if (!uav[j].res) continue;
            ensure(cmd, uav[j].res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (const auto* ob = k.find_binding(uav_shift + j)) {
                if (uav[j].buffer) s.write_buf(ob, uav[j].res, uav[j].bytes);
                else s.write_uav_tex(device, ob, uav[j].res, uav[j].fmt, 0);
            }
        }
        s.write_push(push, dwords * 4u, &k);
        s.dispatch(cmd, rs_outer, k, gx, gy, 1u, !rs_outer_bound_);
        rs_outer_bound_ = true;
        cmd_touched_ = true;
        if (barrier) uav_barrier(cmd);
    }

    bool typed_uav(DXGI_FORMAT f) const {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
        fs.Format = f;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs))) return false;
        return (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
               (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    }
    bool typed_uav_load(DXGI_FORMAT f) const {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
        fs.Format = f;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs))) return false;
        return (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
               (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) &&
               (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    }

    bool build(std::string* why);
    void run_look(ID3D12GraphicsCommandList* cmd, uint32_t pass, Tex& ref, const look::Settings& ls, bool motion_copied,
                  const Frame& fr, bool reset, bool& shaped, bool& transport, bool up_fused);
    void copy_frame(ID3D12GraphicsCommandList* cmd, const Frame& f);
    uint32_t record(ID3D12GraphicsCommandList* cmd, const Frame& f, const Frame::Net& net);

    // Passes (fast)'s workset (nr_renodx.cpp's ensure_st, the images only - the programs are built at start).
    bool ensure_st(std::string* why) {
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
            // all or none: a set with a hole would be dispatched (and never made again: st_fineh is there)
            for (Tex* t : {&st_fineh, &st_d0, &st_d1, &st_t0, &st_t1, &st_g0, &st_g1})
                if (!t->res) {
                    for (Tex* u : {&st_fineh, &st_d0, &st_d1, &st_t0, &st_t1, &st_g0, &st_g1}) drop(*u);
                    return false;
                }
        }
        return true;
    }
    bool ensure_st_feedback(std::string* why) {
        const uint32_t nw = cfg.nr_width, nh = cfg.nr_height;
        const uint32_t lw = (nw + 3) / 4, lh = (nh + 3) / 4;
        if (st_fbadd.res) return true;
        st_fbadd = make_tex(nw, nh, DXGI_FORMAT_R16G16B16A16_FLOAT, true, why);
        for (Tex* t : {&st_f0[0], &st_f0[1], &st_f1[0], &st_f1[1]})
            *t = make_tex(lw, lh, DXGI_FORMAT_R32G32B32A32_FLOAT, true, why);
        st_valid = false;
        for (Tex* t : {&st_fbadd, &st_f0[0], &st_f0[1], &st_f1[0], &st_f1[1]})
            if (!t->res) {
                for (Tex* u : {&st_fbadd, &st_f0[0], &st_f0[1], &st_f1[0], &st_f1[1]}) drop(*u);
                return false;
            }
        return true;
    }
    // The feedback's images when the method is no longer Feedback / Hybrid (nr_renodx.cpp ensure_st's retire)
    void drop_st_feedback() {
        if (!st_fbadd.res) return;
        for (Tex* t : {&st_fbadd, &st_f0[0], &st_f0[1], &st_f1[0], &st_f1[1]}) drop(*t);
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
        uav[4] = tv(st_t0, true); uav[5] = tv(st_t1, true);   // the box means, then their coefficients' means below
        dispatch(cmd, kStBox, srv, uav, &g, sizeof g / 4, gx, gy);
        g.coef = 0u;
        srv[0] = tv(st_t0, false); srv[1] = tv(st_t1, false);
        uav[4] = tv(outa, true); uav[5] = tv(outb, true);
        dispatch(cmd, kStBox, srv, uav, &g, sizeof g / 4, gx, gy);
    }

    // ---- Passes (fast) Interleaved (nr_renodx.cpp's il_*: the same programs, as HLSL - nr_d3d12_il.hpp) ----
    static constexpr uint32_t kIlSlots = 8;
    d3d12_outer::OuterKernel il_kernels[2][7]{};   // preset 0 (P1) and preset 1 (P2)
    d3d12_outer::OuterKernel ilpp_kernel{};
    bool il_loaded = false;
    Tex il_base, il_base2, il_trust, il_tdbg, il_hist, il_inc[2][kIlSlots], il_pyr[6], il_dpyr[6], il_depth[2];
    Tex il_lf[2];
    uint32_t il_preset = 0, il_lfcur = 0;
    bool il_lf_valid = false, il_depth_const = false;
    static uint32_t selected_il_preset(const Settings& s) {
        static const uint32_t env = [] {
            const char* e = std::getenv("NR_IL_PRESET");
            return e && (e[0] == '1' || e[0] == '2') ? uint32_t(e[0] - '0') : 0u;
        }();
        return env ? env : s.fast_interleaved_preset == 1u || s.fast_interleaved_preset == 2u
            ? s.fast_interleaved_preset : fast::kDefaultInterleavedPreset;
    }
    ID3D12Resource* lin_hist = nullptr;   // a slot's history in the network's history layout (NetTemporal::history_va)
    uint32_t il_slots = 0, il_pyr_slots = 0, il_cur = 0, il_bcur = 0, il_dcur = 0, il_frame = 0, il_last[kIlSlots] = {};
    bool il_valid = false, il_graded_last = false, il_missing_logged = false, il_depth_logged = false;
    bool load_il(d3d12_outer::OuterArchive& arch, std::string* why) {
        for (uint32_t step : {0u, 1u, 3u, 4u, 5u}) {
            if (!arch.build_kernel(device, "il/il0_" + std::to_string(step), &il_kernels[0][step], why)) return false;
        }
        for (uint32_t step : {0u, 1u, 3u, 4u, 5u, 6u}) {
            if (!arch.build_kernel(device, "il/il_" + std::to_string(step), &il_kernels[1][step], why)) return false;
        }
        if (!arch.build_kernel(device, "il/il_pp", &ilpp_kernel, why)) return false;
        il_loaded = true;
        return true;
    }
    void drop_il() {
        for (Tex* t : {&il_base, &il_base2, &il_trust, &il_tdbg, &il_hist}) drop(*t);
        for (auto& a : il_inc) for (Tex& t : a) drop(t);
        for (Tex& t : il_pyr) drop(t);
        for (Tex& t : il_dpyr) drop(t);
        for (Tex& t : il_depth) drop(t);
        for (Tex& t : il_lf) drop(t);
        drop_buf(lin_hist);
        il_valid = false;
        il_slots = 0;
        il_pyr_slots = 0;
        il_preset = 0; il_lf_valid = false; il_depth_const = false;
    }
    // Interleaved's images at the network's extent for `slots` passes (nr_renodx.cpp ensure_il), and the slot history's
    // linear buffer for the network's history texel of `hist_bytes`; before the frame's first command.
    bool ensure_il(uint32_t nw, uint32_t nh, uint32_t slots, uint32_t hist_bytes, uint32_t preset, std::string* why) {
        const DXGI_FORMAT h4 = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (il_base.w != nw || il_base.h != nh || il_preset != preset) {
            drop_il(); il_preset = preset;
            il_base = make_tex(nw, nh, h4, true, why);
            il_base2 = make_tex(nw, nh, h4, true, why);
            il_trust = make_tex(nw, nh, DXGI_FORMAT_R16_FLOAT, true, why);
            il_tdbg = make_tex(1, 1, h4, true, why);
            il_hist = make_tex(nw, nh, h4, true, why);
            uint32_t pw = nw, ph = nh;
            for (Tex& t : il_dpyr) { pw = (pw + 1) / 2; ph = (ph + 1) / 2; t = make_tex(pw, ph, DXGI_FORMAT_R32G32_FLOAT, true, why); }
            for (Tex& t : il_depth) t = make_tex(nw, nh, DXGI_FORMAT_R32_FLOAT, true, why);
            if (preset == 2u) for (Tex& t : il_lf)
                t = make_tex(2 * ((nw + 7) / 8), (nh + 7) / 8, h4, true, why);
        }
        // a change of the passes: only the slots past it, and the pyramid when it must grow (nr_renodx.cpp's rule: a
        // remake of everything under VRAM pressure put the new images in system memory for good)
        if (il_slots != slots) {
            for (auto& a : il_inc)
                for (uint32_t i = 0; i < kIlSlots; ++i) {
                    if (i < slots && !a[i].res) a[i] = make_tex(nw, nh, h4, true, why);
                    else if (i >= slots && a[i].res) drop(a[i]);
                }
            if (slots > il_pyr_slots) {
                uint32_t pw = nw, ph = nh;
                for (Tex& t : il_pyr) { drop(t); pw = (pw + 1) / 2; ph = (ph + 1) / 2; t = make_tex(pw * slots, ph, h4, true, why); }
                il_pyr_slots = slots;
            }
            il_slots = slots;
            il_valid = false;
        }
        {
            bool ok = il_base.res && il_base2.res && il_trust.res && il_tdbg.res && il_hist.res;
            for (auto& a : il_inc) for (uint32_t i = 0; i < slots; ++i) ok = ok && a[i].res;
            for (Tex& t : il_pyr) ok = ok && t.res;
            for (Tex& t : il_dpyr) ok = ok && t.res;
            for (Tex& t : il_depth) ok = ok && t.res;
            if (preset == 2u) for (Tex& t : il_lf) ok = ok && t.res;
            if (!ok) { drop_il(); return false; }
        }
        hist_bytes = std::max(hist_bytes, 8u);   // (0: the network's layout not known yet - its own history is used then)
        const uint64_t need = uint64_t(lin_pitch(nw, hist_bytes)) * nh * hist_bytes;
        if (!lin_hist || lin_hist->GetDesc().Width < need) {
            drop_buf(lin_hist);
            lin_hist = make_buffer(need, why);
            if (!lin_hist) { drop_il(); return false; }
        }
        return true;
    }
    void dispatch_il(ID3D12GraphicsCommandList* cmd, const d3d12_outer::OuterKernel& k,
                     const std::vector<d3d12_il::Bind>& binds, const void* push, uint32_t dwords,
                     uint32_t gx, uint32_t gy, bool barrier = true) {
        if (!k.pso || !rs_outer) {
            record_ok_ = false;
            nr::pe::log("[nr] RenoDX stage (D3D12): Interleaved outer kernel missing");
            return;
        }
        std::string slot_err;
        d3d12_outer::OuterSlot s = outer_pool.acquire(current_ticket, &slot_err, k.desc_zero_bytes);
        if (!s.cpu) {
            record_ok_ = false;
            nr::pe::log("[nr] RenoDX stage (D3D12): %s", slot_err.empty() ? "no outer descriptor slot" : slot_err.c_str());
            return;
        }
        for (const auto& b : binds)
            if (b.res) ensure(cmd, b.res, b.write ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        d3d12_il::write_outer_binds(device, s, k, binds.data(), binds.size());
        s.write_push(push, dwords * 4u, &k);
        s.dispatch(cmd, rs_outer, k, gx, gy, 1u, !rs_outer_bound_);
        rs_outer_bound_ = true;
        cmd_touched_ = true;
        if (barrier) uav_barrier(cmd);
    }
    // The frame's Interleaved work around the network (nr_renodx.cpp's il_on branch, the product path): one slot a frame
    // (every slot in order on a reset = legacy N), its input the frame + the earlier slots' moved increments, the answer
    // the frame + every increment. `in`: the network's input image (the proxy, or SDR Preprocess's image apart).
    template <class NetFn>
    bool il_run(ID3D12GraphicsCommandList* cmd, const Frame& f, const Settings& s, bool reset, Tex& in,
                bool motion_direct, uint32_t m_rx, uint32_t m_ry, uint32_t m_rw, uint32_t m_rh, const NetFn& pass_net) {
        const uint32_t NW = cfg.nr_width, NH = cfg.nr_height, nx = (NW + 15) / 16, ny = (NH + 15) / 16;
        const uint32_t n = il_slots, pi = il_preset - 1u;
        const bool stable = il_preset == 2u;
        // the game's depth for the disocclusion test, the vector dilation and the depth-aware fill (as on Vulkan)
        ID3D12Resource* depth = f.depth;
        DXGI_FORMAT dfmt = DXGI_FORMAT_UNKNOWN;
        uint32_t dw = 1, dh = 1;
        if (depth) {
            const D3D12_RESOURCE_DESC dd = depth->GetDesc();
            dfmt = depth_srv_format_of(dd.Format);
            dw = f.depth_w ? f.depth_w : uint32_t(dd.Width);
            dh = f.depth_h ? f.depth_h : uint32_t(dd.Height);
            if (dfmt == DXGI_FORMAT_UNKNOWN) depth = nullptr;
        }
        bool inverted = f.depth_inverted;
        if (s.depth_mode == 1u) inverted = false;
        if (s.depth_mode == 2u) inverted = true;
        if (depth && !il_depth_logged) {
            il_depth_logged = true;
            nr::pe::log("[nr] RenoDX stage (D3D12): Interleaved reads the game's depth (DXGI format %d, %ux%u)", int(dfmt), dw, dh);
        }
        struct { uint32_t size[2], mbase[2], mgrid[2]; float mscale[2]; uint32_t mode, slot, n, flags; float k0, k1;
                 uint32_t dbase[2], dgrid[2]; uint32_t styles; float pad0; float inten[4]; } ip{
            {NW, NH}, {motion_direct ? m_rx : 0u, motion_direct ? m_ry : 0u},
            {motion_direct ? m_rw : f.motion_w, motion_direct ? m_rh : f.motion_h},
            {f.motion_scale_x * s.mvec_scale_x * float(NW), f.motion_scale_y * s.mvec_scale_y * float(NH)}, 0u, 0u, n, 0u,
            0.02f, 0.06f, {f.depth_x, f.depth_y}, {dw, dh}, 0u, 0.f, {1.f, 1.f, 1.f, 1.f}};
        static_assert(sizeof ip == 96, "il's push block");
        // Each pass's Intensity and Style (pass_controls); not plain: the slots' networks run plain and il composes
        bool graded = false;
        for (uint32_t k = 0; k < 4; ++k) {
            const bool own = k > 0 && !s.pass_follow[k - 1];
            const uint32_t st = f.style_grade ? std::min(own ? s.pass_style[k - 1] : s.style, 2u) : 0u;
            const float iv = k == 0 ? s.intensity : s.pass_intensity[k - 1];
            ip.styles |= st << (2 * k);
            ip.inten[k] = iv;
            if (k < n && (st != 0 || iv != 1.0f)) graded = true;
        }
        if (graded != il_graded_last) { il_valid = false; il_graded_last = graded; }
        bool moved = false;
        const auto il = [&](uint32_t mode, uint32_t slot, bool move) {
            if (mode == 3u) { il_bcur ^= 1u; return; }   // (the proxy kept, il.comp flags & 268435456: no Base copy - the pair's turn only)
            if (mode == 0u) il_cur ^= 1u;
            Tex& base = il_bcur ? il_base2 : il_base;
            Tex& base_prev = il_bcur ? il_base : il_base2;
            const uint32_t d = il_cur, o = d ^ 1u;
            ip.mode = mode; ip.slot = slot;
            // the product's flags (nr_renodx.cpp: fresh history, vector test with its gradient prediction, box test, depth
            // disocclusion and dilation, edge, the fill and its depth-aware pyramid, the proxy kept, step 0's pyr1)
            ip.flags = (move ? 1u : 0u) | (mode == 0u && move ? 2u : 0u) | 268435456u | (move ? 8u : 0u) | (moved ? 64u : 0u) |
                       (mode == 0u && move ? 536870912u : 0u) | (mode == 0u && move && depth ? 1073741824u : 0u) |
                       (move ? 128u : 0u) | (depth && move ? 768u : 0u) | (inverted ? 1024u : 0u) | (graded ? 2048u : 0u) |
                       (mode == 1u && moved ? 65536u : 0u) | (move ? 524288u | 1048576u : 0u) | (move ? 33554432u : 0u) |
                       (mode == 4u && depth ? 67108864u : 0u) |
                       (stable && depth ? 131072u : 0u) |
                       (stable && (mode == 6u || mode == 1u) && il_lf_valid ? 16u : 0u);
            std::vector<d3d12_il::Bind> b;
            const auto rd = [&](uint32_t binding, const Tex& t) { b.push_back({binding, t.res, srv_format_of(t.format), false}); };
            const auto wr = [&](uint32_t binding, const Tex& t) { b.push_back({binding, t.res, uav_format_of(t.format), true}); };
            wr(0, in); wr(1, neural); wr(2, base);
            if (motion_direct) b.push_back({3, f.motion, srv_format_of(f.motion->GetDesc().Format), false});
            else rd(3, motion);
            wr(20, il_hist); rd(21, base_prev); wr(22, il_trust); wr(43, il_tdbg);
            if (mode != 0u) {
                for (uint32_t l = 0; l < 6; ++l) { rd(23 + l, il_pyr[l]); rd(37 + l, il_dpyr[l]); }
            }
            if (mode == 0u) {
                wr(44, il_pyr[0]);
                wr(45, il_dpyr[0]);
            }
            if (stable) { rd(46, il_lf[il_lfcur ^ 1u]); wr(47, il_lf[il_lfcur]); }
            if (depth) b.push_back({29, depth, dfmt, false});
            wr(30, il_depth[il_dcur]); rd(31, il_depth[il_dcur ^ 1u]);
            for (uint32_t i = 0; i < kIlSlots; ++i) {
                rd(4 + i, il_inc[o][i < n ? i : 0]);
                wr(12 + i, il_inc[d][i < n ? i : 0]);
            }
            const auto& k = il_kernels[pi][mode];
            std::vector<d3d12_il::Bind> used;
            for (const auto& x : b) {
                if (d3d12_il::has_binding(k, x.binding, x.write)) used.push_back(x);
            }
            dispatch_il(cmd, k, used, &ip, 24u, nx, ny);
        };
        const auto pyramid = [&] {
            uint32_t sw = NW, sh = NH;
            for (uint32_t l = 0; l < 6; ++l) {
                const uint32_t dw2 = (sw + 1) / 2, dh2 = (sh + 1) / 2;
                if (l == 0) { sw = dw2; sh = dh2; continue; }   // (step 0's pyr1 already made the first level)
                const uint32_t pp[7] = {sw, sh, dw2, dh2, l ? 1u : 0u, n, depth ? 1u : 0u};
                std::vector<d3d12_il::Bind> b;
                b.push_back({0, il_trust.res, srv_format_of(il_trust.format), false});
                for (uint32_t i = 0; i < kIlSlots; ++i)
                    if (i < n) b.push_back({1 + i, il_inc[il_cur][i].res, srv_format_of(il_inc[il_cur][i].format), false});
                const Tex& prev = il_pyr[l ? l - 1 : 1];
                const Tex& dprev = il_dpyr[l ? l - 1 : 1];
                if (l) b.push_back({9, prev.res, srv_format_of(prev.format), false});
                b.push_back({10, il_pyr[l].res, uav_format_of(il_pyr[l].format), true});
                b.push_back({11, il_depth[il_dcur].res, srv_format_of(il_depth[il_dcur].format), false});
                if (l) b.push_back({12, dprev.res, srv_format_of(dprev.format), false});
                b.push_back({13, il_dpyr[l].res, uav_format_of(il_dpyr[l].format), true});
                std::vector<d3d12_il::Bind> used;
                for (const auto& x : b) {
                    if (d3d12_il::has_binding(ilpp_kernel, x.binding, x.write)) used.push_back(x);
                }
                dispatch_il(cmd, ilpp_kernel, used, pp, 7u, (dw2 * n + 15) / 16, (dh2 + 15) / 16);
                sw = dw2; sh = dh2;
            }
        };
        // a slot's history (il_hist, made by step 0) in the network's history layout, for its network call (no layout
        // known: the network's own history)
        const auto history = [&]() -> uint64_t {
            const uint32_t hb = f.net_history_bytes;
            if (hb != 8u && hb != 16u) return 0;
            struct { uint32_t w, h, what, pad; float region[4]; } c{NW, NH, hb == 16u ? 1u : 0u, 0, {1.f, 1.f, 0.f, 0.f}};
            View srv[8] = {};
            srv[0] = tv(il_hist, false);
            View uav[14] = {};
            uav[0] = bv(lin_hist, uint64_t(lin_pitch(NW, hb)) * NH * hb, true, true);
            dispatch(cmd, kTexToBuf, srv, uav, &c, 8, (NW + 7) / 8, (NH + 7) / 8, false);
            uav_barrier(cmd);
            ensure(cmd, lin_hist, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            return lin_hist->GetGPUVirtualAddress();
        };
        // the game's depth read through the whole sequence (the network reads its own resampled copy), its state given
        // back on every way out
        struct DepthHeld {
            Impl& m; ID3D12GraphicsCommandList* cmd; ID3D12Resource* r; D3D12_RESOURCE_STATES st;
            ~DepthHeld() { if (r) m.ensure(cmd, r, st); }
        } held{*this, cmd, depth, f.depth_state};
        if (depth) ensure(cmd, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (depth) { il_dcur ^= 1u; il(5u, 0u, false); il_depth_const = false; }
        else if (stable && !il_depth_const) {
            il(5u, 0u, false); il_dcur ^= 1u; il(5u, 0u, false);
            il_depth_const = true;
        }
        if (!record_ok_) return false;
        const bool ext_hist_ok = f.net_history_bytes == 8u || f.net_history_bytes == 16u;
        if (reset || !il_valid) {
            il_lf_valid = false;
            for (uint32_t j = 0; j < n; ++j) {
                il(3u, j, false); il(0u, j, false);
                Tex& slot_in = il_bcur ? il_base2 : il_base;
                if (!record_ok_ || !pass_net(slot_in, j, true, graded, 0, ext_hist_ok)) return false;
                il(1u, j, false);
                il_last[j] = 0;
            }
            il_valid = true;
            il_frame = 1;
        } else {
            const uint32_t slot = il_frame % n;
            il(3u, slot, true);
            moved = true;
            il(0u, slot, true);
            pyramid();
            il(4u, slot, true);
            if (!record_ok_) return false;
            const uint64_t hva = history();
            Tex& slot_in = il_bcur ? il_base2 : il_base;
            if (!pass_net(slot_in, slot, false, graded, hva, hva != 0)) return false;
            if (stable) { il_lfcur ^= 1u; il(6u, slot, false); il_lf_valid = true; }
            il(1u, slot, false);
            il_last[slot] = il_frame++;
        }
        return record_ok_;
    }
};

bool Stage::Impl::build(std::string* why) {
    if (!d3d12_outer::create_outer_root_signature(device, &rs_outer, why)) return false;
    auto arch = d3d12_outer::shared_archive(why);
    if (!arch) return false;

    struct Entry { P p; const char* key; };
    const Entry common[] = {
        {kEncodeL, "renodx/legacy_encode"}, {kDecodeL, "renodx/legacy_decode"},
        {kReduceL, "renodx/legacy_reduce"}, {kReduceLU, "renodx/legacy_reduce_unshaped"},
        {kApplyL, "renodx/legacy_apply"}, {kDecodeReduceL, "renodx/legacy_decode_reduce"},
        {kCompose, "renodx/look_compose"}, {kUpsample, "renodx/look_upsample"}, {kBand, "renodx/look_band"},
        {kDebug, "renodx/renodx_debug"}, {kPrep, "renodx/renodx_prep"},
        {kStFbIn, "renodx/renodx_st_fb_in"}, {kStPrep, "renodx/renodx_st_prep"},
        {kStBox, "renodx/renodx_st_box"}, {kStApply, "renodx/renodx_st_apply"},
        {kTexToBuf, "renodx_aux/tex_to_buf"}, {kBufToTex, "renodx_aux/buf_to_tex"},
        {kCopyOut, "renodx_aux/copy_out"}, {kToMotion, "renodx_aux/to_motion"},
        {kToDepth, "renodx_aux/to_depth"}, {kSdrEncodeLin, "renodx_aux/sdr_encode_lin"},
        {kSdrDecodeLin, "renodx_aux/sdr_decode_lin"},
    };
    const Entry hdr_only[] = {
        {kLinearize, "renodx/v6_linearize"}, {kAutoscale, "renodx/v6_autoscale"},
        {kExposureScale, "renodx/v6_exposure_scale"}, {kEncode6, "renodx/v6_encode"},
        {kResolve6, "renodx/v6_resolve"}, {kReduce6, "renodx/v6_pedestal_reduce"},
        {kReduce6U, "renodx/v6_pedestal_reduce_unshaped"}, {kCommit6, "renodx/v6_commit"},
        {kCommitExposure, "renodx/v6_commit_exposure"}, {kResolveCommit6, "renodx/v6_resolve_commit"},
        {kResolve6Prep, "renodx/v6_resolve_prep"},
        {kResolveCommit6Prep, "renodx/v6_resolve_commit_prep"},
        {kReduce6UPrep, "renodx/v6_pedestal_reduce_unshaped_prep"},
        {kEncode6Lin, "renodx/v6_encode_lin"},
        {kResolve6Lin, "renodx/v6_resolve_lin"},
        {kResolveCommit6Lin, "renodx/v6_resolve_commit_lin"},
        {kPrepEncode, "renodx/renodx_prep_encode"},
    };
    for (const Entry& e : common) {
        if (!arch->build_kernel(device, e.key, &kernels[e.p], why)) return false;
    }
    if (hdr) {
        for (const Entry& e : hdr_only) {
            if (!arch->build_kernel(device, e.key, &kernels[e.p], why)) return false;
        }
    }
    // The transport folded into the decode / the last resolve (nr_renodx.cpp's; NR_RDX_UP_FUSED=0: apart). Optional:
    // without them the stage runs look_upsample into P_up / N'_up as before.
    static const bool up_fused_off = [] { const char* e = std::getenv("NR_RDX_UP_FUSED"); return e && e[0] == '0'; }();
    if (!up_fused_off) {
        std::string up_why;
        const Entry up = hdr ? Entry{kResolveCommit6Up, "renodx/v6_resolve_commit_up"}
                             : Entry{kDecodeUpL, "renodx/legacy_decode_up"};
        if (!arch->build_kernel(device, up.key, &kernels[up.p], &up_why)) {
            nr::pe::log("[nr] RenoDX stage (D3D12): %s not made (%s); the transport runs apart", up.key, up_why.c_str());
        }
    }
    std::string il_why;
    if (!load_il(*arch, &il_why)) {
        nr::pe::log("[nr] RenoDX stage (D3D12): Interleaved's programs are not available (%s): Interleaved cannot run",
                    il_why.c_str());
    }
    if (!outer_pool.init(
            device, 512,
            [](const nr::gpu::Ticket& t) { return nr::pe::d3d12_direct::completed(t); },
            [this](IUnknown* r) { if (retire) retire(r); else release(r); }, why)) {
        return false;
    }
    {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 512; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&zero_upload))) || !zero_upload) {
            *why = "renodx: the zero upload buffer could not be made";
            return false;
        }
        void* zm = nullptr;
        if (FAILED(zero_upload->Map(0, nullptr, &zm)) || !zm) {
            *why = "renodx: the zero upload buffer could not be mapped";
            return false;
        }
        std::memset(zm, 0, 512);
        zero_upload->Unmap(0, nullptr);
        nr::vram::track_d3d12(device, zero_upload, nr::vram::kRenoDX);
    }

    const uint32_t W = cfg.width, H = cfg.height, NW = cfg.nr_width, NH = cfg.nr_height;
    const DXGI_FORMAT frame = cfg.frame_format;
    decoded_storage = typed_uav(frame);
    decoded_load = typed_uav_load(frame);
    if (!decoded_storage) decoded = make_tex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true, why);
    block_mean = make_tex((W + 31) / 32, (H + 31) / 32, DXGI_FORMAT_R32G32B32A32_FLOAT, true, why);
    // (proxy / neural: made on demand when a path other than fast_lin runs; P_up / N'_up: record())
    if ((!decoded_storage && !decoded.res) || !block_mean.res) {
        *why = "renodx: a stage texture could not be made";
        return false;
    }
    trace = make_buffer(16 * 1024, why);
    lin_proxy = make_buffer(uint64_t(lin_pitch(NW, 8)) * NH * 8, why);
    lin_neural = make_buffer(uint64_t(lin_pitch(NW, 8)) * NH * 8, why);
    lin_mv = make_buffer(uint64_t(lin_pitch(NW, 8)) * NH * 8, why);
    lin_depth = make_buffer(256, why);
    if (!trace || !lin_proxy || !lin_neural || !lin_mv || !lin_depth) return false;
    neutral_mv = make_tex(1, 1, DXGI_FORMAT_R16G16_FLOAT, false, why);   // zero: the heap guarantees it
    if (!neutral_mv.res) return false;
    looks.resize(1);
    if (hdr) {
        // The v6 codec's surfaces: the scale and the commit 1x1 RGBA32F; the work images (RGBA16F like the proxy) when
        // a frame's path first needs them (record(), nr_renodx.cpp's "made the first time one needs them").
        scale = make_tex(1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, true, why);
        commit = make_tex(1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, true, why);
        if (!scale.res || !commit.res) {
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
                           bool motion_copied, const Frame& fr, bool reset, bool& shaped_out, bool& transport_out,
                           bool up_fused) {
    const uint32_t W = cfg.width, H = cfg.height, NW = cfg.nr_width, NH = cfg.nr_height;
    transport_out = (NW < W || NH < H) && look::transport_wanted(ls, NW, NH, W, H);
    shaped_out = look::dispatched(ls, transport_out);
    PassLook& pl = looks[pass];
    const bool temporal = shaped_out && ls.stabilize != 0;
    if (!temporal) {
        pl.hist.valid = false;
        if (pl.history[0]) { drop_buf(pl.history[0]); drop_buf(pl.history[1]); }
    }
    if (!shaped_out) return;
    if (!look_out.res) {
        std::string why;
        look_out = make_tex(NW, NH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, &why);
        if (!look_out.res) { shaped_out = false; transport_out = false; return; }
    }
    if (look::bands_wanted(ls) && !band_a.res) {
        const uint32_t bw = (NW + 1) / 2, bh = (NH + 1) / 2;
        std::string why;
        band_a = make_tex(bw, bh, DXGI_FORMAT_R32G32B32A32_FLOAT, true, &why);
        band_b = make_tex(bw, bh, DXGI_FORMAT_R32G32B32A32_FLOAT, true, &why);
        band_max_a = make_tex(bw, bh, DXGI_FORMAT_R32G32_FLOAT, true, &why);
        band_max_b = make_tex(bw, bh, DXGI_FORMAT_R32G32_FLOAT, true, &why);
        if (!band_a.res || !band_b.res || !band_max_a.res || !band_max_b.res) {
            for (Tex* t : {&band_a, &band_b, &band_max_a, &band_max_b}) drop(*t);
            shaped_out = transport_out = false;   // the caller then resolves the network's own answer
            return;
        }
    }
    if (temporal && !pl.history[0]) {
        pl.history[0] = make_buffer(uint64_t(NW) * NH * 12, nullptr);
        pl.history[1] = make_buffer(uint64_t(NW) * NH * 12, nullptr);
        if (!pl.history[0] || !pl.history[1]) {
            drop_buf(pl.history[0]); drop_buf(pl.history[1]);
            pl.history[0] = pl.history[1] = nullptr;
            shaped_out = transport_out = false;
            return;
        }
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
    Tex& mv = motion.res ? motion : proxy;
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
    if (transport_out && !up_fused) {   // RecordUpsample: P_up and N'_up at the frame's extent (fused: the decode makes them)
        look::Constants up = lc;
        up.size[0] = W; up.size[1] = H;
        View srv[8] = {}, uav[14] = {};
        look_set(look_out, nullptr, nullptr, srv, uav);
        // look_out is read here (t2): its unused u0 slot takes P_up instead, or the dispatch would put the image it
        // reads into UNORDERED_ACCESS (the Vulkan stage's GENERAL has no such conflict)
        uav[0] = tv(up_proxy, true);
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
// The exposure texture's float view (a TYPELESS resource read as the float of its family, as the Vulkan route reads it)
static DXGI_FORMAT exposure_view_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        default: return srv_format_of(f);
    }
}
static bool exposure_format_readable(DXGI_FORMAT f) {
    f = exposure_view_format(f);
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
    }
    cmd->ResourceBarrier(2, b);
    states[f.colour] = D3D12_RESOURCE_STATE_COPY_SOURCE;
    states[f.output] = D3D12_RESOURCE_STATE_COPY_DEST;
    cmd->CopyResource(f.output, f.colour);
    for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    cmd->ResourceBarrier(2, b);
    states[f.colour] = f.colour_state;
    states[f.output] = f.output_state;
    cmd_touched_ = true;
}

uint32_t Stage::Impl::record(ID3D12GraphicsCommandList* cmd, const Frame& f, const Frame::Net& net) {
    record_ok_ = true;
    net_ran_ = false;
    cmd_touched_ = false;
    frame_dispatches_ = 0;
    rs_outer_bound_ = false;
    current_ticket = nr::pe::d3d12_direct::book(cmd);
    if (!current_ticket) {
        record_ok_ = false;
        return 0;
    }
    nr::pe::d3d12_direct::Scope direct_scope(cmd);
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

    // The stack's own images and look histories, as this frame's depth needs them.
    if (hdr && passes > 1 && !work_b.res) {
        std::string why;
        work_b = make_tex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true, &why);
        if (!work_b.res) { record_ok_ = false; return 0; }
    }
    const bool debug_show = s.debug_view >= 1u && s.debug_view <= 3u;
    const bool sdr_restore = s.codec_mode != 0u && s.sdr_black_restore != 0u;
    const float restore_mode = float(s.sdr_black_restore == 3u ? 0u : s.sdr_black_restore);
    const bool direct_out = decoded_storage && !debug_show && (hdr != 0 || !sdr_restore);
    if ((!direct_out || (!hdr && passes > 1)) && !decoded.res) {
        std::string why;
        decoded = make_tex(W, H, decoded_storage ? cfg.frame_format : DXGI_FORMAT_R16G16B16A16_FLOAT, true, &why);
        if (!decoded.res) { record_ok_ = false; return 0; }
    }
    if (!hdr && passes > 1 && !decoded_alt.res) {
        std::string why;
        decoded_alt = make_tex(W, H, decoded.format, true, &why);
        if (!decoded_alt.res) { record_ok_ = false; return 0; }
    }
    if (passes == 1 && (work_b.res || decoded_alt.res)) {   // one pass: the stack's images go (nr_renodx.cpp)
        drop(work_b);
        drop(decoded_alt);
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
    // NRFastMethod: 0 Interleaved, 1 Extrapolation, 2 Feedback, 3 Hybrid, 4 Edit strength. `fm`: the others in this
    // stage's own order (0 Extrapolation, 1 Feedback, 2 Hybrid, 3 Edit strength). Interleaved (nr_renodx.cpp's) needs the
    // motion vectors and its programs: without them it does not run - one network pass, NR Control says why.
    const bool il_asked = s.passes_mode == 1u && s.fast_method == 0u;
    const uint32_t il_n = std::clamp(s.fast_interleaved_passes, 2u, kIlSlots);
    const uint32_t fm = s.fast_method == 0u ? 0u : std::min(s.fast_method, 4u) - 1u;
    const uint32_t fast_slot = std::min(fm, 2u);
    const bool fast_motion = have_motion && f.fast_motion_available;
    const bool il_on = il_asked && fast_motion && il_loaded;
    // Feedback / Hybrid need the motion vectors too: without them they do not run (one network pass, said)
    const bool fast_history = !il_asked && (fm == 1u || fm == 2u);
    const bool fb_missing = s.passes_mode == 1u && fast_history && !fast_motion;
    const bool fast = s.passes_mode == 1u && !il_asked && !fb_missing &&
                      (fm == 3u ? s.fast_edit_strength != 1.f : s.fast_passes[fast_slot] > 1.f);
    const bool fast_feedback = fast && fast_history;
    facts.fast_passes = s.passes_mode == 1u ? (il_asked ? (il_on ? float(il_n) : 0.f) : fb_missing ? 0.f
                                               : fm == 3u ? s.fast_edit_strength : s.fast_passes[fast_slot]) : 0.f;
    facts.motion = fast_motion;
    facts.interleaved = il_on;
    facts.il_missing = il_asked && !il_on ? (!fast_motion ? 1u : 3u) : 0u;
    if (facts.il_missing && !il_missing_logged) {
        il_missing_logged = true;
        nr::pe::log("[nr] RenoDX stage (D3D12): Passes (fast) Interleaved cannot run: %s (one network pass)",
                    facts.il_missing == 1u ? "no motion vectors" : "its programs are not available");
    } else if (il_on) {
        il_missing_logged = false;
    }
    // The edge-aware transport below the frame's extent, and whether the decode / last resolve makes P_up and N'_up
    // itself (nr_renodx.cpp's up_fused: SDR without the black-level restore; HDR one fused pass without Preprocess's
    // resolve; never under a debug view)
    const bool transport_now = (NW < W || NH < H) && look::transport_wanted(ls, NW, NH, W, H);
    const bool prep_resolve_now = prep_on && hdr && pp.inverse;
    const bool up_fused = transport_now && !debug_show &&
        (hdr ? kernels[kResolveCommit6Up].pso && !fuse_off_env() && !frame_pedestal_removal(s, hdr, units) && passes == 1 && !prep_resolve_now
             : kernels[kDecodeUpL].pso && !sdr_restore);
    D3D12_RESOURCE_DESC motion_desc{};
    uint32_t motion_rx = 0, motion_ry = 0, motion_rw = 0, motion_rh = 0;
    if (have_motion) {
        motion_desc = f.motion->GetDesc();
        motion_rx = std::min(f.motion_x, uint32_t(motion_desc.Width));
        motion_ry = std::min(f.motion_y, uint32_t(motion_desc.Height));
        motion_rw = std::min(f.motion_w, uint32_t(motion_desc.Width) - motion_rx);
        motion_rh = std::min(f.motion_h, uint32_t(motion_desc.Height) - motion_ry);
    }
    const bool il_motion_direct = il_on && motion_rw > 0 && motion_rh > 0 &&
                                  srv_format_of(motion_desc.Format) != DXGI_FORMAT_UNKNOWN &&
                                  f.motion != f.colour && f.motion != f.output && (!f.depth || f.motion != f.depth);
    const bool motion_wanted = (ls.stabilize == 2u && have_motion && look::dispatched(ls, false)) ||
                               fast_feedback || (il_on && !il_motion_direct);
    const D3D12_RESOURCE_DESC src_desc = f.colour->GetDesc();
    const DXGI_FORMAT src_fmt = srv_format_of(src_desc.Format);
    const bool in_srgb = src_fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || src_fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool linear_float_src = (src_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                                   src_fmt == DXGI_FORMAT_R11G11B10_FLOAT ||
                                   src_fmt == DXGI_FORMAT_R32G32B32A32_FLOAT) &&
                                  src_desc.Width >= W && src_desc.Height >= H;
    {
        std::string why;
        if (motion_wanted && have_motion) {
            if (motion.res && (motion.w != f.motion_w || motion.h != f.motion_h || motion.format != motion_desc.Format)) drop(motion);
            if (!motion.res) motion = make_tex(f.motion_w, f.motion_h, motion_desc.Format, false, &why);
            if (!motion.res) { record_ok_ = false; return 0; }
        } else if (!motion_wanted && motion.res) {
            drop(motion);
        }
        // Passes (fast)'s work sets (nr_renodx.cpp ensure_st): made here, before the frame's first command, so that a
        // failure leaves the caller's list as it came; the feedback's go when the method is not Feedback / Hybrid.
        if (fast) {
            if (!ensure_st(&why) || (fast_history && !ensure_st_feedback(&why))) {
                nr::pe::log("[nr] RenoDX stage (D3D12): Passes (fast)'s images could not be made: %s", why.c_str());
                record_ok_ = false;
                return 0;
            }
            if (!fast_history) drop_st_feedback();
        }
        // Interleaved's images (nr_renodx.cpp ensure_il), the same way; they go when it does not run
        if (il_on) {
            if (!ensure_il(NW, NH, il_n, f.net_history_bytes, selected_il_preset(s), &why)) {
                nr::pe::log("[nr] RenoDX stage (D3D12): Passes (fast) Interleaved's images could not be made: %s", why.c_str());
                record_ok_ = false;
                return 0;
            }
        } else if (il_slots) {
            drop_il();
        }
        // The images this frame's path writes or reads (nr_renodx.cpp's): work0 unless the linear frame is read as it
        // is, work_a for a stack, an unfused commit or a zero-strength first pass, P_up / N'_up for the transport apart.
        {
            float t0 = 0.f, c0 = 0.f;
            strengths(settings, 0, &t0, &c0);
            const bool fuse_now = hdr && !fuse_off_env() && !frame_pedestal_removal(s, hdr, units);
            const bool w0_now = hdr && f.colour != f.output && !prep_on && f6.encoding == 1u && linear_float_src;
            if (hdr && !w0_now && !work0.res) work0 = make_tex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true, &why);
            if (hdr && !w0_now && !work0.res) { record_ok_ = false; return 0; }
            if (hdr && (passes > 1 || !fuse_now || !(t0 > 0.f || c0 > 0.f)) && !work_a.res)
                work_a = make_tex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true, &why);
            if (hdr && (passes > 1 || !fuse_now || !(t0 > 0.f || c0 > 0.f)) && !work_a.res) { record_ok_ = false; return 0; }
            if (transport_now && !up_fused && !up_proxy.res) {
                up_proxy = make_tex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true, &why);
                up_neural = make_tex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true, &why);
                if (!up_proxy.res || !up_neural.res) { drop(up_proxy); drop(up_neural); record_ok_ = false; return 0; }
            }
        }
        if (hdr && f.exposure) {
            const DXGI_FORMAT ef = exposure_view_format(f.exposure->GetDesc().Format);   // the copy's own, typed
            const bool readable = exposure_format_readable(ef);
            if (readable && select_feed(s, hdr, units, readable, f.auto_exposure || !f.create_flags_known) == kFeedTexture) {
                if (!exposure_1x1.res || exposure_1x1.format != ef) {
                    drop(exposure_1x1);
                    exposure_1x1 = make_tex(1, 1, ef, false, &why);
                }
                if (!exposure_1x1.res) { record_ok_ = false; return 0; }
            }
        }
        const bool net_wants_depth_pre = have_motion && f.depth && !f.motion_without_depth;
        const uint64_t depth_bytes_need = uint64_t(lin_pitch(NW, 16)) * NH * 16;
        if (net_wants_depth_pre && (!lin_depth || lin_depth->GetDesc().Width < depth_bytes_need)) {
            if (ID3D12Resource* nd = make_buffer(depth_bytes_need, nullptr)) { drop_buf(lin_depth); lin_depth = nd; }
        }
        const bool look_wanted_pre = look::dispatched(ls, transport_now);
        const bool area_enc_pre = ls.upsample == look::kUpsampleAreaInput && transport_now;
        const bool sdr_fast_lin_pre = !v6 && passes == 1 && !prep_on && !fast && !il_on && !look_wanted_pre && !sdr_restore && !debug_show &&
                                      !area_enc_pre && (s.codec_mode == 0u || s.transfer_strength == 1.0f) && NW == W && NH == H;
        const bool v6_fast_lin_pre = v6 && !prep_on && !fast && !il_on && !look_wanted_pre && !debug_show;
        const bool fast_lin_pre = sdr_fast_lin_pre || v6_fast_lin_pre;
        if (!fast_lin_pre) {
            if (!proxy.res) proxy = make_tex(NW, NH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, &why);
            if (!neural.res) neural = make_tex(NW, NH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, &why);
            if (!proxy.res || !neural.res) { drop(proxy); drop(neural); record_ok_ = false; return 0; }
        } else if (proxy.res || neural.res) {
            drop(proxy);
            drop(neural);
        }
        const bool direct_in_pre = in_srgb ? (f.colour != f.output && sdr_fast_lin_pre)
                                           : (f.colour != f.output || v6 || sdr_fast_lin_pre);
        if (!direct_in_pre && !original.res) {
            original = make_tex(W, H, cfg.frame_format, false, &why);
            if (!original.res) { record_ok_ = false; return 0; }
        }
        const uint32_t slot_budget = 8u + passes * (26u + (il_on ? il_n * 6u + 8u : 0u));
        std::string pool_err;
        if (!outer_pool.reserve(slot_budget, &pool_err)) {
            record_ok_ = false;
            nr::pe::log("[nr] RenoDX stage: %s", pool_err.empty() ? "no outer descriptor slot" : pool_err.c_str());
            return 0;
        }
    }

    // Preprocess's state cleared when it is new - after every allocation above, the frame's first command.
    if (prep_on) {
        if (prep_state_fresh) {
            ensure(cmd, prep_state, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyBufferRegion(prep_state, 0, zero_upload, 0, 16);
            ensure(cmd, prep_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            uav_barrier(cmd);
            prep_state_fresh = false;
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
    // The _prep resolve programs (the ratio taken behind Preprocess's curve) with its Inverse on, as the Vulkan stage
    // (prep_resolve); ratio_resolve stays make_v6's own argument.
    const bool prep_resolve = prep_on && hdr && pp.inverse;
    const float floor_behind = prep_resolve ? prep_floor(prep_inverse_curve, prep_push.k, prep_push.contrast,
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
        srv[4] = prep_scale_view;
        View uav[14] = {};
        uav[0] = bv(prep_state, 16, true, true);
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
    facts.fast_fallback = fb_missing;
    if (!fast && st_fineh.res) {   // Passes (fast) off: its images go (nr_renodx.cpp's retire)
        for (Tex* t : {&st_fineh, &st_fbadd, &st_d0, &st_d1, &st_t0, &st_t1, &st_g0, &st_g1, &st_f0[0], &st_f0[1],
                       &st_f1[0], &st_f1[1]})
            drop(*t);
        st_valid = false;
    }

    // The game's motion vectors, for Detail stability / Passes (fast) Feedback / Interleaved fallback: the subrect copied
    // to the origin of `motion.res` when needed (Interleaved normally reads `f.motion` in place via mbase/mgrid).
    bool motion_copied = false;
    if (motion_wanted && have_motion) {
        ensure(cmd, f.motion, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ensure(cmd, motion.res, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = f.motion;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        dst.pResource = motion.res;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        // The caller reports the region; a box outside the source, or an empty one, is not a copy D3D12 accepts.
        D3D12_BOX box{};
        box.left = motion_rx;
        box.top = motion_ry;
        box.right = motion_rx + motion_rw;
        box.bottom = motion_ry + motion_rh;
        box.back = 1;
        if (!motion_rw || !motion_rh) {
            // An empty or out-of-bounds region is not a copy D3D12 accepts: skip it and let the look run without motion.
            static bool said = false;
            if (!said) { said = true; nr::pe::log("[nr] RenoDX stage (D3D12): the motion region is outside the guide; the look runs without motion"); }
        } else {
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
            cmd_touched_ = true;
            motion_copied = true;
        }
        ensure(cmd, motion.res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    const bool recreate_epoch = recreate || history_reset;   // the governor's reset-epoch snap (PlanFrameScale)
    const bool reset_all = f.reset || history_reset;
    if (reset_all || !fast_feedback) st_valid = false;
    history_reset = false;
    recreate = false;

    struct GuideRestore {
        Impl& m;
        ID3D12GraphicsCommandList* cmd;
        ID3D12Resource* motion = nullptr;
        D3D12_RESOURCE_STATES motion_st = D3D12_RESOURCE_STATE_COMMON;
        ID3D12Resource* depth = nullptr;
        D3D12_RESOURCE_STATES depth_st = D3D12_RESOURCE_STATE_COMMON;
        ~GuideRestore() {
            if (motion) m.ensure(cmd, motion, motion_st);
            if (depth) m.ensure(cmd, depth, depth_st);
        }
    } guide_restore{*this, cmd};

    const bool net_wants_depth = have_motion && f.depth && !f.motion_without_depth;
    const bool have_depth = net_wants_depth && lin_depth && lin_depth->GetDesc().Width >= uint64_t(lin_pitch(NW, 16)) * NH * 16;

    // The network's guides, onto the network's extent (once a frame): kToMotion, kToDepth, and the initial encode
    // write independent buffers and overlap until pass_net's UAV barrier.
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
            uav[0] = bv(lin_mv, uint64_t(lin_pitch(NW, 8)) * NH * 8, true, true);
            dispatch(cmd, kToMotion, srv, uav, &c, sizeof c / 4, (NW + 7) / 8, (NH + 7) / 8, false);
            if (!record_ok_) return 0;
            if (f.motion != f.colour && f.motion != f.output && (!have_depth || f.motion != f.depth)) {
                guide_restore.motion = f.motion;
                guide_restore.motion_st = f.motion_state;
            } else {
                ensure(cmd, f.motion, f.motion_state);
            }
            lin_mv_dirty = true;
        } else if (lin_mv_dirty) {
            View srv[8] = {};
            srv[0] = tv(neutral_mv, false);
            View uav[14] = {};
            uav[0] = bv(lin_mv, uint64_t(lin_pitch(NW, 8)) * NH * 8, true, true);
            dispatch(cmd, kToMotion, srv, uav, &c, sizeof c / 4, (NW + 7) / 8, (NH + 7) / 8, false);
            if (!record_ok_) return 0;
            lin_mv_dirty = false;
        }
    }
    if (have_depth) {
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
        uav[0] = bv(lin_depth, uint64_t(lin_pitch(NW, 16)) * NH * 16, false, true);
        dispatch(cmd, kToDepth, srv, uav, &c, sizeof c / 4, (NW + 7) / 8, (NH + 7) / 8, false);
        if (!record_ok_) return 0;
        if (f.depth != f.colour && f.depth != f.output) {
            guide_restore.depth = f.depth;
            guide_restore.depth_st = f.depth_state;
        } else {
            ensure(cmd, f.depth, f.depth_state);
        }
    }

    const bool look_wanted = look::dispatched(ls, transport_now);
    const bool area_enc = ls.upsample == look::kUpsampleAreaInput && transport_now;
    const bool sdr_fast_lin = !v6 && passes == 1 && !prep_on && !fast && !il_on && !look_wanted && !sdr_restore && !debug_show &&
                              !area_enc && (s.codec_mode == 0u || s.transfer_strength == 1.0f) && NW == W && NH == H;
    const bool v6_fast_lin = v6 && !prep_on && !fast && !il_on && !look_wanted && !debug_show;
    const bool fast_lin = sdr_fast_lin || v6_fast_lin;

    // 1. The frame input: read in place when colour != output (including _SRGB on sdr_fast_lin), on v6 (where
    // linearize copies into work0 first), or on the SDR linear-buffer fast path; otherwise copied into `original`
    // so SDR decode / multi-pass / look can read `original`.
    const bool direct_in = in_srgb ? (f.colour != f.output && sdr_fast_lin)
                                   : (f.colour != f.output || v6 || sdr_fast_lin);
    Tex in_src{f.colour, W, H, src_fmt};
    if (!direct_in) {
        in_src = original;
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
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        }
        transition(cmd, f.colour, D3D12_RESOURCE_STATE_COPY_SOURCE, f.colour_state);
        ensure(cmd, original.res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    const bool w0_frame = v6 && f.colour != f.output && !prep_on && f6.encoding == 1u && linear_float_src;
    Tex& w0 = w0_frame ? in_src : work0;

    if (v6) {
        // 2a. linearize: in_src -> work0, the linear source everything after reads (skipped when w0_frame aliases in_src).
        if (!w0_frame) {
            const V6Constants lin = make_v6(s, f6, W, H, W, H, W, H, W, H, 0.f, 0.f, 0.f);
            View srv[8] = {};
            srv[0] = srv[1] = srv[2] = srv[3] = tv(in_src, false);
            View uav[14] = {};
            uav[0] = tv(work0, true);
            dispatch(cmd, kLinearize, srv, uav, &lin, sizeof lin / 4, gx, gy);
            if (!record_ok_) return 0;
            if (direct_in && f.colour == f.output) ensure(cmd, f.colour, f.colour_state);
        }
        // with Preprocess's exposure the frame is metered after its forward half; without it before (RenoDX's scale)
        if (prep_on && !prep_normalise) {
            prep_forward_hdr(w0, true);
            if (!record_ok_) return 0;
        }
        Tex& metered = (prep_on && !prep_normalise) ? work_p : w0;
        feed = kFeedV1;
        if (!units.absolute && !prep_exposure) {
            // 2b. this frame's scale texel: the v1 meter and the governor (feed v1).
            if (!commit_cleared) {
                ensure(cmd, commit.res, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = commit.res;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                src.pResource = zero_upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Offset = 0;
                src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                src.PlacedFootprint.Footprint.Width = 1;
                src.PlacedFootprint.Footprint.Height = 1;
                src.PlacedFootprint.Footprint.Depth = 1;
                src.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
                const D3D12_BOX box{0, 0, 0, 1, 1, 1};
                cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                ensure(cmd, commit.res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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
                if (!record_ok_) return 0;
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
                if (!record_ok_) return 0;
            }
        }
        if (prep_normalise) prep_forward_hdr(w0, false);   // after RenoDX's scale, which it divides by
        if (!record_ok_) return 0;
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
    for (uint32_t k = 0; k < passes && net_ok && record_ok_; ++k) {
        float transfer = 1.f, color = 1.f;
        strengths(settings, k, &transfer, &color);
        Tex& ref = k == 0 ? (v6 ? w0 : in_src) : stack_out(k - 1);
        Tex& write = stack_out(k);
        if (v6 && transfer <= 0.f && color <= 0.f) {
            // An explicit zero-strength pass carries the running image across (kCopyOut if source format/size differs, e.g. w0_frame).
            const D3D12_RESOURCE_DESC rd = ref.res->GetDesc(), wd = write.res->GetDesc();
            if (rd.Format == wd.Format && rd.Width == wd.Width && rd.Height == wd.Height) {
                ensure(cmd, ref.res, D3D12_RESOURCE_STATE_COPY_SOURCE);
                ensure(cmd, write.res, D3D12_RESOURCE_STATE_COPY_DEST);
                cmd->CopyResource(write.res, ref.res);
                uav_barrier(cmd);
            } else {
                struct { uint32_t w, h, what, pad; float region[4]; } c{W, H, 0, 0, {}};
                View srv[8] = {};
                srv[0] = tv(ref, false);
                View uav[14] = {};
                uav[0] = tv(write, true);
                dispatch(cmd, kCopyOut, srv, uav, &c, 8, (W + 7) / 8, (H + 7) / 8);
                if (!record_ok_) return 0;
            }
            ensure(cmd, write.res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            executed = int32_t(k);
            final_transfer = final_color = 0.f;
            final_shaped = false;
            continue;
        }
        // 3a. encode: the running image -> the proxy at the network's extent.
        if (v6) {
            if (prep_on && k > 0) {
                prep_forward_hdr(ref, false);
                if (!record_ok_) return 0;
            }
            if (prep_on && pp.curve != 0) {
                // Preprocess's curve in place of RenoDX's: the same footprint and sRGB, no toe or shoulder.
                const uint32_t push[4] = {NW, NH, W, H};
                View srv[8] = {};
                srv[0] = tv(work_p, false);
                View uav[14] = {};
                uav[4] = tv(proxy, true);
                dispatch(cmd, kPrepEncode, srv, uav, push, 4, nx, ny);
            } else {
                V6Constants enc = make_v6(s, f6, NW, NH, W, H, NW, NH, NW, NH, transfer, color, 0.f);
                if (prep_normalise) enc.divisor = 1.f;   // Preprocess's forward half divided already
                View srv[8] = {};
                srv[0] = srv[1] = srv[2] = tv(prep_on ? work_p : ref, false);
                srv[3] = units.absolute ? tv(w0, false) : tv(scale, false);
                View uav[14] = {};
                if (v6_fast_lin) {
                    uav[1] = bv(lin_proxy, uint64_t(lin_pitch(NW, 8)) * NH * 8, true, true);
                    dispatch(cmd, kEncode6Lin, srv, uav, &enc, sizeof enc / 4, nx, ny, false);
                } else {
                    uav[0] = tv(proxy, true);
                    dispatch(cmd, kEncode6, srv, uav, &enc, sizeof enc / 4, nx, ny);
                }
            }
        } else if (sdr_fast_lin) {
            struct { uint32_t w, h, what, pad; uint32_t region[4]; } c{NW, NH, 0, (k == 0 && direct_in && in_srgb) ? 1u : 0u, {W, H, 0, 0}};
            View srv[8] = {};
            srv[0] = tv(ref, false);
            View uav[14] = {};
            uav[0] = bv(lin_proxy, uint64_t(lin_pitch(NW, 8)) * NH * 8, true, true);
            dispatch(cmd, kSdrEncodeLin, srv, uav, &c, 8, (NW + 7) / 8, (NH + 7) / 8, false);
        } else {
            LegacyConstants c{};
            c.size[0] = NW; c.size[1] = NH; c.source_size[0] = W; c.source_size[1] = H;
            c.proxy_size[0] = c.neural_size[0] = NW; c.proxy_size[1] = c.neural_size[1] = NH;
            c.paper_white_scale = s.paper_white_scale; c.transfer_strength = transfer; c.color_strength = color;
            c.hdr = 0; c.diffuse_white_nits = s.diffuse_white_nits;
            if (ls.upsample == look::kUpsampleAreaInput && transport_now)
                c.padding[0] = look::kAreaEncode;
            View srv[8] = {};
            srv[0] = srv[1] = srv[2] = srv[3] = tv(ref, false);
            View uav[14] = {};
            uav[0] = tv(proxy, true);
            dispatch(cmd, kEncodeL, srv, uav, &c, sizeof c / 4, nx, ny);
        }
        if (prep_on && !v6) {   // the network's input in place (mode 1), the meter once a frame
            View srv[8] = {};
            srv[4] = tv(block_mean, false);   // RenoDX's scale (binding 4): never read on SDR
            View uav[14] = {};
            uav[0] = bv(prep_state, 16, true, true);
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
        if (!record_ok_) return 0;
        // Work in the input the network actually reads, including SDR Preprocess APART.
        Tex& fast_input = prep_apart ? prep_keep : proxy;
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
            if (!record_ok_) return 0;
        }
        // 3b. the network: the input into a linear buffer, the D3D12 network, its answer back into `neural` (`kk` the pass
        // whose controls and history it runs with; Interleaved runs it once a slot, `hist_va` that slot's history, `raw`
        // plain - Intensity 1, no Style grade)
        const auto pass_net = [&](Tex& net_in, uint32_t kk, bool reset, bool raw, uint64_t hist_va, bool ext_hist) -> bool {
            if (!record_ok_) return false;
            if (!fast_lin) {
                if ((NW & 31u) == 0) {
                    ensure(cmd, net_in.res, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    ensure(cmd, lin_proxy, D3D12_RESOURCE_STATE_COPY_DEST);
                    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                    src.pResource = net_in.res;
                    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    src.SubresourceIndex = 0;
                    dst.pResource = lin_proxy;
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    dst.PlacedFootprint.Footprint.Width = NW;
                    dst.PlacedFootprint.Footprint.Height = NH;
                    dst.PlacedFootprint.Footprint.Depth = 1;
                    dst.PlacedFootprint.Footprint.RowPitch = NW * 8;
                    D3D12_BOX box{0, 0, 0, NW, NH, 1};
                    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                } else {
                    struct { uint32_t w, h, what, pad; float region[4]; } c{NW, NH, 0, 0, {1.f, 1.f, 0.f, 0.f}};
                    View srv[8] = {};
                    srv[0] = tv(net_in, false);
                    View uav[14] = {};
                    uav[0] = bv(lin_proxy, uint64_t(lin_pitch(NW, 8)) * NH * 8, true, true);
                    dispatch(cmd, kTexToBuf, srv, uav, &c, 8, (NW + 7) / 8, (NH + 7) / 8, false);
                    if (!record_ok_) return false;
                }
            }
            // Pass k's controls, RenoDX's (record_renodx.cpp's pass_controls): pass 1's, or pass k's once its
            // "Same as pass 1" is off; with NRSkinIndependent and the Character mask on, Structure at most 1 and a
            // stock Skin sent as 1 (SentSkinSteering).
            nr::D3d12NetControls ck = f.base_controls;
            ck.history_strength = 1.f;
            {
                const uint32_t ki = std::min(kk, 3u);
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
            // RenoDX's guides: NRDepthMode overrides the game's depth direction, NRMVecScaleX / Y scale its motion
            // vectors (the network's and the look's).
            NetTemporal nt;
            nt.motion_va = lin_mv->GetGPUVirtualAddress();
            nt.motion_scale_x = f.motion_scale_x * s.mvec_scale_x;
            nt.motion_scale_y = f.motion_scale_y * s.mvec_scale_y;
            nt.depth_va = have_depth ? lin_depth->GetGPUVirtualAddress() : 0;
            nt.have_depth = have_depth;
            nt.depth_inverted = f.depth_inverted;
            if (s.depth_mode == 1u) nt.depth_inverted = false;
            if (s.depth_mode == 2u) nt.depth_inverted = true;
            nt.reset = reset;
            // (Interleaved: each slot its own feature, as each legacy pass has its own history)
            nt.feature = (cfg.feature || passes > 1 || il_on) ? stack_feature(cfg.feature ? cfg.feature : kUnnamedStack, kk) : 0;
            nt.history_va = hist_va;
            nt.external_history = ext_hist;
            nt.have_motion = true;
            // The net reaches these by raw address: lin_proxy and the guides are read (UAV), lin_neural is written (UAV).
            // A real barrier each frame entry - the table is not evidence that the GPU moved.
            ensure(cmd, lin_proxy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ensure(cmd, lin_neural, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ensure(cmd, lin_mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ensure(cmd, lin_depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            uav_barrier(cmd);   // the guides and lin_proxy are read by the net by raw address
            if (raw) { ck.intensity = 1.f; ck.no_grade = true; }   // (Interleaved composes each pass's Intensity and Style itself)
            rs_outer_bound_ = false;
            cmd_touched_ = true;
            const bool net_ok_here = net(kk, nt.feature, ck, nt, lin_proxy, lin_neural);
            net_ran_ = net_ran_ || net_ok_here;
            if (!net_ok_here) {
                if (!net_failed_logged) {
                    net_failed_logged = true;
                    nr::pe::log("[nr] RenoDX stage (D3D12): the network recorded nothing; the frame as it came");
                }
                return false;
            }
            if (!fast_lin) {
                if ((NW & 31u) == 0) {
                    ensure(cmd, lin_neural, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    ensure(cmd, neural.res, D3D12_RESOURCE_STATE_COPY_DEST);
                    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                    src.pResource = lin_neural;
                    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    src.PlacedFootprint.Footprint.Width = NW;
                    src.PlacedFootprint.Footprint.Height = NH;
                    src.PlacedFootprint.Footprint.Depth = 1;
                    src.PlacedFootprint.Footprint.RowPitch = NW * 8;
                    dst.pResource = neural.res;
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst.SubresourceIndex = 0;
                    D3D12_BOX box{0, 0, 0, NW, NH, 1};
                    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                } else {
                    ensure(cmd, lin_neural, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    struct { uint32_t w, h, what, pad; float region[4]; } c{NW, NH, 0, 0, {1.f, 1.f, 0.f, 0.f}};
                    View srv[8] = {};
                    srv[0] = bv(lin_neural, uint64_t(lin_pitch(NW, 8)) * NH * 8, true, false);
                    View uav[14] = {};
                    uav[0] = tv(neural, true);
                    dispatch(cmd, kBufToTex, srv, uav, &c, 8, (NW + 7) / 8, (NH + 7) / 8);
                    if (!record_ok_) return false;
                }
            }
            return true;
        };
        const bool reset = reset_all || (k > 0 && !s.chained_history);
        // (Interleaved reads f.motion in place when safe or falls back to the copied subrect; a region outside leaves
        // none - the plain pass then, as on Vulkan)
        const bool il_motion_ok = il_motion_direct || motion_copied;
        if (il_on && il_motion_ok ? !il_run(cmd, f, s, reset, fast_input, il_motion_direct, motion_rx, motion_ry, motion_rw, motion_rh, pass_net)
                                  : !pass_net(fast_input, k, reset, false, 0, false)) {
            if (!record_ok_) return 0;
            net_ok = false;
            break;
        }
        if (!record_ok_) return 0;
        ++ran;   // the network ran this pass
        if (fast) {
            const bool fb = fast_feedback && motion_copied;
            struct { uint32_t size[2], low[2]; float k; uint32_t mode; float halo, chroma; uint32_t satkeep;
                     float fine, base, rho, lam, dcol, cfloor, ring, ccap, nohalo; uint32_t restored; float ocap, darkcol; }
                ap{{NW, NH}, {(NW + 3) / 4, (NH + 3) / 4}, fast_k, fb ? (fm == 2u ? 8u : 6u) : 9u,
                   fb ? nfast - 1.f : (s.fast_halo_limit[fast_slot] ? 0.f : 1.f), fb ? 0.6f : 0.3f, bits(0.5f),
                   fb ? 1.f : 2.f, 1.f, 0.3f, 0.f, 0.5f, fb ? 0.f : 0.6f, 2.f, fb ? 0.f : 2.5f,
                   fb ? (s.fast_halo_limit[fast_slot] ? 1.f : 0.5f) : 0.f, 0u, fb ? -0.5f : 0.f, fb ? 0.45f : 0.f};
            if (fb && fm == 1u) ap.k = nfast - 1.f;
            const float fine = s.fast_fine[fast_slot];
            if (fine != 1.f) {
                if (!fb) ap.fine *= fine;
                else if (fm == 1u) ap.k *= fine;
                else { ap.halo *= fine; ap.fine = fine; }
            }
            ap.restored = fb ? 1u : 0u;
            const bool edit = fm == 3u;   // Edit strength: the apply alone
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
        if (!record_ok_) return 0;
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
            srv[4] = tv(block_mean, false);
            View uav[14] = {};
            uav[0] = bv(prep_state, 16, true, true);
            uav[1] = tv(nin, true);
            uav[2] = tv(kept, true);
            uav[3] = tv(neural, true);
            dispatch(cmd, kPrep, srv, uav, &back, sizeof back / 4, nx, ny);
            if (!record_ok_) return 0;
        }
        // 3c. the look: Adjust NR's look, Detail stability, edge-aware upsampling.
        bool shaped = false, transport = false;
        run_look(cmd, k, ref, ls, motion_copied, f, reset, shaped, transport, up_fused);
        if (!record_ok_) return 0;
        const bool decode_up = up_fused && transport;   // the decode / last resolve makes P_up and N'_up (legacy_decode_up)
        if (transport && up_fused_logged != int(decode_up)) {   // said on a change: which way the transport runs
            up_fused_logged = int(decode_up);
            nr::pe::log("[nr] RenoDX stage (D3D12): the transport %s", decode_up ? "folded into the decode (no P_up / N'_up images)"
                                                                               : "apart (look_upsample into P_up / N'_up)");
        }
        Tex* rp = transport ? &up_proxy : &proxy;
        Tex* rn = transport ? &up_neural : (shaped ? &look_out : &neural);
        const uint32_t pw = transport ? W : NW, ph = transport ? H : NH;
        // 3d. resolve onto the running image (or directly into f.output on the final pass when direct_out is on).
        Tex out_target{f.output, W, H, f.output->GetDesc().Format};
        if (v6 && fuse && k + 1 == passes) {
            // The last pass's resolve and the commit in one dispatch when the commit takes no pedestal; decode_up: P_up
            // and N'_up made in it from P and N' at the network's extent (ProxySize = the network's extent).
            const uint32_t qw = decode_up ? NW : pw, qh = decode_up ? NH : ph;
            const V6Constants res = make_v6(s, f6, W, H, W, H, qw, qh, qw, qh, transfer, color,
                                            s.chroma_clamp_stops, ratio_resolve);
            View srv[8] = {};
            srv[0] = tv(ref, false);
            if (v6_fast_lin) {
                srv[1] = bv(lin_proxy, uint64_t(lin_pitch(NW, 8)) * NH * 8, true, false);
                srv[2] = bv(lin_neural, uint64_t(lin_pitch(NW, 8)) * NH * 8, true, false);
            } else {
                srv[1] = tv(decode_up ? proxy : *rp, false);
                srv[2] = tv(decode_up ? look_out : *rn, false);
            }
            srv[3] = units.absolute ? tv(w0, false) : tv(scale, false);
            View uav[14] = {};
            uav[0] = tv(direct_out ? out_target : decoded, true);
            const V6PrepConstants pres = prep_constants(res);
            const P rc = v6_fast_lin ? kResolveCommit6Lin : decode_up ? kResolveCommit6Up
                       : prep_resolve ? kResolveCommit6Prep : kResolveCommit6;
            const bool tail_bar = !direct_out || f.output_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            dispatch(cmd, rc, srv, uav, prep_resolve ? static_cast<const void*>(&pres) : static_cast<const void*>(&res),
                     prep_resolve ? sizeof pres / 4 : sizeof res / 4, gx, gy, tail_bar);
            fused = true;
        } else if (v6) {
            const V6Constants res = make_v6(s, f6, W, H, W, H, pw, ph, pw, ph, transfer, color,
                                            s.chroma_clamp_stops, ratio_resolve);
            View srv[8] = {};
            srv[0] = tv(ref, false);
            if (v6_fast_lin) {
                srv[1] = bv(lin_proxy, uint64_t(lin_pitch(NW, 8)) * NH * 8, true, false);
                srv[2] = bv(lin_neural, uint64_t(lin_pitch(NW, 8)) * NH * 8, true, false);
            } else {
                srv[1] = tv(*rp, false);
                srv[2] = tv(*rn, false);
            }
            srv[3] = units.absolute ? tv(w0, false) : tv(scale, false);
            View uav[14] = {};
            uav[0] = tv(write, true);
            const V6PrepConstants pres = prep_constants(res);
            dispatch(cmd, v6_fast_lin ? kResolve6Lin : (prep_resolve ? kResolve6Prep : kResolve6),
                     srv, uav, prep_resolve ? static_cast<const void*>(&pres) : static_cast<const void*>(&res),
                     prep_resolve ? sizeof pres / 4 : sizeof res / 4, gx, gy);
        } else if (sdr_fast_lin) {
            Tex& dst_tex = (direct_out && k + 1 == passes) ? out_target : write;
            const bool inplace_uav = ref.res == dst_tex.res;
            const bool proxy_src = (k == 0 && direct_in && in_srgb) || (inplace_uav && !decoded_load);
            const uint32_t src_mode = proxy_src ? 2u : (inplace_uav ? 1u : 0u);
            struct { uint32_t w, h, what, pad; uint32_t region[4]; } c{W, H, bits(color), src_mode, {pw, ph, 0, 0}};
            View srv[8] = {};
            if (src_mode == 0u) srv[0] = tv(ref, false);
            srv[1] = bv(lin_neural, uint64_t(lin_pitch(NW, 8)) * NH * 8, true, false);
            if (src_mode == 2u) srv[2] = bv(lin_proxy, uint64_t(lin_pitch(NW, 8)) * NH * 8, true, false);
            View uav[14] = {};
            uav[0] = tv(dst_tex, true);
            const bool tail_bar = !(direct_out && k + 1 == passes) || f.output_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            dispatch(cmd, kSdrDecodeLin, srv, uav, &c, 8, (W + 7) / 8, (H + 7) / 8, tail_bar);
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
            uav[0] = tv((direct_out && k + 1 == passes) ? out_target : write, true);
            const bool fuse_decode_reduce = sdr_restore && !shaped && !transport;
            const bool tail_bar = sdr_restore || !(direct_out && k + 1 == passes) || f.output_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            if (fuse_decode_reduce) {
                uav[1] = tv(block_mean, true);
                dispatch(cmd, kDecodeReduceL, srv, uav, &c, sizeof c / 4, (W + 31) / 32, (H + 31) / 32, tail_bar);
            } else if (decode_up) {   // legacy_decode_up: N'_up made from P and N' at the network's extent
                LegacyConstants u = c;
                u.proxy_size[0] = u.neural_size[0] = NW; u.proxy_size[1] = u.neural_size[1] = NH;
                srv[1] = tv(proxy, false); srv[2] = tv(look_out, false);
                dispatch(cmd, kDecodeUpL, srv, uav, &u, sizeof u / 4, gx, gy, tail_bar);
            } else {
                dispatch(cmd, kDecodeL, srv, uav, &c, sizeof c / 4, gx, gy, tail_bar);
            }
            if (sdr_restore) {   // RunBlackLevelRestore: the reduce (when not fused), then the apply
                c.proxy_size[0] = c.neural_size[0] = NW; c.proxy_size[1] = c.neural_size[1] = NH;
                View rsrv[8] = {};
                rsrv[0] = tv(ref, false); rsrv[1] = tv(proxy, false); rsrv[2] = tv(neural, false);
                rsrv[3] = tv(ref, false);
                View ruav[14] = {};
                ruav[0] = tv(write, true);
                ruav[1] = tv(block_mean, true);
                if (!fuse_decode_reduce)
                    dispatch(cmd, shaped ? kReduceLU : kReduceL, rsrv, ruav, &c, sizeof c / 4, (W + 31) / 32, (H + 31) / 32);
                if (!record_ok_) return 0;
                LegacyConstants a = c;
                a.padding[0] = s.sdr_black_restore == 1u ? 2.f : 0.061f;
                a.padding[1] = restore_mode;
                ruav[1] = tv(block_mean, true);   // the apply reads the means back (a UAV load, as its declare)
                dispatch(cmd, kApplyL, rsrv, ruav, &a, sizeof a / 4, gx, gy);
            }
        }
        if (!record_ok_) return 0;
        executed = int32_t(k);
        final_transfer = transfer;
        final_color = color;
        final_shaped = shaped;
    }
    if (!record_ok_) return 0;

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
                srv[0] = tv(w0, false); srv[1] = tv(proxy, false); srv[2] = tv(neural, false);
                srv[3] = units.absolute ? tv(w0, false) : tv(scale, false);
                View uav[14] = {};
                uav[0] = tv(block_mean, true);
                dispatch(cmd, prep_resolve ? kReduce6UPrep : kReduce6U, srv, uav,
                         prep_resolve ? static_cast<const void*>(&pred) : static_cast<const void*>(&red),
                         prep_resolve ? sizeof pred / 4 : sizeof red / 4, (W + 31) / 32, (H + 31) / 32);
            } else {
                View srv[8] = {};
                srv[0] = tv(work_final, false); srv[1] = tv(w0, false);
                srv[2] = units.absolute ? tv(w0, false) : tv(scale, false);
                srv[3] = tv(w0, false);
                View uav[14] = {};
                uav[0] = tv(block_mean, true);
                dispatch(cmd, kReduce6, srv, uav, &red, sizeof red / 4, (W + 31) / 32, (H + 31) / 32);
            }
            if (!record_ok_) return 0;
        }
        const V6Constants com = make_v6(s, f6, W, H, W, H, W, H, W, H, final_transfer, final_color, 0.f);
        const bool v2 = !units.absolute && feed != kFeedV1;
        View srv[8] = {};
        srv[0] = tv(work_final, false); srv[1] = tv(w0, false);
        srv[2] = v2 ? tv(scale, false) : (units.absolute ? tv(w0, false) : tv(scale, false));
        srv[3] = tv(w0, false);
        Tex out_target{f.output, W, H, f.output->GetDesc().Format};
        View uav[14] = {};
        uav[0] = tv(direct_out ? out_target : decoded, true);
        uav[1] = tv(block_mean, true);
        const bool tail_bar = !direct_out || f.output_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        dispatch(cmd, v2 ? kCommitExposure : kCommit6, srv, uav, &com, sizeof com / 4, gx, gy, tail_bar);
        if (!record_ok_) return 0;
    }

    if (!record_ok_) return 0;
    if (!net_ok) {   // the network recorded nothing: the frame as it came
        if (direct_in && f.colour != f.output) ensure(cmd, f.colour, f.colour_state);
        copy_frame(cmd, f);
        return 0;
    }
    if (direct_in && f.colour != f.output) ensure(cmd, f.colour, f.colour_state);
    if (direct_out) {
        ensure(cmd, f.output, f.output_state);
        return ran;
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
        uav[4] = tv(result, true);
        dispatch(cmd, kDebug, srv, uav, dv, 5, gx, gy);
        if (!record_ok_) return 0;
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
        ensure(cmd, f.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(cmd, kCopyOut, srv, uav, &c, 8, (W + 7) / 8, (H + 7) / 8,
                 f.output_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!record_ok_) return 0;
        ensure(cmd, f.output, f.output_state);
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
                   &m.st_g0, &m.st_g1, &m.st_f0[0], &m.st_f0[1], &m.st_f1[0], &m.st_f1[1],
                   &m.work0, &m.work_a, &m.work_b, &m.scale, &m.commit, &m.work_p, &m.exposure_1x1})
        m.drop(*t);
    m.drop_buf(m.trace);
    m.drop_buf(m.prep_state);
    m.drop_buf(m.zero_upload);
    m.drop_buf(m.lin_proxy);
    m.drop_buf(m.lin_neural);
    m.drop_buf(m.lin_mv);
    m.drop_buf(m.lin_depth);
    for (auto& pl : m.looks) { m.drop_buf(pl.history[0]); m.drop_buf(pl.history[1]); }
    m.drop_il();
    m.outer_pool.reset();
    for (auto& bank : m.il_kernels) for (auto& k : bank) release(k.pso);
    release(m.ilpp_kernel.pso);
    for (auto& k : m.kernels) release(k.pso);
    release(m.rs_outer);
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
bool Stage::cmd_touched() const { return impl_ ? impl_->cmd_touched_ : false; }
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

#ifdef NR_TOOLS_BUILD
void Stage::set_test_fail_dispatch_at(uint32_t dispatch_index) {
    g_test_fail_dispatch_at.store(dispatch_index, std::memory_order_relaxed);
}
#endif

void prewarm_shaders(const std::wstring& /*shaders*/) {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([] { (void)d3d12_outer::shared_archive(nullptr); }).detach();
}

}  // namespace nr::pe::d3d12_passthrough::renodx12
