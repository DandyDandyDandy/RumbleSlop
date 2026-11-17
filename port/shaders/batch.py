"""Full conversion pipeline: every Switch-only Shader object in the retargeted asset tree -> Vulkan.

    python batch.py all    [--jobs 8]      scan + convert + write + data   (the normal run)
    python batch.py scan   [--jobs 8]      find the bundles that hold Shader / ComputeShader objects -> state/scan.json
    python batch.py convert [--jobs 8]     convert every distinct Shader object (results cached under state/)
    python batch.py write  [--jobs 8]      build the output tree aa_vk (converted bundles written, the rest hard-linked)
    python batch.py data                   build data_vk/data.unity3d from the original player data
    python batch.py verify [--jobs 8]      re-parse every Shader of the output, spirv-val every unique module

All under build/work (port/paths.py):
Inputs  (read only):  unity/aa_win (bundles already retargeted to build target 19), game/romfs/Data/data.unity3d
Outputs:              unity/aa_vk (same layout as aa_win), unity/data_vk/data.unity3d
State / caches:       unity/shader_vk_state/
    scan.json                 bundles with shaders, per shader object its content hash
    units/xx/<sha1>           converted subprogram blob per (Switch subprogram blob, bindings)   [program-hash cache]
    shaders/<sha1>.pkl        converted Shader fields per distinct Shader object content
    verify.json, *.log        reports

A Shader that cannot be converted is neutralised (no subshaders, empty Vulkan blob) so that no Switch program is
left anywhere: the Vulkan player crashes in ShaderLab::SubProgram::Compile on Switch programs.
"""
import argparse
import collections
import hashlib
import json
import os
import pickle
import shutil
import struct
import subprocess
import sys
import threading
import time
import traceback
from concurrent.futures import ProcessPoolExecutor, as_completed

import lz4.block
import UnityPy
from UnityPy.enums import BuildTarget
from UnityPy.files import SerializedFile, BundleFile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import paths  # noqa: E402

ROOT = str(paths.UNITY)
SRC = os.path.join(ROOT, 'aa_win')
DST = os.path.join(ROOT, 'aa_vk')
DATA_SRC = str(paths.ROMFS_DATA / 'data.unity3d')
DATA_DST = os.path.join(ROOT, 'data_vk', 'data.unity3d')
STATE = os.path.join(ROOT, 'shader_vk_state')
UNITS = os.path.join(STATE, 'units')
SHADERS = os.path.join(STATE, 'shaders')
TMP = os.path.join(STATE, 'tmp')
# Files in UNITS after a full conversion of game version 2.2.0 (every build ends with this many): the measure of
# the progress line, which the setup shows (lines starting with "progress:").
EXPECTED_UNITS = 34535
FIELDS = ('platforms', 'offsets', 'compressedLengths', 'decompressedLengths', 'compressedBlob', 'stageCounts', 'm_ParsedForm')


def shader_hash(tt):
    h = hashlib.sha1(bytes(tt['compressedBlob']))
    h.update(json.dumps([tt['m_ParsedForm'], tt['platforms'], tt['offsets'], tt['decompressedLengths']], sort_keys=True, default=repr).encode())
    return h.hexdigest()


def all_bundles():
    out = []
    for r, _, fs in os.walk(SRC):
        out += [os.path.join(r, f) for f in fs]
    return sorted(out)


# ---------------------------------------------------------------- scan

def scan_one(path):
    rec = dict(path=path, shaders=[], compute=[], error=None)
    if not path.endswith('.bundle'):
        return rec
    try:
        env = UnityPy.load(path)
        for obj in env.objects:
            tn = obj.type.name
            if tn == 'Shader':
                tt = obj.read_typetree()
                nvar = sum(len(l) for ss in tt['m_ParsedForm']['m_SubShaders'] for ps in ss['m_Passes']
                           for l in ps['progVertex']['m_PlayerSubPrograms'])
                rec['shaders'].append(dict(path_id=obj.path_id, name=tt['m_ParsedForm']['m_Name'], hash=shader_hash(tt),
                                           platforms=tt['platforms'], variants=nvar,
                                           blob=sum(sum(x) if isinstance(x, list) else x for x in tt['decompressedLengths'])))
            elif tn == 'ComputeShader':
                try:
                    rec['compute'].append(obj.read_typetree().get('m_Name', '?'))
                except Exception:
                    rec['compute'].append('?')
    except Exception as e:
        rec['error'] = repr(e)[:300]
    return rec


