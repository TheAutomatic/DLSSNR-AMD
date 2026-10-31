#!/usr/bin/env bash
# The wilsjo2 programs for the native D3D12 route (M5 v2): from the Vulkan stage's SPIR-V (build_wilsjo2.sh's
# wilsjo2_<name>.spv, as 0.0.8.1 ships them) to the DXBC the .asi gives the D3D12 driver (dlssnr-amd-asd\wilsjo2-d3d12):
#   1. w2_spv_d3d12.py: the constant block from push constants to a read-only buffer (set 0, binding 6), nothing else
#   2. spirv-val
#   3. SPIRV-Cross: HLSL, shader model 5.0 (t0..t3 + samplers s0..s3, u4 / u5, the constants a ByteAddressBuffer at t6)
#   4. isnan / isinf as bit tests (the SPIR-V's own meaning; FXC may otherwise fold them)
#   5. w2_hlsl_dxbc.exe (d3dcompiler_47, cs_5_0, optimisation 3, IEEE strictness)
# Run under WSL on Windows (step 5 is a Windows program).
#   build_wilsjo2_d3d12.sh <spv dir> <out dir> <spirv-cross> <w2_hlsl_dxbc.exe>
set -euo pipefail
src=$(realpath "$1"); out=$2; cross=$3; dxbc=$(realpath "$4")
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"/spv "$out"/hlsl "$out"/dxbc
out=$(realpath "$out")
prefix='// wilsjo2 (M5 v2): isnan / isinf as the SPIR-V means them (IEEE bit tests)
bool w2_isnan(float x) { return (asuint(x) & 0x7fffffffu) > 0x7f800000u; }
bool2 w2_isnan(float2 x) { return (asuint(x) & 0x7fffffffu) > 0x7f800000u; }
bool3 w2_isnan(float3 x) { return (asuint(x) & 0x7fffffffu) > 0x7f800000u; }
bool4 w2_isnan(float4 x) { return (asuint(x) & 0x7fffffffu) > 0x7f800000u; }
bool w2_isinf(float x) { return (asuint(x) & 0x7fffffffu) == 0x7f800000u; }
bool2 w2_isinf(float2 x) { return (asuint(x) & 0x7fffffffu) == 0x7f800000u; }
bool3 w2_isinf(float3 x) { return (asuint(x) & 0x7fffffffu) == 0x7f800000u; }
bool4 w2_isinf(float4 x) { return (asuint(x) & 0x7fffffffu) == 0x7f800000u; }
#define isnan w2_isnan
#define isinf w2_isinf
'
n=0
for f in "$src"/wilsjo2_*.spv; do
    b=$(basename "$f" .spv)
    case "$b" in *fast*|*half_copy*|wilsjo2_st_*) continue ;; esac   # Passes (fast) programs: not on this route
    python3 "$here/w2_spv_d3d12.py" "$f" "$out/spv/$b.spv"
    spirv-val "$out/spv/$b.spv"
    "$cross" --hlsl --shader-model 50 "$out/spv/$b.spv" --output "$out/hlsl/$b.raw.hlsl"
    { printf '%s' "$prefix"; cat "$out/hlsl/$b.raw.hlsl"; } > "$out/hlsl/$b.hlsl"
    rm -f "$out/hlsl/$b.raw.hlsl"
    (cd "$out" && "$dxbc" "hlsl/$b.hlsl" "dxbc/$b.dxbc")
    n=$((n + 1))
done
echo "wilsjo2 D3D12 programs: $n in $out/dxbc"
