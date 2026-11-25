#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."

out=${1:-artifacts/windows/pal-gfx103-test}
mkdir -p -- "$out"

cxx=${NR_HOST_CXX:-g++}
"$cxx" -std=c++17 -O2 -Wall -Wextra -Werror \
    -Iwindows/src/core \
    windows/tools/test_pal_gfx103.cpp -o "$out/test_pal_gfx103"
"$out/test_pal_gfx103" windows/data/aco/rdna2/records
