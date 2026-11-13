"""Generate the icall signature table (icall_sigs.inc) for the AAPCS64 -> Win64 thunks of icall.c.

    python gen_icall_sigs.py <main.bin flat image> <global-metadata.dat> <out.inc> [icalls_managed_used_main.txt]

Every method flagged InternalCall in the metadata gets "Ns.Type::Method" -> signature (token language in icall.c).
Overloads of one name must agree (the engine registers one function per bare name); disagreeing ones are dropped
and reported. The optional list of icalls that managed code resolves is used for the coverage report only.
"""
import struct, sys, os, collections
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
from il2cpp_link import Meta

img_path, meta_path, out_path = sys.argv[1:4]
used_path = sys.argv[4] if len(sys.argv) > 4 else None
img = open(img_path, 'rb').read()
m = Meta(meta_path)
d = m.d
MR = 0x5f94058                                     # g_MetadataRegistration (work/unity/anchors_report.txt)
mr = struct.unpack_from('<16Q', img, MR)
types_cnt, types_tab = mr[6], mr[7]
sizes_cnt, sizes_tab = mr[12], mr[13]
q = lambda a: struct.unpack_from('<Q', img, a)[0]
u = lambda a: struct.unpack_from('<I', img, a)[0]
sec = lambda i: struct.unpack_from('<II', d, 8 + 8 * i)
param_off = sec(10)[0]
field_off = sec(11)[0]


def type_ptr(index):
    return q(types_tab + 8 * index)


def tinfo(tp):
    data, bits = q(tp), u(tp + 8)
    return data, bits & 0xffff, (bits >> 16) & 0xff, (bits >> 29) & 1


INTS = {2: 1, 3: 2, 4: 1, 5: 1, 6: 2, 7: 2, 8: 4, 9: 4, 10: 8, 11: 8, 0x18: 8, 0x19: 8}
PTRS = {0x0e, 0x0f, 0x12, 0x14, 0x1b, 0x1c, 0x1d, 0x16 + 100}


class Unsupported(Exception):
    pass


def typedef_is_enum(ti):
    return (m.types[ti][24] >> 1) & 1


def typedef_is_value(ti):
    return m.types[ti][24] & 1


def fields_of(ti):
    t = m.types[ti]
    for i in range(t[8], t[8] + t[18]):
        ni, typ, tok = struct.unpack_from('<IiI', d, field_off + 12 * i)
        yield typ


def layout(tp, inst=None, depth=0):
    """-> (size, align, leaves) of a value of Il2CppType tp as stored in a field / passed by value.
    leaves: flat list of 'f', 'd' or 'x' (anything else)."""
    if depth > 12:
        raise Unsupported('depth')
    data, attrs, k, byref = tinfo(tp)
    if byref:
        return 8, 8, ['x']
    if k in INTS:
        return INTS[k], INTS[k], ['x']
    if k == 12:
        return 4, 4, ['f']
    if k == 13:
        return 8, 8, ['d']
    if k in (0x0e, 0x0f, 0x12, 0x14, 0x1b, 0x1c, 0x1d):
        return 8, 8, ['x']
    if k == 0x11:                                   # VALUETYPE: data = type definition index
        return struct_layout(data, None, depth)
    if k == 0x15:                                   # GENERICINST: Il2CppGenericClass {type, class_inst, method_inst}
        gdef = q(data)
        gdata, _, gk, _ = tinfo(gdef)
        if not typedef_is_value(gdata):
            return 8, 8, ['x']
        ci = q(data + 8)
        argc, argv = u(ci), q(ci + 8)
        return struct_layout(gdata, [q(argv + 8 * i) for i in range(argc)], depth)
    if k == 0x13:                                   # VAR: generic parameter index -> position in the instantiation
        if inst is None:
            raise Unsupported('open generic')
        gp_off = sec(12)[0]
        num = struct.unpack_from('<H', d, gp_off + 16 * data + 12)[0]
        return layout(inst[num], None, depth + 1)
    raise Unsupported(f'type kind {k:#x}')


