from _env import GAME, REPO, TOOLS, UNITY_EDITOR, W  # noqa: E402,F401
import sys; sys.path.insert(0,TOOLS)
from api_map_data import W
import numpy as np, bisect, re
s=np.load(W + r'\work\unity\api\fde_starts.npy').tolist()
i0=bisect.bisect_left(s,0x4698c0); i1=bisect.bisect_left(s,0x46a328)
cl=s[i0:i1]; used=[v[0] for v in W.values()]
print(len(cl),len(W),len(set(used)), 'unassigned',[hex(a) for a in cl if a not in used], 'bad',[hex(a) for a in used if a not in cl])
print('order ok', used==sorted(used))
h=open(UNITY_EDITOR + r'\il2cpp\libil2cpp\il2cpp-api-functions.h').read()
api=re.findall(r'^\s*DO_API(?:_NO_RETURN)?\((.*?),\s*(il2cpp_\w+)\s*,\s*(\(.*\))\);',h,re.M)
names=[a[1] for a in api if 'profiler' not in a[1]]
print('api',len(names),'W not in api',set(W)-set(names))
use={}
for l in open(W + r'\work\unity\api\up_users.tsv').read().split('\n')[1:]:
    p=l.split('\t')
    if len(p)>2: use[p[0]]=(int(p[2]),p[4])
miss=[n for n in names if n not in W]
print('missing',len(miss))
print('MISSING BUT USED ON WINDOWS:')
for n in miss:
    if use[n][0]: print('  ',n,use[n][0],use[n][1][:300])
print('PRESENT ON SWITCH, UNUSED ON WINDOWS:',[n for n in W if not use[n][0]])
