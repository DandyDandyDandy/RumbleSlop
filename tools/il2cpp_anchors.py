"""Extra checks after il2cpp_link.py / il2cpp_layout.py: semantic spot-checks, interop table, .text component map.

    python il2cpp_anchors.py <nso> <outdir>
"""
import struct, sys, os, json, bisect, collections, re
import numpy as np
import capstone
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from il2cpp_link import load_image

nso_path, outdir = sys.argv[1:3]
n, img, _, _, _ = load_image(nso_path)
text_hi = n.text[1]
layout = json.load(open(os.path.join(outdir, 'layout.json')))
H = lambda s: int(s, 16) if isinstance(s, str) else s
q = lambda a: struct.unpack_from('<Q', img, a)[0]
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
out = open(os.path.join(outdir, 'anchors_report.txt'), 'w', encoding='utf-8', newline='\n')


def P(*a):
    s = ' '.join(str(x) for x in a)
    print(s)
    out.write(s + '\n')


h0 = n.eh_frame_hdr[0]
cnt = struct.unpack_from('<I', img, h0 + 8)[0]
tab = np.frombuffer(bytes(img[h0 + 12:h0 + 12 + 8 * cnt]), dtype='<i4').reshape(-1, 2)
starts_l = np.unique(tab[:, 0].astype(np.int64) + h0).tolist()
func_of = lambda a: starts_l[bisect.bisect_right(starts_l, a) - 1]
fsize = lambda f: (starts_l[bisect.bisect_right(starts_l, f)] if f < starts_l[-1] else text_hi) - f


def dis(a, nbytes=48, maxins=12):
    r = []
    for ins in md.disasm(bytes(img[a:a + nbytes]), a):
        r.append(f'{ins.mnemonic} {ins.op_str}'.strip())
        if len(r) >= maxins or ins.mnemonic in ('ret',):
            break
    return '; '.join(r)


# ---- semantic spot checks
want = {('mscorlib.dll', 'System.Object', '.ctor'), ('mscorlib.dll', 'System.String', 'get_Length'), ('mscorlib.dll', 'System.String', 'get_Chars'),
        ('UnityEngine.CoreModule.dll', 'UnityEngine.Vector3', '.ctor'), ('UnityEngine.CoreModule.dll', 'UnityEngine.Vector2', 'get_Item'),
        ('mscorlib.dll', 'System.IntPtr', 'get_Size'), ('mscorlib.dll', 'System.Boolean', 'GetTypeCode'), ('mscorlib.dll', 'System.Int32', 'GetTypeCode'),
        ('mscorlib.dll', 'System.Double', 'GetTypeCode'), ('mscorlib.dll', 'System.Array', 'get_Rank'), ('UnityEngine.CoreModule.dll', 'UnityEngine.Color', '.ctor'),
        ('mscorlib.dll', 'System.Collections.Generic.List`1', 'get_Count'), ('UnityEngine.CoreModule.dll', 'UnityEngine.Mathf', 'Clamp01'),
        ('UnityEngine.CoreModule.dll', 'UnityEngine.Time', 'get_deltaTime'), ('mscorlib.dll', 'System.Char', 'GetTypeCode')}
P('semantic spot checks (name -> disassembly):')
seen = set()
rows = []
with open(os.path.join(outdir, 'methods.tsv'), encoding='utf-8') as f:
    next(f)
    for line in f:
        p = line.rstrip('\n').split('\t')
        rows.append(p)
        k = (p[0], p[1], p[2])
        if k in want and k not in seen and p[3]:
            seen.add(k)
            a = int(p[3], 16)
            P(f'  {p[1]}::{p[2]} @{a:#x} [{fsize(a)}b]: {dis(a)}')
# icall string check: an extern method should reference its own "Type::Method" icall name string
P('\nicall-name self-reference check (UnityEngine extern wrappers load "Ns.Type::Method" string):')
ww = np.frombuffer(bytes(img[:text_hi & ~3]), dtype='<u4').astype(np.int64)
idx = np.nonzero(((ww & 0x9f000000) == 0x90000000)[:-1])[0]
a0, a1 = ww[idx], ww[idx + 1]
rd = a0 & 31
imm = (((a0 >> 5) & 0x7ffff) << 2) | ((a0 >> 29) & 3)
imm = np.where(imm & (1 << 20), imm - (1 << 21), imm)
page = ((idx * 4) & ~0xfff) + (imm << 12)
is_add = ((a1 & 0xffc00000) == 0x91000000) & ((a1 & 31) == rd) & (((a1 >> 5) & 31) == rd)
xa, xt = idx[is_add] * 4, (page + ((a1 >> 10) & 0xfff))[is_add]
o = np.argsort(xa)
xa, xt = xa[o], xt[o]


def strrefs(f):
    i0, i1 = np.searchsorted(xa, f), np.searchsorted(xa, f + fsize(f))
    r = []
    for t in xt[i0:i1].tolist():
        if n.rodata[0] <= t < n.rodata[0] + n.rodata[1]:
            e = img.find(b'\0', t, t + 200)
            if e > t:
                try:
                    r.append(bytes(img[t:e]).decode('ascii'))
                except UnicodeDecodeError:
                    pass
    return r


