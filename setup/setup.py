"""One-time setup: build the PC game from your own Switch dump.

    setup.cmd                 run every step that is not done yet (safe to run again after an error)
    setup.cmd --redo STEP     run STEP and everything after it again
    setup.cmd --only STEP     run just that step
    setup.cmd --list          show the steps and which are done
    setup.cmd --clean         delete build\\ (the finished game in game\\ is kept)

Put these into input\\ first (see README.md):
    prod.keys            your console's keys
    the base game        .xci (cartridge dump) or .nsp
    the 2.2.0 update     .nsp
    add-on content       .nsp files, optional (added later: setup.cmd --only dlc, needs only prod.keys and them)

Steps: tools, extract, image, generate, compile, link, assets, shaders, textures, assemble, dlc.
Logs of each step are in build\\logs\\.
"""
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'port'))
sys.path.insert(0, str(HERE))
import paths  # noqa: E402

PY = sys.executable
JOBS = max(1, os.cpu_count() or 4)
TITLE_ID = 0x010031F019294000
AOC_BASE = TITLE_ID + 0x1000
STATE = paths.BUILD / 'state.json'
LOGS = paths.BUILD / 'logs'


class Fail(Exception):
    pass


def say(msg=''):
    print(msg, flush=True)


def run(step, *cmd, cwd=None, env=None):
    """Run a tool of the build; its output goes to build\\logs\\<step>.log, the last lines are shown on failure.
    Lines a tool starts with "progress:" are also shown here (the long steps report how far they are)."""
    LOGS.mkdir(parents=True, exist_ok=True)
    log = LOGS / f'{step}.log'
    with open(log, 'a', encoding='utf-8', errors='replace') as f:
        f.write(f'\n$ {" ".join(str(c) for c in cmd)}\n')
        f.flush()
        p = subprocess.Popen([str(c) for c in cmd], cwd=cwd, env=env or paths.tool_env(), stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace', bufsize=1)
        for line in p.stdout:
            f.write(line)
            f.flush()
            if line.startswith('progress:'):
                say('    ' + line[len('progress:'):].strip())
        code = p.wait()
    if code:
        tail = log.read_text(encoding='utf-8', errors='replace').splitlines()[-25:]
        raise Fail(f'step "{step}" failed (exit code {code}). Last lines of {log}:\n    ' + '\n    '.join(tail))


# ------------------------------------------------------------------------------------------------------- inputs

def find_inputs(need_game=True):
    """(keys path, [Container of the game], [Container of each add-on content package], (base, patch)).
    Explains what is missing. need_game=False accepts input\\ without the game (installing add-on content later)."""
    import nx
    paths.INPUT.mkdir(parents=True, exist_ok=True)
    files = sorted(p for p in paths.INPUT.iterdir() if p.is_file())
    keys = [p for p in files if p.name.lower() == 'prod.keys'] or [p for p in files if p.suffix.lower() == '.keys']
    if not keys:
        raise Fail(f'prod.keys is not in {paths.INPUT}.\nIt is the key file of your own console (dumped with Lockpick_RCM).')
    try:
        k = nx.Keys(str(keys[0]))
    except nx.NxError as e:
        raise Fail(str(e)) from None
    game, dlc, problems = [], [], []
    for p in files:
        if p.suffix.lower() in ('.nsz', '.xcz'):
            problems.append(f'{p.name}: compressed dumps are not supported; use the plain .nsp / .xci')
            continue
        if p.suffix.lower() not in ('.xci', '.nsp'):
            continue
        try:
            c = nx.Container(str(p), k)
        except nx.NxError as e:
            problems.append(str(e))
            continue
        ids = {n.title_id for n in c.ncas}
        if any((i & ~0xfff) == TITLE_ID for i in ids):
            game.append(c)
        elif any(0 < i - AOC_BASE < 0x1000 for i in ids):
            dlc.append(c)
        else:
            problems.append(f'{p.name}: not Super Monkey Ball Banana Rumble (title {min(ids):016x})' if ids else f'{p.name}: empty')
    base, patch = nx.find_program(game, TITLE_ID) if game else (None, None)
    if need_game and (base is None or patch is None):
        have = 'only the update' if patch else ('only the base game' if base else 'neither')
        hint = ''
        if base and not patch:
            hint = '\nThe port is made for version 2.2.0 of the game: add the 2.2.0 update (.nsp).'
        raise Fail(f'{paths.INPUT} needs the base game (.xci or .nsp) and the 2.2.0 update (.nsp); found {have}.{hint}'
                   + ''.join('\n  ' + x for x in problems))
    for x in problems:
        say('  note: ' + x)
    return keys[0], game, dlc, (base, patch)


def file_stamp(files):
    """Name, size and time of files, to notice when input\\ changes (as lists: the form it has in state.json)."""
    return sorted([Path(p).name, Path(p).stat().st_size, int(Path(p).stat().st_mtime)] for p in files)


# -------------------------------------------------------------------------------------------------------- steps

def step_tools():
    import fetch
    for name in fetch.ITEMS:
        fetch.fetch(name)
    if not paths.XLATE.exists():
        raise Fail(f'{paths.XLATE} is missing (it is part of this repository)')


def step_extract():
    import nx
    _, _, _, (base, patch) = find_inputs()
    if paths.EXTRACT.exists():
        shutil.rmtree(paths.EXTRACT)
    last = [-1]

    def progress(done, total, path):
        pct = done * 100 // total
        if pct // 10 != last[0]:
            last[0] = pct // 10
            say(f'    {pct}%')
    try:
        n = nx.extract_program(base, patch, str(paths.EXTRACT), progress)
    except nx.NxError as e:
        raise Fail(str(e)) from None
    main = (paths.EXEFS / 'main').read_bytes()
    build_id = main[0x40:0x50].hex()
    if main[:4] != b'NSO0' or build_id != paths.MAIN_BUILD_ID:
        raise Fail(f'This is not version {paths.GAME_VERSION} of the game (executable build id {build_id}, expected '
                   f'{paths.MAIN_BUILD_ID}).\nThe port only supports the game with the {paths.GAME_VERSION} update: put that update '
                   f'(.nsp) into {paths.INPUT}.')
    say(f'    {n} files, version {paths.GAME_VERSION} confirmed')


def step_image():
    paths.RE.mkdir(parents=True, exist_ok=True)
    run('image', PY, paths.ROOT / 'tools' / 'nso.py', 'dump', paths.EXEFS / 'main', paths.MAIN_BIN)


def step_generate():
    run('generate', PY, paths.PORT / 'recomp' / 'gen_il2cpp.py', '--jobs', JOBS)


def step_compile():
    run('compile', PY, paths.PORT / 'build.py', '--jobs', JOBS)


def step_link():
    run('link', PY, paths.PORT / 'runtime' / 'build_runtime.py', '--no-copy')
    if not (paths.OBJ / 'GameAssembly.dll').exists():
        raise Fail('GameAssembly.dll was not linked; see build\\logs\\link.log')
    run('link', PY, paths.PORT / 'launcher' / 'build_launcher.py')      # the exe, with icon and version information


def step_assets():
    aa = paths.ROMFS_DATA / 'StreamingAssets' / 'aa'
    dec = paths.UNITY / 'decrypted_bundles'
    win = paths.UNITY / 'aa_win'
    run('assets', PY, paths.ROOT / 'tools' / 'decrypt_bundle.py', '--all', aa / 'Switch', dec)
    run('assets', PY, paths.PORT / 'assets' / 'retarget_all.py', aa, dec, win, '--jobs', JOBS)
    run('assets', PY, paths.PORT / 'assets' / 'patch_catalog.py', win / 'catalog.json')


def step_shaders():
    run('shaders', PY, paths.PORT / 'shaders' / 'batch.py', 'all', '--jobs', JOBS)


def step_textures():
    cache = paths.UNITY / 'tex_cache'
    run('textures', PY, paths.PORT / 'assets' / 'tex_convert.py', 'tree', paths.UNITY / 'aa_vk', paths.UNITY / 'aa_pc',
        '--cache', cache, '--jobs', JOBS)
    (paths.UNITY / 'data_pc').mkdir(parents=True, exist_ok=True)
    run('textures', PY, paths.PORT / 'assets' / 'tex_convert.py', 'file', paths.UNITY / 'data_vk' / 'data.unity3d',
        paths.UNITY / 'data_pc' / 'data.unity3d', '--cache', cache)


def step_assemble():
    run('assemble', PY, HERE / 'assemble.py')


def step_dlc():
    import nx
    _, _, dlc, _ = find_inputs(need_game=False)
    if not dlc:
        say('    no add-on content in input\\ (optional)')
        return
    n = 0
    for c in dlc:
        for nca in c.ncas:
            index = nca.title_id - AOC_BASE
            if nca.content_type not in ('PublicData', 'Data') or not 0 < index < 0x1000:
                continue
            dst = paths.GAME / 'dlc' / str(index)
            dst.mkdir(parents=True, exist_ok=True)
            sec = nca.section(0)
            try:
                files = nx.extract_romfs(sec.content(), str(dst)) if sec else 0
            except nx.NxError as e:
                raise Fail(f'{os.path.basename(c.path)}: {e}') from None
            say(f'    add-on content {index}: {os.path.basename(c.path)} ({files} file(s))')
            n += 1
    say(f'    {n} pack(s) installed')


def step_shorten():
    # always the last step, and done again on every run: the steps before it put bundles back under their
    # own names (port/assets/shorten.py says why the names are shortened)
    run('shorten', PY, paths.PORT / 'assets' / 'shorten.py', 'apply', paths.GAME)


STEPS = [
    ('tools', 'Getting the build tools (compiler, Unity player; about 300 MB of downloads)', step_tools),
    ('extract', 'Reading your game dump', step_extract),
    ('image', 'Unpacking the game executable', step_image),
    ('generate', 'Translating the game code to C', step_generate),
    ('compile', 'Compiling the game code (a few minutes)', step_compile),
    ('link', 'Linking', step_link),
    ('assets', 'Converting asset bundles for Windows', step_assets),
    ('shaders', 'Converting shaders to Vulkan (the longest step, 15 minutes or more)', step_shaders),
    ('textures', 'Converting textures', step_textures),
    ('assemble', 'Putting the game folder together', step_assemble),
    ('dlc', 'Add-on content', step_dlc),
    ('shorten', 'Shortening long file names', step_shorten),
]
NAMES = [s[0] for s in STEPS]


def load_state():
    try:
        return json.loads(STATE.read_text())
    except (OSError, ValueError):
        return {'done': []}


def save_state(st):
    paths.BUILD.mkdir(parents=True, exist_ok=True)
    STATE.write_text(json.dumps(st, indent=1))


def main():
    args = sys.argv[1:]
    if '-h' in args or '--help' in args:
        say(__doc__)
        return 0
    if '--clean' in args:
        if paths.BUILD.exists():
            shutil.rmtree(paths.BUILD, ignore_errors=True)
        say(f'deleted {paths.BUILD}')
        return 0
    st = load_state()
    if '--list' in args:
        for name, title, _ in STEPS:
            say(f'  [{"x" if name in st["done"] else " "}] {name:9s} {title}')
        return 0
    todo = NAMES
    for flag in ('--redo', '--only'):
        if flag in args:
            name = args[args.index(flag) + 1] if args.index(flag) + 1 < len(args) else ''
            if name not in NAMES:
                say(f'unknown step {name!r}; steps: {" ".join(NAMES)}')
                return 2
            i = NAMES.index(name)
            st['done'] = [d for d in st['done'] if d not in (NAMES[i:] if flag == '--redo' else [name])]
            todo = NAMES[i:] if flag == '--redo' else [name]
    st['done'] = [d for d in st['done'] if d != 'shorten']
    if st.get('port') != paths.PORT_LABEL and 'link' in st['done'] and todo == NAMES:
        # a newer version of the port over a finished build: its own code is compiled and linked again and the
        # game folder put together again (a minute); the converted game data stays
        say(f'Updating the game to {paths.PORT_LABEL} ...')
        st['done'] = [d for d in st['done'] if d not in ('link', 'assemble')]
    st['port'] = paths.PORT_LABEL
    if 'shorten' not in todo:
        todo = todo + ['shorten']
    say(f'{paths.NAME} setup: building the game in {paths.GAME}')
    t_all = time.time()
    try:
        say('Checking input\\ ...')
        only_dlc = [t for t in todo if t != 'shorten'] == ['dlc']
        emptied = 'extract' in st['done'] and not (paths.INPUT.exists() and any(paths.INPUT.glob('*.keys')))
        if emptied:
            # a finished build whose input\ was cleared (it is not needed any more): an update of the port needs no dump
            say('  nothing in input\\: the finished build is used as it is')
        else:
            keys, game, dlc, _ = find_inputs(need_game=not only_dlc and 'extract' not in st['done'])
            say(f'  keys: {keys.name}; game files: {", ".join(os.path.basename(c.path) for c in game) or "none"}'
                + (f'; add-on content: {len(dlc)} file(s)' if dlc else ''))
            if not only_dlc and game:
                stamp = file_stamp([keys] + [c.path for c in game])
                if st.get('inputs') not in (None, stamp) and 'extract' in st['done']:
                    say('The game files in input\\ changed: starting again from the game dump.')
                    st['done'] = [d for d in st['done'] if d == 'tools']
                st['inputs'] = stamp
            dlc_stamp = file_stamp(c.path for c in dlc)
            if st.get('dlc_inputs', []) != dlc_stamp:
                st['done'] = [d for d in st['done'] if d != 'dlc']     # add-on content added: install it, nothing else
            st['dlc_inputs'] = dlc_stamp
        free = shutil.disk_usage(paths.ROOT).free >> 30
        if free < 15 and 'textures' not in st['done']:
            say(f'  warning: only {free} GB free on this drive; the build needs about 15 GB')
        for i, (name, title, fn) in enumerate(STEPS, 1):
            if name not in todo:
                continue
            if name in st['done']:
                say(f'[{i}/{len(STEPS)}] {title}: already done')
                continue
            say(f'[{i}/{len(STEPS)}] {title} ...')
            t = time.time()
            fn()
            st['done'].append(name)
            save_state(st)
            say(f'    done in {int(time.time() - t)} s')
    except Fail as e:
        save_state(st)
        say('\nSETUP STOPPED: ' + str(e))
        say('\nFix the problem and run setup again; finished steps are not repeated.')
        return 1
    except KeyboardInterrupt:
        save_state(st)
        say('\ninterrupted; run setup again to continue')
        return 1
    if todo == NAMES:
        say(f'\nFinished in {int(time.time() - t_all) // 60} min. The game is in {paths.GAME}: start {paths.NAME}.exe.')
        say('The build\\ folder and the files in input\\ are not needed any more (setup.cmd --clean deletes build\\).')
    return 0


if __name__ == '__main__':
    sys.exit(main())
