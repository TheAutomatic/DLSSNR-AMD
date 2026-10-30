DLSSNR-AMD-Vulkan (Windows)
===========================

Runs the neural rendering (NR) model of DLSS 5 in games on AMD graphics cards. The network is
reimplemented in Vulkan; NVIDIA's runtime is neither needed nor called.

It is slower than the Linux version and updated less often, and some releases may be Linux
only. If something breaks, uninstall with option 5 of install.bat.

DX10/11/12 games keep the system's own D3D; nothing is translated. NR runs on a Vulkan device
of its own on the same graphics card, and the frame is handed between the two through shared
textures and a shared fence, all on the GPU. Only DX9 games are moved to Vulkan with DXVK
(D3D9 cannot share a frame with another API). Vulkan games: NR runs on the game's own device.

Requirements
------------
- RX 9000 series (RDNA4): every network
- RX 7000 series, Radeon 700M / 800M / 8000S graphics (RDNA3 / RDNA3.5): CC_SMALL_FP8 and
  CC_SILVER_AARDWOLD (see "ACO Mode")
- AMD Software 26.9 or newer
- 64-bit games
- Do not use it in online games with anti-cheat; start games with EasyAntiCheat with EAC off

Install
-------
Double-click install.bat, pick the game's exe (the one that actually runs - for Unreal Engine
games it is under Binaries\Win64, not the launcher outside), then pick a route:

  1) optiscaler             the game has a DLSS, FSR or XeSS option (DX11 / DX12 / Vulkan). OptiScaler
                            does the upscaling and frame generation (the original OptiScaler, its files
                            unchanged); this project's NR runs as an OptiScaler plugin.
  2) reshade-feeder         other DX10/11/12 games (ReShade + VORT, or the optional LumeniteFX, provide the
                            motion vectors)
  3) reshade-feeder-dx9     old DX9 games (ReShade)
  4) reshade-feeder-vulkan  Vulkan games (ReShade)
  5) Uninstall
  6) Collect logs           when something goes wrong: writes a zip to the desktop to attach to an
                            issue

You can also drag the game's exe onto install.bat. Games under Program Files ask for
administrator rights. On the command line: install.bat <game exe or folder> <route> [-Dll <path>]
[-Model <network>] [-RenoDX on|off] [-Lumenite on|off]. The old route names reshade,
dx9 and vulkan are now reshade-feeder, reshade-feeder-dx9 and reshade-feeder-vulkan; optiscaler-renodx
is the same as optiscaler.

Then the installer asks two questions:

The processing before and after NR (the NR network itself is the same; -RenoDX on|off):
  1) RenoDX style (default)  processed the way RenoDX's DLSS5 add-on does it
  2) wilsjo2 style           the processing of OptiScaler-NR (wilsjo2), ported (see "wilsjo2 style")
In game NR Control (End) or ReShade's Add-ons page switches it at any time.

Networks: all four are installed, named as in NVIDIA's DLL (CC_SMALL_FP8_EXPERIMENTAL is this
project's). The installer asks which one a game starts with (Enter = CC_SMALL_FP8; -Model <name>):

    CC_SMALL_FP8                   the default network of 310.9.16 (default)
    CC_SMALL_FP8_EXPERIMENTAL      experimental: CC_SMALL_FP8 made faster; its picture differs
                                   somewhat. RX 9000 only, with ACO Mode
    CC_SILVER_AARDWOLD             the network of 310.8.0 (also preset 1 of 310.9.16)
    CC_SILVER_AARDWOLD_INT4_MIXED  CC_SILVER_AARDWOLD with part of the work in int4 and other lower
                                   precisions; faster, the picture differs somewhat. RX 9000 only,
                                   with ACO Mode

This only sets [Network] Model in dlssnr-amd.ini; the network can be switched in game at any time:
the drop-down in NR Control (End) or on ReShade's Add-ons page, or Ctrl+F11 to cycle. The new network
is prepared in the background, and the current one keeps running until it is ready. A network that
cannot run is greyed out in the drop-down with the reason (on an RX 7000, for example, the two that
need an RX 9000). The weights of CC_SMALL_FP8_EXPERIMENTAL and CC_SILVER_AARDWOLD_INT4_MIXED are made
at install from your models; if that fails the other networks are installed and the one that failed
is greyed out.

