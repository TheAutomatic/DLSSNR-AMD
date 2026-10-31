#!/usr/bin/env bash
# dlssnr-amd.asi: NR as a plugin of upstream OptiScaler (windows/src/pe/nr_asi_main.cpp), cross-compiled to PE, x86_64
# only (OptiScaler's builds are). D3D10/11/12 games keep the system's D3D: the network runs on a Vulkan device of its
# own and the frame crosses over through shared textures and a shared fence (windows/src/pe/nr_pe_bridge.hpp);
# Vulkan games: on the game's own device.
#
#   build_asi.sh [build dir]     (default artifacts/windows/asi)
# NR_INT4=1 (the default: the int4 mixed network installed beside the others) or 0.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."

out=$(realpath -m -- "${1:-artifacts/windows/asi}")
case "$out" in "$(pwd)"/*) ;; *) echo 'build directory must be in the project' >&2; exit 2;; esac
mkdir -p -- "$out"

int4=${NR_INT4:-1}
cxx=${NR_MINGW:-x86_64-w64-mingw32-g++}
cc=${NR_MINGW_CC:-x86_64-w64-mingw32-gcc}
objdump=${NR_OBJDUMP:-x86_64-w64-mingw32-objdump}
command -v "$cxx" >/dev/null || { echo "no mingw cross compiler ($cxx)" >&2; exit 1; }
minhook=artifacts/ref/DLSS5-Feeder/external/minhook
[[ -d "$minhook" ]] || { echo "missing reference headers: $minhook" >&2; exit 1; }
[[ -d toolchain/Vulkan-Headers/include ]] || { echo "missing toolchain/Vulkan-Headers" >&2; exit 1; }

stamp=${NR_STAMP:-$(bash windows/build/version.sh)}
# pNext sizes for the device watcher, which adds the network's features to the game's Vulkan device.
python3 windows/build/generate_device_chain.py toolchain/Vulkan-Headers "$out/nr_device_chain_sizes.hpp" > /dev/null
common=(-std=c++17 -O2 -DNDEBUG -DNR_BUILD_STAMP="\"$stamp\"" -Itoolchain/Vulkan-Headers/include -Iwindows/src
        -Iwindows/src/core -Iwindows/src/pe -I"$out")
[[ "$int4" == 1 ]] && common+=(-DNR_INT4=1)

# The network the package ships (windows/build/arch/rdna4.sh): the host must be built with the same constants as the
# shaders in dlssnr-amd/shaders.
source windows/build/arch/rdna4.sh
"$cxx" "${common[@]}" "${NR_PRODUCT_DEFINES[@]}" -c windows/src/core/nr_runtime.cpp -o "$out/nr_runtime.o"
"$cxx" "${common[@]}" -c windows/src/core/nr_native_plan.cpp -o "$out/nr_native_plan.o"
"$cxx" "${common[@]}" -c windows/src/core/nr_renodx.cpp -o "$out/nr_renodx.o"
"$cxx" "${common[@]}" -c windows/src/core/nr_wilsjo2.cpp -o "$out/nr_wilsjo2.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_pe_log.cpp -o "$out/log.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_pe_crash.cpp -o "$out/crash.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_pe_interop.cpp -o "$out/interop.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_vram_ledger.cpp -o "$out/vram_ledger.o"
"$cxx" "${common[@]}" -I"$minhook/include" -c windows/src/pe/nr_pe_vkdevice.cpp -o "$out/vkdevice.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_pe_session.cpp -o "$out/session.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_pe_bridge.cpp -o "$out/bridge.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_pe_d3d12split.cpp -o "$out/d3d12split.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_pe_config.cpp -o "$out/config.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_dlssnr_model.cpp -o "$out/model.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_pe_renodx_route.cpp -o "$out/renodx_route.o"
"$cxx" "${common[@]}" -I"$minhook/include" -c windows/src/pe/nr_pe_control.cpp -o "$out/control.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_screenshot.cpp -o "$out/screenshot.o"
"$cxx" "${common[@]}" -I"$minhook/include" -c windows/src/pe/nr_asi_main.cpp -o "$out/asi_main.o"
# The wilsjo2 style (nr_asi_wilsjo2.hpp): its flow and its programs; the shaders go to dlssnr-amd\shaders\wilsjo2.
"$cxx" "${common[@]}" -c windows/src/pe/nr_asi_wilsjo2.cpp -o "$out/asi_wilsjo2.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_asi_wilsjo2_vk.cpp -o "$out/asi_wilsjo2_vk.o"
# HUD FIX's capture on D3D12 (nr_asi_hudcap.hpp): hooks of the game's command lists
"$cxx" "${common[@]}" -I"$minhook/include" -c windows/src/pe/nr_asi_hudcap.cpp -o "$out/asi_hudcap.o"
bash windows/build/build_wilsjo2.sh "$out/shaders/wilsjo2" > /dev/null
# NR Control (End), drawn by the DLL over the game's swap chain: ImGui with its D3D11, D3D12 and Vulkan backends.
imgui=${NR_IMGUI_ROOT:-toolchain/imgui-nr}
[[ -f "$imgui/imgui_impl_dx12.cpp" ]] || { echo "missing $imgui (ImGui with its D3D backends)" >&2; exit 1; }
# NR Control's font: Inter (SIL Open Font License 1.1, toolchain/fonts/inter-4.1/LICENSE.txt), embedded
font=${NR_MENU_FONT:-toolchain/fonts/inter-4.1/Inter-Regular.ttf}
[[ -f "$font" ]] || { echo "missing $font (NR Control's font)" >&2; exit 1; }
python3 windows/build/embed_font.py "$font" "$out/nr_menu_font.hpp" kMenuFont
glslang=${NR_GLSLANG:-toolchain/glslang/bin/glslang}
"$glslang" -V --target-env vulkan1.0 --vn kMenuComposeVert windows/shaders/control/menu_compose.vert -o "$out/menu_compose_vert.h" > /dev/null
"$glslang" -V --target-env vulkan1.0 --vn kMenuComposeFrag windows/shaders/control/menu_compose.frag -o "$out/menu_compose_frag.h" > /dev/null
"$cxx" "${common[@]}" -I"$minhook/include" -I"$imgui" -Iwindows/src/ui -c windows/src/pe/nr_pe_menu.cpp -o "$out/menu.o"
# the native D3D12 ASD backend (M5; off unless dlssnr-amd-asd.ini says Enabled = 1)
"$cxx" "${common[@]}" -c windows/src/pe/nr_d3d12_asd.cpp -o "$out/d3d12_asd.o"
"$cxx" "${common[@]}" -c windows/src/pe/nr_d3d12_asd_backend.cpp -o "$out/d3d12_asd_backend.o"
"$cxx" "${common[@]}" -I"$imgui" -Iwindows/src/ui -c windows/src/pe/nr_d3d12_asd_panel.cpp -o "$out/d3d12_asd_panel.o"
for unit in imgui imgui_draw imgui_tables imgui_widgets imgui_impl_dx11 imgui_impl_vulkan; do
    "$cxx" -std=c++17 -O2 -DNDEBUG -Itoolchain/Vulkan-Headers/include -I"$imgui" -c "$imgui/$unit.cpp" -o "$out/im_$unit.o"
done
"$cxx" -std=c++17 -O2 -DNDEBUG -I"$imgui" -include windows/src/pe/mingw_d3d12_compat.h -c "$imgui/imgui_impl_dx12.cpp" \
    -o "$out/im_imgui_impl_dx12.o"
for unit in hook buffer trampoline; do
    "$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/$unit.c" -o "$out/mh_$unit.o"
done
"$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/hde/hde64.c" -o "$out/mh_hde.o"
# nr_present_probe.exe (dlssnr-amd\): finds DXGI's swap-chain methods for NR Control in a process of its own.
"$cxx" -std=c++17 -O2 -DNDEBUG -municode windows/src/pe/nr_present_probe.cpp -o "$out/nr_present_probe.exe" \
    -ldxgi -static -static-libgcc -static-libstdc++

objs=("$out/asi_main.o" "$out/d3d12_asd.o" "$out/d3d12_asd_backend.o" "$out/d3d12_asd_panel.o" "$out/asi_wilsjo2.o" "$out/asi_wilsjo2_vk.o" "$out/nr_wilsjo2.o" "$out/asi_hudcap.o" "$out/menu.o"
      "$out"/im_*.o "$out/model.o" "$out/session.o" "$out/bridge.o" "$out/d3d12split.o" "$out/config.o" "$out/interop.o"
      "$out/vram_ledger.o" "$out/log.o" "$out/crash.o" "$out/vkdevice.o" "$out/nr_runtime.o" "$out/nr_native_plan.o"
      "$out/nr_renodx.o" "$out/renodx_route.o" "$out/control.o" "$out/screenshot.o" "$out"/mh_*.o)
ldflags=(-ld3d12 -ld3d11 -ldxgi -lole32 -lversion -static -static-libgcc -static-libstdc++)

# Vulkan is resolved at run time rather than imported (windows/build/vulkan_lazy.sh).
count=$(bash windows/build/vulkan_lazy.sh "$out" "$cxx" -shared "${objs[@]}" "${ldflags[@]}" | grep -o '[0-9]*$')
"$cxx" -shared -o "$out/dlssnr-amd.asi" "${objs[@]}" "$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o" "${ldflags[@]}"

echo "built $out/dlssnr-amd.asi ($(stat -c %s "$out/dlssnr-amd.asi") bytes, $count Vulkan entry points at run time), $out/nr_present_probe.exe and $out/shaders/wilsjo2"
echo "exports: $("$objdump" -p "$out/dlssnr-amd.asi" | grep -c 'InitializeASI')  imports: $("$objdump" -p "$out/dlssnr-amd.asi" | grep 'DLL Name' | awk '{print $3}' | tr '\n' ' ')"
