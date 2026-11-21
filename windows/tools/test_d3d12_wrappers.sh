#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."

build_only=0
if [[ ${1:-} == --build-only ]]; then build_only=1; shift; fi
out=${1:-artifacts/windows/wrapper-test}
source_root=${NR_TEST_SOURCE_ROOT:-.}
mkdir -p -- "$out"
"${NR_MINGW:-x86_64-w64-mingw32-g++}" -std=c++17 -O2 -static -static-libgcc -static-libstdc++ \
    -I"$source_root/windows/src/pe" -I"$source_root/windows/src/core" \
    windows/tools/test_d3d12_wrappers.cpp "$source_root/windows/src/pe/nr_pe_d3d12split.cpp" \
    -o "$out/test_d3d12_wrappers.exe" -ld3d12 -ldxgi -lole32 -luuid
echo "built $out/test_d3d12_wrappers.exe"
if [[ $build_only == 0 ]]; then
    "${NR_WINE:-wine}" "$out/test_d3d12_wrappers.exe" | tee "$out/test_d3d12_wrappers.log"
    [[ $(<"$out/test_d3d12_wrappers.log") == *"wrapper regression: 0 failures"* ]]
fi
