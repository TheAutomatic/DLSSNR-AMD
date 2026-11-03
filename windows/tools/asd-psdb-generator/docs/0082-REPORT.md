# 0.0.8.2 run (gfx1201)

Generated with this package on 2026-11-03, Windows x64, CPU only. The
databases under `artifacts/0082/` are the outputs of exactly the commands in
`artifacts/0082/COMMANDS.txt`.

## Inputs

| input | value |
|---|---|
| release | `DLSSNR-AMD-Vulkan-Windows-0.0.8.2-preview-x86_64.zip`, GitLab release `v0.0.8.2` asset |
| release zip sha256 | `85ca7911348a160b10c1a9456a8ef95332468d8402c11c4c4ea6593a7561989f` |
| SPIR-V used (`aco/shaders`, `aco/int4/shaders`, `experimental/shaders`) | 209 files, tree digest `0e7914fc3e0fc4b7025d0ff84f9fad51062a0a8f1e4f792bb4ba6772ebb53913` |
| ACO records (`aco/records`, `aco/int4/records`) | 176 files, tree digest `d0a79e1c7119b781ba91f2f1beca83f9f743a5c2cad84be30de8c02613b01a7c` |
| canary ACO body `canary/gfx1201_g_pbptr.bin` | `fb081569011274e43c272858b129169a81f2f9737469bc3e8be68b50cc440419` |
| `D3D12StateObjectCompiler.dll` (Agility 1.619.5) | `5ca0591e4a20b27d69734940fb61f61307a3cf5c4dbfdfca3d8545005a57a6a6` |
| `D3D12Core.dll` (Agility 1.619.5) | `eddf4cff4eda8162624b88694ad2adf4b09bc5aee6339191f39adf8ae48b41e7` |
| `amdxc_compiler.dll` (32.0.32015.7029) | `d2c3f76277bab61ffc26ad1583695c6ed1c6500d8b93b9ab2750bb0c347f5555` |
| `dxc.exe` (Windows SDK 10.0.26100.0) | `854d5f29dbbe25b584942855e65d673db3164f7f68e20fb784be9cd747d14814` |

Tree digest = SHA-256 over sorted `relpath<space>sha256` lines.

Target selected by the compiler: `AMD_NAVI48:gfx1201`, ABI `9007201352883061`
(`compiler_targets` in `m5-build.json` lists every family the DLL reports).

`aco/rdna3` was not read: the manifest walks only `records` and `int4/records`.
`src` values in the manifest are exactly `root` and `int4`.

## Outputs

| file | sha256 |
|---|---|
| `manifest.tsv` | `acba663687e37413f212107434e191e3ece23869e5604c5bb812efe11292ba57` |
| `manifest.json` | `1f6a0163ac65d56521d04b6d2db0374e1f4f92e4280e74deee63837d3feab171` |
| `carrier.sodb` | `949dbd9e541b6099dff2379345c31d25130f61986bdca64d54ff632069637554` |
| `public-copy.psdb` | `f5772df6cb3b9aa907405a6dcfcc69505df7b92ed814346df91c19568b02925a` (file bytes vary per build) |
| `net-carriers.bin` | `bb270cfc2133333532d2430ddae27cdb431c253642d0bfcc913d8bbea57a7bb9` |
| `native.psdb` | `75a68eb1494f446fb880f0f74db5ca5f96a4374843f6e2962afd12db489fa640` |
| `verify/canonical.jsonl` | `c3a042a7dedd2fa30f06025e04f5db0c69812b893bf73ef116e8c49af3139d23` |
| `canary/canary.sodb` | `716e20499625a1978a627e941c4c2a27ba9ef578f210439996a83dbbe38b2e44` |
| `canary/canary.psdb` | `29ca865348a5d86e578ca385833b3e3cbccec83743d5fbef8ff2a97aaf0d9d57` |

`m5-build.json`: 176 records total, 176 built, 0 skipped; canary key 1,
canary ACO ELF `fed086c12602cfef4eed426ab74a299745cc93fff4905c011b41d1648f75bc29`.
`verify.json`: `PASS_VERIFY`, 176/176 groups with a desc, one key and non-empty
object code + metadata.

Determinism observed:

- `carrier.sodb` and `net-carriers.bin` came out byte-identical on repeated runs.
- `public-copy.psdb` bytes differ per run (database header/ephemeral pages).
  `verify/canonical.jsonl` (and `canonical_sha256`) was byte-identical across
  four independent runs, i.e. the logical content is reproducible.
- `--canary-seed` reproduced the M1/M5 canary: key `77894a6e...`, ACO ELF
  `fed086c1...`.

## 0.0.8.1 → 0.0.8.2 record diff

Full data: `artifacts/0082/record-diff-0081-vs-0082.json`.

- 176 records both releases: 101 `root` + 75 `int4`; 76 distinct SPIR-V shell
  names in both, identical name sets.
- **12 replaced, 0 missing**: `temporal_pre_fp32`, `temporal_pre_fp32nh`,
  `temporal_pre_fp32nhhp`, `fswinimagepreds32`, `fswinimagepreds32nh`
  (each in root and int4 form; `root` has two distinct `fswinimagepreds32` and
  two distinct `temporal_pre_fp32` records).
- 164 records are byte-identical in content (same `.nrp` stem), but **114 of
  them carry a different tag** because tags are positional over the sorted
  record set: their carrier DXIL, PSDB group key (`m2-<tag>`) and wrapped ELF
  differ even though the ACO body is unchanged.
- Consequence for the ASI: take `carrier.sodb`, `public-copy.psdb` and
  `net-carriers.bin` from this one run and register them together; do not mix
  files from 0.0.8.1 and 0.0.8.2 or from two different runs.

## What was not done here

No D3D12 device, dispatch, GPU comparison or game run: the tool is the
generation half only. Execution/hit verification remains the runtime/ASI test.
