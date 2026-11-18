#!/usr/bin/env python3
"""Compile and dual-sentinel ACO-capture all 83 D3D12 outer compute shaders."""

import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SLANGC = ROOT / 'toolchain' / 'slang-2026.19' / 'bin' / 'slangc'
GLSLANG = ROOT / 'toolchain' / 'glslang' / 'bin' / 'glslang'
CAPTURE = ROOT / 'artifacts' / 'outer_aco_capture'

ICD = '/home/kenny/work/openrr-mesa-263-comparison/build/src/amd/vulkan/radeon_devenv_icd.x86_64.json'
SHIM = '/home/kenny/work/openrr-mesa-263-comparison/build/src/amd/drm-shim/libamdgpu_noop_drm_shim.so'
MESA_LIB = '/home/kenny/src/mesa-deps/lib/x86_64-linux-gnu'

ARCHIVE_MAGIC = b'NRD12OUT'
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


def adapt_code(code: bytes, other: bytes, exec_size: int, info: dict, other_info: dict, logs: tuple[str, str]) -> bytes:
    if exec_size <= 0 or exec_size > len(code) or exec_size % 4 or len(code) % 4:
        raise ValueError('invalid code sizes')
    if any(x[0] != 0xBF9F0000 for x in struct.iter_unpack('<I', code[exec_size:])):
        raise ValueError('constant data after executable code is unsupported')
    if code != other or info != other_info:
        raise ValueError('address32 sentinel captures differ in code or metadata')
    reports = []
    for log in logs:
        lines = re.findall(r'^ONR_M0 (.+)$', log, re.MULTILINE)
        if len(lines) != 1:
            raise ValueError(f'expected one ONR_M0 report, got {len(lines)}: {log}')
        report = {k: int(v) for k, v in (x.split('=') for x in lines[0].split())}
        if set(report) != {'subst', 'writes', 'constdata', 'pc_relative'}:
            raise ValueError(f'invalid ONR_M0 report: {report}')
        if report['writes'] or report['constdata'] or report['pc_relative']:
            raise ValueError(f'unsupported body state: {report}')
        reports.append(report)
    if reports[0] != reports[1]:
        raise ValueError('address32 substitution reports differ')
    return code


def parse_layouts(text: str) -> list[tuple[int, int, int, int]]:
    blocks = text.split('set bytes=')
    if len(blocks) < 2:
        return []
    last = blocks[-1].splitlines()
    hdr = re.match(r'^(\d+)\s+bindings=(\d+)$', last[0].strip())
    if not hdr:
        raise ValueError(f'unexpected layout header: {last[0]!r}')
    set_bytes = int(hdr.group(1))
    if set_bytes > OUTER_PUSH_OFF:
        raise ValueError(f'descriptor set bytes {set_bytes} exceeds {OUTER_PUSH_OFF}')
    bindings = []
    for line in last[1:]:
        line = line.strip()
        if not line:
            continue
        m = re.match(r'^binding=(\d+)\s+type=(\d+)\s+count=(\d+)\s+offset=(\d+)\s+stride=(\d+)\s+image=\d+\s+sampler=\d+$', line)
        if not m:
            raise ValueError(f'unexpected binding line: {line!r}')
        b, vk_type, count, offset, stride = map(int, m.groups())
        if count == 0 or stride == 0:
            continue
        if count != 1:
            raise ValueError(f'binding {b}: descriptor array count={count} unsupported')
        if vk_type in (1, 2):  # COMBINED_IMAGE_SAMPLER or SAMPLED_IMAGE
            kind = KIND_SRV_TEX
            if offset % 32 != 0:
                raise ValueError(f'binding {b}: image SRV offset {offset} not 32-byte aligned')
        elif vk_type == 3:  # STORAGE_IMAGE
            kind = KIND_UAV_TEX
            if offset % 32 != 0:
                raise ValueError(f'binding {b}: image UAV offset {offset} not 32-byte aligned')
        elif vk_type in (6, 7):  # UNIFORM_BUFFER or STORAGE_BUFFER
            kind = KIND_BUF
            if offset % 4 != 0:
                raise ValueError(f'binding {b}: buffer offset {offset} not 4-byte aligned')
        elif vk_type == 0:  # SAMPLER (immutable)
            continue
        else:
            raise ValueError(f'binding {b}: unsupported vk_type={vk_type}')
        if offset + (16 if kind == KIND_BUF else 32) > OUTER_PUSH_OFF:
            raise ValueError(f'binding {b}: end offset exceeds {OUTER_PUSH_OFF}')
        bindings.append((b, kind, offset, stride))
    return bindings


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


