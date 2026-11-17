# TFX native Windows port — findings and plan

## What we have

| Source | Contents |
|---|---|
| CD-ROM release, installed | Installed CD version: `TFX\TFX\*` + `cd\TFX.bin/.cue` (data track + 14 CD-audio music tracks) |
| Floppy release | Floppy release (custom-format installer disks `diska..h.dat`), floppy patches 1–3, EN/FR manuals (.doc) |
| Patch set | Floppy patches `tfxpat_1..3` and CD patches `tfxcd_1..3` (1 = switch to DOS/4GW, 2 = v2.00, 3 = v2.01) |

Working copies are in `_extract/` (installed game, ISO of CD track 1 at `_extract/cdiso/track01.iso`).

### The CD data track
`DID.DAT` (11.5 MB, the asset archive), `TFX.DAT`, `INSTALL.EXE`, `TUNES/`, plus a bundled **Inferno** demo (`INFERNO/`, `INFERNO.HD/`).

### The installed game (`_extract/installed/TFX/TFX`)
| File | What it is |
|---|---|
| `START.EXE` | 16-bit real-mode launcher. Holds the OSL music drivers (AdLib, SCC-1/Sound Canvas, LAPC-1/MT-32, LMEDIT track driver). `TFX.BAT` runs `start DID=D: tfx`. |
| `TFX.DAT` | **The game.** Watcom C/C++32 (1993) code, LE format, bound to DOS/4GW 1.97 (so the CD patches are applied; probably v2.01, to confirm). |
| `CONFIG.EXE` | Setup program. Same toolchain (Watcom + DOS/4GW 1.95), much smaller. |
| `GUS.COM`, `Q2.COM` | Gravis Ultrasound helper and a small utility. |
| `TFX.CFG` | Text config (`DETAIL=`, `SOUNDFX=`, `BLASTER=A220 I7 D1 H5 T…`, `JOYSTICK=`…). |

`TFX.DAT` LE layout (from `tools/le_info.py`):
- obj1: 32-bit code, 0x7B4D2 bytes (~500 KB of x86 machine code), link base 0x10000
- obj2: 16-bit code, 0x264 bytes (real-mode / interrupt stubs)
- obj3: 32-bit data, 0x28940 bytes
- ~240 KB of relocation fixups, so the image can be moved anywhere. No imported modules: everything goes through DOS/DPMI interrupts and direct hardware I/O (VGA, Sound Blaster, PIT timer, keyboard, joystick, CD-ROM via MSCDEX).

**There is no source code.** "Porting" means converting the original machine code into something that runs natively on Windows. It does not mean rewriting the game.

## Approaches

1. **Static recompilation (recommended).** Disassemble `TFX.DAT`'s LE image and mechanically translate every x86 instruction into C, then compile that as a native Windows program. Every DOS, DPMI, VGA, Sound Blaster, timer and keyboard access gets replaced by calls into a small runtime built on SDL2. This has been done before for other Watcom/DOS4GW games (e.g. M-HT's Albion and X-COM ports). The game logic, assets and branding stay exactly as they were, with no CPU emulation. Because the output is ordinary C, the renderer can be patched later for widescreen and higher resolutions.
2. **Native loader with trapped instructions.** Load the LE image into a 32-bit Windows process and run the original bytes directly. A vectored exception handler catches `int`/`in`/`out`. This gets to a first frame sooner, but it hits hard walls: WoW64 has no LDT for DPMI selectors, the 16-bit object can't run, and timer and keyboard IRQs would have to be injected by hijacking a thread. Every enhancement would also mean binary patching.
3. **Full hand decompilation** (Ghidra to readable C). This is the cleanest long-term result but also the slowest. It works best as gradual cleanup on top of approach 1, one subsystem at a time, starting with the renderer for widescreen.

## Proposed milestones (approach 1)

