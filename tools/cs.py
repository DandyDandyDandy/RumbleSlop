from _env import GAME, REPO, TOOLS, UNITY_EDITOR, W  # noqa: E402,F401
r"""cs.py - reading aid for the game's managed code (system Python, needs capstone + numpy).

    python tools\cs.py type <substring|=Exact.Name> ...     fields (offset, type) and methods (signature, address)
    python tools\cs.py dis <addr|Type::Method> ...          annotated disassembly: call targets, metadata usages
                                                            (types, methods, strings, fields), interface slots
    python tools\cs.py find <regex>                         method names matching (type::method)
    python tools\cs.py callers <addr|Type::Method>          direct callers

Field offsets are as stored by il2cpp: for classes they include the 16-byte object header; for value types they do
too, so subtract 0x10 for an unboxed struct.
"""
import bisect, csv, json, os, re, struct, sys
import numpy as np

ROOT = REPO
U = W + r'\work\unity'
img = open(W + r'\work\re\main.bin', 'rb').read()
md_ = open(W + r'\work\game\romfs\Data\Managed\Metadata\global-metadata.dat', 'rb').read()
L = json.load(open(U + r'\layout.json'))
MR = {k: (int(v, 16) if isinstance(v, str) else v) for k, v in L['metadata_registration']['fields'].items()}
_h = lambda v: int(v, 16) if isinstance(v, str) else v
TS = _h(L['text'][1])                       # size of .text (from layout.json, so any game version works)
DATA_LO = _h(L['data'][0])                  # start of .data: the metadata-usage pointer slots are here
DATA_HI = DATA_LO + _h(L['data'][1])

q = lambda o: struct.unpack_from('<Q', img, o)[0]
u = lambda o: struct.unpack_from('<I', img, o)[0]
i32 = lambda o: struct.unpack_from('<i', img, o)[0]
H = struct.unpack_from('<62I', md_, 8)
sec = lambda i: (H[2 * i], H[2 * i + 1])
STR = sec(2)[0]


def s(idx):
    a = STR + idx
    return md_[a:md_.index(b'\0', a)].decode('utf-8', 'replace')


