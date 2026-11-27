#!/usr/bin/env bash
# Build the Windows package (64-bit games on the AMD Windows driver):
#   dlssnr-amd/     the network: Windows shaders (windows/build/assemble_product_data.sh), and in aco/ the
#                   same network as ACO's machine code (ACO Mode, always tried first): RDNA4's, the int4 layers' in
#                   int4/, RDNA3's and RDNA3.5's in rdna3/; each optimized network's SPIR-V and data in a folder of its
#                   codename and level (CC_SMALL_FP8_OPTIMIZED, CC_SMALL_FP8_OPTIMIZED-performance,
#                   CC_SMALL_FP8_OPTIMIZED-ultra, CC_SILVER_AARDWOLD_OPTIMIZED, CC_SILVER_AARDWOLD_OPTIMIZED-balanced,
#                   CC_SILVER_AARDWOLD_OPTIMIZED-performance, CC_SILVER_AARDWOLD_OPTIMIZED-ultra)
#   model-tools/    dlssnr_extract_model.exe: install.ps1 makes the model from the user's nvngx_dlssnr.dll;
#                   dlssnr-int4-weights.exe: and the int4 layers' weights from the optimized networks'
#   CC_SMALL_FP8_OPTIMIZED/        its weights as a difference from CC_SMALL_FP8's (install.ps1 applies it)
#   CC_SILVER_AARDWOLD_OPTIMIZED/  its weights as a difference from CC_SILVER_AARDWOLD's (the same)
#   optiscaler/     OptiScaler (the official nightly, unmodified) and our NR plugin for it
#   standalone/     (x86_64 only) dxgi.dll + vk-override/ for any 64-bit DX11/12/Vulkan app without OptiScaler or ReShade
#   reshade/        ReShade + our add-on: dxgi.dll in DX10/11/12 games, a Vulkan layer otherwise
#   dxvk/           D3D9 -> Vulkan, for the DX9 route only
#   vulkan/         the Khronos loader the Vulkan-layer routes put beside the game
#   install.bat, install.ps1, README.txt
#
#   [NR_MODEL_SILVER_AARDWOLD=CC_SILVER_AARDWOLD.bin [NR_MODEL_SMALL_FP8=CC_SMALL_FP8.bin]] [NR_INT4=0] [NR_LANG_EXTRA=<dir>] [NR_SIGN=1] bash windows/build/build_package.sh [build dir]
#
# The package carries no model: install.ps1 extracts it from the user's nvngx_dlssnr.dll on the first
# install. NR_MODEL_SILVER_AARDWOLD puts a CC_SILVER_AARDWOLD.bin you made yourself into the package (for your own use
# only), and with it NR_MODEL_SMALL_FP8 a CC_SMALL_FP8.bin (from the 310.9.16 DLL).
#
# NR_INT4 (default 1): the programs built with CC_SILVER_AARDWOLD_OPTIMIZED's int4 layers and their data in the
# package; install.ps1 installs all four networks (CC_SMALL_FP8, CC_SMALL_FP8_OPTIMIZED, CC_SILVER_AARDWOLD,
# CC_SILVER_AARDWOLD_OPTIMIZED; the two optimized ones run with ACO Mode only) and asks only which one a game starts
# with. NR_INT4=0: no int4 layers.
#
# D3D10/11/12 games keep the system's own D3D: the network runs on a Vulkan device of its own and the
# frame crosses over through shared textures and a shared fence (windows/src/pe/nr_pe_bridge.hpp).
# Only DX9 games are translated (DXVK), because D3D9 shares neither textures nor fences.
set -euo pipefail
model=""
if [[ -n "${NR_MODEL_SILVER_AARDWOLD:-}" ]]; then
    [[ -f "$NR_MODEL_SILVER_AARDWOLD" ]] || { echo "NR_MODEL_SILVER_AARDWOLD: no such file: $NR_MODEL_SILVER_AARDWOLD" >&2; exit 1; }
    model=$(realpath -- "$NR_MODEL_SILVER_AARDWOLD")
fi
model_small_fp8=""
if [[ -n "$model" && -n "${NR_MODEL_SMALL_FP8:-}" ]]; then
    [[ -f "$NR_MODEL_SMALL_FP8" ]] || { echo "NR_MODEL_SMALL_FP8: no such file: $NR_MODEL_SMALL_FP8" >&2; exit 1; }
    model_small_fp8=$(realpath -- "$NR_MODEL_SMALL_FP8")
