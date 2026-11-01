#!/usr/bin/env bash
# nr_parity_host.exe: the reference (Vulkan) route offline, on the same model tree as the package. It links the ASI's
# own objects for the network (nr_runtime.o and friends), so the host carries the product's arch defines exactly as
# the shaders do; the test's own main replaces the ASI's entry point.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
out=artifacts/windows/d3d12_asd_test
objs=artifacts/windows/asi-official
cxx=${NR_MINGW:-x86_64-w64-mingw32-g++}
[[ -d "$objs" ]] || { echo "missing $objs (build the ASI once: windows/build/build_asi.sh ${objs})" >&2; exit 1; }
mkdir -p -- "$out"
list=()
for o in "$objs"/*.o; do
    case "$(basename -- "$o")" in asi_main.o) continue ;; esac
    list+=("$o")
done
for t in nr_parity_host nr_bridge_host; do
    "$cxx" -std=c++17 -O2 -DNDEBUG -Itoolchain/Vulkan-Headers/include -Iwindows/src -Iwindows/src/core -Iwindows/src/pe \
        "windows/tests/d3d12_asd/$t.cpp" "${list[@]}" \
        -o "$out/$t.exe" -ld3d12 -ldxgi -lole32 -lversion -static -static-libgcc -static-libstdc++
    echo "built $out/$t.exe"
done
