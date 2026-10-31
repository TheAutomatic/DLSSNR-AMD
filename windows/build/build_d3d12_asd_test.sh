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
flags=(-std=c++17 -O2 -Wall -Iwindows/src -Iwindows/src/core -Iwindows/src/pe)
srcs=(windows/tests/d3d12_asd/nr_d3d12_asd_test.cpp windows/src/pe/nr_d3d12_asd.cpp windows/src/pe/nr_d3d12_asd_backend.cpp windows/src/pe/nr_d3d12_asd_reprofile.cpp)
libs=(-ld3d12 -lole32 -lshell32 -static -static-libgcc -static-libstdc++)
"$cxx" "${flags[@]}" "${srcs[@]}" -o "$out/nr_d3d12_asd_test_inbox.exe" "${libs[@]}"
"$cxx" "${flags[@]}" -DNR_TEST_AGILITY=619 "${srcs[@]}" -o "$out/nr_d3d12_asd_test_619.exe" "${libs[@]}"
"$cxx" "${flags[@]}" -DNR_TEST_AGILITY=615 "${srcs[@]}" -o "$out/nr_d3d12_asd_test_615.exe" "${libs[@]}"
ls -la "$out"/*.exe
