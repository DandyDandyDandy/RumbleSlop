"""Short file names for the asset bundles with the longest paths.

    python shorten.py apply   [<game folder>]
    python shorten.py restore [<game folder>]

The game's bundles keep the names the game's build gave them, and some are very long: the longest file of the game
folder was 225 characters below the folder itself. Windows tools that copy or zip a folder (Explorer among them) leave
out files whose full path is longer than 260 characters, without saying much, and a game folder that lost such a file
hangs on every screen that needs it (the first report: the tab buttons' animation, so Options and Missions never
opened). So setup ends with `apply`:

  * every bundle under <Name>_Data\\StreamingAssets\\aa\\Switch whose path below aa\\ is longer than LIMIT characters is
    moved to aa\\Switch\\s\\<16 hex digits>.bundle (the digits are a hash of the old path, so a name never changes);
  * the catalogs (aa\\catalog.json, and mania\\catalog.json of an import) name the new place: the engine opens a
    bundle by the path its catalog entry gives, nothing else knows the file's name;
  * a bundle that an import overrides (mania\\Switch\\<same path>) moves the same way, since the override is looked up
    by the path the engine is about to open;
  * aa\\short_names.json lists old path -> new path, which is all `restore` needs to undo it.

After it the longest path below the game folder is 150 characters. `restore` is for the tools that read the game's
bundles by their own names (mania\\build.py does restore, its work, apply). Both can be repeated at any time.
"""
import hashlib
import json
import os
import sys
from pathlib import Path

LIMIT = 110                      # longest path below aa\ that keeps its name
RT = '{UnityEngine.AddressableAssets.Addressables.RuntimePath}/'
MAP = 'short_names.json'


def _aa(game):
    game = Path(game)
    data = [p for p in game.iterdir() if p.is_dir() and p.name.endswith('_Data')]
    if len(data) != 1:
        raise SystemExit(f'shorten: no single <Name>_Data folder in {game}')
    return data[0] / 'StreamingAssets' / 'aa'


def short_name(rel):
    return 'Switch/s/' + hashlib.sha1(rel.encode('utf-8')).hexdigest()[:16] + '.bundle'


def _load_map(aa):
    try:
        return json.loads((aa / MAP).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def _move(src, dst):
    if not src.exists():
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    os.replace(src, dst)
    return True


def _catalogs(game, aa):
    return [p for p in (aa / 'catalog.json', Path(game) / 'mania' / 'catalog.json') if p.exists()]


def _rewrite(catalog, table):
    """Replace internal ids that are keys of `table`; returns how many."""
    c = json.loads(catalog.read_text(encoding='utf-8'))
    ids, n = c['m_InternalIds'], 0
    for i, s in enumerate(ids):
        if s.startswith(RT) and s[len(RT):] in table:
            ids[i] = RT + table[s[len(RT):]]
            n += 1
    if n:
        tmp = catalog.with_suffix('.json.tmp')
        tmp.write_text(json.dumps(c, separators=(',', ':'), ensure_ascii=False), encoding='utf-8')
        os.replace(tmp, catalog)             # also frees a hard link into the build tree
    return n


def _prune(root):
    for d, dirs, files in os.walk(root, topdown=False):
        if not dirs and not files and Path(d) != root:
            try:
                os.rmdir(d)
            except OSError:
                pass


def apply(game):
    aa = _aa(game)
    table = _load_map(aa)
    for d, _, files in os.walk(aa / 'Switch'):
        for f in files:
            rel = (Path(d) / f).relative_to(aa).as_posix()
            if len(rel) > LIMIT and not rel.startswith('Switch/s/'):
                table[rel] = short_name(rel)
    over = Path(game) / 'mania'
    moved = 0
    for rel, short in table.items():
        moved += _move(aa / rel, aa / short)
        _move(over / rel, over / short)
    (aa / MAP).write_text(json.dumps(table, indent=0, sort_keys=True), encoding='utf-8')
    named = sum(_rewrite(c, table) for c in _catalogs(game, aa))
    _prune(aa / 'Switch')
    missing = [short for short in table.values() if not (aa / short).exists()]
    if missing:
        raise SystemExit(f'shorten: {len(missing)} bundle(s) are in the list but not in the game folder, e.g. {missing[0]}')
    print(f'{len(table)} bundles have short names ({moved} moved now, {named} catalog entries changed)')
    return len(table)


def restore(game):
    aa = _aa(game)
    table = _load_map(aa)
    if not table:
        return 0
    back = {short: rel for rel, short in table.items()}
    over = Path(game) / 'mania'
    moved = 0
    for short, rel in back.items():
        moved += _move(aa / short, aa / rel)
        _move(over / short, over / rel)
    named = sum(_rewrite(c, back) for c in _catalogs(game, aa))
    (aa / MAP).unlink(missing_ok=True)
    _prune(aa / 'Switch')
    _prune(over / 'Switch') if (over / 'Switch').exists() else None
    print(f'{moved} bundles have their own names again ({named} catalog entries changed)')
    return moved


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('apply', 'restore'):
        sys.exit(__doc__)
    if len(sys.argv) > 2:
        game = sys.argv[2]
    else:
        sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
        import paths
        game = paths.GAME
    (apply if sys.argv[1] == 'apply' else restore)(game)


if __name__ == '__main__':
    main()
