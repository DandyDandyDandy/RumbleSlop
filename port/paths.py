r"""Where everything lives. Every script of the port takes its folders and tools from here, so nothing else names a
machine-specific path.

    input\      the user's own files: prod.keys, the base game (.xci or .nsp), the 2.2.0 update (.nsp)
    build\      everything made on the way (extracted game, generated C, objects, converted assets, downloaded tools)
    game\       the finished game; nothing in it points back into build\ or input\

Environment overrides (all optional):
    RUMBLESLOP_BUILD, RUMBLESLOP_INPUT, RUMBLESLOP_GAME   other folders
    LLVM_BIN        a folder with clang.exe, clang++.exe, ld.lld.exe, llvm-nm.exe (default: the downloaded llvm-mingw)
    NINJA           ninja.exe
    SPIRV_TOOLS_BIN a folder with spirv-val.exe / spirv-opt.exe (optional tools)
    UNITY_PLAYER    a folder with UnityPlayer.dll of the Windows IL2CPP player 2022.3.30f1 (non-development)
"""
import os
import shutil
from pathlib import Path

NAME = 'RumbleSlop'                     # the game's executable is <NAME>.exe, its data folder <NAME>_Data
UNITY_VERSION = '2022.3.30f1'
GAME_VERSION = '2.2.0'                  # the version of the Switch game the tables and hooks are made for
MAIN_BUILD_ID = '95548fabbca7a4f723c3468e5cfb34d0'      # first 16 bytes of main's build id, version 2.2.0
PORT_LABEL = 'RS 0.1'                 # the port's own version, after the game's on the title ("Ver. 2.20 RS 0.1").
                                        # Every release: agree on the release's number and change it here
                                        # (docs\TECHNICAL.md "The port's version"); the runtime takes it at build time.

PORT = Path(__file__).resolve().parent
ROOT = PORT.parent
THIRD_PARTY = ROOT / 'third_party'
LIFTER = THIRD_PARTY / 'xc2-recomp'     # unmodified upstream lifter modules
TABLES = PORT / 'recomp' / 'tables'     # address tables for GAME_VERSION (facts about the binary, no game code)

BUILD = Path(os.environ.get('RUMBLESLOP_BUILD') or ROOT / 'build')
INPUT = Path(os.environ.get('RUMBLESLOP_INPUT') or ROOT / 'input')
GAME = Path(os.environ.get('RUMBLESLOP_GAME') or ROOT / 'game')
DATA = GAME / f'{NAME}_Data'

DOWNLOADS = BUILD / 'downloads'
TOOLCHAIN = BUILD / 'toolchain'
WORK = BUILD / 'work'
EXTRACT = WORK / 'game'                 # exefs\ and romfs\ of the merged game (base + update)
EXEFS = EXTRACT / 'exefs'
ROMFS_DATA = EXTRACT / 'romfs' / 'Data'
RE = WORK / 're'                        # main.bin: the flat memory image of the Switch executable
MAIN_BIN = RE / 'main.bin'
UNITY = WORK / 'unity'                  # analysis outputs and converted asset trees
METADATA = ROMFS_DATA / 'Managed' / 'Metadata' / 'global-metadata.dat'
GENERATED = BUILD / 'generated'         # C lifted from the game executable
OBJ = BUILD / 'obj'                     # objects, GameAssembly.dll


def _tool_dir(env, default):
    v = os.environ.get(env)
    return Path(v) if v else default


LLVM_BIN = _tool_dir('LLVM_BIN', TOOLCHAIN / 'llvm' / 'bin')
SPIRV_TOOLS_BIN = _tool_dir('SPIRV_TOOLS_BIN', TOOLCHAIN / 'spirv-tools' / 'Library' / 'bin')
UNITY_PLAYER = _tool_dir('UNITY_PLAYER', TOOLCHAIN / 'unity' / 'Variations' / 'win64_player_nondevelopment_il2cpp')
XLATE = THIRD_PARTY / 'xlate' / 'xc2_xlate.exe'


def ninja():
    v = os.environ.get('NINJA')
    if v:
        return Path(v)
    p = TOOLCHAIN / 'ninja' / 'ninja.exe'
    return p if p.exists() else Path(shutil.which('ninja') or p)


def llvm(name):
    """Path of an llvm-mingw tool, e.g. llvm('clang')."""
    return LLVM_BIN / (name + '.exe')


def spirv_tool(name):
    """Path of spirv-val / spirv-opt, or None when SPIRV-Tools is not there (validation and optimisation are then
    skipped)."""
    p = SPIRV_TOOLS_BIN / (name + '.exe')
    if p.exists():
        return p
    w = shutil.which(name)
    return Path(w) if w else None


def tool_env():
    """os.environ with the toolchain's bin folder first on PATH."""
    return dict(os.environ, PATH=f'{LLVM_BIN}{os.pathsep}{os.environ.get("PATH", "")}')
