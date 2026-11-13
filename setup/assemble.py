"""Put the finished game together in game\\ from the build outputs. Nothing in game\\ points back into build\\ or
input\\ afterwards, so both can be deleted.

    python assemble.py

    game\\<Name>.exe                 port\\launcher (starts UnityPlayer.dll with -force-vulkan)
    game\\UnityPlayer.dll ...        Unity's Windows IL2CPP player, exact engine version
    game\\GameAssembly.dll           the recompiled game code + host runtime
    game\\<Name>_Data\\              player data: converted data.unity3d, il2cpp metadata, the flat image of the Switch
                                    executable (main.bin, loaded at run time), StreamingAssets (converted bundles,
                                    movies, sound)
Large files are hard-linked when build\\ and game\\ are on the same drive (no extra disk space, and deleting build\\
later does not touch them), copied otherwise.
"""
import os
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'port'))
import paths  # noqa: E402


def place(src, dst, link=True):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        try:
            if os.path.samefile(src, dst):
                return
        except OSError:
            pass
        dst.unlink()
    if link:
        try:
            os.link(src, dst)
            return
        except OSError:
            pass
    shutil.copy2(src, dst)


def place_tree(src, dst, skip=()):
    n = 0
    for root, _, files in os.walk(src):
        rel = Path(root).relative_to(src)
        for f in files:
            if f in skip:
                continue
            place(Path(root) / f, dst / rel / f)
            n += 1
    # files that are no longer in the source (an older build's leftovers) would confuse the game's catalog
    for root, _, files in os.walk(dst):
        rel = Path(root).relative_to(dst)
        for f in files:
            if not (src / rel / f).exists():
                (Path(root) / f).unlink()
    return n


def boot_config(src, dst):
    """The Switch build turns native graphics jobs on. With them the Windows Vulkan player crashes now and then at
    scene changes (see docs/TECHNICAL.md, "Environment traps"); plain threaded rendering instead."""
    out = []
    for line in src.read_text(encoding='ascii', errors='replace').splitlines():
        if line.startswith(('gfx-enable-gfx-jobs=', 'gfx-enable-native-gfx-jobs=')):
            line = line.split('=')[0] + '=0'
        out.append(line)
    dst.write_text('\n'.join(out) + '\n', encoding='ascii')


def main():
    game, data, rom = paths.GAME, paths.DATA, paths.ROMFS_DATA
    need = {
        'the recompiled game (GameAssembly.dll)': paths.OBJ / 'GameAssembly.dll',
        'the launcher': paths.OBJ / f'{paths.NAME}.exe',
        'the Unity player': paths.UNITY_PLAYER / 'UnityPlayer.dll',
        'the converted player data': paths.UNITY / 'data_pc' / 'data.unity3d',
        'the converted asset bundles': paths.UNITY / 'aa_pc' / 'catalog.json',
        'the flat image of the game executable': paths.MAIN_BIN,
    }
    missing = [k for k, p in need.items() if not p.exists()]
    if missing:
        sys.exit('assemble: not built yet: ' + ', '.join(missing))
    data.mkdir(parents=True, exist_ok=True)

    place(paths.OBJ / f'{paths.NAME}.exe', game / f'{paths.NAME}.exe', link=False)
    place(paths.OBJ / 'GameAssembly.dll', game / 'GameAssembly.dll', link=False)
    for f in paths.UNITY_PLAYER.iterdir():
        if f.suffix.lower() == '.dll' or f.name == 'UnityCrashHandler64.exe':
            place(f, game / f.name, link=False)
    place(paths.UNITY_PLAYER / 'Data' / 'Resources' / 'unity default resources', data / 'Resources' / 'unity default resources',
          link=False)

    for f in ('RuntimeInitializeOnLoads.json', 'ScriptingAssemblies.json'):
        place(rom / f, data / f, link=False)
    boot_config(rom / 'boot.config', data / 'boot.config')
    (data / 'app.info').write_text(f'{paths.NAME}\n{paths.NAME}', encoding='ascii')
    place(paths.UNITY / 'data_pc' / 'data.unity3d', data / 'data.unity3d', link=False)
    place(paths.METADATA, data / 'il2cpp_data' / 'Metadata' / 'global-metadata.dat', link=False)
    place(rom / 'Managed' / 'Resources' / 'mscorlib.dll-resources.dat', data / 'il2cpp_data' / 'Resources' / 'mscorlib.dll-resources.dat',
          link=False)
    place(paths.MAIN_BIN, data / 'il2cpp_data' / 'main.bin', link=False)
    for f in ('methods.tsv', 'generic_methods.tsv'):          # optional symbol tables for backtraces in port.log
        if (paths.UNITY / f).exists():
            place(paths.UNITY / f, data / 'il2cpp_data' / f, link=False)

    sa = data / 'StreamingAssets'
    n = place_tree(paths.UNITY / 'aa_pc', sa / 'aa', skip=('retarget_report.tsv',))
    for sub in sorted(p for p in (rom / 'StreamingAssets').iterdir() if p.is_dir() and p.name != 'aa'):
        n += place_tree(sub, sa / sub.name)
    sys.path.insert(0, str(paths.PORT / 'assets'))
    import delisted
    if delisted.patch(str(sa / 'aa' / 'Switch')):
        print('characters that can no longer be bought or earned: their unlock rules are switched off (the port owns them)')
    print(f'game folder ready: {game} ({n} asset files)')


if __name__ == '__main__':
    main()
