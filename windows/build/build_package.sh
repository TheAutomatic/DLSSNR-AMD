#!/usr/bin/env bash
# Build the Windows package (64-bit games on the AMD Windows driver):
#   dlssnr-amd/     the network: Windows shaders (windows/build/assemble_product_data.sh), and in aco/ the
#                   same network as ACO's machine code (ACO Mode, always tried first): RX 9000's (RDNA4), int4 mixed's in
#                   int4/, RX 7000's and the Radeon 700M / 800M / 8000S graphics' (RDNA3) in rdna3/; int4 mixed's data
#                   in int4/, CC_SMALL_FP8_EXPERIMENTAL's SPIR-V in experimental/
#   model-tools/    dlssnr_extract_model.exe: install.ps1 makes the model from the user's nvngx_dlssnr.dll;
#                   dlssnr-int4-weights.exe: and int4 mixed's weights from the model
#   experimental/   CC_SMALL_FP8_EXPERIMENTAL's weights as a difference from CC_SMALL_FP8's (install.ps1 applies it)
#   optiscaler/     OptiScaler (the official nightly, unmodified) and our NR plugin for it
#   reshade/        ReShade + our add-on: dxgi.dll in DX10/11/12 games, a Vulkan layer otherwise
#   dxvk/           D3D9 -> Vulkan, for the DX9 route only
#   vulkan/         the Khronos loader the Vulkan-layer routes put beside the game
#   install.bat, install.ps1, README.txt
#
#   [NR_MODEL=dlssnr.bin [NR_MODEL_SMALL=dlssnr-small.bin]] [NR_INT4=0] [NR_LANG_EXTRA=<dir>] bash windows/build/build_package.sh [build dir]
#
# The package carries no model: install.ps1 extracts it from the user's nvngx_dlssnr.dll on the first
# install. NR_MODEL puts a dlssnr.bin you made yourself into the package (for your own use only), and with it
# NR_MODEL_SMALL a dlssnr-small.bin (the new model, from the 310.9.16 DLL).
#
# NR_INT4 (default 1): the programs built with int4 mixed and its data in the package; install.ps1 installs all four
# networks (CC_SMALL_FP8, CC_SMALL_FP8_EXPERIMENTAL, CC_SILVER_AARDWOLD, CC_SILVER_AARDWOLD_INT4_MIXED; the
# experimental one and int4 mixed run with ACO Mode only) and asks only which one a game starts with. NR_INT4=0: no int4
# mixed.
#
# D3D10/11/12 games keep the system's own D3D: the network runs on a Vulkan device of its own and the
# frame crosses over through shared textures and a shared fence (windows/src/pe/nr_pe_bridge.hpp).
# Only DX9 games are translated (DXVK), because D3D9 shares neither textures nor fences.
set -euo pipefail
model=""
if [[ -n "${NR_MODEL:-}" ]]; then
    [[ -f "$NR_MODEL" ]] || { echo "NR_MODEL: no such file: $NR_MODEL" >&2; exit 1; }
    model=$(realpath -- "$NR_MODEL")
fi
small=""
if [[ -n "$model" && -n "${NR_MODEL_SMALL:-}" ]]; then
    [[ -f "$NR_MODEL_SMALL" ]] || { echo "NR_MODEL_SMALL: no such file: $NR_MODEL_SMALL" >&2; exit 1; }
    small=$(realpath -- "$NR_MODEL_SMALL")