def capture_spv(name: str, spv_path: Path, amdgpu_id: str, mach: int, driconf_dir: Path,
                cap_dir: Path, push_bytes: int = 0, spec0: int | None = None,
                mip_linear: bool = False) -> tuple[str, tuple[int, int, int], list[tuple[int, int, int, int]], bytes]:
    spv_bytes = spv_path.read_bytes()
    wg = spv_dimensions(spv_bytes)
    spv_hash = fnv64(spv_bytes)
    env = dict(
        os.environ,
        DRIRC_CONFIGDIR=str(driconf_dir),
        VK_ICD_FILENAMES=ICD,
        LD_PRELOAD=SHIM,
        LD_LIBRARY_PATH=MESA_LIB,
        AMDGPU_GPU_ID=amdgpu_id,
        MESA_SHADER_CACHE_DISABLE='true',
        ONR_ACO_PRECISION='local',
    )
    for k in ('ONR_ACO_VECTOR_LOADS', 'ONR_ACO_CMAT_ADDRESS', 'ONR_ACO_CU_MODE',
              'ONR_ACO_WGP_MODE', 'ONR_ACO_SAMPLE_WAIT_ZERO', 'ONR_ACO_SHARED_CMAT'):
        env.pop(k, None)

    safe_name = name.replace('/', '__')
    captures = []
    for suffix, sentinel in (('a', '0x13579bdf'), ('b', '0x2468ace1')):
        prefix = cap_dir / f'{safe_name}_{amdgpu_id}_{suffix}'
        for ext in ('.code', '.json', '.layouts'):
            prefix.with_suffix(ext).unlink(missing_ok=True)
        env['ONR_ACO_CAPTURE'] = str(prefix)
        env['ONR_ACO_ADDR32_HI'] = sentinel
        cmd = [str(CAPTURE), str(spv_path)]
        if push_bytes:
            cmd += ['--push', str(push_bytes)]
        if spec0 is not None:
            cmd += ['--spec0', str(spec0)]
        if mip_linear:
            cmd += ['--mip-linear']
        res = subprocess.run(cmd, env=env, text=True, capture_output=True)
        log = res.stdout + res.stderr
        if res.returncode != 0:
            raise RuntimeError(f'{name} ({amdgpu_id}): capture failed ({res.returncode}):\n{log}')
        info = json.loads(prefix.with_suffix('.json').read_text())
        code = prefix.with_suffix('.code').read_bytes()
        layouts_txt = prefix.with_suffix('.layouts').read_text() if prefix.with_suffix('.layouts').exists() else ''
        captures.append((info, code, log, layouts_txt))

    info, code, log, layouts_txt = captures[0]
    other_info, other_code, other_log, other_layouts = captures[1]
    if layouts_txt != other_layouts:
        raise ValueError(f'{name}: descriptor layouts differ between sentinel captures')
    if info['wave_size'] != 32:
        raise ValueError(f'{name}: unsupported wave_size {info["wave_size"]}')
    if info['push_bytes'] > 256:
        raise ValueError(f'{name}: push_bytes {info["push_bytes"]} > 256')
    if info['set0'] not in (0, -1):
        raise ValueError(f'{name}: unexpected set0 SGPR {info["set0"]}')
    if wg[0] * wg[1] * wg[2] != info['workgroup_size']:
        raise ValueError(f'{name}: workgroup mismatch {wg} vs {info["workgroup_size"]}')
    code = adapt_code(code, other_code, info['exec_bytes'], info, other_info, (log, other_log))
    bindings = parse_layouts(layouts_txt)
    nrp = pack_nrpal002(info, code, spv_hash, mach)
    return name, wg, bindings, nrp


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
            '[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n'
            '    if (id.x >= a_w || id.y >= a_h) return;\n'
            '    const uint2 src_sz = max(uint2(asuint(a_region.xy)), uint2(1, 1));\n'
            '    const uint2 src_px = uint2(asuint(a_region.zw)) + min(uint2(((float2(id.xy) + 0.5f) * float2(src_sz)) / float2(a_w, a_h)), src_sz - 1);\n'
            '    const float3 p = max(aux_tex.Load(int3(src_px, 0)).rgb, 0.0f);\n'
            '    aux_raw_out.Store2((id.y * lp8(a_w) + id.x) * 8, pack_f16x4(float4(p, 1.0f)));\n'
            '}\n'
        ),
        'renodx_aux/sdr_decode_lin': (
            common +
            'Texture2D<float4> aux_tex : register(t0);\n'
            'ByteAddressBuffer aux_raw_neural : register(t1);\n'
            'RWTexture2D<float4> aux_dst : register(u0);\n'
            '[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n'
            '    if (id.x >= a_w || id.y >= a_h) return;\n'
            '    const float4 src = a_pad0 != 0 ? aux_dst[id.xy] : aux_tex.Load(int3(id.xy, 0));\n'
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


def build_glsl_sets(tmp: Path) -> tuple[list[tuple[str, Path, bool]], list[tuple[str, Path, int | None]], list[tuple[str, Path, bool]]]:
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
    for src, dst in (('il.comp', il_spv), ('il0.comp', il0_spv), ('il_pp.comp', ilpp_spv)):
        run_checked([str(GLSLANG), '-V', '--target-env', 'vulkan1.3',
                     f'-I{ROOT}/windows/shaders/renodx-fast',
                     str(ROOT / 'windows' / 'shaders' / 'renodx-fast' / src),
                     '-o', str(dst)])
    for s in range(7):
        il_list.append((f'il/il_{s}', il_spv, s))
        il_list.append((f'il/il0_{s}', il0_spv, s))
    il_list.append(('il/il_pp', ilpp_spv, None))

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

    return renodx_glsl, il_list, w2_list


def build_archive(amdgpu_id: str, mach: int, align: int, out_path: Path) -> None:
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
        renodx_glsl, il_list, w2_list = build_glsl_sets(tmp)

        jobs = []
        for kname, spv, mip in backend + renodx_hlsl + renodx_glsl + renodx_aux:
            jobs.append((kname, spv, 0, None, mip))
        for kname, spv, spec0 in il_list:
            jobs.append((kname, spv, 0, spec0, False))
        for kname, spv, mip in w2_list:
            jobs.append((kname, spv, 256, None, mip))

        assert len(jobs) == 83, f'expected 83 shaders, got {len(jobs)}'
        entries = []
        for idx, (kname, spv, push_b, spec0, mip) in enumerate(jobs, 1):
            name, wg, bindings, nrp = capture_spv(kname, spv, amdgpu_id, mach, driconf, cap_dir,
                                                  push_bytes=push_b, spec0=spec0, mip_linear=mip)
            entries.append((name, wg, bindings, nrp))
            print(f'[{amdgpu_id} {idx:2d}/83] {name}: wg={wg} bindings={len(bindings)} nrp={len(nrp)}', flush=True)

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
    for tool in (SLANGC, GLSLANG, CAPTURE):
        if not tool.is_file():
            raise FileNotFoundError(f'missing tool: {tool}')
    targets = [
        ('gfx1201', 0x4E, 128, ROOT / 'windows' / 'data' / 'aco' / 'd3d12_outer.nra'),
        ('phoenix', 0x44, 256, ROOT / 'windows' / 'data' / 'aco' / 'rdna3' / 'd3d12_outer.nra'),
    ]
    if len(sys.argv) > 1:
        wanted = set(sys.argv[1:])
        targets = [t for t in targets if t[0] in wanted]
    for amdgpu_id, mach, align, out_path in targets:
        build_archive(amdgpu_id, mach, align, out_path)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
