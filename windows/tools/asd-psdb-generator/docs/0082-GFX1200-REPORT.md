# 0.0.8.2 gfx1200 (AMD_NAVI44) PSDB handoff

Generated on Windows x64 on 2026-11-03, using the same 0.0.8.2 SPIR-V/ACO inputs and compiler versions as `0082-REPORT.md`. No GPU device or dispatch was created. The original generator source is ea111a6; this change adds ELF-target validation without changing generation logic.

- Official compiler family: `AMD_NAVI44:gfx1200`.
- ABI: `9007201352883061`.
- Records: 176 built, zero skipped (101 root + 75 int4).
- Official SODB/PSDB reopen: 176 network groups passed; canary also passed.
- All 177 stored object-code values (network + canary) are ELF64 little-endian and have EF_AMDGPU_MACH `0x48` (gfx1200).
- Canonical database SHA256: `8a23bbeb84908a732077462f50229f7f8b78da62edc3df59df6034dc45304c86`.
- The unchanged gfx1201 set was rechecked: all 177 ELF objects have `0x4e`; its canonical SHA256 remains `c3a042a7dedd2fa30f06025e04f5db0c69812b893bf73ef116e8c49af3139d23`.

## Reproduce

Build with `scripts/build.cmd` as described in README. Then:

```powershell
scripts/generate.ps1 -Family gfx1200 -Out C:/out/0082-gfx1200 -AcoRoot <release>/dlssnr-amd/aco -ReleaseRoot <release>/dlssnr-amd -CompilerDir <agility>/build/native/bin/x64 -Plugin <amd-plugin>/amdxc_compiler.dll -Dxc <windows-sdk>/x64/dxc.exe
```

Run `--verify` with `--family gfx1200` and the generated manifest/SODB/PSDB/carrier paths to reproduce the target check. See `artifacts/0082-gfx1200/verify/verify.json` and `SHA256.txt` for results and file identities. PSDB file bytes may vary; compare canonical content.

## Integration scope

Use `carrier.sodb`, `public-copy.psdb`, and `net-carriers.bin` from `artifacts/0082-gfx1200/` together, registered for `AMD_NAVI44:gfx1200`. This is generated data, not a runtime adapter-selection patch: an integrator must select/register the matching family rather than leave a hard-coded gfx1201 configuration. Do not rename a gfx1201 PSDB and treat it as gfx1200.

No RX 9060/gfx1200 hardware execution was performed. This handoff establishes successful official compilation, stored ELF target identity and database integrity, not a real-card canary HIT. No RDNA3/gfx1103 data is included here.