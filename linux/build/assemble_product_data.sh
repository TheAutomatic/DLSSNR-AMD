#!/usr/bin/env bash
# Lay out dlssnr-amd/, the folder the installed DLLs load everything from:
#
#   dlssnr-amd/CC_SILVER_AARDWOLD.bin             the old model's weights (only with --model)
#   dlssnr-amd/CC_SMALL_FP8.bin       the new model's weights (only with --small-model)
#   dlssnr-amd/shaders/               the network (linux/build/arch/rdna4.sh)
#   dlssnr-amd/shaders/runtime/       the passes around it
#   dlssnr-amd/shaders/temporal/      the temporal variants and motion estimator
#   dlssnr-amd/shaders/renodx/        the RenoDX-style option's shaders (RenoDX's own, MIT)
#   dlssnr-amd/shaders/wilsjo2/       the wilsjo2 stage's (HUD FIX on the ReShade routes too; the .asi brings its own)
#   dlssnr-amd/experimental/shaders/  CC_SMALL_FP8_EXPERIMENTAL's network and temporal variants (its weights are made
#                                     at install, linux/build/experimental_weights.py)
#   dlssnr-amd/optimized/shaders/     CC_SILVER_AARDWOLD_OPTIMIZED's network and temporal variants (its weights are
#                                     made at install the same way; with its int4 layers: dlssnr-amd/int4/, the int4 option)
#   dlssnr-amd/lang/                  the menus' texts: English.txt from the code (linux/build/make_lang.py) and
#                                     README.txt, how to make another language
#
#   assemble_product_data.sh <parent dir> [--model CC_SILVER_AARDWOLD.bin [--small-model CC_SMALL_FP8.bin]]
set -euo pipefail
start=$PWD
cd -- "$(dirname -- "$0")/../.."
parent=${1:?usage: assemble_product_data.sh <parent dir> [--model CC_SILVER_AARDWOLD.bin [--small-model CC_SMALL_FP8.bin]]}
source linux/build/arch/rdna4.sh
dst="$parent/dlssnr-amd"
rm -rf -- "$dst"; mkdir -p -- "$dst/shaders/runtime" "$dst/shaders/temporal"

python3 linux/build/build_network.py "$NR_GPU_ARCH" --out "$NR_PRODUCT_SPV" > /dev/null
cp -- "$NR_PRODUCT_SPV"/g_*.spv "$dst/shaders/"
for f in accumulation.txt swin-bias-storage.txt coherent-act.txt shader-constants.txt; do
    cp -- "$NR_PRODUCT_SPV/$f" "$dst/shaders/"
done
cp -- "$NR_PRODUCT_SPV"/runtime/* "$dst/shaders/runtime/"
cp -- "$NR_PRODUCT_SPV"/temporal/* "$dst/shaders/temporal/"
bash linux/build/build_renodx.sh "$dst/shaders/renodx" > /dev/null   # the RenoDX-style option
bash linux/build/build_wilsjo2.sh "$dst/shaders/wilsjo2" > /dev/null   # HUD FIX on every route
# wilsjo2_classic.spv carries AMD FidelityFX Super Resolution 1 (MIT): its notice goes with it
cp -- linux/shaders/passes/include/fsr1/LICENSE.txt "$dst/shaders/wilsjo2/FidelityFX-FSR1-LICENSE.txt"
xs="$dst/experimental/shaders"; mkdir -p -- "$xs/temporal"
python3 linux/build/build_network.py "$NR_GPU_ARCH" --experimental --out "$NR_PRODUCT_SPV-experimental" > /dev/null
cp -- "$NR_PRODUCT_SPV-experimental"/g_*.spv "$xs/"
for f in accumulation.txt swin-bias-storage.txt coherent-act.txt shader-constants.txt fixed-attention.txt; do
    cp -- "$NR_PRODUCT_SPV-experimental/$f" "$xs/"
done
cp -- "$NR_PRODUCT_SPV-experimental"/temporal/* "$xs/temporal/"
os="$dst/optimized/shaders"; mkdir -p -- "$os/temporal"
python3 linux/build/build_network.py "$NR_GPU_ARCH" --optimized --out "$NR_PRODUCT_SPV-optimized" > /dev/null
cp -- "$NR_PRODUCT_SPV-optimized"/g_*.spv "$os/"
for f in accumulation.txt swin-bias-storage.txt coherent-act.txt shader-constants.txt fixed-attention.txt const-heads.txt \
        block-routes.txt; do
    cp -- "$NR_PRODUCT_SPV-optimized/$f" "$os/"
done
cp -- "$NR_PRODUCT_SPV-optimized"/temporal/* "$os/temporal/"
mkdir -p -- "$dst/lang"
python3 linux/build/make_lang.py "$dst/lang/English.txt" linux/src/pe/nr_pe_menu.cpp linux/src/ui/nr_control_ui.hpp \
    linux/src/ui/nr_control_pages.hpp linux/src/core/nr_renodx_keys.hpp linux/src/ui/nr_wilsjo2_shared_keys.hpp \
    linux/src/ui/nr_wilsjo2_keys.hpp linux/src/ui/nr_reshade_layer_keys.hpp linux/src/pe/nr_pe_events.hpp \
    linux/src/pe/nr_reshade_addon.cpp \
    --producers linux/src/pe/nr_asi_main.cpp linux/src/pe/nr_asi_hudcap.cpp linux/src/pe/nr_pe_control.cpp \
    linux/src/pe/nr_dlssnr_model.cpp linux/src/pe/nr_pe_session.cpp linux/src/pe/nr_engine_panel.hpp \
    linux/src/pe/nr_reshade_addon.cpp > /dev/null
cp -- linux/package/lang/README.txt "$dst/lang/README.txt"
cp -- linux/shaders/renodx/LICENSE "$dst/shaders/renodx/RenoDX-LICENSE.txt"

# The weights are NVIDIA's and are never part of this repository: --model takes a CC_SILVER_AARDWOLD.bin and
# --small-model a CC_SMALL_FP8.bin made from your own nvngx_dlssnr.dll by
# linux/package/model-tools/extract_model.sh.
if [[ "${2:-}" == --model ]]; then
    model=$(cd -- "$start" && realpath -- "${3:?--model needs a CC_SILVER_AARDWOLD.bin}")
    [[ -f "$model" ]] || { echo "no model file: $model" >&2; exit 1; }
    cp -- "$model" "$dst/$NR_MODEL_NAME"
    if [[ "${4:-}" == --small-model ]]; then
        small=$(cd -- "$start" && realpath -- "${5:?--small-model needs a CC_SMALL_FP8.bin}")
        [[ -f "$small" ]] || { echo "no model file: $small" >&2; exit 1; }
        cp -- "$small" "$dst/CC_SMALL_FP8.bin"
    fi
fi
echo "$dst: $(find "$dst/shaders" -name '*.spv' | wc -l) shaders$([[ -f "$dst/$NR_MODEL_NAME" ]] && echo ", model included")"
