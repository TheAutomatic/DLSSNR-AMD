#!/usr/bin/env bash
# Download the third-party pieces the build uses into toolchain/ and artifacts/ref/ (neither is in git), each pinned to
# the exact version the packages were built and tested with - except the Windows packages' DXVK, taken from the latest
# GE-Proton release (NR_GE_PROTON=<tag> pins one).
#
#   bash fetch_deps.sh             what the Linux package needs
#   bash fetch_deps.sh --windows   plus what the Windows package needs (that DXVK and its licence)
#
# Already present entries are kept. Needs git, curl, tar and python3.
#
# Nothing from NVIDIA is downloaded: the model comes from your own nvngx_dlssnr.dll
# (linux/package/model-tools/extract_model.sh).
set -euo pipefail
cd -- "$(dirname -- "$0")"
windows=0
for a in "$@"; do
    case "$a" in --windows) windows=1;; *) echo "usage: fetch_deps.sh [--windows]" >&2; exit 2;; esac
done
mkdir -p toolchain artifacts/ref/downloads

# A git tree at one commit, without the history.
repo() {  # <dir> <url> <commit>
    local dir=$1 url=$2 commit=$3
    [[ -d "$dir" ]] && { echo "have $dir"; return; }
    echo "fetch $dir"
    git init -q "$dir"
    git -C "$dir" fetch -q --depth 1 "$url" "$commit"
    git -C "$dir" checkout -q FETCH_HEAD
}

# A file, checked against its SHA-256.
get() {  # <file> <url> <sha256>
    local file=$1 url=$2 sum=$3
    if [[ ! -f "$file" ]]; then
        echo "fetch $file"
        curl -fsSL --retry 3 -o "$file.part" "$url"
        mv -- "$file.part" "$file"
    fi
    echo "$sum  $file" | sha256sum -c --quiet - || { echo "checksum mismatch: $file" >&2; exit 1; }
}

# ---- build tools ------------------------------------------------------------------------------
repo toolchain/Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers.git \
     e3b1eec08173d6b825cd3ac88c885a63b621504a                                      # v1.4.357
# The shaders are only byte-identical to the tested ones with this exact glslang.
get artifacts/ref/downloads/glslang-16.5.0-linux-x86_64-release.tar.gz \
    https://github.com/KhronosGroup/glslang/releases/download/16.5.0/glslang-16.5.0-linux-x86_64-release.tar.gz \
    b9b1f96acb898a62251b171f7695efcecfc206a530299054071919b06820f657
if [[ ! -x toolchain/glslang/bin/glslang ]]; then
    mkdir -p toolchain/glslang
    tar -C toolchain/glslang -xzf artifacts/ref/downloads/glslang-16.5.0-linux-x86_64-release.tar.gz
fi

# ---- sources and headers ----------------------------------------------------------------------
# Slang 2026.19 compiles the RenoDX-style option's shaders (linux/shaders/renodx, RenoDX's own HLSL); with this
# exact release they are the same SPIR-V bytes as the tested ones.
get artifacts/ref/downloads/slang-2026.19-linux-x86_64-glibc-2.28.tar.gz \
    https://github.com/shader-slang/slang/releases/download/v2026.19/slang-2026.19-linux-x86_64-glibc-2.28.tar.gz \
    bd7afcb0956dee9304f44ed115e9aaa7140704271b5c3294bcb237e3ae67adc7
if [[ ! -x toolchain/slang-2026.19/bin/slangc ]]; then
    mkdir -p toolchain/slang-2026.19
    tar -C toolchain/slang-2026.19 -xzf artifacts/ref/downloads/slang-2026.19-linux-x86_64-glibc-2.28.tar.gz
fi

