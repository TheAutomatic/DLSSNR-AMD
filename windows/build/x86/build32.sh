#!/usr/bin/env bash
# Build dlssnr_amd.addon32, the Windows add-on compiled for 32-bit (PE32 i386) games.
#
#   windows/build/x86/build32.sh <minhook> [out dir]
#
# <minhook> is a MinHook checkout with include/ and src/ (32-bit builds use hde32.c, not hde64.c).
# The add-on is built from the tree this script lives in; nothing is copied over it.
#
# Toolchain: MinGW 13 in the dlssnr-amd-build Docker image.
# The Vulkan entry points are not statically imported: vulkan_lazy32.c/.s stand in for vulkan-1.dll
# (one tail jump per entry point, the same idea as windows/build/vulkan_lazy.sh does for x86-64).
set -euo pipefail
MINHOOK=${1:?path to minhook (with include/ and src/)}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../../.." && pwd)
MINHOOK=$(realpath -- "$MINHOOK")
OUT=$(realpath -m -- "${2:-$PWD/out}")

command -v i686-w64-mingw32-g++-posix >/dev/null ||
    { echo 'missing i686-w64-mingw32-g++-posix (apt install g++-mingw-w64-i686-posix)'; exit 1; }

mkdir -p "$OUT"
cd "$SRC"
source windows/build/arch/rdna4.sh          # NR_PRODUCT_DEFINES (the network builds of this release)
source "$SRC/windows/build/parallel.sh"

# Windows 10 declarations are needed by the shared host code.
GXX="$HERE/tc/gxx32"
GCC="$HERE/tc/gcc32"

INC=(-I"$HERE" -I"$SRC/toolchain/Vulkan-Headers/include" -I"$SRC/windows/src" -I"$SRC/windows/src/core"
     -I"$SRC/windows/src/pe" -I"$SRC/windows/src/ui" -I"$MINHOOK/include")
INC_ADDON=(-I"$SRC/artifacts/ref/DLSS5-Feeder/external/imgui" -I"$MINHOOK/include"
           -I"$SRC/artifacts/ref/DLSS5-Feeder/external/reshade/include")
stamp=$(bash windows/build/version.sh)
DEF=(-std=c++17 -O2 -DNDEBUG -DNR_BUILD_STAMP="\"$stamp\"" -DNR_RESHADE_ROUTE=1 -DNR_INT4=1)

mkdir -p "$OUT/shim"; printf '#include <windows.h>\n' > "$OUT/shim/Windows.h"   # reshade.hpp asks for Windows.h

cd "$OUT"
objs=()
nr_par_init
# Launch heaviest units first so they run concurrently with smaller files.
nr_par_run "$GXX" "${DEF[@]}" "${INC[@]}" "${NR_PRODUCT_DEFINES[@]}" -c "$SRC/windows/src/core/nr_runtime.cpp" -o nr_runtime.o
nr_par_run "$GXX" "${DEF[@]}" -I. -I"$OUT/shim" "${INC[@]}" "${INC_ADDON[@]}" "${NR_PRODUCT_DEFINES[@]}" \
       -c "$SRC/windows/src/pe/nr_reshade_addon.cpp" -o reshade_addon.o
for f in nr_native_plan nr_pe_bridge nr_pe_config nr_pe_crash nr_pe_d3d12split \
         nr_pe_interop nr_pe_log nr_pe_session nr_pe_vkdevice nr_renodx nr_runtime nr_screenshot \
         nr_vram_ledger nr_wilsjo2; do
    objs+=("$f.o")
    [[ "$f" == nr_runtime ]] && continue
    p=$(find "$SRC/windows/src" -name "$f.cpp" | head -1)
    nr_par_run "$GXX" "${DEF[@]}" "${INC[@]}" "${NR_PRODUCT_DEFINES[@]}" -c "$p" -o "$f.o"
done
objs+=(reshade_addon.o mh_hook.o mh_buffer.o mh_trampoline.o mh_hde32.o vulkan_lazy_c32.o vulkan_lazy_s32.o)

# MinHook, 32-bit (hde32 only)
nr_par_run "$GCC" -O2 -I"$MINHOOK/include" -I"$MINHOOK/src" -c "$MINHOOK/src/hook.c"       -o mh_hook.o
nr_par_run "$GCC" -O2 -I"$MINHOOK/include" -I"$MINHOOK/src" -c "$MINHOOK/src/buffer.c"     -o mh_buffer.o
nr_par_run "$GCC" -O2 -I"$MINHOOK/include" -I"$MINHOOK/src" -c "$MINHOOK/src/trampoline.c" -o mh_trampoline.o
nr_par_run "$GCC" -O2 -I"$MINHOOK/include" -I"$MINHOOK/src" -c "$MINHOOK/src/hde/hde32.c"  -o mh_hde32.o

nr_par_run "$GCC" -O2 -c "$HERE/vulkan_lazy32.c" -o vulkan_lazy_c32.o
nr_par_run "$GCC" -O2 -c "$HERE/vulkan_lazy32.s" -o vulkan_lazy_s32.o
nr_par_wait

"$GXX" -shared -static -static-libgcc -static-libstdc++ "${objs[@]}" -o "$OUT/dlssnr_amd.addon32" \
       -ld3d12 -ld3d11 -ldxgi -ldwmapi -lgdi32 -lole32      # no -lvulkan-1: the lazy stubs stand in

ls -l "$OUT/dlssnr_amd.addon32"
i686-w64-mingw32-objdump -p "$OUT/dlssnr_amd.addon32" | grep -E 'file format|DLL Name'
