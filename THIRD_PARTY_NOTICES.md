# Third-party notices

## In this repository

| Component | Source | Licence | Where |
|---|---|---|---|
| XC2 Recomp lifter and code generator (`flags.py`, `gen.py`, `jumptables.py`, `layout.py`, `lifter.py`, `module.py`, `nso.py`, `patches.py`, `regs.py`, `simd_c.py`, `check_content.py`), unmodified | https://github.com/AksharDP/xc2-recomp @ `e9885513a2739fc68e3c5f341f192aa71f9926ce`. Copyright (c) 2026 Valiantsin Dzerakh; parts Copyright (c) 2026 Peiikai (LAS Recomp, https://github.com/peiikai/LASRecomp @ `da35b83`) | MIT (`third_party/xc2-recomp/LICENSE`) | `third_party/xc2-recomp/` |
| Code generator front end derived from XC2 Recomp `recomp/gen.py`; build flags from its `tools/build_game.py` | as above | MIT | `port/recomp/gen_il2cpp.py`, `port/build.py` |
| Guest CPU state and SIMD helper headers, adapted from LAS Recomp through XC2 Recomp | as above | MIT (`port/runtime/include/LICENSE.xc2-recomp`) | `port/runtime/include/cpu.h`, `simd.h`, `simd_fp.h` |
| `xc2_xlate.exe`: the shader translator of XC2 Recomp release 0.1.12, a NativeAOT build of the Ryujinx shader translator 1.3.2 (`Ryujinx.Graphics.Shader`, `Spv.Generator`, `Ryujinx.Common`) with MacNativeGames' optimizer patches and translator tool | https://github.com/AksharDP/xc2-recomp/releases/tag/v0.1.12 (built by its `tools/build_xlate.py`); https://github.com/Ryujinx/Ryujinx, Copyright (c) Ryujinx Team and Contributors; https://github.com/ly05010419/MacNativeGames, Copyright (c) 2026 MacNativeGames contributors, tool Copyright (c) 2026 jhaizhou | MIT (texts in `third_party/xlate/`) | `third_party/xlate/` (binary, unmodified) |
| SMOL-V (reference implementation, kept for comparison with the port's own Python implementation) | https://github.com/aras-p/smol-v, Copyright (c) 2016-2018 Aras Pranckevicius | MIT or public domain (`port/shaders/third_party/smolv/LICENSE.txt`) | `port/shaders/third_party/smolv/` |
| Material Icons "content copy" (recoloured) | https://github.com/google/material-design-icons | Apache-2.0 | `port/runtime/icons/content_copy_white.*` |
| Kenney "Input Prompts" 1.5, restyled into the button glyph atlas | https://kenney.nl/assets/input-prompts | CC0 | `port/runtime/icons/glyphs.png` |

The file formats read by `setup/nx.py` (XCI, NSP, NCA, PFS0, HFS0, RomFS, BKTR) are implemented from the public
documentation at https://switchbrew.org; the layout of the patch tables follows hactool
(https://github.com/SciresM/hactool, ISC licence), which was also the reference the extractor was checked against.
No hactool code is included.

## Downloaded by the setup (not in this repository)

`setup/fetch.py` downloads these to `build\` from their publishers, checks a pinned SHA-256 and unpacks them. They
are used to build the game on the user's PC; only Unity's player files end up in the `game` folder.

| Component | Source | Licence |
|---|---|---|
| llvm-mingw 20261104 (clang, lld, mingw-w64 runtime; statically linked parts end up in `GameAssembly.dll` and the launcher) | https://github.com/mstorsjo/llvm-mingw | Apache-2.0 with LLVM exception (LLVM), ZPL / various permissive (mingw-w64), see the package's `LICENSE.txt` |
| Ninja 1.13.2 | https://github.com/ninja-build/ninja | Apache-2.0 |
| SPIRV-Tools 2026.4 (conda-forge build), optional | https://github.com/KhronosGroup/SPIRV-Tools | Apache-2.0 |
| Unity 2022.3.30f1 Windows IL2CPP player (`UnityPlayer.dll`, `baselib.dll`, crash handler, default resources), taken from Unity's "Windows Build Support (IL2CPP)" installer | https://unity.com (download server `download.unity3d.com`) | Unity's terms; proprietary. The files are downloaded by each user from Unity and are not redistributed here. |
| Python packages: UnityPy, numpy, Pillow, etcpak, texture2ddecoder, lz4, pycryptodome, zstandard (and their dependencies) | PyPI, pinned in `setup/requirements.txt` | MIT / BSD / Apache-2.0 / HPND and others, see each package |

## The game

Super Monkey Ball Banana Rumble is (c) SEGA. This repository contains none of it: no code, no assets, no keys.
The address tables in `port/recomp/tables/` and the function offsets in the sources say where things are in the
game's 2.2.0 executable; the executable itself comes from the user's own copy at setup time.