# ---- sources ----------------------------------------------------------------------------------
# Dear ImGui (MIT) as OptiScaler-NR's source tree carries it (v0.8.91 source, commit f45ccf3), for NR Control (the
# OptiScaler route's NR menu, drawn by dlssnr-amd.asi): the files it compiles, each downloaded from that commit and
# checked, into toolchain/imgui-nr, with this project's imconfig.h.
if [[ ! -f toolchain/imgui-nr/imgui_impl_dx12.cpp ]]; then
    echo "fetch toolchain/imgui-nr"
    mkdir -p toolchain/imgui-nr
    for f in LICENSE.txt imgui.cpp imgui.h imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp imgui_internal.h \
             imgui_impl_vulkan.cpp imgui_impl_vulkan.h imgui_impl_dx11.cpp imgui_impl_dx11.h imgui_impl_dx12.cpp \
             imgui_impl_dx12.h imstb_rectpack.h imstb_textedit.h imstb_truetype.h; do
        curl -fsSL --retry 3 -o "toolchain/imgui-nr/$f" \
            "https://raw.githubusercontent.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/f45ccf3a761df91450959d325ac0166cba5364c9/OptiScaler/include/imgui/$f"
    done
    printf '#pragma once\n// Project configuration. Standard stb fonts; no reference-project FreeType hooks.\n' \
        > toolchain/imgui-nr/imconfig.h
fi
(cd toolchain/imgui-nr && sha256sum -c --quiet -) <<EOF || { echo "checksum mismatch: toolchain/imgui-nr" >&2; exit 1; }
c80c5789748d955c4a650562baa0d750494e2c7128c2ca66aaebe2e3c4e198bf  LICENSE.txt
da877efff5dbfc130572e11881751e215e493f647515041195776b974b31dcae  imgui.cpp
9300189bd23f5a920019fb006bf45b297bb389bad24dfdf12768fd04bfa6fc50  imgui.h
4605107eb83713928a1e522b557d7ee22f9e77e5cfb4b09fe8e37c204456289d  imgui_draw.cpp
5c8c575481cc5352510ed55d4dbddfd458268857c1d3be55d8d0ae8c86ad13fe  imgui_tables.cpp
e4ac34e7bc9ef6509e41a1124dcdb460b995b7b56b64681ce924f384518b33e2  imgui_widgets.cpp
7ea0356920f6a8f9b7d85eeead141ad9eadac4f87fdde125262a1b29c8c59aa2  imgui_internal.h
c33a3f98fd7963e5508303f480a6d761198c8d22ad0609f4d23e0e3ff2af9e06  imgui_impl_vulkan.cpp
4723ad0de0d2dc23e6e44871ff5370d3e239e59dc7d2f20d69cae8c33e5266f5  imgui_impl_vulkan.h
63d2e5d0a358a40ded05e2e7d0a8d9c3b7e5eb7a34d352d90dbf3a9e549dc34a  imgui_impl_dx11.cpp
7c40511ddcebd12be1acad7752cb9873a7c621bb5a6b9c58be37500a86f4b7d2  imgui_impl_dx11.h
d4a6f8a59c3c3cc30b7bd05ca58b09f4cd612540980ba7b9e7309920e7aabb41  imgui_impl_dx12.cpp
c74ce2daf4901229511131d06e4d1d2d9f0b22a487e0987ec11a98510e62ef68  imgui_impl_dx12.h
2efa3d5f7d003c19743b15155dad9f46d9f9fc783a18893d703b431d3c990972  imstb_rectpack.h
10777411bf758908aadaf7d73d4710d6ba0464f332f0015db9d1444c19456e6a  imstb_textedit.h
87f08919ae8e7223451d0e9d06b62829075d3e920c9e6ad436db9d7e7427ffce  imstb_truetype.h
EOF
# DLSS5-Feeder (MIT): MinHook, the ReShade add-on headers and Dear ImGui under external/, and the
# DLSS5_Feed.fx effect the ReShade route ships.
repo artifacts/ref/DLSS5-Feeder https://github.com/jlrouzies-fr/DLSS5-Feeder.git \
     76a08db83652617cfe4192eeecf9e46bfc008382
repo artifacts/ref/reshade-shaders https://github.com/crosire/reshade-shaders.git \
     6db142b4b1a05c764222e5b0bd9a644b7ccfe1dc
# vort_Shaders (MIT): the motion vectors of the ReShade route.
repo artifacts/ref/vort_Shaders https://github.com/vortigern11/vort_Shaders.git \
     b410b9f0c0fbb83c8cb42164aaf1655fab386f4a
# The Vulkan loader source is cloned by linux/build/build_vulkan_loader.sh itself.