fi
cd -- "$(dirname -- "$0")/../.."
bash windows/build/check_paths.sh   # UTF-8 paths only (a game folder with any letters)
bash windows/build/check_imgui.sh   # ReShade's ImGui is MSVC's: no ImVec2-returning call from the add-on's code
out=$(realpath -m -- "${1:-artifacts/windows/package-build}")
case "$out" in "$(pwd)"/*) ;; *) echo 'build directory must be in the project' >&2; exit 2;; esac
mkdir -p -- "$out"

cxx=x86_64-w64-mingw32-g++
cc=x86_64-w64-mingw32-gcc
command -v "$cxx" >/dev/null || { echo "no mingw cross compiler ($cxx)" >&2; exit 1; }
minhook=artifacts/ref/DLSS5-Feeder/external/minhook
reshade_inc=artifacts/ref/DLSS5-Feeder/external/reshade/include
proton=${NR_PROTON:-toolchain/GE-Proton11-7-x86_64/files}
opti_7z=${NR_OPTI_ARCHIVE:-artifacts/ref/downloads/OptiScaler_v10.0.0-pre1_20261030.7z}
opti_license=${NR_OPTI_LICENSE:-artifacts/ref/optiscaler-nightly-20261030/LICENSE}
for dir in "$minhook" "$reshade_inc" artifacts/ref/reshade-shaders artifacts/ref/vort_Shaders \
           "$proton/lib/wine/dxvk/x86_64-windows"; do
    [[ -d "$dir" ]] || { echo "missing: $dir" >&2; exit 1; }
done
for f in "$opti_7z" "$opti_license" artifacts/ref/reshade-6.8.0/ReShade64.dll; do
    [[ -f "$f" ]] || { echo "missing: $f" >&2; exit 1; }
done
source windows/build/arch/rdna4.sh

# ---- the ReShade add-on -----------------------------------------------------------------------
stamp=$(bash windows/build/version.sh)
python3 windows/build/generate_device_chain.py toolchain/Vulkan-Headers "$out/nr_device_chain_sizes.hpp"
common=(-std=c++17 -O2 -DNDEBUG -DNR_BUILD_STAMP="\"$stamp\"" -Itoolchain/Vulkan-Headers/include -Iwindows/src
        -Iwindows/src/core -Iwindows/src/pe -I"$out")
[[ "${NR_INT4:-1}" == 1 ]] && common+=(-DNR_INT4=1)
"$cxx" "${common[@]}" "${NR_PRODUCT_DEFINES[@]}" -c windows/src/core/nr_runtime.cpp -o "$out/nr_runtime.o"
"$cxx" "${common[@]}" -c windows/src/core/nr_native_plan.cpp -o "$out/nr_native_plan.o"
"$cxx" "${common[@]}" -c windows/src/core/nr_renodx.cpp -o "$out/nr_renodx.o"
"$cxx" "${common[@]}" -c windows/src/core/nr_wilsjo2.cpp -o "$out/nr_wilsjo2.o"   # its Stage: HUD FIX and the wilsjo2 layers
"$cxx" "${common[@]}" -c windows/src/pe/nr_vram_ledger.cpp -o "$out/vram_ledger.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_pe_log.cpp -o "$out/log.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_pe_crash.cpp -o "$out/crash.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_pe_interop.cpp -o "$out/interop.o"
"$cxx" "${common[@]}" -I"$minhook/include" -c windows/src/pe/nr_pe_vkdevice.cpp -o "$out/vkdevice.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_pe_session.cpp -o "$out/session.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_pe_bridge.cpp -o "$out/bridge.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_pe_d3d12split.cpp -o "$out/d3d12split.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_pe_config.cpp -o "$out/config.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_screenshot.cpp -o "$out/screenshot.o"
# ReShade's headers include <Windows.h>; the case shim maps that onto mingw's.
"$cxx" "${common[@]}" -Iwindows/src/pe/reshade_compat -I"$reshade_inc" -I"$minhook/include" \
    -Iartifacts/ref/DLSS5-Feeder/external/imgui -Iwindows/src/ui -c windows/src/pe/nr_reshade_addon.cpp -o "$out/reshade_addon.o"
for unit in hook buffer trampoline; do
    "$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/$unit.c" -o "$out/mh_$unit.o"
done
"$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/hde/hde64.c" -o "$out/mh_hde64.o"
ldflags=(-ld3d12 -ld3d11 -ldxgi -ldwmapi -lgdi32 -lole32 -static -static-libgcc -static-libstdc++)

# No static import of vulkan-1.dll (it would load before the game set up its graphics): windows/build/vulkan_lazy.sh.
objs=("$out/reshade_addon.o" "$out/session.o" "$out/bridge.o" "$out/d3d12split.o" "$out/config.o" "$out/screenshot.o" "$out/interop.o"
      "$out/vram_ledger.o" "$out/log.o" "$out/crash.o" "$out/vkdevice.o" "$out/nr_runtime.o" "$out/nr_native_plan.o"
      "$out/nr_renodx.o" "$out/nr_wilsjo2.o" "$out"/mh_*.o)
bash windows/build/vulkan_lazy.sh "$out" "$cxx" -shared "${objs[@]}" "${ldflags[@]}" > /dev/null
"$cxx" -shared -o "$out/dlssnr_amd.addon64" "${objs[@]}" "$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o" "${ldflags[@]}"

# ---- the other binaries -------------------------------------------------------------------------
NR_INT4=${NR_INT4:-1} bash windows/build/build_asi.sh artifacts/windows/asi > /dev/null
bash windows/build/build_vulkan_loader.sh > /dev/null

# ---- package ------------------------------------------------------------------------------------
pkg="$out/package"
rm -rf -- "$pkg"; mkdir -p -- "$pkg"
if [[ -n "$model" ]]; then
    bash windows/build/assemble_product_data.sh "$pkg" --model "$model" ${small:+--small-model "$small"}
else
    bash windows/build/assemble_product_data.sh "$pkg"
fi
# NR_LANG_EXTRA=<dir> (test packages only): its language files (*.txt) into dlssnr-amd/lang and its fonts with their
# licences (fonts/) into dlssnr-amd/lang/fonts, for a language file's @font = fonts/<file>
if [[ -n "${NR_LANG_EXTRA:-}" ]]; then
    [[ -d "$NR_LANG_EXTRA" ]] || { echo "NR_LANG_EXTRA: no such folder: $NR_LANG_EXTRA" >&2; exit 1; }
    cp -- "$NR_LANG_EXTRA"/*.txt "$pkg/dlssnr-amd/lang/"
    [[ -d "$NR_LANG_EXTRA/fonts" ]] && cp -r -- "$NR_LANG_EXTRA/fonts" "$pkg/dlssnr-amd/lang/"
fi
# dlssnr-amd/aco/: the network as ACO's machine code, used wherever the driver takes it (ACO Mode; no setting):
# one record per pipeline (windows/data/aco/records), the Linux network the records were made from
# (linux/build/build_network.py; a record is found by its SPIR-V's hash) and the template aliases.
aco="$pkg/dlssnr-amd/aco"
mkdir -p -- "$aco/records"
rm -rf -- "$out/aco-shaders"
python3 linux/build/build_network.py rdna4 --out "$out/aco-shaders" > /dev/null
cp -r -- "$out/aco-shaders" "$aco/shaders"
cp -- windows/data/aco/records/*.nrp "$aco/records/"
cp -- windows/data/aco/shell-aliases.txt "$aco/"
# CC_SMALL_FP8_EXPERIMENTAL (ACO Mode only): the Linux network built with its changes (linux/build/build_network.py
# --experimental); its records are with the others in aco/records (found by the SPIR-V's hash), its SPIR-V in
# dlssnr-amd/experimental/shaders where the runtime looks for it.
rm -rf -- "$out/aco-shaders-experimental"
python3 linux/build/build_network.py rdna4 --experimental --out "$out/aco-shaders-experimental" > /dev/null
mkdir -p -- "$pkg/dlssnr-amd/experimental"
cp -r -- "$out/aco-shaders-experimental" "$pkg/dlssnr-amd/experimental/shaders"
# RDNA3 (RX 7000, Radeon 700M / 800M / 8000S graphics; nr_gpu_arch.hpp): its own network as ACO's machine code only
# (windows/data/aco/rdna3), the SPIR-V the records were made from (windows/build/build_aco_network.py rdna3) and the
# templates (shells: small shaders with the network's interfaces) the driver compiles for the containers.
rm -rf -- "$out/aco-shaders-rdna3"
python3 windows/build/build_aco_network.py rdna3 --out "$out/aco-shaders-rdna3" > /dev/null
mkdir -p -- "$aco/rdna3/records"
cp -r -- "$out/aco-shaders-rdna3" "$aco/rdna3/shaders"
cp -- windows/data/aco/rdna3/records/*.nrp "$aco/rdna3/records/"
cp -r -- windows/data/aco/rdna3/shells "$aco/rdna3/shells"
cp -- windows/data/aco/rdna3/shell-aliases.txt "$aco/rdna3/"
if [[ "${NR_INT4:-1}" == 1 ]]; then
    # int4 mixed as ACO's machine code (windows/data/aco/int4/records) and the Linux int4 network the records were
    # made from (linux/build/build_network.py --int4)
    rm -rf -- "$out/aco-shaders-int4"
    python3 linux/build/build_network.py rdna4 --int4 --out "$out/aco-shaders-int4" > /dev/null
    mkdir -p -- "$aco/int4/records"
    cp -r -- "$out/aco-shaders-int4" "$aco/int4/shaders"
    cp -- windows/data/aco/int4/records/*.nrp "$aco/int4/records/"
    cp -- windows/data/aco/int4/shell-aliases.txt "$aco/int4/"
    # INT4 adds kernels whose PAL template is one of the native default shaders.
    python3 - "$aco" <<'PY'
import sys
from pathlib import Path
root = Path(sys.argv[1]); aliases = {}
for file in [root / 'shell-aliases.txt', root / 'int4/shell-aliases.txt']:
    for line in file.read_text().splitlines():
        if not line.strip(): continue
        name, shell = line.split()
        if name in aliases and aliases[name] != shell:
            raise SystemExit('conflicting ACO template alias: ' + name)
        aliases[name] = shell
(root / 'shell-aliases.txt').write_text(''.join(f'{a} {b}\n' for a, b in sorted(aliases.items())))
PY
    # dlssnr-amd/int4/: the int4 data (one copy for both platforms, linux/data/int4/rdna4) and the SPIR-V the ACO
    # records belong to
    mkdir -p -- "$pkg/dlssnr-amd/int4"
    cp -r -- linux/data/int4/rdna4/{vit,swin,settings.txt} "$pkg/dlssnr-amd/int4/"
    cp -r -- "$out/aco-shaders-int4" "$pkg/dlssnr-amd/int4/shaders"
    "$cxx" "${common[@]}" "${NR_PRODUCT_DEFINES[@]}" -c windows/src/tools/nr_int4_weights.cpp -o "$out/int4_weights.o"
    # The generator stops before device creation. Vulkan calls abort if this contract is broken.
    syms=$({ "$cxx" -static -municode -o /dev/null "$out/int4_weights.o" "$out/nr_native_plan.o" 2>&1 || true; } |
           grep -o "undefined reference to .vk[A-Za-z0-9_]*" | sed "s/.*to .//" | sort -u)
    { echo '#include <stdlib.h>'; for s in $syms; do echo "void $s(void) { abort(); }"; done; } > "$out/int4_vkstub.c"
    "$cc" -O2 -c "$out/int4_vkstub.c" -o "$out/int4_vkstub.o"
    "$cxx" -static -municode -o "$out/dlssnr-int4-weights.exe" "$out/int4_weights.o" "$out/nr_native_plan.o" "$out/int4_vkstub.o"
    mkdir -p -- "$pkg/model-tools"
    cp -- "$out/dlssnr-int4-weights.exe" "$pkg/model-tools/"
    cp -- linux/data/int4/rdna4/dlssnr-int4.sha256 "$pkg/model-tools/dlssnr-int4.sha256"
fi
# model-tools/: the extractor install.ps1 runs when the package has no model.
bash windows/package/model-tools/build_extract_model.sh "$out/extract" > /dev/null
mkdir -p -- "$pkg/model-tools"
cp -- "$out/extract/dlssnr_extract_model.exe" "$pkg/model-tools/"

# dxvk/: D3D9 (and the DXGI it was installed with) for the DX9 route, as GE-Proton ships them.
mkdir -p -- "$pkg/dxvk"
for f in dxgi.dll d3d9.dll; do cp -- "$proton/lib/wine/dxvk/x86_64-windows/$f" "$pkg/dxvk/"; done
cat "$proton/lib/wine/dxvk/version" > "$pkg/dxvk/version.txt"
cp -- artifacts/ref/dxvk/LICENSE "$pkg/dxvk/LICENSE.txt"

# vulkan/: the Khronos loader the DX9 and Vulkan routes put beside the game (windows/build/build_vulkan_loader.sh):
# never calls DXGI, and finds ReShade's layer manifest beside itself.
mkdir -p -- "$pkg/vulkan"
cp -- artifacts/windows/vulkan-loader/x86_64/vulkan-1.dll "$pkg/vulkan/"
cp -- artifacts/windows/vulkan-loader/x86_64/LICENSE.txt "$pkg/vulkan/Vulkan-Loader-LICENSE.txt"
cp -- artifacts/windows/vulkan-loader/x86_64/PATCHES.diff "$pkg/vulkan/Vulkan-Loader-PATCHES.diff"

# reshade/: ReShade 6.8 (add-on build): installed as dxgi.dll in DX10/11/12 games, and as a Vulkan layer
# (vk-override/, found through that loader) for DX9 and Vulkan games.
rs="$pkg/reshade"
mkdir -p -- "$rs/reshade-shaders/Shaders" "$rs/reshade-shaders/Textures" "$rs/vk-override/implicit_layer"
cp -- "$out/dlssnr_amd.addon64" artifacts/ref/reshade-6.8.0/ReShade64.dll "$rs/"
cp -- artifacts/ref/reshade-6.8.0/ReShade-LICENSE.md "$rs/ReShade-LICENSE.md"
cat > "$rs/vk-override/implicit_layer/ReShade64.json" <<'JSON'
{
  "file_format_version": "1.0.0",
  "layer": {
    "name": "VK_LAYER_reshade",
    "type": "GLOBAL",
    "library_path": "..\\..\\ReShade64.dll",
    "api_version": "1.3.0",
    "implementation_version": "1",
    "description": "ReShade",
    "functions": {
      "vkGetInstanceProcAddr": "vkGetInstanceProcAddr",
      "vkGetDeviceProcAddr": "vkGetDeviceProcAddr",
      "vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion"
    },
    "disable_environment": { "DISABLE_RESHADE": "1" }
  }
}
JSON
cp -- artifacts/ref/DLSS5-Feeder/shaders/DLSS5_Feed.fx "$rs/reshade-shaders/Shaders/"
cp -- artifacts/ref/DLSS5-Feeder/LICENSE "$rs/reshade-shaders/Shaders/DLSS5_Feed-LICENSE.txt"
cp -- artifacts/ref/reshade-shaders/Shaders/ReShade.fxh artifacts/ref/reshade-shaders/Shaders/ReShadeUI.fxh \
      "$rs/reshade-shaders/Shaders/"
cp -r -- artifacts/ref/vort_Shaders/Shaders/. "$rs/reshade-shaders/Shaders/"
cp -r -- artifacts/ref/vort_Shaders/Textures/. "$rs/reshade-shaders/Textures/"
cp -- artifacts/ref/vort_Shaders/LICENSE "$rs/reshade-shaders/Shaders/vort-LICENSE.txt"
cat > "$rs/ReShade.ini" <<'INI'
[GENERAL]
EffectSearchPaths=.\reshade-shaders\Shaders\**
TextureSearchPaths=.\reshade-shaders\Textures\**
PresetPath=.\ReShadePreset.ini
PerformanceMode=0
[OVERLAY]
TutorialProgress=4
[ADDON]
AddonPath=.\
OverlayCollapsed=Generic Depth
INI
cat > "$rs/ReShadePreset.ini" <<'INI'
PreprocessorDefinitions=DLSS5_MV_PROVIDER=2,V_MV_MODE=1,V_ENABLE_MOT_BLUR=0,V_ENABLE_TAA=0
Techniques=vort_MotionEffects@vort_Motion.fx,DLSS5_Feed@DLSS5_Feed.fx
TechniqueSorting=vort_MotionEffects@vort_Motion.fx,DLSS5_Feed@DLSS5_Feed.fx
INI
# Old D3D9 games are not reversed-Z.
sed 's/^PreprocessorDefinitions=.*/&,RESHADE_DEPTH_INPUT_IS_REVERSED=0/' "$rs/ReShadePreset.ini" > "$rs/ReShadePreset-d3d9.ini"
for f in "$rs/ReShade.ini" "$rs/ReShadePreset.ini" "$rs/ReShadePreset-d3d9.ini"; do sed -i 's/$/\r/' "$f"; done

