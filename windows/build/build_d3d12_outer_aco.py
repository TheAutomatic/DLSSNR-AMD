#!/usr/bin/env python3
"""The D3D12 passthrough's outer compute kernels (113: the backend's, RenoDX's, Interleaved's, wilsjo2's, HUD FIX's,
frame reuse's) as RADV's ACO machine code, packed into windows/data/aco/d3d12_outer.nra (RDNA4, gfx1201) and
windows/data/aco/rdna3/d3d12_outer.nra (RDNA3, captured as Phoenix, gfx1103), then the manifest check_outer_aco.py
verifies the package build against.

    build_d3d12_outer_aco.py [gfx1201] [phoenix]      (both by default; with a target given the manifest is not written)

Needs: toolchain/slang-2026.19 and toolchain/glslang (as the rest of the build); a Mesa whose RADV makes the code and
whose amdgpu noop drm-shim stands in for the GPU (NR_ACO_MESA, default /opt/mesa26.2.7: RADV built from the
mesa-26.2.7 tag with -Dvulkan-drivers=amd -Dtools=drm-shim); g++ and toolchain/Vulkan-Headers for the two capture
tools (outer_aco_capture.cpp, outer_aco/capture_dump.cpp, built into a temporary folder); llvm-objdump-21 for the
checks of outer_aco/aco_record.py. Nothing runs on a GPU. The same Mesa makes the same bytes: the code changes only
where a shader (or the Mesa) does.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent / 'outer_aco'))
import aco_record  # noqa: E402

SLANGC = ROOT / 'toolchain' / 'slang-2026.19' / 'bin' / 'slangc'
GLSLANG = ROOT / 'toolchain' / 'glslang' / 'bin' / 'glslang'
MESA = Path(os.environ.get('NR_ACO_MESA', '/opt/mesa26.2.7'))

ARCHIVE_MAGIC = b'NRD12OUT'
KERNELS = 113   # 83 + the 14 closed-form grade twins of Interleaved (il_ga_0..6, il0_ga_0..6) + 16 of frame reuse
ARCHIVE_VERSION = 1

KIND_SRV_TEX = 1
KIND_UAV_TEX = 2
KIND_BUF = 3

OUTER_PUSH_OFF = 1664


def fnv64(data: bytes) -> int:
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def spv_dimensions(data: bytes) -> tuple[int, int, int]:
    w = struct.unpack('<' + 'I' * (len(data) // 4), data)
    if len(w) < 5 or w[0] != 0x07230203:
        raise ValueError('invalid SPIR-V header')
    constants = {}
    dims = None
    ids = None
    p = 5
    while p < len(w):
        n = w[p] >> 16
        op = w[p] & 0xFFFF
        if n == 0 or p + n > len(w):
            raise ValueError('invalid SPIR-V instruction')
        a = w[p + 1:p + n]
        if op == 16 and len(a) >= 5 and a[1] == 17:
            dims = (a[2], a[3], a[4])
        elif op == 331 and len(a) >= 5 and a[1] == 38:
            ids = (a[2], a[3], a[4])
        elif (op == 43 or op == 50) and len(a) >= 3:
            constants[a[1]] = a[2]
        p += n
    if ids is not None:
        dims = tuple(constants[x] for x in ids)
    if dims is None or len(dims) != 3 or not all(x > 0 for x in dims):
        raise ValueError('no literal workgroup dimensions')
    return dims


def pack_nrpal002(info: dict, code: bytes, spv_hash: int, mach: int) -> bytes:
    pairs = info['inline']
    fields = [info[k] for k in ('rsrc1', 'rsrc2', 'rsrc3', 'exec_bytes', 'workgroup_size',
                                'shared_bytes', 'push_bytes', 'user_sgprs')]
    header = struct.pack('<8sQQ8I3i4I', b'NRPAL002', spv_hash, fnv64(code), *fields,
                         info['set0'], info['push'], info['grid'], len(pairs), len(code),
                         mach, info['scratch_bytes'])
    assert len(header) == 84
    return header + b'\0' * 12 + b''.join(struct.pack('<ii', *p) for p in pairs) + code


def run_checked(cmd: list[str], env: dict[str, str] | None = None) -> None:
    res = subprocess.run(cmd, env=env, text=True, capture_output=True)
    if res.returncode != 0:
        raise RuntimeError(f'command failed ({res.returncode}): {" ".join(cmd)}\n{res.stdout}\n{res.stderr}')


def mesa_paths() -> tuple[str, str, str]:
    for lib in (MESA / 'lib' / 'x86_64-linux-gnu', MESA / 'usr' / 'lib' / 'x86_64-linux-gnu'):
        if (lib / 'libvulkan_radeon.so').is_file():
            break
    else:
        raise FileNotFoundError(f'no RADV (libvulkan_radeon.so) in {MESA} (NR_ACO_MESA)')
    shim = lib / 'libamdgpu_noop_drm_shim.so'
    if not shim.is_file():
        raise FileNotFoundError(f'no amdgpu noop drm-shim in {lib} (build the Mesa with -Dtools=drm-shim)')
    icds = sorted((lib.parents[1] / 'share' / 'vulkan' / 'icd.d').glob('radeon_icd*.json'))
    if not icds:
        raise FileNotFoundError(f'no radeon ICD file under {lib.parents[1]}/share/vulkan/icd.d')
    return str(lib), str(icds[0]), str(shim)


def build_tools(tmp: Path) -> tuple[Path, Path]:
    vk = ROOT / 'toolchain' / 'Vulkan-Headers' / 'include'
    capture = tmp / 'outer_aco_capture'
    dump = tmp / 'capture_dump.so'
    run_checked(['g++', '-std=c++17', '-O2', f'-I{vk}', str(ROOT / 'windows' / 'build' / 'outer_aco_capture.cpp'),
                 '-o', str(capture), '-l:libvulkan.so.1'])
    run_checked(['g++', '-std=c++17', '-O2', '-shared', '-fPIC', f'-I{vk}',
                 str(ROOT / 'windows' / 'build' / 'outer_aco' / 'capture_dump.cpp'), '-ldl', '-o', str(dump)])
    return capture, dump


def capture_spv(name: str, spv_path: Path, amdgpu_id: str, mach: int, driconf_dir: Path, cap_dir: Path,
                tools: tuple[Path, Path], push_bytes: int = 0, spec0: int | None = None,
                mip_linear: bool = False) -> tuple[str, tuple[int, int, int], list[tuple[int, int, int, int]], bytes]:
    spv_bytes = spv_path.read_bytes()
    wg = spv_dimensions(spv_bytes)
    spv_hash = fnv64(spv_bytes)
    lib, icd, shim = mesa_paths()
    capture, dump_lib = tools
    dump = cap_dir / name.replace('/', '__')
    dump.mkdir(parents=True)
    env = dict(os.environ, DRIRC_CONFIGDIR=str(driconf_dir), VK_ICD_FILENAMES=icd, VK_DRIVER_FILES=icd,
               LD_LIBRARY_PATH=lib, LD_PRELOAD=f'{shim}:{dump_lib}', AMDGPU_GPU_ID=amdgpu_id, CAPTURE_DUMP=str(dump),
               MESA_SHADER_CACHE_DISABLE='true')
    cmd = [str(capture), str(spv_path)]
    if push_bytes:
        cmd += ['--push', str(push_bytes)]
    if spec0 is not None:
        cmd += ['--spec0', str(spec0)]
    if mip_linear:
        cmd += ['--mip-linear']
    res = subprocess.run(cmd, env=env, text=True, capture_output=True)
    key = f'{spv_hash:016x}'
    if res.returncode != 0 or not (dump / f'{key}.blob').is_file():
        raise RuntimeError(f'{name} ({amdgpu_id}): capture failed ({res.returncode}):\n{res.stdout}{res.stderr}')
    info, code = aco_record.record(dump, key, name, cap_dir / 'scan.o', 'gfx1201' if mach == 0x4E else 'gfx1103')
    if info['push_bytes'] > 256:
        raise ValueError(f'{name}: push_bytes {info["push_bytes"]} > 256')
    if info['set0'] not in (0, -1):
        raise ValueError(f'{name}: unexpected set0 SGPR {info["set0"]}')
    if wg[0] * wg[1] * wg[2] != info['workgroup_size']:
        raise ValueError(f'{name}: workgroup mismatch {wg} vs {info["workgroup_size"]}')
    bindings = aco_record.bindings(spv_bytes)
    for b, kind, offset, stride in bindings:
        if offset + (16 if kind == KIND_BUF else 32) > OUTER_PUSH_OFF:
            raise ValueError(f'{name}: binding {b} ends past {OUTER_PUSH_OFF}')
    return name, wg, bindings, pack_nrpal002(info, code, spv_hash, mach)


def lp_hlsl(align: int) -> str:
    m = f'{align - 1}u'
    return (
        f'uint lp8(uint w) {{ return ((w * 8u + {m}) & ~{m}) / 8u; }}\n'
        f'uint lp16(uint w) {{ return ((w * 16u + {m}) & ~{m}) / 16u; }}\n'
    )


def build_backend_hlsl(tmp: Path, align: int) -> list[tuple[str, Path, bool]]:
    bdir = tmp / 'backend_src'
    bdir.mkdir(parents=True, exist_ok=True)
    header = (
        '[[vk::push_constant]] cbuffer C {\n'
        '    uint g_w; uint g_h; uint g_pitch; uint g_pad1; float4 g_region;\n'
        '};\n'
    )
    entries = {
        'backend/to_linear': (
            header +
            '[[vk::binding(0, 0)]] Texture2D<float4> src_tex;\n'
            '[[vk::binding(3, 0)]] RWStructuredBuffer<uint2> lin_dst;\n'
            '[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n'
            '    if (id.x < g_w && id.y < g_h) {\n'
            '        uint4 h = f32tof16(src_tex.Load(int3(id.xy, 0)));\n'
            '        lin_dst[id.y * g_pitch + id.x] = uint2(h.x | (h.y << 16), h.z | (h.w << 16));\n'
            '    }\n'
            '}\n'
        ),
        'backend/to_linear32': (
            header +
            '[[vk::binding(0, 0)]] Texture2D<float4> src_tex;\n'
            '[[vk::binding(3, 0)]] RWStructuredBuffer<uint2> lin_dst;\n'
            '[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n'
            '    if (id.x < g_w && id.y < g_h) {\n'
            '        const uint4 v = asuint(src_tex.Load(int3(id.xy, 0)));\n'
            '        lin_dst[(id.y * g_pitch + id.x) * 2] = v.xy;\n'
            '        lin_dst[(id.y * g_pitch + id.x) * 2 + 1] = v.zw;\n'
            '    }\n'
            '}\n'
        ),
        'backend/from_linear': (
            header +
            '[[vk::binding(1, 0)]] StructuredBuffer<uint2> lin_src;\n'
            '[[vk::binding(2, 0)]] RWTexture2D<float4> dst_tex;\n'
            '[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n'
            '    if (id.x >= g_w || id.y >= g_h) return;\n'
            '    uint2 p = lin_src[id.y * g_pitch + id.x];\n'
            '    dst_tex[id.xy] = f16tof32(uint4(p.x & 0xffffu, p.x >> 16, p.y & 0xffffu, p.y >> 16));\n'
            '}\n'
        ),
        'backend/to_motion': (
            header +
            '[[vk::binding(0, 0)]] Texture2D<float4> src_tex;\n'
            '[[vk::binding(3, 0)]] RWStructuredBuffer<uint2> lin_dst;\n'
            '[[vk::binding(4, 0)]] SamplerState linear_clamp;\n'
            '[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n'
            '    if (id.x >= g_w || id.y >= g_h) return;\n'
            '    const float2 uv = (float2(id.xy) + 0.5) / float2(g_w, g_h);\n'
            '    uint2 h = f32tof16(src_tex.SampleLevel(linear_clamp, uv * g_region.xy + g_region.zw, 0).xy);\n'
            '    lin_dst[id.y * g_pitch + id.x] = uint2(h.x | (h.y << 16), 0u);\n'
            '}\n'
        ),
        'backend/to_depth': (
            header +
            '[[vk::binding(0, 0)]] Texture2D<float4> src_tex;\n'
            '[[vk::binding(3, 0)]] RWStructuredBuffer<uint2> lin_dst;\n'
            '[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n'
            '    if (id.x >= g_w || id.y >= g_h) return;\n'
            '    const float2 s = floor((float2(id.xy) + 0.5) * g_region.xy / float2(g_w, g_h));\n'
            '    const int2 t = min(int2(s), int2(g_region.xy) - 1);\n'
            '    uint idx = (id.y * g_pitch + id.x) * 2;\n'
            '    lin_dst[idx] = uint2(asuint(src_tex.Load(int3(t + int2(g_region.zw), 0)).r), 0u);\n'
            '    lin_dst[idx + 1] = uint2(0u, 0u);\n'
            '}\n'
        ),
        'backend/copy_depth_r32': (
            '[[vk::push_constant]] cbuffer C { uint g_w; uint g_h; };\n'
            '[[vk::binding(0, 0)]] Texture2D<float> src_tex;\n'
            '[[vk::binding(2, 0)]] RWTexture2D<float> dst_tex;\n'
            '[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n'
            '    if (id.x < g_w && id.y < g_h) dst_tex[id.xy] = src_tex.Load(int3(id.xy, 0));\n'
            '}\n'
        ),
    }
    out = []
    for kname, code in entries.items():
        short = kname.split('/')[1]
        hlsl = bdir / f'{short}.comp.hlsl'
        spv = bdir / f'{short}.spv'
        hlsl.write_text(code)
        run_checked([str(SLANGC), str(hlsl), '-target', 'spirv', '-profile', 'glsl_460',
                     '-entry', 'main', '-stage', 'compute', '-O2', '-o', str(spv)])
        out.append((kname, spv, False))
    return out


def prepare_renodx_tree(tmp: Path, align: int) -> Path:
    rdir = tmp / 'renodx_src'
    shutil.copytree(ROOT / 'windows' / 'shaders' / 'renodx', rdir)
    pdir = rdir / 'renodx-prep'
    pdir.mkdir(exist_ok=True)
    for f in (ROOT / 'windows' / 'shaders' / 'renodx-prep').glob('*.hlsli'):
        shutil.copy2(f, pdir / f.name)

    (rdir / 'vk_binding.hlsli').write_text(
        '#ifndef RENODX_VK_BINDING_HLSLI\n'
        '#define RENODX_VK_BINDING_HLSLI\n'
        '#define VK_BINDING(n)\n'
        '#define PUSH_CONSTANTS(name) [[vk::push_constant]] cbuffer name\n'
        '#define VK_OUTPUT_PIXEL(pixel) (pixel)\n'
        'float2 VkUnpackHalf2x16(uint word) {\n'
        '  return spirv_asm { result:$$float2 = OpExtInst glsl450 UnpackHalf2x16 $word };\n'
        '}\n'
        'uint VkPackHalf2x16(float2 pair) {\n'
        '  return spirv_asm { result:$$uint = OpExtInst glsl450 PackHalf2x16 $pair };\n'
        '}\n'
        'float VkF16ToF32(uint word) { return VkUnpackHalf2x16(word).x; }\n'
        'uint VkF32ToF16(float value) { return VkPackHalf2x16(float2(value, 0.f)); }\n'
        '#define f16tof32 VkF16ToF32\n'
        '#define f32tof16 VkF32ToF16\n'
        '#endif\n'
    )

    v6 = (rdir / 'v6_common.hlsli').read_text()
    v6 = v6.replace('Proxy.Load(', 'LoadProxyTexel(tex_size.x, ')
    v6 = v6.replace('Neural.Load(', 'LoadNeuralTexel(tex_size.x, ')
    row = f'lp8(max(w, 1u))'
    lin_io_block = (
        '#ifdef NR_LIN_IO\n'
        'ByteAddressBuffer Proxy : register(t1);\n'
        'ByteAddressBuffer Neural : register(t2);\n'
        'RWByteAddressBuffer OutputLin : register(u1);\n' +
        lp_hlsl(align) +
        f'float4 LoadProxyTexel(uint w, int3 c) {{ uint2 p = Proxy.Load2((uint(c.y) * {row} + uint(c.x)) * 8u); '
        'return float4(VkUnpackHalf2x16(p.x), VkUnpackHalf2x16(p.y)); }\n'
        f'float4 LoadNeuralTexel(uint w, int3 c) {{ uint2 p = Neural.Load2((uint(c.y) * {row} + uint(c.x)) * 8u); '
        'return float4(VkUnpackHalf2x16(p.x), VkUnpackHalf2x16(p.y)); }\n'
        '#else\n'
        'VK_BINDING(1) Texture2D<float4> Proxy : register(t1);'
    )
    if 'VK_BINDING(1) Texture2D<float4> Proxy : register(t1);' not in v6:
        raise RuntimeError('v6_common.hlsli Proxy declaration missing')
    v6 = v6.replace('VK_BINDING(1) Texture2D<float4> Proxy : register(t1);', lin_io_block)
    if 'VK_BINDING(2) Texture2D<float4> Neural : register(t2);' not in v6:
        raise RuntimeError('v6_common.hlsli Neural declaration missing')
    v6 = v6.replace(
        'VK_BINDING(2) Texture2D<float4> Neural : register(t2);',
        'VK_BINDING(2) Texture2D<float4> Neural : register(t2);\n'
        'float4 LoadProxyTexel(uint w, int3 c) { return Proxy.Load(c); }\n'
        'float4 LoadNeuralTexel(uint w, int3 c) { return Neural.Load(c); }\n#endif'
    )
    (rdir / 'v6_common.hlsli').write_text(v6)
    return rdir


def build_renodx_hlsl(tmp: Path, align: int) -> list[tuple[str, Path, bool]]:
    rdir = prepare_renodx_tree(tmp, align)
    sdir = tmp / 'renodx_spv'
    sdir.mkdir(parents=True, exist_ok=True)
    lin_store = (
        '{ float4 s = float4(SrgbEncode(proxy), 1.0); '
        'OutputLin.Store2((pixel.y * lp8(max(Size.x, 1u)) + pixel.x) * 8u, '
        'uint2(VkPackHalf2x16(s.xy), VkPackHalf2x16(s.zw))); }'
    )

    specs = [
        ('renodx/legacy_encode', 'legacy_encode', None),
        ('renodx/legacy_decode', 'legacy_decode', None),
        ('renodx/legacy_reduce', 'legacy_reduce', None),
        ('renodx/legacy_reduce_unshaped', 'legacy_reduce_unshaped', None),
        ('renodx/legacy_apply', 'legacy_apply', None),
        ('renodx/legacy_decode_reduce', 'legacy_decode_reduce', None),
        ('renodx/look_compose', 'look_compose', None),
        ('renodx/look_upsample', 'look_upsample', None),
        ('renodx/look_band', 'look_band', None),
        ('renodx/v6_linearize', 'v6_linearize', None),
        ('renodx/v6_autoscale', 'v6_autoscale', None),
        ('renodx/v6_exposure_scale', 'v6_exposure_scale', None),
        ('renodx/v6_encode', 'v6_encode', None),
        ('renodx/v6_resolve', 'v6_resolve', None),
        ('renodx/v6_pedestal_reduce', 'v6_pedestal_reduce', None),
        ('renodx/v6_pedestal_reduce_unshaped', 'v6_pedestal_reduce_unshaped', None),
        ('renodx/v6_commit', 'v6_commit', None),
        ('renodx/v6_commit_exposure', 'v6_commit_exposure', None),
        ('renodx/v6_resolve_commit', 'v6_resolve_commit', None),
        ('renodx/v6_resolve_prep', 'v6_resolve', 'NR_PREP_RESOLVE'),
        ('renodx/v6_resolve_commit_prep', 'v6_resolve_commit', 'NR_PREP_RESOLVE'),
        ('renodx/v6_pedestal_reduce_unshaped_prep', 'v6_pedestal_reduce_unshaped', 'NR_PREP_RESOLVE'),
        ('renodx/v6_encode_lin', 'v6_encode', 'NR_LIN_IO'),
        ('renodx/v6_resolve_lin', 'v6_resolve', 'NR_LIN_IO'),
        ('renodx/v6_resolve_commit_lin', 'v6_resolve_commit', 'NR_LIN_IO'),
        ('renodx/v6_resolve_commit_up', 'v6_resolve_commit_up', None),
        ('renodx/legacy_decode_up', 'legacy_decode_up', None),
    ]
    out = []
    for kname, src_stem, macro in specs:
        text = (rdir / f'{src_stem}.cs_5_1.hlsl').read_text()
        if macro == 'NR_LIN_IO':
            text = text.replace('Output[pixel] = float4(SrgbEncode(proxy), 1.0);', lin_store)
        wrapper = sdir / f'{kname.split("/")[1]}.comp.slang'
        prefix = f'#define {macro} 1\n' if macro else ''
        # Inline patched source or write a local patched file per variant
        patched_hlsl = rdir / f'_var_{kname.split("/")[1]}.hlsl'
        patched_hlsl.write_text(prefix + text)
        wrapper.write_text(f'#include "{patched_hlsl.name}"\n')
        spv = sdir / f'{kname.split("/")[1]}.spv'
        run_checked([
            str(SLANGC), str(wrapper), '-I', str(rdir), '-I', str(rdir / 'renodx-prep'),
            '-target', 'spirv', '-profile', 'glsl_460', '-entry', 'main', '-stage', 'compute', '-O2',
            '-fvk-t-shift', '0', 'all', '-fvk-u-shift', '6', 'all', '-o', str(spv)
        ])
        out.append((kname, spv, False))
    return out


def build_renodx_aux_hlsl(tmp: Path, align: int) -> list[tuple[str, Path, bool]]:
    adir = tmp / 'renodx_aux_src'
    adir.mkdir(parents=True, exist_ok=True)
    common = (
        '[[vk::push_constant]] cbuffer AuxC {\n'
        '    uint a_w; uint a_h; uint a_what; uint a_pad0; float4 a_region;\n'
        '};\n' +
        lp_hlsl(align) +
        'float2 VkUnpackHalf2x16(uint word) {\n'
        '    return spirv_asm { result:$$float2 = OpExtInst glsl450 UnpackHalf2x16 $word };\n'
        '}\n'
        'uint VkPackHalf2x16(float2 pair) {\n'
        '    return spirv_asm { result:$$uint = OpExtInst glsl450 PackHalf2x16 $pair };\n'
        '}\n'
        'uint2 pack_f16x4(float4 v) { return uint2(VkPackHalf2x16(v.xy), VkPackHalf2x16(v.zw)); }\n'
        'float4 unpack_f16x4(uint2 p) { return float4(VkUnpackHalf2x16(p.x), VkUnpackHalf2x16(p.y)); }\n'
    )
    entries = {
        'renodx_aux/tex_to_buf': (
            common +
            'Texture2D<float4> aux_tex : register(t0);\n'
            'RWByteAddressBuffer aux_raw_out : register(u0);\n'
            '[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n'
            '    if (id.x < a_w && id.y < a_h) {\n'
            '        if (a_what == 1u) aux_raw_out.Store4((id.y * lp16(a_w) + id.x) * 16, asuint(aux_tex.Load(int3(id.xy, 0))));\n'
            '        else aux_raw_out.Store2((id.y * lp8(a_w) + id.x) * 8, pack_f16x4(aux_tex.Load(int3(id.xy, 0))));\n'
            '    }\n'
            '}\n'
        ),
        'renodx_aux/buf_to_tex': (
            common +
            'ByteAddressBuffer aux_raw_in : register(t0);\n'
            'RWTexture2D<float4> aux_dst : register(u0);\n'
            '[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n'
            '    if (id.x >= a_w || id.y >= a_h) return;\n'
            '    aux_dst[id.xy] = unpack_f16x4(aux_raw_in.Load2((id.y * lp8(a_w) + id.x) * 8));\n'
            '}\n'
        ),
        'renodx_aux/copy_out': (
            common +
            'Texture2D<float4> aux_tex : register(t0);\n'
            'RWTexture2D<float4> aux_dst : register(u0);\n'
            '[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n'
            '    if (id.x >= a_w || id.y >= a_h) return;\n'
            '    aux_dst[id.xy] = aux_tex.Load(int3(id.xy, 0));\n'
            '}\n'
        ),
        'renodx_aux/to_motion': (
            common +
            'Texture2D<float4> aux_tex : register(t0);\n'
            'SamplerState aux_linear_clamp : register(s0);\n'
            'RWByteAddressBuffer aux_raw_out : register(u0);\n'
            '[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n'
            '    if (id.x >= a_w || id.y >= a_h) return;\n'
            '    const float2 uv = (float2(id.xy) + 0.5f) / float2(a_w, a_h);\n'
            '    const float2 mv = aux_tex.SampleLevel(aux_linear_clamp, uv * a_region.xy + a_region.zw, 0).xy;\n'
            '    aux_raw_out.Store2((id.y * lp8(a_w) + id.x) * 8, uint2(VkPackHalf2x16(mv), 0u));\n'
            '}\n'
        ),
        'renodx_aux/to_depth': (
            common +
            'Texture2D<float4> aux_tex : register(t0);\n'
            'RWStructuredBuffer<float4> aux_out : register(u0);\n'
            '[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n'
            '    if (id.x >= a_w || id.y >= a_h) return;\n'
            '    const float2 s = floor((float2(id.xy) + 0.5f) * a_region.xy / float2(a_w, a_h));\n'
            '    const int2 t = min(int2(s), int2(a_region.xy) - 1);\n'
            '    aux_out[id.y * lp16(a_w) + id.x] = float4(aux_tex.Load(int3(t + int2(a_region.zw), 0)).r, 0, 0, 0);\n'
            '}\n'
        ),
        'renodx_aux/sdr_encode_lin': (
            common +
            'Texture2D<float4> aux_tex : register(t0);\n'
            'RWByteAddressBuffer aux_raw_out : register(u0);\n'
            'float lin2srgb1(float x) { return x <= 0.0031308f ? x * 12.92f : 1.055f * pow(x, 1.0f / 2.4f) - 0.055f; }\n'
            '[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n'
            '    if (id.x >= a_w || id.y >= a_h) return;\n'
            '    const uint2 src_sz = max(uint2(asuint(a_region.xy)), uint2(1, 1));\n'
            '    const uint2 src_px = uint2(asuint(a_region.zw)) + min(uint2(((float2(id.xy) + 0.5f) * float2(src_sz)) / float2(a_w, a_h)), src_sz - 1);\n'
            '    const float4 s = aux_tex.Load(int3(src_px, 0));\n'
            '    float3 p = max(s.rgb, 0.0f);\n'
            '    if (a_pad0 != 0u) p = float3(lin2srgb1(p.x), lin2srgb1(p.y), lin2srgb1(p.z));\n'
            '    aux_raw_out.Store2((id.y * lp8(a_w) + id.x) * 8, pack_f16x4(float4(p, s.a)));\n'
            '}\n'
        ),
        'renodx_aux/sdr_decode_lin': (
            common +
            'Texture2D<float4> aux_tex : register(t0);\n'
            'ByteAddressBuffer aux_raw_neural : register(t1);\n'
            'ByteAddressBuffer aux_raw_proxy : register(t2);\n'
            'RWTexture2D<float4> aux_dst : register(u0);\n'
            '[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n'
            '    if (id.x >= a_w || id.y >= a_h) return;\n'
            '    const float4 src = a_pad0 == 2u ? unpack_f16x4(aux_raw_proxy.Load2((id.y * lp8(a_w) + id.x) * 8))\n'
            '                     : (a_pad0 != 0u ? aux_dst[id.xy] : aux_tex.Load(int3(id.xy, 0)));\n'
            '    const uint2 n_sz = max(uint2(asuint(a_region.xy)), uint2(1, 1));\n'
            '    const uint2 n_px = min(uint2(((float2(id.xy) + 0.5f) * float2(n_sz)) / float2(a_w, a_h)), n_sz - 1);\n'
            '    const float3 neural = unpack_f16x4(aux_raw_neural.Load2((n_px.y * lp8(n_sz.x) + n_px.x) * 8)).rgb;\n'
            '    const float orig_y = dot(src.rgb, float3(0.212639f, 0.715169f, 0.072192f));\n'
            '    const float upg_y = dot(neural, float3(0.212639f, 0.715169f, 0.072192f));\n'
            '    const float ratio = orig_y == 0.0f ? 1.0f : clamp(upg_y / orig_y, 0.0f, 4.0f);\n'
            '    const float3 result = lerp(src.rgb * ratio, neural, asfloat(a_what));\n'
            '    aux_dst[id.xy] = float4(result, src.a);\n'
            '}\n'
        ),
    }
    out = []
    for kname, code in entries.items():
        short = kname.split('/')[1]
        hlsl = adir / f'{short}.comp.slang'
        spv = adir / f'{short}.spv'
        hlsl.write_text(code)
        run_checked([
            str(SLANGC), str(hlsl), '-target', 'spirv', '-profile', 'glsl_460',
            '-entry', 'main', '-stage', 'compute', '-O2',
            '-fvk-t-shift', '0', 'all', '-fvk-u-shift', '8', 'all', '-fvk-s-shift', '16', 'all',
            '-o', str(spv)
        ])
        out.append((kname, spv, False))
    return out


def build_glsl_sets(tmp: Path) -> tuple[list[tuple[str, Path, bool]], list[tuple[str, Path, int | None]], list[tuple[str, Path, bool]],
                                        list[tuple[str, Path, bool]]]:
    gdir = tmp / 'glsl_spv'
    gdir.mkdir(parents=True, exist_ok=True)
    renodx_glsl = []
    for k in ('renodx_prep_encode', 'renodx_debug', 'renodx_prep'):
        spv = gdir / f'{k}.spv'
        run_checked([str(GLSLANG), '-V', '--target-env', 'vulkan1.3',
                     f'-I{ROOT}/windows/shaders/renodx-prep',
                     str(ROOT / 'windows' / 'shaders' / 'renodx-prep' / f'{k}.comp'),
                     '-o', str(spv)])
        renodx_glsl.append((f'renodx/{k}', spv, False))
    for k in ('st_fb_in', 'st_prep', 'st_box', 'st_apply'):
        spv = gdir / f'renodx_{k}.spv'
        run_checked([str(GLSLANG), '-V', '--target-env', 'vulkan1.3',
                     f'-I{ROOT}/windows/shaders/renodx-fast',
                     str(ROOT / 'windows' / 'shaders' / 'renodx-fast' / f'{k}.comp'),
                     '-o', str(spv)])
        renodx_glsl.append((f'renodx/renodx_{k}', spv, False))

    il_list = []
    il_spv = gdir / 'il.spv'
    il0_spv = gdir / 'il0.spv'
    ilpp_spv = gdir / 'il_pp.spv'
    # (+ the Style grade in closed form, [Network] Style Grade Acceleration: il_ga / il0_ga, the Vulkan routes' *_ga)
    il_ga_spv = gdir / 'il_ga.spv'
    il0_ga_spv = gdir / 'il0_ga.spv'
    for src, dst, defs in (('il.comp', il_spv, []), ('il0.comp', il0_spv, []), ('il_pp.comp', ilpp_spv, []),
                           ('il.comp', il_ga_spv, ['-DNR_STYLE_GRADE_CLOSED']),
                           ('il0.comp', il0_ga_spv, ['-DNR_STYLE_GRADE_CLOSED'])):
        run_checked([str(GLSLANG), '-V', '--target-env', 'vulkan1.3', *defs,
                     f'-I{ROOT}/windows/shaders/renodx-fast',
                     str(ROOT / 'windows' / 'shaders' / 'renodx-fast' / src),
                     '-o', str(dst)])
    for s in range(7):
        il_list.append((f'il/il_{s}', il_spv, s))
        il_list.append((f'il/il0_{s}', il0_spv, s))
    il_list.append(('il/il_pp', ilpp_spv, None))
    for s in range(7):
        il_list.append((f'il/il_ga_{s}', il_ga_spv, s))
        il_list.append((f'il/il0_ga_{s}', il0_ga_spv, s))
    w2_list = []
    w2_src = ROOT / 'windows' / 'shaders' / 'wilsjo2'
    for k in ('encode', 'resolve', 'meter', 'autoexposure', 'finished', 'clamp',
              'downsample', 'os', 'spatial', 'inspect', 'light', 'half_copy', 'copy'):
        spv = gdir / f'wilsjo2_{k}.spv'
        run_checked([str(GLSLANG), '-V', '--target-env', 'vulkan1.3',
                     f'-I{w2_src}', str(w2_src / f'wilsjo2_{k}.comp'), '-o', str(spv)])
        w2_list.append((f'wilsjo2/{k}', spv, True))

    extra_w2 = [
        ('wilsjo2/spatial_guides', w2_src / 'wilsjo2_spatial.comp', ['-DWILSJO2_GUIDES', f'-I{w2_src}']),
        ('wilsjo2/prep', w2_src / 'wilsjo2_prep.comp', [f'-I{w2_src}', f'-I{ROOT}/windows/shaders/renodx-prep']),
        ('wilsjo2/prep_meter', w2_src / 'wilsjo2_prep.comp', ['-DMETER', f'-I{w2_src}', f'-I{ROOT}/windows/shaders/renodx-prep']),
        ('wilsjo2/classic', w2_src / 'wilsjo2_classic.comp', [f'-I{w2_src}', f'-I{ROOT}/windows/shaders/passes/include']),
        ('wilsjo2/copy_load', w2_src / 'wilsjo2_copy.comp', ['-DCOPY_LOAD', f'-I{w2_src}']),
        ('wilsjo2/hudfix_clear', ROOT / 'windows' / 'shaders' / 'hudfix' / 'hudfix.comp', ['-DHUD_CLEAR']),
        ('wilsjo2/hudfix_count', ROOT / 'windows' / 'shaders' / 'hudfix' / 'hudfix.comp', ['-DHUD_COUNT']),
        ('wilsjo2/hudfix_compose', ROOT / 'windows' / 'shaders' / 'hudfix' / 'hudfix.comp', ['-DHUD_COMPOSE']),
    ]
    for kname, src, flags in extra_w2:
        spv = gdir / f'{kname.replace("/", "_")}.spv'
        run_checked([str(GLSLANG), '-V', '--target-env', 'vulkan1.3', *flags, str(src), '-o', str(spv)])
        w2_list.append((kname, spv, True))

    # Frame reuse (NRFrameReuse) on the D3D12 route: its shaders as build_frame_reuse.sh compiles them (the same SPIR-V
    # the Vulkan routes run), the ones the CPU-cadenced product path records (no GPU decision: no speed / decide /
    # history_copy / history_weight); frame_copy(10) also stores the RGBA16F answer into the frame's format (no blit)
    fr_list = []
    fr_src = ROOT / 'windows' / 'shaders' / 'frame-reuse'
    for kname, src, env, defs in (('frame_reuse/guides', 'guides', 'vulkan1.3', []),
                                  ('frame_reuse/compose16u', 'compose', 'vulkan1.2', ['-DUNORM16_OUTPUT']),
                                  ('frame_reuse/compose', 'compose', 'vulkan1.2', []),
                                  ('frame_reuse/scrgb_encode', 'scrgb', 'vulkan1.2', []),
                                  ('frame_reuse/scrgb_decode', 'scrgb', 'vulkan1.2', ['-DDECODE']),
                                  ('frame_reuse/context', 'context', 'vulkan1.3', []),
                                  ('frame_reuse/display', 'display', 'vulkan1.2', []),
                                  ('frame_reuse/display10', 'display', 'vulkan1.2', ['-DTEN_BIT']),
                                  ('frame_reuse/display16u', 'display', 'vulkan1.2', ['-DFLOAT_OUTPUT=rgba16']),
                                  ('frame_reuse/frame_copy16u', 'frame_copy', 'vulkan1.2', ['-DFLOAT_OUTPUT=rgba16']),
                                  ('frame_reuse/display16f', 'display', 'vulkan1.2', ['-DFLOAT_OUTPUT=rgba16f']),
                                  ('frame_reuse/display11f', 'display', 'vulkan1.2', ['-DFLOAT_OUTPUT=r11f_g11f_b10f']),
                                  ('frame_reuse/frame_copy', 'frame_copy', 'vulkan1.2', []),
                                  ('frame_reuse/frame_copy10', 'frame_copy', 'vulkan1.2', ['-DTEN_BIT']),
                                  ('frame_reuse/frame_copy16f', 'frame_copy', 'vulkan1.2', ['-DFLOAT_OUTPUT=rgba16f']),
                                  ('frame_reuse/frame_copy11f', 'frame_copy', 'vulkan1.2', ['-DFLOAT_OUTPUT=r11f_g11f_b10f'])):
        spv = gdir / f'{kname.replace("/", "_")}.spv'
        run_checked([str(GLSLANG), '-V', '--target-env', env, *defs, str(fr_src / f'{src}.comp'), '-o', str(spv)])
        fr_list.append((kname, spv, False))

    return renodx_glsl, il_list, w2_list, fr_list


def build_archive(amdgpu_id: str, mach: int, align: int, out_path: Path, tools: tuple[Path, Path]) -> None:
    with tempfile.TemporaryDirectory(prefix=f'outer_{amdgpu_id}_') as td:
        tmp = Path(td)
        driconf = tmp / 'driconf'
        driconf.mkdir()
        (driconf / '00-radv-defaults.conf').write_text(
            '<driconf><device><application name="Default">'
            '<option name="radv_enable_custom_border_on_compute_queue" value="true"/>'
            '</application></device></driconf>\n'
        )
        cap_dir = tmp / 'cap'
        cap_dir.mkdir()

        backend = build_backend_hlsl(tmp, align)
        renodx_hlsl = build_renodx_hlsl(tmp, align)
        renodx_aux = build_renodx_aux_hlsl(tmp, align)
        renodx_glsl, il_list, w2_list, fr_list = build_glsl_sets(tmp)

        jobs = []
        for kname, spv, mip in backend + renodx_hlsl + renodx_glsl + renodx_aux:
            jobs.append((kname, spv, 0, None, mip))
        for kname, spv, spec0 in il_list:
            jobs.append((kname, spv, 0, spec0, False))
        for kname, spv, mip in w2_list:
            jobs.append((kname, spv, 256, None, mip))
        for kname, spv, mip in fr_list:
            jobs.append((kname, spv, 0, None, mip))

        assert len(jobs) == KERNELS, f'expected {KERNELS} shaders, got {len(jobs)}'
        entries = []
        for idx, (kname, spv, push_b, spec0, mip) in enumerate(jobs, 1):
            name, wg, bindings, nrp = capture_spv(kname, spv, amdgpu_id, mach, driconf, cap_dir, tools,
                                                  push_bytes=push_b, spec0=spec0, mip_linear=mip)
            entries.append((name, wg, bindings, nrp))
            print(f'[{amdgpu_id} {idx:2d}/{KERNELS}] {name}: wg={wg} bindings={len(bindings)} nrp={len(nrp)}', flush=True)

        blob = bytearray(struct.pack('<8s4I', ARCHIVE_MAGIC, ARCHIVE_VERSION, mach, len(entries), 0))
        for name, wg, bindings, nrp in entries:
            nb = name.encode('utf-8')
            blob += struct.pack('<H', len(nb)) + nb
            blob += struct.pack('<3IH', wg[0], wg[1], wg[2], len(bindings))
            for b, kind, offset, stride in bindings:
                blob += struct.pack('<HBBHH', b, kind, 0, offset, stride)
            blob += struct.pack('<I', len(nrp)) + nrp

        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(bytes(blob))
        print(f'Wrote {out_path} ({len(blob)} bytes, {len(entries)} kernels)', flush=True)


def main() -> int:
    for tool in (SLANGC, GLSLANG):
        if not tool.is_file():
            raise FileNotFoundError(f'missing tool: {tool}')
    if not shutil.which('llvm-objdump-21'):
        raise FileNotFoundError('missing llvm-objdump-21')
    mesa_paths()
    targets = [
        ('gfx1201', 0x4E, 128, ROOT / 'windows' / 'data' / 'aco' / 'd3d12_outer.nra'),
        ('phoenix', 0x44, 256, ROOT / 'windows' / 'data' / 'aco' / 'rdna3' / 'd3d12_outer.nra'),
    ]
    if len(sys.argv) > 1:
        wanted = set(sys.argv[1:])
        targets = [t for t in targets if t[0] in wanted]
    out_dir = os.environ.get('NR_OUTER_ARCHIVE_DIR')   # another folder (a comparison run): no manifest written
    with tempfile.TemporaryDirectory(prefix='outer_tools_') as td:
        tools = build_tools(Path(td))
        for amdgpu_id, mach, align, out_path in targets:
            if out_dir:
                out_path = Path(out_dir) / out_path.relative_to(ROOT / 'windows' / 'data' / 'aco')
            build_archive(amdgpu_id, mach, align, out_path, tools)
    if len(sys.argv) <= 1 and not out_dir:
        run_checked([sys.executable, str(ROOT / 'windows' / 'build' / 'check_outer_aco.py'), '--write-manifest'])
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
