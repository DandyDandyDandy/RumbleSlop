"""For each function in the il2cpp-api cluster print its tail-call/bl targets and the head of each target."""
from _env import GAME, REPO, TOOLS, UNITY_EDITOR, W  # noqa: E402,F401
import sys, bisect, re, numpy as np
sys.path.insert(0, TOOLS)
from api_cluster_dump import dis, fde_starts, d, ROOT
s = fde_starts(); e = np.load(W + r'\work\unity\api\fde_ends.npy')
sl = s.tolist()
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16); mx = int(sys.argv[3]) if len(sys.argv) > 3 else 22
def strnote(lines):
    pages = {}; out = []
    for l in lines:
        m = re.match(r'adrp (x\d+), #(0x[0-9a-f]+)', l)
        if m: pages[m.group(1)] = int(m.group(2), 16)
        m = re.match(r'add (x\d+), (x\d+), #(0x[0-9a-f]+|\d+)$', l)
        if m and m.group(2) in pages:
            t = pages[m.group(2)] + int(m.group(3), 0)
            if 0x483e000 <= t < 0x5e22000:
                st = d[t:t+48].split(b'\0')[0]
                if len(st) >= 4 and all(32 <= c < 127 for c in st): l += ' {%r}' % st.decode()
        out.append(l)
    return out
i = bisect.bisect_left(sl, lo)
while i < len(sl) and sl[i] < hi:
    body = dis(sl[i], int(e[i]), 40)
    print('== %x [%d]: %s' % (sl[i], e[i]-s[i], '; '.join(strnote(body[:16]))))
    seen = set()
    for l in body:
        m = re.match(r'(b|bl) #(0x[0-9a-f]+)$', l)
        if m:
            t = int(m.group(2), 16)
            if lo <= t < hi or t in seen or t > 0xc6e000 or (sl[i] <= t < e[i]): continue
            seen.add(t)
            j = bisect.bisect_right(sl, t) - 1
            off = t - sl[j]
            print('   -> %x (%s+%x, fn size %d): %s' % (t, hex(sl[j]), off, e[j]-s[j], '; '.join(strnote(dis(t, min(int(e[j]), t + 4*mx), mx)))))
    i += 1
