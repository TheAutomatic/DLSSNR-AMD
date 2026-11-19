#!/usr/bin/env bash
# The Vulkan loader both routes put in the game folder: the Khronos loader with
# two patches, 64-bit.
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
# Output: artifacts/windows/vulkan-loader/x86_64/{vulkan-1.dll, LICENSE.txt, PATCHES.diff}
set -euo pipefail
cd "$(dirname "$0")/../.."
out=artifacts/windows/vulkan-loader/x86_64
if [[ "${NR_REBUILD_VK_LOADER:-0}" != 1 && -s "$out/vulkan-1.dll" && -s "$out/LICENSE.txt" && -s "$out/PATCHES.diff" ]]; then
    stale=0
    for dep in windows/build/build_vulkan_loader.sh \
               windows/vulkan-loader/patch-local-override.py \
               windows/vulkan-loader/patch-no-dxgi.py \
               windows/vulkan-loader/toolchain-x86_64.cmake; do
        [[ "$dep" -nt "$out/vulkan-1.dll" ]] && { stale=1; break; }
    done
    if [[ $stale == 0 ]]; then
        echo "reusing $out: $(ls "$out" | tr '\n' ' ')"
        exit 0
    fi
fi
src=artifacts/ref/Vulkan-Loader
tag=v1.4.357
if [[ ! -d "$src" ]]; then
    git clone -q --depth 1 --branch "$tag" https://github.com/KhronosGroup/Vulkan-Loader.git "$src"
fi
root=$PWD
# The Linux build patches the same checkout differently; start from clean sources every time.
( cd "$src" && git checkout -q -- loader && python3 "$root/windows/vulkan-loader/patch-local-override.py" && python3 "$root/windows/vulkan-loader/patch-no-dxgi.py" )
deps_args=(-DUPDATE_DEPS=ON)
if [[ -d toolchain/Vulkan-Headers ]]; then
    vh_install="$PWD/artifacts/ref/vulkan-headers-install"
    if [[ ! -f "$vh_install/share/cmake/VulkanHeaders/VulkanHeadersConfig.cmake" ]]; then
        cmake -S toolchain/Vulkan-Headers -B artifacts/ref/vulkan-headers-build -G Ninja \
            -DCMAKE_BUILD_TYPE=Release -DVULKAN_HEADERS_ENABLE_TESTS=OFF -DVULKAN_HEADERS_ENABLE_INSTALL=ON > /dev/null
        cmake --install artifacts/ref/vulkan-headers-build --prefix "$vh_install" > /dev/null
    fi
    deps_args=(-DUPDATE_DEPS=OFF -DVULKAN_HEADERS_INSTALL_DIR="$vh_install")
fi
build=$src/build-windows-x86_64
mkdir -p "$out"
cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/windows/vulkan-loader/toolchain-x86_64.cmake" \
    -DCMAKE_BUILD_TYPE=Release "${deps_args[@]}" -DBUILD_TESTS=OFF -DUSE_GAS=ON > "$build.cmake.log" 2>&1
ninja -C "$build" > "$build.ninja.log" 2>&1
cp -- "$build/loader/vulkan-1.dll" "$out/vulkan-1.dll"
cp -- "$src/LICENSE.txt" "$out/LICENSE.txt"
( cd "$src" && git diff -- loader ) > "$out/PATCHES.diff"
( cd "$src" && git checkout -q -- loader )
echo "built $out: $(ls "$out" | tr '\n' ' ')"
