DLSSNR-AMD-Vulkan (Linux)
=========================

Runs the neural rendering (NR) model of DLSS 5 in games on AMD graphics cards, under Linux + Proton.
The network is reimplemented in Vulkan and runs on the game's own Vulkan device; NVIDIA's
runtime is neither needed nor called.

Requirements
------------
- RX 9000 series (RDNA4) graphics card
- Mesa 26.2 or newer (RADV driver)
- Proton: tested on GE-Proton 11-9
- python3 (the installer and the model extraction use it)

Install
-------
    bash install.sh "/path/to/steamapps/common/<game folder>" --dll /path/to/nvngx_dlssnr_2026_10_16.zip

The folder is the one that holds the game's exe (the one that actually runs - for Unreal Engine
games it is <project>/Binaries/Win64, not the top folder with the launcher). The script lists
the routes; you can also name one after the folder:

    optiscaler             for games with a DLSS, FSR or XeSS option. OptiScaler does the upscaling and
                           frame generation, this project is its DLSS-NR backend. 64-bit package only.
    optiscaler-renodx      as optiscaler, with the picture before and after NR processed the RenoDX way
                           (see "RenoDX style" below). 64-bit package only.
    reshade-feeder         for D3D10/11/12 games without a usable upscaler. ReShade + VORT (or the
                           optional LumeniteFX) provide the motion vectors.
    reshade-feeder-vulkan  Vulkan games, or D3D9 games (under Proton, DXVK turns D3D9 into Vulkan).
    reshade-feeder-dx9     as reshade-feeder-vulkan, plus the depth direction for old D3D9 games.
    remove                 uninstall.

The earlier route names reshade, vulkan and dx9 are now reshade-feeder, reshade-feeder-vulkan and
reshade-feeder-dx9.

When it is done the script prints the line for the Steam launch options, for example:

    WINEDLLOVERRIDES="dxgi=n,b" %command%

On optiscaler / optiscaler-renodx this line also has VK_ADD_LAYER_PATH and VK_INSTANCE_LAYERS, which
the NR Control menu (see "Use") needs; copy the whole line.

Use the i686 package for 32-bit games and the x86_64 package for 64-bit games.

Networks: all three are installed, named as in NVIDIA's DLL:
    CC_SMALL_FP8                   the default network of 310.9.16, the fastest (default)
    CC_SILVER_AARDWOLD             the network of 310.8.0 (also preset 1 of 310.9.16)
    CC_SILVER_AARDWOLD_INT4_MIXED  CC_SILVER_AARDWOLD with part of the work in int4 and other lower
                                   precisions; faster, the picture differs somewhat
The installer asks once which one a game starts with (Enter = CC_SMALL_FP8; not asked when not run in
a terminal), or give --model <name> (the earlier --new-model, --old-model and --int4 still work;
--no-int4 is no longer needed). This only sets [Network] Model in dlssnr-amd.ini; the network can be
switched in game at any time: the drop-down in NR Control (End) or on the Add-ons page, or Ctrl+F11 to
cycle. The new network is prepared in the background, and the current one keeps running until it is
ready. A network that cannot run is greyed out in the drop-down with the reason.
CC_SILVER_AARDWOLD_INT4_MIXED needs a Vulkan layer: besides the main launch line the installer always
prints a second one with that layer (VK_ADD_LAYER_PATH and VK_INSTANCE_LAYERS added); use that line to
use it, and copy the whole line. When games start with it, the main line is that one. Its weights are
made at install; if that fails the other two are installed and it is greyed out in the menus. The
first time it is used its network takes longer to build, and until then the picture goes without NR.

LumeniteFX (optional, reshade-feeder routes only): motion vectors from LumeniteFX Kernel (by Afzaal
(Kaidō), https://github.com/umar-afzaal/LumeniteFX, AGNYA licence). It is not part of this project and
not in this package. With --lumenite the installer downloads the version this package was tested with
from its author's GitHub and installs it unchanged into the game folder (the downloaded zip is kept in
this package's lumenite/, so later installs do not download again); or download it yourself (Code >
Download ZIP on its GitHub page) and install with --lumenite <zip>, which installs the version in the
zip. Without either, the installer asks once (Enter = no).
After installing, Motion vectors on the Add-ons page switches between LumeniteFX Kernel and VORT
(ReShade compiles its effects again, and until that is done the picture goes without NR; NR's history
starts again); it is MotionVectors in dlssnr-amd.ini. At every game start the add-on checks that
LumeniteFX works (compiled, on, in order, its flow textures, its motion vectors); if anything fails,
VORT runs that time, the reason is on the Add-ons page and in dlssnr-amd.log, and the next start tries
again.

