#!/usr/bin/env bash
# windows/tests/d3d12_asd: the D3D12 ASD backend outside a game (nr_d3d12_asd_test.cpp).
#   build_d3d12_asd_test.sh [build dir]   (default artifacts/windows/d3d12_asd_test)
# Three executables: _inbox (the system's D3D12), _619 and _615 (an Agility SDK D3D12Core.dll of that version in
# <exe dir>\D3D12\, which this script does not provide).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
out=$(realpath -m -- "${1:-artifacts/windows/d3d12_asd_test}")
mkdir -p -- "$out"
cxx=${NR_MINGW:-x86_64-w64-mingw32-g++}
flags=(-std=c++17 -O2 -Wall -Iwindows/src -Iwindows/src/core -Iwindows/src/pe -Itoolchain/Vulkan-Headers/include)
srcs=(windows/tests/d3d12_asd/nr_d3d12_asd_test.cpp windows/src/pe/nr_d3d12_asd.cpp windows/src/pe/nr_d3d12_asd_backend.cpp windows/src/pe/nr_renodx_d3d12.cpp windows/src/pe/nr_d3d12_asd_net.cpp windows/src/pe/nr_d3d12_capture.cpp windows/src/pe/nr_d3d12_asd_reprofile.cpp windows/src/core/nr_renodx.cpp windows/src/core/nr_runtime.cpp windows/src/core/nr_native_plan.cpp windows/src/pe/nr_pe_config.cpp)
libs=(-ld3d12 -ldxgi -lole32 -lshell32 -lversion -static -static-libgcc -static-libstdc++)
# nr_renodx.cpp (the RenoDX stage's host logic, shared with the D3D12 one) imports Vulkan: resolved at run time,
# as in the .asi (vulkan_lazy.sh).
count=$(bash windows/build/vulkan_lazy.sh "$out" "$cxx" "${flags[@]}" "${srcs[@]}" "${libs[@]}" | grep -o '[0-9]*$' || echo 0)
"$cxx" "${flags[@]}" "${srcs[@]}" -o "$out/nr_d3d12_asd_test_inbox.exe" "$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o" "${libs[@]}"
"$cxx" "${flags[@]}" -DNR_TEST_AGILITY=619 "${srcs[@]}" -o "$out/nr_d3d12_asd_test_619.exe" "$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o" "${libs[@]}"
"$cxx" "${flags[@]}" -DNR_TEST_AGILITY=615 "${srcs[@]}" -o "$out/nr_d3d12_asd_test_615.exe" "$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o" "${libs[@]}"
ls -la "$out"/*.exe
