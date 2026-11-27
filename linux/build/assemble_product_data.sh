#!/usr/bin/env bash
# Lay out dlssnr-amd/, the folder the installed DLLs load everything from:
#
#   dlssnr-amd/CC_SILVER_AARDWOLD.bin  CC_SILVER_AARDWOLD's weights (only with --silver-aardwold-model)
#   dlssnr-amd/CC_SMALL_FP8.bin       CC_SMALL_FP8's weights (only with --small-fp8-model)
#   dlssnr-amd/shaders/               the network (linux/build/arch/rdna4.sh)
#   dlssnr-amd/shaders/runtime/       the passes around it
#   dlssnr-amd/shaders/temporal/      the temporal variants and motion estimator
#   dlssnr-amd/shaders/renodx/        the RenoDX-style option's shaders (RenoDX's own, MIT)
#   dlssnr-amd/shaders/frame-reuse/   NRFrameReuse's shaders (ours)
#   dlssnr-amd/shaders/wilsjo2/       the wilsjo2 stage's (HUD FIX on the ReShade routes too; the .asi brings its own)
#   dlssnr-amd/CC_SMALL_FP8_OPTIMIZED/shaders/  CC_SMALL_FP8_OPTIMIZED's network (Quality) and temporal variants (its
#                                     weights are made at install, linux/build/delta_weights.py)
#   dlssnr-amd/CC_SMALL_FP8_OPTIMIZED-performance/shaders/  CC_SMALL_FP8_OPTIMIZED at Performance (the same weights'
#                                     "performance/" entries)
#   dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED/shaders/  CC_SILVER_AARDWOLD_OPTIMIZED's network and temporal variants (its
#                                     weights are made at install the same way; its levels with int4 layers come with
#                                     the package's int4/)
#   dlssnr-amd/lang/                  the menus' texts: English.txt from the code (linux/build/make_lang.py) and
#                                     README.txt, how to make another language
#
#   assemble_product_data.sh <parent dir> [--silver-aardwold-model CC_SILVER_AARDWOLD.bin [--small-fp8-model CC_SMALL_FP8.bin]]
set -euo pipefail
start=$PWD
cd -- "$(dirname -- "$0")/../.."
parent=${1:?usage: assemble_product_data.sh <parent dir> [--silver-aardwold-model CC_SILVER_AARDWOLD.bin [--small-fp8-model CC_SMALL_FP8.bin]]}
source linux/build/arch/rdna4.sh
dst="$parent/dlssnr-amd"
rm -rf -- "$dst"; mkdir -p -- "$dst/shaders/runtime" "$dst/shaders/temporal"

# Every network set and the outer shaders at once (linux/build/parallel.sh), then their files copied into place.
source linux/build/parallel.sh
par python3 linux/build/build_network.py "$NR_GPU_ARCH" --out "$NR_PRODUCT_SPV" > /dev/null
for n in CC_SMALL_FP8_OPTIMIZED CC_SMALL_FP8_OPTIMIZED-performance CC_SILVER_AARDWOLD_OPTIMIZED; do
    par python3 linux/build/build_network.py "$NR_GPU_ARCH" --network "$n" --out "$NR_PRODUCT_SPV-$n" > /dev/null
done
par bash linux/build/build_renodx.sh "$dst/shaders/renodx" > /dev/null
par bash linux/build/build_frame_reuse.sh "$dst/shaders/frame-reuse" > /dev/null
par bash linux/build/build_wilsjo2.sh "$dst/shaders/wilsjo2" > /dev/null
par_wait
cp -- "$NR_PRODUCT_SPV"/g_*.spv "$dst/shaders/"
for f in accumulation.txt swin-bias-storage.txt coherent-act.txt shader-constants.txt; do
    cp -- "$NR_PRODUCT_SPV/$f" "$dst/shaders/"
