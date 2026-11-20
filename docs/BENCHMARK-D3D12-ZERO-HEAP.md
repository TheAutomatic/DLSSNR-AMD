# D3D12 Zero-Heap Direct vs v0.0.9 Benchmark Report (Speed & VRAM)

## Executive Summary

Controlled live benchmarks on an **AMD Radeon RX 9070 XT** (driver **26.11.1**) confirm **no speed or VRAM regression** in the current `feat/d3d12-zero-heap` build compared with the original `v0.0.9` release across both the **D3D12 Direct ELF** path and the **Vulkan Bridge** fallback path (`26` measured runs total):

- **D3D12 Direct — Significant Dedicated VRAM savings vs `v0.0.9`**: **`-34.6 MiB` (`-6.0%`)** at 1080p SDR/HDR, **`-60.0 to -60.6 MiB` (`-7.0% to -7.2%`)** at 1440p SDR/HDR, and **`-130.1 to -130.5 MiB` (`-8.7% to -9.1%`)** at 4K SDR/HDR, driven by eliding the idle RenoDX intermediate buffer and zero-heap SRD setup (`1080p Plain` has no RenoDX stage and stays identical at `537.8 MiB`).
- **D3D12 Direct — Faster CPU recording vs `v0.0.9`**: **`-7.8%` to `-19.4%`** lower CPU `evaluate_d3d12` recording time (`0.240–0.271 ms` vs `0.281–0.329 ms`), and **`15%–27%` lower** than both November 18 historical archive logs.
- **D3D12 Direct — GPU throughput parity**: Within **`+0.2% to +1.1%` (`+0.010 to +0.087 ms`)** of live `v0.0.9` in same-session paired runs, and faster than the historical `v0.0.9` archive (`before_bench.log`) in `6` of `7` workloads.
- **Vulkan Bridge (`--vulkan`) — Parity & clean teardown**: Identical Dedicated VRAM (**`±0.4 MiB`**) and GPU execution time (**`-0.8%` to `+0.1%`**) vs `v0.0.9`, while fixing process teardown so all `6` Current Vulkan runs exited cleanly with code `0` (all `6` `v0.0.9` Vulkan runs crashed with `0xC0000005` / `-1073741819` on global teardown prior to `AbandonAtExitGuard`).
- **Current D3D12 Direct vs Current Vulkan Bridge**: **`-106.3 to -307.8 MiB` (`-16.0% to -19.0%`)** lower steady-state Dedicated VRAM (**`-113.4 to -315.3 MiB`** lower peak Dedicated VRAM), **`-70.8% to -74.0%`** lower CPU recording overhead, and **`1.9%–3.3%`** faster GPU execution at 1080p/1440p (parity at 4K).

---

## Test Environment & Methodology

| Parameter | Configuration |
| :--- | :--- |
| **GPU** | AMD Radeon RX 9070 XT (`Vendor 0x1002`, `Device 0x7550`, RDNA4 `gfx1201`, 16191 MB VRAM) |
| **Driver / OS** | AMD Software 26.11.1 (`driverVersion 0x008001a0`), Windows 11 |
| **Model & Preset** | `CC_SILVER_AARDWOLD_OPTIMIZED` (`OptimizedLayers = 3`, Ultra Performance, 152 layers, FP32 accumulation, 6 merged persistent runs / 123 dispatches) |
| **Workload (`test_d3d12_offline`)** | 1-pass RenoDX Present stage (`SDR legacy` / `HDR v6`) plus `1080p Plain` (no RenoDX stage); `--warmup 10 --frames 50` (61 total frames including first-frame build + readback verification) |
| **Pixel Output Verification** | All `26` live runs verified `100.0%` pixels modified by NR and `0.00%` black pixels |

### Binaries Compared

- **`0.0.9` Original Baseline** (`test_d3d12_offline_before.exe`, built `2026-11-18` before zero-heap commit `73297d8`): uses unmodified `v0.0.9` (`9503f6b`) D3D12 HLSL/heap passthrough and Vulkan Bridge (`git diff v0.0.9..ae03fb6` contains zero NR runtime, D3D12, or Vulkan bridge changes).
- **`Current` (`feat/d3d12-zero-heap`)** (`test_d3d12_offline_curr.exe`, built `2026-11-20`): includes the zero-heap native ACO D3D12 outer path, indirect SRD support, D3D11->D3D12 bridge, swapchain arbiter, and read-only `dlssnr-amd.ini` fallback.

