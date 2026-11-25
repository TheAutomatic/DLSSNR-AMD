#!/usr/bin/env python3
"""Generate NRPAL002 gfx1030 (navi21) ACO records for all RDNA2 FP16 and INT8 kernels."""

from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CAPTURE = ROOT / "artifacts" / "rdna2_aco_capture"
CAPTURE_SRC = ROOT / "windows" / "build" / "rdna2_aco_capture.cpp"

ICD = "/home/kenny/work/openrr-mesa-263-comparison/build/src/amd/vulkan/radeon_devenv_icd.x86_64.json"
SHIM = "/home/kenny/work/openrr-mesa-263-comparison/build/src/amd/drm-shim/libamdgpu_noop_drm_shim.so"
MESA_LIB = "/home/kenny/src/mesa-deps/lib/x86_64-linux-gnu"

AMDGPU_GPU_ID = "navi21"
MACH_GFX1030 = 0x36


def fnv64(data: bytes) -> int:
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def nr_binding_layout(filename: str) -> tuple[int, list[str]]:
    if filename in ("g_fswinimagepreds32.spv", "g_fswinimagepreds32nh.spv"):
        return 6, ["c"]
    if filename in ("g_fswinimagepost32.spv", "g_fswinimagepost32_grade.spv"):
        return 5, ["s", "s", "c"]
    if filename in ("temporal_pre_fp32.spv", "temporal_pre_fp32nh.spv"):
        return 7, ["c", "c", "c", "c"]
    if filename == "temporal_pre_fp32nhhp.spv":
        return 7, ["c", "c", "c", "c", "s"]
    if filename in (
        "temporal_post_fp32.spv",
        "temporal_post_fp32_grade.spv",
        "temporal_post_fp32h16.spv",
        "temporal_post_fp32h16_grade.spv",
    ):
        return 6, ["s", "s", "c", "c", "c"]
    if filename in ("temporal_post_fp32hp.spv", "temporal_post_fp32hp_grade.spv"):
        return 6, ["s", "s", "c", "c", "c", "s"]
    if filename.startswith("g_fswinfusedup"):
        return 5, []
    if filename.startswith("g_fswin"):
        return 6, []
    if filename.startswith("g_gemm"):
        return 8, []
    if filename in ("g_ffwd3.spv", "g_ffwd3w.spv"):
        return 6, []
    if filename == "g_attn.spv":
        return 5, []
    if filename in ("g_decups.spv", "g_qkvnorm.spv", "g_vitattn.spv"):
        return 3, []
    if filename.startswith("g_upsblend"):
        return 2, []
    if filename in ("g_repack.spv", "g_upsview.spv", "g_noisefield.spv") or filename.startswith("g_pool"):
        return 1, []
    raise ValueError(f"unknown binding layout for {filename}")