done
cp -- "$NR_PRODUCT_SPV"/runtime/* "$dst/shaders/runtime/"
cp -- "$NR_PRODUCT_SPV"/temporal/* "$dst/shaders/temporal/"
# wilsjo2_classic.spv carries AMD FidelityFX Super Resolution 1 (MIT): its notice goes with it
cp -- linux/shaders/passes/include/fsr1/LICENSE.txt "$dst/shaders/wilsjo2/FidelityFX-FSR1-LICENSE.txt"
# vit-tail.txt is the host's marker of CC_SMALL_FP8_OPTIMIZED's levels (nr_graph.cpp: the ViT padding tail, the rules at
# edge-padded ViT rasters): every level's folder needs it, as build_network.py writes it
xs="$dst/CC_SMALL_FP8_OPTIMIZED/shaders"; mkdir -p -- "$xs/temporal"
cp -- "$NR_PRODUCT_SPV-CC_SMALL_FP8_OPTIMIZED"/g_*.spv "$xs/"
for f in accumulation.txt swin-bias-storage.txt coherent-act.txt shader-constants.txt fixed-attention.txt vit-tail.txt; do
    cp -- "$NR_PRODUCT_SPV-CC_SMALL_FP8_OPTIMIZED/$f" "$xs/"
done
cp -- "$NR_PRODUCT_SPV-CC_SMALL_FP8_OPTIMIZED"/temporal/* "$xs/temporal/"
ps="$dst/CC_SMALL_FP8_OPTIMIZED-performance/shaders"; mkdir -p -- "$ps/temporal"
cp -- "$NR_PRODUCT_SPV-CC_SMALL_FP8_OPTIMIZED-performance"/g_*.spv "$ps/"
for f in accumulation.txt swin-bias-storage.txt coherent-act.txt shader-constants.txt fixed-attention.txt vit-kv-pool.txt vit-tail.txt; do
    cp -- "$NR_PRODUCT_SPV-CC_SMALL_FP8_OPTIMIZED-performance/$f" "$ps/"
done
cp -- "$NR_PRODUCT_SPV-CC_SMALL_FP8_OPTIMIZED-performance"/temporal/* "$ps/temporal/"
os="$dst/CC_SILVER_AARDWOLD_OPTIMIZED/shaders"; mkdir -p -- "$os/temporal"
cp -- "$NR_PRODUCT_SPV-CC_SILVER_AARDWOLD_OPTIMIZED"/g_*.spv "$os/"
for f in accumulation.txt swin-bias-storage.txt coherent-act.txt shader-constants.txt fixed-attention.txt const-heads.txt \
        block-routes.txt; do
    cp -- "$NR_PRODUCT_SPV-CC_SILVER_AARDWOLD_OPTIMIZED/$f" "$os/"
done
cp -- "$NR_PRODUCT_SPV-CC_SILVER_AARDWOLD_OPTIMIZED"/temporal/* "$os/temporal/"
mkdir -p -- "$dst/lang"
python3 linux/build/make_lang.py "$dst/lang/English.txt" linux/src/pe/nr_pe_menu.cpp linux/src/ui/nr_control_ui.hpp \
    linux/src/ui/nr_control_pages.hpp linux/src/core/nr_renodx_keys.hpp linux/src/ui/nr_wilsjo2_shared_keys.hpp \
    linux/src/ui/nr_wilsjo2_keys.hpp linux/src/ui/nr_reshade_layer_keys.hpp linux/src/pe/nr_pe_events.hpp \
    linux/src/pe/nr_reshade_addon.cpp \
    --producers linux/src/core/nr_frame_reuse_policy.hpp linux/src/pe/nr_asi_main.cpp linux/src/pe/nr_asi_hudcap.cpp linux/src/pe/nr_pe_control.cpp \
    linux/src/pe/nr_dlssnr_model.cpp linux/src/pe/nr_pe_session.cpp linux/src/pe/nr_engine_panel.hpp \
    linux/src/pe/nr_reshade_addon.cpp linux/src/core/nr_control_shared.hpp > /dev/null
cp -- linux/package/lang/README.txt "$dst/lang/README.txt"
cp -- linux/shaders/renodx/LICENSE "$dst/shaders/renodx/RenoDX-LICENSE.txt"

# The weights are NVIDIA's and are never part of this repository: --silver-aardwold-model takes a CC_SILVER_AARDWOLD.bin
# and --small-fp8-model a CC_SMALL_FP8.bin made from your own nvngx_dlssnr.dll by
# linux/package/model-tools/extract_model.sh.
if [[ "${2:-}" == --silver-aardwold-model ]]; then
    model=$(cd -- "$start" && realpath -- "${3:?--silver-aardwold-model needs a CC_SILVER_AARDWOLD.bin}")
    [[ -f "$model" ]] || { echo "no model file: $model" >&2; exit 1; }
    cp -- "$model" "$dst/$NR_MODEL_NAME"
    if [[ "${4:-}" == --small-fp8-model ]]; then
        model_small_fp8=$(cd -- "$start" && realpath -- "${5:?--small-fp8-model needs a CC_SMALL_FP8.bin}")
        [[ -f "$model_small_fp8" ]] || { echo "no model file: $model_small_fp8" >&2; exit 1; }
        cp -- "$model_small_fp8" "$dst/CC_SMALL_FP8.bin"
    fi
fi
echo "$dst: $(find "$dst/shaders" -name '*.spv' | wc -l) shaders$([[ -f "$dst/$NR_MODEL_NAME" ]] && echo ", model included")"