1. **Tooling:** LE loader and fixup parser; a disassembler pass (capstone) that separates code from data and resolves jump tables and indirect calls; a full list of every `int` and port I/O site, so we know exactly what the runtime must provide.
2. **Pilot on `CONFIG.EXE`:** same compiler and extender, 80 KB of code. It shows the recompiler works end to end: a native window with its menus, and it writes `TFX.CFG`.
3. **Runtime (SDL2):** DPMI memory and selectors, DOS file I/O (mapped to the game folder, with `D:` mapped to the extracted CD files), VGA mode 13h / Mode X with palette, keyboard (int 9 scancodes), PIT timer IRQ, mouse, joystick, Sound Blaster digital audio for sound effects, CD-audio music played from the original `.bin` tracks.
4. **Recompile `TFX.DAT`** and reach the title screen, then menus, then flight.
5. **Music drivers:** the CD-audio option needs no synth. AdLib music needs the OSL driver logic plus an OPL3 synth library such as Nuked-OPL3 (a chip synthesizer, not a system emulator). General MIDI / MT-32 would go out through Windows MIDI.
6. **Enhancements:** windowed/fullscreen scaling first, then widescreen and higher resolution by reworking the decompiled renderer.

## Status

**Milestones 1–2 done: CONFIG.EXE runs natively on Windows 11.** It reads TFX.CFG, shows the original menu in Mode X, takes mouse input, saves TFX.CFG and quits, all from recompiled code without an emulator.

**TFX.DAT is playable** (tags `v0.3-3d-view`, `v0.4-modes`): all views, weapons and every game mode tested (TFX_CODE.md §9.8). Intro, menus, mission load, cockpit, HUD/MFDs, flight model and the polygon world (sky, shaded terrain, horizon, roll/pitch) all run natively. In-flight CD music plays from the BIN/CUE (MSCDEX Red Book emulation). Self-modification that the static scan can't see is covered by a runtime-observed list (`tools/smc/TFX.DAT.txt`; see "Self-modifying code" below).

### How it is built
```
tools/le.py         LE/DOS4GW loader: objects + fixups, relocation
tools/analyze.py    disassembly: tiered code discovery, jump tables, SMC detection
tools/recomp.py     x86 -> C translator (one C function per guest function)
runtime/            native runtime (SDL2)
  cpu.c/.h          registers, lazy flags, x87 (long double), call/return model
  interp.c          x86 interpreter fallback (run-time/threaded code, self-modified insns)
  dpmi.c            descriptors, interrupt vectors, DPMI int 31h
  dos.c             int 21h (32-bit DOS/4GW semantics), file I/O mapped to game/ and cd/
  bios.c            int 10h/16h/1Ah/33h/2Fh
  hw.c              PIC, PIT timer IRQs, keyboard controller, port dispatch
  vga.c             planar VGA (Mode X, write modes, latches), DAC, SDL presentation, input
build.bat           one click: fetches the build tools (tools/setup.ps1), then builds
tools/build.py      the build: python tools/build.py [tfx|config|all] [--game DIR] [--regen] [--check]
tools/smc_merge.py  merge runtime-observed code patches into tools/smc/<EXE>.txt
dev/                developer tools (Git Bash): run_tfx.sh / run_config.sh run a build with
                    logging, drive.sh live-drives a running game (TFX_CMDFILE), to_flight.sh
                    boots to the cockpit, play/ game-driving tests, tests/ native differential
                    tests (flags_test.c, x87_test.c), ghidra/ headless Ghidra helpers
```

### Self-modifying code
TFX patches many instruction immediates through computed addresses (the 3D clip rectangle, the transform matrix, renderer jumps). The runtime's live `c` command, and every crash, logs code bytes that differ from the loaded image. To add a session's observations and rebuild:
```bash
py -3.12 tools/smc_merge.py build/tfx/game/TFX.DAT build/tfx/runtime.log
```
```bash
python tools/build.py tfx --regen
```
Instructions that overlap an observed byte are generated to run with their live bytes. The list only covers what the recorded sessions exercised, so record more screens and game modes over time.

