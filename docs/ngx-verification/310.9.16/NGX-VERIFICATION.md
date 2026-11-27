# Checking DLSSNR-AMD against NVIDIA's own DLL: 310.9.16, `CC_SMALL_FP8`

This compares the picture DLSSNR-AMD produces with `CC_SMALL_FP8` against the picture NVIDIA's DLSS 5
Neural Rendering DLL 310.9.16 produces for the same input and the same settings: single frames, and
short moving sequences where motion vectors and the previous frame (history) are used. The inputs,
settings and measures are the ones of the [310.8.0 check](../310.8.0/NGX-VERIFICATION.md).

## The two models in 310.9.16

`nvngx_dlssnr.dll` 310.9.16 holds two models and picks one with the `DLSSNR.Hint.Render.Preset`
creation parameter: preset 1 is `CC_SILVER_AARDWOLD`, the model of 310.8.0; preset 2, which is also the default,
is `CC_SMALL_FP8`. On the RTX 5090, through NGX:

- 310.9.16 with preset 1 gives byte-identical output to 310.8.0 at 1920x1080, 2560x1440 and 3840x2160.
- 310.9.16 with its default preset and with preset 2 give byte-identical output: the default is the new
  model.

GPU time of NVIDIA's DLL per evaluate on the RTX 5090 (D3D12 timestamps around 50 back-to-back
`EvaluateFeature` calls in one command list, median over frames 3-11 of 12; the single-frame input and
settings below):

| | 1920x1080 | 2560x1440 | 3840x2160 |
|---|---|---|---|
| 310.8.0 (`CC_SILVER_AARDWOLD`) | 2.825 ms | 3.816 ms | 7.708 ms |
| 310.9.16 (`CC_SMALL_FP8`) | 1.671 ms | 2.149 ms | 4.197 ms |

## What ran on each side

**NVIDIA side (the reference).** The unmodified `nvngx_dlssnr.dll` 310.9.16.0, called through NVIDIA NGX
the way a game calls it, with its default preset (`CC_SMALL_FP8`). The DLL ran its own host code and its
own GPU kernels end to end; nothing was extracted from it, re-implemented or replayed.

- GPU: NVIDIA GeForce RTX 5090, Linux driver 595.71.05.
- NGX core: `nvngx.dll` / `_nvngx.dll` from the NVIDIA Linux driver package 615.71.09; the driver version
  was reported as 615.71.
- D3D12 on Linux: Wine 11.18 (staging, wow64 build), vkd3d-proton, DXVK's DXGI and dxvk-nvapi (DLLs from
  GE-Proton 11-7), on NVIDIA's Vulkan driver.
- Host programs: the same small D3D12 programs as for 310.8.0.

**DLSSNR-AMD side.** The Linux build of DLSSNR-AMD 0.0.6-linux-preview with `CC_SMALL_FP8`, on an AMD
Radeon RX 9070 XT (Mesa 26.2.4 RADV), through the same code path a game on the OptiScaler route uses:
its NGX core under GE-Proton 11-7 Wine and vkd3d-proton, driven by the same host programs with the same
input files and parameters. DLSSNR-AMD builds its network in the background from the first evaluate on
and passes frames through until it is ready, where NVIDIA's DLL is ready when `CreateFeature` returns. So
on this side the single-frame program waits 8 seconds between frames, and the sequence program evaluates
the first frame once before the sequence (with `DLSSNR.Reset` set, output discarded) and waits 8 seconds;
the sequence itself then starts with `DLSSNR.Reset` as on the NVIDIA side. Every frame compared went
through the network.

## Single frames

### Input and settings

As for 310.8.0: one Tomb Raider (2013) frame, 3840x2160, and its downscales to 2560x1440 and 1920x1080
(`../310.8.0/single-frame-inputs/`), constant depth, zero motion, `DLSSNR.Reset` on every evaluate.
Settings: `Intensity` 1, `Style` 0, `LocalToneStrength` 1, `LocalStructureStrength` 1,
`SkinStructureStrength` -1, `UseAutoMask` 1.

### Results

Both outputs compared as 8-bit RGB. SSIM is the mean over R, G and B (11x11 Gaussian window, sigma 1.5).
"Edit" is output minus input, i.e. what the network changed.

| Resolution | PSNR vs NVIDIA | SSIM vs NVIDIA | Correlation of the edits | Mean difference (1/255, R G B) | Pixels with all channels within one 8-bit step |
|---|---|---|---|---|---|
| 1920x1080 | 49.15 dB | 0.9971 | 0.9955 | -0.02 +0.00 -0.02 | 86.3% |
| 2560x1440 | 49.40 dB | 0.9969 | 0.9962 | +0.00 +0.00 +0.02 | 87.0% |
| 3840x2160 | 50.59 dB | 0.9971 | 0.9956 | -0.01 -0.01 -0.03 | 92.2% |

