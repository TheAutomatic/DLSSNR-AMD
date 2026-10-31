# M5: D3D12 ASD backend shell (branch feat/d3d12-asd-shell) - status 2026-10-31

Off by default. Without `plugins\dlssnr-amd-asd\dlssnr-amd-asd.ini` (or with `Enabled=0`) nothing new runs and
nothing new is logged: the ASI behaves as 0.0.8.1.

What is in:
- `nr_d3d12_asd_api.h`: the public D3D12 shader cache registration (D3DSCR) and SODB interfaces, declared by hand
  (mingw's d3d12.h has neither), reached through `D3D12GetInterface`. Public API only: no driver hooks, no
  per-driver code, no adapter name/vendor checks.
- `nr_d3d12_asd.cpp`: self-registration on its own thread from the ASI's start (not under the loader lock), with
  the built-in identity the SODB/PSDB were made with (as asd_probe's `fill_app_identity`); only our own earlier
  entries (same exe) are removed, a foreign entry is left alone and we do not register. Registration and Unregister
  are serialised (one installer at a time; an Unregister during the registration removes what it registered).
  The pbptr canary: the `g_pbptr` pipeline exactly as stored in the SODB (serialized root signature 112 B + DXIL
  3572 B, built into the ASI as `nr_d3d12_asd_canary_blob.h`, made by the test's `dump-canary` on the 1.619 runtime)
  is created on the game's device with CreateRootSignature + CreateComputePipelineState and run; ACO's values = HIT,
  HLSL's = MISS, else FAIL. Only HIT lets the backend run.
- No `CLSID_D3D12StateObjectFactory` (SODB reader) at run time: the in-box runtime (10.0.26100.9818), which every
  SDKVersion=615 app on this machine actually loads (Minecraft included, lab run `20261031-m1-runtime615-c`), does
  not have it (E_NOINTERFACE), but D3DSCR registration + PSDB lookup work there - the same as asd_probe's M1
  `--execute` path, which never reads the SODB. Where the reader exists (1.619), the SODB's identity is only compared
  (a warning when it differs). For M4: the network's kernels must be built in the same way (their exact RS + DXIL
  from the compile), not read from the SODB.
- `nr_d3d12_asd_backend.cpp`: the slot `nr::dlssnr::set_d3d12_backend` (set only when Enabled=1): HLSL copy of
  the colour into a linear float4 buffer, `D3d12AsdNetwork::record`, HLSL writeback; any failure returns false
  and the 0.0.8.1 path runs for that frame. The network is `PlaceholderCopyNetwork` (input -> output).
- `nr_d3d12_asd_panel.cpp`: NR Control > Diagnostics: status, Enabled (next launch), Unregister (writes
  `Register=0` so no launch registers again) / Allow registration again.
- `windows/tests/d3d12_asd`: standalone test (`build_d3d12_asd_test.sh`), modes early / late / unregister / dump-canary.

ini `[D3D12ASD]`: Enabled=0, Register=1, DebugTint=0, Sodb=dlssnr-amd-asd.sodb, Psdb=dlssnr-amd-asd-gfx1201.psdb,
AdapterFamily=AMD_NAVI48:gfx1201.

Tested (Navi48 gfx1201, test exe): early = HIT and frame path 0 differing bytes (separate and in place) on all three:
`_615` (SDKVersion 615, Minecraft's 1.615.1 core in `D3D12\`; loads `C:\Windows\SYSTEM32\D3D12Core.dll`), `_inbox`
(System32 core) and `_619` (app-local 1.619.5). Earlier, on 1.619: late after unregister = MISS, next launch = HIT;
Register=0 suppresses registration; unregister cleans up (also when it races the registration).
(The first version read identity and canary from the SODB and so failed on 615/in-box with
`D3D12GetInterface(CLSID_D3D12StateObjectFactory) 0x80004002`; fixed.)

Placeholder: the network (M4 goes behind `D3d12AsdNetwork`), the linear layout (float4 row-major), RenoDX pre/post
skipped in the D3D12 path, the lab canary's identity, built-in blob and assets (asd_probe.exe / "ACO public entry probe").
Source complete, waiting for a real game (源码完成、待实机): everything in game (OptiScaler, NR Control panel, game queue).

## v5: driver application profile (2026-10-31)
- Cause of Minecraft's ASD_MISS: the driver gives `Minecraft.Windows.exe` app profile 0x9E6ED99973A9329A (GetPrecompileTargets with that exe name; the public plugin's GetApplicationProfileVersion gives the same). The PSDB value key depends on the profile (g_pbptr: be7111bd... at profile 0, 40c20d1b... at 0x9E6E...), so a PSDB compiled for profile 0 misses. Eropsychosis-Win64-Shipping.exe has profile 0 (HIT in game).
- Registration: the running exe's profile for our family is read (public D3DSCR); if it differs from `[D3D12ASD] PsdbProfile` (default 0) the PSDB used is `<Psdb stem>-<profile hex>.psdb` from %LOCALAPPDATA%\dlssnr-amd-asd, else the plugin folder, else made by `ReprofileTool` (default `<folder>\reprofile\dlssnr-amd-asd-reprofile.exe` = nr_d3d12_asd_test_619.exe, with D3D12StateObjectCompiler.dll, amdxc_compiler.dll and D3D12\D3D12Core.dll 1.619.5 beside it; `Groups` = comma list of group keys, default g_pbptr). No exe/adapter list.
- nr_d3d12_asd_reprofile.cpp: per group, the SODB's pipeline desc compiled by the public plugin for the new identity (exe name) gives the new keys/metadata; the object code stored is the source PSDB's (our ACO, 2456 B; the plugin's own is 2368 B and is not used). Public d3d12compiler.h API only.
- Offline (inbox runtime, test exe): Minecraft.Windows.exe + profile PSDB HIT, + profile-0 PSDB MISS; asdtest.exe + profile-0 PSDB HIT, + profile PSDB MISS; auto selection: shipped HIT, runtime-made HIT, none MISS.
- Limits: the reprofile tool needs a runtime with the SODB reader (Agility 1.619, app-local); its SODB path must be ASCII (non-ASCII folder: 0x8007023E). The identity in the tool's reprofile mode is the placeholder's (same as builtin_identity).