LumeniteFX (optional, reshade-feeder routes only): motion vectors from LumeniteFX Kernel (by Afzaal
(Kaidō), https://github.com/umar-afzaal/LumeniteFX, AGNYA licence). It is not part of this project and
not in this package. On the ReShade routes the installer asks once (Enter = no); with y it downloads
the version this package was tested with from its author's GitHub and installs it unchanged into the
game folder (the downloaded zip is kept in this package's lumenite\, so later installs do not download
again). On the command line: -Lumenite on, or -LumeniteZip <zip> with a zip you downloaded yourself
(Code > Download ZIP on its GitHub page), which installs the version in the zip.
After installing, Motion vectors on the Add-ons page switches between LumeniteFX Kernel and VORT
(ReShade compiles its effects again, and until that is done the picture goes without NR; NR's history
starts again); it is MotionVectors in dlssnr-amd.ini. At every game start the add-on checks that
LumeniteFX works; if anything fails, VORT runs that time, the reason is on the Add-ons page and in
dlssnr-amd.log, and the next start tries again.

Model
-----
The package does not contain NVIDIA's model. The first install asks for nvngx_dlssnr.dll, version
310.9.16 or 310.8.0, or a zip that contains it (on the command line: -Dll):

    dlssnr-amd\dlssnr-small.bin  CC_SMALL_FP8 (the default network of 310.9.16), also the source of
                                 CC_SMALL_FP8_EXPERIMENTAL
    dlssnr-amd\dlssnr.bin        CC_SILVER_AARDWOLD (the network of 310.8.0; 310.9.16 has it too), also
                                 the source of CC_SILVER_AARDWOLD_INT4_MIXED

Use 310.9.16 when you can: it already contains the 310.8.0 network, so with it both models are
extracted and installed; 310.8.0 gives CC_SILVER_AARDWOLD and its int4 mixed only (CC_SMALL_FP8 and
CC_SMALL_FP8_EXPERIMENTAL are then greyed out in the menus). The installer extracts the models with
model-tools\dlssnr_extract_model.exe (it only reads the weight data; the DLL is never loaded or run),
checks every entry against known hashes and writes a model file only if all of them match. The models
stay in this package, so later installs from it (into other games too) do not ask again; to add
CC_SMALL_FP8 to a package that has only CC_SILVER_AARDWOLD, install again with -Dll pointing at the
310.9.16 DLL. With a newer package, choose the DLL once more or copy those files over.

ACO Mode
--------
The network runs as machine code made in advance by the Linux driver's compiler (ACO), shipped in the
package and loaded by the AMD driver as it is, instead of the driver compiling the network itself; it is
faster than the driver's own compile. This is not a setting: it is always tried first. When the driver
cannot take it (a driver without pipeline binaries, for example), the reason is shown under "Network model
and options" in NR Control and on the Add-ons page and written to dlssnr-amd.log, and:
  - an RX 9000 runs the network compiled by the AMD driver instead (CC_SMALL_FP8_EXPERIMENTAL and
    CC_SILVER_AARDWOLD_INT4_MIXED are greyed out then);
  - an RX 7000 / Radeon 700M - 8000S has no NR (their network exists as ACO's machine code only).
The same place shows "ACO Mode: on in this run" and for which graphics while it is in use. A failure that
is not ACO's own (video memory running short, for example) does not turn it off. The RDNA3 machine code was made
and tested by TheAutomatic (https://github.com/TheAutomatic), building on mauri870's DLSSNR-RDNA3
(https://github.com/mauri870/DLSSNR-RDNA3).

Use
---
optiscaler: turn on DLSS, FSR or XeSS in the game's settings; Insert opens the OptiScaler menu
            (upscaling and frame generation only).
            End opens NR Control, this project's NR menu, which holds every NR setting (another key: the
            Hotkeys page, [NRControl] MenuKey in dlssnr-amd.ini):
            - The sidebar's pages: NR settings - the processing (the RenoDX or the wilsjo2 style, see below),
              the network ([Network] Model or one of its own), the place (Present, after the upscaler,
              before it), all its settings and Engine data (what NR gets, see below); Network - the
              networks and their options (see below), and at its foot the Layers switch ([NRControl]
              Layers, off by default); Preprocess (see below); HUD FIX; Hotkeys (every key NR reads: click
              a key, then press the new one); Screenshot (F5); Compare; Diagnostics; NR Control (menu size,
              text size, colours, language); Status.
            - The header: API, graphics card, driver and the Neural Rendering switch (F6: NR off or on for
              this run, not saved). The footer: NR's cost and the upscaling.
            - Status: what NR is doing - one line a layer (processing, where it runs, network, size,
              passes, cost split into the network itself and the processing around it; the column headers
              explain on hover), then the sums; video memory (this project's by network and use, and what
              the whole process uses), with Video memory, in detail for the breakdown.
            - Layers (only for special cases; for ordinary stacking use Passes): with the switch on, NR
              can be several layers on the NR chain page, each with its own processing, network, place and
              settings and its own page. Layers at one place run in layer order; the places run before
              the upscaler, after it, then at Present. Every layer runs the network once more.
            - Diagnostics: Copy a report copies the layers' state, video memory, dlssnr-amd.ini and the
              log's last lines to the clipboard and saves them as dlssnr-amd\nr-report.txt (nothing is
              sent anywhere); the residency and an event timeline.
            - Compare ([Inspect] in dlssnr-amd.ini): NR off against NR on, on the screen - side by side
              or with a split line (sides can be swapped, NR off / NR on tags), on the final picture or
              one layer; with Apply NR off the layers run as usual but their result is not shown. For a
              chosen layer also: the network's input, its output, its change x20 (Debug view), the skin
              weight (wilsjo2 layers), a held frame (the layer keeps processing one frame, for tuning),
              the spatial compression's work area and centre band (wilsjo2 layers).
            - Language: the menus' texts come from a language file in dlssnr-amd\lang (see the
              README.txt there); English.txt is made from the program.
            Changes are written to dlssnr-amd.ini at once and apply in the game within about a second
            (except those marked as applied after a game restart). While the menu is open it takes the
            keyboard and mouse; its key or Esc closes it. A game that uses only raw input may still receive
            input while the menu is open. The menu is drawn over the game's picture (DX11 / DX12 /
            Vulkan); while it is closed it draws nothing (except the few seconds the network's name shows
            in a corner, and the tags and spatial compression frames turned on in Compare).
            Present: NR runs on the game's finished picture, before OptiScaler's frame generation (the
            generated frames are made from the picture after NR). HUD FIX (its own page, off by
            default) keeps the game's HUD as it is: from the HUD-less frame a game hands to frame
            generation, a picture taken before the game draws its HUD, or an estimate.
            If NR Control keeps waiting for the game's upscaler, set [Spoofing] Dxgi=false in
            OptiScaler.ini and try again.
            A layer placed at Present runs only at Present, never moved before or after the upscaler;
            when Present cannot serve it (for example a screen colour format it does not support), the
            layer does not run and its line in the menu says why.
