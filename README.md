# TFX-port

A native Windows port of **TFX** (Digital Image Design / Ocean, 1993). The game's own DOS program is translated to C ahead of time ("static recompilation") and runs on a small runtime that provides what DOS/4GW, VGA, the BIOS and the CD-ROM driver used to. There is no emulator: the game's code runs directly as a Windows program. Original assets and branding are untouched and are read from your own copy of the game.

This is an unofficial fan project. It is not affiliated with or endorsed by the game's developers, publishers or current rights holders. **The repository contains no part of the game**: no data, no art, no music, and none of its code. You build the executable yourself from your own copy.

## Features

- **A native 64-bit Windows program.** No DOSBox, no emulator and no DOS extender: the game's code runs as compiled C. The setup program (`tfxconfig.exe`) is ported the same way.
- **No CD drive needed.** The game reads its data and plays its CD soundtrack straight from the CD image. Saved pilots stay in the game folder as before.
- **Sound as in 1993.** Sound effects play through an emulated Sound Blaster 16. Music plays on AdLib FM, or on a Roland MT-32 / SCC-1 (or an emulator of one) through Windows MIDI.
- **A Direct3D 12 renderer** draws the 3D world on the GPU at full window resolution. The HUD is redrawn as smooth lines; clouds and missile smoke are see-through; night lights and distant aircraft are round dots. The original's cockpit art is kept.
- **Widescreen at any aspect ratio** (tested from 16:10 to 32:9): the world fills the screen, and the cockpit and HUD stay 4:3 in the middle.
- **Double the frame rate.** 54.6 fps instead of the original's 27, with the game's timing unchanged. With DLSS Frame Generation it reaches 109 fps.
- **Anti-aliasing and upscaling:** the port's own temporal upscaler, AMD FSR 3.1 / FSR 4, NVIDIA DLSS and DLAA, and DLSS Frame Generation (2x to 4x).
- **A sharper cockpit:** pixel-art filters (MMPX, xBR) for the cockpit, MFDs and panel text. An optional AI-upscaled cockpit art pack can be made from your own copy of the game.
- **Display options:** windowed or borderless fullscreen (Alt+Enter), any window size, 4:3 or stretched aspect, and sharp, whole-pixel or smooth scaling.
- **Joysticks, HOTAS and game controllers** work as soon as they're plugged in, and several devices can be used at once. A controls window binds any button or hat direction to any game key, and there is a ready-made layout for the Thrustmaster Warthog.
- **Head tracking** with FreeTrack / opentrack in the virtual cockpit: look up to 160° to each side and 80° up and down, around a full 3D cockpit and airframe.
- **A one-click build** (`build.bat`). It checks your copy of the game before it downloads anything, and fetches pinned, checksummed tools into the repository folder without installing anything on the PC.

## What you need

- **Your own copy of TFX**: the CD-ROM release, installed and patched to v2.01 (`TFX.DAT` is 1,032,503 bytes), with its CD image:

```
TFX\
  TFX\        <- the game folder (TFX.DAT, CONFIG.EXE, TFX.CFG, ...)
  cd\         <- TFX.cue + TFX.bin (the CD image)
```

- Windows 10 or 11 (64-bit), a DirectX 12 graphics card for the new renderer, about 1 GB of free disk space (2 GB with the optional upscaler SDKs) and an internet connection for the build tools.

Other releases of the game (the floppy version, unpatched CD versions, other languages) don't work: the port patches the game's code at fixed addresses. `build.bat` checks your `TFX.DAT` before it downloads anything, says which version it found and what to do (an unpatched CD copy needs the official CD patches `tfxcd_1` to `tfxcd_3`), and warns if the CD image is missing. `python tools\build.py --check --game <folder>` runs the same check.

## Building (one double-click)

1. Download or clone this repository.
2. Double-click **`build.bat`**.
3. Choose your game folder (the one that holds `TFX.DAT`) when asked.

The script then:
- fetches the build tools into the repository's `toolchain\` folder. Nothing is installed on the PC. Each download is a fixed version with its SHA-256 checked: the LLVM-MinGW compiler (182 MB), SDL 2.30.9 (8 MB), a private Python 3.12 (11 MB) and the capstone disassembler (2 MB);
- asks whether to also fetch AMD's FidelityFX SDK and NVIDIA's Streamline SDK (about 480 MB, under their own licences), which add FSR, DLSS and DLSS Frame Generation. Without them the game uses the port's own upscaler;
- translates your `TFX.DAT` and `CONFIG.EXE` to C and compiles them with the runtime: 5 to 10 minutes the first time;
- offers to copy the result (`tfx.exe`, `tfxconfig.exe`, `controls.bat` and the upscaler DLLs, if any) into your game folder. Nothing there is changed or removed.