# ---- binaries the packages carry unmodified ---------------------------------------------------
# ReShade 6.8.0 with add-on support: the DLLs come out of the official installer, which is a zip.
get artifacts/ref/downloads/ReShade_Setup_6.8.0_Addon.exe \
    https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe \
    afe4c8f13048306307983b8b3d41d5bf00a86820440b0e57dea10950e1176445
rs=artifacts/ref/reshade-6.8.0
if [[ ! -f "$rs/ReShade64.dll" ]]; then
    mkdir -p "$rs"
    python3 - artifacts/ref/downloads/ReShade_Setup_6.8.0_Addon.exe "$rs" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    for name in ("ReShade32.dll", "ReShade64.dll"):
        open(f"{sys.argv[2]}/{name}", "wb").write(z.read(name))
PY
fi
sha256sum -c --quiet - <<EOF
da430e0a9c6eecefa0d1b27d05e16c426fb5d04e808b194d914eaac4b31bc0f8  $rs/ReShade32.dll
0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7  $rs/ReShade64.dll
EOF
get "$rs/ReShade-LICENSE.md" https://raw.githubusercontent.com/crosire/reshade/v6.8.0/LICENSE.md \
    237ded5b8344f820113efab1e65e91e1f159d9202c5b4856606a0590d3ffdab0
# OptiScaler (GPL-3.0), the OptiScaler route's host on Linux and Windows, shipped as released: the official nightly
# the packages were tested with (OptiScaler v10.0.0-pre1, nightly-20261127), and its licence from its source tree at
# that nightly.
get artifacts/ref/downloads/OptiScaler_v10.0.0-pre1_20261127_a1e0e193.7z \
    https://github.com/optiscaler/OptiScaler-nightly/releases/download/nightly-20261127/OptiScaler_v10.0.0-pre1_20261127_a1e0e193.7z \
    272f073566fa2634e2c9827abcc496d5b3f3e9b4575b9520e472f4fda3d575f7
mkdir -p artifacts/ref/optiscaler-nightly-20261127
get artifacts/ref/optiscaler-nightly-20261127/LICENSE \
    https://raw.githubusercontent.com/optiscaler/OptiScaler/a1e0e19335c1da2366d12ee2fdb9276daec7d080/LICENSE \
    3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986
# Inter 4.1 (SIL Open Font License 1.1), NR Control's font on the Linux OptiScaler route (embedded in dlssnr-amd.asi).
get artifacts/ref/downloads/Inter-4.1.zip https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip \
    9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
if [[ ! -f toolchain/fonts/inter-4.1/Inter-Regular.ttf ]]; then
    mkdir -p toolchain/fonts/inter-4.1
    python3 - artifacts/ref/downloads/Inter-4.1.zip toolchain/fonts/inter-4.1 <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    open(f"{sys.argv[2]}/Inter-Regular.ttf", "wb").write(z.read("extras/ttf/Inter-Regular.ttf"))
    open(f"{sys.argv[2]}/LICENSE.txt", "wb").write(z.read("LICENSE.txt"))
PY
fi
sha256sum -c --quiet - <<EOF
40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82  toolchain/fonts/inter-4.1/Inter-Regular.ttf
262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a  toolchain/fonts/inter-4.1/LICENSE.txt
EOF