### Reverse-engineering with Ghidra
Ghidra 12.0.1 with JDK 21 and the `ghidra-lx-loader` extension (v12.0.1) are unpacked under `toolchain/` (downloads checked against the GitHub SHA-256 digests). `dev/ghidra/import.sh` imports TFX.DAT at the runtime's load addresses. Then `dev/ghidra/decompile.sh out.c 46ab50 44c904` writes pseudo-C, and `SCRIPT=RefsTo.java dev/ghidra/decompile.sh refs.txt LO-HI` lists references. Ghidra is used for reading the code only; the port itself stays on the recompiler.
Build:
```bash
python tools/build.py tfx --regen
```
```bash
python tools/build.py config --regen
```
`--regen` re-runs the translation (about 4 minutes for TFX.DAT). Without it, only the C code is recompiled. The build produces `build/<name>/<name>.exe` with debug info, and a stripped single-file copy in `dist/`. SDL is linked statically.

**Running (the launcher):** copy `dist/tfx.exe` (or `tfxconfig.exe`) into the game folder next to `TFX.DAT` and start it; see [README.md](README.md).
- **Loading the program:** the runtime reads the original `TFX.DAT` / `CONFIG.EXE` from the game folder and relocates it itself (`runtime/le_load.c`, checked against the FNV-1a hash recorded at build time).
- **Finding the folders:** it uses the exe's own folder when the program file is there, else `game\` next to the exe, else `--game`. It finds the CD image as the first `*.cue` in `..\cd`, `cd\`, the exe's `cd\`, the game folder, the exe folder or the parent folder (`--cdimage` overrides).
- **CD files:** they are read from the image's ISO 9660 file system unless an extracted copy is given with `--cd` (or exists as `cd\DID.DAT` next to the exe).
- **Defaults:** for TFX.DAT, `DID=D:` and the flow argument `tfx`.
- **Mouse:** it is captured while the window is active and released when it isn't. **Ctrl+F10** toggles capture, a click recaptures, and **Alt+Enter** toggles fullscreen. Scripted runs (`TFX_CMDFILE` / `TFX_SCRIPT`) never capture automatically and never show message boxes.
- **Idle CPU:** busy-wait loops sleep 1 ms at a time once the polled value stops changing: the BIOS tick (the frame limiter, via `rt_idle_hint()` that the recompiler puts at the entry of `0x435690`), int 33h polls, and 3DAh retrace polls while retrace is more than 2 ms away. Flight drops from a full core to about 12%, menus to about 22%, and frame rates are unchanged.

For development, `dev/run_tfx.sh` / `dev/run_config.sh` still run the builds in `build/` with explicit `--cd`/`--cdimage` options.

Debug switches (environment variables): `TFX_DEBUG=1` (heartbeat, function trace at exit), `TFX_TRACE_INT=1` (log every interrupt), `TFX_SHOT=<file.bmp>` (screenshot every second), `TFX_SCRIPT="ms action args;..."` (scripted input: `r dx dy buttons`, `k scancode`, `s file.bmp`). Build with `XCFLAGS=-DESP_DEBUG` to check stack balance.

### Next
1. ~~Run TFX.DAT~~, ~~menus and flight~~, ~~3D view~~, ~~views, weapons, every game mode, frame rate~~ (9.1 fps, the game's own cap). Next: complete a whole mission (landing, debrief, scoring) and finish training so Tour of Duty and Flash Points unlock; keep merging self-modification observations.
7. ~~Host CPU busy-waiting~~ (done, `v0.5-launcher`). Also done: the double-click launcher, CD files read from the image, single static exe.
8. ~~Higher frame rate~~ (done, `stage8-framerate`): `framerate=high` is the default, 54.6 fps (a frame every tick of the driver's 54.6 Hz clock). Game speed is measured unchanged (1.09 game seconds per real second either way), and every reader of the tick count is a linear timer. Frames are now presented as soon as the game flips a page (flip to screen 8 ms → 0.1 ms on average); with DLSS Frame Generation that is 109 frames a second. More than 54.6 fps would need a finer game clock, which every timer and menu shares.
9. Check against the original whether the grey gradient strips at the top/bottom of some screens (simulator world map, weather, arming) are part of the design (they match the chrome "RETURN TO MAIN MENU" bar) or a display bug.
10. From the first play-through (2026-11-10), for later:
    - ~~**Sky artefacts with DLSS**~~ (fixed 2026-11-11, DEBUG_LOG 115): the sky/haze bands had no motion vectors; they now move with the camera's rotation. Checked in rolls in clear weather; clouds with DLSS not checked.
    - ~~**F5 locks the external view**~~ (fixed 2026-11-11, DEBUG_LOG 111): a `framerate=high` regression, the chase camera's ease-in rounded to 0 with 1-tick frames; `runtime/gamefix.c`. `dev/play/views.sh` checks every view key from the chase view.
    - ~~**Fullscreen and any aspect ratio**~~ (done 2026-11-11, `stage8-wide`): checked from 5:4 to 32:9 and fullscreen at 5120×1440 with DLSS and frame generation; black corners in rolls fixed (they showed at 16:9 too). The game's object visibility test is widened for wide views (`stage8-wide-cull`), so terrain and objects reach the sides at 32:9. Open: 7680×2160 itself untested.
11. ~~Joysticks and game controllers~~ (done 2026-11-11, `stage9-joystick`): SDL devices presented as the game's CH Flightstick Pro on port 201h (`runtime/joy.c`, TFX_CODE.md 9.22): stick, throttle (also on a separate unit), hat, the four buttons, other buttons bound to keys in `tfx.ini`, the stick as the menus' pointer. Checked in flight with a virtual stick from the command file; with real hardware only that the devices are found (a HOTAS Warthog stick and throttle) and the game switches to joystick control. A controls window (`tfx.exe --controls`, `runtime/joyui.c`, `stage9-controls`) binds buttons, hat directions and axes and writes the `joy...` lines of `tfx.ini`. Open: a play test on the real stick (throttle direction, button numbers); setting an axis in the window with a real device; the afterburner on a throttle's detent.
12. ~~FreeTrack head tracking~~ (done 2026-11-11, `stage9-headtrack`): in the virtual cockpit (F10) the view follows the yaw and pitch of a FreeTrack source (`runtime/track.c`, TFX_CODE.md 9.23), all the way round, since the cockpit is a full 3D model. Checked with a stand-in source (`tools/fake_freetrack.ps1`) and by the user with a real tracker: yaw was right, pitch was inverted (fixed, DEBUG_LOG 120). Open: the user's re-test after that fix; the HUD symbols stay screen-fixed when looking away (the game draws them that way); head roll and position; how smooth small movements look (the game's camera angles count in 0.35° steps).
13. ~~Polygon code out of the interpreter~~ (done 2026-11-11, `stage9-compiled-fills`): the scan converters' computed jumps and the textured and stippled span fills are compiled (TFX_CODE.md 9.23). Looking down in the virtual cockpit: 54.6 fps instead of about 50, with 83% of the frame idle; three interpreter entry points left (the music driver and one data address). The cmdfile command `P` gives a sampling profile.
14. ~~Settings in the controls window~~ (done 2026-11-17): a Settings page next to Controls (`runtime/joyui.c`; `tfx.exe --settings` opens it there) sets the `tfx.ini` options (window, picture, frame rate, presenter, renderer, widescreen, cockpit filter, upscaler, render scale, frame generation, joysticks, head tracking, dead zone) with a click. Options that need the native renderer or an upscaler are greyed out, and the description warns when the AMD or NVIDIA DLLs or the art pack aren't next to the exe. Save rewrites only the changed lines in place (the joystick lines too only when a control changed), so the file's comments and hand-written values stay. Checked with the window's test script: the saved file's diff, Default settings, and the greyed options.
2. ~~Music~~ (done, `v0.7-music`): CD audio plays from the BIN/CUE. START.EXE's original OSL music drivers run on a real-mode interpreter (`runtime/rm86.c`, `music.c`) with an OPL2 synthesiser (`opl.c`) for AdLib and an MPU-401 UART to Windows MIDI (`mpu.c`) for SCC-1/LAPC-1. The driver's timer also restores the game's intended clock (27 fps, correct game speed).
3. ~~Sound Blaster digital effects~~ (done, `v0.6-soundblaster`): `runtime/sb.c` emulates an SB16 DSP and mixer, both 8237 DMA controllers and the IRQ, on its own SDL audio device. Port, IRQ and DMA follow the `BLASTER` line in the game's TFX.CFG, and the same value goes into the environment. `TFX_TRACE_SB=1` logs card, DMA and PIC traffic; `TFX_SB_WAV=<file>` records the output.
4. Catalogue TFX.DAT's self-modifying renderer code and check its performance.
5. ~~Enhancements: scaling, aspect, frame rate~~ (done, `v0.8-enhancements`; `tfx.ini`, `runtime/settings.c`). Widescreen is presentation-only so far (`aspect=fill`). A true widescreen picture needs its own project, see "Widescreen" below.
6. ~~Mouse capture on focus~~ (done, `v0.5-launcher`).

### Native renderer (in progress)
See [RENDERER.md](RENDERER.md): a Direct3D 12 renderer for the 3D world, built from geometry captured from the game each frame, with the cockpit/HUD kept as a layer. It produces scene colour, depth, motion vectors and jitter, so DLSS (including DLSS 5) or FSR can be switched on in `tfx.ini`. Stages 1–7 are done: scene capture, the Direct3D 12 presenter, the world drawn natively (`renderer=native`) with flat, Gouraud and textured polygons, discs, lines and the sky, per-pixel motion vectors, widescreen (`widescreen=on`), and the upscaler slot with the port's temporal upscaler (`upscaler=taa`, `render_scale`) AMD FSR (`upscaler=fsr`, signed FidelityFX DLLs; the port is built as x64 for them) and NVIDIA DLSS Super Resolution (`upscaler=dlss`, Streamline 2.14.16) and DLSS Frame Generation (`frame_generation=on`, with Reflex). The sky and clouds were checked against the original in every theatre and weather (stippled cloud puffs drawn see-through). Still open: DLSS 5 (not in that SDK).

Optional explore: DLSS 5 on RTX 40 through the community OptiScaler fork (Skynizz/optiscaler-dlss5), hooked at the port's `upscaler=fsr` path (`fsr.c` loads the AMD DLLs without a signature check; Streamline rejects swapped DLLs). Personal experiment only: NVIDIA's `nvngx_dlssnr.dll` isn't redistributable, and the user installs OptiScaler themselves. Official DLSS 5 through a newer Streamline is the clean route once NVIDIA enables RTX 40.

### Widescreen (analysis, not done)
The game draws into the VGA's 64 KB planes: three 16,000-byte pages (0000h, 3E80h, 7D00h) plus the cockpit template at BB80h, 64,000 of 65,536 bytes per plane. A 16:9 picture at 200 lines needs about 427 pixels (107 bytes per line, 21,400 bytes per page). Four of those don't fit in the 64 KB window the game can address, and its blitters hard-code the 80-byte pitch. So the game's own drawing can't simply be widened.

The realistic route is a **native wide renderer** for the 3D view:
1. Intercept the polygon routines (`0x44C904`, `0x44CBE8`), which receive projected vertices and a colour (TFX_CODE.md §9.7).
2. Widen the clip rectangle and the horizontal field of view in the projection.
3. Draw the polygons on the host at any resolution and aspect.
4. Composite the game's own cockpit and HUD picture on top.

The same interception would also give a higher-resolution 3D view.

## Build tools
- `build.bat` fetches these into `toolchain/` (not committed), each a fixed version with its SHA-256 checked (`tools/setup.ps1`): LLVM-MinGW 20261020, SDL2 2.30.9 (MinGW development package), an embeddable Python 3.12 with capstone 5.0.7.
- Optional, on request: AMD's FidelityFX SDK 2.4.0 and NVIDIA's Streamline SDK 2.14.16 (FSR, DLSS, Frame Generation). Without them the port builds with its own upscaler.
- Optional, for reverse engineering: Ghidra 12.0.1, Temurin JDK 21 and ghidra-lx-loader v12.0.1 under `toolchain/`.

## Note on rights
TFX is still copyrighted, however old it is. The repository holds no game data and no translated game code: the port reads the game from the user's own copy, and the executable is built from that copy on the user's PC. Built executables contain code translated from the game and aren't for redistribution.