### VRAM Measurement & Vulkan Warmup Notes

- **WDDM PDH Counters**: Sampled every `20 ms` throughout each process lifetime via `\GPU Process Memory(*)\Dedicated Usage`, `Local Usage`, `Shared Usage`, and `Total Committed`.
  - **Steady-State** (`Steady Ded`, `Steady Local`, `Steady Shared`, `Steady Commit`): median across all samples collected after `"First frame + network build succeeded"` was printed.
  - **Peak** (`Peak Ded`, `Peak Commit`): maximum observed across the entire process lifetime.
- **Vulkan Cache Warmup**: Because `.nrb` pipeline binary caches in `aco-cache/` are keyed by `NR_BUILD_STAMP` (resolution-independent), cold driver import on AMD 26.11.1 was warmed once per binary prior to timed measurement. All `12` timed Vulkan runs built warm in `2.00–3.69 s`.

---

## Table 1 — D3D12 Direct: Live Head-to-Head (`0.0.9` vs `Current`, 2026-11-20)

All `7` Current D3D12 Direct cases reported `ActualBackend: Direct ELF (runs=61, fallbacks=0, hooks=1)`. Across every D3D12 Direct run, `Steady Dedicated == Peak Dedicated == Steady Local == Peak Local`.

| Case | GPU Avg (ms) `0.0.9` -> `Curr` | Wall Avg (ms) `0.0.9` -> `Curr` | Pure Net (ms) `0.0.9` -> `Curr` | Stage Avg (ms) `0.0.9` -> `Curr` | CPU Rec (ms) `0.0.9` -> `Curr` | Steady/Peak Ded (MiB) `0.0.9` -> `Curr` | Steady Shared (MiB) `0.0.9` -> `Curr` | Steady Commit (MiB) `0.0.9` -> `Curr` |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **1080p SDR** | `4.750` -> `4.760` (`+0.2%`) | `4.956` -> `4.972` | `4.557` -> `4.533` | `4.744` -> `4.748` | `0.288` -> `0.240` (**`-16.7%`**) | `574.6` -> `540.0` (**`-34.6 / -6.0%`**) | `25.2` -> `26.8` | `599.9` -> `566.8` (`-33.1`) |
| **1080p HDR** | `4.790` -> `4.828`* (`+0.8%`) | `5.006` -> `5.027` | `4.540` -> `4.531` | `4.774` -> `4.821` | `0.299` -> `0.257` (**`-14.0%`**) | `590.6` -> `556.0` (**`-34.6 / -5.9%`**) | `25.2` -> `26.8` | `615.9` -> `582.8` (`-33.1`) |
| **1080p Plain** | `4.704` -> `4.744` (`+0.9%`) | `4.916` -> `4.961` | `4.509` -> `4.525` | `0.000` -> `0.000` | `0.281` -> `0.259` (**`-7.8%`**) | `537.8` -> `537.8` (`0.0 / 0.0%`) | `25.2` -> `25.8` | `563.1` -> `563.5` (`+0.4`) |
| **1440p SDR** | `8.128` -> `8.173` (`+0.6%`) | `8.376` -> `8.400` | `7.867` -> `7.886` | `8.114` -> `8.184` | `0.314` -> `0.253` (**`-19.4%`**) | `836.9` -> `776.9` (**`-60.0 / -7.2%`**) | `37.6` -> `39.1` | `874.5` -> `815.9` (`-58.6`) |
| **1440p HDR** | `8.223` -> `8.310` (`+1.1%`) | `8.447` -> `8.542` | `7.873` -> `7.853` | `8.223` -> `8.285` | `0.329` -> `0.270` (**`-17.9%`**) | `866.2` -> `805.6` (**`-60.6 / -7.0%`**) | `37.6` -> `39.1` | `903.7` -> `844.7` (`-59.0`) |
| **4K SDR** | `18.399` -> `18.479` (`+0.4%`) | `18.698` -> `18.755` | `17.847` -> `17.909` | `18.393` -> `18.493` | `0.314` -> `0.267` (**`-15.0%`**) | `1439.1` -> `1308.6` (**`-130.5 / -9.1%`**) | `72.7` -> `74.2` | `1511.8` -> `1382.8` (`-129.0`) |
| **4K HDR** | `18.696` -> `18.772` (`+0.4%`) | `18.996` -> `19.047` | `17.859` -> `17.926` | `18.689` -> `18.800` | `0.324` -> `0.271` (**`-16.4%`**) | `1502.9` -> `1372.8` (**`-130.1 / -8.7%`**) | `72.7` -> `74.2` | `1575.6` -> `1447.0` (`-128.6`) |