mo, ms = sec(5); to, tsz = sec(19); po, psz = sec(10); fo, fsz = sec(11); gpo, gps = sec(12); nto, nts = sec(15)
slo, sls = sec(0); sdo, _ = sec(1); fro, frs = sec(22); ino, ins_ = sec(16)
NT = tsz // 88
types = [struct.unpack_from('<16i8HII', md_, to + 88 * i) for i in range(NT)]
nested = struct.unpack_from('<%di' % (nts // 4), md_, nto)
decl = [-1] * NT
for ti, t in enumerate(types):
    for k in range(t[12], t[12] + t[20]):
        decl[nested[k]] = ti
_tn = {}


def type_name(ti):
    r = _tn.get(ti)
    if r is None:
        t = types[ti]
        nm = s(t[0])
        if decl[ti] >= 0:
            r = type_name(decl[ti]) + '+' + nm
        else:
            ns = s(t[1])
            r = ns + '.' + nm if ns else nm
        _tn[ti] = r
    return r


def method(i):
    return struct.unpack_from('<IiiiiIHHHH', md_, mo + 32 * i)   # name, decl, ret, pstart, gcont, token, flags, iflags, slot, pcount


PRIM = {1: 'void', 2: 'bool', 3: 'char', 4: 'sbyte', 5: 'byte', 6: 'short', 7: 'ushort', 8: 'int', 9: 'uint', 10: 'long', 11: 'ulong',
        12: 'float', 13: 'double', 14: 'string', 0x16: 'TypedReference', 0x18: 'IntPtr', 0x19: 'UIntPtr', 0x1c: 'object'}


def short(n):
    return n


def tname_p(tp, depth=0):
    if depth > 12 or not tp or tp + 16 > len(img):
        return '?'
    data, bits = q(tp), u(tp + 8)
    k = (bits >> 16) & 0xff
    ref = '&' if (bits >> 29) & 1 else ''
    if k in PRIM:
        return PRIM[k] + ref
    if k in (0x11, 0x12):
        return (type_name(data) if data < NT else '?') + ref
    if k == 0x1d:
        return tname_p(data, depth + 1) + '[]' + ref
    if k == 0x0f:
        return tname_p(data, depth + 1) + '*' + ref
    if k == 0x14:
        return tname_p(q(data), depth + 1) + '[,]' + ref
    if k in (0x13, 0x1e):
        return (s(struct.unpack_from('<I', md_, gpo + 16 * data + 4)[0]) if data * 16 < gps else 'T?') + ref
    if k == 0x15:
        return tname_p(q(data), depth + 1).split('`')[0] + '<' + inst(q(data + 8), depth + 1) + '>' + ref
    return '?%x' % k


def inst(ip, depth=0):
    if not ip:
        return ''
    argc, argv = u(ip), q(ip + 8)
    if argc > 32:
        return '?'
    return ', '.join(tname_p(q(argv + 8 * j), depth) for j in range(argc))


def tname(idx):
    return tname_p(q(MR['types'] + 8 * idx)) if 0 <= idx < MR['typesCount'] else '?'


def tattrs(idx):
    return u(q(MR['types'] + 8 * idx) + 8) & 0xffff


# ---- addresses
rows = list(csv.DictReader(open(U + r'\methods.tsv', encoding='utf-8'), delimiter='\t'))
m_addr = {}
by_name = {}
mi_addr = {}
for r in rows:
    n = r['type'] + '::' + r['method']
    if r['address']:
        a = int(r['address'], 16)
        m_addr.setdefault(a, n)
        mi_addr[int(r['method_index'])] = a
        by_name.setdefault(n, []).append(a)
names = dict(m_addr)
gm_spec = {}
for r in csv.DictReader(open(U + r'\generic_methods.tsv', encoding='utf-8'), delimiter='\t'):
    if r['address']:
        names.setdefault(int(r['address'], 16), r['name'])
for _f, _k, _v in ((U + r'\closure\boundary_direct.tsv', 'target', 'identification'), (U + r'\api\runtime_symbols.tsv', 'address', 'name')):
    try:                                    # developer outputs of closure_build.py / api_map_build.py: names for native code
        for r in csv.DictReader(open(_f, encoding='utf-8'), delimiter='\t'):
            names.setdefault(int(r[_k], 16), r[_v])
    except OSError:
        pass
try:
    for r in csv.DictReader(open(W + r'\generated\plt_stubs.tsv', encoding='utf-8'), delimiter='\t'):
        names.setdefault(int(r['nso_offset'], 16), 'import ' + r['symbol'])
except OSError:
    pass
fs_ = np.load(U + r'\api\fde_starts.npy'); fe_ = np.load(U + r'\api\fde_ends.npy')
fsl = fs_.tolist()


def sig(mi, with_type=True):
    m = method(mi)
    ps = []
    for k in range(m[3], m[3] + m[9]):
        ni, tok, ti = struct.unpack_from('<IIi', md_, po + 12 * k)
        ps.append(tname(ti) + ' ' + s(ni))
    st = 'static ' if m[6] & 0x10 else ''
    return '%s%s %s%s(%s)' % (st, tname(m[2]), (type_name(m[1]) + '::') if with_type else '', s(m[0]), ', '.join(ps))


_fdv = None


def const_value(field_index):
    """The constant of a literal field (an enum member), or None. Integers are stored compressed (metadata v29+)."""
    global _fdv
    if _fdv is None:
        o, n = sec(7)
        _fdv = {struct.unpack_from('<i', md_, o + 12 * k)[0]: struct.unpack_from('<ii', md_, o + 12 * k + 4) for k in range(n // 12)}
    e = _fdv.get(field_index)
    if not e or e[1] < 0:
        return None
    a = sec(8)[0] + e[1]
    kind = (u(q(MR['types'] + 8 * e[0]) + 8) >> 16) & 0xff if 'types' in MR else 0
    b0 = md_[a]
    if kind in (8, 9):                                   # int, uint: compressed
        if b0 < 0x80: v = b0
        elif b0 & 0xc0 == 0x80: v = ((b0 & 0x3f) << 8) | md_[a + 1]
        elif b0 & 0xe0 == 0xc0: v = ((b0 & 0x1f) << 24) | (md_[a + 1] << 16) | (md_[a + 2] << 8) | md_[a + 3]
        elif b0 == 0xf0: v = struct.unpack_from('<I', md_, a + 1)[0]
        elif b0 == 0xfe: v = 0xfffffffe
        else: v = 0xffffffff
        if kind == 8:                                    # signed: zig-zag style (low bit = sign)
            v = -(v >> 1) - 1 if v & 1 else v >> 1
        return v
    fmt = {2: '<B', 4: '<b', 5: '<B', 6: '<h', 7: '<H', 10: '<q', 11: '<Q'}.get(kind)
    return struct.unpack_from(fmt, md_, a)[0] if fmt else None


def fields_of(ti):
    t = types[ti]
    offs = q(MR['fieldOffsets'] + 8 * ti)
    out = []
    for k in range(t[18]):
        ni, tyi, tok = struct.unpack_from('<IiI', md_, fo + 12 * (t[8] + k))
        off = i32(offs + 4 * k) if offs else -1
        a = tattrs(tyi)
        nm = s(ni)
        if a & 0x40:
            v = const_value(t[8] + k)
            if v is not None:
                nm += ' = %d' % v if abs(v) < 4096 else ' = %#x' % (v & 0xffffffffffffffff)
        out.append((off, 'static' if a & 0x10 else ('const' if a & 0x40 else ''), tname(tyi), nm))
    return out


def show_type(ti):
    t = types[ti]
    par = tname(t[4]) if t[4] >= 0 else ''
    ifs = [tname(struct.unpack_from('<i', md_, ino + 4 * k)[0]) for k in range(t[13], t[13] + t[22])]
    print('== %s : %s%s   [type %d]' % (type_name(ti), par, (' , ' + ', '.join(ifs)) if ifs else '', ti))
    for off, st, ty, nm in fields_of(ti):
        print('   %-6s %s%s %s' % ('+%#x' % off if off >= 0 else '', st + ' ' if st else '', ty, nm))
    for mi in range(t[9], t[9] + t[16]):
        m = method(mi)
        a = mi_addr.get(mi)
        print('   %-10s slot %-5s %s' % ('%#x' % a if a else '-', m[8] if m[8] != 0xffff else '', sig(mi, False)))


def find_types(arg):
    if arg.startswith('='):
        return [i for i in range(NT) if type_name(i) == arg[1:]]
    return [i for i in range(NT) if arg.lower() in type_name(i).lower()]


# ---- metadata usage decode
def strlit(i):
    ln, di = struct.unpack_from('<Ii', md_, slo + 8 * i)
    return md_[sdo + di:sdo + di + ln].decode('utf-8', 'replace')


def methodspec(i):
    mdi, ci, mi_ = struct.unpack_from('<iii', img, MR['methodSpecs'] + 12 * i)
    m = method(mdi)
    n = type_name(m[1])
    if ci >= 0:
        n = n.split('`')[0] + '<' + inst(q(MR['genericInsts'] + 8 * ci)) + '>'
    n += '::' + s(m[0])
    if mi_ >= 0:
        n += '<' + inst(q(MR['genericInsts'] + 8 * mi_)) + '>'
    return n


def usage(v):
    if not (v & 1) or v >> 32:
        return None
    k, idx = (v >> 29) & 7, (v & 0x1ffffffe) >> 1
    try:
        if k == 1: return 'class ' + tname(idx)
        if k == 2: return 'type ' + tname(idx)
        if k == 3: return 'method ' + type_name(method(idx)[1]) + '::' + s(method(idx)[0])
        if k == 4:
            ti, fi = struct.unpack_from('<ii', md_, fro + 8 * idx)
            return 'field ' + tname(ti) + '.' + '?%d' % fi
        if k == 5: return 'str ' + repr(strlit(idx))
        if k == 6: return 'method ' + methodspec(idx)
        if k == 7: return 'fieldrva %d' % idx
    except Exception as e:
        return 'usage? %d/%d' % (k, idx)
    return None


def klass_of_usage(v):
    """type definition index for a 'class' usage of a plain (non generic) type, else -1"""
    if (v & 1) and (v >> 29) & 7 in (1, 2):
        tp = q(MR['types'] + 8 * ((v & 0x1ffffffe) >> 1))
        if (u(tp + 8) >> 16) & 0xff in (0x11, 0x12):
            return q(tp)
    return -1


def resolve(arg):
    try:
        return [int(arg, 16)]
    except ValueError:
        pass
    if arg in by_name:
        return by_name[arg]
    r = [a for n, al in by_name.items() if n.endswith(arg) for a in al]
    if not r:
        sys.exit('no method ' + arg)
    return r


VERBOSE = False
BORING = re.compile(r'(stp|ldp) x(19|2\d), x(19|2\d|30), \[sp|(str|ldr) x(19|2\d), \[sp, #0x[0-9a-f]+\]$|ldrh w9, \[x8, #0x12e\]|ldr x10, \[x8, #0xb0\]|add x10, x10, #(8|0x10)$|ldur x11, \[x10, #-8\]|cmp x11, x1$|subs x9, x9, #1|ldrsw x9, \[x10\]|add x8, x8, x9, lsl #4|ldr w8, \[x0, #0xe0\]|nop|mov w8, #1$|strb w8, \[x2\d, #0x[0-9a-f]+\]$|mov x29, sp|stp x29, x30|ldp x29, x30')


def dis(a, mx=4000):
    import capstone
    cs_ = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    j = bisect.bisect_right(fsl, a) - 1
    lo, hi = fsl[j], int(fe_[j])
    print('== %#x-%#x %s' % (lo, hi, names.get(lo, '')))
    pages = {}
    slots = {}      # reg -> description of loaded metadata slot (for interface slot annotation)
    iface = None
    n = 0
    for ins in cs_.disasm(img[lo:hi], lo):
        n += 1
        if n > mx:
            print('   ...'); break
        t = ins.mnemonic + ' ' + ins.op_str
        note = ''
        m = re.match(r'adrp (x\d+), #(0x[0-9a-f]+)', t)
        if m:
            pages[m.group(1)] = int(m.group(2), 16)
            continue
        m = re.match(r'ldr[bh]? ([xw]\d+), \[(x\d+), #(0x[0-9a-f]+|\d+)\]$', t) or re.match(r'ldr[bh]? ([xw]\d+), \[(x\d+)\]()$', t)
        if m and m.group(2) in pages:
            ad = pages[m.group(2)] + int(m.group(3) or '0', 0)
            pages.pop('x' + m.group(1)[1:], None)
            if ins.mnemonic != 'ldr':
                continue    # metadata-initialised flag bytes
            v = q(ad) if ad + 8 <= len(img) else 0
            us = usage(q(v)) if DATA_LO <= v < len(img) - 8 else usage(v)
            if us:
                slots[m.group(1)] = us
                note = '   ; &' + us
            else:
                note = '   ; [%#x]' % ad
        elif m and m.group(2) in slots and not m.group(3):
            note = '   ; ' + slots[m.group(2)]
            slots.pop(m.group(1), None)
        else:
            mm = re.match(r'(?:ldr[bh]?|mov|ldp|add|sub|ldur|ldrs[bhw]) ([xw]\d+),', t)
            if mm:
                slots.pop('x' + mm.group(1)[1:], None); pages.pop('x' + mm.group(1)[1:], None)
        m2 =re.match(r'add (x\d+), (x\d+), #(0x[0-9a-f]+|\d+)$', t)
        if m2 and m2.group(2) in pages:
            ad = pages[m2.group(2)] + int(m2.group(3), 0)
            st = img[ad:ad + 80].split(b'\0')[0]
            note = '   ; @%#x %r' % (ad, st.decode('latin1')) if len(st) >= 3 and all(32 <= ch < 127 for ch in st) else '   ; @%#x' % ad
        m3 = re.match(r'(bl|b) #(0x[0-9a-f]+)$', t)
        if m3:
            tg = int(m3.group(2), 16)
            if not lo <= tg < hi:
                nm = names.get(tg, '')
                if tg in (0x4506d0,):
                    continue
                note = '   ; ' + (nm or '?')
        if ins.mnemonic in ('strb',) and re.search(r'\[x\d+, #0x[0-9a-f]+\]$', t) and any(r in t for r in pages) and 'w8' in t:
            pass
        if not VERBOSE and not note and BORING.match(t):
            continue
        if not VERBOSE and 'Il2CppCodeGenWriteBarrier' in note or 'il2cpp_codegen_runtime_class_init' in note:
            continue
        m4 = re.match(r'mov w2, (wzr|#(0x[0-9a-f]+|\d+))$', t)
        if m4:
            pend = 0 if m4.group(1) == 'wzr' else int(m4.group(2), 0)
        if t.startswith('bl #0x459c1c'):
            note = '   ; interface-slot lookup (slot in w2 above, interface class in x1)'
        print('   %x  %s%s' % (ins.address, t, note))


def callers(a):
    w = np.frombuffer(img[:TS & ~3], dtype='<u4')
    pc = np.arange(len(w), dtype=np.int64) * 4
    isb = (w & 0x7c000000) == 0x14000000
    imm = (w & 0x3ffffff).astype(np.int64); imm = np.where(imm & (1 << 25), imm - (1 << 26), imm)
    h = np.nonzero(isb & (pc + imm * 4 == a))[0] * 4
    for x in h.tolist():
        j = bisect.bisect_right(fsl, x) - 1
        print('   %#x in %#x %s' % (x, fsl[j], names.get(fsl[j], '')))
    # address-taken (method pointers in data, e.g. delegates / vtables) are not listed


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'type':
        for arg in sys.argv[2:]:
            for ti in find_types(arg):
                show_type(ti)
    elif cmd == 'dis':
        for arg in sys.argv[2:]:
            for a in resolve(arg):
                dis(a)
    elif cmd == 'find':
        rx = re.compile(sys.argv[2])
        for a, n in sorted(m_addr.items()):
            if rx.search(n):
                print('%#x %s' % (a, n))
    elif cmd == 'got':          # got <regex>: metadata-usage pointer slots whose usage matches
        rx = re.compile(sys.argv[2])
        for ad in range(DATA_LO, min(len(img) - 8, DATA_HI), 8):
            v = q(ad)
            if DATA_LO <= v < len(img) - 8:
                us = usage(q(v))
                if us and rx.search(us):
                    print('%#x -> %#x %s' % (ad, v, us))
    elif cmd == 'callers':
        for arg in sys.argv[2:]:
            for a in resolve(arg):
                print('== callers of %#x %s' % (a, names.get(a, '')))
                callers(a)