reshade-feeder routes: Home opens ReShade; the NR settings are on the Add-ons page, laid out as NR
            Control:
            - The same pages as NR Control (NR settings, Network with the Layers switch, Preprocess, HUD FIX,
              Hotkeys, Screenshot, Compare, Diagnostics, Status), plus Motion vectors (VORT or LumeniteFX
              Kernel) and Language (with Text size); the menu's size and font are ReShade's own settings.
            - NR settings: the processing (the wilsjo2 style = these routes' original processing, or the
              RenoDX style, see below), the network and all its settings; on these routes NR always runs
              at Present.
            - Status: one line a layer, then the sums; whether DLSS5_Feed and its motion vectors and depth
              were found; video memory, with Video memory, in detail for the breakdown.
            - Layers (switch on): several layers, run on the picture in layer order; every layer runs the
              network once more, and layers with different settings each take their own network memory.
            - Compare and Diagnostics: as in NR Control (screenshots, F5, are taken by the RenoDX style's
              layers).
            Changes are written to dlssnr-amd.ini in the game folder at once and apply (except those
            marked as applied after a game restart). A dlssnr-amd.ini without [LayerN] sections (as the
            installer writes it) has one layer: the RenoDX style with [RenoDX] Enabled = 1 (the [RenoDX]
            settings), else the wilsjo2 style (the [DlssNr] settings); its first change on the page
            turns it into a [Layer1] section, after which these NR settings in [DlssNr] / [RenoDX] no
            longer apply; removing every layer goes back to them.
            Model resolution and Passes of 5 or more rebuild the network; for the few seconds of a
            rebuild the picture does not go through NR. Below 100% Model resolution, Enlargement
            (Transfer) chooses how the model's result is enlarged back to full resolution; Model
            resolution goes up to 200% (supersampling). Passes 1..3, up to 30 with Unlock passes.
            HDR: scRGB (linear) and HDR10 (PQ) are supported; HLG pictures are not handled on these
            routes, set the game to SDR, scRGB or HDR10.
New noise every frame: the network's noise seed changes every frame, as in NVIDIA's original. RenoDX
layers (NRNoiseEveryFrame): on by default. wilsjo2 layers (NoiseEveryFrame): only on frames without
motion vectors; on by default on the OptiScaler route, off on the ReShade routes. Without motion
vectors there is no previous result to average the noise, so with it on a still picture shimmers
slightly.

