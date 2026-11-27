#!/usr/bin/env bash
# Frame reuse's shaders (windows/shaders/frame-reuse, ours) -> SPIR-V:
#   guides.comp   the game's motion / depth / exposure onto the stage's own grids
#   compose.comp  the vectors of two frames composed; a rebuilt frame's edit; the fill of uncovered pixels
#   context.comp  the outline context classes of a depth image (the two-sided rebuild's merge weights)
#   display.comp  the shown edit's temporal filter (display10.spv: on 10-bit frames)
#   speed.comp    how fast the picture moves (the fast-motion rule)
#   decide.comp   NR or a rebuilt frame, decided on the GPU from this frame's motion (conditional rendering's word)
#   history_copy.comp / frame_copy.comp / history_weight.comp   the GPU decision's carry, copy and history weight
#   (frame_copy10.spv: into a 10-bit frame)
#   build_frame_reuse.sh <out dir>        NR_GLSLANG overrides the compiler
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
out=$(realpath -m -- "${1:?usage: build_frame_reuse.sh <out dir>}")
glslang=${NR_GLSLANG:-toolchain/glslang/bin/glslang}
src=windows/shaders/frame-reuse
mkdir -p -- "$out"
log=$(mktemp "${TMPDIR:-/tmp}/frame-reuse.XXXXXX"); trap 'rm -f -- "$log"' EXIT
build() { "$glslang" -V "$@" > "$log" || { cat "$log" >&2; exit 1; }; }
build --target-env vulkan1.3 "$src/guides.comp" -o "$out/guides.spv"
build --target-env vulkan1.2 "$src/compose.comp" -o "$out/compose.spv"
build --target-env vulkan1.2 -DUNORM16_OUTPUT "$src/compose.comp" -o "$out/compose16u.spv"
build --target-env vulkan1.3 "$src/context.comp" -o "$out/context.spv"
build --target-env vulkan1.2 "$src/display.comp" -o "$out/display.spv"
build --target-env vulkan1.2 -DTEN_BIT "$src/display.comp" -o "$out/display10.spv"
build --target-env vulkan1.3 "$src/speed.comp" -o "$out/speed.spv"
for k in decide history_copy frame_copy history_weight; do build --target-env vulkan1.2 "$src/$k.comp" -o "$out/$k.spv"; done
build --target-env vulkan1.2 -DTEN_BIT "$src/frame_copy.comp" -o "$out/frame_copy10.spv"
build --target-env vulkan1.2 "$src/scrgb.comp" -o "$out/scrgb_encode.spv"
build --target-env vulkan1.2 -DDECODE "$src/scrgb.comp" -o "$out/scrgb_decode.spv"
for k in display frame_copy; do
    build --target-env vulkan1.2 -DFLOAT_OUTPUT=rgba16 "$src/$k.comp" -o "$out/${k}16u.spv"
    build --target-env vulkan1.2 -DFLOAT_OUTPUT=rgba16f "$src/$k.comp" -o "$out/${k}16f.spv"
    build --target-env vulkan1.2 -DFLOAT_OUTPUT=r11f_g11f_b10f "$src/$k.comp" -o "$out/${k}11f.spv"
done
echo "$out: $(ls "$out"/*.spv | wc -l) frame reuse shaders"