The model
---------
The weights are NVIDIA's and are not distributed with this project. They are extracted from your
own copy of nvngx_dlssnr.dll, version 310.9.16 or 310.8.0:

    dlssnr-amd/dlssnr-small.bin  CC_SMALL_FP8 (the default network of 310.9.16)
    dlssnr-amd/dlssnr.bin        CC_SILVER_AARDWOLD (the network of 310.8.0; 310.9.16 has it too), also
                                 the source of CC_SILVER_AARDWOLD_INT4_MIXED

- use 310.9.16 when you can: it already contains the 310.8.0 network, so with it both models are
  extracted and installed; 310.8.0 gives CC_SILVER_AARDWOLD only (CC_SMALL_FP8 is then greyed out in
  the menus);
- give --dll either the DLL itself or a zip that contains exactly one nvngx_dlssnr.dll
  (it may sit in a subfolder of the zip);
- a DLL of any other version is refused before anything is installed;
- every extracted entry (599 for the old model, 255 for the new one) is checked against known
  hashes, and a model file is written only if all of them match. This takes about 40 seconds for
  310.9.16 and 20 seconds for 310.8.0.

The result is dlssnr-amd/dlssnr-small.bin (113,917,248 bytes) and dlssnr-amd/dlssnr.bin
(147,756,560 bytes) in the game folder.

This is needed only once: the extracted models are also kept in this package's dlssnr-amd/,
and later installs from this package without --dll check their SHA256 and install them from
there. With a new package, use --dll once more, or copy those two files over.

To make the model files without installing anything (into an existing folder):

    bash model-tools/extract_model.sh /path/to/nvngx_dlssnr_2026_10_16.zip <folder>

It prints "599 entries, 140.9 MiB" for dlssnr.bin and "255 entries, 108.6 MiB" for
dlssnr-small.bin when it succeeds. The files can then be copied to <game folder>/dlssnr-amd/ by hand.

After installation the game folder has
--------------------------------------
    dlssnr-amd/                  model and shaders; the run-time pipeline cache goes here too; on the
                                 OptiScaler route also NR Control's Vulkan layer (control/)
    dlssnr-amd.ini               settings, written at install
    dlssnr-amd-install.txt       the installation record, which remove deletes by
plus the route's own files (OptiScaler's or ReShade's DLLs, ini files, shaders).
Screenshots of the RenoDX style go to NR Screenshots/ (made at the first screenshot, not removed by
uninstall).

Installing again (another route, another package) first uninstalls by
the installation record, and dlssnr-amd.ini is replaced by a fresh default file: earlier settings are
not kept, back the file up if you need them.

