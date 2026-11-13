"""Recover (icall name -> engine function address) from main.bin by decoding every call site of
scripting_add_internal_call (0x106fdc0). Two patterns:
  loop:     ldrsw x8,[xA,xI,lsl#2]; add x0,xA,x8; ldr x1,[xB,xI,lsl#3]; bl; add xI,xI,#1; cmp xI,#N
            (xA = int32 self-relative name table in rodata, xB = function pointer table in data)
  unrolled: adrp/add x0 -> name ; adrp x1 ; ldr x1,[x1,#off] -> function pointer slot in data ; bl
Writes work/unity/icalls_main.tsv"""
from _env import GAME, REPO, TOOLS, UNITY_EDITOR, W  # noqa: E402,F401
import struct, numpy as np, capstone, re, os
MAIN = W + r'\work\re\main.bin'
OUT = W + r'\work\unity'
REG = 0x106fdc0
TS = 0x483dae8
d = open(MAIN, 'rb').read()
w = np.frombuffer(d[:TS & ~3], dtype='<u4')
pc = np.arange(len(w), dtype=np.int64) * 4
imm = (w & 0x3ffffff).astype(np.int64); imm = np.where(imm & (1 << 25), imm - (1 << 26), imm)
sites = [int(x) * 4 for x in np.nonzero(((w & 0x7c000000) == 0x14000000) & (pc + imm * 4 == REG))[0]]
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
def cstr(a): return d[a:d.index(b'\0', a)].decode('latin1')
def q(a): return struct.unpack_from('<Q', d, a)[0]
def run(start, end):
    """linear register tracking over [start,end)"""
    r = {}
    for i in md.disasm(d[start:end], start):
        m, o = i.mnemonic, i.op_str
        if m == 'adrp':
            a, b = o.split(', #'); r[a] = int(b, 16)
        elif m == 'adr':
            a, b = o.split(', #'); r[a] = int(b, 16)
        elif m == 'add':
            mm = re.match(r'(x\d+), (x\d+), #(0x[0-9a-f]+|\d+)$', o)
            if mm and mm.group(2) in r: r[mm.group(1)] = r[mm.group(2)] + int(mm.group(3), 0)
            elif mm: r.pop(mm.group(1), None)
        elif m == 'ldr':
            mm = re.match(r'(x\d+), \[(x\d+)(?:, #(0x[0-9a-f]+|\d+))?\]$', o)
            if mm and mm.group(2) in r:
                r[mm.group(1)] = ('mem', r[mm.group(2)] + int(mm.group(3) or '0', 0))
            elif mm: r.pop(mm.group(1), None)
        elif m == 'mov':
            mm = re.match(r'(x\d+), (x\d+)$', o)
            if mm and mm.group(2) in r: r[mm.group(1)] = r[mm.group(2)]
            elif mm: r.pop(mm.group(1), None)
    return r
rows = []; bad = []; nloop = nunr = 0; loops = []
for s in sites:
    ins = list(md.disasm(d[s - 12:s + 16], s - 12))
    if ins[0].mnemonic == 'ldrsw':
        mA = re.match(r'x8, \[(x\d+), (x\d+), lsl #2\]', ins[0].op_str)
        mB = re.match(r'x1, \[(x\d+), (x\d+), lsl #3\]', ins[2].op_str)
        mC = re.match(r'(x\d+), #(0x[0-9a-f]+|\d+)$', ins[5].op_str) if ins[5].mnemonic == 'cmp' else None
        r = run(s - 12 - 4 * 24, s - 12)
        if not (mA and mB and mC and mA.group(1) in r and mB.group(1) in r):
            bad.append((s, 'loop-unparsed')); continue
        A, B, N = r[mA.group(1)], r[mB.group(1)], int(mC.group(2), 0)
        loops.append((s, A, B, N))
        for i in range(N):
            rel = struct.unpack_from('<i', d, A + 4 * i)[0]
            rows.append((cstr(A + rel), q(B + 8 * i), s, 'loop'))
        nloop += 1
    else:
        r = run(s - 4 * 6, s)
        x0, x1 = r.get('x0'), r.get('x1')
        if not isinstance(x0, int) or x1 is None:
            bad.append((s, 'unrolled-unparsed')); continue
        fn = q(x1[1]) if isinstance(x1, tuple) else x1
        rows.append((cstr(x0), fn, s, 'unrolled' if isinstance(x1, tuple) else 'unrolled-direct'))
        nunr += 1
print('call sites', len(sites), 'loops', nloop, 'unrolled', nunr, 'unparsed', bad and [(hex(a), b) for a, b in bad])
for s, A, B, N in loops: print('  loop @%#x names=%#x funcs=%#x count=%d first=%s' % (s, A, B, N, cstr(A + struct.unpack_from('<i', d, A)[0])))
names = {}
dup = 0
for n, f, s, k in rows:
    if n in names and names[n][0] != f: dup += 1
    names[n] = (f, s, k)
oob = [n for n, (f, s, k) in names.items() if not (0 < f < TS and f % 4 == 0)]
notname = [n for n in names if '::' not in n]
print('rows', len(rows), 'unique names', len(names), 'conflicting dups', dup, 'fn outside text', len(oob), oob[:5], 'non-icall-looking names', len(notname), notname[:5])
with open(os.path.join(OUT, 'icalls_main.tsv'), 'w') as f:
    f.write('name\taddress\tregistered_at\tpattern\n')
    for n in sorted(names): f.write('%s\t%#x\t%#x\t%s\n' % (n, names[n][0], names[n][1], names[n][2]))
eng = set(open(os.path.join(OUT, 'icalls_engine_main.txt')).read().split('\n')) - {''}
used = {l.split('(')[0] for l in open(os.path.join(OUT, 'icalls_managed_used_main.txt')) if l.strip()}
print('string-scan engine names', len(eng), 'covered', len(eng & set(names)), 'string-scan only', len(eng - set(names)), sorted(eng - set(names))[:30])
print('table-only (not in string scan)', len(set(names) - eng), sorted(set(names) - eng)[:30])
print('managed-used', len(used), 'mapped', len(used & set(names)), 'unmapped', sorted(used - set(names))[:30])
import collections
c = collections.Counter(f for f, s, k in names.values())
print('distinct fn addrs', len(c), 'most shared', [(hex(a), n) for a, n in c.most_common(5)])
