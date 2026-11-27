#!/usr/bin/env bash
# Lay out dlssnr-amd/, the folder the installed DLLs load everything from:
#
#   dlssnr-amd/CC_SILVER_AARDWOLD.bin  CC_SILVER_AARDWOLD's weights (only with --silver-aardwold-model)
#   dlssnr-amd/CC_SMALL_FP8.bin       CC_SMALL_FP8's weights (only with --small-fp8-model)
#   dlssnr-amd/shaders/               the network (windows/build/arch/rdna4.sh)
#   dlssnr-amd/shaders/runtime/       the passes around it
#   dlssnr-amd/shaders/temporal/      the temporal variants and motion estimator
#   dlssnr-amd/shaders/renodx/        the RenoDX style's shaders (RenoDX's own, MIT), Passes (fast) and Preprocess
#   dlssnr-amd/shaders/frame-reuse/   NRFrameReuse's shaders (ours)
#   dlssnr-amd/shaders/wilsjo2/       the wilsjo2 style and HUD FIX
#   dlssnr-amd/lang/                  the menus' texts: English.txt from the code (windows/build/make_lang.py) and
#                                     README.txt, how to make another language
#
#   assemble_product_data.sh <parent dir> [--silver-aardwold-model CC_SILVER_AARDWOLD.bin [--small-fp8-model CC_SMALL_FP8.bin]]
set -euo pipefail
start=$PWD
cd -- "$(dirname -- "$0")/../.."
parent=${1:?usage: assemble_product_data.sh <parent dir> [--silver-aardwold-model CC_SILVER_AARDWOLD.bin [--small-fp8-model CC_SMALL_FP8.bin]]}
source windows/build/arch/rdna4.sh
dst="$parent/dlssnr-amd"
rm -rf -- "$dst"; mkdir -p -- "$dst/shaders/runtime" "$dst/shaders/temporal"

# Every network set and the outer shaders at once (windows/build/parallel.sh), then their files copied into place.
source windows/build/parallel.sh
par env NR_SHADER_JOBS="${NR_JOBS:-}" python3 windows/build/build_network.py "$NR_GPU_ARCH" --out "$NR_PRODUCT_SPV" > /dev/null
par bash windows/build/build_renodx.sh "$dst/shaders/renodx" > /dev/null
par bash windows/build/build_frame_reuse.sh "$dst/shaders/frame-reuse" > /dev/null
par bash windows/build/build_wilsjo2.sh "$dst/shaders/wilsjo2" > /dev/null
par_wait
cp -- "$NR_PRODUCT_SPV"/g_*.spv "$dst/shaders/"
for f in accumulation.txt swin-bias-storage.txt coherent-act.txt shader-constants.txt; do
    cp -- "$NR_PRODUCT_SPV/$f" "$dst/shaders/"
done
cp -- "$NR_PRODUCT_SPV"/runtime/* "$dst/shaders/runtime/"
cp -- "$NR_PRODUCT_SPV"/temporal/* "$dst/shaders/temporal/"
cp -- windows/shaders/renodx/LICENSE "$dst/shaders/renodx/RenoDX-LICENSE.txt"
# wilsjo2_classic.spv carries AMD FidelityFX Super Resolution 1 (MIT): its notice goes with it
cp -- windows/shaders/passes/include/fsr1/LICENSE.txt "$dst/shaders/wilsjo2/FidelityFX-FSR1-LICENSE.txt"
mkdir -p -- "$dst/lang"
python3 windows/build/make_lang.py "$dst/lang/English.txt" windows/src/pe/nr_pe_menu.cpp windows/src/ui/nr_control_ui.hpp \
    windows/src/ui/nr_control_pages.hpp windows/src/core/nr_renodx_keys.hpp windows/src/ui/nr_wilsjo2_shared_keys.hpp \
    windows/src/ui/nr_wilsjo2_keys.hpp windows/src/ui/nr_reshade_layer_keys.hpp windows/src/pe/nr_pe_events.hpp \
    windows/src/pe/nr_reshade_addon.cpp windows/src/pe/nr_d3d12_passthrough_panel.cpp \
    --producers windows/src/core/nr_frame_reuse_policy.hpp windows/src/pe/nr_asi_main.cpp windows/src/pe/nr_asi_hudcap.cpp windows/src/pe/nr_pe_control.cpp \
    windows/src/pe/nr_dlssnr_model.cpp windows/src/pe/nr_pe_session.cpp windows/src/pe/nr_engine_panel.hpp \
    windows/src/pe/nr_reshade_addon.cpp windows/src/pe/nr_pe_bridge.cpp windows/src/pe/nr_pe_d3d12split.cpp \
    windows/src/core/nr_control_shared.hpp > /dev/null
cp -- windows/package/lang/README.txt "$dst/lang/README.txt"

# The weights are NVIDIA's and are never part of this repository: --silver-aardwold-model takes a CC_SILVER_AARDWOLD.bin
# made from your own nvngx_dlssnr.dll by linux/package/model-tools/extract_model.sh, --small-fp8-model a
# CC_SMALL_FP8.bin made the same way from the 310.9.16 DLL.
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
