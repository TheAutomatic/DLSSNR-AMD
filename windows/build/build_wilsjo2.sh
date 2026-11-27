#!/usr/bin/env bash
# The wilsjo2 style's shaders (windows/shaders/wilsjo2, ours, written from our specification of the style) ->
# SPIR-V with glslang, like every other pass of ours. Installed as dlssnr-amd\shaders\wilsjo2\wilsjo2_<mode>.spv.
#   build_wilsjo2.sh <out dir>        NR_GLSLANG overrides the compiler (default toolchain/glslang)
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source windows/build/parallel.sh
out=$(realpath -m -- "${1:?usage: build_wilsjo2.sh <out dir>}")
glslang=${NR_GLSLANG:-toolchain/glslang/bin/glslang}
src=windows/shaders/wilsjo2
mkdir -p -- "$out"
rm -f -- "$out"/wilsjo2_*.spv "$out"/hudfix_*.spv   # only this tree's programs (a reused folder may hold another build's)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/wilsjo2.XXXXXX")
trap 'rm -rf -- "$tmp_dir"' EXIT
run_glsl() {
    local out_f=$1 log_f=$2
    shift 2
    if ! "$glslang" -V --target-env vulkan1.3 "$@" -o "$out_f" > "$log_f" 2>&1; then
        cat "$log_f" >&2
        return 1
    fi
}
for k in encode resolve meter autoexposure finished clamp downsample os spatial inspect light; do
    par run_glsl "$out/wilsjo2_$k.spv" "$tmp_dir/$k.log" -I"$src" "$src/wilsjo2_$k.comp"
done
# the peripheral compression's guides (mode 101) as a shader of their own, as the original has them
par run_glsl "$out/wilsjo2_spatial_guides.spv" "$tmp_dir/spatial_guides.log" -I"$src" -DWILSJO2_GUIDES "$src/wilsjo2_spatial.comp"
# [Preprocess] on a wilsjo2 layer: the shared math (windows/shaders/renodx-prep/prep_math.glsl, RenoDX's too); the meter
# as a program of its own (one 16 x 16 group)
par run_glsl "$out/wilsjo2_prep.spv" "$tmp_dir/prep.log" -I"$src" -Iwindows/shaders/renodx-prep "$src/wilsjo2_prep.comp"
par run_glsl "$out/wilsjo2_prep_meter.spv" "$tmp_dir/prep_meter.log" -I"$src" -Iwindows/shaders/renodx-prep -DMETER "$src/wilsjo2_prep.comp"
# Enlargement Classic's scaler (FSR 1 EASU from windows/shaders/passes/include/fsr1, AMD, MIT)
par run_glsl "$out/wilsjo2_classic.spv" "$tmp_dir/classic.log" -I"$src" -Iwindows/shaders/passes/include "$src/wilsjo2_classic.comp"
# Passes (fast): the RenoDX-style stage's programs (windows/shaders/renodx-fast, the same sources as renodx_st_*.spv and
# renodx_il*.spv: Interleaved's too), and the network's input in 16-bit float for them
for k in st_fb_in st_prep st_box st_apply il il0 il_pp; do
    par run_glsl "$out/wilsjo2_$k.spv" "$tmp_dir/fast_$k.log" -Iwindows/shaders/renodx-fast "windows/shaders/renodx-fast/$k.comp"
done
# Interleaved with the Style grade in closed form ([Network] Style Grade Acceleration)
for k in il il0; do
    par run_glsl "$out/wilsjo2_${k}_ga.spv" "$tmp_dir/fast_${k}_ga.log" -DNR_STYLE_GRADE_CLOSED -Iwindows/shaders/renodx-fast "windows/shaders/renodx-fast/$k.comp"
done
par run_glsl "$out/wilsjo2_half_copy.spv" "$tmp_dir/half_copy.log" -I"$src" "$src/wilsjo2_half_copy.comp"
# the .asi's Vulkan copies a transfer cannot make: one file, two programs (the source sampled / read as storage)
par run_glsl "$out/wilsjo2_copy.spv" "$tmp_dir/copy.log" -I"$src" "$src/wilsjo2_copy.comp"
par run_glsl "$out/wilsjo2_copy_load.spv" "$tmp_dir/copy_load.log" -I"$src" -DCOPY_LOAD "$src/wilsjo2_copy.comp"
# HUD FIX (windows/shaders/hudfix, any processing's; it runs on this stage): one file, three programs
for k in clear count compose; do
    par run_glsl "$out/hudfix_$k.spv" "$tmp_dir/hudfix_$k.log" "-DHUD_${k^^}" windows/shaders/hudfix/hudfix.comp
done
par_wait
echo "$out: $(ls "$out"/wilsjo2_*.spv | wc -l) wilsjo2 shaders, $(ls "$out"/hudfix_*.spv | wc -l) HUD FIX shaders"
