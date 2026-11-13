"""fn.py ADDR[,ADDR..] [maxinsn] - one-line disassembly (with string annotations) of the FDE function at/containing each address, plus caller counts by region."""
from _env import GAME, REPO, TOOLS, UNITY_EDITOR, W  # noqa: E402,F401
import sys, bisect, re, numpy as np
sys.path.insert(0, TOOLS)
from api_cluster_dump import dis, fde_starts, d, ROOT
s = fde_starts(); e = np.load(W + r'\work\unity\api\fde_ends.npy'); sl = s.tolist()
TS = 0x483dae8
w = np.frombuffer(d[:TS & ~3], dtype='<u4')
pc = np.arange(len(w), dtype=np.int64) * 4
isb = (w & 0x7c000000) == 0x14000000
imm = (w & 0x3ffffff).astype(np.int64); imm = np.where(imm & (1 << 25), imm - (1 << 26), imm)
tgt = np.where(isb, pc + imm * 4, -1)
def region(a):
    for nm, lo, hi in (('glue', 0x690, 0x3d8480), ('gc', 0x3d8480, 0x3e6c30), ('rt', 0x3e6c30, 0x474500), ('os', 0x474500, 0x477600), ('cri', 0x477600, 0x57c000), ('plug', 0x57c000, 0xc6e000), ('eng', 0xc6e000, 0x1f4ee64), ('gen', 0x1f8acb0, TS)):
        if lo <= a < hi: return nm
    return 'other'
def callers(a):
    h = np.nonzero(tgt == a)[0] * 4
    return h
def strnote(lines):
    pages = {}; out = []
    for l in lines:
        m = re.match(r'adrp (x\d+), #(0x[0-9a-f]+)', l)
        if m: pages[m.group(1)] = int(m.group(2), 16)
        m = re.match(r'add (x\d+), (x\d+), #(0x[0-9a-f]+|\d+)$', l)
        if m and m.group(2) in pages:
            t = pages[m.group(2)] + int(m.group(3), 0)
            if 0x483e000 <= t < 0x5e22000:
                st = d[t:t+60].split(b'\0')[0]
                if len(st) >= 3 and all(32 <= c < 127 for c in st): l += ' {%r}' % st.decode()
            else: l += ' {@%x}' % t
        out.append(l)
    return out
def show(a, mx=40):
    j = bisect.bisect_right(sl, a) - 1
    c = callers(sl[j])
    import collections
    cr = collections.Counter(region(int(x)) for x in c)
    print('== %x%s [%d] callers %d %s e.g. %s\n   %s' % (sl[j], '' if sl[j] == a else '(+%x)' % (a - sl[j]), e[j]-s[j], len(c), dict(cr), [hex(int(x)) for x in c[:4]], '; '.join(strnote(dis(sl[j], int(e[j]), mx)))))
if __name__ == '__main__':
    mx = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    for a in sys.argv[1].split(','):
        show(int(a, 16), mx)
