# Developer tools

Players don't need anything here: `build.bat` in the repository root builds the game.

These are the scripts used while porting and testing. They are bash scripts and
need **Git Bash** (Git for Windows); run them from the repository root, for
example `bash dev/play/arcade.sh`. They expect the game laid out in the checkout as

```
_extract/installed/TFX/TFX/   the installed game (TFX.DAT, CONFIG.EXE, ...)
_extract/installed/TFX/cd/    TFX.cue + TFX.bin
_extract/cdfiles/             the CD's files, extracted (tools/cd_extract.py)
```

and a build in `build/` (`python tools/build.py`).

| Script | What it does |
|---|---|
| `run_tfx.sh`, `run_config.sh` | run the built game / setup program with a timeout and show the end of `runtime.log` |
| `drive.sh` | live-drive a game started with `TFX_CMDFILE=work/cmd.txt`: keys, mouse, screenshots, the runtime's debug commands (see DEBUG_LOG.md, "Live commands") |
| `to_flight.sh` | from the game's start to the Arcade cockpit with the saved pilot in slot 1 |
| `play/` | game-driving tests: `arcade.sh` starts an Arcade flight; `flighttest.sh`, `views.sh`, `roll_gaps.sh`, `fps_case.sh`, `sky_case.sh` and others test one thing each (see the head of each script) |
| `tests/` | native differential tests of the CPU emulation's flags and x87 against the host CPU |
| `callers.sh`, `calltree.sh` | who calls a generated function / what it calls, with call counts |
| `ghidra/` | headless Ghidra import and decompilation of TFX.DAT (Ghidra 12, JDK 21 and ghidra-lx-loader under `toolchain/`) |
