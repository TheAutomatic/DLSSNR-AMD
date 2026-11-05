#!/usr/bin/env bash
# Make the model files from NVIDIA's nvngx_dlssnr.dll, written into <output folder>:
#   310.9.16: CC_SILVER_AARDWOLD.bin (the old model, the DLL's preset 1) and CC_SMALL_FP8.bin (the new model, the DLL's default)
#   310.8.0:  CC_SILVER_AARDWOLD.bin only
#
#   bash extract_model.sh <nvngx_dlssnr.dll or a .zip containing it> <output folder>
#
# Only reads the weight data in the DLL; never loads or runs it. Every entry is checked against
# model-files.sha256 / model-files-small.sha256, and a file is written only if it matches the tested
# model byte for byte.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
src=${1:?usage: extract_model.sh <nvngx_dlssnr.dll or .zip> <output folder>}
out=${2:?usage: extract_model.sh <nvngx_dlssnr.dll or .zip> <output folder>}
[[ -f "$src" ]] || { echo "no such file: $src" >&2; exit 1; }
[[ -d "$out" ]] || { echo "no such output folder: $out" >&2; exit 1; }
out=$(cd -- "$out" && pwd)
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 1; }

sha_3108=e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e
sha_3109=2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

# <resource> <descriptor> <post-block record> <file list> <checksums> <output .bin>
one() {
    local w="$work/$1"
    mkdir -p -- "$w/graph"
    cp -- "$here/$2" "$w/graph/descriptor.json"
    if ! python3 "$here/inspect_nr.py" "$src" --output "$w/inventory" --extract --resource "$1" > "$w/inspect.json" 2> "$w/inspect.err"; then
        echo "Cannot read nvngx_dlssnr weights from $src (needs nvngx_dlssnr.dll version 310.9.16 or 310.8.0, or its zip)." >&2
        exit 1
    fi
    (cd -- "$here"
     python3 unpack_swin_family.py --artifacts "$w" --output "$w/unpacked" --max-c 256 > /dev/null
     python3 unpack_splitswin.py --artifacts "$w" --output "$w/unpacked-splitswin" > /dev/null
     python3 unpack_vit.py --artifacts "$w" --output "$w/unpacked-vit" > /dev/null
     python3 unpack_preblock.py "$w/inventory/weights/block0.layer0.layer.bin" "$w/unpacked-preblock" > /dev/null
     python3 unpack_postblock.py "$w/inventory/weights/$3.bin" "$w/unpacked-postblock" > /dev/null
     python3 pack_model.py --root "$w" --list "$4" --out "$6" --verify "$5")
}

if ! python3 "$here/inspect_nr.py" "$src" --output "$work/probe" > "$work/probe.json" 2> "$work/probe.err"; then
    echo "Cannot read nvngx_dlssnr weights from $src (needs nvngx_dlssnr.dll version 310.9.16 or 310.8.0, or its zip)." >&2
    exit 1
fi
got=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["dll_sha256"])' "$work/probe.json")
case "$got" in
    "$sha_3109")
        one WEIGHTS_HT descriptor.json block70.layer0.layer model-files.txt model-files.sha256 "$out/CC_SILVER_AARDWOLD.bin"
        one WEIGHTS_CC_SMALL_FP8 descriptor-small.json block30.layer0.layer model-files-small.txt model-files-small.sha256 "$out/CC_SMALL_FP8.bin";;
    "$sha_3108")
        one WEIGHTS_HT descriptor.json block70.layer0.layer model-files.txt model-files.sha256 "$out/CC_SILVER_AARDWOLD.bin";;
    *)
        echo "This is not nvngx_dlssnr 310.9.16 or 310.8.0 (SHA256 $got). One of these two DLL versions is required." >&2
        exit 1;;
esac