def cmd_scan(a):
    files = all_bundles()
    t0 = time.time()
    recs = []
    with ProcessPoolExecutor(a.jobs) as ex:
        for i, r in enumerate(ex.map(scan_one, files, chunksize=32)):
            recs.append(r)
            if i % 2000 == 0:
                print(f'scan {i}/{len(files)}', flush=True)
    keep = [r for r in recs if r['shaders'] or r['compute'] or r['error']]
    json.dump(dict(files=len(files), records=keep), open(os.path.join(STATE, 'scan.json'), 'w'), indent=0)
    objs = [s for r in keep for s in r['shaders']]
    print(f'scanned {len(files)} files in {time.time() - t0:.0f}s: {sum(1 for r in keep if r["shaders"])} bundles hold '
          f'{len(objs)} Shader objects ({len({s["name"] for s in objs})} names, {len({s["hash"] for s in objs})} distinct contents, '
          f'{sum(s["variants"] for s in objs)} variant references); '
          f'{sum(len(r["compute"]) for r in keep)} ComputeShader objects in {sum(1 for r in keep if r["compute"])} bundles; '
          f'{sum(1 for r in keep if r["error"])} load errors')
    for r in keep:
        if r['error']:
            print('  load error', r['path'], r['error'])
        if r['compute']:
            print('  ComputeShader', r['path'], r['compute'])


# ---------------------------------------------------------------- convert

def neutral_fields(tt, reason):
    """no subshaders, an empty Vulkan blob: nothing of the Switch programs is left"""
    table = struct.pack('<I', 0)
    comp = lz4.block.compress(table, mode='high_compression', store_size=False)
    pf = tt['m_ParsedForm']
    pf['m_SubShaders'] = []
    return dict(platforms=[18], offsets=[[0]], compressedLengths=[[len(comp)]], decompressedLengths=[[len(table)]],
                compressedBlob=comp, stageCounts=[1], m_ParsedForm=pf, _neutralized=reason)


def convert_one(task):
    path, path_id, h = task
    import convert
    out = os.path.join(SHADERS, h + '.pkl')
    if os.path.exists(out):
        with open(out, 'rb') as fh:
            r = pickle.load(fh)
        return dict(hash=h, name=r['_name'], cached=True, stats=r['_stats'], neutralized=r.get('_neutralized'), seconds=0)
    t0 = time.time()
    env = UnityPy.load(path)
    obj = next(o for o in env.objects if o.type.name == 'Shader' and o.path_id == path_id)
    tt = obj.read_typetree()
    name = tt['m_ParsedForm']['m_Name']
    work = os.path.join(TMP, h)
    stats = {}
    try:
        if tt['platforms'] != [19]:
            raise ValueError(f"platforms {tt['platforms']}")
        stats = dict(convert.convert_tt(tt, work, log=lambda *x: None, cache=UNITS, optimize=True))
        rec = {k: tt[k] for k in FIELDS}
        rec['compressedBlob'] = bytes(rec['compressedBlob'])
    except Exception as e:
        tt = obj.read_typetree()
        rec = neutral_fields(tt, f'{type(e).__name__}: {e}'[:400])
    shutil.rmtree(work, ignore_errors=True)
    rec['_name'] = name
    rec['_stats'] = stats
    tmp = out + f'.{os.getpid()}.tmp'
    with open(tmp, 'wb') as fh:
        pickle.dump(rec, fh)
    os.replace(tmp, out)
    return dict(hash=h, name=name, cached=False, stats=stats, neutralized=rec.get('_neutralized'), seconds=time.time() - t0)


