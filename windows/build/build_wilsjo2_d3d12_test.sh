#!/usr/bin/env bash
# windows/tests/wilsjo2_d3d12: the wilsjo2 stage on the native D3D12 route outside a game (nr_wilsjo2_d3d12_test.cpp).
#   build_wilsjo2_d3d12_test.sh [build dir]   (default artifacts/windows/wilsjo2_d3d12_test)
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
out=$(realpath -m -- "${1:-artifacts/windows/wilsjo2_d3d12_test}")
mkdir -p -- "$out"
cxx=${NR_MINGW:-x86_64-w64-mingw32-g++}
flags=(-std=c++17 -O2 -Wall -Iwindows/src -Iwindows/src/core -Iwindows/src/pe -Itoolchain/Vulkan-Headers/include)
"$cxx" "${flags[@]}" -c windows/tests/wilsjo2_d3d12/nr_wilsjo2_d3d12_test.cpp -o "$out/test.o"
"$cxx" "${flags[@]}" -c windows/src/core/nr_wilsjo2.cpp -o "$out/nr_wilsjo2.o"
"$cxx" "${flags[@]}" -c windows/src/pe/nr_pe_log.cpp -o "$out/log.o"
objs=("$out/test.o" "$out/nr_wilsjo2.o" "$out/log.o")
libs=(-ld3d12 -lole32 -lshell32 -static -static-libgcc -static-libstdc++)
# Vulkan (the stage's own Vulkan half, unused here) resolved at run time, as in the .asi (vulkan_lazy.sh)
bash windows/build/vulkan_lazy.sh "$out" "$cxx" "${objs[@]}" "${libs[@]}" > /dev/null
"$cxx" -o "$out/nr_wilsjo2_d3d12_test.exe" "${objs[@]}" "$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o" "${libs[@]}"
ls -la "$out"/*.exe
