# DLSSNR-AMD

Runs the neural rendering (NR) model of NVIDIA DLSS 5 in games on AMD Radeon RX 9000 (RDNA4)
graphics cards.

- The network is reimplemented as Vulkan compute shaders.
- The model is not included. The installer extracts it from your own copy of `nvngx_dlssnr.dll`.
  Version 310.9.16 holds two models: `CC_SMALL_FP8`, its default and faster, and `CC_SILVER_AARDWOLD`, the model
  of 310.8.0, so from 310.9.16 alone the installer installs both. Since 0.0.6-linux-preview (Linux) and
  0.0.6-win-preview-alpha (Windows) `CC_SMALL_FP8` runs by default; `CC_SILVER_AARDWOLD` stays selectable
  (see [The model](#the-model)). Since 0.0.7 the networks carry the names in NVIDIA's DLL -
  `CC_SMALL_FP8`, `CC_SILVER_AARDWOLD` and `CC_SILVER_AARDWOLD_INT4_MIXED` ([int4 mixed](#int4-mixed-optional)) -
  and all of them can be switched in game.
  Since 0.0.8 (Linux) and 0.0.8.1 (Windows) `CC_SMALL_FP8_EXPERIMENTAL` can be chosen as well
  ([CC_SMALL_FP8_EXPERIMENTAL](#cc_small_fp8_experimental-optional)).
- The Linux build with `CC_SMALL_FP8`, on an RX 9070 XT, checked against NVIDIA's own `nvngx_dlssnr.dll`
  310.9.16 run on an RTX 5090 through NGX, the same way as below. Single frames: PSNR 49.15 dB at 1080p,
  49.40 dB at 1440p and 50.59 dB at 4K, SSIM 0.997. Moving sequences: PSNR 50.6-52.3 dB at 1080p,
  51.2-52.4 dB at 1440p and 52.2-53.1 dB at 4K, SSIM 0.998, and the output changes from frame to frame by
  about as much as NVIDIA's. Method, pictures and data:
  [docs/ngx-verification/310.9.16](docs/ngx-verification/310.9.16/NGX-VERIFICATION.md).
- The Linux build with `CC_SILVER_AARDWOLD`, on an RX 9070 XT, checked against NVIDIA's own `nvngx_dlssnr.dll`
  310.8.0 run on an RTX 5090 through NGX (the way a game calls it), with the same input and settings.
  Single frames: PSNR 45.56 dB at 1080p, 47.99 dB at 1440p and 49.06 dB at 4K (higher is closer; the input frame
  itself scores 26-29 dB), SSIM 0.996-0.997 (1 = identical); NVIDIA's DLL gives byte-identical output
  from run to run, so the difference is between the two implementations, not noise. Moving sequences
  with motion vectors and history (a still scene, a moving object, a panning camera, and NR after an
  upscaler; 10 frames each): PSNR 46.3-47.0 dB at 1080p, 50.1-50.9 dB at 1440p and 51.1-51.6 dB at 4K,
  SSIM 0.997-0.998, and the output changes from frame to frame by about as much as NVIDIA's. Method,
  pictures and data: [docs/ngx-verification/310.8.0](docs/ngx-verification/310.8.0/NGX-VERIFICATION.md).
- **Tested only on an RX 9070 XT.**
- **Windows:** since 0.0.8.1 the Windows version is the Linux version (the same routes, NR Control and
  settings). It loads the network as machine code that was made in advance with Mesa's ACO compiler (the Linux
  driver's compiler) and ships in the package, and comes close to Linux (see [Performance](#performance)); see
  [Windows](#windows).
- **RX 7000 and Radeon 700M / 800M / 8000S graphics (RDNA3), Windows only, experimental:** `CC_SMALL_FP8` and
  `CC_SILVER_AARDWOLD`, thanks to [TheAutomatic](https://github.com/TheAutomatic)
  ([#26](https://github.com/mochizuki0323/DLSSNR-AMD/pull/26)) and to [mauri870](https://github.com/mauri870), whose
  [DLSSNR-RDNA3](https://github.com/mauri870/DLSSNR-RDNA3) port it builds on. Tried on few machines; expect more
  problems than on RX 9000, and for now it may not match the stability and performance of DLSSNR-RDNA3 (see
  [Windows](#windows)).
- **Also worth a try:** [TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)
  (OptScaler(NR)), an OptiScaler-based project with several NR backends.

This is an independent project. It is not affiliated with, endorsed by or supported by NVIDIA or
AMD. DLSS is a trademark of NVIDIA Corporation.

## Before you use it

This is an early project (version 0.0.x) and its testing is limited:

- one graphics card, one Linux desktop, Mesa 26.2 and GE-Proton;
- a small number of games, at a few resolutions and settings;

Other cards, drivers, Proton versions, distributions and games have not been tested. Expect problems:
games that fail to start or crash, visual artifacts, settings that do not behave as described, or
performance that differs from the numbers below. Problems are fixed as they are found, and the project
will keep changing, including in ways that break earlier setups.

- The installer puts DLLs into the game folder. Keep a backup of anything you care about; `remove`
  deletes what was installed.
- **Do not use it in online games with anti-cheat.** Injected DLLs can get an account banned.
- It is provided as is, without warranty (see [LICENSE](LICENSE)).

If something goes wrong, please open an issue with the game, the route, your card and driver, and
these logs:

- `dlssnr-amd.log` and `OptiScaler.log` or `ReShade.log` (whichever route you use), all in the game
  folder. On Windows, option 5 of `install.bat` puts them into one zip.
- On Linux, if you can, a Proton log as well: set the game's launch options in Steam to
  `PROTON_LOG=1 %command%`, run the game until the problem shows, and attach `steam-<appid>.log` from
  your home folder.

## Status

| Platform | Needs | State |
| --- | --- | --- |
| **Linux** (Steam / Proton) | Mesa 26.2 or newer, a recent Proton | The main version, used in games. |
| **Windows** | AMD Software 26.9 or newer | The same features as Linux; close to Linux in speed with ACO's machine code. |

Both need an RX 9000 series (RDNA4) card for every network. On Windows RX 7000 and Radeon 700M / 800M / 8000S
graphics (RDNA3) run `CC_SMALL_FP8` and `CC_SILVER_AARDWOLD` (experimental); they lack the FP8 matrix
instructions, and their network runs FP16 in place of FP8. Older cards are not supported.

## Performance

`CC_SMALL_FP8` and `CC_SMALL_FP8_EXPERIMENTAL` (310.9.16), GPU time of the network per frame on an RX 9070 XT
(network only). Linux **measured in game** (`CC_SMALL_FP8` with v0.0.6-linux-preview, `CC_SMALL_FP8_EXPERIMENTAL`
with 0.0.8); Windows with the driver's own compiler from
[#13](https://github.com/mochizuki0323/DLSSNR-AMD/pull/13); Windows with ACO's machine code not measured (it runs the
same machine code as Linux):

| | 1080p | 1440p | 4K |
| --- | --- | --- | --- |
| Linux, `CC_SMALL_FP8` | 3.27 ms | 5.53 ms | 12.67 ms |
| Linux, [`CC_SMALL_FP8_EXPERIMENTAL`](#cc_small_fp8_experimental-optional) | 2.90 ms | 4.84 ms | 11.09 ms |
| Windows, `CC_SMALL_FP8`, the driver's own compiler | 4.673 ms | 7.982 ms | 16.940 ms |
| Windows, `CC_SMALL_FP8`, [ACO's machine code](#windows) (the default) | about the same as Linux | about the same as Linux | about the same as Linux |
| Windows, [`CC_SMALL_FP8_EXPERIMENTAL`](#cc_small_fp8_experimental-optional), [ACO's machine code](#windows) | about the same as Linux | about the same as Linux | about the same as Linux |

`CC_SILVER_AARDWOLD` (310.8.0), GPU time of the network per frame on an RX 9070 XT, **offline benchmark** (network
only), measured with v0.0.3 (the `CC_SILVER_AARDWOLD_INT4_MIXED` row with v0.0.4, the Windows ACO Mode row with v0.0.4-win-preview
on AMD Software 26.9.2). In games, the network-only time the log shows is lower than in this offline
benchmark:

| | 1080p | 1440p | 4K |
| --- | --- | --- | --- |
| Linux | 5.60 ms | 9.70 ms | 21.89 ms |
| Linux, [`CC_SILVER_AARDWOLD_INT4_MIXED`](#int4-mixed-optional) | 4.99 ms | 8.69 ms | 19.64 ms |
| Windows, the driver's own compiler | 7.21 ms | 12.59 ms | 27.32 ms |
| Windows, [ACO's machine code](#windows) (the default) | 5.70 ms | 9.96 ms | 22.49 ms |
| Windows, [ACO's machine code](#windows), [`CC_SILVER_AARDWOLD_INT4_MIXED`](#int4-mixed-optional) | about the same as Linux | about the same as Linux | about the same as Linux |

Running the model below the output resolution about doubles the frame rate: the resolution setting (model
resolution in the [wilsjo2 style](#wilsjo2-style), the network's resolution in the [RenoDX style](#renodx-style))
on any route, or on the OptiScaler route NR placed before the upscaler, which runs the model on the game's
render-resolution frame.

## Supported games

Linux and Windows have the same routes:

| Game uses | Route |
| --- | --- |
| **DirectX 12 / 11** | `optiscaler` if the game has a DLSS, FSR or XeSS option, otherwise `reshade-feeder` |
| **DirectX 10** | `reshade-feeder` |
| **DirectX 9** | `reshade-feeder-dx9` |
| **Vulkan** | `optiscaler` if the game has a DLSS, FSR or XeSS option, otherwise `reshade-feeder-vulkan` |
| OpenGL | not supported |

- **`optiscaler`**: for games with a DLSS, FSR or XeSS option, with the game's own motion vectors. 64-bit games
  only. The original [OptiScaler](https://github.com/optiscaler/OptiScaler) (shipped unchanged) does the
  upscaling and frame generation, and this project's NR runs as an OptiScaler plugin (`dlssnr-amd.asi`) on
  DirectX 12, DirectX 11 and Vulkan games (since 0.0.8 on Linux, 0.0.8.1 on Windows; up to 0.0.7 this route
  was [OptiScaler-NR](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) with this project as its
  DLSS-NR backend).
- **`reshade-feeder`**: for other DirectX 10/11/12 games. Runs through ReShade; motion is estimated from
  the picture by a ReShade shader.
- **`reshade-feeder-vulkan` / `reshade-feeder-dx9`**: for Vulkan and DirectX 9 games. DirectX 9 games
  run on Vulkan through DXVK, and ReShade is loaded as a Vulkan layer underneath it.
  `reshade-feeder-dx9` also sets the depth direction old DirectX 9 games use.

These routes were called `reshade`, `vulkan` and `dx9` up to 0.0.4 on Linux and up to 0.0.4-win-preview
on Windows. Any route can also use the [RenoDX style](#renodx-style).

32-bit games work on Linux (use the i686 package; not with `optiscaler`). The Windows version is
64-bit only.

## NR Control (OptiScaler route)

End opens NR Control, this project's NR menu, drawn over the game's DirectX 11, DirectX 12 or Vulkan swap chain
(another key: `[NRControl] MenuKey`). Nothing to set up. Changes apply in game within about a second and are kept in
`dlssnr-amd.ini`. It holds every NR setting; OptiScaler's own menu (Insert) only drives upscaling and frame
generation. The ReShade routes have the same pages on ReShade's Add-ons page (plus Motion vectors and Language).

- **NR settings**: the processing ([RenoDX style](#renodx-style) or [wilsjo2 style](#wilsjo2-style)), the network,
  the place (at Present, the default, or before or after the upscaler; on the ReShade routes always Present) and
  its settings, and Engine data (what the game hands NR).
- **Network**: the networks and their options (model, hotkey, residency, Memory Guard, OSD). At its foot the
  **Layers** switch, off by default: on, NR can be several layers, each a complete NR run with its own processing,
  network, place and settings (the NR chain page). For ordinary stacking use Passes, not layers.
- **Preprocess**, **HUD FIX** (keeps the game's HUD as it is at Present; off by default), **Hotkeys** (every key NR
  reads, press a key to assign it), **Screenshot** (F5: the same frame without and with NR).
- **Compare**: NR off against NR on, side by side or with a split line, with a zoom and debug views.
- **Diagnostics**: cost, video memory, residency, an event timeline, and Copy a report (to the clipboard and
  `dlssnr-amd\nr-report.txt`; nothing is sent anywhere).
- **Status**: what NR does and costs (the network and its processing apart); video memory by network and use.
- The header has the **Neural Rendering** switch (F6: NR off or on for this run); the footer NR's GPU time and the
  upscaling.

## Languages

NR Control (OptiScaler route) and the Add-ons page (ReShade routes) take their texts from a language file in
`dlssnr-amd\lang` in the game folder, chosen on NR Control's NR Control page or on the Add-ons page's Language
page. Any language can be added: copy `English.txt` there (every text of the menus, made from the program), name
the copy after the language and translate the text after each `msgstr`. On the OptiScaler route a font that has the
language's letters can be named in the file (`@font`); the ReShade routes use ReShade's own font (ReShade's
settings, Font). The file is read again each time it is saved, so the translation can be checked in the game;
`README.txt` in that folder explains it step by step.

## RenoDX style

The processing around NR - how the picture is handed to the network and how its result is put back - as
[RenoDX's DLSS5 add-on](https://github.com/PEQHUB/RenoDX-DLSS5-Generic) v8.5.0-rc10 does it, with RenoDX's own
shaders. The NR network itself is the same. It is the default processing on every route (the installer asks;
`--wilsjo2` on Linux or `-RenoDX off` on Windows chooses the other), and NR Control switches it in game. Its
settings keep RenoDX's names, defaults and ranges:

- **Network**: Intensity, Style, Local tone, Local structure, Skin structure, automatic mask, new noise every frame.
- **Passes**: legacy (up to 4, or 30 unlocked; passes 2 and later with their own strengths and network settings) or
  fast (the network runs once and its change is strengthened).
- **Composition**: colour and transfer strength, chroma clamp, transfer mode, highlight guard, Detail stability (Off
  by default), black restore and pedestal, codec.
- **Resolution**: the network's resolution, following the render resolution, upsampling.
- **Adjust NR's look**: brightening, darkening, colour, hue, shadows / midtones / highlights, large-scale tone and
  fine detail, halos, temporal smoothing.
- **Encoding**: how the picture's values are read (SDR, linear, PQ; units and white points for HDR) and the divisor.
- **Guides**: depth and motion vectors.

## wilsjo2 style

The processing around NR of [OptiScaler-NR](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)
(wilsjo2's OptiScaler fork, which the OptiScaler route used up to 0.0.7), ported to this project. The NR network
itself is the same. It is the other processing on every route (`--wilsjo2` on Linux, `-RenoDX off` on Windows at
install, or NR Control in game); on the ReShade routes it is these routes' own processing (`[DlssNr]`). Its settings
keep OptiScaler-NR's `[DlssNr]` names, defaults and ranges:

- **Network**: Style, Intensity, Local structure, Local tone, Skin structure, Character mask, new noise without
  motion vectors.
- **Passes**: legacy (up to 3, or 30 unlocked; each pass its own values) or fast.
- **Composition**: detail strength, colour strength, highlight guard, HDR mapping (ReversibleMode), restore sharpness.
- **Resolution**: model resolution from 25 % to 200 % (above 100 % supersampling, with a choice of filters), how the
  picture is made smaller and how the result is enlarged.
- **Peripheral compression**: the centre at full resolution, the surround compressed; the centre's and the work
  area's size and position.
- **Exposure**: the white point from the game's exposure, set by hand, or metered automatically, with trims and
  highlight protection. On the ReShade routes the picture is already exposed and tone mapped: the white point is set
  by hand only.
- **Skin and environment**: skin protection, and how much detail and colour change is kept on skin and elsewhere.

## Preprocess (optional)

`[Preprocess]` in `dlssnr-amd.ini` in the game folder changes the picture the NR network is shown (exposure,
display curve, contrast, saturation), and so how NR edits the picture. It has two uses: a personal look in any
game, and fixing games that do not hand their exposure to the upscaler (below). The installer writes the
file; NR Control and ReShade's Add-ons page show the settings on their Preprocess page.

- **Enabled**: off by default; when off, none of it runs. Turned on, it starts from auto exposure and the
  filmic curve.
- **Exposure**:
  - `auto`: auto exposure after Unreal Engine's design, for games that compute their exposure but do not hand
    it to the upscaler.
  - `off`: the exposure stays as the game or OptiScaler set it.
  - `fixed`: `ExposureBias` alone.
- **ExposureBias** (EV, -8 to +8, decimals allowed): with `fixed` it is the whole gain; with `auto` it is added
  to what auto works out.
- **Curve, Contrast, Saturation**: change the picture NR is shown, and so how it edits the picture.
- **Hotkey** (Ctrl+F10 by default): switches the preprocess on and off for the current run, to compare on
  the same picture. It does not change the file.
- **Sound**: a short sound when it switches: two notes going up for on, going down for off. `Sound = 0`
  turns it off.

Some games hand the upscaler a picture without its exposure, and NR then works on a frame that is several
stops too dark: 007 First Light turns green and grainy with NR. Such games are fixed by the preprocess: set
`Enabled = 1` (its defaults, auto exposure and the filmic curve, are meant for this).

For a personal look in a game that gives its exposure properly, `auto` is not needed: use `Exposure = off`
(or `fixed` for a small shift) and change `Curve`, `Contrast` or `Saturation`. This departs from the original
look; it may be better or worse.

The first time the preprocess is turned on, NR rebuilds; a second or two of frames go without NR.
The file itself explains every setting.

<a id="int4-mixed-optional"></a>

## CC_SILVER_AARDWOLD_INT4_MIXED (optional)

`CC_SILVER_AARDWOLD` with part of its computation in int4, faster (see Performance): int4 is a lower precision
than the original network uses, so its picture differs somewhat from `CC_SILVER_AARDWOLD`'s. It exists for
`CC_SILVER_AARDWOLD` only. Both Linux packages and the Windows package have it, and
since 0.0.7 it is always installed as the network `CC_SILVER_AARDWOLD_INT4_MIXED`.

On Windows it runs as [ACO's machine code](#windows) only, on RX 9000: the AMD driver's own
compiler makes slow code for the int4 kernels, so when ACO's machine code cannot be used it is greyed out. It
needs no layer or launch options on Windows. The rest of this section is about
Linux; choosing it, the weights and their check work the same way on Windows (`-Model
CC_SILVER_AARDWOLD_INT4_MIXED` in the Windows installer).

- It is one of the networks: choose it as the one a game starts with at install (`--model
  CC_SILVER_AARDWOLD_INT4_MIXED`, or `--int4` as before), or switch to it in game (the drop-down in NR
  Control or on the Add-ons page, or Ctrl+F11 to cycle the networks).
- On the ReShade routes it needs a Vulkan layer in the game folder: besides the main Steam launch options the
  installer always prints a second line with `VK_ADD_LAYER_PATH` and `VK_INSTANCE_LAYERS` for it; use that line to
  use it, and copy the whole line. The OptiScaler route needs no layer since 0.0.8 (the NR plugin adds the device
  extensions it needs).
- The first time it is used its network takes longer to build; until then the picture goes without NR.
- Its weights are made during installation from your model and the tables in the package, and checked
  against a known SHA-256. The tables (`linux/data/int4/`) come from this project's own calibration on
  1,112 game frames and darkened copies of some of them. int4 mixed is the network these tables define;
  another calibration would give a different network.

<a id="cc_small_fp8_experimental-linux-optional"></a>

## CC_SMALL_FP8_EXPERIMENTAL (optional)

`CC_SMALL_FP8` made faster by this project (see Performance). On Windows it runs as ACO's machine code only, on
RX 9000. Its Swin blocks (all but the first)
use ReLU as the MLP activation, and some window-attention layers in the decoder use one fixed attention table per layer (no QK^T,
no softmax) with the V and output projections merged. The weights of the changed layers were fitted to the original
network's outputs. Its picture differs somewhat from
`CC_SMALL_FP8`'s. It is always installed and chosen like the
others (`--model CC_SMALL_FP8_EXPERIMENTAL`, the drop-downs, Ctrl+F11). Its weights are made during installation
from your `CC_SMALL_FP8` model and a difference file in the package (`linux/data/experimental/`, each changed
entry XOR the original, so it holds none of NVIDIA's weights), and checked against a known SHA-256; so it needs
the 310.9.16 DLL.

## Why Vulkan

HIP would work too: ROCm supports RDNA4 and its matrix (WMMA) instructions. Vulkan fits this job
better:

- The network runs on the game's own Vulkan device and queue (DXVK / vkd3d-proton under Proton). The
  frame never leaves that device, and no second GPU context or cross-API synchronisation is needed.
- Nothing extra to install: Vulkan comes with the graphics driver. On Linux the HIP runtime
  (`libamdhip64`) is not part of Mesa and has to be installed separately (ROCm or the distribution's
  packages), plus a bridge to reach it from a game running in Proton (a Windows process). On Windows
  the AMD driver includes it (`amdhip64_7.dll` in current drivers).
- RDNA4's matrix instructions are available in Vulkan through `VK_KHR_cooperative_matrix`, both in
  Mesa (Linux) and in the AMD Windows driver.

How the routes work in detail, and where the code is: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Install (Linux)

Download a `DLSSNR-AMD-Vulkan-Linux-*-x86_64.tar.gz` (64-bit games) or `-i686.tar.gz` (32-bit games) from
the releases, unpack it and run:

```sh
bash install.sh "/path/to/steamapps/common/<game folder>" --dll /path/to/nvngx_dlssnr_2026_10_16.zip
```

The folder is the one holding the game's exe. The installer lists the routes, asks which network a game
starts with (Enter = `CC_SMALL_FP8`; all four are installed and can be switched
in game), which processing NR uses (Enter = the [RenoDX style](#renodx-style); or the
wilsjo2 style) and, on the ReShade routes,
whether to use [LumeniteFX](https://github.com/umar-afzaal/LumeniteFX) Kernel (by Afzaal) for the motion
vectors instead of VORT (downloaded from its author's GitHub and installed unchanged; it is not part of this
project), extracts the model (below) and prints the Steam launch options to use. `--dll` is needed only for the first install:
the extracted model is kept and reused for every later one. Needs `bash` and `python3`. See
[linux/package/README.txt](linux/package/README.txt).

## Install (Windows)

Download `DLSSNR-AMD-Vulkan-Windows-*-preview-x86_64.zip` from the releases, unpack it and double-click
`install.bat`. Pick the game's exe (the one that actually runs, not a launcher), then the route, the processing
(Enter = the [RenoDX style](#renodx-style); or the wilsjo2 style), on the ReShade routes whether to use
[LumeniteFX](https://github.com/umar-afzaal/LumeniteFX) Kernel (by Afzaal) for the motion vectors instead of
VORT (downloaded from its author's GitHub and installed unchanged; it is not part of this project), and which
network a game starts with (Enter = `CC_SMALL_FP8`; all four are installed and can be switched in game). The
first install asks for `nvngx_dlssnr.dll` (310.9.16 or 310.8.0) or a zip that contains it and extracts the
models (below); later installs from the same package do not ask again. Read
[windows/package/README.txt](windows/package/README.txt) and [Windows](#windows)
first.

## The model

The weights are NVIDIA's and are not part of this project. They are extracted from your own copy of
`nvngx_dlssnr.dll`, **version 310.9.16** or **310.8.0**. 310.9.16 already contains the 310.8.0
model, so with it both models are extracted and installed; 310.8.0 gives `CC_SILVER_AARDWOLD` only. Use
310.9.16 when you can.

| File | Model |
| --- | --- |
| `dlssnr-amd/dlssnr-small.bin` | `CC_SMALL_FP8` (310.9.16's default, faster); the default |
| `dlssnr-amd/dlssnr.bin` | `CC_SILVER_AARDWOLD` (the model of 310.8.0; 310.9.16 has it too) |

- Input: the DLL itself, or a zip that contains exactly one `nvngx_dlssnr.dll` (it may be in a
  subfolder).
- A DLL of any other version is refused.
- Every extracted entry (599 for `CC_SILVER_AARDWOLD`, 255 for `CC_SMALL_FP8`) is checked
  against known hashes; a model file is written only if every one matches. On Linux it takes about 40 seconds for 310.9.16 and 20
  seconds for 310.8.0, and needs `bash` and `python3`. The Windows package does the same with
  `model-tools\dlssnr_extract_model.exe` (no Python needed), in about a second.
- The extracted models are kept in the package's own `dlssnr-amd/`. Later installs from that package
  without `--dll` (`-Dll` on Windows) check their SHA256 and install them from there, so the extraction runs only once per
  package. With a new package, use `--dll` once more or copy those files over.
- Every model the package has is installed. Which network runs is `[Network] Model` in `dlssnr-amd.ini`:
  `CC_SMALL_FP8`, `CC_SMALL_FP8_EXPERIMENTAL`, `CC_SILVER_AARDWOLD` or
  `CC_SILVER_AARDWOLD_INT4_MIXED`, set at install (`--model` on Linux, `-Model` on Windows, or the installer's
  question) and switchable in game; a network without its model (`CC_SMALL_FP8` from a 310.8.0 DLL) is greyed
  out in the menus with the reason.

Two ways to run it:

```sh
# while installing: the models go to <game folder>/dlssnr-amd/
bash install.sh "<game folder>" --dll nvngx_dlssnr_2026_10_16.zip

# on its own, into an existing folder: model-tools/ in the package, linux/package/model-tools/ in this repository
bash model-tools/extract_model.sh nvngx_dlssnr_2026_10_16.zip <folder>
```

On success it prints `599 entries, 140.9 MiB` for `dlssnr.bin` (147,756,560 bytes) and
`255 entries, 108.6 MiB` for `dlssnr-small.bin` (113,917,248 bytes). Put the files in
`<game folder>/dlssnr-amd/`, or pass them to a package build with `NR_MODEL=` and `NR_MODEL_SMALL=`.

On Windows, `install.bat` extracts the model by itself; to run the tool on its own:

```bat
model-tools\dlssnr_extract_model.exe nvngx_dlssnr.dll <folder>
```

It takes the DLL itself (unpack it from the zip first) and writes `dlssnr.bin`, and from 310.9.16 also
`dlssnr-small.bin`, into the folder.

<a id="windows-experimental-preview"></a>

## Windows

It is built from the same code as the Linux version and changed only where the AMD Windows driver needs it.
Since 0.0.8.1 it has everything the Linux version has. Some releases may be Linux only.

DirectX 10/11/12 games keep the system's own D3D. NR runs on a Vulkan device of its own on the same
card, and each frame is handed between the two through shared textures and a shared fence, on the
GPU. Vulkan games: NR runs on the game's own device. DirectX 9 games run on Vulkan through DXVK, because D3D9
cannot share a frame with another API.

**ACO's machine code (ACO Mode).** The package carries the network as machine code made in advance with Mesa's
ACO compiler (the Linux driver's compiler), and the AMD driver loads that code instead of compiling the network;
it runs faster than the AMD driver's own compile (see Performance). Since 0.0.8.1 this is not a setting: it is
always tried first. When the driver cannot take it, NR Control and `dlssnr-amd.log` say why, and an RX 9000 runs
the network compiled by the AMD driver instead (without `CC_SMALL_FP8_EXPERIMENTAL` and
[`CC_SILVER_AARDWOLD_INT4_MIXED`](#int4-mixed-optional)); RX 7000 and Radeon 700M / 800M / 8000S graphics have no
NR then. The imported machine code is kept in `dlssnr-amd\aco-cache` in the game's folder, so later starts are
quicker.

**RX 7000 and Radeon 700M / 800M / 8000S graphics (RDNA3), experimental.** `CC_SMALL_FP8` and
`CC_SILVER_AARDWOLD` as ACO's machine code, made and tested by
[TheAutomatic](https://github.com/TheAutomatic) ([#26](https://github.com/mochizuki0323/DLSSNR-AMD/pull/26)),
building on [mauri870](https://github.com/mauri870)'s [DLSSNR-RDNA3](https://github.com/mauri870/DLSSNR-RDNA3).
Tried on few machines: expect more problems than on RX 9000, and for now it may not match the stability and
performance of DLSSNR-RDNA3.

Known issues:

- When ACO's machine code cannot be used (the AMD driver's own compiler) it is slower than the Linux version (see
  Performance).
- The first time NR runs in a game, the network takes a while to build before it takes effect (the driver
  compiles a template of each shader even for ACO's machine code); the OSD shows its progress. Later starts in
  that game are quicker.
  Until then the picture looks as without NR, which does not mean the mod is not working: give it a
  minute.
- DirectX 9 games run entirely on DXVK, which changes their performance and behaviour; overlays such
  as Steam's may not show.
- In DirectX 10 games the game's depth buffer is not passed to NR yet (not implemented).
- NR Control has been tested over DirectX 12 swap chains only; the RenoDX style has not been tested in a
  DirectX 10 game.
- Games whose FSR runs in their own shaders never hand it to OptiScaler: pick the game's DLSS option
  instead, which OptiScaler offers.
- The network pauses itself when video or system memory runs short.
- Without ACO's machine code the network is built by the AMD Windows driver's shader compiler, which rounds
  differently from the Linux version in places; on a 1080p test frame the two outputs are 48.5 dB
  PSNR apart.

## Build

Tested on Ubuntu 24.04. Everything, including the Windows DLLs, is cross-compiled on Linux.

```sh
sudo apt install git curl python3 cmake ninja-build mingw-w64 g++-multilib p7zip-full
bash fetch_deps.sh                          # pinned third-party pieces -> toolchain/, artifacts/ref/
bash linux/build/build_vulkan_loader.sh     # the patched Vulkan loader for the reshade-feeder-vulkan/-dx9 routes
NR_ARCH=x86_64 bash linux/build/build_package.sh
NR_ARCH=i686   bash linux/build/build_package.sh
```

The packages land in `linux/package/`. They contain no model; `install.sh --dll` extracts it. To put
models you extracted yourself into a package for your own use, set `NR_MODEL=/path/to/dlssnr.bin` (and
`NR_MODEL_SMALL=/path/to/dlssnr-small.bin` for `CC_SMALL_FP8`).
Do not share packages that contain the model.

### Windows package

```sh
bash fetch_deps.sh --windows                # adds GE-Proton 11-7 (DXVK)
bash windows/build/build_package.sh
```

The result is `windows/package/DLSSNR-AMD-Vulkan-Windows-*-preview-x86_64.zip`; on Windows, run `install.bat` from it.
It contains no model; the installer extracts it on the first install. `NR_MODEL=/path/to/dlssnr.bin`
(and `NR_MODEL_SMALL=/path/to/dlssnr-small.bin` for `CC_SMALL_FP8`) puts them into the package
for your own use. Do not share packages that contain the model.

## Licence

MIT for the code in this repository, see [LICENSE](LICENSE). The packages also carry third-party
components under their own licences, listed in [THIRD_PARTY.md](THIRD_PARTY.md). NVIDIA's model
weights are NVIDIA's property and are neither included nor distributed.
