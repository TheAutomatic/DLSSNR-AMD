#!/usr/bin/env python3
"""One outer kernel's ACO record from a capture_dump.so dump (build_d3d12_outer_aco.py): RADV's ABI read from the
serialized shader, the machine code checked and adapted for the D3D12 PAL ELF, the descriptor offsets from the SPIR-V.

record(dump_dir, key, name, tmp, mcpu) -> (info, code)
    info: rsrc1/2/3, exec_bytes, workgroup_size, shared_bytes, push_bytes, user_sgprs, set0 / push / grid SGPRs
          (-1: none), inline [[sgpr, push dword]], scratch_bytes (per wave) - build_d3d12_outer_aco.py packs them.
    code: the machine code with RADV's 32-bit address high words (s_movk_i32 sN, 0x8000, each the high half of an
          s_load base) turned into s_mov_b32 sN, m0 (same 4 bytes): the ELF's prologue puts the D3D12 driver's high
          word into m0.
bindings(spv) -> [(binding, kind, offset, stride)]: where RADV's descriptor set layout puts each set-0 binding.
"""
import re
import struct
import subprocess

# radv_userdata_locations: descriptor_sets[32] {int8 sgpr_idx, uint8 num_sgprs}, then the shader data entries in this
# order; found by the 31 unused set entries after set 0 (0xff, 0).
UNUSED_SETS = bytes([0xff, 0x00]) * 31
UD = ['SCRATCH', 'PUSH_PTR', 'PUSH_INLINE', 'INDIRECT_DESC', 'VIEW', 'SO_BUF', 'SO_STATE', 'TASK', 'NGGC_S', 'NGGC_VP',
      'NGG_LDS', 'NGG_ST', 'NGG_Q', 'ESGS', 'VRS', 'TASK_RING', 'NEXT_PC', 'EPILOG', 'DYN_DESC', 'DYN_DESC_OFF', 'CS_GRID']
S_CODE_END = 0xBF9F0000
RE_LINE = re.compile(r'^\s+(\S+)\s*(.*?)\s*//\s*([0-9A-F]+):')
RE_SREG = re.compile(r'\bs\[(\d+):(\d+)\]|\bs(\d+)\b')
RE_VREG = re.compile(r'\bv\[(\d+):(\d+)\]|\bv(\d+)\b')


class RecordError(Exception):
    pass