def data_shader_tasks():
    tasks = []
    env = UnityPy.load(DATA_SRC)
    for obj in env.objects:
        if obj.type.name == 'Shader':
            tt = obj.read_typetree()
            tasks.append((DATA_SRC, obj.path_id, shader_hash(tt), sum(sum(x) if isinstance(x, list) else x for x in tt['decompressedLengths'])))
    return tasks


def cmd_convert(a):
    scan = json.load(open(os.path.join(STATE, 'scan.json')))
    tasks = {}
    for r in scan['records']:
        for s in r['shaders']:
            tasks.setdefault(s['hash'], (r['path'], s['path_id'], s['hash'], s['blob']))
    for p, pid, h, blob in data_shader_tasks():
        tasks.setdefault(h, (p, pid, h, blob))
    order = sorted(tasks.values(), key=lambda t: -t[3])        # biggest first
    print(f'{len(order)} distinct Shader objects to convert', flush=True)
    t0 = time.time()
    tot = collections.Counter()
    neutral = []
    log = open(os.path.join(STATE, 'convert.log'), 'a', encoding='utf-8')
    done = [0]
    stop = threading.Event()

    def progress():                     # every 30 s: the program units converted so far (the cache files)
        while not stop.wait(30):
            units = sum(len(os.listdir(e.path)) for e in os.scandir(UNITS) if e.is_dir()) if os.path.isdir(UNITS) else 0
            pct = min(99, units * 100 // EXPECTED_UNITS)
            print(f'progress: converting shaders {pct}% ({units} of about {EXPECTED_UNITS} programs, {done[0]} of '
                  f'{len(order)} shaders done, {int(time.time() - t0) // 60} min)', flush=True)
    threading.Thread(target=progress, daemon=True).start()
    with ProcessPoolExecutor(a.jobs) as ex:
        futs = [ex.submit(convert_one, t[:3]) for t in order]
        for n, f in enumerate(as_completed(futs)):
            done[0] = n + 1
            r = f.result()
            for k, v in r['stats'].items():
                tot[k] += v
            tot['shader objects'] += 1
            tot['from shader cache'] += r['cached']
            if r['neutralized']:
                neutral.append((r['name'], r['neutralized']))
            log.write(json.dumps(r) + '\n')
            log.flush()
            if not r['cached'] and (r['seconds'] > 20 or n % 25 == 0):
                print(f"[{time.time() - t0:6.0f}s] {n + 1}/{len(order)} {r['name']} units={r['stats'].get('units', 0)} {r['seconds']:.0f}s"
                      + (' NEUTRALISED: ' + r['neutralized'] if r['neutralized'] else ''), flush=True)
    stop.set()
    print(f'convert done in {time.time() - t0:.0f}s')
    for k in sorted(tot):
        print(f'{tot[k]:>10}  {k}')
    print('neutralised:')
    for n, why in sorted(set(neutral)):
        print(f'   {n}: {why}')
    json.dump(dict(totals=tot, neutralised=sorted(set(neutral)), seconds=time.time() - t0),
              open(os.path.join(STATE, 'convert_summary.json'), 'w'), indent=1)


# ---------------------------------------------------------------- write

def apply_shaders(env):
    n = 0
    for obj in env.objects:
        if obj.type.name != 'Shader':
            continue
        tt = obj.read_typetree()
        h = shader_hash(tt)
        with open(os.path.join(SHADERS, h + '.pkl'), 'rb') as fh:
            rec = pickle.load(fh)
        blob_is_bytes = isinstance(tt['compressedBlob'], (bytes, bytearray))
        for k in FIELDS:
            tt[k] = rec[k]
        if not blob_is_bytes:
            tt['compressedBlob'] = list(rec['compressedBlob'])
        obj.save_typetree(tt)
        n += 1
    return n


def save_env(env, dst, target=None):
    root = env.file
    if target is not None:
        tgt = BuildTarget(target)

        def walk(f):
            if isinstance(f, SerializedFile):
                f.target_platform = tgt
                f._m_target_platform = int(target)
                f.mark_changed()
            elif hasattr(f, 'files'):
                for c in f.files.values():
                    walk(c)
        walk(root)
    data = root.save(packer='lz4') if isinstance(root, BundleFile) else root.save()
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + '.tmp'
    with open(tmp, 'wb') as fh:
        fh.write(data)
    os.replace(tmp, dst)
    return len(data)


def write_one(path):
    dst = os.path.join(DST, os.path.relpath(path, SRC))
    env = UnityPy.load(path)
    n = apply_shaders(env)
    size = save_env(env, dst)
    return path, n, os.path.getsize(path), size


def cmd_write(a):
    scan = json.load(open(os.path.join(STATE, 'scan.json')))
    with_shaders = {r['path'] for r in scan['records'] if r['shaders']}
    files = all_bundles()
    t0 = time.time()
    linked = 0
    for f in files:
        if f in with_shaders:
            continue
        dst = os.path.join(DST, os.path.relpath(f, SRC))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.exists(dst):
            os.remove(dst)
        try:
            if not f.endswith('.bundle'):
                raise OSError('copy catalog/settings so that later edits cannot reach aa_win')
            os.link(f, dst)
        except OSError:
            shutil.copy2(f, dst)
        linked += 1
    print(f'{linked} unchanged files hard-linked in {time.time() - t0:.0f}s', flush=True)
    insz = outsz = nsh = 0
    rows = []
    with ProcessPoolExecutor(a.jobs) as ex:
        for path, n, a_, b_ in ex.map(write_one, sorted(with_shaders), chunksize=4):
            insz += a_; outsz += b_; nsh += n
            rows.append((os.path.relpath(path, SRC), n, a_, b_))
    with open(os.path.join(STATE, 'write_report.tsv'), 'w') as fh:
        fh.write('bundle\tshaders\tbytes_in\tbytes_out\n')
        for r in rows:
            fh.write('\t'.join(map(str, r)) + '\n')
    print(f'{len(with_shaders)} bundles rewritten ({nsh} Shader objects): {insz / 1e6:.1f} MB -> {outsz / 1e6:.1f} MB; total {time.time() - t0:.0f}s')


def cmd_data(a):
    env = UnityPy.load(DATA_SRC)
    n = apply_shaders(env)
    for obj in env.objects:
        if obj.type.name == 'BuildSettings':
            tt = obj.read_typetree()
            tt['m_GraphicsAPIs'] = [21]                 # Vulkan instead of Switch NVN (22)
            obj.save_typetree(tt)
        elif obj.type.name == 'GraphicsSettings':
            tt = obj.read_typetree()
            for d in tt['m_ShaderDefinesPerShaderCompiler']:
                if d['shaderPlatform'] == 19:
                    d['shaderPlatform'] = 18
            obj.save_typetree(tt)
    size = save_env(env, DATA_DST, target=19)
    print(f'data.unity3d: {n} Shader objects, {os.path.getsize(DATA_SRC) / 1e6:.2f} MB -> {size / 1e6:.2f} MB at {DATA_DST}')


# ---------------------------------------------------------------- verify

def verify_one(path):
    import smolv
    import vk_shader
    from switch_shader import platform_segments, entry_table, entry_bytes, iter_variants
    c = collections.Counter()
    problems = []
    mods = {}
    try:
        env = UnityPy.load(path)
        for obj in env.objects:
            if obj.type.name == 'ComputeShader':
                c['ComputeShader objects'] += 1
            if obj.type.name != 'Shader':
                continue
            c['shader objects'] += 1
            tt = obj.read_typetree()
            name = tt['m_ParsedForm']['m_Name']
            if tt['platforms'] != [18]:
                problems.append(f'{name}: platforms {tt["platforms"]}')
                continue
            segs = dict(platform_segments(tt))[18]
            ents = entry_table(segs)
            if not tt['m_ParsedForm']['m_SubShaders']:
                c['neutralised shader objects'] += 1
            parsed = {}
            for si, pi, ps, prog, t, k, sp, pbi in iter_variants(tt):
                c['variants'] += 1
                if prog != 'progVertex' or sp['m_GpuProgramType'] != 25:
                    problems.append(f"{name}: {prog} program type {sp['m_GpuProgramType']}")
                    continue
                for idx, kind in ((sp['m_BlobIndex'], 's'), (pbi, 'p')):
                    if (idx, kind) in parsed:
                        continue
                    parsed[(idx, kind)] = 1
                    b = entry_bytes(segs, ents[idx])
                    if kind == 'p':
                        vk_shader.parse_params_raw(b)
                        c['parameter blobs'] += 1
                        continue
                    sub = vk_shader.parse_subprogram_kw(b)
                    vk = vk_shader.parse_vk_code(sub['code'])
                    c['subprogram blobs'] += 1
                    if set(vk['modules']) != {0, 1}:
                        problems.append(f'{name}: stages {sorted(vk["modules"])}')
                    for st, m in vk['modules'].items():
                        c['modules'] += 1
                        hx = m['hash'].hex()
                        if hx in mods:
                            continue
                        mods[hx] = len(m['smolv'])
                        mark = os.path.join(STATE, 'validated', hx[:2], hx)
                        if os.path.exists(mark):
                            continue
                        spv = smolv.decode(m['smolv'])
                        if hashlib.md5(spv).digest() != m['hash']:
                            problems.append(f'{name}: module hash mismatch')
                        tmp = os.path.join(TMP, f'v{os.getpid()}.spv')
                        with open(tmp, 'wb') as fh:
                            fh.write(spv)
                        err = convert.spirv_val(tmp)
                        if err:
                            problems.append(f'{name}: spirv-val: {err[:200]}')
                        else:
                            os.makedirs(os.path.dirname(mark), exist_ok=True)
                            open(mark, 'w').close()
                            c['modules validated by this run'] += 1
    except Exception as e:
        problems.append(f'exception {type(e).__name__}: {e} {traceback.format_exc()[-300:]}')
    return path, dict(c), problems, mods


def cmd_verify(a):
    os.makedirs(os.path.join(STATE, 'validated'), exist_ok=True)
    os.makedirs(TMP, exist_ok=True)
    files = []
    for r, _, fs in os.walk(DST):
        files += [os.path.join(r, f) for f in fs if f.endswith('.bundle')]
    files = sorted(files) + [DATA_DST]
    t0 = time.time()
    tot = collections.Counter()
    problems = []
    mods = {}
    with ProcessPoolExecutor(a.jobs) as ex:
        for i, (path, c, pr, m) in enumerate(ex.map(verify_one, files, chunksize=16)):
            for k, v in c.items():
                tot[k] += v
            if c.get('shader objects'):
                tot['files with Shader objects'] += 1
            problems += [f'{path}: {x}' for x in pr]
            mods.update(m)
            if i % 2000 == 0:
                print(f'verify {i}/{len(files)}', flush=True)
    tot['files scanned'] = len(files)
    tot['unique modules'] = len(mods)
    tot['unique module SMOL-V bytes'] = sum(mods.values())
    print(f'verify done in {time.time() - t0:.0f}s')
    for k in sorted(tot):
        print(f'{tot[k]:>12}  {k}')
    print(f'problems: {len(problems)}')
    for p in problems[:40]:
        print('  ', p)
    json.dump(dict(totals=tot, problems=problems), open(os.path.join(STATE, 'verify.json'), 'w'), indent=1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['all', 'scan', 'convert', 'write', 'data', 'verify'])
    ap.add_argument('--jobs', type=int, default=8)
    a = ap.parse_args()
    for d in (STATE, UNITS, SHADERS, TMP):
        os.makedirs(d, exist_ok=True)
    steps = dict(scan=cmd_scan, convert=cmd_convert, write=cmd_write, data=cmd_data, verify=cmd_verify)
    for s in (['scan', 'convert', 'write', 'data'] if a.cmd == 'all' else [a.cmd]):
        t0 = time.time()
        steps[s](a)
        print(f'== {s}: {time.time() - t0:.0f}s', flush=True)


if __name__ == '__main__':
    main()
