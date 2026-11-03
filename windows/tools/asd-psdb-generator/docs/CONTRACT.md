# Contracts

## Command line

Global: `--out DIR` (required), `--root DIR` (only for default toolchain paths),
`--compiler-dir DIR`, `--plugin FILE`, `--shaders DIR`, `--dxc FILE`,
`--family SUBSTRING` (default `gfx1201`), app identity `--app-exe`,
`--app-name`, `--app-version`, `--app-engine`, `--app-engine-version`.

Modes (exactly one):

| mode | extra arguments | writes |
|---|---|---|
| `--m2-manifest` | `--aco-root`, `--release-root` | `manifest.tsv`, `manifest.json` |
| `--m2-build` | `--manifest`, `--aco-root` | `carrier.sodb`, `public-copy.psdb`, `native.psdb`, `keys/`, `build.jsonl`, `m2-build.json` |
| `--canary-seed` | optional `--canary-aco` | `canary.sodb`, `canary.psdb`, `native.psdb`, key/value dumps, `canary-seed.json` |
| `--m5-build` | `--manifest`, `--aco-root`, `--canary-sodb`, `--canary-psdb` | `carrier.sodb`, `public-copy.psdb`, `net-carriers.bin`, `keys/`, `build.jsonl`, `m5-build.json` |
| `--verify` | `--manifest`, `--sodb`, `--psdb`, `--carriers` | `canonical.jsonl`, `verify.json` |

Exit codes: 0 success, 1 failure, 2 manifest has unusable records
(`--m2-manifest` / `--m2-build`).

## Inputs

`--aco-root` is the release's `aco` directory; `--release-root` its parent
(`dlssnr-amd`). `--m2-manifest` indexes `aco/shaders`, `aco/int4/shaders` and
`release-root/experimental/shaders` by FNV-1a-64 of the `.spv` bytes, then reads
`aco/records/*.nrp` (`src=root`) and `aco/int4/records/*.nrp` (`src=int4`). Each
record's stored SPIR-V hash selects its shell; a record with no shell, a missing
literal `LocalSize`, a non-buffer-then-image set-0 layout, or a
`LocalSize`/`tidig_comp_cnt` mismatch gets a note and is skipped by the build.
`aco/rdna3` is never read.

`manifest.tsv` (tab separated, one line per record, tags 0..N-1 sorted by
`(src, file)`):
`tag file src shell spv note dims tidig tgid tgsize user push_bytes push_reg
grid_reg lds vgpr exec inlines bindings`.

## Databases

- `carrier.sodb` (`ID3D12StateObjectDatabase`): application desc + one
  `StorePipelineStateDesc` per group. Group names: `g_pbptr` (canary) and
  `m2-<tag>` (records). The SODB desc is opaque when read back; it is re-stored
  verbatim when the canary is copied into the final database.
- `public-copy.psdb` (`ID3D12CompilerCacheSession`, object code + metadata):
  per group one value key, values = wrapped ACO ELF (`.text` = 60-byte prefix
  for the canary, or the M2 prefix + record body; `.note` = AMDGPU metadata
  retargeted to the record) and the compiler metadata blob.
- `native.psdb`: the official compiler's values before substitution; template
  only, not registered.

Both databases are created for one adapter family + ABI
(`AMD_NAVI48:gfx1201`, ABI `9007201352883061` for the 0082 run) and one
application identity. Register them together, with the SODB as the component's
state object database and the PSDB as its compiler database.

## `net-carriers.bin`

Little endian:

```
"NRCAR001"
u32 rs_bytes; u8 rs[rs_bytes]           serialized root signature (b0 constants + u0 UAV)
u32 count
count x {
  u32 tag
  u32 dims[3]                            shell LocalSize
  u32 n; char stem[n]                    record file stem (fnv hex)
  u32 n; char src[n]                     "root" | "int4"
  u32 n; u8  dxil[n]                     carrier DXIL exactly as stored in the SODB desc
}
```

`--verify` checks count and tag/stem/src against `manifest.tsv`, then checks that
every group has a desc and exactly one non-empty key, object code and metadata.

## Canonical dump

`canonical.jsonl` lines, canary first:

```
{"group":"g_pbptr","key":"<sha256 of key bytes>","code":"<sha256>"}
{"group":"m2-<tag>","stem":"...","src":"...","key":"...","code":"...","meta":"..."}
```

`canonical_sha256` in `verify.json` is the SHA-256 of that file; equal values mean
two PSDB files hold the same logical content even though their bytes differ.