Networks and other settings ([Network] in dlssnr-amd.ini; in NR Control and on the Add-ons page under
Network model and options):
  - Residency: what is kept of a network that is no longer running. The networks share one activation
    memory, so what is kept is mainly the weights.
        auto      only the weights stay in video memory, switching back is quick; when video memory
                  runs short they move to RAM, and are freed when RAM runs short too (default)
        resident  the whole network stays in video memory; switching back is instant, uses the most
                  video memory
        offload   the weights move to RAM and all video memory is freed; switching back copies them
                  back first
        minimal   video memory and RAM are freed; switching back builds the network again
    With NR off for more than 10 seconds, offload and minimal treat the running network so too; auto
    only when video memory runs short.
  - OSD: the running network shown in a corner of the screen (one line a layer with several layers).
    switch (default) = while switching, with the progress, then for 3 seconds; always = all the time;
    cost = all the time, with each layer's cost split into the network and its processing, and the
    sums; off = never.
  - Resize History (default 0): what happens to NR's previous result when the game's render size
    changes (dynamic resolution); only matters with NR before the upscaler. 0 = dropped, NR starts
    again at the new size, as NVIDIA's DLL does; 1 = scaled to the new size and kept.
  - Style Grade (default 1): the grade of the Natural and Cinematic styles. 1 = as NVIDIA's DLL, the
    picture is graded for the style after the network; 0 = the picture of 0.0.6 and earlier (without
    that step). Standard is not affected; applied at once.
  - Motion Without Depth (default 1): 1 = the network fetches the previous frame with the pixel's own
    motion vector, depth not used, as NVIDIA's DLL does; 0 = with depth, the motion vector of whichever
    of the pixel and its four diagonal neighbours is nearest the camera. Applied after a game restart.
  - Hotkey, Sound: the hotkey that cycles the networks (default Ctrl+F11) and its sound.
  - Memory Guard (default 1): a network is built only while 1 GB of video memory stays free for the game, and the
    running one steps aside while video memory runs out. 0 = no such check: NR builds and runs in whatever video
    memory is left; a game left short of it may stutter or fail to load its own data. Applied at once.
  - [Log]: whether dlssnr-amd.log is written and whether it is cleared at every start; applied after a
    game restart.
Dynamic resolution: with NR before the upscaler and a render size that keeps changing, every new
size is prepared in the frame it appears in (except the first range of sizes seen), so NR is no
longer skipped for stretches of frames; its video memory is bounded.
Engine data (on the NR settings page in NR Control and on the Add-ons page; with layers on, each layer's
page): what the network actually gets, for troubleshooting -
the colour format and HDR, the encoding the network sees, the exposure (who
computes it, the white point or divisor), the motion vectors (size, sampling, scale), size changes,
resets. What the game does not hand over but something else covers says who covers it; with no
exposure from anywhere, a hint says to try Preprocess with Exposure = auto if the picture looks wrong.
The name at the start of each line has a colour: green = from the game, yellow = not from the game but
filled in (what fills it in is on the line), orange = not from the game and nothing fills it in.

Preprocess (optional, off by default): [Preprocess] in dlssnr-amd.ini in the game folder (written
at install; NR Control and the Add-ons page show it on their Preprocess page). It changes the picture
the network is shown (exposure, display curve, contrast, saturation), and so how NR edits the picture. Two uses: a
personal look in any game (it departs from the original look; the result may be better or worse), and
games that do not hand their exposure to the upscaler, which it fixes (see 007 First Light below).
Ctrl+F10 switches it for the current run, to compare. The file explains every setting.

RenoDX style: the processing around NR (how the picture is handed to NR and how NR's result is put
back into the picture) follows RenoDX's DLSS5 add-on v8.5.0-rc10, with RenoDX's own shaders (MIT,
licence in dlssnr-amd\shaders\renodx\RenoDX-LICENSE.txt). The NR network itself is the same. It is
the default processing on every route (the NR settings page chooses it; with layers on, each layer).
  - The settings keep RenoDX's own names, defaults and ranges; the file explains every setting, and so
    does (?) in the menus. Apart from Enabled, changes apply in the running game.
  - NR on/off (NeuralUplift; F6 in game switches it for the current run, all layers together), the
    network's settings (Intensity, Style, Local tone, Local structure, Skin structure, Character mask),
    the network's resolution (NRResolutionScale). On the optiscaler route NR's place is the layer's
    Placement, by default Present.
  - Passes (NRPasses 1-4): each pass hands the previous pass's result to the network again; passes 2, 3
    and 4 each have their own settings. NRUnlockPasses = 1 allows up to 30 passes. Every pass runs the
    network once more, which lowers the frame rate a lot. Passes (fast): the network runs once a frame
    and its change is strengthened instead (methods Extrapolation, Feedback, Hybrid, Edit strength,
    each with its own settings).
  - Adjust NR's look (NRLookMode and NRLook*): adjusts how much NR changed, directly on its result. Off
    by default; off leaves the picture unchanged.
  - Screenshot (F5, also a button in NR Control and on the Add-ons page): the same frame without and
    with NR, saved to NR Screenshots\ in the game folder. HDR pictures are saved as 16-bit PQ PNG (with
    cICP and ICC, so they stay HDR), SDR as 8-bit sRGB. Nothing is saved while NR is off.
  - The automatic mask (NRAutoMask) is off by default, as in RenoDX.

