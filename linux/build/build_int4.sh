#!/usr/bin/env bash
# CC_SILVER_AARDWOLD_OPTIMIZED's int4 layers: the Linux-side parts, from the same tree as everything else:
#   <out>/network-<name>/      the networks with them (build_network.py <arch> --network <name>)
#   <out>/layer/               the Vulkan layer that gives the game's device VK_KHR_pipeline_binary (src/layer); what
#                              install.sh copies into dlssnr-amd/int4/layer
#   <out>/dlssnr-int4-weights  the install-time weights generator (src/tools/nr_int4_weights.cpp), fully static
#   <out>/CC_SILVER_AARDWOLD_OPTIMIZED-balanced/     Optimized layers Balanced: the network,
#                              linux/data/CC_SILVER_AARDWOLD_OPTIMIZED-balanced/<arch>/ (quantisers, recovery tables,
#                              settings.txt); what install.sh copies into dlssnr-amd/ under the same name, as the others
#   <out>/CC_SILVER_AARDWOLD_OPTIMIZED-performance/  Performance: its network and Balanced's tables
#   <out>/CC_SILVER_AARDWOLD_OPTIMIZED-ultra/        Ultra Performance: its network, Balanced's tables with
#                              linux/data/CC_SILVER_AARDWOLD_OPTIMIZED-ultra/<arch>/ added (and its settings.txt); the
#                              install-time generator makes the int4 weights of all three from these tables (theirs
#                              cover the others')
#   <out>/CC_SMALL_FP8_OPTIMIZED-ultra/              CC_SMALL_FP8_OPTIMIZED at Ultra Performance: its network and its
#                              own tables (linux/data/CC_SMALL_FP8_OPTIMIZED-ultra/<arch>/); the generator makes its int4
#                              weights (CC_SMALL_FP8_OPTIMIZED_INT4.bin) from them
#
#   build_int4.sh <out dir>
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
out=$(realpath -m -- "${1:?usage: build_int4.sh <out dir>}")
case "$out" in "$(pwd)"/*) ;; *) echo 'build directory must be in the project' >&2; exit 2;; esac
source linux/build/arch/rdna4.sh
src=linux/data/CC_SILVER_AARDWOLD_OPTIMIZED-balanced/$NR_GPU_ARCH
[[ -f "$src/settings.txt" ]] || { echo "missing $src" >&2; exit 1; }
rm -rf -- "$out"; mkdir -p -- "$out/layer" "$out/gen"

for n in CC_SILVER_AARDWOLD_OPTIMIZED-balanced CC_SILVER_AARDWOLD_OPTIMIZED-performance CC_SILVER_AARDWOLD_OPTIMIZED-ultra \
         CC_SMALL_FP8_OPTIMIZED-ultra; do
    python3 linux/build/build_network.py "$NR_GPU_ARCH" --network "$n" --out "$out/network-$n" > /dev/null
done

python3 linux/build/generate_device_chain.py toolchain/Vulkan-Headers "$out/gen/nr_device_chain_sizes.hpp" > /dev/null
g++ -std=c++17 -O2 -fPIC -shared -Wl,-Bsymbolic -Wl,-z,defs -static-libstdc++ -static-libgcc \
    -Itoolchain/Vulkan-Headers/include -Ilinux/src/core -I"$out/gen" linux/src/layer/nr_int4_layer.cpp -lpthread \
    -o "$out/layer/libVkLayer_dlssnr_int4.so"
cp -- linux/src/layer/VkLayer_dlssnr_int4.json "$out/layer/"
# NR_LAYER32=1 (the i686 package): a 32-bit build of the layer too. Proton runs a 32-bit game with a 32-bit Unix
# side (lib/wine/i386-unix), so the system Vulkan loader in that process is 32-bit; with PROTON_USE_WOW64=1 it is
# the 64-bit one. One manifest each, marked with library_arch, and the loader skips the one of the other width.
if [[ "${NR_LAYER32:-0}" == 1 ]]; then
    mkdir -p -- "$out/layer/lib32"
    g++ -m32 -std=c++17 -O2 -fPIC -shared -Wl,-Bsymbolic -Wl,-z,defs -static-libstdc++ -static-libgcc \
        -Itoolchain/Vulkan-Headers/include -Ilinux/src/core -I"$out/gen" linux/src/layer/nr_int4_layer.cpp -lpthread \
        -o "$out/layer/lib32/libVkLayer_dlssnr_int4.so"
    python3 - "$out/layer" <<'PY'
import json, sys, pathlib
d = pathlib.Path(sys.argv[1]); m = json.loads((d / "VkLayer_dlssnr_int4.json").read_text())
m["file_format_version"] = "1.2.1"
m64 = json.loads(json.dumps(m)); m64["layer"]["library_arch"] = "64"
m32 = json.loads(json.dumps(m)); m32["layer"]["library_arch"] = "32"
m32["layer"]["library_path"] = "./lib32/libVkLayer_dlssnr_int4.so"
(d / "VkLayer_dlssnr_int4.json").write_text(json.dumps(m64, indent=4) + "\n")
(d / "VkLayer_dlssnr_int4_32.json").write_text(json.dumps(m32, indent=4) + "\n")
PY
fi

# The generator stops before the GPU, so the Vulkan entry points it links are stubs that abort: no libvulkan and no
# glibc version requirement on the user's machine.
inc=(-Itoolchain/Vulkan-Headers/include -Ilinux/src -Ilinux/src/core -Ilinux/src/layer -I"$out/gen")
g++ -std=c++17 -O2 -w -DNR_INT4=1 "${inc[@]}" "${NR_PRODUCT_DEFINES[@]}" -c linux/src/tools/nr_int4_weights.cpp -o "$out/gen/gen.o"
g++ -std=c++17 -O2 -w -DNR_INT4=1 "${inc[@]}" "${NR_PRODUCT_DEFINES[@]}" -c linux/src/core/nr_native_plan.cpp -o "$out/gen/plan.o"
syms=$({ g++ -static -o /dev/null "$out/gen/gen.o" "$out/gen/plan.o" -lpthread 2>&1 || true; } |
       grep -o "undefined reference to .vk[A-Za-z0-9_]*" | sed "s/.*to .//" | sort -u)
{ echo "#include <stdlib.h>"; for s in $syms; do echo "void $s(void) { abort(); }"; done; } > "$out/gen/vkstub.c"
gcc -O2 -c "$out/gen/vkstub.c" -o "$out/gen/vkstub.o"
g++ -static -O2 "$out/gen/gen.o" "$out/gen/plan.o" "$out/gen/vkstub.o" -lpthread -o "$out/dlssnr-int4-weights"
strip "$out/dlssnr-int4-weights"

for n in CC_SILVER_AARDWOLD_OPTIMIZED-balanced CC_SILVER_AARDWOLD_OPTIMIZED-performance CC_SILVER_AARDWOLD_OPTIMIZED-ultra; do
    e="$out/$n"
    mkdir -p -- "$e/shaders"
    cp -- "$out/network-$n"/g_* "$out/network-$n"/*.txt "$e/shaders/"   # the SPVs and the host markers (.iu4, hs-pipelines.txt)
    cp -r -- "$out/network-$n/temporal" "$e/shaders/temporal"
    cp -r -- "$src"/. "$e/"
    rm -f -- "$e/CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin.sha256"
    [[ $n == *-ultra ]] && cp -r -- "linux/data/CC_SILVER_AARDWOLD_OPTIMIZED-ultra/$NR_GPU_ARCH"/. "$e/"   # its tables and settings.txt
done
e="$out/CC_SMALL_FP8_OPTIMIZED-ultra"
mkdir -p -- "$e/shaders"
cp -- "$out/network-CC_SMALL_FP8_OPTIMIZED-ultra"/g_* "$out/network-CC_SMALL_FP8_OPTIMIZED-ultra"/*.txt "$e/shaders/"
cp -r -- "$out/network-CC_SMALL_FP8_OPTIMIZED-ultra/temporal" "$e/shaders/temporal"
cp -r -- "linux/data/CC_SMALL_FP8_OPTIMIZED-ultra/$NR_GPU_ARCH"/. "$e/"
rm -f -- "$e/CC_SMALL_FP8_OPTIMIZED_INT4.bin.sha256" "$e/CC_SMALL_FP8_OPTIMIZED_INT4_SINK.bin.sha256"
echo "$out: int4 networks$(for n in CC_SILVER_AARDWOLD_OPTIMIZED-balanced CC_SILVER_AARDWOLD_OPTIMIZED-performance CC_SILVER_AARDWOLD_OPTIMIZED-ultra CC_SMALL_FP8_OPTIMIZED-ultra; do echo -n " $n $(ls "$out/$n/shaders"/g_*.spv | wc -l)"; done) pipelines"
