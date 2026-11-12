#!/usr/bin/env bash
# The D3D12 route's own programs as HLSL, from the package's SPIR-V (one source for both routes): the plugin's native D3D12
# backend compiles them in the game with d3dcompiler_47 (cs_5_0). RenoDX's own programs are not here: the D3D12 route
# compiles RenoDX's HLSL sources (dlssnr-amd/shaders/renodx/*.hlsl) as they are.
#   renodx/  NR's programs of the RenoDX style: Preprocess (renodx_prep, renodx_prep_encode), the debug views
#            (renodx_debug) and Passes (fast) (renodx_st_*, Interleaved's renodx_il, preset 1's renodx_il0 / renodx_il_pp); push constants ->
#            cbuffer b0 (their layouts have no arrays)
#   wilsjo2/ the wilsjo2 style's programs (wilsjo2_*, but its Passes (fast) ones: the renodx_st_* above are the same SPIR-V);
#            their 256-byte constant block holds float[16] at stride 4, which no cbuffer can lay out, so it is moved
#            from push constants to a read-only buffer at binding 6 first (w2_spv_d3d12.py) - SPIRV-Cross then writes a
#            ByteAddressBuffer at t6 read at the same offsets
# Registers: texture binding N -> tN, image / storage buffer binding N -> uN (SPIRV-Cross's default). isnan / isinf
# keep the SPIR-V's meaning (IEEE bit tests: FXC may fold them otherwise).
#   build_d3d12_hlsl.sh <dlssnr-amd/shaders> <out dir>      NR_SPIRV_CROSS overrides the converter
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
src=$(realpath -- "$1"); out=$(realpath -m -- "$2")
cross=${NR_SPIRV_CROSS:-toolchain/SPIRV-Cross/build/spirv-cross}
[[ -x "$cross" ]] || { echo "no SPIRV-Cross ($cross): fetch_deps.sh --windows" >&2; exit 1; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/d3d12hlsl.XXXXXX")
trap 'rm -rf -- "$tmp"' EXIT
mkdir -p -- "$out/renodx" "$out/wilsjo2"
prefix='// Made by windows/build/build_d3d12_hlsl.sh from the SPIR-V of the same name (SPIRV-Cross). Do not edit.
bool nr_isnan(float x) { return (asuint(x) & 0x7fffffffu) > 0x7f800000u; }
bool2 nr_isnan(float2 x) { return (asuint(x) & 0x7fffffffu) > 0x7f800000u; }
bool3 nr_isnan(float3 x) { return (asuint(x) & 0x7fffffffu) > 0x7f800000u; }
bool4 nr_isnan(float4 x) { return (asuint(x) & 0x7fffffffu) > 0x7f800000u; }
bool nr_isinf(float x) { return (asuint(x) & 0x7fffffffu) == 0x7f800000u; }
bool2 nr_isinf(float2 x) { return (asuint(x) & 0x7fffffffu) == 0x7f800000u; }
bool3 nr_isinf(float3 x) { return (asuint(x) & 0x7fffffffu) == 0x7f800000u; }
bool4 nr_isinf(float4 x) { return (asuint(x) & 0x7fffffffu) == 0x7f800000u; }
#define isnan nr_isnan
#define isinf nr_isinf
'
convert() {   # convert <spv> <out hlsl>
    "$cross" --hlsl --shader-model 50 "$1" --output "$tmp/raw.hlsl"
    { printf '%s' "$prefix"; cat "$tmp/raw.hlsl"; } > "$2"
}
n=0
# (renodx_il / renodx_il0 / renodx_il_pp: Passes (fast) Interleaved's two presets, on both native stages - the wilsjo2 one reads these too, as it
# reads renodx_st_*; il's step stays its specialization constant, SPIRV_CROSS_CONSTANT_ID_0, set when it is compiled)
for p in renodx_prep renodx_prep_encode renodx_debug renodx_st_fb_in renodx_st_prep renodx_st_box renodx_st_apply \
         renodx_il renodx_il0 renodx_il_pp; do
    convert "$src/renodx/$p.spv" "$out/renodx/$p.hlsl"; n=$((n + 1))
done
# (wilsjo2_il*: the same SPIR-V as renodx_il*, above)
for f in "$src"/wilsjo2/wilsjo2_*.spv; do
    b=$(basename "$f" .spv)
    case "$b" in wilsjo2_st_*|wilsjo2_il*) continue ;; esac
    python3 windows/build/w2_spv_d3d12.py "$f" "$tmp/$b.spv" > /dev/null
    convert "$tmp/$b.spv" "$out/wilsjo2/$b.hlsl"; n=$((n + 1))
done
# HUD FIX (windows/shaders/hudfix, hudfix_<step>.spv in the wilsjo2 folder): as wilsjo2_hudfix_<step>.hlsl, the name the
# native stage loads (nr_asi_wilsjo2_d3d12.hpp kPrograms)
# Remove the retired program from reused output folders and never convert stale input copies.
rm -f -- "$out/wilsjo2/wilsjo2_hudfix_estimate.hlsl"
for step in clear count compose; do
    f="$src/wilsjo2/hudfix_$step.spv"
    b=$(basename "$f" .spv)
    python3 windows/build/w2_spv_d3d12.py "$f" "$tmp/$b.spv" > /dev/null
    convert "$tmp/$b.spv" "$out/wilsjo2/wilsjo2_$b.hlsl"; n=$((n + 1))
done
echo "D3D12 programs: $n in $out"
