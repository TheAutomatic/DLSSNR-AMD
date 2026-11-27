#!/usr/bin/env python3
"""Verify that prebuilt D3D12 outer ACO archives (.nra) match shader sources and layout invariants."""

import hashlib
import json
import pathlib
import struct
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = ROOT / 'windows' / 'data' / 'aco' / 'd3d12_outer.manifest.json'
ARCHIVES = (
    ('rdna4', ROOT / 'windows' / 'data' / 'aco' / 'd3d12_outer.nra', False),
    ('rdna3', ROOT / 'windows' / 'data' / 'aco' / 'rdna3' / 'd3d12_outer.nra', True),
)
INPUT_DIRS = (
    'windows/shaders/renodx',
    'windows/shaders/renodx-prep',
    'windows/shaders/renodx-fast',
    'windows/shaders/wilsjo2',
    'windows/shaders/rdna4/include',
    'windows/shaders/passes/include',
    'windows/shaders/hudfix',
    'windows/shaders/frame-reuse',
)
INPUT_FILES = (
    'windows/build/build_d3d12_outer_aco.py',
    'windows/build/outer_aco_capture.cpp',
    'windows/build/outer_aco/aco_record.py',
    'windows/build/outer_aco/capture_dump.cpp',
)
GFX11_MACHS = {0x41, 0x43, 0x44, 0x46, 0x47, 0x4A, 0x55, 0x58}


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fnv1a_64(data: bytes) -> int:
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def collect_inputs() -> dict[str, str]:
    out: dict[str, str] = {}
    for rel in INPUT_FILES:
        p = ROOT / rel
        if not p.is_file():
            raise FileNotFoundError(f'missing outer ACO input: {rel}')
        out[rel] = sha256_bytes(p.read_bytes())
    for drel in INPUT_DIRS:
        d = ROOT / drel
        if not d.is_dir():
            raise FileNotFoundError(f'missing outer shader directory: {drel}')
        for p in sorted(d.rglob('*')):
            if p.is_file():
                rel = p.relative_to(ROOT).as_posix()
                out[rel] = sha256_bytes(p.read_bytes())
    return dict(sorted(out.items()))


def validate_archive(label: str, path: pathlib.Path, expect_rdna3: bool,
                     spv_hashes_out: dict[str, int] | None = None) -> dict[str, object]:
    if not path.is_file():
        raise FileNotFoundError(f'missing outer ACO archive ({label}): {path.relative_to(ROOT)}')
    raw = path.read_bytes()
    if len(raw) < 24 or raw[:8] != b'NRD12OUT':
        raise ValueError(f'{label}: bad NRD12OUT header')
    version, mach, count, reserved = struct.unpack_from('<4I', raw, 8)
    if version != 1 or count != 113 or reserved != 0:
        raise ValueError(f'{label}: unexpected header version={version} count={count} reserved={reserved}')
    if not expect_rdna3 and mach != 0x4E:
        raise ValueError(f'{label}: expected gfx1201 (0x4E), got 0x{mach:X}')
    if expect_rdna3 and mach not in GFX11_MACHS:
        raise ValueError(f'{label}: unexpected RDNA3 mach 0x{mach:X}')

    pos = 24
    seen_names: set[str] = set()
    for idx in range(count):
        if pos + 2 > len(raw):
            raise ValueError(f'{label}: truncated entry {idx}')
        name_len = struct.unpack_from('<H', raw, pos)[0]
        pos += 2
        if name_len == 0 or pos + name_len + 14 > len(raw):
            raise ValueError(f'{label}: truncated metadata at entry {idx}')
        name = raw[pos:pos + name_len].decode('utf-8')
        pos += name_len
        if name in seen_names:
            raise ValueError(f'{label}: duplicate kernel name {name!r}')
        seen_names.add(name)
        wg0, wg1, wg2, nb = struct.unpack_from('<3IH', raw, pos)
        pos += 14
        if not wg0 or not wg1 or not wg2 or wg0 * wg1 * wg2 > 1024:
            raise ValueError(f'{label}: invalid workgroup {wg0}x{wg1}x{wg2} in {name}')
        if pos + nb * 8 + 4 > len(raw):
            raise ValueError(f'{label}: truncated bindings in {name}')
        intervals: list[tuple[int, int]] = []
        for _ in range(nb):
            _b, kind, pad, offset, stride = struct.unpack_from('<HBBHH', raw, pos)
            pos += 8
            if kind not in (1, 2, 3):
                raise ValueError(f'{label}: invalid binding kind {kind} in {name}')
            if pad != 0:
                raise ValueError(f'{label}: non-zero binding pad={pad} in {name}')
            if (kind == 1 and stride not in (32, 64)) or (kind == 2 and stride != 32) or (kind == 3 and stride != 16):
                raise ValueError(f'{label}: invalid binding stride={stride} for kind={kind} in {name}')
            sz = 16 if kind == 3 else 32
            align = 4 if kind == 3 else 32
            if offset % align != 0 or offset + sz > 1664:
                raise ValueError(f'{label}: binding offset={offset} sz={sz} invalid in {name}')
            for a0, a1 in intervals:
                if offset < a1 and a0 < offset + sz:
                    raise ValueError(f'{label}: overlapping bindings in {name}')
            intervals.append((offset, offset + sz))
        nrp_len = struct.unpack_from('<I', raw, pos)[0]
        pos += 4
        if nrp_len < 96 or pos + nrp_len > len(raw):
            raise ValueError(f'{label}: truncated NRP record in {name}')
        rec = raw[pos:pos + nrp_len]
        pos += nrp_len
        if rec[:8] not in (b'NRPAL001', b'NRPAL002'):
            raise ValueError(f'{label}: invalid NRP magic in {name}')
        spv_hash, code_hash = struct.unpack_from('<QQ', rec, 8)
        if spv_hashes_out is not None:
            spv_hashes_out[name] = spv_hash
        ninl, csz = struct.unpack_from('<II', rec, 68)
        if 96 + 8 * ninl + csz != nrp_len:
            raise ValueError(f'{label}: NRP size mismatch in {name}')
        code = rec[96 + 8 * ninl:]
        if fnv1a_64(code) != code_hash:
            raise ValueError(f'{label}: code hash mismatch in {name}')
    if pos != len(raw):
        raise ValueError(f'{label}: trailing {len(raw) - pos} bytes after {count} entries')
    return {
        'path': path.relative_to(ROOT).as_posix(),
        'size': len(raw),
        'sha256': sha256_bytes(raw),
        'kernels': count,
        'mach': f'0x{mach:02X}',
    }


