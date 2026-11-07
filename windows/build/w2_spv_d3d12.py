#!/usr/bin/env python3
# The wilsjo2 programs for the native D3D12 route (M5 v2): the very SPIR-V the Vulkan stage loads
# (wilsjo2_<mode>.spv), with one change - its 256-byte constant block moved from push constants to a read-only
# storage buffer at set 0, binding 6 (same std430 offsets, same members), so that SPIRV-Cross can write it as HLSL:
# a push block's float[16] at stride 4 has no HLSL cbuffer layout, a ByteAddressBuffer reads it at the same offsets.
# Nothing else in the program is touched.
#   w2_spv_d3d12.py <in.spv> <out.spv>
import struct, sys

PUSH, SSBO = 9, 12
OP_DECORATE, OP_MEMBER_DECORATE, OP_TYPE_POINTER, OP_VARIABLE = 71, 72, 32, 59
DEC_BINDING, DEC_SET, DEC_NONWRITABLE = 33, 34, 24

def main(src, dst):
    data = open(src, 'rb').read()
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    if words[0] != 0x07230203: raise SystemExit('not SPIR-V: ' + src)
    insts, i = [], 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xffff
        insts.append([op] + words[i + 1:i + n])
        i += n
    push_vars, changed_ptrs = [], 0
    for ins in insts:
        if ins[0] == OP_TYPE_POINTER and ins[2] == PUSH:
            ins[2] = SSBO; changed_ptrs += 1
        elif ins[0] == OP_VARIABLE and ins[3] == PUSH:
            ins[3] = SSBO; push_vars.append(ins[2])
    if len(push_vars) > 1: raise SystemExit('more than one push block: ' + src)
    out = []
    for ins in insts:
        out.append(ins)
        if push_vars and ins[0] == OP_DECORATE and ins[1] == push_vars[0]:
            pass
    if push_vars:
        # the decorations go with the others (after the last OpDecorate / OpMemberDecorate)
        last = max(k for k, ins in enumerate(out) if ins[0] in (OP_DECORATE, OP_MEMBER_DECORATE))
        v = push_vars[0]
        out[last + 1:last + 1] = [[OP_DECORATE, v, DEC_SET, 0], [OP_DECORATE, v, DEC_BINDING, 6],
                                  [OP_DECORATE, v, DEC_NONWRITABLE]]
    body = []
    for ins in out:
        body.append(((len(ins)) << 16) | ins[0])
        body.extend(ins[1:])
    open(dst, 'wb').write(struct.pack('<%dI' % (5 + len(body)), *(words[:5] + body)))
    print('%s: %d push block(s) -> storage buffer t6, %d pointer type(s)' % (src.split('/')[-1], len(push_vars), changed_ptrs))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