wilsjo2 style: the processing around NR of OptiScaler-NR (wilsjo2's OptiScaler fork, which the
optiscaler route used up to 0.0.7), ported to this project (after its method, none of its code). The
settings keep its [DlssNr] names, defaults and ranges: white point and exposure (the game's exposure,
manual, automatic metering), the HDR curve (ReversibleMode), detail and colour strength, the brightness
guard, skin and environment protection, passes and each pass's own settings, the model resolution
(WorkingScale, 0.25..2, above 1 is supersampling) and the enlargement (Transfer), spatial compression.
On the reshade-feeder routes these are the routes' original settings ([DlssNr]); those routes get the
game's finished picture, already exposed and tone mapped, so the white point is manual only.

The first time NR runs in a game the network has to compile; it takes effect after about a
minute. Until then the picture looks as without NR, which does not mean the mod is not working:
give it a minute (the OSD shows the progress). After that it is cached. This happens once for each
game.

Files put into the game folder
------------------------------
    dlssnr-amd\                  models and shaders; on the optiscaler route also the wilsjo2 style's
                                 shaders and nr_present_probe.exe, which NR Control uses
    dlssnr-amd-install.txt       the installation record, which uninstall follows
    dlssnr-amd.ini               settings, written at install
    optiscaler route:            dxgi.dll (that is OptiScaler.dll), OptiScaler.ini and OptiScaler\ (the NR
                                 plugin is OptiScaler\plugins\dlssnr-amd.asi, OptiScaler's licences are in
                                 OptiScaler\Licenses\)
    reshade-feeder (DX10/11/12): dxgi.dll (ReShade), dlssnr_amd.addon64, ReShade's settings and
                                 shaders
    reshade-feeder-dx9:          d3d9.dll dxgi.dll (DXVK), vulkan-1.dll (Vulkan loader; never calls
                                 DXGI and loads the ReShade layer from the game folder), ReShade's files
    reshade-feeder-vulkan:       vulkan-1.dll and ReShade's files
Files of the same name already in the game folder are first moved to dlssnr-amd-backup\ and
put back on uninstall. Screenshots go to NR Screenshots\ (made at the first screenshot, not removed
by uninstall).
Installing again (another route, another package) first uninstalls by the installation record,
and dlssnr-amd.ini is replaced with a new default file: earlier settings are not kept, so back
the file up first if you need them.

Logs: dlssnr-amd.log, and OptiScaler.log or ReShade.log, all in the game folder. Option 6 of
install.bat collects them into a zip on the desktop.

Known limits
------------
- DX9 games run entirely on DXVK; the frame rate can differ from native D3D9, and overlays such
  as Steam's may not show.
- In DX10 games the game's depth buffer is not passed to NR yet (not implemented).
- Games whose FSR runs in their own shaders never hand it to OptiScaler: pick the game's DLSS
  option instead, which OptiScaler offers.
- When video memory runs short, the network pauses by itself and the picture returns to normal until memory frees
  up (Memory Guard, see "Networks and other settings").
- 007 First Light (optiscaler route, NR before or after the upscaler): the picture turns green and
  grainy, since the game hands the upscaler a picture that is not yet tone mapped and no exposure.
  Turn on Preprocess with Exposure = auto.
- 64-bit package only.

Third-party components
----------------------
OptiScaler (GPL-3.0; the official nightly v10.0.0-pre1 of 2026-10-30, unchanged; licences in
OptiScaler\Licenses\ of the game folder). DXVK (zlib licence); the version is in dxvk\version.txt,
source https://github.com/doitsujin/dxvk . The licence files of ReShade 6.8.0, the Vulkan Loader (with
a description of the changes), vort_Shaders, DLSS5-Feeder, RenoDX, Dear ImGui and the Inter font (NR
Control) are in their folders.
