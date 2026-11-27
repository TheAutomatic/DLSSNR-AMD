#!/usr/bin/env bash
# Build dlssnr_extract_model: the Windows .exe (static, mingw-w64) and a native
# Linux binary for testing.
#
#   bash windows/package/model-tools/build_extract_model.sh <output dir> [descriptor-CC_SILVER_AARDWOLD.json]
#
# Run from anywhere in the repository. dlssnr_extract_tables_cc_silver_aardwold.hpp and dlssnr_extract_tables_cc_small_fp8.hpp
# (next to this script) are regenerated first from linux/package/model-tools/: descriptor-CC_SILVER_AARDWOLD.json (or
# the one given) with model-files-CC_SILVER_AARDWOLD.{txt,sha256}, and descriptor-CC_SMALL_FP8.json with
# model-files-CC_SMALL_FP8.{txt,sha256}.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/../../.." && pwd)
out=${1:?usage: build_extract_model.sh <output dir> [descriptor-CC_SILVER_AARDWOLD.json]}
desc=${2:-$root/linux/package/model-tools/descriptor-CC_SILVER_AARDWOLD.json}
mkdir -p -- "$out"
out=$(cd -- "$out" && pwd)

python3 "$here/gen_extract_tables.py" --descriptor "$desc" \
    --list "$root/linux/package/model-tools/model-files-CC_SILVER_AARDWOLD.txt" --sums "$root/linux/package/model-tools/model-files-CC_SILVER_AARDWOLD.sha256" \
    --out "$here/dlssnr_extract_tables_cc_silver_aardwold.hpp"
python3 "$here/gen_extract_tables.py" --descriptor "$root/linux/package/model-tools/descriptor-CC_SMALL_FP8.json" \
    --list "$root/linux/package/model-tools/model-files-CC_SMALL_FP8.txt" --sums "$root/linux/package/model-tools/model-files-CC_SMALL_FP8.sha256" \
    --namespace small_fp8_tables --shared-types --out "$here/dlssnr_extract_tables_cc_small_fp8.hpp"

flags=(-std=c++17 -O2 -Wall -Wextra)
g++ "${flags[@]}" -o "$out/dlssnr_extract_model" "$here/dlssnr_extract_model.cpp"
x86_64-w64-mingw32-g++ "${flags[@]}" -municode -static -static-libgcc -static-libstdc++ \
    -o "$out/dlssnr_extract_model.exe" "$here/dlssnr_extract_model.cpp"
x86_64-w64-mingw32-strip "$out/dlssnr_extract_model.exe"
ls -l -- "$out/dlssnr_extract_model" "$out/dlssnr_extract_model.exe"
