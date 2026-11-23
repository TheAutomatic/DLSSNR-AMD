#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."

out=${1:-artifacts/windows/device-features-test}
mkdir -p -- "$out"

cxx=${NR_HOST_CXX:-g++}
vk_headers=${NR_VULKAN_HEADERS:-toolchain/Vulkan-Headers}
vk_inc=${NR_VULKAN_INCLUDE:-$vk_headers/include}
[[ -f "$vk_inc/vulkan/vulkan.h" ]] || { echo "missing vulkan headers under $vk_inc" >&2; exit 1; }

if [[ -n "${NR_CHAIN_SIZES_DIR:-}" && -f "$NR_CHAIN_SIZES_DIR/nr_device_chain_sizes.hpp" ]]; then
    chain_inc=$NR_CHAIN_SIZES_DIR
else
    chain_inc=$out
    python3 windows/build/generate_device_chain.py "$vk_headers" "$chain_inc/nr_device_chain_sizes.hpp" > /dev/null
fi

"$cxx" -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -Werror \
    -I"$vk_inc" -Iwindows/src/core -I"$chain_inc" \
    windows/tools/test_device_features.cpp -o "$out/test_device_features"
"$out/test_device_features"