def build_manifest() -> dict[str, object]:
    archives = {}
    for label, path, expect_rdna3 in ARCHIVES:
        archives[label] = validate_archive(label, path, expect_rdna3)
    return {
        'version': 1,
        'archives': archives,
        'inputs': collect_inputs(),
    }


def verify_spv_hashes() -> None:
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    import build_d3d12_outer_aco as builder

    for tool in (builder.SLANGC, builder.GLSLANG):
        if not tool.is_file():
            raise FileNotFoundError(f'missing shader compiler for --verify-spv: {tool}')

    with tempfile.TemporaryDirectory(prefix='check_outer_spv_') as td:
        tmp = pathlib.Path(td)
        renodx_glsl, il_list, w2_list, fr_list = builder.build_glsl_sets(tmp)
        for label, path, expect_rdna3 in ARCHIVES:
            align = 256 if expect_rdna3 else 128
            sub = tmp / label
            sub.mkdir()
            jobs = (builder.build_backend_hlsl(sub, align) +
                    builder.build_renodx_hlsl(sub, align) +
                    renodx_glsl +
                    builder.build_renodx_aux_hlsl(sub, align))
            compiled = {k: builder.fnv64(spv.read_bytes()) for k, spv, _ in jobs}
            for k, spv, _ in il_list:
                compiled[k] = builder.fnv64(spv.read_bytes())
            for k, spv, _ in w2_list + fr_list:
                compiled[k] = builder.fnv64(spv.read_bytes())
            recorded: dict[str, int] = {}
            validate_archive(label, path, expect_rdna3, recorded)
            if set(compiled) != set(recorded):
                raise ValueError(f'{label}: compiled kernel set differs from archive')
            diffs = [k for k in sorted(recorded) if compiled[k] != recorded[k]]
            if diffs:
                raise ValueError(
                    f'{label}: {len(diffs)} SPIR-V hash mismatch(es) (first: {diffs[0]} '
                    f'compiled=0x{compiled[diffs[0]]:016x} vs nra=0x{recorded[diffs[0]]:016x})'
                )


def main(argv: list[str]) -> int:
    write = '--write-manifest' in argv
    verify_spv = '--verify-spv' in argv or write
    current = build_manifest()
    if verify_spv:
        verify_spv_hashes()
    if write:
        MANIFEST.parent.mkdir(parents=True, exist_ok=True)
        MANIFEST.write_text(json.dumps(current, indent=2, sort_keys=True) + '\n', encoding='utf-8')
        print(f'wrote {MANIFEST.relative_to(ROOT)}')
        return 0
    if not MANIFEST.is_file():
        print(f'missing outer ACO manifest: {MANIFEST.relative_to(ROOT)}', file=sys.stderr)
        return 1
    recorded = json.loads(MANIFEST.read_text(encoding='utf-8'))
    errors: list[str] = []
    if recorded.get('archives') != current['archives']:
        errors.append('prebuilt .nra archive checksums/sizes differ from manifest')
    rec_in = recorded.get('inputs', {})
    cur_in = current['inputs']
    for k in sorted(set(rec_in) | set(cur_in)):
        if rec_in.get(k) != cur_in.get(k):
            errors.append(f'input changed since outer .nra capture: {k}')
    if errors:
        for e in errors[:10]:
            print(f'check_outer_aco: {e}', file=sys.stderr)
        print('Run windows/build/build_d3d12_outer_aco.py (with ACO capture tools) and --write-manifest to rebuild.',
              file=sys.stderr)
        return 1
    print('check_outer_aco: ok (rdna4 + rdna3 113 kernels in sync with shader sources)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