Use
---
optiscaler / optiscaler-renodx: turn on DLSS, FSR or XeSS in the game; Insert opens the OptiScaler
            menu. End opens NR Control, this project's own NR menu: it shows what NR is doing (frame
            and network sizes, passes, NR's cost split into the network and the rest of the
            processing), selects the network, adjusts Preprocess and the settings under "Networks and
            other settings" below, and shows the data the game hands NR (Engine data); on
            optiscaler-renodx it also holds every NR setting (see "RenoDX style" below). Changes are
            written to dlssnr-amd.ini at once and apply in the game within about a second (except those
            applied after a game restart). While the menu is open it takes the keyboard and mouse; End
            or Esc closes it.
            On the optiscaler route the other NR settings are on the DLSS Neural Rendering page of
            the OptiScaler menu.
            Works on X11 (XWayland, Proton's default) and Wayland (PROTON_ENABLE_WAYLAND=1). On
            Wayland the menu reads the keyboard and mouse through the game window; a game that uses
            only raw input may still receive input while the menu is open.
            While the menu is closed it draws nothing and submits no GPU work (except the few seconds
            the network's name shows in a corner, see OSD).
            If the NR page keeps showing "Waiting for the upscaler to run", try
            [Spoofing] Dxgi=false in OptiScaler.ini (Dying Light: The Beast has needed it).
reshade-feeder routes: Home opens ReShade, the settings are on the Add-ons page; the same
            settings are kept in dlssnr-amd.ini in the game folder and changes apply live.
            Model resolution and Model passes of 5 or more rebuild the network; for the few
            seconds of a rebuild the picture does not go through NR. Switching Model passes between
            1 and 2 or more also rebuilds in the background; until it is done NR keeps running with
            the previous number of passes.
            NoiseEveryFrame (default false): on frames without motion vectors (DLSS5_MV not bound) the network's noise seed changes every frame, as in
            NVIDIA's original; with no previous frame to average the noise, a still picture shimmers
            slightly. false: a fixed seed.
            Below 100% Model resolution, Enlargement chooses how the model's result is enlarged
            back to full resolution: Matched residual (default) and Edge-aware lighting + colour
            enlarge the model's change and apply it to the full-resolution picture; Classic
            enlarges the model's output picture directly (scalers to choose from, FSR 1 among
            them).
            HDR: scRGB (linear) HDR is supported; HDR10 (PQ/HLG) pictures are not handled on these
            routes, set the game to SDR or scRGB. With the RenoDX style on, HDR10 (PQ) is handled too,
            HLG is not.

Networks and other settings ([Network] in dlssnr-amd.ini; in NR Control and on the Add-ons page under
Network model and options):
  - Residency: what is kept of a network that is no longer running. The three networks share one
    activation memory, so what is kept is mainly the weights.
        auto      only the weights stay in video memory, switching back is quick; when video memory
                  runs short they move to RAM, and are freed when RAM runs short too (default)
        resident  the whole network stays in video memory; switching back is instant, uses the most
                  video memory
        offload   the weights move to RAM and all video memory is freed; switching back copies them
                  back first
        minimal   video memory and RAM are freed; switching back builds the network again
    With NR off for more than 10 seconds, offload and minimal treat the running network so too; auto
    only when video memory runs short.
  - OSD: the running network shown in a corner of the screen. switch (default) = while switching, with
    the progress, then for 3 seconds; always = all the time; off = never.
  - Resize History (default 0): what happens to NR's previous result when the game's render size
    changes (dynamic resolution); only matters with NR before the upscaler. 0 = dropped, NR starts
    again at the new size, as NVIDIA's DLL does; 1 = scaled to the new size and kept. In our tests 1
    is steadier and closer to the picture at a constant size when the size changes often, but it is
    not what NVIDIA's DLL does; most DLSS games jitter their input, which hides most of the flicker of
    a restart, so the default is 0.
  - Style Grade (default 1): the grade of the Natural and Cinematic styles. 1 = as NVIDIA's DLL, the
    picture is graded for the style after the network; 0 = the picture of 0.0.6 and earlier (without
    that step), for those who prefer it. Standard is not affected; applied at once.
  - Motion Without Depth (default 1): 1 = the network fetches the previous frame with the pixel's own
    motion vector, depth not used, as NVIDIA's DLL does; 0 = with depth, the motion vector of whichever
    of the pixel and its four diagonal neighbours is nearest the camera. Only the edges of moving
    objects change; applied after a game restart.
  - Hotkey, Sound: the hotkey that cycles the networks (default Ctrl+F11) and its sound.
  - [Log]: whether dlssnr-amd.log is written and whether it is cleared at every start; applied after a
    game restart.
Dynamic resolution: with NR before the upscaler and a render size that keeps changing, every new
size is prepared in the frame it appears in (except the first range of sizes seen), so NR is no
longer skipped for stretches of frames; its video memory is bounded.
Engine data (NR Control and the Add-ons page): what the network actually gets, for troubleshooting -
the colour format and HDR, the encoding the network sees, the exposure (who computes it, the white
point or divisor), the motion vectors (size, sampling, scale), size changes, resets. What the game does
not hand over but something else covers says who covers it; with no exposure from anywhere, a hint
says to try Preprocess with Exposure = auto if the picture looks wrong. Depth is shown only with
Motion Without Depth = 0 (NVIDIA's DLL does not use depth). The name at the start of each line has a
colour: green = from the game, yellow = not from the game but filled in (what fills it in is on the
line), orange = not from the game and nothing fills it in.

Preprocess (optional, off by default): [Preprocess] in dlssnr-amd.ini in the game folder (written
at install; the ReShade routes also show it on the Add-ons page). It changes the picture the
network is shown (exposure, display curve, contrast, saturation), and so how NR edits the picture.
Two uses: a personal look in any game (it departs from the original look; the result may be better
or worse), and games that do not hand their exposure to the upscaler, which it fixes (see 007 First
Light below). Ctrl+F10 switches it for the current run, to compare. The file explains every
setting.

RenoDX style (optional, off by default): the processing around NR (how the picture is handed to NR
and how NR's result is put back into the picture) follows RenoDX's DLSS5 add-on v8.5.0-rc10, with
RenoDX's own shaders (MIT, licence in dlssnr-amd/shaders/renodx/RenoDX-LICENSE.txt). The NR network
itself is the same.
  - Switch: [RenoDX] Enabled in dlssnr-amd.ini, applied after a game restart. optiscaler-renodx
    installs it on; on the reshade-feeder routes set it to 1 (the Add-ons page has the switch too).
  - With it on, every NR setting is in [RenoDX]: on optiscaler-renodx in NR Control (End), on the
    reshade-feeder routes on the Add-ons page, or edit the file. Apart from Enabled, changes apply in
    the running game. The file explains every setting, and so does (?) in the menus. The settings
    keep RenoDX's own names, defaults and ranges.
  - optiscaler-renodx (with the RenoDX style on): OptiScaler only does the upscaling and frame
    generation (its upscaler, sharpening and frame generation settings work as usual); the settings
    on its DLSS Neural Rendering page have no effect (fixed while running: NR on, 1 pass, Model
    resolution 100%, Peripheral compression / Finished Picture / Deferred / debug views off).
    reshade-feeder routes: the NR settings in [DlssNr] have no effect.
  - NR on/off (NeuralUplift; F6 in game switches it for the current run), the network's settings
    (Intensity, Style, Local tone, Local structure, Skin structure, Character mask), NR before or
    after the upscaler (NRHookPoint, OptiScaler route only; always after it when the game uses Ray
    Reconstruction), the network's resolution (NRResolutionScale).
  - Passes (NRPasses 1-4): each pass hands the previous pass's result to the network again. Passes
    2, 3 and 4 each have their own strengths (Transfer / Color) and network settings; with "Same as
    pass 1" on, the network settings follow pass 1, Intensity is always the pass's own.
    NRUnlockPasses = 1 allows up to 30 passes (as OptiScaler's UnlockPasses; RenoDX itself stops at
    4); passes 5 and later take pass 4's settings. Every pass runs the network once more, which
    lowers the frame rate a lot.
  - Adjust NR's look (NRLookMode and NRLook*): adjusts how much NR changed, directly on its result -
    brighten, darken, colour, hue, shadows / midtones / highlights, broad lighting and detail, dark
    halos, temporal smoothing. Off by default; off leaves the picture unchanged.
  - Screenshot (F5, also a button in NR Control and on the Add-ons page): the same frame without
    and with NR, saved to NR Screenshots/ in the game folder. By default the final picture: on the
    reshade-feeder routes the picture sent to the display; on optiscaler-renodx NR moves to
    OptiScaler's finished picture for one frame and moves back afterwards. HDR pictures are saved
    as 16-bit PQ PNG (with cICP and ICC, so they stay HDR), SDR as 8-bit sRGB. JPEG, both PNG and
    JPEG, or an extra side-by-side picture can be chosen; NRScreenshotSource = 2 saves the model's
    input and output (for diagnosis, without the game's tone mapping and UI). Nothing is saved
    while NR is off.
  - The automatic mask (NRAutoMask) is off by default, as in RenoDX (the Character mask in
    OptiScaler's menu on the optiscaler route is on by default); when it is on, skin follows only
    Skin structure by default (NRSkinIndependent, as RenoDX).
  - Preprocess can be used with it.
  - On Vulkan games on the OptiScaler route the automatic mode does not use the game's exposure (the
    game's AutoExposure flag is not available there) and computes it from the picture;
    NRFeedMode = 2 forces the game's exposure.

Logs: dlssnr-amd.log (and OptiScaler.log or ReShade.log), all in the game folder.

Known issues
------------
- 32-bit games at 4K: on the first launch the 32-bit address space is tight (ReShade compiles its
  shaders at the same time). If dlssnr-amd.log shows out of host memory, NR retries every few
  seconds by itself; if it never succeeds, please send us the log.
- 007 First Light (optiscaler route): with NR before or after the upscaler the picture turns
  dark, green and grainy. The game leaves exposure to the upscaler and hands over none, so the
  frame NR gets is far too dark. Turn on Preprocess (above); its defaults are meant for this.
  The older workarounds, lowering Paper white on the DLSS Neural Rendering page (about 0.06;
  [DlssNr] WhitePointScale) or [DlssNr] FinishedPicture=true, are no longer recommended.

Uninstall
---------
    bash install.sh "/path/to/<game folder>" remove

Only the files in the installation record are deleted (dlssnr-amd.ini and the logs included). A file
of the game folder that the installation overwrote (for example another mod's dxgi.dll) is not
brought back.