ok = tot = 0
examples = []
for p in rows:
    if not p[3] or not p[0].startswith('UnityEngine.'):
        continue
    a = int(p[3], 16)
    if fsize(a) > 160:
        continue
    refs = [s for s in strrefs(a) if '::' in s]
    if not refs:
        continue
    tot += 1
    exp = p[1].replace('+', '/') + '::' + p[2]
    hit = any(s.split('(')[0] == exp for s in refs)
    ok += hit
    if len(examples) < 4 or (not hit and len(examples) < 10):
        examples.append((exp, refs[0], hit))
P(f'  small UnityEngine.* methods that reference an icall-name string: {tot}; string equals the method\'s own Type::Method: {ok}')
for e in examples:
    P('   ', e)

# ---- interop data
cr = layout['code_registration']['fields']
it, ic = H(cr['interopData']), H(cr['interopDataCount'])
ia = set()
for i in range(ic):
    for j in range(5):
        v = q(it + 56 * i + 8 * j)
        if v:
            ia.add(v)
P(f'\ninteropData: {ic} records @{it:#x}; {len(ia)} distinct function pointers, range {min(ia):#x}-{max(ia):#x}; all in .text: {all(x < text_hi for x in ia)}')

# ---- registration plumbing
f = 0x17d350
got = q(0x6129028)
P(f'\ns_Il2CppCodegenRegistration @{f:#x}: x0=&g_CodeRegistration, x1=[GOT 0x6129028]={got:#x} (MetadataRegistration={layout["metadata_registration"]["address"]}), x2=CodeGenOptions 0x4b03ca0, tail-calls 0x4506c0')
P(f'  il2cpp_codegen_register @0x4506c0 [{fsize(0x4506c0)}b]: {dis(0x4506c0, 64, 16)}')
ia_arr = n.dt[25]
P(f'  init_array {ia_arr:#x} entries: {n.dt[27] // 8}')

# ---- Unity side caller of il2cpp_init: function with "IL2CPP Root Domain"
for fa, label in ((0x107b060, 'Unity scripting init (refs "IL2CPP Root Domain")'), (0x436a40, 'refs "global-metadata.dat" (MetadataCache/MetadataLoader init)'),
                  (0x471420, 'refs "mscorlib.dll" in libil2cpp (candidate il2cpp::vm::Runtime::Init)')):
    P(f'\n{label} @{fa:#x} [{fsize(fa)}b]:')
    k = 0
    for ins in md.disasm(bytes(img[fa:fa + min(fsize(fa), 400)]), fa):
        if ins.mnemonic in ('bl', 'b', 'adrp', 'add') and (ins.mnemonic != 'add' or '#0x' in ins.op_str and 'sp' not in ins.op_str):
            P(f'    {ins.address:#x}: {ins.mnemonic} {ins.op_str}')
            k += 1
            if k > 40:
                break

# ---- component map of the non-generated middle: classify functions by referenced source-path / id strings
P('\ncomponent map of .text by string evidence (function start ranges):')
pats = collections.OrderedDict([
    ('libil2cpp', re.compile(r'il2cpp/libil2cpp/|^il2cpp::|IL2CPP')),
    ('bdwgc', re.compile(r'^GC Warning|^GC_|Too many heap sections')),
    ('criware', re.compile(r'^[EW]\d{8,}[A-Za-z0-9]*:|CRIWARE|^cri[A-Z]\w+')),
    ('unity_engine', re.compile(r'^\./(Runtime|Modules|PlatformDependent|Platforms|External)/|^(Runtime|Modules|PlatformDependent)/')),
    ('nintendo_sdk', re.compile(r'^nn::|siglo|Siglo|NintendoSDK')),
    ('physx', re.compile(r'PhysX|physx|^Px[A-Z]')),
    ('fmod', re.compile(r'FMOD|fmod')),
])
comp = collections.defaultdict(list)
i_all = np.arange(len(xa))
ro0, ro1 = n.rodata[0], n.rodata[0] + n.rodata[1]
cache = {}
for a, t in zip(xa.tolist(), xt.tolist()):
    if not ro0 <= t < ro1:
        continue
    c = cache.get(t)
    if c is None:
        e = img.find(b'\0', t, t + 160)
        s = bytes(img[t:e if e > 0 else t + 160]).decode('latin1')
        c = ''
        for k, rx in pats.items():
            if rx.search(s):
                c = k
                break
        cache[t] = c
    if c:
        comp[c].append(a)
for k, v in comp.items():
    v.sort()
    # 1%..99% quantile range and 256K-bucket density
    lo, hi = v[len(v) // 100], v[-1 - len(v) // 100]
    b = collections.Counter(x >> 20 for x in v)
    dense = sorted(b)
    P(f'  {k:13} {len(v):>7} xrefs; min {v[0]:#x} p1 {lo:#x} p99 {hi:#x} max {v[-1]:#x}')
    P('      per-MiB: ' + ' '.join(f'{m:#x}:{b[m]}' for m in dense if b[m] >= max(3, len(v) // 400)))
out.close()