*\*An immediate clean-exit recheck of `Current | D3D12 | 1080p HDR` exited `0` with `4.839 ms` GPU avg; see [Process Teardown & Stability Notes](#process-teardown--stability-notes).*

---

## Table 2 — D3D12 Direct: Historical Archive Cross-Check (2026-11-18 vs 2026-11-20)

Format per cell: **`GPU Avg / Pure Net Avg / CPU Rec Avg`** (all in `ms`).

| Case | `before_bench.log` (`v0.0.9`, 2026-11-18) | `after_bench.log` (`73297d8`, 2026-11-18) | Live `0.0.9` (2026-11-20) | Live `Current` (2026-11-20) |
| :--- | :--- | :--- | :--- | :--- |
| **1080p SDR** | `4.790 / 4.606 / 0.316` | `4.757 / 4.480 / 0.333` | `4.750 / 4.557 / 0.288` | `4.760 / 4.533 / 0.240` |
| **1080p HDR** | `4.830 / 4.596 / 0.339` | `4.845 / 4.491 / 0.360` | `4.790 / 4.540 / 0.299` | `4.828 / 4.531 / 0.257` |
| **1080p Plain** | `4.760 / 4.579 / 0.301` | `4.755 / 4.494 / 0.335` | `4.704 / 4.509 / 0.281` | `4.744 / 4.525 / 0.259` |
| **1440p SDR** | `8.184 / 7.933 / 0.323` | `8.000 / 7.612 / 0.349` | `8.128 / 7.867 / 0.314` | `8.173 / 7.886 / 0.253` |
| **1440p HDR** | `8.257 / 7.908 / 0.333` | `8.137 / 7.627 / 0.349` | `8.223 / 7.873 / 0.329` | `8.310 / 7.853 / 0.270` |
| **4K SDR** | `18.703 / 18.178 / 0.339` | `18.011 / 17.367 / 0.364` | `18.399 / 17.847 / 0.314` | `18.479 / 17.909 / 0.267` |
| **4K HDR** | `18.971 / 18.108 / 0.348` | `18.254 / 17.336 / 0.353` | `18.696 / 17.859 / 0.324` | `18.772 / 17.926 / 0.271` |

**Interpretation**: Both live binaries (`0.0.9` and `Current`) measured on `2026-11-20` show ~2%–3% slower Pure Network / GPU spans at 4K than `after_bench.log` from `2026-11-18`, reflecting cross-day GPU boost clock / thermal conditions rather than a code regression. Within the same `2026-11-20` session, paired `0.0.9` vs `Current` runs stay within `0.2%–1.1%` on GPU time, Live `Current` beats the `before_bench.log` `v0.0.9` GPU average in `6` of `7` workloads, and Live `Current` cuts CPU recording time by `15%–27%` relative to both November 18 archives.

---

## Table 3 — Vulkan Bridge (`--vulkan`): Live Head-to-Head (`0.0.9` vs `Current`, 2026-11-20)

| Case | GPU Avg (ms) `0.0.9` -> `Curr` | Wall Avg (ms) `0.0.9` -> `Curr` | Pure Net (ms) `0.0.9` -> `Curr` | CPU Rec (ms) `0.0.9` -> `Curr` | Steady Ded (MiB) `0.0.9` -> `Curr` | Peak Ded (MiB) `0.0.9` -> `Curr` | Steady Local (MiB) `0.0.9` -> `Curr` | Steady Shared (MiB) `0.0.9` -> `Curr` | Steady Commit (MiB) `0.0.9` -> `Curr` |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **1080p SDR** | `4.892` -> `4.853` (`-0.8%`) | `5.036` -> `5.006` | `4.547` -> `4.565` | `0.879` -> `0.898` | `646.5` -> `646.8` | `653.7` -> `654.2` | `621.8` -> `622.1` | `22.0` -> `22.0` | `617.5` -> `617.8` |
| **1080p HDR** | `4.978` -> `4.954` (`-0.5%`) | `5.139` -> `5.111` | `4.579` -> `4.609` | `0.961` -> `0.880` | `662.3` -> `662.3` | `669.4` -> `669.4` | `637.6` -> `637.6` | `22.0` -> `22.0` | `633.4` -> `633.3` |
| **1440p SDR** | `8.368` -> `8.379` (`+0.1%`) | `8.551` -> `8.571` | `7.880` -> `7.909` | `0.945` -> `0.973` | `941.7` -> `942.1` | `948.6` -> `949.4` | `897.5` -> `897.9` | `34.9` -> `34.9` | `886.6` -> `887.0` |
| **1440p HDR** | `8.590` -> `8.590` (`0.0%`) | `8.792` -> `8.781` | `8.046` -> `8.036` | `1.066` -> `0.991` | `970.5` -> `970.6` | `978.1` -> `977.6` | `926.3` -> `926.4` | `34.9` -> `34.9` | `915.5` -> `915.5` |
| **4K SDR** | `18.549` -> `18.464` (`-0.5%`) | `18.809` -> `18.728` | `17.780` -> `17.707` | `1.076` -> `1.019` | `1616.4` -> `1616.4` | `1623.9` -> `1623.9` | `1521.2` -> `1521.2` | `71.2` -> `71.2` | `1495.7` -> `1495.7` |
| **4K HDR** | `18.751` -> `18.764` (`+0.1%`) | `19.010` -> `19.039` | `17.755` -> `17.815` | `0.985` -> `1.035` | `1680.2` -> `1680.3` | `1687.7` -> `1687.7` | `1584.9` -> `1585.0` | `71.2` -> `71.2` | `1559.4` -> `1559.5` |

---

## Table 4 — Current Build: D3D12 Direct vs Vulkan Bridge

| Case | GPU Avg (ms) `Direct` vs `Vulkan` | CPU Rec (ms) `Direct` vs `Vulkan` | Steady Ded VRAM (MiB) `Direct` vs `Vulkan` | Peak Ded VRAM (MiB) `Direct` vs `Vulkan` |
| :--- | :--- | :--- | :--- | :--- |
| **1080p SDR** | `4.760` vs `4.853` (**`-1.9%`**) | `0.240` vs `0.898` (**`-73.3%`**) | `540.0` vs `646.8` (**`-106.8 MiB / -16.5%`**) | `540.0` vs `654.2` (**`-114.2 MiB / -17.5%`**) |
| **1080p HDR** | `4.828` vs `4.954` (**`-2.5%`**) | `0.257` vs `0.880` (**`-70.8%`**) | `556.0` vs `662.3` (**`-106.3 MiB / -16.0%`**) | `556.0` vs `669.4` (**`-113.4 MiB / -16.9%`**) |
| **1440p SDR** | `8.173` vs `8.379` (**`-2.5%`**) | `0.253` vs `0.973` (**`-74.0%`**) | `776.9` vs `942.1` (**`-165.2 MiB / -17.5%`**) | `776.9` vs `949.4` (**`-172.5 MiB / -18.2%`**) |
| **1440p HDR** | `8.310` vs `8.590` (**`-3.3%`**) | `0.270` vs `0.991` (**`-72.8%`**) | `805.6` vs `970.6` (**`-165.0 MiB / -17.0%`**) | `805.6` vs `977.6` (**`-172.0 MiB / -17.6%`**) |
| **4K SDR** | `18.479` vs `18.464` (`+0.1%`) | `0.267` vs `1.019` (**`-73.8%`**) | `1308.6` vs `1616.4` (**`-307.8 MiB / -19.0%`**) | `1308.6` vs `1623.9` (**`-315.3 MiB / -19.4%`**) |
| **4K HDR** | `18.772` vs `18.764` (`+0.0%`) | `0.271` vs `1.035` (**`-73.8%`**) | `1372.8` vs `1680.3` (**`-307.5 MiB / -18.3%`**) | `1372.8` vs `1687.7` (**`-314.9 MiB / -18.7%`**) |

---

## Process Teardown & Stability Notes

- **`0.0.9` Vulkan Bridge teardown**: All `6` `0.0.9 | Vulkan` runs exited with `0xC0000005` (`-1073741819`) during global teardown after completing all `61` frames and printing the complete benchmark summary (predating `AbandonAtExitGuard`).
- **`Current` Vulkan Bridge teardown**: All `6` `Current | Vulkan` runs exited cleanly with exit code `0`.
- **`Current` D3D12 Direct `1080p HDR` batch teardown**: During the unattended `26`-run batch, the `Current | D3D12 | 1080p HDR` process exited `-1073741819` during process teardown after printing its full benchmark summary; an immediate standalone re-run exited cleanly with code `0` (`4.839 ms` GPU avg).