For scale, the input itself (Neural Rendering off) against NVIDIA's output: PSNR 28.88 / 28.21 / 30.09 dB,
SSIM 0.9736 / 0.9696 / 0.9774. Size of the edit (RMS, 1/255): NVIDIA 9.18 / 9.91 / 7.98,
DLSSNR-AMD 9.36 / 9.77 / 8.04. `CC_SMALL_FP8` changes the picture less than `CC_SILVER_AARDWOLD` (NVIDIA's edit
with 310.8.0: 12.15 / 10.48 / 9.37).

NVIDIA's DLL gave byte-identical output in separate runs of the same input, so the differences above are
what remains between the two implementations.

Pictures (`single-frame-images/`): input, NVIDIA and DLSSNR-AMD side by side, for each resolution, full
frame and a close-up.

Raw outputs (`single-frame-outputs/`): both sides at every resolution as lossless 8-bit PNG, exactly the
pixels compared above.

## Moving sequences

### Input and settings

The four sequences of the 310.8.0 check (still scene, moving object, panning camera + moving object, the
same with NR after an upscaler), 10 frames each at 1920x1080, 2560x1440 and 3840x2160, with the same
settings.

### Results

Every frame compared as 8-bit RGB the same way as above; "mean" is over the 10 frames. The first frame
has no history yet; the last frame is the tenth. CIEDE2000 is the colour difference as the eye sees it
(below 1 is generally not visible). "Change per frame" is how much each output still changes from one
frame to the next once the previous frame is moved by the motion vectors (mean over all pixels, 1/255).

**1920x1080**

| Sequence | PSNR vs NVIDIA (mean / first frame / last frame) | SSIM vs NVIDIA | Correlation of the edits | CIEDE2000 mean / 95th percentile | Change per frame: NVIDIA / DLSSNR-AMD / input |
|---|---|---|---|---|---|
| Still scene | 52.27 / 49.48 / 53.11 dB | 0.9983 | 0.9973 | 0.37 / 0.93 | 0.283 / 0.286 / 0.000 |
| Moving object | 51.79 / 49.17 / 52.40 dB | 0.9982 | 0.9969 | 0.38 / 0.94 | 0.312 / 0.323 / 0.038 |
| Panning camera + moving object | 51.52 / 49.17 / 52.64 dB | 0.9981 | 0.9969 | 0.38 / 0.95 | 1.678 / 1.687 / 2.054 |
| Same, NR after an upscaler | 50.58 / 49.17 / 51.26 dB | 0.9980 | 0.9962 | 0.38 / 0.94 | 1.683 / 1.679 / 2.056 |

**2560x1440**

| Sequence | PSNR vs NVIDIA (mean / first frame / last frame) | SSIM vs NVIDIA | Correlation of the edits | CIEDE2000 mean / 95th percentile | Change per frame: NVIDIA / DLSSNR-AMD / input |
|---|---|---|---|---|---|
| Still scene | 52.36 / 49.52 / 53.44 dB | 0.9981 | 0.9979 | 0.37 / 0.93 | 0.252 / 0.253 / 0.000 |
| Moving object | 52.23 / 49.66 / 53.29 dB | 0.9981 | 0.9977 | 0.37 / 0.93 | 0.289 / 0.288 / 0.038 |
| Panning camera + moving object | 52.19 / 49.66 / 52.93 dB | 0.9980 | 0.9978 | 0.37 / 0.93 | 1.101 / 1.098 / 1.229 |
| Same, NR after an upscaler | 51.21 / 49.66 / 51.55 dB | 0.9979 | 0.9973 | 0.37 / 0.94 | 1.100 / 1.099 / 1.231 |

**3840x2160**

| Sequence | PSNR vs NVIDIA (mean / first frame / last frame) | SSIM vs NVIDIA | Correlation of the edits | CIEDE2000 mean / 95th percentile | Change per frame: NVIDIA / DLSSNR-AMD / input |
|---|---|---|---|---|---|
| Still scene | 53.14 / 50.71 / 54.08 dB | 0.9982 | 0.9974 | 0.34 / 0.91 | 0.256 / 0.256 / 0.000 |
| Moving object | 53.10 / 50.69 / 53.91 dB | 0.9982 | 0.9973 | 0.34 / 0.91 | 0.286 / 0.283 / 0.038 |
| Panning camera + moving object | 53.12 / 50.69 / 54.07 dB | 0.9982 | 0.9973 | 0.33 / 0.91 | 0.338 / 0.336 / 0.046 |
| Same, NR after an upscaler | 52.24 / 50.69 / 52.99 dB | 0.9981 | 0.9968 | 0.34 / 0.91 | 0.341 / 0.342 / 0.048 |

Data (`moving-sequence-results.json`): every number above, per frame. As for 310.8.0, the moving sequences
come without pictures.

## Limits

- One picture. The single frames cover three resolutions; the moving sequences are short (10 frames),
  made from that picture with exact motion vectors, not recorded from a game.
- The input is handed straight to the network. How a game or OptiScaler prepares its frame before
  Neural Rendering is not part of this comparison.
- One AMD card (RX 9070 XT) and one NVIDIA card (RTX 5090).
