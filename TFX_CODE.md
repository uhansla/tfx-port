# TFX: findings about the original code

The debugging history (every attempt and its outcome) is in [DEBUG_LOG.md](DEBUG_LOG.md).

A running record of what we know about how TFX (Digital Image Design / Ocean, 1993) is built. It comes from static analysis of the shipped binaries. Everything here can be reproduced with the scripts in `tools/`.

Analysis reports from the latest run: `work/config_report.txt` (CONFIG.EXE) and `work/tfx_report.txt` (TFX.DAT).

---

## 1. Distribution and file layout

### 1.1 The releases examined
| Release | Contents |
|---|---|
| CD-ROM release, installed | The installed CD version (`TFX/TFX/`), the CD image `TFX/cd/TFX.bin` + `TFX.cue`, and a `run.bat` launcher menu (SB16 + CD music / MT-32 / Sound Canvas). |
| Floppy release | Floppy release: installer `tfx/install.exe` plus 8 disk images `diska.dat`–`diskh.dat`; floppy patches `tfxpat_1..3`; English and French manuals (`manuel anglais.doc`, `manuel francais.doc`); `tfxkeys.txt`. |
| Patch set | Floppy patches `tfxpat_1..3` and CD patches `tfxcd_1..3`, each with its readme. |

Extracted copies are in `_extract/` (`main/`, `patches/`, `installed/`, `cdiso/`).

### 1.2 Patch history (from the patch readmes and strings)
| Patch | Effect |
|---|---|
| `tfxpat_1` / `tfxcd_1` | "patch to change TFX to use Rational Systems 1.95 dos extender". Fixes the "unexpected interrupt" error and other crashes. Rewrites `CONFIG.EXE`, `START.EXE`, `TFX.DAT`. |
| `tfxpat_2` / `tfxcd_2` | Takes TFX to **version 2.0**. Requires patch 1. |
| `tfxcd_3` | "CD patch to take TFX from version 2.00 to version 2.01. Please re-run CONFIG.EXE." Rewrites `config.exe`, `start.exe`, `tfx.dat` (via a temporary `tfx.$$$`). |

The patch programs are self-applying patchers (Borland/Watcom-built DOS executables with an embedded diff). They write a temporary `tfx.$$$` / `tfxout.$$$`; the installed copy still contains a 0-byte `TFXOUT.$$$`, which is evidence the patches were applied.

The installed `TFX.DAT` is bound to DOS/4GW **1.97** (May 19 1994) and `CONFIG.EXE` to **1.95** (Nov 23 1993). The installed `TFX.DAT` (1,032,503 bytes) differs from the one on the CD (1,013,255 bytes). Conclusion: **the installed copy is patched, very likely to v2.01.** Not yet confirmed byte for byte by applying the patches to the CD files.

### 1.3 The floppy installer
- `install.exe` is 16-bit real mode, built with **Borland C++ (1991)**.
- The disk images are a **custom DID archive format**. `diska.dat` starts with a plain-text readme ("TFX notes ... Thank you for purchasing TFX ..."), followed by a binary section marked `RA` (offset 0x9E9 in `diska.dat`) and compressed data. The format hasn't been decoded, and doesn't need to be, because the installed CD copy has everything.
- Installer strings list the files it writes: `readme.txt`, `tunes\intro.rld|adl|scc`, `waytxt\col`, `waybin\col`, `tfx.dat`, `q2.com`, `start.exe`, `config.exe`, `tfx.adv`, `did.dat`. It creates `TFX.bat` and then runs `config.exe`.
- Default install path: `?:\TFX`, with the data file `?:\TFX\did.dat`.

### 1.4 CD image
`TFX.cue`: track 01 is MODE1/2352 data (00:00:00–04:14:12). **Tracks 02–15 are Red Book audio**: the CD music, 14 tracks, about 56 minutes.

ISO9660 contents of track 01 (extracted to `_extract/cdiso/track01.iso`):
| Path | Size | Notes |
|---|---|---|
| `/DID.DAT` | 11,509,643 | Main asset archive (graphics, 3D models, terrain, sound). Read from the drive given by `DID=` (see `TFX.BAT`). |
| `/DISK1.DAT` | 1,422,373 | Installer archive (CD install) |
| `/INSTALL.EXE` | 20,614 | CD installer |
| `/TFX.DAT` | 1,013,255 | Unpatched game executable |
| `/TFX.BAT` | 11 | |
| `/TUNES/INTRO.ADL, .RLD, .SCC` | ~8–11 KB each | Intro music in three driver formats: AdLib, Roland (MT-32/LAPC-1), Sound Canvas (SCC-1) |
| `/INFERNO/*`, `/INFERNO.HD/*`, `INFERNO.BAT`, `INFERHD.BAT` | | Bundled demo of DID's *Inferno* (its own `TFX.DAT`, `START.EXE`, `DID.DAT`) |