# optiscaler/: OptiScaler as released - the official nightly this package was tested with, unpacked from its archive,
# every file unmodified (optiscaler/release) - with its licence (GPL-3.0, from its source tree at that commit); and our
# NR as an OptiScaler plugin (optiscaler/asi): dlssnr-amd.asi, installed into OptiScaler\plugins, which runs NR on the
# game's own upscaler call and draws NR Control, with NR Control's swap-chain probe (into dlssnr-amd\) and the notices of
# what the plugin has compiled in (NR Control's font Inter, SIL OFL 1.1; Dear ImGui, MIT).
if [[ -z "${NR_OPTI_ARCHIVE:-}" ]]; then   # the archive the package was tested with
    [[ "$(sha256sum -- "$opti_7z" | cut -d' ' -f1)" == db4fe2deae7edd9f6657f5cb223edc694ebdec62b52f15a3a59913f8b656c176 ]] ||
        { echo "$opti_7z is not the tested OptiScaler nightly-20261030" >&2; exit 1; }
fi
command -v 7z >/dev/null || { echo "7z is needed to unpack $opti_7z" >&2; exit 1; }
op="$pkg/optiscaler"
mkdir -p -- "$op/release" "$op/asi"
7z x -y -o"$op/release" -- "$opti_7z" > /dev/null
[[ -f "$op/release/OptiScaler.dll" && -f "$op/release/OptiScaler.ini" && -d "$op/release/OptiScaler" ]] ||
    { echo "$opti_7z is not laid out as OptiScaler v10" >&2; exit 1; }