def raw_elf(code):
    """Raw machine code in a minimal ELF, for llvm-objdump (--mcpu decides the decoding)."""
    shstr = b'\0.text\0.shstrtab\0'
    text_off = 0x100
    shstr_off = text_off + len(code)
    sh_off = (shstr_off + len(shstr) + 7) & ~7
    eh = struct.pack('<4sBBBBB7sHHIQQQIHHHHHH', b'\x7fELF', 2, 1, 1, 65, 0, b'\0' * 7,
                     1, 224, 1, 0, 0, sh_off, 0x4E, 64, 0, 0, 64, 3, 2)
    text = struct.pack('<IIQQQQIIQQ', 1, 1, 6, 0, text_off, len(code), 0, 0, 256, 0)
    strs = struct.pack('<IIQQQQIIQQ', 7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    out = bytearray(eh.ljust(text_off, b'\0')) + code + shstr
    out += b'\0' * (sh_off - len(out)) + b'\0' * 64 + text + strs
    return bytes(out)


def disassemble(code, tmp, mcpu):
    tmp.write_bytes(raw_elf(code))
    return subprocess.run(['llvm-objdump-21', '-d', f'--mcpu={mcpu}', str(tmp)], capture_output=True, text=True,
                          check=True).stdout


def sregs(text):
    out = []
    for m in RE_SREG.finditer(text):
        if m.group(3) is not None:
            out.append(int(m.group(3)))
        else:
            out += list(range(int(m.group(1)), int(m.group(2)) + 1))
    return out


def scan(asm):
    """The address high-word sites and everything that would tie the code to RADV beyond them."""
    ins = [(int(m.group(3), 16), m.group(1), m.group(2)) for m in map(RE_LINE.match, asm.splitlines()) if m]
    r = {'hi_sites': [], 'hi_bad': [], 'getpc': [], 'odd': []}
    for i, (off, op, args) in enumerate(ins):
        parts = [p.strip() for p in args.split(',')]
        nodst = op.startswith(('s_cmp', 's_bitcmp', 'buffer_store', 'global_store', 's_cbranch', 's_wait', 's_delay',
                               'ds_write', 'ds_store'))
        dst = sregs(parts[0]) if parts and not nodst else []
        if op == 's_movk_i32' and parts[1].lower() in ('0x8000', '-32768'):
            n = dst[0]
            use = None
            for off2, op2, args2 in ins[i + 1:i + 40]:
                if f's[{n - 1}:{n}]' in args2 and op2.startswith('s_load'):
                    use = (off2, op2)
                    break
                if n in sregs(args2.split(',')[0]) and not op2.startswith('s_load'):
                    break
            (r['hi_sites'] if use else r['hi_bad']).append({'off': off, 'sgpr': n, 'use': use})
        if '0xffff8000' in args.lower() or (op != 's_movk_i32' and re.search(r'\b-32768\b', args)):
            r['odd'].append((off, op, args))
        if op == 's_getpc_b64':
            r['getpc'].append((off, args))
        if op.startswith(('s_setreg', 's_getreg', 's_sendmsg', 'scratch_', 's_trap', 's_sethalt')):
            r['odd'].append((off, op, args))
    return r


def allowed(o, gfx11):
    if o[1] == 's_sendmsg' and 'DEALLOC_VGPRS' in o[2]:
        return True
    if o[1] == 's_setreg_imm32_b32' and re.fullmatch(r'hwreg\(HW_REG_MODE, 23, 1\), [01]', o[2]):
        return True   # FP16_OVFL around the saturating fp8 conversions
    if gfx11:
        if o[1].startswith(('scratch_load', 'scratch_store')):
            return True   # spills: the record carries the scratch size
        # LLVM 21 does not decode some gfx11 VOP3P words (.long) and resyncs inside the next instruction: a "write" of
        # the read-only HW_ID1 is such a misdecode, not code
        if o[1].startswith(('s_setreg', 's_getreg')) and re.search(r'hwreg\((HW_REG_HW_ID1|\d+),', o[2]):
            return True
    return False


def check_m0(name, asm, sites, gfx11):
    if not gfx11:
        if re.search(r'\bm0\b', asm):
            raise RecordError(f'{name}: uses m0')
        return
    # gfx11: ACO may set m0 itself (s_mov_b32 m0, ...). The patched s_mov_b32 sN, m0 read the prologue's m0, so every
    # one of them has to run before the first write of m0, and nothing after that write may branch back before them.
    lines = [(int(m.group(2), 16), m.group(1)) for m in re.finditer(r'^\s+(\S.*?)\s*//\s*([0-9A-F]+):', asm, re.M)]
    writes = [o for o, t in lines if re.match(r's_\w+\s+m0\b', t)]
    reads = [o for o, t in lines if re.search(r'\bm0\b', t) and not re.match(r's_\w+\s+m0\b', t)]
    if writes:
        first = min(writes)
        last_site = max([x['off'] for x in sites], default=-1)
        if reads or last_site > first:
            raise RecordError(f'{name}: uses m0 (reads {reads[:3]}, first write {first:#x}, last site {last_site:#x})')
        for o, t in lines:
            m = re.match(r's_(cbranch_\w+|branch)\s+(-?\d+)', t)
            if o > first and m and o + 4 + 4 * int(m.group(2)) <= last_site:
                raise RecordError(f'{name}: branch at {o:#x} back before the m0 reads')


def record(dump, key, name, tmp, mcpu):
    gfx11 = mcpu.startswith('gfx11')
    blob = (dump / f'{key}.blob').read_bytes()
    exec_size = int(re.search(r'\bexec (\d+)', (dump / f'{key}.info').read_text()).group(1))
    code = bytearray((dump / f'{key}.code').read_bytes())
    tail = code[exec_size:]
    if len(tail) % 4 or any(struct.unpack_from('<I', tail, i)[0] != S_CODE_END for i in range(0, len(tail), 4)):
        raise RecordError(f'{name}: data after the code')

    loc = blob.find(UNUSED_SETS) - 2
    if loc < 48:
        raise RecordError(f'{name}: radv_userdata_locations not found')
    # radv_shader_info just before: workgroup_size, nir_shared_size, inline_push_constant_mask, push_constant_size,
    # desc_set_used_mask
    wg, shared, mask, psize, set_mask = struct.unpack_from('<IIQII', blob, loc - 48)
    if set_mask not in (0, 1):
        raise RecordError(f'{name}: descriptor sets {set_mask:#x}, only set 0 handled')
    set0 = blob[loc:loc + 2]
    sd = blob[loc + 64 + 4:loc + 64 + 4 + 2 * len(UD)]
    ud = {nm: (sd[2 * j], sd[2 * j + 1]) for j, nm in enumerate(UD) if sd[2 * j] != 0xff}
    unknown = set(ud) - {'PUSH_PTR', 'PUSH_INLINE', 'CS_GRID'}
    if unknown:
        raise RecordError(f'{name}: unhandled user SGPRs {unknown}')
    rsrc1, rsrc2, rsrc3 = struct.unpack_from('<3I', blob, 48)
    scratch = struct.unpack_from('<I', blob, 40)[0]   # RADV's config: scratch bytes a wave
    if bool(rsrc2 & 1) != bool(scratch):
        raise RecordError(f'{name}: scratch enable {rsrc2 & 1} but {scratch} bytes')

    bits = [j for j in range(64) if mask >> j & 1]
    n_inline = ud.get('PUSH_INLINE', (0, 0))[1]
    inline = bits[:n_inline]   # allocate_inline_push_consts keeps the lowest set bits
    if len(inline) != n_inline:
        raise RecordError(f'{name}: inline mask {mask:#x} vs {n_inline} SGPRs')

    asm = disassemble(bytes(code[:exec_size]), tmp, mcpu)
    s = scan(asm)
    if s['hi_bad'] or s['getpc'] or [o for o in s['odd'] if not allowed(o, gfx11)]:
        raise RecordError(f'{name}: unchecked sites {s["hi_bad"]} {s["getpc"]} {s["odd"]}')
    check_m0(name, asm, s['hi_sites'], gfx11)
    if scratch and not gfx11:
        raise RecordError(f'{name}: scratch on gfx12')
    for site in s['hi_sites']:
        off, n = site['off'], site['sgpr']
        if struct.unpack_from('<I', code, off)[0] != (0xB0008000 | n << 16):
            raise RecordError(f'{name}: {off:#x} is not s_movk_i32 s{n}, 0x8000')
        struct.pack_into('<I', code, off, 0xBE80007D | n << 16)   # s_mov_b32 sN, m0
    if set_mask and set0[1] != 1:
        raise RecordError(f'{name}: set 0 pointer in {set0[1]} SGPRs')

    info = {
        'rsrc1': rsrc1, 'rsrc2': rsrc2, 'rsrc3': rsrc3, 'exec_bytes': exec_size, 'workgroup_size': wg,
        'shared_bytes': shared, 'push_bytes': psize, 'user_sgprs': (rsrc2 >> 1) & 0x1f,
        'set0': set0[0] if set_mask else -1,
        'push': ud['PUSH_PTR'][0] if 'PUSH_PTR' in ud else -1,
        'grid': ud['CS_GRID'][0] if 'CS_GRID' in ud else -1,
        'inline': [[ud['PUSH_INLINE'][0] + i, dw] for i, dw in enumerate(inline)],
        'scratch_bytes': scratch,
    }
    return info, bytes(code)


# RADV's descriptor sizes (radv_descriptor_set.c): sampled / storage image 32 bytes, combined image + sampler 64,
# buffer 16, a sampler with only immutable samplers none; images align to 32, buffers and samplers to 16.
SIZE = {'srv': (32, 32), 'uav': (32, 32), 'comb': (64, 32), 'buf': (16, 16), 'sampler': (0, 16)}
KIND = {'srv': 1, 'uav': 2, 'comb': 1, 'buf': 3}   # build_d3d12_outer_aco.py's KIND_SRV_TEX / KIND_UAV_TEX / KIND_BUF


def reflect(spv):
    w = struct.unpack('<%dI' % (len(spv) // 4), spv)
    deco_b, deco_s, types, ptrs, vars_ = {}, {}, {}, {}, []
    p = 5
    while p < len(w):
        n, op = w[p] >> 16, w[p] & 0xffff
        a = w[p + 1:p + n]
        if op == 71 and a[1] == 33: deco_b[a[0]] = a[2]          # OpDecorate Binding
        elif op == 71 and a[1] == 34: deco_s[a[0]] = a[2]        # OpDecorate DescriptorSet
        elif op == 25: types[a[0]] = ('image', a[6])             # OpTypeImage: Sampled 1 (SRV) / 2 (storage)
        elif op == 26: types[a[0]] = ('sampler',)
        elif op == 27: types[a[0]] = ('sampledimage',)
        elif op in (28, 29): types[a[0]] = ('array', a[1])
        elif op == 30: types[a[0]] = ('struct',)
        elif op == 32: ptrs[a[0]] = a[2]                         # OpTypePointer -> pointee
        elif op == 59: vars_.append((a[0], a[1]))                # OpVariable type, id
        p += n
    out = {}
    for ty, vid in vars_:
        if vid not in deco_b or deco_s.get(vid, 0) != 0:
            continue
        t = ptrs[ty]
        while types.get(t, ('',))[0] == 'array':
            t = types[t][1]
        k = types.get(t, ('',))
        kinds = {'sampledimage': 'comb', 'sampler': 'sampler', 'struct': 'buf'}
        if k[0] == 'image':
            out[deco_b[vid]] = 'srv' if k[1] == 1 else 'uav'
        elif k[0] in kinds:
            out[deco_b[vid]] = kinds[k[0]]
        else:
            raise RecordError(f'binding {deco_b[vid]}: type {k}')
    return out


def bindings(spv):
    # radv_CreateDescriptorSetLayout: two passes over the bindings in binding order; the first takes those whose
    # alignment is (the last binding's alignment == 32 ? 16 : 32), the second the rest
    binds = sorted(reflect(spv).items())
    if not binds:
        return []
    first = 16 if SIZE[binds[-1][1]][1] == 32 else 32
    off, rows = 0, []
    for pass_ in (0, 1):
        for b, kind in binds:
            size, align = SIZE[kind]
            if (align == first) != (pass_ == 0):
                continue
            off = (off + align - 1) // align * align
            if size:
                rows.append((b, KIND[kind], off, size))
            off += size
    return sorted(rows)
