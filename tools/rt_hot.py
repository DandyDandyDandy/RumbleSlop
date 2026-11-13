"""Rank runtime (libil2cpp range) functions by number of direct call sites; writes work/unity/api/rt_callcounts.tsv"""
from _env import GAME, REPO, TOOLS, UNITY_EDITOR, W  # noqa: E402,F401
import sys, bisect, numpy as np, collections
sys.path.insert(0, TOOLS)
import fn
t = fn.tgt
m = (t >= 0x3e6c30) & (t < 0x477600)
src = np.nonzero(m)[0] * 4
tg = t[m]
u, inv = np.unique(tg, return_inverse=True)
reg = np.array([fn.region(int(x)) for x in src])
rows = []
for k, a in enumerate(u.tolist()):
    r = reg[inv == k]
    c = collections.Counter(r.tolist())
    rows.append((a, len(r), c))
rows.sort(key=lambda x: -x[1])
with open(fn.W + r'\work\unity\api\rt_callcounts.tsv', 'w') as f:
    f.write('target\tcallsites\tby_region\tis_fde_start\n')
    for a, n, c in rows:
        f.write('%#x\t%d\t%s\t%d\n' % (a, n, dict(c), a in set(fn.sl[bisect.bisect_left(fn.sl, a):bisect.bisect_left(fn.sl, a)+1])))
for a, n, c in rows[:70]:
    j = bisect.bisect_right(fn.sl, a) - 1
    print('%x %d %s :: %s' % (a, n, dict(c), '; '.join(fn.strnote(fn.dis(a, int(fn.e[j]), 9)))[:230]))