def inspect_spv(spv_bytes: bytes) -> tuple[tuple[int, int, int], int, list[int]]:
    if len(spv_bytes) < 20 or len(spv_bytes) % 4 != 0:
        raise ValueError("invalid SPIR-V size")
    w = struct.unpack("<" + "I" * (len(spv_bytes) // 4), spv_bytes)
    if w[0] != 0x07230203:
        raise ValueError("invalid SPIR-V magic")
    types = {}
    vars_ = []
    dec_bind = {}
    dec_off = {}
    consts = {}
    dims = None
    dim_ids = None
    p = 5
    while p < len(w):
        wc = w[p] >> 16
        op = w[p] & 0xFFFF
        if wc == 0 or p + wc > len(w):
            raise ValueError("truncated SPIR-V instruction")
        a = w[p + 1 : p + wc]
        if op in (43, 50) and len(a) >= 3:
            consts[a[1]] = a[2]
        elif op == 16 and len(a) >= 5 and a[1] == 17:
            dims = (a[2], a[3], a[4])
        elif op == 331 and len(a) >= 5 and a[1] == 38:
            dim_ids = (a[2], a[3], a[4])
        elif op == 71 and len(a) >= 3 and a[1] == 33:
            dec_bind[a[0]] = a[2]
        elif op == 72 and len(a) >= 4 and a[2] == 35:
            dec_off[(a[0], a[1])] = a[3]
        elif op == 32 and len(a) >= 3:
            types[a[0]] = ("ptr", a[1], a[2])
        elif op == 30:
            types[a[0]] = ("struct", list(a[1:]))
        elif op == 28 and len(a) >= 3:
            types[a[0]] = ("array", a[1], a[2])
        elif op == 21:
            types[a[0]] = ("int", a[1])
        elif op == 22:
            types[a[0]] = ("float", a[1])
        elif op == 23:
            types[a[0]] = ("vec", a[1], a[2])
        elif op == 59 and len(a) >= 3:
            vars_.append((a[0], a[1], a[2]))
        p += wc
    if dim_ids is not None:
        dims = tuple(consts[x] for x in dim_ids)
    if dims is None or len(dims) != 3 or not all(x > 0 for x in dims):
        raise ValueError("missing literal workgroup size")

    def type_size(tid: int) -> int:
        t = types.get(tid, ("",))
        if t[0] in ("int", "float"):
            return max(4, t[1] // 8)
        if t[0] == "vec":
            return max(4, type_size(t[1]) * t[2])
        if t[0] == "array":
            return type_size(t[1]) * consts.get(t[2], 1)
        return 4

    push_bytes = 0
    for rtid, _vid, sc in vars_:
        if sc == 9:  # PushConstant
            st_id = types[rtid][2]
            mems = types[st_id][1]
            for idx, mtid in enumerate(mems):
                off = dec_off.get((st_id, idx), 0)
                push_bytes = max(push_bytes, off + type_size(mtid))
    push_bytes = ((push_bytes + 3) // 4) * 4
    aliases = sorted(set(b for b in dec_bind.values() if b >= 16))
    return dims, push_bytes, aliases


def adapt_code(code: bytes, other: bytes, exec_size: int, info: dict, other_info: dict, logs: tuple[str, str]) -> bytes:
    if exec_size <= 0 or exec_size > len(code) or exec_size % 4 or len(code) % 4:
        raise ValueError("invalid code sizes")
    if any(x[0] != 0xBF9F0000 for x in struct.iter_unpack("<I", code[exec_size:])):
        raise ValueError("constant data after executable code is unsupported")
    if code != other or info != other_info:
        raise ValueError("address32 sentinel captures differ in code or metadata")
    reports = []
    for log in logs:
        lines = re.findall(r"^ONR_M0 (.+)$", log, re.MULTILINE)
        if len(lines) != 1:
            raise ValueError(f"expected one ONR_M0 report, got {len(lines)}: {log}")
        report = {k: int(v) for k, v in (x.split("=") for x in lines[0].split())}
        if set(report) != {"subst", "writes", "constdata", "pc_relative"}:
            raise ValueError(f"invalid ONR_M0 report: {report}")
        if report["writes"] or report["constdata"] or report["pc_relative"]:
            raise ValueError(f"unsupported body state: {report}")
        reports.append(report)
    if reports[0] != reports[1]:
        raise ValueError("address32 substitution reports differ")
    return code


def pack_nrpal002(info: dict, code: bytes, spv_hash: int, mach: int) -> bytes:
    pairs = info["inline"]
    fields = [
        info[k]
        for k in (
            "rsrc1",
            "rsrc2",
            "rsrc3",
            "exec_bytes",
            "workgroup_size",
            "shared_bytes",
            "push_bytes",
            "user_sgprs",
        )
    ]
    header = struct.pack(
        "<8sQQ8I3i4I",
        b"NRPAL002",
        spv_hash,
        fnv64(code),
        *fields,
        info["set0"],
        info["push"],
        info["grid"],
        len(pairs),
        len(code),
        mach,
        info["scratch_bytes"],
    )
    assert len(header) == 84
    return header + b"\0" * 12 + b"".join(struct.pack("<ii", *p) for p in pairs) + code


def ensure_capture_tool() -> None:
    if CAPTURE.is_file() and CAPTURE.stat().st_mtime >= CAPTURE_SRC.stat().st_mtime:
        return
    CAPTURE.parent.mkdir(parents=True, exist_ok=True)
    candidates = [
        os.environ.get("NR_VULKAN_HEADERS"),
        str(ROOT / "toolchain" / "Vulkan-Headers"),
        "/mnt/c/Users/Kenny/Downloads/DLSSNR-AMD-mochizuki/artifacts/ref/Vulkan-Loader/external/Windows/Release/AMD64/Vulkan-Headers",
    ]
    inc = None
    for c in candidates:
        if c and Path(c, "include", "vulkan", "vulkan_core.h").is_file():
            inc = str(Path(c, "include"))
            break
    if not inc:
        raise FileNotFoundError("Vulkan-Headers include dir not found (set NR_VULKAN_HEADERS)")
    cmd = ["g++", "-O2", "-std=c++17", f"-I{inc}", str(CAPTURE_SRC), "-o", str(CAPTURE), "-lvulkan"]
    res = subprocess.run(cmd, text=True, capture_output=True)
    if res.returncode != 0:
        raise RuntimeError(f"failed to compile outer_aco_capture:\n{res.stdout}\n{res.stderr}")


def newest_shader_mtime() -> float:
    return max(p.stat().st_mtime for p in (ROOT / "linux" / "shaders" / "rdna2").rglob("*") if p.is_file())


def ensure_spvs(out_dir: Path, int8: bool, aco_dot2: bool = False) -> None:
    stamp = out_dir / "shader-constants.txt"
    if stamp.is_file() and stamp.stat().st_mtime >= newest_shader_mtime():
        return
    cmd = [sys.executable, str(ROOT / "windows" / "build" / "build_network.py"), "rdna2", "--tree", "linux", "--no-quad"]
    if int8:
        cmd.append("--int8")
    if aco_dot2:
        cmd += ["--define", "NR_ACO_DOT2=1"]
    cmd += ["--out", str(out_dir)]
    res = subprocess.run(cmd, text=True, capture_output=True)
    if res.returncode != 0:
        raise RuntimeError(f"build_network.py failed:\n{res.stdout}\n{res.stderr}")


def verify_amdvlk_layout(layout_text: str, ssbos: int, images: list[str], aliases: list[int]) -> None:
    blocks = layout_text.split("set bytes=")
    if len(blocks) < 2:
        raise ValueError("missing descriptor layout output")
    last = blocks[-1].splitlines()
    hdr = re.match(r"^(\d+)\s+bindings=(\d+)$", last[0].strip())
    if not hdr:
        raise ValueError(f"unexpected layout header: {last[0]!r}")
    set_bytes = int(hdr.group(1))
    parsed: dict[int, tuple[int, int, int, int]] = {}
    for line in last[1:]:
        line = line.strip()
        if not line:
            continue
        m = re.match(
            r"^binding=(\d+)\s+type=(\d+)\s+count=(\d+)\s+offset=(\d+)\s+stride=(\d+)\s+image=\d+\s+sampler=(\d+)$",
            line,
        )
        if not m:
            raise ValueError(f"unexpected binding line: {line!r}")
        b, vk_type, count, offset, stride, sampler = map(int, m.groups())
        if count == 0 or stride == 0:
            continue
        if count != 1:
            raise ValueError(f"binding {b}: unexpected count={count}")
        parsed[b] = (vk_type, offset, stride, sampler)

    expected_order: list[tuple[int, int, int, int]] = []
    for b in range(ssbos):
        expected_order.append((b, 7, 16, 0))
    for i, kind in enumerate(images):
        if kind == "c":
            expected_order.append((ssbos + i, 1, 48, 32))
        elif kind == "s":
            expected_order.append((ssbos + i, 3, 32, 0))
        else:
            raise ValueError(f"unknown image kind: {kind}")
    for ab in aliases:
        expected_order.append((ab, 7, 16, 0))

    if sorted(parsed) != sorted(b for b, *_ in expected_order):
        raise RuntimeError(f"layout bindings mismatch: got {sorted(parsed)}, expected {[b for b, *_ in expected_order]}")
    running_off = 0
    for b, exp_type, exp_stride, exp_sampler in expected_order:
        got_type, got_off, got_stride, got_sampler = parsed[b]
        if (got_type, got_off, got_stride, got_sampler) != (exp_type, running_off, exp_stride, exp_sampler):
            raise RuntimeError(
                f"binding {b} AMDVLK layout mismatch: got {(got_type, got_off, got_stride, got_sampler)}, "
                f"expected {(exp_type, running_off, exp_stride, exp_sampler)}"
            )
        running_off += exp_stride
    if set_bytes != running_off:
        raise RuntimeError(f"descriptor set total size mismatch: got {set_bytes}, expected {running_off}")


def capture_one(label: str, portable_spv: Path, aco_spv: Path, cap_dir: Path) -> tuple[str, int, dict, bytes]:
    portable_bytes = portable_spv.read_bytes()
    aco_bytes = aco_spv.read_bytes()
    wg, push_bytes, aliases = inspect_spv(portable_bytes)
    aco_meta = inspect_spv(aco_bytes)
    if (wg, push_bytes, aliases) != aco_meta:
        raise RuntimeError(f"{label}/{portable_spv.name}: portable vs ACO SPIR-V interface mismatch: {(wg, push_bytes, aliases)} vs {aco_meta}")
    spv_hash = fnv64(portable_bytes)
    ssbos, images = nr_binding_layout(aco_spv.name)

    base_env = dict(
        os.environ,
        VK_ICD_FILENAMES=ICD,
        LD_PRELOAD=SHIM,
        LD_LIBRARY_PATH=MESA_LIB,
        AMDGPU_GPU_ID=AMDGPU_GPU_ID,
        MESA_SHADER_CACHE_DISABLE="true",
        ONR_ACO_PRECISION="local",
    )
    if images:
        base_env["ONR_ACO_AMDVLK_LAYOUT"] = "1"
        base_env["RADV_DEBUG"] = "nofmask"
    else:
        base_env.pop("ONR_ACO_AMDVLK_LAYOUT", None)
    for k in (
        "ONR_ACO_VECTOR_LOADS",
        "ONR_ACO_CMAT_ADDRESS",
        "ONR_ACO_CU_MODE",
        "ONR_ACO_WGP_MODE",
        "ONR_ACO_SAMPLE_WAIT_ZERO",
        "ONR_ACO_SHARED_CMAT",
    ):
        base_env.pop(k, None)

    safe = f"{label}_{aco_spv.stem}"
    captures = []
    for suffix, sentinel in (("a", "0x13579bdf"), ("b", "0x2468ace1")):
        prefix = cap_dir / f"{safe}_{suffix}"
        for ext in (".code", ".json", ".layouts"):
            prefix.with_suffix(ext).unlink(missing_ok=True)
        env = dict(base_env, ONR_ACO_CAPTURE=str(prefix), ONR_ACO_ADDR32_HI=sentinel)
        cmd = [str(CAPTURE), str(aco_spv), "--push", str(push_bytes), "--nr-ssbos", str(ssbos)]
        if images:
            cmd += ["--nr-images", ",".join(images)]
        if aliases:
            cmd += ["--alias-bindings", ",".join(str(x) for x in aliases)]
        res = subprocess.run(cmd, env=env, text=True, capture_output=True)
        log = res.stdout + res.stderr
        if res.returncode != 0:
            raise RuntimeError(f"{label}/{aco_spv.name}: capture failed ({res.returncode}):\n{log}")
        if images and suffix == "a":
            verify_amdvlk_layout(prefix.with_suffix(".layouts").read_text(), ssbos, images, aliases)
        info = json.loads(prefix.with_suffix(".json").read_text())
        code = prefix.with_suffix(".code").read_bytes()
        captures.append((info, code, log))

    info, code, log = captures[0]
    other_info, other_code, other_log = captures[1]
    if info["scratch_bytes"] != 0 or (info["rsrc2"] & 1) != 0:
        raise RuntimeError(
            f"{label}/{aco_spv.name}: unexpected register spill (scratch={info['scratch_bytes']} rsrc2=0x{info['rsrc2']:x})"
        )
    if info["wave_size"] != 32:
        raise RuntimeError(f"{label}/{aco_spv.name}: unexpected wave_size {info['wave_size']}")
    if info["push_bytes"] > push_bytes or info["push_bytes"] > 128 or info["push_bytes"] % 4 != 0:
        raise RuntimeError(f"{label}/{aco_spv.name}: push_bytes {info['push_bytes']} vs {push_bytes}")
    if info["set0"] < 0:
        raise RuntimeError(f"{label}/{aco_spv.name}: unexpected set0 SGPR {info['set0']}")
    expected_usgprs = (
        (2 if info["set0"] == 2 else 0)
        + 1
        + (1 if info["push"] >= 0 else 0)
        + len(info["inline"])
        + (3 if info["grid"] >= 0 else 0)
    )
    if info["user_sgprs"] != expected_usgprs:
        raise RuntimeError(
            f"{label}/{aco_spv.name}: unexpected user_sgprs={info['user_sgprs']} vs expected={expected_usgprs} "
            f"(set0={info['set0']} push={info['push']} grid={info['grid']} inlines={len(info['inline'])})"
        )
    if wg[0] * wg[1] * wg[2] != info["workgroup_size"]:
        raise RuntimeError(f"{label}/{aco_spv.name}: workgroup mismatch {wg} vs {info['workgroup_size']}")
    code = adapt_code(code, other_code, info["exec_bytes"], info, other_info, (log, other_log))
    nrp = pack_nrpal002(info, code, spv_hash, MACH_GFX1030)
    return f"{label}/{portable_spv.name}", spv_hash, info, nrp


def main() -> int:
    ensure_capture_tool()
    rdna2_build = ROOT / "build" / "windows" / "rdna2"
    fp16_dir = rdna2_build / "network"
    int8_dir = rdna2_build / "network-int8"
    fp16_aco = rdna2_build / "network-aco-dot2"
    int8_aco = rdna2_build / "network-int8-aco-dot2"
    ensure_spvs(fp16_dir, False, aco_dot2=False)
    ensure_spvs(int8_dir, True, aco_dot2=False)
    ensure_spvs(fp16_aco, False, aco_dot2=True)
    ensure_spvs(int8_aco, True, aco_dot2=True)

    out_dir = ROOT / "windows" / "data" / "aco" / "rdna2" / "records"
    out_dir.mkdir(parents=True, exist_ok=True)

    jobs: dict[int, tuple[str, Path, Path]] = {}
    for label, pdir, adir in (("fp16", fp16_dir, fp16_aco), ("int8", int8_dir, int8_aco)):
        spvs = sorted(pdir.glob("g_*.spv")) + sorted((pdir / "temporal").glob("temporal_*.spv"))
        for spv in spvs:
            rel = spv.relative_to(pdir)
            h = fnv64(spv.read_bytes())
            if h not in jobs:
                jobs[h] = (label, spv, adir / rel)

    results: dict[int, tuple[str, dict, bytes]] = {}
    with tempfile.TemporaryDirectory(prefix="rdna2_aco_") as td:
        cap_dir = Path(td)
        workers = min(8, os.cpu_count() or 4)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futs = [pool.submit(capture_one, label, pspv, aspv, cap_dir) for label, pspv, aspv in jobs.values()]
            for fut in as_completed(futs):
                name, spv_hash, info, nrp = fut.result()
                results[spv_hash] = (name, info, nrp)

    for old in out_dir.glob("*.nrp"):
        old.unlink()
    for spv_hash in sorted(results):
        name, info, nrp = results[spv_hash]
        rec_path = out_dir / f"{spv_hash:016x}.nrp"
        rec_path.write_bytes(nrp)
        print(
            f"[rdna2] {name:38s} -> {rec_path.name} "
            f"(vgpr={info['vgprs']:3d} lds={info['shared_bytes']:5d} push={info['push_bytes']:3d} "
            f"code={info['exec_bytes']:6d} scratch={info['scratch_bytes']})",
            flush=True,
        )
    print(f"Wrote {len(results)} NRPAL002 gfx1030 records to {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