cp -- "$opti_license" "$op/OptiScaler_LICENSE.txt"
cp -- artifacts/windows/asi/dlssnr-amd.asi artifacts/windows/asi/nr_present_probe.exe "$op/asi/"
cp -- "${NR_MENU_FONT_LICENSE:-toolchain/fonts/inter-4.1/LICENSE.txt}" "$op/asi/Inter-LICENSE.txt"
cp -- "${NR_IMGUI_ROOT:-toolchain/imgui-nr}/LICENSE.txt" "$op/asi/DearImGui-LICENSE.txt"
cmp -s <(cd artifacts/windows/asi/shaders/wilsjo2 && sha256sum -- *.spv) <(cd "$pkg/dlssnr-amd/shaders/wilsjo2" && sha256sum -- *.spv) ||
    { echo "the plugin's wilsjo2 shaders are not dlssnr-amd/shaders/wilsjo2's" >&2; exit 1; }

# experimental/: what install.ps1 makes CC_SMALL_FP8_EXPERIMENTAL's weights with - the difference of its fitted weights
# from the small model's (linux/data/experimental: each changed entry XOR the small model's, so it holds none of NVIDIA's
# weights) and the SHA-256 of the result.
mkdir -p -- "$pkg/experimental"
cp -- linux/data/experimental/dlssnr-small-experimental.delta "$pkg/experimental/"
cp -- linux/data/experimental/dlssnr-small-experimental.sha256 "$pkg/experimental/dlssnr-small-experimental.bin.sha256"