fi
cd -- "$(dirname -- "$0")/../.."
bash windows/build/check_paths.sh   # UTF-8 paths only (a game folder with any letters)
bash windows/build/check_imgui.sh   # ReShade's ImGui is MSVC's: no ImVec2-returning call from the add-on's code
python3 windows/build/check_outer_aco.py > /dev/null
out=$(realpath -m -- "${1:-artifacts/windows/package-build}")
case "$out" in "$(pwd)"/*) ;; *) echo 'build directory must be in the project' >&2; exit 2;; esac
mkdir -p -- "$out"

cxx=x86_64-w64-mingw32-g++
cc=x86_64-w64-mingw32-gcc
command -v "$cxx" >/dev/null || { echo "no mingw cross compiler ($cxx)" >&2; exit 1; }
minhook=artifacts/ref/DLSS5-Feeder/external/minhook
reshade_inc=artifacts/ref/DLSS5-Feeder/external/reshade/include
dxvk=${NR_DXVK:-artifacts/ref/dxvk-ge}   # DXVK from the latest GE-Proton release (fetch_deps.sh --windows)
opti_7z=${NR_OPTI_ARCHIVE:-artifacts/ref/downloads/OptiScaler_v10.0.0-pre1_20261030.7z}
opti_license=${NR_OPTI_LICENSE:-artifacts/ref/optiscaler-nightly-20261030/LICENSE}
for dir in "$minhook" "$reshade_inc" artifacts/ref/reshade-shaders artifacts/ref/vort_Shaders \
           "$dxvk/x86_64-windows"; do
    [[ -d "$dir" ]] || { echo "missing: $dir" >&2; exit 1; }
done
for f in "$opti_7z" "$opti_license" artifacts/ref/reshade-6.8.0/ReShade64.dll; do
    [[ -f "$f" ]] || { echo "missing: $f" >&2; exit 1; }
done
source windows/build/arch/rdna4.sh
source windows/build/parallel.sh

# ---- the ReShade add-on -----------------------------------------------------------------------
stamp=$(bash windows/build/version.sh)
python3 windows/build/generate_device_chain.py toolchain/Vulkan-Headers "$out/nr_device_chain_sizes.hpp"
common=(-std=c++17 -O2 -DNDEBUG -DNR_BUILD_STAMP="\"$stamp\"" -Itoolchain/Vulkan-Headers/include -Iwindows/src
        -Iwindows/src/core -Iwindows/src/pe -I"$out")
# (NR_RESHADE_ROUTE: these are the ReShade add-on's objects, all of them - the route's defaults, nr_fast_passes.hpp)
common+=(-DNR_RESHADE_ROUTE=1)
[[ "${NR_INT4:-1}" == 1 ]] && common+=(-DNR_INT4=1)
par "$cxx" "${common[@]}" "${NR_PRODUCT_DEFINES[@]}" -c windows/src/core/nr_runtime.cpp -o "$out/nr_runtime.o"
par "$cxx" "${common[@]}" -c windows/src/core/nr_native_plan.cpp -o "$out/nr_native_plan.o"
par "$cxx" "${common[@]}" -c windows/src/core/nr_renodx.cpp -o "$out/nr_renodx.o"
par "$cxx" "${common[@]}" -c windows/src/core/nr_wilsjo2.cpp -o "$out/nr_wilsjo2.o"   # its Stage: HUD FIX and the wilsjo2 layers
par "$cxx" "${common[@]}" -c windows/src/pe/nr_vram_ledger.cpp -o "$out/vram_ledger.o"
par "$cxx" "${common[@]}" -c windows/src/pe/nr_pe_log.cpp -o "$out/log.o"
par "$cxx" "${common[@]}" -c windows/src/pe/nr_pe_crash.cpp -o "$out/crash.o"
par "$cxx" "${common[@]}" -c windows/src/pe/nr_pe_interop.cpp -o "$out/interop.o"
par "$cxx" "${common[@]}" -I"$minhook/include" -c windows/src/pe/nr_pe_vkdevice.cpp -o "$out/vkdevice.o"
par "$cxx" "${common[@]}" -c windows/src/pe/nr_pe_session.cpp -o "$out/session.o"
par "$cxx" "${common[@]}" -c windows/src/pe/nr_pe_bridge.cpp -o "$out/bridge.o"
par "$cxx" "${common[@]}" -c windows/src/pe/nr_pe_d3d12split.cpp -o "$out/d3d12split.o"
par "$cxx" "${common[@]}" -c windows/src/pe/nr_pe_config.cpp -o "$out/config.o"
par "$cxx" "${common[@]}" -c windows/src/pe/nr_screenshot.cpp -o "$out/screenshot.o"
par "$cxx" "${common[@]}" -c windows/src/core/nr_frame_reuse.cpp -o "$out/nr_frame_reuse.o"   # NRFrameReuse (the session's)
# ReShade's headers include <Windows.h>; the case shim maps that onto mingw's.
par "$cxx" "${common[@]}" -idirafter windows/src/pe/reshade_compat -I"$reshade_inc" -I"$minhook/include" \
    -Iartifacts/ref/DLSS5-Feeder/external/imgui -Iwindows/src/ui -c windows/src/pe/nr_reshade_addon.cpp -o "$out/reshade_addon.o"
for unit in hook buffer trampoline; do
    par "$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/$unit.c" -o "$out/mh_$unit.o"
done
par "$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/hde/hde64.c" -o "$out/mh_hde64.o"
# the Linux network sets the ACO records belong to (linux/build/build_network.py; copied into the package below), beside
# the compiler runs
rm -rf -- "$out"/aco-shaders "$out"/aco-shaders-*
par python3 linux/build/build_network.py rdna4 --out "$out/aco-shaders" > /dev/null
for n in CC_SMALL_FP8_OPTIMIZED CC_SMALL_FP8_OPTIMIZED-performance CC_SILVER_AARDWOLD_OPTIMIZED; do
    par python3 linux/build/build_network.py rdna4 --network "$n" --out "$out/aco-shaders-$n" > /dev/null
done
if [[ "${NR_INT4:-1}" == 1 ]]; then
    for n in CC_SILVER_AARDWOLD_OPTIMIZED-balanced CC_SILVER_AARDWOLD_OPTIMIZED-performance CC_SILVER_AARDWOLD_OPTIMIZED-ultra \
             CC_SMALL_FP8_OPTIMIZED-ultra; do
        par python3 linux/build/build_network.py rdna4 --network "$n" --out "$out/aco-shaders-$n" > /dev/null
    done
fi
par_wait
ldflags=(-ld3d12 -ld3d11 -ldxgi -ldwmapi -lgdi32 -lole32 -static -static-libgcc -static-libstdc++)

# No static import of vulkan-1.dll (it would load before the game set up its graphics): windows/build/vulkan_lazy.sh.
objs=("$out/reshade_addon.o" "$out/session.o" "$out/bridge.o" "$out/d3d12split.o" "$out/config.o" "$out/screenshot.o" "$out/interop.o"
      "$out/vram_ledger.o" "$out/log.o" "$out/crash.o" "$out/vkdevice.o" "$out/nr_runtime.o" "$out/nr_native_plan.o"
      "$out/nr_renodx.o" "$out/nr_frame_reuse.o" "$out/nr_wilsjo2.o" "$out"/mh_*.o)
bash windows/build/vulkan_lazy.sh "$out" "$cxx" -shared "${objs[@]}" "${ldflags[@]}" > /dev/null
"$cxx" -shared -o "$out/dlssnr_amd.addon64" "${objs[@]}" "$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o" "${ldflags[@]}"

# ---- the other binaries -------------------------------------------------------------------------
NR_INT4=${NR_INT4:-1} bash windows/build/build_asi.sh artifacts/windows/asi > /dev/null
bash windows/build/build_vulkan_loader.sh > /dev/null
bash windows/build/x86/build32.sh "$minhook" "$out/x86" > /dev/null
NR_ARCH=i686 bash windows/build/build_vulkan_loader.sh > /dev/null

# ---- package ------------------------------------------------------------------------------------
pkg="$out/package"
rm -rf -- "$pkg"; mkdir -p -- "$pkg"
if [[ -n "$model" ]]; then
    bash windows/build/assemble_product_data.sh "$pkg" --silver-aardwold-model "$model" ${model_small_fp8:+--small-fp8-model "$model_small_fp8"}
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
cp -r -- "$out/aco-shaders" "$aco/shaders"
cp -- windows/data/aco/records/*.nrp "$aco/records/"
cp -- windows/data/aco/d3d12_outer.nra "$aco/"
cp -- windows/data/aco/shell-aliases.txt "$aco/"
# CC_SMALL_FP8_OPTIMIZED (ACO Mode only): the Linux network built with its changes (linux/build/build_network.py
# --network CC_SMALL_FP8_OPTIMIZED: Quality; CC_SMALL_FP8_OPTIMIZED-performance: Performance); its records are with the
# others in aco/records (found by the SPIR-V's hash), its SPIR-V in dlssnr-amd/CC_SMALL_FP8_OPTIMIZED/shaders and
# dlssnr-amd/CC_SMALL_FP8_OPTIMIZED-performance/shaders where the runtime looks for it. CC_SILVER_AARDWOLD_OPTIMIZED
# (ACO Mode only) the same way: --network CC_SILVER_AARDWOLD_OPTIMIZED, its records in aco/records, its SPIR-V in
# dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED/shaders.
for n in CC_SMALL_FP8_OPTIMIZED CC_SMALL_FP8_OPTIMIZED-performance CC_SILVER_AARDWOLD_OPTIMIZED; do
    mkdir -p -- "$pkg/dlssnr-amd/$n"
    cp -r -- "$out/aco-shaders-$n" "$pkg/dlssnr-amd/$n/shaders"
done
# RDNA3 and RDNA3.5 (nr_gpu_arch.hpp): their own network as ACO's machine code only (windows/data/aco/rdna3/records, one
# record a pipeline), and in place of its SPIR-V the stand-ins the runtime takes for it (rdna3/shaders,
# rdna3/CC_SMALL_FP8_OPTIMIZED/shaders, rdna3/CC_SILVER_AARDWOLD_OPTIMIZED/shaders: each pipeline's interface shell
# naming its record, nr_pipeline_binary.hpp record_reference, with the network's marker files) and the templates
# (shells: small shaders with the network's interfaces) the driver compiles for the containers. CC_SMALL_FP8_OPTIMIZED
# and CC_SILVER_AARDWOLD_OPTIMIZED (at Quality) have their records with the default ones.
mkdir -p -- "$aco/rdna3/records" "$aco/rdna3/CC_SMALL_FP8_OPTIMIZED" "$aco/rdna3/CC_SILVER_AARDWOLD_OPTIMIZED"
cp -r -- windows/data/aco/rdna3/shaders "$aco/rdna3/shaders"
cp -r -- windows/data/aco/rdna3/CC_SMALL_FP8_OPTIMIZED/shaders "$aco/rdna3/CC_SMALL_FP8_OPTIMIZED/shaders"
cp -r -- windows/data/aco/rdna3/CC_SILVER_AARDWOLD_OPTIMIZED/shaders "$aco/rdna3/CC_SILVER_AARDWOLD_OPTIMIZED/shaders"
cp -- windows/data/aco/rdna3/records/*.nrp "$aco/rdna3/records/"
cp -- windows/data/aco/rdna3/d3d12_outer.nra "$aco/rdna3/"
cp -r -- windows/data/aco/rdna3/shells "$aco/rdna3/shells"
cp -- windows/data/aco/rdna3/shell-aliases.txt "$aco/rdna3/"
if [[ "${NR_INT4:-1}" == 1 ]]; then
    # The int4 layers as ACO's machine code (windows/data/aco/int4/records: the records of CC_SILVER_AARDWOLD_OPTIMIZED's
    # three int4 levels, Balanced, Performance and Ultra Performance, and of CC_SMALL_FP8_OPTIMIZED's Ultra Performance -
    # one folder, the records are named by their SPIR-V) and the Linux networks the records were made from
    # (linux/build/build_network.py --network <name>; the optimized builds read their SPIR-V from their own dlssnr-amd
    # folder, --spv-dir-aco)
    mkdir -p -- "$aco/int4/records"
    cp -r -- "$out/aco-shaders-CC_SILVER_AARDWOLD_OPTIMIZED-balanced" "$aco/int4/shaders"
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
    # dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED-balanced/: the int4 data (one copy for both platforms,
    # linux/data/CC_SILVER_AARDWOLD_OPTIMIZED-balanced/rdna4) and the SPIR-V the ACO records belong to;
    # CC_SILVER_AARDWOLD_OPTIMIZED-performance/ (Balanced's tables, its network) and CC_SILVER_AARDWOLD_OPTIMIZED-ultra/
    # (Balanced's tables with linux/data/CC_SILVER_AARDWOLD_OPTIMIZED-ultra/rdna4 added, its settings.txt, its network);
    # install.ps1 makes the int4 weights of all three from the -ultra tables
    for n in CC_SILVER_AARDWOLD_OPTIMIZED-balanced CC_SILVER_AARDWOLD_OPTIMIZED-performance CC_SILVER_AARDWOLD_OPTIMIZED-ultra; do
        mkdir -p -- "$pkg/dlssnr-amd/$n"
        cp -r -- linux/data/CC_SILVER_AARDWOLD_OPTIMIZED-balanced/rdna4/{vit,swin,settings.txt} "$pkg/dlssnr-amd/$n/"
        [[ $n == *-ultra ]] && cp -r -- linux/data/CC_SILVER_AARDWOLD_OPTIMIZED-ultra/rdna4/. "$pkg/dlssnr-amd/$n/"
        cp -r -- "$out/aco-shaders-$n" "$pkg/dlssnr-amd/$n/shaders"
    done
    # CC_SMALL_FP8_OPTIMIZED at Ultra Performance (RDNA4): dlssnr-amd/CC_SMALL_FP8_OPTIMIZED-ultra - its ViT int4 tables
    # (vit/; vit-sink/ for the 1440p-class frames), settings.txt and the Linux network its records (aco/int4/records)
    # belong to (linux/build/build_network.py --network CC_SMALL_FP8_OPTIMIZED-ultra); install.ps1 makes its int4 weights
    # (dlssnr-int4-weights.exe --network CC_SMALL_FP8_OPTIMIZED [--sink]) and checks them against the SHA-256 files
    mkdir -p -- "$pkg/dlssnr-amd/CC_SMALL_FP8_OPTIMIZED-ultra"
    cp -r -- linux/data/CC_SMALL_FP8_OPTIMIZED-ultra/rdna4/{vit,vit-sink,settings.txt} "$pkg/dlssnr-amd/CC_SMALL_FP8_OPTIMIZED-ultra/"
    cp -r -- "$out/aco-shaders-CC_SMALL_FP8_OPTIMIZED-ultra" "$pkg/dlssnr-amd/CC_SMALL_FP8_OPTIMIZED-ultra/shaders"
    # RDNA3's int4 levels (the same tables as above): their gfx11 records (aco/rdna3/int4/records, the iu4 rewrite of
    # the NR_I4_MMA2 pairs made with them) and the stand-ins naming them, one folder a level as dlssnr-amd's
    mkdir -p -- "$aco/rdna3/int4"
    cp -r -- windows/data/aco/rdna3/int4/records "$aco/rdna3/int4/records"
    for lv in CC_SILVER_AARDWOLD_OPTIMIZED-balanced CC_SILVER_AARDWOLD_OPTIMIZED-performance CC_SILVER_AARDWOLD_OPTIMIZED-ultra; do
        mkdir -p -- "$aco/rdna3/$lv"
        cp -r -- "windows/data/aco/rdna3/$lv/shaders" "$aco/rdna3/$lv/shaders"
    done
    "$cxx" "${common[@]}" "${NR_PRODUCT_DEFINES[@]}" -c windows/src/tools/nr_int4_weights.cpp -o "$out/int4_weights.o"
    # The generator stops before device creation. Vulkan calls abort if this contract is broken.
    syms=$({ "$cxx" -static -municode -o /dev/null "$out/int4_weights.o" "$out/nr_native_plan.o" 2>&1 || true; } |
           grep -o "undefined reference to .vk[A-Za-z0-9_]*" | sed "s/.*to .//" | sort -u)
    { echo '#include <stdlib.h>'; for s in $syms; do echo "void $s(void) { abort(); }"; done; } > "$out/int4_vkstub.c"
    "$cc" -O2 -c "$out/int4_vkstub.c" -o "$out/int4_vkstub.o"
    "$cxx" -static -municode -o "$out/dlssnr-int4-weights.exe" "$out/int4_weights.o" "$out/nr_native_plan.o" "$out/int4_vkstub.o"
    mkdir -p -- "$pkg/model-tools"
    cp -- "$out/dlssnr-int4-weights.exe" "$pkg/model-tools/"
    cp -- linux/data/CC_SILVER_AARDWOLD_OPTIMIZED-balanced/rdna4/CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin.sha256 "$pkg/model-tools/CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin.sha256"
    cp -- linux/data/CC_SMALL_FP8_OPTIMIZED-ultra/rdna4/CC_SMALL_FP8_OPTIMIZED_INT4.bin.sha256 "$pkg/model-tools/CC_SMALL_FP8_OPTIMIZED_INT4.bin.sha256"
    cp -- linux/data/CC_SMALL_FP8_OPTIMIZED-ultra/rdna4/CC_SMALL_FP8_OPTIMIZED_INT4_SINK.bin.sha256 "$pkg/model-tools/CC_SMALL_FP8_OPTIMIZED_INT4_SINK.bin.sha256"
fi
# model-tools/: the extractor install.ps1 runs when the package has no model.
bash windows/package/model-tools/build_extract_model.sh "$out/extract" > /dev/null
mkdir -p -- "$pkg/model-tools"
cp -- "$out/extract/dlssnr_extract_model.exe" "$pkg/model-tools/"

# dxvk/: D3D9 (and the DXGI it was installed with) for the DX9 route, as GE-Proton ships them.
mkdir -p -- "$pkg/dxvk"
for f in dxgi.dll d3d9.dll; do cp -- "$dxvk/x86_64-windows/$f" "$pkg/dxvk/"; done
mkdir -p -- "$pkg/dxvk/x86"
for f in dxgi.dll d3d9.dll; do cp -- "$dxvk/i386-windows/$f" "$pkg/dxvk/x86/"; done
cat "$dxvk/version" > "$pkg/dxvk/version.txt"
cp -- artifacts/ref/dxvk/LICENSE "$pkg/dxvk/LICENSE.txt"

# vulkan/: the Khronos loader the DX9 and Vulkan routes put beside the game (windows/build/build_vulkan_loader.sh):
# never calls DXGI, and finds ReShade's layer manifest beside itself.
mkdir -p -- "$pkg/vulkan"
cp -- artifacts/windows/vulkan-loader/x86_64/vulkan-1.dll "$pkg/vulkan/"
cp -- artifacts/windows/vulkan-loader/x86_64/LICENSE.txt "$pkg/vulkan/Vulkan-Loader-LICENSE.txt"
cp -- artifacts/windows/vulkan-loader/x86_64/PATCHES.diff "$pkg/vulkan/Vulkan-Loader-PATCHES.diff"
mkdir -p -- "$pkg/vulkan/x86"
cp -- artifacts/windows/vulkan-loader/i686/vulkan-1.dll "$pkg/vulkan/x86/"
cp -- artifacts/windows/vulkan-loader/i686/LICENSE.txt "$pkg/vulkan/x86/Vulkan-Loader-LICENSE.txt"
cp -- artifacts/windows/vulkan-loader/i686/PATCHES.diff "$pkg/vulkan/x86/Vulkan-Loader-PATCHES.diff"

# reshade/: ReShade 6.8 (add-on build): installed as dxgi.dll in DX10/11/12 games, and as a Vulkan layer
# (vk-override/, found through that loader) for DX9 and Vulkan games.
rs="$pkg/reshade"
mkdir -p -- "$rs/reshade-shaders/Shaders" "$rs/reshade-shaders/Textures" "$rs/vk-override/implicit_layer"
cp -- "$out/dlssnr_amd.addon64" artifacts/ref/reshade-6.8.0/ReShade64.dll "$rs/"
cp -- "$out/x86/dlssnr_amd.addon32" artifacts/ref/reshade-6.8.0/ReShade32.dll "$rs/"
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
sed 's/ReShade64.dll/ReShade32.dll/' "$rs/vk-override/implicit_layer/ReShade64.json" > "$rs/vk-override/implicit_layer/ReShade32.json"
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
opti_commit=$(sed -n 's|.*raw\.githubusercontent\.com/optiscaler/OptiScaler/\([0-9a-f]\{40\}\)/LICENSE.*|\1|p' fetch_deps.sh | head -1)
opti_url=$(sed -n 's|.*\(https://github\.com/optiscaler/[^ ]*\.7z\).*|\1|p' fetch_deps.sh | head -1)
{
    echo "OptiScaler is distributed unmodified as an independent aggregate under the GNU General Public License v3.0 (see OptiScaler_LICENSE.txt), with no additional restrictions."
    echo "Upstream source repository: https://github.com/optiscaler/OptiScaler"
    [[ -n "$opti_commit" ]] && echo "Corresponding source commit: https://github.com/optiscaler/OptiScaler/tree/$opti_commit"
    [[ -n "$opti_url" ]] && echo "Upstream binary archive: $opti_url"
} > "$op/OptiScaler_SOURCE.txt"
cp -- artifacts/windows/asi/dlssnr-amd.asi artifacts/windows/asi/nr_present_probe.exe "$op/asi/"
cp -- "${NR_MENU_FONT_LICENSE:-toolchain/fonts/inter-4.1/LICENSE.txt}" "$op/asi/Inter-LICENSE.txt"
cp -- "${NR_IMGUI_ROOT:-toolchain/imgui-nr}/LICENSE.txt" "$op/asi/DearImGui-LICENSE.txt"
cmp -s <(cd artifacts/windows/asi/shaders/wilsjo2 && sha256sum -- *.spv) <(cd "$pkg/dlssnr-amd/shaders/wilsjo2" && sha256sum -- *.spv) ||
    { echo "the plugin's wilsjo2 shaders are not dlssnr-amd/shaders/wilsjo2's" >&2; exit 1; }

# standalone/ (x86_64 only; removed from package-i686 below): DX11 / DX12 / Vulkan apps without OptiScaler or ReShade.
# install.ps1 reuses optiscaler\asi\dlssnr-amd.asi, nr_present_probe.exe and font/ImGui licences (no duplication here).
st="$pkg/standalone"
mkdir -p -- "$st/vk-override/implicit_layer"
cp -- artifacts/windows/asi/dxgi-standalone.dll "$st/dxgi.dll"
cat > "$st/vk-override/implicit_layer/dlssnr_standalone.json" <<'JSON'
{
  "file_format_version": "1.0.0",
  "layer": {
    "name": "VK_LAYER_DLSSNR_standalone",
    "type": "GLOBAL",
    "library_path": "..\\..\\dlssnr-amd.asi",
    "api_version": "1.3.0",
    "implementation_version": "1",
    "description": "DLSSNR-AMD standalone layer",
    "functions": {
      "vkGetInstanceProcAddr": "NR_Layer_GetInstanceProcAddr",
      "vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion"
    },
    "disable_environment": { "NR_DISABLE_VK_STANDALONE": "1" }
  }
}
JSON

# CC_SMALL_FP8_OPTIMIZED/: what install.ps1 makes CC_SMALL_FP8_OPTIMIZED's weights with - the difference of its fitted
# weights from CC_SMALL_FP8's (linux/data/CC_SMALL_FP8_OPTIMIZED: each changed entry XOR CC_SMALL_FP8's, so it holds none
# of NVIDIA's weights; Performance's own entries added as "performance/<name>") and the SHA-256 of the result.
# CC_SILVER_AARDWOLD_OPTIMIZED/: the same for CC_SILVER_AARDWOLD_OPTIMIZED, from CC_SILVER_AARDWOLD's weights
# (linux/data/CC_SILVER_AARDWOLD_OPTIMIZED); the int4 layers' weights are made from the result
# (model-tools/dlssnr-int4-weights.exe).
for n in CC_SMALL_FP8_OPTIMIZED CC_SILVER_AARDWOLD_OPTIMIZED; do
    mkdir -p -- "$pkg/$n"
    cp -- "linux/data/$n/$n.delta" "linux/data/$n/$n.bin.sha256" "$pkg/$n/"
done

# dlssnr-amd.ini, put in place at install: the text the programs write themselves (windows/package/make_ini.py),
# with the line ends a Windows program writes.
# [Network] Model CC_SMALL_FP8; install.ps1 writes the network chosen at install.
python3 windows/package/make_ini.py windows/src/pe/nr_pe_config.cpp "$pkg/ini" > /dev/null
sed -i 's/$/\r/' "$pkg/ini/"*.ini

cp -- windows/package/install.bat windows/package/README.txt LICENSE NOTICE "$pkg/"
# Windows PowerShell 5.1 reads a script without a byte-order mark in the ANSI code page: one UTF-8 BOM each (a
# second one would be text, and the script's first line a command), any BOM of the source dropped first.
printf '\xef\xbb\xbf' > "$pkg/install.ps1"; sed '1s/^\xEF\xBB\xBF//' windows/package/install.ps1 >> "$pkg/install.ps1"
printf '\xef\xbb\xbf' > "$pkg/README.txt"; sed '1s/^\xEF\xBB\xBF//; s/$/\r/' windows/package/README.txt >> "$pkg/README.txt"
for f in install.ps1 README.txt; do
    [[ "$(head -c 6 "$pkg/$f" | od -An -tx1 | tr -d ' \n')" == efbbbf* && "$(head -c 6 "$pkg/$f" | od -An -tx1 | tr -d ' \n')" != efbbbfefbbbf ]] ||
        { echo "$pkg/$f: not exactly one UTF-8 BOM" >&2; exit 1; }
done
sed -i 's/$/\r/' "$pkg/install.bat"

# Two packages, as Linux has them: x86_64 (64-bit games, every route) and i686 (32-bit games: the ReShade routes). The
# i686 one is the same folder without OptiScaler (64-bit only) and with the 32-bit loader, DXVK, ReShade and add-on in
# the places the 64-bit ones take; each install.ps1 knows its games ($PackageBits) and names the other package for an
# exe of the other kind.
pkg32="$out/package-i686"
rm -rf -- "$pkg32"; cp -a --reflink=auto -- "$pkg" "$pkg32"
rm -rf -- "$pkg32/optiscaler" "$pkg32/standalone"
rm -f -- "$pkg32/ini/dlssnr-amd-standalone.ini"
for f in dxgi.dll d3d9.dll; do mv -f -- "$pkg32/dxvk/x86/$f" "$pkg32/dxvk/$f"; done
for f in vulkan-1.dll Vulkan-Loader-LICENSE.txt Vulkan-Loader-PATCHES.diff; do mv -f -- "$pkg32/vulkan/x86/$f" "$pkg32/vulkan/$f"; done
rmdir -- "$pkg32/dxvk/x86" "$pkg32/vulkan/x86"
rm -f -- "$pkg32/reshade/ReShade64.dll" "$pkg32/reshade/dlssnr_amd.addon64" "$pkg32/reshade/vk-override/implicit_layer/ReShade64.json"
sed -i 's/^\$PackageBits = 64$/$PackageBits = 32/' "$pkg32/install.ps1"
grep -q '^\$PackageBits = 32' "$pkg32/install.ps1" || { echo "$pkg32/install.ps1: \$PackageBits not set to 32" >&2; exit 1; }
rm -rf -- "$pkg/dxvk/x86" "$pkg/vulkan/x86"
rm -f -- "$pkg/reshade/ReShade32.dll" "$pkg/reshade/dlssnr_amd.addon32" "$pkg/reshade/vk-override/implicit_layer/ReShade32.json"
grep -q '^\$PackageBits = 64' "$pkg/install.ps1" || { echo "$pkg/install.ps1: \$PackageBits is not 64" >&2; exit 1; }

# NR_SIGN=1: every Windows binary without a signature of its own signed with a self-signed certificate made for this
# build (windows/build/sign_pe.py; some games load only signed DLLs and do not ask by whom)
[[ "${NR_SIGN:-0}" == 1 ]] && python3 windows/build/sign_pe.py "$pkg" && python3 windows/build/sign_pe.py "$pkg32"

# ---- archive ------------------------------------------------------------------------------------
# A Windows-only release's version already says preview (v0.0.4-win-preview).
preview=-preview; [[ "$stamp" == *preview* ]] && preview=
for a in x86_64 i686; do
name="DLSSNR-AMD-Vulkan-Windows-$stamp$preview-$a"
dir=$pkg; [[ $a == i686 ]] && dir=$pkg32
rm -f -- "$out/$name.zip"
if command -v 7z >/dev/null; then
    ( cd "$dir" && 7z a -tzip -mm=Deflate -mx=6 -mmt="$NR_JOBS" -bd "$out/$name.zip" . > /dev/null )
else
    python3 - "$dir" "$out/$name.zip" <<'PY'
import os, sys, zipfile
root, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for d, _, files in sorted(os.walk(root)):
        for f in sorted(files):
            full = os.path.join(d, f)
            z.write(full, os.path.relpath(full, root))
PY
fi
mv -f -- "$out/$name.zip" "windows/package/$name.zip"
du -sh -- "$dir"
sha256sum -- "windows/package/$name.zip"
done
