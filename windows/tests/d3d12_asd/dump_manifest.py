#!/usr/bin/env python3
"""Write MANIFEST.md for a dump directory: every .bin with its size and sha256, plus the semantics known here.

  dump_manifest.py <dir>
"""
import hashlib
import os
import sys

SEMANTICS = {
    "asd_noise.bin": "network noise field, wgt[noise.off*4 .. +w*h*8], f16x4, ASD route",
    "vulkan_noise.bin": "network noise field, wgt[noise.off*4 .. +w*h*8], f16x4, reference route",
    "asd_wgt.bin": "weight blob wgt[0 .. weights_end), raw bytes, ASD route",
    "vulkan_wgt.bin": "weight blob wgt[0 .. weights_end), raw bytes, reference route",
    "asd_pre.bin": "act region captured after the temporal pre dispatch (offset in the run log), ASD route",
    "vulkan_pre.bin": "act region captured after the temporal pre dispatch (same offset), reference route",
    "asd_kern.bin": "act region captured after the first fswin32nh (NR_NET_KERN_DUMP/REGION_*), ASD route",
    "vulkan_kern.bin": "act region captured after the first fswin32nh (NR_RUNTIME_KERN_DUMP/REGION_*), reference route",
}

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    d = sys.argv[1]
    rows = []
    for name in sorted(os.listdir(d)):
        if not name.endswith(".bin"):
            continue
        p = os.path.join(d, name)
        h = hashlib.sha256()
        with open(p, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        rows.append((name, os.path.getsize(p), h.hexdigest(), SEMANTICS.get(name, "unclassified")))
    out = os.path.join(d, "MANIFEST.md")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Dump manifest\n\n| file | bytes | sha256 | semantics |\n|---|---|---|---|\n")
        for name, size, sha, sem in rows:
            f.write(f"| {name} | {size} | `{sha}` | {sem} |\n")
    print(out)

if __name__ == "__main__":
    main()
