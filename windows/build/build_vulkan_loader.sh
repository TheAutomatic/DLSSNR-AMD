#!/usr/bin/env bash
# The Vulkan loader both routes put in the game folder: the Khronos loader with
# two patches, x86-64 or i686 (NR_ARCH).
#
# Why a loader at all: in the game folder the game runs on DXVK / vkd3d-proton,
# i.e. on Vulkan, and ReShade's Vulkan backend is a Vulkan layer. A layer is
# normally registered in HKLM for every Vulkan program on the machine; this
# loader instead reads layer manifests from vk-override\implicit_layer\ beside
# itself (windows/vulkan-loader/patch-local-override.py), so the install stays
# inside the game folder. DXVK and vkd3d-proton load vulkan-1.dll by name, so
# the copy in the game folder is the one they get. Drivers are found the normal
# way (the package ships no vk-override\nr-icd.json).
# patch-no-dxgi.py: the stock loader calls CreateDXGIFactory1 while creating an
# instance; with OptiScaler as dxgi.dll that reaches DXVK's dxgi, which creates
# an instance through this loader again (the likely cause of 7 Days to Die
# hanging at start in the first Windows package test).
#
# Output: artifacts/windows/vulkan-loader/<x86_64|i686>/{vulkan-1.dll, LICENSE.txt, PATCHES.diff}
set -euo pipefail
cd "$(dirname "$0")/../.."
arch=${NR_ARCH:-x86_64}
case "$arch" in x86_64|i686) ;; *) echo 'NR_ARCH must be x86_64 or i686' >&2; exit 2;; esac
out=artifacts/windows/vulkan-loader/$arch
src=artifacts/ref/Vulkan-Loader
tag=v1.4.357
if [[ ! -d "$src" ]]; then
    git clone -q --depth 1 --branch "$tag" https://github.com/KhronosGroup/Vulkan-Loader.git "$src"
fi
root=$PWD
# The Linux build patches the same checkout differently; start from clean sources every time.
( cd "$src" && git checkout -q -- loader && python3 "$root/windows/vulkan-loader/patch-local-override.py" && python3 "$root/windows/vulkan-loader/patch-no-dxgi.py" )
build=$src/build-windows-$arch
mkdir -p "$out"
cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/windows/vulkan-loader/toolchain-$arch.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DUPDATE_DEPS=ON -DBUILD_TESTS=OFF -DUSE_GAS=ON > "$build.cmake.log" 2>&1
ninja -C "$build" > "$build.ninja.log" 2>&1
cp -- "$build/loader/vulkan-1.dll" "$out/vulkan-1.dll"
cp -- "$src/LICENSE.txt" "$out/LICENSE.txt"
( cd "$src" && git diff -- loader ) > "$out/PATCHES.diff"
( cd "$src" && git checkout -q -- loader )
echo "built $out: $(ls "$out" | tr '\n' ' ')"
