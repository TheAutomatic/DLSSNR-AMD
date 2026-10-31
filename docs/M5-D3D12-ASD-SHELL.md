# M5: D3D12 ASD backend shell (branch feat/d3d12-asd-shell) - status 2026-10-31

Off by default. Without `plugins\dlssnr-amd-asd\dlssnr-amd-asd.ini` (or with `Enabled=0`) nothing new runs and
nothing new is logged: the ASI behaves as 0.0.8.1.

What is in:
- `nr_d3d12_asd_api.h`: the public D3D12 shader cache registration (D3DSCR) and SODB interfaces, declared by hand
  (mingw's d3d12.h has neither), reached through `D3D12GetInterface`. Public API only: no driver hooks, no
  per-driver code, no adapter name/vendor checks.
- `nr_d3d12_asd.cpp`: self-registration on its own thread from the ASI's start (not under the loader lock);
  the identity comes from the SODB (`GetApplicationDesc`); only our own earlier entries (same exe) are removed,
  a foreign entry is left alone and we do not register. The pbptr canary: the SODB's `g_pbptr` pipeline is
  built on the game's device and run; ACO's values = HIT, HLSL's = MISS, else FAIL. Only HIT lets the backend run.
- `nr_d3d12_asd_backend.cpp`: the slot `nr::dlssnr::set_d3d12_backend` (set only when Enabled=1): HLSL copy of
  the colour into a linear float4 buffer, `D3d12AsdNetwork::record`, HLSL writeback; any failure returns false
  and the 0.0.8.1 path runs for that frame. The network is `PlaceholderCopyNetwork` (input -> output).
- `nr_d3d12_asd_panel.cpp`: NR Control > Diagnostics: status, Enabled (next launch), Unregister (writes
  `Register=0` so no launch registers again) / Allow registration again.
- `windows/tests/d3d12_asd`: standalone test (`build_d3d12_asd_test.sh`), modes early / late / unregister.

ini `[D3D12ASD]`: Enabled=0, Register=1, DebugTint=0, Sodb=dlssnr-amd-asd.sodb, Psdb=dlssnr-amd-asd-gfx1201.psdb,
AdapterFamily=AMD_NAVI48:gfx1201, CanaryKey=g_pbptr.

Tested (Navi48 gfx1201, test exe, Agility 1.619.5): early = HIT, frame path 0 differing bytes (separate and
in place); late after unregister = MISS, next launch = HIT; Register=0 suppresses registration; unregister cleans up.
Agility 1.615.1 (Minecraft's) and the inbox runtime: `D3D12GetInterface(CLSID_D3D12StateObjectFactory)` is
E_NOINTERFACE, so no ASD there (FAIL -> 0.0.8.1 path).

Placeholder: the network (M4 goes behind `D3d12AsdNetwork`), the linear layout (float4 row-major), RenoDX pre/post
skipped in the D3D12 path, the lab canary's identity and assets (asd_probe.exe / "ACO public entry probe").
Source complete, waiting for a real game (源码完成、待实机): everything in game (OptiScaler, NR Control panel, game queue).
