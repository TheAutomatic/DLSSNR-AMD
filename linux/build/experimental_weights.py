#!/usr/bin/env python3
"""CC_SMALL_FP8_EXPERIMENTAL's weights: the small model's pack (CC_SMALL_FP8.bin, pack_model.py) with some of its
entries replaced by the fitted ones (same names, same sizes). The package carries only the difference, each changed
entry XOR the small model's, so the weights are made at install from the model the user's DLL holds.

    experimental_weights.py delta --small DIR --fitted DIR --list LIST --out FILE
        (package build) the difference of every file under --fitted from the same file under --small
    experimental_weights.py apply SMALL_PACK DELTA OUT
        (install) the small model's pack with the difference applied, written to OUT

Difference file (little-endian): b"NRXDIFF1", u32 count, then per entry u32 name length, name (UTF-8, as in the
pack), u64 size, the bytes (fitted XOR small).
An entry the pack does not have is ADDED (CC_SILVER_AARDWOLD_OPTIMIZED's levels in one pack: "performance/<name>" =
the Performance levels' own copy of <name>): its bytes XOR the pack's entry named by the rest after the first "/", as
the pack was before this difference. The result then has the added entries after the pack's own (index and data;
the pack's own entries move by the added index lines).
"""
import struct
import sys
from pathlib import Path

MAGIC = b"NRXDIFF1"


def xor(a, b):
    return (int.from_bytes(a, "little") ^ int.from_bytes(b, "little")).to_bytes(len(a), "little")


def delta(args):
    opts = dict(zip(args[::2], args[1::2]))
    small, fitted, out = Path(opts["--small"]), Path(opts["--fitted"]), Path(opts["--out"])
    names = [l.strip() for l in Path(opts["--list"]).read_text().splitlines() if l.strip() and not l.startswith("#")]
    changed = sorted(str(p.relative_to(fitted)) for p in fitted.rglob("*") if p.is_file() and not p.is_symlink())
    if not changed:
        sys.exit(f"no weight files under {fitted}")
    body = bytearray(MAGIC + struct.pack("<I", len(changed)))
    for n in changed:
        if n not in names:
            sys.exit(f"{n} is not in {opts['--list']}")
        a, b = (fitted / n).read_bytes(), (small / n).read_bytes()
        if len(a) != len(b):
            sys.exit(f"{n}: {len(a)} bytes, the small model's {len(b)}")
        body += struct.pack("<I", len(n.encode())) + n.encode() + struct.pack("<Q", len(a)) + xor(a, b)
    out.write_bytes(body)
    print(f"{out}: {len(changed)} entries, {len(body) / 2**20:.1f} MiB")


def apply(args):
    pack_path, delta_path, out = map(Path, args)
    pack = bytearray(pack_path.read_bytes())
    if pack[:8] != b"NRMODEL1":
        sys.exit(f"{pack_path} is not a model pack")
    (count_pack,) = struct.unpack_from("<I", pack, 8)
    index, at = {}, 16
    for _ in range(count_pack):
        (n,) = struct.unpack_from("<I", pack, at)
        name = pack[at + 4:at + 4 + n].decode()
        off, size = struct.unpack_from("<QQ", pack, at + 4 + n)
        index[name] = (off, size)
        at += 4 + n + 16
    d = delta_path.read_bytes()
    if d[:8] != MAGIC:
        sys.exit(f"{delta_path} is not a weights difference")
    (count,) = struct.unpack_from("<I", d, 8)
    at = 12
    original = bytes(pack)   # the added entries XOR the pack as it was
    added = []
    for _ in range(count):
        (n,) = struct.unpack_from("<I", d, at)
        name = d[at + 4:at + 4 + n].decode()
        (size,) = struct.unpack_from("<Q", d, at + 4 + n)
        at += 4 + n + 8
        if name not in index:
            base = name.split("/", 1)[1] if "/" in name else ""
            if index.get(base, (0, -1))[1] != size or any(a[0] == name for a in added):
                sys.exit(f"{name}: an added entry without {base} of {size} bytes in {pack_path}")
            off = index[base][0]
            added.append((name, xor(original[off:off + size], d[at:at + size])))
        else:
            if index[name][1] != size:
                sys.exit(f"{name}: not in {pack_path} with {size} bytes")
            off = index[name][0]
            pack[off:off + size] = xor(bytes(pack[off:off + size]), d[at:at + size])
        at += size
    if at != len(d):
        sys.exit(f"{delta_path}: {len(d) - at} bytes left over")
    if added:   # index lines for them, every offset moved by those lines, their bytes at the end
        grow = sum(4 + len(n.encode()) + 16 for n, _ in added)
        head = bytearray(pack[:16]); struct.pack_into("<I", head, 8, count_pack + len(added))
        lines, at = bytearray(), 16
        for _ in range(count_pack):
            (n,) = struct.unpack_from("<I", pack, at)
            off, size = struct.unpack_from("<QQ", pack, at + 4 + n)
            lines += pack[at:at + 4 + n] + struct.pack("<QQ", off + grow, size)
            at += 4 + n + 16
        end = len(pack) + grow
        for n, b in added:
            lines += struct.pack("<I", len(n.encode())) + n.encode() + struct.pack("<QQ", end, len(b))
            end += len(b)
        pack = head + lines + pack[at:] + b"".join(b for _, b in added)
    out.write_bytes(pack)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "delta" and len(sys.argv) == 10:
        delta(sys.argv[2:])
    elif len(sys.argv) == 5 and sys.argv[1] == "apply":
        apply(sys.argv[2:])
    else:
        sys.exit(__doc__)
