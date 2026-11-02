#!/usr/bin/env python3
"""Compare two complete raw frame dumps (R10G10B10A2, RGBA16F or RGBA32F).

Both files must have exactly the byte count for the selected format: a short file is a failure, never a truncation to the shorter one
(a missing half of a frame would otherwise read as agreement). Reports per channel max/mean/RMSE/quantiles, the
share of differing pixels, and rejects non-finite values in float dumps.

  compare_dumps.py A.bin B.bin <width> <height> [r10g10b10a2|rgba16f|rgba32f]
"""
import sys
import numpy as np

def channels(path, w, h, kind):
    raw = np.fromfile(path, dtype=np.uint8)
    sizes = {"r10g10b10a2": 4, "rgba16f": 8, "rgba32f": 16}
    if kind not in sizes:
        sys.exit(f"FAIL: unknown format {kind!r}; expected one of {', '.join(sizes)}")
    per_px = sizes[kind]
    want = w * h * per_px
    if raw.size != want:
        sys.exit(f"FAIL {path}: {raw.size} bytes, expected exactly {want} ({w}x{h}x{per_px})")
    if kind == "r10g10b10a2":
        v = raw.view(np.uint32)
        return {n: ((v >> s) & ((1 << b) - 1)).astype(np.float64) for n, s, b in
                (("R", 0, 10), ("G", 10, 10), ("B", 20, 10), ("A", 30, 2))}, 1023.0, False
    a = raw.view(np.dtype("<f4" if kind == "rgba32f" else "<f2"))
    if a.size != w * h * 4:
        sys.exit(f"FAIL {path}: {a.size} values, expected {w*h*4} (= {w}x{h}x4 channels)")
    a = a.reshape(-1, 4).astype(np.float32)
    if not np.isfinite(a).all():
        sys.exit(f"FAIL {path}: {np.count_nonzero(~np.isfinite(a))} non-finite values")
    return {n: a[:, i].astype(np.float64) for i, n in enumerate("RGBA")}, None, True

def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    pa, pb, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    kind = sys.argv[5] if len(sys.argv) > 5 else "r10g10b10a2"
    ca, full, is_float = channels(pa, w, h, kind)
    cb, full_b, is_float_b = channels(pb, w, h, kind)
    if is_float != is_float_b or full != full_b:
        sys.exit("FAIL: the two dumps are not the same kind")
    n = w * h
    print(f"{pa} vs {pb}: {w}x{h}, {kind}")
    for name in "RGBA":
        d = ca[name] - cb[name]
        ad = np.abs(d)
        rms = float(np.sqrt(np.mean(d * d)))
        q = np.percentile(ad, [50, 90, 99])
        print(f"  {name}: max {ad.max():.8g} mean {ad.mean():.8g} rmse {rms:.8g} "
              f"p50 {q[0]:.8g} p90 {q[1]:.8g} p99 {q[2]:.8g} diff {(ad > 0).mean()*100:6.2f}%")
    # One number for "how different", in units of the format's full scale.
    worst = max(float(np.abs(ca[k] - cb[k]).max()) for k in "RGB")
    if full:
        print(f"  worst channel max {worst:.3f} of full scale {full:.0f} = {100.0 * worst / full:.2f}%")
    else:
        print(f"  worst channel max {worst:.6f} (float dump: absolute units; "
              f"range {float(ca['R'].min()):.4f}..{float(ca['R'].max()):.4f})")

if __name__ == "__main__":
    main()
