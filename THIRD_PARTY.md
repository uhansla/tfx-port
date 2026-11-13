# Third-party components

Nothing below is stored in this repository unless it says so. The build
(`build.bat`, `tools/setup.ps1`) downloads the tools from their publishers.

## Linked into the built program

| Component | Licence | Notes |
|---|---|---|
| [SDL 2.30.9](https://github.com/libsdl-org/SDL) | zlib | Linked statically. Copyright (C) 1997-2024 Sam Lantinga. |
| MinGW-w64 runtime, compiler-rt, libc++ (from [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)) | ZPL / public domain parts (MinGW-w64), Apache 2.0 with LLVM exceptions | Linked statically by the compiler. |

## Loaded at run time, only when fetched with `build.bat -Upscalers`

| Component | Licence | Notes |
|---|---|---|
| [AMD FidelityFX SDK 2.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) (FSR) | MIT | AMD's signed DLLs are copied next to the exe with `AMD_FidelityFX_LICENSE.md`. The build uses the SDK's API headers. |
| [NVIDIA Streamline SDK 2.14.16](https://github.com/NVIDIA-RTX/Streamline) | MIT (Streamline), NVIDIA RTX SDKs licence (the DLSS, DLSS Frame Generation and Reflex DLLs) | NVIDIA's signed DLLs are copied next to the exe with their licence files. They are not open source and must not be redistributed on their own; read `NVIDIA_DLSS_LICENSE.txt` before distributing anything that includes them. The build uses Streamline's headers. |

## Used to build

| Component | Licence |
|---|---|
| [llvm-mingw 20261020](https://github.com/mstorsjo/llvm-mingw) (clang, lld) | Apache 2.0 with LLVM exceptions; MinGW-w64 parts under their own terms |
| [Python 3.12](https://www.python.org/) (embeddable package) | PSF licence |
| [capstone 5.0.7](https://www.capstone-engine.org/) | BSD 3-clause |

## Algorithms implemented in this repository

| Where | After | Notes |
|---|---|---|
| `runtime/d3d12.c`, shader `xbr_ui`; `tools/uifilters.py` | xBR level 2 by Hyllian (Sergio G. D. B.), published under the MIT licence | A Direct3D 12 / Python implementation of the published filter, adapted to a masked UI layer. Copyright (C) 2011-2016 Hyllian for the original algorithm and reference shaders. |
| `runtime/d3d12.c`, shader `ps_mmpx`; `tools/uifilters.py` | MMPX by Morgan McGuire and Mara Gagiu ("MMPX Style-Preserving Pixel Art Magnification", 2021), reference code under the MIT licence | A Direct3D 12 / Python implementation of the published rules. Copyright 2020 Morgan McGuire and Mara Gagiu for the algorithm and reference implementation. |
| `tools/uifilters.py`, EPX / Scale2x | EPX (Eric Johnston), Scale2x (Andrea Mazzoleni) | The well-known 2x rule, written from its description. |

The MIT licence text for the two filters is the same as in [LICENSE](LICENSE) with the copyright lines above.

## Optional, not part of the build

- **Art packs** (`ui_filter=remacri`): made on your PC from your own copy of the game with `tools/artpack.ps1` and an upscaling model you supply (for example Real-ESRGAN with the Remacri model; check that model's own licence). An art pack is derived from the game's art and isn't for redistribution.
- **Ghidra** and the `ghidra-lx-loader` extension, for reverse engineering (`dev/ghidra/`): Apache 2.0.

## The game

TFX (1993) is not part of this repository and is not covered by any licence here. See the README.
