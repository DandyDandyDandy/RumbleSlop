"""Build the game's executable (the launcher) with its icon and version information.

    python build_launcher.py [--copy]

Writes build/obj/<Name>.exe; --copy also puts it into the game folder (the game must not be running).
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
import paths  # noqa: E402
import make_icon  # noqa: E402


def run(*cmd):
    r = subprocess.run([str(c) for c in cmd], cwd=HERE, env=paths.tool_env(), capture_output=True, text=True)
    if r.returncode:
        sys.exit('build_launcher: ' + ' '.join(str(c) for c in cmd) + '\n' + r.stdout + r.stderr)


def main():
    paths.OBJ.mkdir(parents=True, exist_ok=True)
    res = paths.OBJ / 'launcher_res.o'
    exe = paths.OBJ / f'{paths.NAME}.exe'
    ver = paths.GAME_VERSION
    make_icon.make()                    # build/obj/icon.ico from the dump's title picture; -I below lets the .rc find it
    run(paths.llvm('x86_64-w64-mingw32-windres'), '-I', paths.OBJ, f'-DGAME_NAME=\\"{paths.NAME}\\"', f'-DGAME_VER=\\"{ver}\\"',
        '-DGAME_VER_NUM=' + ','.join((ver.split('.') + ['0', '0', '0'])[:4]), 'launcher.rc', '-O', 'coff', '-o', res)
    run(paths.llvm('clang'), '-O2', '-municode', '-mwindows', f'-DGAME_NAME_W=L"{paths.NAME}"', 'launcher.c', res, '-o', exe, '-static')
    print('built', exe)
    if '--copy' in sys.argv:
        shutil.copy2(exe, paths.GAME / exe.name)
        print('copied to', paths.GAME / exe.name)


if __name__ == '__main__':
    main()