# dlssnr-amd.ini, put in place at install: the text the programs write themselves (windows/package/make_ini.py),
# with the line ends a Windows program writes.
# [Network] Model CC_SMALL_FP8; install.ps1 writes the network chosen at install.
python3 windows/package/make_ini.py windows/src/pe/nr_pe_config.cpp "$pkg/ini" > /dev/null
sed -i 's/$/\r/' "$pkg/ini/"*.ini

cp -- windows/package/install.bat windows/package/README.txt "$pkg/"
# Windows PowerShell 5.1 reads a script without a byte-order mark in the ANSI code page: one UTF-8 BOM each (a
# second one would be text, and the script's first line a command), any BOM of the source dropped first.
printf '\xef\xbb\xbf' > "$pkg/install.ps1"; sed '1s/^\xEF\xBB\xBF//' windows/package/install.ps1 >> "$pkg/install.ps1"
printf '\xef\xbb\xbf' > "$pkg/README.txt"; sed '1s/^\xEF\xBB\xBF//; s/$/\r/' windows/package/README.txt >> "$pkg/README.txt"
for f in install.ps1 README.txt; do
    [[ "$(head -c 6 "$pkg/$f" | od -An -tx1 | tr -d ' \n')" == efbbbf* && "$(head -c 6 "$pkg/$f" | od -An -tx1 | tr -d ' \n')" != efbbbfefbbbf ]] ||
        { echo "$pkg/$f: not exactly one UTF-8 BOM" >&2; exit 1; }
done
sed -i 's/$/\r/' "$pkg/install.bat"

# ---- archive ------------------------------------------------------------------------------------
# A Windows-only release's version already says preview (v0.0.4-win-preview).
preview=-preview; [[ "$stamp" == *preview* ]] && preview=
name="DLSSNR-AMD-Vulkan-Windows-$stamp$preview-x86_64"
rm -f -- "$out/$name.zip"
python3 - "$pkg" "$out/$name.zip" <<'PY'
import os, sys, zipfile
root, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, _, files in sorted(os.walk(root)):
        for f in sorted(files):
            full = os.path.join(d, f)
            z.write(full, os.path.relpath(full, root))
PY
mv -f -- "$out/$name.zip" "windows/package/$name.zip"
du -sh -- "$pkg"
sha256sum -- "windows/package/$name.zip"