### 1.5 Installed game directory (`_extract/installed/TFX/TFX/`)
| File | Size | Role |
|---|---|---|
| `TFX.BAT` | 18 | `start DID=D: tfx`: runs START.EXE, telling it where DID.DAT lives (the CD drive). |
| `START.EXE` | 36,288 | 16-bit real-mode launcher **and music driver host**. Contains the OSL (Ocean Software Ltd) drivers: "LMEDIT Track Driver Version 1.00", "ADLIB Device Driver Version 1.00", "SCC1 Device Driver Version 1.00", "LAPC1\MT32 Device Driver Version 1.00", all "(C) 1993 OSL." It stays resident while the game runs (see §4.6). |
| `TFX.DAT` | 1,032,503 | **The game.** A DOS/4GW-bound Watcom LE executable (§2). |
| `CONFIG.EXE` | 282,334 | Setup program (sound, controls, detail). Same toolchain as the game. |
| `GUS.COM` | 23,376 | Gravis UltraSound support (MZ exe despite the `.COM` name; same stub layout as START.EXE). |
| `Q2.COM` | 14,750 | Small real-mode utility (true .COM). Purpose not yet analysed. |
| `TFX.CFG` | 222 | Text config written by CONFIG.EXE (§1.6). Variants in `SB16/`, `MT32/`, `SC55/`, used by the launcher menu. |
| `TFX.ADV` | 258 | Thrustmaster FCS/WCS button-mapping file. |
| `TFX.MK1`, `TFXMK1.TXT` | | Thrustmaster WCS Mark I configuration and its readme. |
| `DID.SWP` | 0 | Swap file named by `SWAP=` in TFX.CFG (DOS/4GW virtual memory). |
| `MISSIONS/MISS830.MIS` | 696 | A saved or user mission. |
| `TUNES/` | | Intro music (copy of the CD's). |
| `WAYBIN/COL`, `WAYTXT/COL` | 2 each | Waypoint data folders (binary/text). |

### 1.6 TFX.CFG keys (SB16 version)
```
SWAP=did.swp        DOS/4GW swap file
CONTROL=0           input device
DETAIL=2            graphics detail level
MODEL=3             flight model
SOUNDFX=3           effects device
MUSIC=1             music device (CD audio in the SB16 profile)
STICK=0
LANG=1
LOADLOW=0
LOWMEM=0
JOYSTICK=4000,0,0,0,0,0,0,0,0,0,0    joystick calibration
BLASTER=A220 I7 D1 H5 T6 F3          Sound Blaster port/IRQ/DMA
REALISM=1,0,1,1,0,1
JOYSCALE=
ULTRASND=
IRQBASE=135
```

---

## 2. Executable format: TFX.DAT and CONFIG.EXE

Both files are **Rational Systems DOS/4GW "bound" executables**:

1. An MZ stub, which is the DOS/4GW extender loader itself (contains "DOS/4G Copyright (C) Rational Systems, Inc. 1987 - 1993").
2. A `BW` block (DOS/4GW Professional bind format) right after the first MZ image (`TFX.DAT` at 0xF474).
3. The extender proper ("Rational Protected Mode Run-time", with `D32DebugHook`, `D32ChainInterrupt`, etc.).
4. A second MZ stub whose `e_lfanew` points to the **LE (Linear Executable)** header of the application.

| | TFX.DAT | CONFIG.EXE |
|---|---|---|
| LE stub MZ offset | 0x352A4 | 0x25214 |
| LE header offset | 0x35304 | 0x270CC |
| Pages (4 KB) | 0x8C (140) | 0x19 (25) |
| Entry | obj1 + 0x43AA4 | obj1 + 0x10 |
| Initial ESP | obj3 + 0x28940 | obj3 + 0x5D40 |
| Imported modules | 0 | 0 |
| Fixups | 28,960 | 2,968 |

### 2.1 Objects
**TFX.DAT**
| Obj | Link base | Size | Type |
|---|---|---|---|
| 1 | 0x00010000 | 0x7B4D2 (≈494 KB) | 32-bit code (`flags 0x2045`) |
| 2 | 0x00090000 | 0x264 | **16-bit code** (`flags 0x45`) |
| 3 | 0x000A0000 | 0x28940 (≈162 KB) | 32-bit data, incl. stack (`flags 0x2043`) |

**CONFIG.EXE**
| Obj | Link base | Size | Type |
|---|---|---|---|
| 1 | 0x00010000 | 0x140DC (≈80 KB) | 32-bit code |
| 2 | 0x00030000 | 0x8E | 16-bit code |
| 3 | 0x00040000 | 0x5D40 | 32-bit data |

TFX.DAT's data object is linked at 0xA0000, the VGA window. That is only possible because DOS/4GW relocates everything at load time. Our loader does the same and places objects contiguously from 0x00400000 (`recomp.py: default_bases`).

### 2.2 Fixups
- **Every fixup is a 32-bit absolute offset** (LE source type 7). There are no self-relative, selector or 16:16 fixups. Calls and jumps between code locations are plain `E8/E9` relatives that need no relocation.
- TFX.DAT fixup sources → targets (obj→obj): 1→3: 20,865 · 1→1: 6,831 · 3→3: 1,000 · 3→1: 261 · 1→2: 3.
- The 1→2 references (3 in TFX, 1 in CONFIG) take the address of the 16-bit object, which is almost certainly copied into DOS memory as a real-mode stub or used as a 16-bit interrupt entry.
- 3→1 references (261) are code pointers stored in data: callback tables, handler tables, function-pointer structures.

---

## 3. Toolchain and code characteristics

- **Compiler:** WATCOM C/C++32 (1993). The code segment starts with `CC EB FD 90…` (an `int3`/`jmp $` trap), then at +0x10 a `jmp` over the banner "WATCOM C/C++32 Run-Time system. (c) Copyright by WATCOM International Corp. 1988-1993."
- **Calling convention:** Watcom register-based (`eax, edx, ebx, ecx` for the first arguments). Function prologues are typically `push ebx/ecx/edx/esi/edi; push ebp; mov ebp, esp` or `enter n, 0`.
- **Padding** between functions uses `8D 40 00` (`lea eax,[eax+0]`), `8B C0` (`mov eax,eax`) and `90`.
- **Hand-written assembly** is clearly mixed in: lodsw/stosw loops, `xlatb`, `aam`, heavy `out dx` sequences (renderer and VGA), x87 code.
- **Instruction mix (TFX.DAT):** ~117k instructions reached. Top entries: mov 37.9k, push 12k, call 9.7k, pop 9.3k, cmp 6.7k, add 5.8k, movsx 4.4k, je 3.7k, jmp 3.4k, sub 3.5k, movzx 2.8k, lea 3.0k, fmul 1.5k, fld 1.4k, sar 1.3k, imul 1.2k, fstp 0.9k. There are about 6.5k x87 instructions in total. The flight model and 3D maths are floating point (`fsin`, `fcos`, `fpatan`, `fsqrt`, `fptan` all appear).
- **No 486+/Pentium instructions** (no `bswap`, `cmpxchg`, `cpuid`, `rdtsc`). It targets a 386 with an FPU.

### 3.1 Code patterns that matter for analysis and recompilation
1. **Switch jump tables** are emitted *inside the code object* immediately after the dispatch:
   ```
   cmp  reg, N
   ja   default
   jmp  dword ptr [reg*4 + table]     ; table follows in code
   ```
   Table entries are LE fixups pointing into code. A `ds:` prefixed variant (`3E FF 24 85 …`) also occurs; capstone shows it as `notrack jmp`. TFX has 64 tables found so far, CONFIG 17.
2. **Call-over-data:** some runtime routines are entered with `call routine` followed by an inline data table. The routine pops the return address to use as a data pointer, then `ret`s to its caller's caller. Example in CONFIG.EXE: `0x23D6B call 0x23E20`, followed by a table of powers of ten at 0x23D70, used by the number formatter (which itself uses `aam 10`). So the analyzer must **not** assume execution resumes after every `call`.
3. **Data embedded in code:** VGA register tables, bit-mask tables (e.g. CONFIG 0x1EA44: `01 03 07 0F …`), digit strings `"0123456789abcdefghijklmnopqrstuvwxyz"` (CONFIG 0x1F1B0), and pointer tables. About 13–20% of each code object is data or unreferenced code.
4. **Unreferenced library functions:** the linker pulled in whole Watcom library modules, so many functions are never called directly. They're found by scanning gaps for prologues.

---

## 4. Interaction with DOS, DOS/4GW and hardware

Neither binary imports anything. All system access goes through software interrupts and direct port I/O. This is the complete list of what a native runtime has to provide (from `work/*_report.txt`; the AH/AX values are the nearest preceding load).

### 4.1 Watcom startup (`cstart`, CONFIG.EXE entry 0x1008A)
1. `sti`, aligns ESP, saves the stack top.
2. `int 21h AH=30h` with `EBX='PHAR'`: DOS version, plus Phar Lap detection (Phar Lap answers "DX" in the high word of EAX). Then `int 21h AX=FF00h, DX=0078h`: **DOS/4GW detection**. If AL≠0 it takes the DOS/4G path: saves `GS`, takes `ES` as the **PSP selector** and reads the **environment selector** from `ES:[2Ch]`.
3. Writes the flat DS selector into a code-segment variable (`mov es:[0x10084], ds` via a flat ES alias) so interrupt handlers can reload DS with `mov ds, cs:[0x10084]`.
4. Copies the command tail from `PSP:81h` and the environment strings (via `mov ds, env_selector`, so **DS is temporarily non-flat**) into its own memory. It also checks for an `NO87=` environment variable (FPU emulation override).
5. Zeroes BSS, sets up the heap pointer, then calls the C runtime init and `main`.

So the runtime must give the program a **PSP selector in ES** and an **environment selector at PSP:2Ch**, both with proper base addresses.

### 4.2 DOS (`int 21h`, DOS/4GW 32-bit extended semantics: DS:EDX pointers are flat)
TFX.DAT uses: 1Ah (set DTA), 25h/35h (set/get vector) and **25xxh DOS/4GW extensions** 2502h, 2503h, 2504h, 2505h, 2507h, 250Ch; 2Ah (date), 2Ch (time), 2Fh (get DTA), 30h, 3Ch/3Dh/3Eh/3Fh/40h/41h/42h (create/open/close/read/write/delete/seek), 44h (IOCTL), 49h/4Ah (free/resize memory), 4Ch (exit), 4Eh/4Fh (find first/next), 09h (print string), FF00h (DOS/4GW check).

CONFIG.EXE additionally uses 0Bh, 19h (current drive), 37h, 41h, 43h (attributes), 47h (cwd), 48h (alloc), 4Bh (EXEC, to launch programs), 4Dh, 01h.

### 4.3 DPMI (`int 31h`)
TFX.DAT: 0000h? (CONFIG), 0005h, 0200h/0201h (get/set **real-mode** interrupt vector), 0202h/0203h (get/set exception handler), 0204h/0205h (get/set **protected-mode** interrupt vector), **0303h (allocate real-mode callback)**, 0304h (free callback), 0400h (DPMI version), 0600h (lock memory), 0A00h (vendor API).

The real-mode callback (0303h) is how real-mode code calls into the protected-mode game. The most likely caller is the music driver resident in START.EXE (§4.6).

### 4.4 Other software interrupts
- `int 10h`: video BIOS, mode set and `AH=0Fh` get mode.
- `int 33h`: mouse, functions 03h (position/buttons), 05h (button press), 0Bh (motion counters).
- `int 2Fh`: 1600h (Windows enhanced-mode check), 1686h (DPMI mode check, TFX), 00FAh family (×4–6: a vendor/TSR multiplex ID, probably the OSL music driver or Rational's own; to investigate).
- `int 8`, `int 0`: invoked directly in a few places (timer chain, divide trap).

### 4.5 Port I/O
TFX.DAT has 533 `out dx, al`, 48 `out dx, ax`, 29 `in al, dx` and a handful of immediate-port `in/out`. CONFIG.EXE has 97 `out dx, al`, 17 `out dx, ax` and 12 `in al, dx`. Most are VGA register programming:
- `3C4h/3C5h` sequencer (`mov dx,3C4h; mov ax,0F02h; out dx,ax` = **map mask = all planes**, the classic unchained "Mode X" pattern seen at CONFIG 0x1AC9A, 0x1AD22, 0x1ADAA). Together with `3CEh` graphics controller writes, this means the game draws in **planar VGA (Mode X-style) memory**, not plain chained mode 13h.
- `3D4h` CRTC (start address for page flipping, CONFIG 0x1E8DD), `3DAh` retrace polling, `3C8h/3C9h` DAC palette.
- PIC (`20h`, `A0h` EOI), PIT reprogramming (timer rate), keyboard `60h`, joystick `201h`, and Sound Blaster DSP at the `BLASTER` base. Still to be catalogued per port.

Some immediate-port instructions in the TFX report (`out 2, eax`, `in eax, 0x4a`, …) come from data mis-decoded as code. The analyzer still accepts a few false positives in TFX.DAT.

### 4.6 Process structure at run time
```
TFX.BAT -> START.EXE DID=D: tfx     (real mode, loads OSL music driver chosen by TFX.CFG MUSIC=)
              \-> spawns TFX.DAT     (DOS/4GW, protected mode)
                     <-> music driver via real-mode interrupt / real-mode callback (DPMI 0300h/0303h)
```
START.EXE holds four OSL drivers: AdLib (OPL2/3 FM), SCC-1 (Sound Canvas, GM), LAPC-1/MT-32 (Roland), and the LMEDIT track driver (sequencer). Music data comes in matching formats (`.ADL`, `.SCC`, `.RLD`). The CD version can also play the Red Book tracks (`MUSIC=1` in the SB16 profile = CD audio via MSCDEX).

---

## 5. Static-analysis results

| | CONFIG.EXE | TFX.DAT |
|---|---|---|
| Instructions decoded | 23,812 | 117,127 |
| Code bytes covered | 86.7 % | 78.8 % |
| Function entries | 695 | 3,055 |
| Basic-block leaders | 6,167 | 25,954 |
| Jump tables | 17 | 64 |
| Unresolved indirect jmp/call sites | 163 | 617 |

Method (`tools/analyze.py`):
1. Recursive descent from the LE entry point.
2. Fixup targets in code are code-pointer candidates **only if** the fixup is an immediate operand (`push offset`, `mov reg, offset`) or lies outside decoded code (data object, code-embedded tables). Memory displacements are data references.
3. Each candidate, and each "fall-through after call" address, is traced as a **trial** and kept only if it decodes without hitting data, privileged/implausible instructions, zero runs (`00 00`), or overlaps with already accepted code.
4. Jump tables are bounded by the guarding `cmp reg, N`.
5. Remaining gaps are scanned for Watcom prologue bytes and traced the same way.

---

### 5.1 Analysis method changes since the first pass
- **Analysis runs at the link-time addresses** (0x10000…), then results are mapped to the load addresses. In relocated form, pointer tables such as `3F 42 40 00` decode as plausible code. At the link base (`3F 42 01 00`) their zero bytes expose them as data. Analyzing at load addresses had let junk "functions" overlap and hide real code.
- **Evidence tiers.** Each tier is completed before moving to the next:
  1. Fall-through after calls, and code pointers stored in the data object. These are traced with *pruning*: a bad instruction stops only that path instead of rejecting the whole tree.
  2. Prologue scanning of unreached regions.
  3. Weak candidates (immediate operands, tables inside code).
- After discovery, every address-taken code location that is already an instruction start becomes a function entry too (§7.3).
- Current CONFIG.EXE result: 24,772 instructions, 89.1% of the code object, 1,067 C functions. Only 3 "non-returning" call sites remain, all genuine: `exit()`, the call-over-data idiom at 0x23D6B, and one runtime helper.

---

## 7. Code idioms that need special handling (found by running CONFIG.EXE)

These are now handled generically by the recompiler and runtime. TFX.DAT shares the same Watcom runtime and DID library code, so they apply there too.

### 7.1 Watcom `int386x()`: `push stub; ret` into an `int N; ret` table
`0x1982B` computes `0x198CC + 3*N`, pushes it, loads the registers and executes `ret`. Control "returns" into a stub (`CD nn C3`), whose own `ret` goes back to the original caller. There are 256 stubs at 0x198CC. The analyzer recognizes runs of `int N; ret` triples and makes each one a function. At run time a `ret` whose popped address doesn't match the expected one is treated as a computed jump.

### 7.2 Watcom cross-jumping (shared epilogues)
Functions push registers and then `jmp` into the tail of another function. In C that's a tail call, and the callee must compare its final `ret` against **the original caller's** return address, not against whatever is on top of the stack at entry. Generated functions therefore take the expected return address as a parameter (`void f_xxx(uint32_t ret_)`), and tail calls pass on their own.

### 7.3 Threaded-code text renderer (`0x148F7`, also in TFX.DAT at 0x70Dxx)
The proportional-font string drawer builds a **list of code addresses**: one fragment per glyph, chosen from tables at 0x14828/0x14838 (targets 0x14AD9, 0x14ADC, 0x14AF1, 0x14B10…). It copies the list onto the stack (`sub esp, n; mov edi, esp; rep movsd`), pushes a final continuation (0x14985) and executes `ret`. Each glyph fragment ends in `ret` and so jumps to the next. Many targets are mid-function addresses. They are now function entries, and a mismatching `ret` that isn't a genuine call continuation dispatches to them. Fragments draw into Mode X by `out 3C5h` (map mask) plus `mov [edi], cl`.

### 7.4 Self-modifying code (SMC)
CONFIG's sprite/blit routines (0x1A568…0x1A594, 0x1AF4D…, 0x1B61D…) contain placeholder operands that are patched before each use:
- `add esi, 0x12345678` (immediates patched by `add [0x1A579], ecx` and similar),
- `mov byte ptr [edi], 0x12`, `mov bl, 0x12`, `mov bh, 0x12`, `shl ah, 0` (patched colours, shift counts),
- `jmp 0x1A252` at 0x1A57D, whose **rel32 target** is rewritten (`mov [0x1A57E], eax` computed from a code address, `add [0x1A57E], 10`).

The analyzer finds every store whose fixed address lands inside a decoded instruction (CONFIG: 19 patched instructions; TFX.DAT: 11 `cs:` stores plus non-`cs:` ones, still to be catalogued). The recompiler emits those instructions as `rt_step(addr)`, which interprets the live bytes. A patched `jmp` becomes a dispatch on the current displacement.

### 7.5 Code copied into conventional memory
CONFIG copies its keyboard IRQ handler from the code object into a DOS memory block (DPMI 0100h). It installs it both as the real-mode vector (DPMI 0201h, `1000:0102`) and as the protected-mode vector 9 (DPMI 0205h, `0008:00010191`). The copy contains a real-mode part and a 32-bit part, marked with a `0x5678` signature. The runtime first tries to match copied bytes against known functions. If nothing matches uniquely, the interpreter runs the copy, and that is what happens for this handler.

### 7.6 Watcom `memcpy` uses `F2 A5` (`repne movsd`)
Capstone decodes `F2 A5` as SSE `movsd` and drops the prefix. On a 386, F2 on `movs` behaves exactly like `rep`. REP prefixes are now read from the raw instruction bytes.

### 7.7 Calls whose return address is consumed (call-over-data)
`0x23D6B: call 0x23E20` where `0x23E20: pop edi; ret`. This puts the address of the powers-of-ten table at 0x23D70 into EDI and returns straight to the caller of 0x23D6B. The runtime unwinds the C frames until the matching call continuation is reached; the generator emits a table of all call-continuation addresses for this.

---

## 8. CONFIG.EXE behaviour (as observed running natively)

- **Start-up:** Watcom `cstart` → `main` at **0x142A0**: init (0x143D2, 0x10290, 0x132F0) → `getenv("BLASTER")`/`ULTRASND` parsing (0x13224) → read **TFX.CFG** (open mode 0) → video init → keyboard init (0x185B8) → copy palette (768 bytes from 0x41CDC) → set palette (0x1E558, `out 3C8h/3C9h`) → **UI loop 0x141EC** → restore keyboard LEDs (0x18650) → shutdown (0x19688).
- **Video:** `int 10h AX=0012h` then `AX=0013h`, then unchained **Mode X** (sequencer reg 4 = 06h, CRTC offset 28h, i.e. 320×200 with 4 planes). Drawing uses write mode 1 (latch copies, 0x1ACDE copies 16,000 bytes per page) and plane masks.
- **Keyboard:** sets the LEDs by sending `EDh` to port 60h and polling port 64h for the `FAh` ACK (0x18538). Installs its own IRQ1 handler (§7.5) and restores vector 9 at exit.
- **Mouse:** polls `int 33h` functions 03h (buttons), 05h (press counts) and 0Bh (motion counters). The cursor position is **integrated from mickeys** into 0..319 × 0..199 (0x19333), not taken from the driver's absolute position. The cursor is a software sprite (red/white arrow).
- **Menu layout (320×200):** Swap file (OFF/ON + filename), Control method (Keyboard/Joystick), Detail level (Low/Medium/High), Flight model (Arcade/Simple/Realistic/Military Spec), Sound FX (Off/Sound Blaster/LAPC1/SB16/GUS/?), Music (Off/SB-CD/LAPC1/Adlib/SCC1), Joystick type (Normal/Flightstick Pro/Thrustmaster), Language (English/Français/Deutsch/Italia/España), plus buttons About, Quit, Save, Conf Joystick, Examine System.
- **Saving** rewrites TFX.CFG (open mode 1). Selecting Thrustmaster changes `STICK=0` to `STICK=2`.
- **Quit** restores int 9 and text mode 03h and exits with **code 1** after a save (the installer probably checks this).

---

## 9. TFX.DAT behaviour (as observed running natively)

### 9.1 How START.EXE and TFX.DAT cooperate
- `TFX.BAT` = `start DID=D: tfx`. START.EXE (16-bit Borland C++ 1991) reads TFX.CFG and sets **environment variables** for the game: `DID=<drive>:` (where DID.DAT lives), `BLASTER=…`, `ULTRASND=…`, `DOS4GVM=deleteswap minmem#820 virtualsize#5000 swapname#<SWAP>`. It may run `q2.com` and `gus.com`, loads the OSL music driver, prints "Loading..." and spawns `tfx.dat`. Afterwards it reads `tfxout.$$$` to decide what to do next ("TFX spawn error", "Hardware change please install again" are its messages).
- **`DID` must be in the environment before TFX.DAT starts.** TFX.DAT's argument handler (`0x442EE0`) also `putenv`s any `NAME=value` argument, but by then the archive layer has already cached a NULL prefix (§9.2) during the first `coltab.dat` lookups. The runtime therefore takes `--env DID=D:`.
- The remaining argument is the **flow (parser) name**: `tfx` → `getenv("DID") + "\tfx.par"` (`0x442E2C`, format `%s\%s.par`). Without it TFX prints "No parser file specified". `tfx.par` chains to **`cdintro.par`** (the CD intro).
- The music track list lives in START.EXE (`tunes\intro.rld|adl|SCC`, `inter`, `news`, `enrol`, `death`, theatre themes `columbia`, `libya`, `ocean`, `somalia`, `yugo`, mission-type folders `airsuper\`, `closeair\`, `defssupr\`, `intercep\`, `interdic\`, `maritime\`, `antiarm\`, plus `fxonly.rld`, `engine.adl`, `1unsuc`, `2succ`, `3-9tunes`).

### 9.2 DID.DAT packed archive (decoded: `tools/didfs.py`)
```
u32 dir_offset                        (0x00AEFA16 on the CD)
... file data ...
dir_offset: u16 nfiles (3488), u16 ndirs (29), u16 nexts (45), u16 ? (493)
            ndirs  x (u8 len, "tunes\airsuper\" ...)
            nexts  x (u8 len, ".dir" ".dat" ".par" ...)
            nfiles x {i32 hash, u32 offset, u32 size}   sorted by signed hash
```
- **Lookup** (`0x45804C`): lower-case the name, strip the `DID` prefix and following `\` (only when the prefix pointer `[0x49ACEC]` was set by `0x458148`), replace a leading directory with `chr('0'+i)` and the extension with `chr('A'+i)`, hash, then binary-search with a *signed* compare (`0x457DF8`).
- **Hash** (`0x4759E0`): over the bytes `c[i]`: `B0 = (B0<<1) | (c[i-1] < c[i])` (skipped after `_`), `sum += c`, `B1 = (B1<<1) ^ c` (or `B1 -= i` for `_`), `B2 += sum`. Result `(len&15)<<28 | (sum&15)<<24 | B2<<16 | B1<<8 | B0`.
- Files missing from the archive fall back to plain DOS opens, e.g. `d:\SPR\global_5.TM`. Some optional files are absent from the CD release (`global_5..8.tm`, `tfxa0033.lbm`, `newspp.lbm`, `cdc2.lbm`). The game tolerates that.
- Packed files are compressed with an **LZ + Huffman-style coder** (`0x46DF70`, helpers `0x46E078`/`0x46E0E3`/`0x46E112`/`0x46E167`, bit refill `0x46E218`). It is a bit reader built on `add al, al` / `adc al, al` carries kept across `dec dx` and calls (the refill returns with `stc`), with overlapping `rep movsb` back-references.

### 9.3 Start-up sequence observed
`open did.dat` (via DID) → read the directory (0xA380 bytes) → TFX.CFG ×3 → install IRQ1 (keyboard copied to DOS memory, like CONFIG) and int 2 (NMI) handlers → MSCDEX: install check plus `IOCTL 11` (audio track info) for every track (CD music) → **SB probe** at `BLASTER` (reset 226h, read 22Ah/22Eh, mixer 224h/225h, IRQ 7 → vector 0Fh) and load `samples\fx4.sam` → VGA 12h → 13h → Mode X → `tfx.par`, `cdintro.par` → `spr\global_1..4.tm` → `anim\didt0.256` (credits animation: DID logo, "Design and Direction Martin Kenwright", "Vectorgraphic Design", "Audio" …) → newspaper "Planet Police" picture with the UN Charter quote.

### 9.4 Runtime issues found by TFX.DAT (fixed)
- Lazy-flags macro evaluated the preserved carry of `inc`/`dec` *after* switching the flag state, which corrupted the decompressor's bit stream. The carry argument is now evaluated first.
- Calls to function entries that never decoded now go through `rt_call` (interpreter fallback) instead of failing to link.

### 9.5 Front end and flight (as observed)
- **Front-end flow:** Esc skips the credits and the office/briefing rooms → "CREATE NEW PILOT / LOAD PREVIOUS GAME" → UN Enrolment (wing; first name, surname and call sign, where Enter fills defaults such as "DIRK" / "LOOSE CANNON") → "Choose save game slot" (`tfxsav.0`…`tfxsav.9` in the game directory, with an overwrite Y/N prompt) → "Enrol to UNAF" over an animated UN flag (Arcade, Training, Simulator, Tour of Duty, Flash Points, UN Commander) → Arcade (Start, View Hi Scores, Main Menu) → "WAR!" newspaper → "LEVEL ONE" → cockpit.
- **Mouse:** the menus are mouse-driven (Enter only works in some screens). Like CONFIG, TFX integrates int 33h fn 0Bh mickeys 1:1 into its own cursor, starting at (0,0).
- **Arcade level 1** loads `missions\miss601.mis` and the Somalia theatre: `lev\somal_tw.txt`, `colours\somal_tw.col`, `som_twhi.col`, `infared.col`, `lev\somalia.asc|dat|lev|le2`, `waybin\craftype.cfg`, `waybin\miss601.way`, `cbn.dir` + `cbn\*.000`, about 300 `3\*.3` object models and `ss\*.ssd` shape definitions (carrier group, MiG-21/29, refueller, weapons), and the F-22 cockpit (`3\cpt_f22.3`, `spr\*.spr`, `dat\infcock.hdd/.hud`, `dat\misshud.*`, `dat\cockmap.bin`).
- **Timing:** there is no timer ISR. `0x435690` reads the BIOS tick count at 0:46Ch (or, if a resident driver provides one, `seg<<4 + 0x1900`). The frame limiter `0x41B998` busy-waits until at least 2 ticks have passed (4 in mode 6), giving about 9–10 fps at 18.2 Hz.
- **Main loop** `0x41EAC8` dispatches on the byte at `0x4A347A` (values 0, 1, 2 and 6, set by `0x4232E0` from four UI flag blocks and randomly by `0x435774`) through jump tables at `0x41EA90` / `0x41EAAC`. Mode 1 → `0x42F370` (simulation updates), 0 → `0x41D1AC`, 2 → `0x41D7DC` (cockpit/view drawing). Its exact meaning is still unknown.
- **3D engine ("Vectorgraphic"):** a bytecode interpreter. `0x44C528` and friends transform vertices with a fixed-point matrix stored *inside the code segment* (`0x447C6E`…), then `lodsw; jmp [eax + 0x44A814]` dispatches 64 opcode handlers (`0x44AA14`…`0x44C6F0`). Handler `0x44AA16`… **patches a conditional jump** at `0x44AAC1` (`7D 0D` jge / `7D 1D` / `7E 0D` jle / `7E 1D`), presumably the backface/clip test. The polygon fillers include an unrolled span routine with entries every 2 bytes at `0x4729A0`.
- **Code loaded at run time:** a position-independent sprite routine is loaded from the data files into the heap (`e8 00 00 00 00 / 58` = `call $+5; pop eax`) and called from `0x4037AA`. It runs in the interpreter.
- **Display:** Mode X triple-buffered pages at 0000h, 3E80h and 7D00h (CRTC start address flips between them), plus the cockpit template at BB80h, copied each frame with write mode 1. The cockpit bitmaps are drawn by the self-modifying transparent blitter (same code as CONFIG's 0x1A568).

### 9.6 Recompiler bugs found by flight mode (fixed)
- **`sahf` / `popf` did not reset the jcc fast path.** After `fcomp / fnstsw ax / sahf`, a following `jb`/`je` reused the operands of an earlier `cmp` in the same block, so every FPU comparison could branch the wrong way. This froze the flight model (speed, altitude and fuel stayed 0).
- **Patched conditional jumps** (opcode and displacement rewritten at run time) are now evaluated from the live bytes (`rt_jcc_live`), with the possible targets worked out from the immediate values the patch instructions store.
- The interpreter gained x87, `xlatb`, `loope/loopne`, `les/lds`, `bt*`, `bsf/bsr`, `ins/outs`, the address-size prefix on `jcxz`/`loop`, and re-entrancy (nested interpretation).

### 9.7 The 3D polygon path and why the view was black (fixed)
In flight the cockpit, HUD, MFDs, radar, moving map and flight model all worked, but the outside view stayed black. The cause was code patched through **computed addresses**, which the static SMC scan (§7.4) can't see.

**Polygon routine `0x44C904`** (called about 33,000 times in a few seconds of flight):
1. Copies the projected vertices (x, y words via the vertex table at `0x46815A`) into `0x447E2C…` and tracks the bounding box: max y `0x447850`, min y `0x447852`, max x `0x447854`, min x `0x447856`. The lowest vertex pointer goes to `0x447858`, and the vertex count is in `0x44785C`.
2. **Trivial reject against the clip rectangle**, which is held as immediates inside the code:

   | Instruction | Placeholder | Patched (320×200 view) |
   |---|---|---|
   | `0x44C994 cmp [maxY], imm16` (imm at `0x44C99B`) | 5678h | 0 (top) |
   | `0x44C9A3 cmp [minY], imm16` (`0x44C9AA`) | 8765h | 199 (bottom) |
   | `0x44C9B2 cmp [maxX], imm16` (`0x44C9B9`) | 1234h | 0 (left) |
   | `0x44C9C1 cmp [minX], imm16` (`0x44C9C8`) | 4321h | 319 (right) |

   The clipping block `0x44CAAF–0x44CB11` repeats the same four values as `cmp` and `mov` immediates, and calls the edge clippers `0x44D696` (top), `0x44D5AA` (bottom), `0x44D88F` (right) and `0x44D782` (left).
3. **Back-face test:** the sign of the cross product of the first two edges.
4. Coordinates are clamped to ±3FFFh when the box overflows 16 bits.
5. The vertex ring is duplicated (so edges can be walked without wrapping), then `0x46A5FC` and `0x46A688` build the left and right edge lists. Either one returns CF=1 to skip.
6. The scan converter `0x46AB50` runs, then the span filler is called through `[0x447CEA]` with the colour from `0x447CA8`. The `mov dx/cx, 1234h` at `0x44CBCB` / `0x44CBCF` receive the top y and the height.

With the placeholders left in the compiled code, the reject test needed max y > 5677h and min y < −789Ah. That never happens, so every polygon was rejected and nothing reached the rasterizer.

**Scan converter `0x46AB50`** (fully self-modifying; Ghidra's decompile makes this obvious):
- On entry it pops its own return address and writes `ret − 0x46AC2E` into the rel32 of the `jmp` at `0x46AC29`, so the routine's exit jumps straight back to the caller.
- It aligns ESP and pushes a sentinel `013F0000h`. Then, per pair of left/right edge records (26 bytes each: `+0` height in lines, `+2` x as 16.16 fixed point, `+4` next-x word, `+6` dx/dy step), it pushes one dword per scanline. That dword is built with two `shld eax, reg, 16` from the left and right x, so it packs left x and right x.
- The per-edge step values live in the dwords at `0x46AC2E` / `0x46AC32`. These are data inside the code, swapped with ESI/EDI by `xchg`.
- The span count selects an entry into an unrolled loop: `jmp eax` with `eax = 0x46B4E8 − 11·lines`. Each step is 11 bytes (`shld; shld; push; add; add`).
- The loop ends at `0x46B4F4 jmp rel32`. Its displacement is patched to one of `0x46ABA4` (advance right edge), `0x46ABCC` (advance left edge) or `0x46ABF4` (advance both), depending on which edge runs out first.
- At the end it pushes the two "next x" words and takes the patched `jmp` back to the caller. The caller pops the span list off the stack.

**Runtime-observed SMC.** The runtime's live `c` command lists every code byte that differs from the loaded image. In the cockpit, 624 runs (5,468 bytes) had been modified. They include the clip rectangle above, the transform code's `78 56 34 12` / `34 12` immediates (`0x4489A3…`, `0x448C90…`, `0x4491E1…`, many more), and tables kept inside the code object. `tools/smc_merge.py` stores these as link addresses in `tools/smc/TFX.DAT.txt`. The recompiler passes them to the analyzer, and every instruction overlapping a modified byte is generated as a live-byte instruction (`rt_step`, dynamic `jmp`, or `rt_jcc_live`, with the observed byte added as a jcc target).

Two runtime bugs then surfaced on the newly reached code, both fixed:
- **Return by jump.** The scan converter exits with a patched `jmp` to the caller's continuation. `rt_call(t, r)` with `t == r` now returns at once, and the interpreter returns when execution reaches its expected return address by any route.
- **16-bit push in the interpreter.** The `push word [esi+4]` / `push word [edi+4]` at the scan converter's exit pushed dwords, which shifted the span list by 4 bytes. Interpreter pushes and pops now honour the 0x66 prefix.

With these fixes the world renders: sky gradient, shaded terrain, the horizon tracking roll and pitch, and haze.

**Edge builders** `0x46A5FC` (walks the vertex ring forwards) and `0x46A688` (backwards) build 26-byte edge records from the lowest vertex down. Each record holds the line count, x in 16.16 starting at x+0.5 (plus half a step on x-major edges), and the step dx·2·recip(dy). They use the reciprocal word table at `0x4483AC`, which is generated at start-up inside the code object. The list ends with a zero count and the final x. CF=1 (no lines) skips the polygon.

**Span filler** `0x472560` (through `[0x447CEA]`) pops the return address and patches its own exit `jmp` at `0x4725C0` the same way. It selects VGA map-mask writes per 4-pixel column and pops `(left, right)` word pairs until the `013Fh/0000h` sentinel. It jumps into an unrolled store sequence through the table at `0x471F08`, indexed by span width and alignment.

**Sound:** in flight the game starts CD audio (MSCDEX Play Audio) and the music plays from the BIN/CUE through SDL.

**More renderer self-modification:**
- The polygon routine `0x44CBE8` (a second variant of `0x44C904`, used for effects such as missile smoke) temporarily overwrites `mov bx, [0x447E2C]` at `0x44CCA2` with `EB 4A` (`jmp short 0x44CCEE`) and later restores it. The analyzer now works out the branch targets of short jumps stored over ordinary instructions, and the generated code checks the live opcode before single-stepping.
- The rasterizer `0x439190` (and `0x437DB4`) set VGA plane masks with long unrolled runs of `outsb`, after a single `mov edx, 3C4h` near the routine's start. `outs*` is accepted as code when a `mov (e)dx, 3xxh` occurs within the previous 4 KB; plain text decodes 6E/6F ("n"/"o") as `outs`, so it can't be accepted unconditionally.

### 9.11 The 3D pipeline in detail (for the native renderer, RENDERER.md)
- **Vertex records** are 20h bytes in a table addressed as `0x46814C + offset` (the offset is what the primitives receive):

  | Offset | Contents |
  |---|---|
  | +00h/+04h/+08h | view-space X/Y/Z (32-bit) |
  | +0Ch | flag: 0 projected, FFFFh in front of the near plane |
  | +0Eh/+10h | screen x/y (16-bit) |
  | +14h/+18h/+1Ch | the rotated local coordinates the VM's transform handlers write |

- **Transform handlers:** `0x44C528` (two-axis delta, then dispatch), `0x44C594` (writes record +14h.. directly into `0x468160 + offset`), and the one-axis forms `0x44C288` and `0x44C3B0`. They rotate delta-coded vertices with the object-to-view matrix kept in the code segment at `0x447C6E` (16-bit entries at C6E, C72, C76, C7A, C7E…).
- **Projection `0x448990`** (EDI = first record, ECX = count, stride 20h):
  - view = local + object origin, the origin being patched immediates at `0x4489A3/A9/AE` (written by the handler at `0x44B9C7`);
  - if `Z >> 16` > 10h: x = ((X >> 8) / (Z >> 16)) + cx and y = ((Y >> 8) / (Z >> 16)) + cy, i.e. a focal length of 256 pixels (64° horizontal FOV at 320 wide);
  - cx/cy are patched immediates at `0x4489D8/ED` (159/56 in the cockpit view);
  - otherwise it sets the flag, writes `F8h` into the visibility test at `0x44AFB6`, and switches the primitive pointers `[0x447CCE]`, `[0x447CDA]`, `[0x447CE6]` to the clipping versions.
- **Primitive pointers** (set by the mode handlers `0x448812` single-colour and `0x44886B` normal):

  | Routine | Kind | Fill | Clipping version |
  |---|---|---|---|
  | `0x44C904` | flat | `[0x447CEA]` | `0x466EF4` |
  | `0x44CEB8` | Gouraud: one extra word per vertex at +180h of the edge buffer | `0x4756CC` | `0x4675D0` |
  | `0x44D194` | textured: one extra dword per vertex at +380h (two words) | `0x475334` | `0x467B84` |
  | `0x44CBE8` | effects variant | — | — |

  - Inputs: ESI = word list of record offsets, ECX = count.
  - The colour byte is `0x447CA8`. Colour stores in the VM handlers go through self-modified pointers (`0x44B0C2`, `0x44B105`, `0x44B11B`, `0x44BBC6`, `0x44BFBE` are patched to point at `0x447CA8`, `0x447CAA` or `0x447BFC`).
- **Measured** in Arcade flight (Somalia, detail HIGH): about 800 primitives per frame, all flat or flat-clipped (about 216 a frame) plus a few effects; no Gouraud or textured primitives in that scene. A re-projection of the captured view-space vertices with the formula above reproduces the game's screen points for all but about 1 in 2,000 vertices.
- **Other projections** (sprites and points): `0x44BE12` (point: x = X/Z + [0x447736], y = (Y >> 8)/Z + [0x447738]), `0x44B48B` and `0x44B538` (sized sprites), `0x44A5B0` (particles, against the same clip rectangle).

### 9.12 Shading, textures, discs and screen-space polygons
Found for stage 4 of the native renderer (disassembly, plus live traces of the span fills with the runtime's `i`/`t` commands).

- **Remap table.** Every colour goes through a 256-byte table, pointer `[0x44787E]`:
  - The default table is `0x4B4C9C` (identity), stored by `0x4488FB`, `0x449323`, `0x449396`, `0x449409`, `0x44947F` and `0x44BF08`. Lighting code (`0x449F98`, `0x44A18D`, `0x44A383`, `0x44A4FD`, `0x44A710`) stores per-object tables; the arming-screen aircraft uses `0x4B4D9C`.
  - Flat colours are remapped once in the VM: `0x44BFB4` does colour = table[bytecode byte] into `0x447CA8`.
  - Gouraud and textured spans remap every pixel (`xlatb`).
- **Gouraud.** The VM handler (e.g. `0x44B9F4`) writes one shade word per vertex, in list order, at `0x447FAC + 4i` (byte << 8); the near-plane version copies them into record +12h.
  - The span fill `0x4756CC` (pointer `[0x468112]`) interpolates the shade and writes table[shade >> 8].
  - Checked against the game's own pixels at polygon centroids: 73% match (the rest are pixels overdrawn later or on edges); the alternatives score lower.
- **Textured.** Handler `0x44B8DC` reads a texture id:
  - Index `0x447C84 + id`: FFFFh means not available, and the primitive pointer is switched to a skip routine. Otherwise `0x457AFC(id)` returns `[0x4B12FC + id*4]`, stored in `[0x44787A]`.
  - It then copies one dword per vertex from the bytecode to `0x4481AC + 4i`. Low word A, high word B.
  - Edge walkers `0x46A924`/`0x46AA38` interpolate A (+380h) and B (+382h) as integers. The span emitter `0x46B6EC` packs (A << 16 | B) per edge and pushes three dwords per scan line (left attributes, right attributes, x pair).
  - The span fill `0x475334` (pointer `[0x46810E]`) turns a word w into 16.16 with texel = w >> 7, and fetches `[tex + (B >> 7) * 256 + (A >> 7)]`: 256-byte rows, row from B, column from A, both wrapping at 256.
  - The fetched byte is remapped; a remapped 0 is transparent (not written).
  - Confirmed by tracing the fetch addresses (e.g. A = 3633h, B = 2980h: row 83, column ≈ 106 from base `0x22F4024`).
  - Textures live in high memory (`0x22E7024`, `0x22F4024`… 53 KB apart on the arming screen) and are mostly transparent around the drawn parts.
- **Back-face tests.** Each primitive's test is `(y2−y0)(x1−x0) − (x2−x0)(y1−y0)`; it skips the polygon when negative. The tests live at `0x44CA16` (flat), `0x44CFD6` (Gouraud) and `0x44D2B2` (textured), each `0F 88 js`. The near-plane copies re-enter these routines before the test.
- **Viewports.** One frame can contain several 3D viewports. The arming screen has the aircraft, a small rotating model, and one per weapon in the list.
  - Each has its own projection centre (immediates `0x4489D8/ED`) and clip rectangle (flat `0x44C99B/AA/B9/C8`; the other primitives and the disc routine have their own copies), patched before each viewport is drawn.
  - The projection scale is always 256.
- **Discs** (the pilot's helmet, lights).
  - The sprite handler `0x44B48B` reads a vertex offset and a world radius from the bytecode, skips near vertices, and computes r = (radius << 7) / (Z >> 16).
  - It calls `0x44F26A(x, y, r)` (cdecl words) with EBX still holding the record; the return address is `0x44B4C2`.
  - `0x44F26A` rejects r < 1 or r ≥ `[0x466EA0]`, clips against its own immediates, and builds spans from the circle table at `0x465A84`.
  - It fills them through `[0x447CEA]` in colour `[0x447CA8]`. `0x44B4C6` sets that colour from `[0x447CB0]`, and the fill from `call [0x46810A]` with AL = `[0x447CB1]`.
- **Screen-space polygons** (sky gradient, ground haze).
  - The routine block around `0x449300` builds 2D-only vertex records: screen x/y, with stale X/Y/Z.
  - It calls the primitives directly: Gouraud from `0x44932D` (return `0x449332`), textured from `0x4493A0` (`0x4493A5`), flat from `0x449413` (`0x449418`) and `0x449489` (`0x44948E`).
  - Some fills are a code stub generated at run time at `0x447CF0`.
  - Every other primitive call goes through the VM's `call [0x447CE6]` (or the near-plane routines).
  - In Arcade flight these are about 32 polygons a frame.

### 9.13 Frame-to-frame identity, and span routines that patch their own return
- **Primitive identity.** The primitives get ESI pointing at their vertex-offset list inside the model's bytecode. The address is the same every frame for the same polygon of the same model, and the list order fixes the vertex order. The native renderer's motion vectors rely on this (RENDERER.md, stage 5).
  - Models drawn several times per frame share the address: in Arcade flight about 400 of 580 primitives come from repeated lists, mostly terrain tiles.
  - The set of tiles changes as the camera moves, so a list can stand for a different ground cell in the next frame.
  - In chase views the player's aircraft keeps nearly the same view-space position from frame to frame, moving with the camera.
- **Span routines that return through a patched `jmp`.** Sixteen span and edge routines start with `pop esi` (or `pop eax`), `sub esi, X+4`, `mov [X], esi`. That stores the return address relative to a `jmp rel32` at X−1, which they end with instead of `ret`.

  | Routine | Patched displacement |
  |---|---|
  | `0x46AB50` | `0x46AC2A` |
  | `0x46B4FC` | `0x46B6DC` |
  | `0x46B770` | `0x46B8C2` |
  | `0x472560` | `0x4725C1` |
  | `0x4725FB` | `0x4726CA` |
  | `0x4726CE` | `0x47277E` |
  | `0x4727FF` | `0x4728AE` |
  | `0x47292F` | `0x472A31` |
  | `0x472C36` | `0x472D83` |
  | `0x472F60` | `0x4733EC` |
  | `0x473ACD` | `0x473D88` |
  | `0x47415A` | `0x474239` |
  | `0x47424C` | `0x474311` |
  | `0x47434B` | `0x474BA4` |
  | `0x475334` | `0x4756A9` |
  | `0x4756CC` | `0x475831` |

  - Seven of these displacements had never been observed while the runtime's self-modification list was collected, so they were compiled as `jmp +0`, falling into the next instruction.
  - `0x47292F` is the dithered disc fill: every other row, selected with `and ebx, 1`, and the span width indexes a code table at `0x472AEE`/`0x472B92`. It crashed the first time a dithered disc was drawn (a chase-view dive): `jmp 0`.
  - All are now in `tools/smc/TFX.DAT.live.txt`, found by scanning the image for the pattern.

### 9.14 Lines, sky bands and the culling margin
- **Model lines.** The VM handler `0x44AFAC` reads two vertex offsets. If one end is near, it clips it in 3D with `0x4771EC`. It stores the screen points at `0x447BFE` and calls the 2D line routine `0x44EA99` (ESI = x0, y0, x1, y1 words).
  - On that call, EDX and EBX hold the two records.
  - The line colour is `[0x447BFC]`, set by handler `0x44B0B8` through the remap table.
  - `0x44EA99` clips against its own patched rectangle and draws through the span fills.
  - It is also called from `0x44EA8E`, `0x45CD83`, `0x45D8E3`, `0x4605FE` and `0x462C9B` (2D lines: map, HUD and others). Calls from the VM return to `0x44B05D`.
  - In a chase view about 185 lines are drawn per frame: rivers, roads, edge lines on the aircraft.
- **The line handlers.** All three VM line handlers call `0x44AFAC`, which returns with the carry set when the line is rejected; they differ in how they draw it:
  - `0x44B062` leaves the drawing to `0x44EA99`;
  - `0x44B078` stores a colour word at `0x447BFA` and calls `[0x468102]` (`0x46ECEC`);
  - `0x44B09C` calls `[0x4680FE]`, the second line rasteriser `0x454336`, with ESI → the colour word `[0x447BFC]` followed by the clipped end points (return address `0x44B0AC`). Distant terrain detail is drawn this way: in Arcade about 55 of 125 lines per frame, all on the horizon (colours 74, 119–123), and none in the Yugoslavia runway starts checked.
- **Polygon outlines.** `0x46053F` draws a polygon's edges, given the same list of vertex-record offsets as a fill (ESI, ECX): it applies the back-face test, skips edges with a near end, clips each with `0x44EA99` and draws it with `[0x4680FE]` in colour `[0x460535]`.
  - `0x46049C(list)` uses it for 2D outlines (colour, count, then screen points copied into the vertex records); it is called from `0x41A448`–`0x41A6D4`.
  - `0x460464(colour)` points the VM's polygon fills `[0x447CC6]`/`[0x447CCA]` at it, and `0x460487` restores `0x44C904`/`0x466EF4`. Neither has a static caller, and `0x46053F` wasn't called at all in flight (Arcade, Yugoslavia).
- **Sky and haze bands** (the `0x4493xx` block, 9.12) are drawn back to front, colours 240–247 in Arcade.
  - Each is the region between two parallel lines (the horizon offsets), clipped to the screen: (0,52)–(319,52)–(319,54)–(0,54) in level flight, (138,0)–(184,200)–(181,200)–(135,0) when rolled.
  - The routine clips x to the screen sides (0 and 319) itself. y is clipped at 0 and 200 only sometimes; in a climb, polygon corners at y = −15,952 and y = 2,868 were logged.
  - It never clips to the 3D view's clip rectangle, which in the cockpit view ends at the panel (about row 145). The span fill's clipping takes care of that in the original.
  - There are 32 bands per frame in Arcade, drawn in a chain where each shares its boundaries with the previous and next band:
    - sky bands (colours 240–251, the top four all 251) from the horizon upward, the last reaching y ≈ −32000;
    - then ground-haze bands (224–239) from far below the screen up to the horizon.
  - Band boundaries can fall exactly on rows 199/200, so they can coincide with the screen edge.
  - The chain contains zero-height bands and consecutive identical ones (e.g. two colour-237 bands with the same corners).
  - **16-bit corners.** The 2D records hold screen x/y as words (`+0Eh`, `+10h`). The last sky band's far edge (y ≈ −32,000 in level flight) goes beyond the 16-bit range in some attitudes: −47,400 is stored as +18,136. The band's winding then flips, and the game's ordinary back-face test (flat: `0x44CA16`) skips it. Seen above the clouds in a banked climb: corners (0,18136) (319,18290) (319,−1036) (0,−1190).
  - **Off-screen slivers.** In a near-vertical roll the bands run up and down the screen. y is clipped to 0/200 and x is not, so some bands lie thousands of pixels to the side. One ground-haze band was a degenerate sliver: (−6161,0) (−6237,200) (−6237,200) (−6161,199). One corner was rounded to 199, not 200. The game draws nothing of it.
- **Culling margin.** The game submits a good deal of geometry beyond the screen sides. In Arcade level flight 186 polygons per frame lie wholly outside, nearest edge distances (game pixels beyond the side, focal length 256):

  | Distance | Polygons |
  |---|---|
  | 0–40 | 60 |
  | 40–80 | 35 |
  | 80–120 | 16 |
  | 120–160 | 25 |
  | 160–200 | 10 |
  | 200–240 | 6 |
  | 240+ | 34 |

  Vertices reach thousands of pixels off-screen. A 16:9 view at the original vertical FOV needs 53 game pixels per side.

### 9.15 Weather, clouds and their stippled fills (Simulator)
- **Time of day and weather** (Simulator: dawn, daytime clear/cloud/storm, nighttime clear/cloud; the ocean theatre has daytime only) change the palette entries of the sky (240–251) and ground-haze (224–239) bands, not the band geometry. In cloud and storm the haze is grey and low and hides distant terrain early.
- **Clouds are discs.** The cloud layer is made of hundreds of filled discs (the sprite handler `0x44B48B` → `0x44F26A`, 9.12) in colours 32–41: about 350 puffs per frame above the layer, at view depths 1,600–6,800 and beyond. Inside the layer the view is one flat grey.
- **Radius limit.** `0x44F26A` draws nothing when r < 1 or r ≥ `[0x466EA0]` (`cmp cx, [0x466EA0]` / `jge`, a signed word; 100 in flight). Puffs close to the camera (r up to 1,684 was seen) are skipped this way.
- **Stippled fills.** A disc is filled through `[0x447CEA]`. For cloud puffs this points to one of three span routines generated at run time:
  - `0x472560`: the ordinary solid fill (left-edge plane mask from `0x46E394`, then unrolled spans by length), used by the near puffs (colours 32–35);
  - `0x4726CE`: even rows write VGA planes 1+3, odd rows planes 0+2 (`and al, 0Ah` at `0x47275C`, `and al, 5` at `0x4727D0`): a checkerboard, 50 % coverage (colours 37–39);
  - `0x4727FF`: even rows plane 1, odd rows plane 3 (`and al, 2` at `0x47288C`, `and al, 8` at `0x472900`): 25 % coverage, the farthest puffs (colours 39–41).
  - Both stippled routines pick the row's path with `and ebx, 1` / `jmp [ebx*4 + table]` (row parity) and AND the plane mask from `0x46E394`/`0x46E254` with the pattern before `out 3C5h`. In Mode X the plane is x mod 4, so these are screen-door transparency: overlapping far puffs build up white, and their edges show the checkerboard.

### 9.16 Points (lights)
- **VM handlers.** `0x44BA9C` (1×1) and `0x44BAF4` (2×2) read a colour byte (remapped through `[0x44787E]`, into CH) and a vertex offset word.
  - They skip the point when the vertex's near flag (record `+0Ch`, at `0x468158 + offset`) is set, or when its projected x/y (`+0Eh`/`+10h`) is off screen: 0–319 × 0–199 for 1×1, strictly inside for 2×2.
  - Then they push ESI (the bytecode pointer, just past the vertex offset) and call the plotter through `[0x468116]` (1×1) or `[0x46811A]` (2×2). The calls return to `0x44BAE6` and `0x44BB3E`.
- **Plotters.** `0x4759A8` writes one pixel (x = EBX, y = ESI, colour CH; plane mask from `0x46C1E0`). `0x4759D4` writes a 2×2 block: two planes (`33h << (x & 3)`), two rows, and a second byte when the block straddles a 4-pixel group. `0x475A10` onwards are list versions.
- **Use.** At night these are the lights: runway edge and centre-line lights, approach lights and town lights, in colours such as 18–23, 49 and 144–146. A night runway start in Yugoslavia has about 290 per frame; Arcade by day has none.
- The plotters lie just past the Gouraud span fill. A pixel-ownership test that took only the span fills as "3D" kept the lights as UI pixels.

### 9.17 How the HUD is drawn
Found with the VGA write profile (`g`), the call histogram (`z`/`h`), live disassembly and the per-pixel writer probe (`w X Y`), in Arcade flight. Of 40 green HUD pixels picked at random, 33 came from the bitmap blit, one from the HUD line routine and six from the sprite blitter.
- **1-bit bitmap blit: most of the HUD.** `0x45CFA4(bitmap, y, x, colour)` (`enter`; y at `[ebp+0Ch]` and x at `[ebp+10h]` are sign-extended words, the colour a byte at `[ebp+14h]`; it returns with `ret 10h`) draws a 1-bit bitmap.
  - It draws the text (MIG21, S/H/A/R, the pitch-scale numbers, 6439…), and also the gun ring, the pitch ladder, the horizon bar with the aircraft symbol, and the heading tape: these are bitmaps too, not lines.
  - The bitmap is a width byte, a height byte, then rows of ⌈width/8⌉ bytes, most significant bit first. Set bits are drawn in the colour; clear bits are left alone.
  - `0x45D050` clips it to the rectangle at `0x44FB78` (x min), `0x44FB7A` (x max, exclusive), `0x44FB7C` (y min), `0x44FB7E` (y max, exclusive). The pixel loop is at `0x45CFD0`–`0x45D04F` (writes at `0x45D01D`/`0x45D02F`); the routine ends at `0x45D171`.
  - About 80 calls per frame in the cockpit view.
- **HUD line rasteriser.** `0x454212` draws an already clipped Bresenham line, end points included, from globals just before its code: colour `0x454208`, x0 `0x45420A`, y0 `0x45420C`, x1 `0x45420E`, y1 `0x454210`. About 10 calls per frame, all from the block `0x45E7xx`–`0x45ECxx` (e.g. the target marker). It ends at `0x454335`.
- **Second line rasteriser.** `0x454336` is the same with ESI → five words: colour (low byte), x0, y0, x1, y1. It patches the colour and the row step (±50h) into its own code (`0x4543EC`, `0x4543F2`, `0x454402`, `0x454405`, `0x45442C`), and ends at `0x454445`. It is reached through `[0x4680FE]`, from the VM's line handler `0x44B09C` (model lines, mostly distant terrain detail; 9.14) and from the polygon outline routine `0x46053F`. It doesn't draw the HUD.
- **Pixel plot.** `0x45658C(y, x, colour)` (`ret 0Ch`) writes one pixel; it ends at `0x4565D6`.
- **Sprite blitter.** `0x44FEF0`–`0x450238` draws 8-bit sprites with colour 0 transparent (`lodsb`/`test al, al`/`stosb`, patched immediates), one plane at a time through `0x4505E2`, `0x450603`, `0x450763`, `0x450784`…: the cockpit art, the MFD text and pictures, the status bar, and some HUD pieces (the speed and altitude brackets, the target box).
- **Not drawing:** `0x453CAC`/`0x453E8C`/`0x453D80` keep a sorted queue of up to 348 entries (`0x450B6C`/`0x450E24`). `0x458E34(...)` (5 arguments, `ret 14h`) and `0x459040`… fill rectangles.

### 9.18 Effects: disc fill styles and distant objects as dots
Found by firing missiles in Arcade and following them (F8), with the per-writer count of the pixels still drawn by the game (cmdfile `U`).
- **Missile smoke and flames** are discs (`0x44F26A`, 9.12) from the sprite handler. Most use the solid fill.
- **Fill styles.** `[0x46810A]` = `0x4752B8` returns a fill routine from the table at `0x4752C3`:
  - `0x4726CE` (50 %) and `0x4727FF` (25 %), as for the cloud puffs (9.15);
  - `0x47292F`: one plane of every other byte on every other row (about 1/16);
  - `0x472C36`: one plane of every fourth byte on every other row (about 1/32);
  - `0x472F60` and `0x473ACD`: two planes per row, each kept by a random bit (`add ch, cl` / `adc cl, ch` / `and al, cl` / `and al, 5` or `0Ah`), about 1/4;
  - `0x47415A` (twice) and `0x47424C`: solid spans with masked edges.
  - Measured in flight (pixels written over the area of unclipped discs, the solid fill giving 1.11 for the pixel rounding): 50 % → 0.57, 25 % → 0.30, `0x47292F` → 0.08.
- **Distant objects as dots.** `0x44A57D` (2×2) and `0x44A5F0` (1×1) draw object EDI from its view-space position: X at `[0x45212E + 4i]`, Y at `[0x45269E + 4i]`, Z at `[0x452C0E + 4i]`.
  - x = (X >> 8) / (Z >> 16) + centre, and y likewise, against patched bounds. Objects with Z >> 16 ≤ 16 are skipped.
  - The colour comes from `[0x451396 + i]` through a table at `0x4B4C9C`.
  - They call the point plotters through `[0x46811A]` / `[0x468116]` (returning to `0x44A5EF` / `0x44A662`): distant aircraft, debris and sparks.

### 9.19 The sprite blitter
- **Entry.** `0x450454(y, x, index, bank)` (`enter`, `ret 10h`; reached through `[0x468126]`) draws sprite `index` of a bank, centred on x (by the sprite's width) and y (by half its height).
  - The bank starts with a word table of offsets (`bank + 1 + 2·index`).
  - A sprite starts with two bytes: width, height.
  - `0x4504F1` clips it against `0x44FB80`–`0x44FB88`. The page is `[0x455907] << 4`, and rows come from the table `0x45590B`.
  - Drivers `0x4505E2`, `0x450603`, `0x450626`, `0x450649`… (one per starting plane, from `0x45038D`) set the map mask and call the row routine `0x44FB8C`–`0x450238` once per plane. The row routine is unrolled `lodsb` / `test al, al` / `stosb`, so colour 0 is transparent.
  - `0x4503BD` is a non-centring twin with no caller.
- **What it draws** (cockpit view, Arcade; return address → item):

  | Return | Item |
  |---|---|
  | `0x40F7D6` | the speed/altitude brackets: 129×9, one colour |
  | `0x4103E0` / `0x4103F8` | the gun-cross markers either side (7×5), one colour, and sprites 1 and 2 of the same bank (off screen in the test) |
  | `0x4106C5` | the heading tape ticks: 66×9, one colour |
  | `0x41136C` | the "THREAT" label: three colours |
  | `0x411C90` | the panel's digits: 3×5 shaded LCD glyphs, colours 2, 1, 107 |
  | `0x416A88`, `0x41849F`, `0x411AA8` | the MFD pages: whole multi-colour pictures of 52×35 to 62×46 |
  | `0x40FA59` / `0x40FAB3` | warning lights |

- **Status bar** (chase views): a separate font routine around `0x45BA58`. It draws a blocky two-colour font with a drop shadow, nibble-packed through `xlatb`.

### 9.20 Cockpit assets (for an upscaled art pack)
Found with the archive lookups logged (`TFX_INTERP=45804c TFX_DEBUG=1`: `0x45804C` runs interpreted and logs each name) and the cmdfile `A DIR` dump, in Arcade.
- **Files the cockpit loads:**
  - sprite banks `spr\ladnums.spr`, `warlites.spr`, `warlite2.spr`, `disp3.spr`, `disp32.spr`, `targcros.spr`, `disp2.spr`, `compdisp.spr`, `newhud2.spr`, `nwsg.spr`, `aoasprs.spr`, `pitfont.spr`, `autolite.spr`, `disp12.spr`, `somalmap.spr`, `englite.spr`;
  - `dat\infcock.hdd`, `dat\infcock.hud`, `dat\cockmap.bin`;
  - also tried: `spr\global_1..8.tm` (5–8 are absent from the CD).

  They are packed in `DID.DAT` (LZ + Huffman, 9.2), so they are taken from memory instead.
- **The template at `BB80h`** (copied onto each frame with write mode 1, 9.1) holds the lower cockpit panel: 320×55 game pixels (rows 0–54), shown at screen rows 145–199. It covers the MFD frames, the side panels with their hazard stripes, and the CHF/FLR labels. The rest of that memory holds unrelated data.
- **Static sprites** (`0x450454`, 9.19), drawn over it every frame:
  - the left MFD's page (the labels SPD, ALT, BRG, FUE, RNG on two shades: 56×37, bank `…b024` index 3);
  - the radar page (rings and the red sector: 62×46);
  - the speed/altitude brackets, the heading tape, the "THREAT" label and the gun-cross markers.
- **Dynamic content** drawn on top: the values (font routines), the radar contacts, the map (52×35, drawn each frame), and the panel's 3×5 digits.
- **The canopy frame** (the top and side struts) comes from neither. Its pixels' writers are addresses around `0x0234xxxx`, a different one for each 4-pixel group, in memory outside the program image. Probably generated "compiled sprite" code; not verified.

### 9.21 Object visibility and the draw queue
- **Draw queue.** `0x453DE9` empties it at the start of a frame; `0x453E8C` (AX = depth key) adds an entry, sorted, and refuses past 15Ch = 348 (`[0x450B68]` count, `[0x450B6A]` the new entry's slot). Per entry: flags at `0x4510DE + i` and `0x45123A + i`, a detail level 0–15 at `0x451396 + i`, the model at `0x45164E + 2i`, the view-space position at `0x45212E`/`0x45269E`/`0x452C0E + 4i`.
- **Adding an object.** `0x454A63`, `0x454B15` and `0x454BE5` (7 or more arguments, `ret 1Ch`…) transform a world position to view space with `0x4491F8` (ESI/EDI/EDX = X/Y/Z), test it with `0x44915C` and add it. `0x45B6xx` (three variants) add an object whose view-space position the caller already has, without that test.
- **Visibility test `0x44915C`** (AX = radius, ESI/EDI/EBP = X/Y/Z, high words used; carry set = not visible):
  - behind the camera by more than the radius plus 10h: out; farther than 4000h: out;
  - `|x| − r ≥ z`: out (more than 45° to the side); else `((|x| − r) << 8) / z > limit`: out. The same for y.
  - The limits are immediates patched by the viewport setup `0x448A..`: x at `0x4491AE` = centre − left (A0h = 160 for the full view), y at `0x4491D6` (64h = 100). The shift counts (`0x4491A4`, `0x4491CC`) are live bytes too.
  - So an object counts as visible when its centre, less its radius, projects inside the viewport. The polygons of large objects (terrain pieces) reach well beyond it, which is the "culling margin" of 9.14.
- **Wide views.** The port replaces the test for the full-width viewport when its 3D view is wider than 4:3 (`runtime/gamefix.c`, a hook at the entry): the x limit grows by the extension (53 pixels at 16:9, 267 at 32:9) and the 45° cap, which ends at 256 pixels, is dropped. In Arcade at 32:9 that keeps about 20 more objects a frame; the queue peaked at 115 of 348.

### 9.22 The joystick
- **Config (TFX.CFG).** `CONTROL=` (handler `0x435700`) sets the control method `[0x4A3478]` (0 keyboard, 1 joystick, 2 mouse) and its copy for the in-game options screen `[0x4A3AED]`. `STICK=` (`0x4356F0`) sets the type `[0x4A3267]`: 0 normal, 1 CH Flightstick Pro, 2 Thrustmaster. `JOYSCALE=` is the float `[0x49AC68]` (512). `JOYSTICK=` (`0x4357F8`) has 11 numbers:
  1. the reader's poll limit `[0x49AE68]` (4000);
  2. x centre `[0x4B0C6A]`, 3. x range `[0x4B0C68]`, 4. y centre `[0x4B0C60]`, 5. y range `[0x4B0C66]`;
  6–7. the throttle's two ends: the smaller to `[0x4A3254]`, the larger to `[0x4A3252]`;
  8–10. rudder minimum, maximum and centre: `[0x4A324C]` = the smaller half range, `[0x4A3246]` = the centre;
  11. the Thrustmaster hat's reference `[0x4A3242]`.
  After the last line `0x435BA8` sets the reader's axis mask `[0x49AE6C]`: 3000h (axes 1 and 2); 7000h for a Thrustmaster with calibrated pedals; bit 15 (axis 4) for a Flightstick Pro, or a Thrustmaster with a hat reference.
- **Reader `0x4602D8`** (port 201h):
  - reads the buttons first: `[0x49AE10..13]` = FFh for buttons 1–4 (port bits 4–7, low = pressed);
  - waits up to 20 polls for the x and y bits to be low, else gives up (all results 0);
  - writes the port (starts the four one-shots), discards one read, then polls with interrupts off: each poll adds each axis bit to its counter, until the masked axes are low or the poll limit runs out (then: all results 0);
  - keeps the last four counts per axis in rings (`0x49AE18`, `0x49AE2C`; axes 3 and 4, halved, at `0x49AE3C` and `0x49AE50`). X `[0x49AE14]` and y `[0x49AE28]` are the ring's sum / 16, a quarter of the average count. Both 0 means "no joystick".
  - `0x46028C` is the detection (one read, then five more); the menus' input poll `0x4601CC` takes buttons 1 and 2 as a key press.
- **Stick position `0x45401C`** (called once a frame from `0x41BB34`, only in joystick mode): `v = (x − centre) / range`, limited to ±1, then `v·|v|·JOYSCALE` into `[0x49AC60]` (roll) and `[0x49AC62]` (pitch), limited to ±200h. The flight model's control routine `0x429660` has one branch per control method; the joystick branch (`0x429B4F`) turns the two words into the roll and pitch demands.
- **Buttons, throttle, rudder, hat: `0x415A8C`** (from the per-frame input routine `0x415DF4`, only in joystick mode):
  - The four buttons form an index (pressed bits inverted) into a table of 16 entries of handler, repeat delay and timer: `0x497808`, or `0x497788` for the Flightstick Pro. Only single buttons have an action: 1 = `0x413504` (fire, the Space handler), 2 = `0x413FD4` (air-to-ground weapons, Backspace), 3 = `0x413AF4` (air-to-air weapons, Enter), 4 = `0x41510C` (next target, C). The Flightstick Pro table adds its hat, which that stick reports as combinations: 1+2+3+4 = up `0x4148A4`, 1+2+4 = right `0x414A84`, 1+2+3 = down `0x414B14`, 1+2 = left `0x4149F4` (the look-around views).
  - Flightstick Pro: the throttle `[0x4A33F8]` (float; 0.6 idle to 1.0 full) = `1 − 0.4·(v − min)/(max − min)` from axis 4, while it is at least 0.6 and both ends are calibrated.
  - Thrustmaster: the rudder `[0x4A48F0]` (double, ±1) from axis 3; the hat from the level of axis 4 (0, ¼, ½, ¾ of the reference ± 1/11).
  - The control routine multiplies the rudder by 0.8 a frame above 205 (speed) and copies it to `[0x4A4730]`; below that the left/right keys move it. No stick type has both a throttle and a rudder.
- **The port** (`runtime/joy.c`) presents SDL devices as a Flightstick Pro: the axis bits of port 201h stay high for 1600 ± 1200 polls (throttle: 400 to 2400), and a hook at the entry of `0x415DF4` keeps the matching calibration, type and mask in the variables above, switches `[0x4A3478]` to 1 when a device appears and back when none is left, and writes the rudder directly when an axis is assigned to it. TFX.CFG isn't changed.

### 9.23 Keys, views and the virtual cockpit
- **Key handlers.** `0x412ECC` takes the last key code (`[[0x4AB238]]`) and calls the handler in the table at `0x4A3030 + 4·scancode` (filled at start-up by code near `0x412F2C`); `[0x4A323C]` = a Shift key is down. The key state matrix is at `[0x4AB23C]`, one byte per scan code. Examples: Space `0x413504` (fire), Backspace `0x413FD4`, Enter `0x413AF4`, C `0x41510C`, F1–F10 `0x4148A4`, `0x4149F4`, `0x414A84`, `0x414B14`, `0x414B48`, `0x414C24`, `0x414D3C`, `0x414E04`, `0x414EE8`, `0x414D7C`.
- **Views.** The view number is `[0x4987B6]`. A handler asks for a view with `0x41F2E0` (EAX = view), which queues it (`0x4A3508`, up to 10). Once a frame `0x4216A0` dispatches on the view through the jump table at `0x42162C` (29 entries). Known: 9 = virtual cockpit (`0x420530`), 8 = `0x4204F8` (the same with the look direction reset every frame), 11h = chase view (`0x4203C4`), 0Ch/0Dh/17h/18h/1Ah = the cockpit's side and rear views.
- **Look keys.** Per frame `0x415850` builds `[0x4A358A]` from the keys held with Right Shift: bit 0 left, 1 right, 2 up, 3 down, 4 Home, 5 End, 6 Page Up, 7 Page Down (`[0x4A358B]`: the same with Ctrl). `0x41F41C` (cockpit views) and `0x41F4B4` (outside views) turn the view by 400h per frame for bits 0–3, whatever the frame took.
- **Matrices.** 3×3, nine words in 1.15 fixed point (7FFFh = 1), row by row. `0x45C220` = identity, `0x45BFE8` = copy (EAX = to, EDX = from), `0x45BED8` = multiply. `0x45C5B4` (EAX = matrix, DX = angle, 10000h = a full turn) rotates about the vertical axis: for each row, columns 0 and 2 become `m0·cos − m2·sin` and `m0·sin + m2·cos`; `0x45C4A8` does the same with columns 1 and 2 (pitch). Both take sine and cosine from the table at `0x456D34` (words, 1024 steps a turn, the cosine 256 entries on), so an angle counts in steps of 40h (0.35°). `0x45CAD0`, `0x45CB04` and `0x45CB20` read the pitch, yaw and roll angles back out of a matrix.
- **The virtual cockpit** (view 9, `0x420530`): the look direction is the matrix at `0x4987F0`, relative to the aircraft. Each frame: `0x41F41C` applies the look keys; `0x41F544` reads the angles back (`[0x4987B0]` pitch, `[0x4987B2]` yaw) and turns the view back by 200h a frame when one is beyond its limit, from the table at `0x4987CC` (pairs of pitch and yaw limit; the virtual cockpit uses the third pair, `0x4987D4` = 300h (4.2°) and `0x4987D6` = 1300h (26.7°)); `0x41FEB8` multiplies it with the aircraft's attitude, reads the three angles out and sets the camera with them (`0x448AC2`).
  - A positive angle turns the view right (yaw) and down (pitch): the arrow keys move the cockpit, not the head.
  - The cockpit and the aircraft around it are a complete 3D model: with the limits raised the view turns all the way round (wings, tail, side consoles). The HUD symbols stay fixed on the screen whatever the direction.
- **Divide errors are expected.** At start-up `0x44863A` installs a handler for processor exception 0 (int 31h fn 0203h, BL = 0; the old one is kept at `0x44786C`): `0x448708`. It looks at the faulting address:
  - `0x4489D3` / `0x4489E8`, the vertex projection's `idiv bx` for x and y (`(x >> 8) / z` into 16 bits; a vertex far to the side of a point just beyond the near plane overflows): continue at `0x4488DF`, which sets ESP back to the value saved at the model routine's entry (`[0x447860]`), cleans up and returns from it. The object isn't drawn.
  - `0x4491A9` / `0x4491D1`, the visibility test's two divides: continue at `0x4491DC` (carry set, not visible).
  - anywhere else: the game's fatal error (`0x44877A`).
  - The port has no processor exceptions for generated code: `tools/recomp.py` (`DIV_RECOVER`) makes those four divides jump to the same continuations. A hook at `0x4488DF` counts them (stats log).
- **The 3D view's background.** Each frame the view is first filled with one colour by the rectangle fill `0x458E34` (stack arguments: bottom, right, top, left and the colour); the sky and haze bands are drawn over it. Looking more than about 50° up or down the game draws no bands, only that fill.
- **The scan converters and the recompiler.** The flat-polygon scan converter `0x46AB50` steps both edges in an unrolled block: 200 units of 11 bytes at `0x46AC50`–`0x46B4E8` (`shld eax, ebx, cl / shld eax, edx, cl / push eax / add ebx, esi / add edx, edi`: one pair of x values pushed per line), entered for ECX lines by `imul eax, ecx, -0Bh / add eax, 46B4E8h / mov cl, 10h / jmp eax`. After the block a `jmp` whose rel32 is set by immediate stores (`0x46B4F5`) continues with one of three edge blocks; the textured and Gouraud variants (`0x46B4FC`, `0x46B770`) pick their edge blocks the same way (`0x46B76C`, `0x46B918`). The function returns by a `jmp` patched to the caller's continuation.
  - The recompiler follows both: `analyze.py` `_block_jump` recognises the computed jump and makes every unit a label; stored rel32 values of a near jump become targets (`find_smc`). Before that, the unrolled block and the edge blocks ran in the interpreter for every polygon (about 200 `interpreter: entering at` lines per session).
  - Code reached only through pointers has to be named in `recomp.py` `EXTRA_ENTRIES`: the textured span fill `0x475334` (a pixel loop with a plane select per pixel), the stippled fills `0x4726CE`, `0x4727FF`, `0x47292F`, `0x47415A`, `0x47424C`, and `0x4725C0`, `0x44C0A0`, `0x44AA15`. Interpreted, the textured fill took over half of the frame time when the cockpit fills the screen.
- **The port:** `runtime/track.c` (head tracking, a hook at `0x420530`) raises the two limits and writes the matrix from a FreeTrack pose; `runtime/gamefix.c` halves the look keys' rate on 1-tick frames.

### 9.10 Music: START.EXE's resident OSL driver
**START.EXE** (Borland C++, real mode, 36 KB) is more than a launcher.
- **Startup:** it reads TFX.CFG, sets `BLASTER`, `ULTRASND`, `DID=` and `DOS4GVM`, runs `gus.com` for the Gravis card, and spawns TFX.DAT (`TFX spawn error`).
- **Resident music system:** it contains OSL's music system (1993): the **"LMEDIT Track Driver"** (segment 0353h of the image) and three device drivers. These are **AdLib** (055Bh, id 2, port 388h), **Roland SCC-1** (0669h, id 8, MPU-401 at 330h) and **LAPC-1/MT-32** (06F2h, id 4, MPU-401 at 330h).
- **Tune lists:** for each device it lists about 20 tunes (intro, inter, news, enrol, death, five theatres, the mission types, `fxonly`, `engine`). The CD release ships only `TUNES\INTRO.ADL/.RLD/.SCC`; everything else became CD audio.

**Installation (START main):**
1. The settings live in START's segment 0549h (+14h SOUNDFX, +16h MUSIC). `main` applies "MUSIC = 1 if MUSIC = 0 and SOUNDFX = 1" and allocates a 9A28h-byte song buffer.
2. **`0549:0041` (install):**
   - It writes the IRQBASE vector (TFX.CFG `IRQBASE=135` = 87h) into every `int 60h` placeholder in the blobs.
   - It records the buffer segment.
   - It lists the device driver for MUSIC: **1 → AdLib**, 2 → LAPC-1, 4 → SCC-1. MUSIC=3 ("ADLIB" in CONFIG) gets no device.
   - It far-calls the track driver's entry at offset 0 with AX = vector, BX = FFFFh (so the driver's own timer hook at 0853h is skipped) and ES:DI = device list.
3. `1000:087F` calls `0549:0020` = **AH=15h: hook int 8** (handler 1E03h). It registers an `atexit` that unhooks it (16h).
4. `0549:002A` = **AH=17h: PIT to 5555h (54.6 Hz)**, chaining the BIOS tick every 137/50 interrupts.
5. Spawn TFX.DAT; afterwards AH=18h (PIT back) and AH=00h.

**Track driver API** (`int 87h`, AH ≤ 21h through the word table at CS:0724h; the target is stored into a `jmp` at CS:0693h):
- **Detection:** TFX finds the driver by reading the real-mode vector table directly and checking `DID!` in the 4 bytes before the handler (`0x4346D8`). It calls with DPMI 0300h and the register block at `0x4A6F34`.
- **Functions TFX uses:** 19h (address of the driver's 32-bit tick counter, CS:1DE0h), 1Bh (song buffer), 13h (BL = tune → name, from three tables picked by MUSIC), 14h/02h (play the "OSL1" song at ES:DI; returns the slot), 04h/06h/03h (per slot), 1Ch/1Dh (enable/disable the sequencer: device command 09h with CL = 7Fh/0), 07h, 1Fh, 20h, 21h and 17h.
- **Loading:** TFX loads (and unpacks: the files are `RA`-compressed) the tune into the buffer itself.

**Timer handler** (`1E03h`):
- `inc dword [1DE0h]` (the tick counter);
- if playing, `int 87h` AH=01h, the sequencer tick at 1026h;
- every 137/50 calls, chain to the old int 8.

**TFX uses the driver's counter as its game clock** (`0x435690` uses it when AH=19h returns non-zero, else 0:46Ch). The frame limiter's "2 ticks" is therefore 37 ms (27 fps), and its tick-to-seconds factor of 0.02 matches ~50 Hz. Without START's driver the game runs at 9 fps with game time about 2.7× too slow.

**"OSL1" song format** (tracker-like):
- **Header:** `OSL1`, the title at 28h, the sub-song count at 26h, dword offsets at 48h.
- **Sub-song:** title, speed, an order count at +4Eh and the order list at +50h, and a pattern pointer table at +150h.
- **Track headers:** 0AEh bytes each, with a GM-style name, the GM program at +30h, a drum flag at +2Fh, volume and transpose.
- **Patterns:** a word header (bit 15 = packed, bits 0–11 = rows) and packed cells holding up to four notes, an instrument and an effect with its parameter (32 effects).
- **Playback:** the per-tick sequencer runs 16 song slots of 39h bytes at CS:0037h. It calls the device driver through a far-pointer table in its header (+08h note on, +0Ch extra notes, +10h all off, +14h program, +20h volume, +30h extended effects).

**The port runs this original code** (`runtime/music.c`, `rm86.c`):
- START.EXE is loaded from the game folder (MZ relocations applied) and the steps above are run on a separate 8086/286 real-mode interpreter.
- TFX's DPMI 0300h calls to a real-mode-hooked vector run the real handler.
- IRQ0 goes to the hooked int 8, whose chain reaches the BIOS tick.
- **Devices:** an OPL2 synthesiser (`opl.c`) serves the AdLib driver. Its status reads count 1 µs each, so the driver's timer-based detection succeeds. An MPU-401 UART (`mpu.c`) feeds Windows MIDI for SCC-1/LAPC-1.
- A file-name detail: START's tables contain `"tunes\\intro.SCC "` with a trailing space, which DOS ignores.

### 9.9 Sound Blaster (digital effects)
- **Configuration:** TFX.CFG holds `SOUNDFX=n` (CONFIG: 0 off, 1 Sound Blaster, 2 GAPC1, 3 SB16, 4 GUS), its own `BLASTER=A220 I7 D1 H5 T6 F3` line, `ULTRASND=` and `IRQBASE=135`. TFX.DAT reads the `BLASTER`/`ULTRASND` settings (strings at file offset 0xF1658).
- **Detection:** DSP reset (226h = 1, then 0), poll 22Eh, read AAh from 22Ah. Then it probes an SB Pro-style mixer: it writes 26h, reads it back, and sets registers 22h/04h/26h/28h/2Eh to EEh. Last comes `D1h` (speaker on). There is no version query. On failure it prints "SB failed to respond, waiting to write 0x%02X for %s".
- **IRQ handler:** a small routine copied into conventional memory (linear 42570h, selector A0h for its data; "nomem for SB handler" if the allocation fails), installed with DPMI set-PM-vector for 8+IRQ. It acknowledges the DSP (reads 22Eh), sends EOI to port 20h, and works through a queue counter at offset 20h of its data, starting the next queued block if there is one.
- **Playback:** classic SB 2.0 style. The game sets the time constant (`40h`, giving 7518 or 8403 Hz), programs DMA channel 1 (mask, clear flip-flop, address, page 83h, count, mode 49h = single-cycle read), then sends `14h len-1` (8-bit single-cycle DMA). It often starts a short block, pauses (`D0h`) and restarts with the full sample. Every effect (radio calls, gun, weapon select, missile launch) is one or more single-cycle blocks ending in an IRQ. There is no continuous engine loop.

### 9.8 Game modes, input and frame rate (as observed)
- **Frame rate:** 9.1 fps in flight, exactly the game's own cap (the limiter `0x41B998` waits 2 BIOS ticks at 18.2 Hz). The recompiled code has large headroom: the limiter polls the tick count about 4 million times per frame. Screens without the limiter run faster (the arming screen at about 49 fps). These busy-wait loops keep one host core at 100%.
- **Views** F1–F10 all render: front, 60° left/right steps, rear, chase, fixed camera, tactical, missile, target and virtual cockpit, plus the full-screen map with the contacts panel (M).
- **Weapons:** Enter cycles the air-to-air weapons (gun `DGFT`, AIM-9, AIM-120 `AARM`); Space fires. Missiles fly with smoke trails, and F8 follows them.
- **Frame limiter detail:** `0x41B998` accumulates ticks from `0x403BB0` until `cmp dx, 2` (at `0x41B9B6`, immediate at `0x41B9B9`) is satisfied, clamps to 4 in mode 1 and to 18 overall, and sets the frame time step to ticks × 0.02 s (`0x4A42C0`). Lowering the minimum to 1 tick (the port's `framerate=high`, the default) doubles the frame rate without changing game speed: measured 54.6 fps × 0.02 s = 27.3 fps × 0.04 s = 1.09 game seconds per real second. The tick count is also left in `[0x4A3476]`, which about 140 instructions read (and the main loop passes it on to `0x404CE0`, `0x401530` and `[0x49AC80]`).
  - Most add it to or subtract it from timers, or multiply by it before dividing (`value × ticks / 50`, `× ticks >> 8`), so they run at the same speed with 1-tick frames, with slightly coarser rounding for small values.
  - **One breaks:** the chase view `0x4203C4` grows the camera distance `[0x4987B8]` (5 → 28h) by `(ticks << 4) / 30` a frame, which is 0 for 1 tick. Only from 28h on does it call `0x41F410`, which also handles the view keys. So with 1-tick frames the camera stayed on top of the aircraft and no view key worked after F5. The port adds the missing step (`runtime/gamefix.c`, a hook at the function's entry).
  - Others that round to 0 do so at 2 ticks as well (`ticks / 5` at `0x41D4A8`, `ticks >> 4` at `0x4335E9`), so they behave as in the original.
- **Busy-wait loops:** the frame limiter `0x41B998` calls `0x403BB0` (ticks elapsed since the previous call) until 2 ticks have passed. `0x403BB0` reads the tick through `0x435690`, which uses 0:46Ch unless a resident driver offers its own counter at `seg:1900h`. Menu screens wait for vertical retrace by polling 3DAh at 70 Hz, and some poll int 33h fn 3 in between.
- **Held keys:** throttle (`=`/`-` on the main keyboard, manual: "+"/"-") and the flight controls are read from the key-state matrix once per frame (about every 110 ms), so a press shorter than a frame is missed. View and toggle keys are latched. `[` / `]` start engines 1 and 2, W toggles the wheel brakes, G the gear.
- **Mouse, two styles:**
  - The menus integrate int 33h fn 0Bh mickeys into their own cursor.
  - The arming screen (and the other briefing-room screens) poll fn 03h directly. `0x45685B` returns x/2 (the driver's 0–639 virtual range), y and the buttons; `0x45689A` returns only the buttons and is used in wait-for-release loops.
  - The arming screen hit-tests `(x+5, y−8)` against rectangle tables at `0x497F44` and `0x497FC8` (x1, y1, x2, y2, handler; strict comparisons). The handler goes into the state pointer `0x4A32FC`. OK is the rectangle 196–208 × 188–194, so it is hit only at the very bottom rows of the screen.
- **Modes:**
  - **Arcade:** straight into an airborne mission.
  - **Training:** Miramar intro, an animated commander briefing with subtitles, Fighter Weapons School, the laptop arming screen (E.F.2000 rotating in 3D), then hop one against a MiG-21.
  - **Simulator:** world map → theatre → time of day/weather (dawn, day clear/cloud/storm, night clear/cloud) → arming → runway start: engines, throttle, brake release, takeoff and climb into cloud.
  - **Tour of Duty** and **Flash Points:** locked until training is complete (the game's own message).
  - **UN Commander:** "UN Central Command" → world map → the theatre mission planner (terrain map, mission menu, target panel).
  - Missing files `WAYTXT\UNCFILES.SOM` and `tfxstat.lbm` are tried and skipped: they are not in DID.DAT or on the CD, and UNCFILES is presumably created on save.

---

## 10. Open questions / TODO
- Confirm the installed version is 2.01 (apply the `tfxcd_*` patches to the CD `TFX.DAT` and compare).
- Identify the 16-bit object's role (real-mode callback stub vs. 16-bit interrupt handler).
- Catalogue every port with its access sites, and work out how the renderer uses VGA (page layout, resolution: likely 320×200 or 320×240/400 Mode X).
- `int 2Fh AX=00FAxx`: which TSR it talks to.
- ~~The `DID.DAT` archive format~~ (done, §9.2; the exact compression format could still be documented).
- The OSL music driver API between START.EXE and TFX.DAT.
- Self-modifying code: catalogued in §7.4, §9.5 and §9.7. Stores through computed addresses are covered by the runtime-observed list (`tools/smc/TFX.DAT.txt`), which only contains what the recorded sessions exercised. Other screens or game states may patch more code, so record more runs and merge them.
- Who writes the clip-rectangle immediates (which function computes the patch addresses)?