Things to know:
- **Antivirus.** A script that downloads a compiler and produces a new, unsigned program is what security software distrusts. It may ask for confirmation, slow the build down or, on a managed PC, block it.
- **`build.bat` options:** `-Game "D:\Games\TFX"`, `-Upscalers` / `-NoUpscalers`, `-Install` / `-NoInstall`. See the head of `tools\setup.ps1`.
- **By hand:** with the tools in `toolchain\` and a Python that has `capstone`, run `python tools\build.py --game "<game folder>"`; `--help` lists the rest.
- **Don't share the built `tfx.exe`.** It contains code translated from your copy of the game.

## Playing

Double-click `tfx.exe` in the game folder (next to `TFX.DAT`).

Sound effects play through an emulated Sound Blaster 16 configured like your TFX.CFG. The music plays from the CD image, and the intro theme also plays on your configured music device, as in the original. START.EXE's own music drivers run inside the port: AdLib FM for "SB/CD", Roland SCC-1 or LAPC-1 through Windows MIDI. Set `TFX_MIDI_DEVICE` to part of a MIDI device's name to choose the output, for example an MT-32 emulator for LAPC-1. It finds the CD image in `..\cd` by itself (or in a `cd` folder next to the game, or next to the exe). The CD's data files are read straight from the image, and the soundtrack plays from its audio tracks. Saved pilots (`tfxsav.*`) go into the game folder as before.

`tfxconfig.exe` is the configuration program (detail level, flight model, sound, language); copy it next to `CONFIG.EXE` the same way.

**Controls:** the mouse is captured while the window is active and released when it isn't. **Ctrl+F10** frees or recaptures it, and **Alt+Enter** toggles fullscreen. Game keys are as in the original manual: for example F1–F10 views, Space fire, Enter/Backspace weapon select, `[` `]` engines, `=` `-` throttle (hold), and `P` pause.

**Joysticks and controllers** work without setup: with one connected, the game flies by joystick, and by keyboard again when it is unplugged (`joystick=off` in `tfx.ini` turns this off). The game sees a CH Flightstick Pro, one of the sticks it supports, so its own response curve and its "joystick scale" option apply; TFX.CFG and the DOS setup program aren't needed for it.
- **Joystick / HOTAS:** stick = pitch and roll, hat = look around (bindable like a button), buttons 1–4 = the game's four joystick buttons: fire, air-to-ground weapons, air-to-air weapons, next target. The throttle is the stick's lever or a separate throttle unit (idle to full; the afterburner stays on the keypad keys). Several devices are used together; the log (`runtime.log`) lists them with their numbers.
- **Game controller:** left stick, d-pad = look around, A/B/X/Y = the four buttons, triggers = throttle down/up, LB = air brake, RB = cannons, Back = cockpit view, Start = pause, stick clicks = gear and wheel brakes.
- **In the menus** the stick moves the pointer and button 1 clicks.
- **The controls window** binds everything else: `tfx.exe --controls` (`controls.bat` next to it). It lists the game's controls from the manual; click one and press the button or hat direction you want for it on any device. Del removes a control's buttons, and pressing a button shows what it is bound to. The axes are set by moving them (Set), with Rev to reverse one. The right side shows every device live, with its number (D1, D2, ...). Save writes the `joy...` lines of `tfx.ini` and keeps the rest of the file.
- **By hand in `tfx.ini`:** a button or hat direction is bound to `button1`..`button4`, `look_front|look_right|look_back|look_left` (the game's hat), `none`, or a key from the manual, with a modifier if needed:
  - `joy_button5=B` and `joy_hat_up=F1` (main device), `joy3_button7=LShift+B` and `joy3_hat_left=M` (device 3), `joy_x=G` (controller: `joy_a`, `joy_b`, `joy_x`, `joy_y`, `joy_lb`, `joy_rb`, `joy_lt`, `joy_rt`, `joy_back`, `joy_start`, `joy_ls`, `joy_rs`, `joy_dpad_up|right|down|left`).
  - Key names: `Space`, `Return`, `Backspace`, `Insert`, `Delete`, `F1`, `C`, `Keypad +`, `RCtrl`, `Alt+L`, `RShift+F1`.
  - The log names each unbound control the first time it is pressed.
- **Axes:** `joy_axes=x,y,rudder,throttle`, each `[device.]axis`, 0 = none, negative = reversed; for example `joy_axes=1,2,0,-3.3` reverses a throttle on device 3, axis 3. The rudder has no axis by default (the game only steers with it on the ground). `joy_device=` picks the main device by number or name; `joy_deadzone=` is the dead zone in percent.
- A layout for the Thrustmaster HOTAS Warthog stick (trigger = fire by default): `joy_button2=Space`, `joy_button3=X`, `joy_button4=B`, `joy_button5=RCtrl`, TMS `joy_button7=C`, `joy_button8=Return`, `joy_button9=X`, `joy_button10=Backspace`, DMS `joy_button11=M`, `joy_button12=2`, `joy_button13=3`, `joy_button14=1`, CMS `joy_button15=Insert`, `joy_button16=E`, `joy_button17=Delete`, `joy_button18=S`.

**Head tracking:** in the virtual cockpit (F10) the view follows a head tracker that speaks FreeTrack, for example opentrack with its output set to "freetrack 2.0 Enhanced". Yaw and pitch are used, up to 160° to each side and 80° up and down: the cockpit and the aircraft around it are a full 3D model, so you can look back at the wings and tail. Nothing needs setting up in the game: start the tracker before or after it, press F10, and the view follows as soon as poses arrive. When the tracker stops, the view looks ahead again and Right Shift + the arrow keys work as before (within the original's 27° to the sides). `head_tracking=off` in `tfx.ini` turns it off. The HUD symbols stay in the middle of the screen whichever way you look, as in the original. Head roll and position aren't used.

**Display and frame rate** are set in `tfx.ini` next to the exe. It's written with comments the first time you start the game.
- `window=1280x960`, `fullscreen=0|1`. Fullscreen uses the desktop resolution (borderless), and the window can have any shape: with the native renderer and `widescreen=on` the 3D world fills it at any ratio wider than 4:3 (checked at 16:10, 16:9, 21:9 and 32:9, fullscreen at 5120×1440 with DLSS and Frame Generation); the cockpit and HUD stay 4:3 in the middle. Narrower than 4:3 gets bars above and below.
- `aspect=4:3` (as on a 1993 monitor, the default), `square` (16:10 pixels) or `fill` (stretch to a widescreen display).
- `scaling=sharp` (crisp, even pixels, the default), `pixel` (whole-number scaling) or `smooth`.
- `framerate=high` (default: 54.6 fps, a frame every tick of the game clock; the game's time step is per tick, so flight speed and its timers stay correct) or `normal` (the original's 27 fps). Frames are presented as soon as the game finishes them. With DLSS Frame Generation, `high` gives 109 frames a second (measured).
- `presenter=d3d12|sdl`: the picture is shown with Direct3D 12 (falls back to SDL's renderer on PCs without a DirectX 12 GPU).
- `renderer=native` draws the 3D world on the GPU at full window resolution, with the cockpit and HUD kept from the original. It covers flat, Gouraud-shaded and textured polygons, round sprites, model lines, the sky and the clouds (the game's stippled cloud puffs drawn see-through), lights at night as round dots at their exact positions, and the HUD (text, gun ring, pitch ladder, heading tape, speed/altitude brackets, gun-cross markers) redrawn as smooth lines at full resolution, in every view and on the arming screen; distant aircraft, debris and sparks are round dots too, and missile smoke is drawn see-through where the game stipples it. It needs `presenter=d3d12`.
- `widescreen=on|off` (native renderer): in a window wider than 4:3, the 3D world fills the whole width and height; the cockpit and HUD stay 4:3 in the middle. Menus, maps and briefings stay 4:3.
- `widescreen_sides=world|cockpit`: what the sides show below the horizon in the cockpit view, the world (default) or the cockpit panel continued sideways. `renderer=game` is the original picture; `wireframe` is a debug view.
- `ui_filter=off|mmpx|remacri|xbr` (native renderer, 3D views): upscales the game's own pixels (the cockpit, MFDs, panel digits, status bar) with a pixel-art filter, including the cockpit's edge against the 3D world. `mmpx` (MMPX, 4×) smooths slopes and keeps text and small marks exactly as drawn; `xbr` is smoother and rounder but rounds small letters ("B" into "8") and turns short dashes into dots. `remacri` is MMPX plus the cockpit's static art (the panel strip and the MFD, radar and map pages) drawn from an AI-upscaled art pack in `art\remacri` next to the exe; without a pack it works as `mmpx`. `off` (default) keeps them as set by `scaling`.
  - **Making the art pack** (it is made from your copy of the game, and stays out of the repository):
    1. In flight, with the game started by `dev/play/arcade.sh`, run `dev/drive.sh A <folder>` to dump the cockpit art.
    2. `tools/artpack.ps1 prepare -Dump <folder> -Work work\artwork`, then `tools/artpack.ps1 upscale -Work work\artwork -Exe <realesrgan-ncnn-vulkan.exe> -Models <models folder>` (Real-ESRGAN's portable build with the Remacri 4x model), then `tools/artpack.ps1 pack -Work work\artwork -Out <exe folder>\art\remacri`.
- `upscaler=off|taa|fsr|dlss` and `render_scale=50..100` (native renderer): at `render_scale=100` the upscaler smooths the jagged edges (anti-aliasing); below 100 it draws the world at that percentage of the window and upscales it.
  - `taa` is the port's own temporal upscaler.
  - `fsr` is AMD FSR: FSR 4 on Radeon RX 9000 cards, FSR 3.1 on other GPUs. It needs the two `amd_fidelityfx_*_dx12.dll` files next to `tfx.exe`; the build puts them there when the FidelityFX SDK was fetched (`build.bat -Upscalers`), with AMD's licence in `AMD_FidelityFX_LICENSE.md`.
  - `dlss` is NVIDIA DLSS Super Resolution (GeForce RTX): `render_scale=100` is DLAA, lower values pick Quality, Balanced or Performance. It needs the NVIDIA DLLs next to `tfx.exe`; the build puts them there when the Streamline SDK was fetched (`build.bat -Upscalers`), with NVIDIA's licences. Switching to or from `dlss` needs a restart; `render_scale` can change live.
- `frame_generation=off|on|3x|4x` (native renderer with any upscaler): NVIDIA DLSS Frame Generation on GeForce RTX 40/50 cards. In flight the game keeps its own frame rate (about 27 frames per second) and DLSS adds a generated frame between each pair (`on` = 2x; 3x and 4x need an RTX 50 card, otherwise 2x). It is off in menus and while the window isn't focused. It needs Windows' hardware-accelerated GPU scheduling, and the NVIDIA DLLs next to `tfx.exe`. Turning it on needs a restart.
  - `dlss5=on` isn't available: DLSS 5 isn't in the Streamline SDK this build uses ([RENDERER.md](RENDERER.md), stage 7).
- The port is a 64-bit Windows program (AMD's DLLs are 64-bit only).

**Options** (command line, all optional): `--game <folder>`, `--cdimage <file.cue>`, `--cd <folder with the extracted CD files>`, `--env NAME=VALUE`, then any arguments for the game itself (default `tfx`).

If something goes wrong, `runtime.log` next to the exe says what happened.

## The porting work

[PORTING_PLAN.md](PORTING_PLAN.md) has the approach, the status and the roadmap; [TFX_CODE.md](TFX_CODE.md) everything learned about the original program; [RENDERER.md](RENDERER.md) the GPU renderer; [DEBUG_LOG.md](DEBUG_LOG.md) how each problem was found and fixed. The developer tools are in [`dev/`](dev/README.md): bash scripts (they need Git Bash) that run a build with logging, drive a running game for tests, and help with debugging. Players don't need them.

## Licence and rights

- The port's own code (the runtime, the recompiler and the tools in this repository) is under the [MIT licence](LICENSE). Third-party components and their licences are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
- **TFX itself is not covered by that licence.** The game, its code, art, music and name belong to their respective owners, and the game is still under copyright however old it is. You need a legitimately obtained copy; this project doesn't provide one and its issue tracker is not the place to ask for one.
- The executable you build contains code translated from your copy of the game. It is for your own use with that copy and isn't for redistribution.
- "TFX" and the names of its developer and publisher are used only to say what this software works with.