def struct_layout(ti, inst, depth):
    if typedef_is_enum(ti):
        return layout(type_ptr(m.types[ti][5]), None, depth + 1)
    size = align = 0
    leaves = []
    explicit = (m.types[ti][7] & 0x18) == 0x10      # TYPE_ATTRIBUTE_EXPLICIT_LAYOUT
    for ftyp in fields_of(ti):
        fp = type_ptr(ftyp)
        _, fattrs, _, _ = tinfo(fp)
        if fattrs & 0x10:                           # static
            continue
        s, a, lv = layout(fp, inst, depth + 1)
        size = (size + a - 1) & ~(a - 1)
        size += s
        align = max(align, a)
        leaves += lv
    if align == 0:
        size, align, leaves = 1, 1, ['x']
    size = (size + align - 1) & ~(align - 1)
    if inst is None and ti < sizes_cnt:             # the runtime's own size (handles explicit layout, packing)
        real = u(q(sizes_tab + 8 * ti)) - 16
        if real > 0 and real != size:
            size = real
            leaves = ['x'] * max(1, len(leaves))    # layout differs from the natural one: no HFA claims
    if explicit:
        leaves = ['x'] * max(1, len(leaves))
    return size, align, leaves


def token(tp, is_ret):
    data, attrs, k, byref = tinfo(tp)
    if byref:
        return 'q' if is_ret else 'i'
    if k == 1:
        if not is_ret:
            raise Unsupported('void parameter')
        return 'v'
    if k in INTS:
        return {1: 'b', 2: 'h', 4: 'w', 8: 'q'}[INTS[k]] if is_ret else 'i'
    if k == 12:
        return 'f'
    if k == 13:
        return 'd'
    if k in (0x0e, 0x0f, 0x12, 0x14, 0x1b, 0x1c, 0x1d):
        return 'q' if is_ret else 'i'
    if k == 0x11 and typedef_is_enum(data):
        return token(type_ptr(m.types[data][5]), is_ret)
    size, align, leaves = layout(tp)
    if len(leaves) <= 4 and len(set(leaves)) == 1 and leaves[0] in 'fd' and size == len(leaves) * (4 if leaves[0] == 'f' else 8):
        return f'H{len(leaves)}{leaves[0]}'
    if k == 0x15 and len(leaves) == 1 and leaves[0] == 'x' and size == 8:
        pass
    if size <= 16:
        return f'A{size}.'
    return 'X' if is_ret else 'i'


sigs = collections.defaultdict(set)
errors = collections.Counter()
struct_tokens = collections.Counter()
n_icalls = 0
for ti in range(m.ntypes):
    t = m.types[ti]
    tname = m.type_name(ti).replace('+', '/')
    for mi in range(t[9], t[9] + t[16]):
        ni, decl, rti, pstart, gc, tok, fl, ifl, slot, pc = m.methods[mi]
        if not ifl & 0x1000:
            continue
        n_icalls += 1
        name = f'{tname}::{m.s(ni)}'
        try:
            s = token(type_ptr(rti), True) + ':'
            if not fl & 0x10:                       # instance method: this
                s += 'i'
            for pi in range(pstart, pstart + pc):
                pn, ptok, ptyp = struct.unpack_from('<IIi', d, param_off + 12 * pi)
                tk = token(type_ptr(ptyp), False)
                s += tk
                if tk[0] in 'AH':
                    struct_tokens[tk] += 1
            if s[0] in 'AHX':
                struct_tokens['ret ' + s.split(':')[0]] += 1
            sigs[name].add(s)
        except Unsupported as e:
            errors[str(e)] += 1
            sigs[name].add(None)

out = []
conflicts = []
for name in sorted(sigs, key=lambda s: s.encode()):
    v = sigs[name]
    if len(v) == 1 and None not in v:
        out.append((name, next(iter(v))))
    elif None not in v and len({s for s in v}) > 1:
        conflicts.append((name, sorted(v)))
with open(out_path, 'w', newline='\n') as f:
    f.write('// GENERATED by gen_icall_sigs.py from the metadata - do not edit.\n')
    for name, s in out:
        f.write(f'    {{"{name}", "{s}"}},\n')
print(f'icall signatures: {n_icalls} InternalCall methods, {len(sigs)} names, {len(out)} with a signature, '
      f'{len(conflicts)} dropped (overloads disagree), {sum(errors.values())} unsupported {dict(errors)}')
print('by-value aggregate tokens:', dict(struct_tokens.most_common(40)))
for name, v in conflicts[:20]:
    print('  conflict', name, v)
if used_path:
    have = {n for n, _ in out}
    used = {l.split('(')[0].strip() for l in open(used_path, encoding='utf-8') if l.strip()}
    missing = sorted(used - have)
    print(f'managed-used icalls: {len(used)} bare names, {len(used & have)} with a signature, {len(missing)} without')
    for n in missing[:40]:
        print('  no signature:', n)