# ---- Windows package only ---------------------------------------------------------------------
if [[ $windows == 1 ]]; then
    # DXVK as the latest GE-Proton release ships it (PE build, it runs on Windows): the DX9 route's d3d9.dll and
    # dxgi.dll, 64-bit and 32-bit, with GE-Proton's dxvk version file, into artifacts/ref/dxvk-ge (ge-proton: the
    # release they came from). NR_GE_PROTON=<tag> (for example GE-Proton11-14) takes that release instead. The release
    # tarball is checked against the .sha512sum GE-Proton publishes beside it (and deleted after, unless it was there
    # already); an unpacked toolchain/<tag>-x86_64 is used as it is.
    # The latest release's tag from the page GitHub redirects releases/latest to (no API call: the API allows 60
    # anonymous calls an hour per address, which shared CI runners use up); the API itself (GH_TOKEN, when set, signs
    # it) only when that page did not name a release.
    ge=${NR_GE_PROTON:-}
    if [[ -z "$ge" ]]; then
        ge=$(curl -fsSLI --retry 3 -o /dev/null -w '%{url_effective}' \
             https://github.com/GloriousEggroll/proton-ge-custom/releases/latest |
             sed -n 's|.*/releases/tag/\(GE-Proton[0-9][0-9]*-[0-9][0-9]*\)$|\1|p') || true
    fi
    if [[ -z "$ge" ]]; then
        ge=$(curl -fsSL --retry 3 ${GH_TOKEN:+-H "Authorization: Bearer $GH_TOKEN"} \
             https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases/latest |
             python3 -c 'import json, sys; print(json.load(sys.stdin)["tag_name"])') ||
            { echo "cannot tell the latest GE-Proton release (set NR_GE_PROTON=<tag>)" >&2; exit 1; }
    fi
    [[ "$ge" =~ ^GE-Proton[0-9]+-[0-9]+$ ]] || { echo "not a GE-Proton release: $ge" >&2; exit 1; }
    dx=artifacts/ref/dxvk-ge
    from=lib/wine/dxvk
    files=(x86_64-windows/d3d9.dll x86_64-windows/dxgi.dll i386-windows/d3d9.dll i386-windows/dxgi.dll version)
    have=1
    [[ "$(cat "$dx/ge-proton" 2>/dev/null)" == "$ge" ]] || have=0
    for f in "${files[@]}"; do [[ -f "$dx/$f" ]] || have=0; done
    if [[ $have == 1 ]]; then
        echo "have $dx ($ge)"
    else
        echo "fetch $dx ($ge)"
        rm -rf -- "$dx"; mkdir -p -- "$dx/x86_64-windows" "$dx/i386-windows"
        if [[ -d "toolchain/$ge-x86_64/files/$from" ]]; then
            for f in "${files[@]}"; do cp -- "toolchain/$ge-x86_64/files/$from/$f" "$dx/$f"; done
        else
            url=https://github.com/GloriousEggroll/proton-ge-custom/releases/download/$ge
            tgz=artifacts/ref/downloads/$ge-x86_64.tar.gz keep=1
            if [[ ! -f "$tgz" ]]; then
                keep=0
                curl -fsSL --retry 3 -o "$tgz.part" "$url/$ge-x86_64.tar.gz"
                mv -- "$tgz.part" "$tgz"
            fi
            sum=$(curl -fsSL --retry 3 "$url/$ge-x86_64.sha512sum" | awk '{print $1}')
            [[ "$sum" =~ ^[0-9a-f]{128}$ ]] || { echo "no checksum published for $ge" >&2; exit 1; }
            echo "$sum  $tgz" | sha512sum -c --quiet - || { echo "checksum mismatch: $tgz" >&2; exit 1; }
            tar -C "$dx" --strip-components=5 -xzf "$tgz" "${files[@]/#/$ge-x86_64/files/$from/}"
            [[ $keep == 1 ]] || rm -f -- "$tgz"
        fi
        echo "$ge" > "$dx/ge-proton"
    fi
    # DXVK's licence, from the commit its version file names
    commit=$(awk '{print $1; exit}' "$dx/version")
    [[ "$commit" =~ ^[0-9a-f]{40}$ ]] || { echo "no DXVK commit in $dx/version" >&2; exit 1; }
    mkdir -p artifacts/ref/dxvk
    if [[ "$(cat artifacts/ref/dxvk/commit 2>/dev/null)" != "$commit" ]]; then
        curl -fsSL --retry 3 -o artifacts/ref/dxvk/LICENSE.part "https://raw.githubusercontent.com/doitsujin/dxvk/$commit/LICENSE"
        grep -q "This software is provided 'as-is'" artifacts/ref/dxvk/LICENSE.part ||
            { echo "DXVK $commit: not the zlib licence" >&2; exit 1; }
        mv -- artifacts/ref/dxvk/LICENSE.part artifacts/ref/dxvk/LICENSE
        echo "$commit" > artifacts/ref/dxvk/commit
    fi
fi
echo "dependencies ready"
