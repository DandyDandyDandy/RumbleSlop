"""Layout analysis of il2cpp-generated code inside the NSO, after il2cpp_link.py.

    python il2cpp_layout.py <nso> <outdir>

Uses .eh_frame_hdr as the function-start table, classifies every function, finds runtime anchors by
string xrefs (adrp+add), and spot-checks method entry points. Writes <outdir>/layout_report.txt, regions.tsv.
"""
import struct, sys, os, json, bisect, collections
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from il2cpp_link import load_image


def main():
    nso_path, outdir = sys.argv[1:3]
    n, img, _, _, _ = load_image(nso_path)
    out = open(os.path.join(outdir, 'layout_report.txt'), 'w', encoding='utf-8', newline='\n')

    def P(*a):
        s = ' '.join(str(x) for x in a)
        print(s)
        out.write(s + '\n')
    text_hi = n.text[1]
    layout = json.load(open(os.path.join(outdir, 'layout.json')))
    H = lambda s: int(s, 16) if isinstance(s, str) else s

    # ---- function starts from eh_frame_hdr
    h0, h1 = n.eh_frame_hdr
    ver, e1, e2, e3 = img[h0:h0 + 4]
    assert (ver, e1, e2, e3) == (1, 0x1b, 3, 0x3b), (ver, e1, e2, e3)
    cnt = struct.unpack_from('<I', img, h0 + 8)[0]
    tab = np.frombuffer(bytes(img[h0 + 12:h0 + 12 + 8 * cnt]), dtype='<i4').reshape(-1, 2)
    starts = np.unique(tab[:, 0].astype(np.int64) + h0)
    P(f'eh_frame_hdr: {cnt} FDEs, {len(starts)} distinct starts, range {starts[0]:#x}-{starts[-1]:#x}')
    starts_l = starts.tolist()
    ends = starts_l[1:] + [text_hi]
    size = dict(zip(starts_l, (e - s for s, e in zip(starts_l, ends))))
    startset = set(starts_l)

    # ---- classify entries
    kind = {}
    img_of = {}
    with open(os.path.join(outdir, 'methods.tsv'), encoding='utf-8') as f:
        next(f)
        for line in f:
            p = line.rstrip('\n').split('\t')
            if p[3]:
                a = int(p[3], 16)
                kind[a] = 'method'
                img_of[a] = p[0]
    ngen_adj = 0
    with open(os.path.join(outdir, 'generic_methods.tsv'), encoding='utf-8') as f:
        next(f)
        for line in f:
            p = line.rstrip('\n').split('\t')
            if p[2]:
                kind.setdefault(int(p[2], 16), 'generic')
            if p[3]:
                kind.setdefault(int(p[3], 16), 'generic_adjustor')
    q = lambda a: struct.unpack_from('<Q', img, a)[0]
    cr = layout['code_registration']['fields']
    for nm, k, c in (('invokerPointers', 'invoker', 'invokerPointersCount'), ('reversePInvokeWrappers', 'reverse_pinvoke', 'reversePInvokeWrapperCount'),
                     ('unresolvedVirtualCallPointers', 'unresolved_call', 'unresolvedIndirectCallCount'),
                     ('unresolvedInstanceCallPointers', 'unresolved_call', 'unresolvedIndirectCallCount'),
                     ('unresolvedStaticCallPointers', 'unresolved_call', 'unresolvedIndirectCallCount'),
                     ('genericAdjustorThunks', 'generic_adjustor', None)):
        t = H(cr[nm])
        c = H(cr[c]) if c else H(layout['tables']['genericAdjustorThunks']['count'])
        for i in range(c):
            a = q(t + 8 * i)
            if a:
                kind.setdefault(a, k)
    mods = H(cr['codeGenModules'])
    for i in range(H(cr['codeGenModulesCount'])):
        rec = q(mods + 8 * i)
        ac, ap = struct.unpack_from('<I', img, rec + 0x18)[0], q(rec + 0x20)
        for j in range(ac):
            kind.setdefault(q(ap + 16 * j + 8), 'adjustor')
    cnts = collections.Counter(kind.values())
    P('il2cpp table entries by kind (distinct addresses):', dict(cnts))
    miss = [a for a in kind if a not in startset]
    P(f'entries that are FDE starts: {len(kind) - len(miss)}/{len(kind)}  (missing {len(miss)}; e.g. {[hex(x) for x in sorted(miss)[:8]]})')
    P(f'entries 16-byte aligned: {sum(1 for a in kind if a % 16 == 0)}/{len(kind)}')

    # ---- prologue / boundary spot check on every entry
    w = np.frombuffer(bytes(img[:text_hi & ~3]), dtype='<u4')
    def is_term(x):  # ret, br, b, udf/zero padding, nop, brk
        return x == 0xd65f03c0 or (x & 0xfffffc1f) == 0xd61f0000 or (x & 0xfc000000) == 0x14000000 or x == 0 or x == 0xd503201f or (x & 0xffe0001f) == 0xd4200000
    ent = np.array(sorted(a for a in kind if a >= 4), dtype=np.int64)
    prev = w[ent // 4 - 1]
    okp = sum(1 for x in prev.tolist() if is_term(x))
    # walk back over padding to the last real instruction
    bad = 0
    for a in ent.tolist():
        i = a // 4 - 1
        while i > 0 and (w[i] == 0 or w[i] == 0xd503201f):
            i -= 1
        x = int(w[i])
        if not is_term(x) and (x & 0xfc000000) != 0x94000000:  # bl noreturn tail also seen
            bad += 1
    P(f'entries whose preceding instruction is ret/b/br/padding: {okp}/{len(ent)}; after skipping padding, non-terminator (and non-bl) predecessors: {bad}')
    first = w[ent // 4]
    cat = collections.Counter()
    for x in first.tolist():
        if (x & 0xffc003e0) == 0xa98003e0 or (x & 0xffc003e0) == 0xa90003e0 or (x & 0x7fc003e0) == 0x29800 << 8:
            cat['stp ..,[sp..]'] += 1
        elif (x & 0xff0003ff) == 0xd10003ff:
            cat['sub sp,sp,#'] += 1
        elif (x & 0xffe00fe0) == 0xf8000fe0 or (x & 0xffe00fe0) == 0xfc000fe0:
            cat['str reg,[sp,#-N]!'] += 1
        elif x == 0xd65f03c0:
            cat['ret'] += 1
        elif (x & 0xfc000000) == 0x14000000:
            cat['b (tail)'] += 1
        elif (x & 0x9f000000) == 0x90000000:
            cat['adrp'] += 1
        elif x == 0 or x == 0xffffffff:
            cat['ZERO/INVALID'] += 1
        else:
            cat['other'] += 1
    P('first instruction of entries:', dict(cat))

    # ---- regions: classify every FDE function; merge runs
    def region_kind(a):
        return kind.get(a)
    # label each function start by nearest known il2cpp entry cluster: build runs where consecutive il2cpp-entry functions
    # are separated by < GAP bytes of unknown functions
    il = sorted(a for a in kind)
    GAP = 0x8000
    runs = []
    s = p = il[0]
    for a in il[1:]:
        if a - p > GAP:
            runs.append((s, p))
            s = a
        p = a
    runs.append((s, p))
    P(f'\nil2cpp entry clusters (gap > {GAP:#x} splits):')
    tot_il = 0
    with open(os.path.join(outdir, 'regions.tsv'), 'w', newline='\n') as rt:
        rt.write('start\tend\tbytes\tfunctions\tin_tables\tkinds\n')
        for s, e in runs:
            i0 = bisect.bisect_left(starts_l, s)
            i1 = bisect.bisect_right(starts_l, e)
            end = ends[i1 - 1] if i1 > i0 else e
            fns = starts_l[i0:i1]
            kc = collections.Counter(kind.get(a, 'not_in_tables') for a in fns)
            nb = collections.Counter()
            for a in fns:
                nb[kind.get(a, 'not_in_tables')] += size[a]
            tot_il += end - s
            P(f'  {s:#9x}-{end:#9x}  {end - s:>9} bytes  {len(fns):>6} funcs  ' + ', '.join(f'{k}:{v} ({nb[k]}b)' for k, v in kc.most_common()))
            rt.write(f'{s:#x}\t{end:#x}\t{end - s}\t{len(fns)}\t{len(fns) - kc["not_in_tables"]}\t{dict(kc)}\n')
    P(f'total bytes in il2cpp clusters: {tot_il} ({tot_il / text_hi:.1%} of .text {text_hi})')

    # bytes by kind using FDE sizes
    bk = collections.Counter()
    for a, k in kind.items():
        bk[k] += size.get(a, 0)
    P('bytes by entry kind (FDE start to next FDE start):', dict(bk), 'sum', sum(bk.values()))
    bi = collections.Counter()
    ci = collections.Counter()
    for a, im in img_of.items():
        bi[im] += size.get(a, 0)
        ci[im] += 1
    with open(os.path.join(outdir, 'image_bytes.tsv'), 'w', newline='\n') as o:
        o.write('image\tcode_methods\tbytes\n')
        for im, b in bi.most_common():
            o.write(f'{im}\t{ci[im]}\t{b}\n')

    # ---- xrefs: adrp+add pairs over .text
    ww = w.astype(np.int64)
    is_adrp = (ww & 0x9f000000) == 0x90000000
    idx = np.nonzero(is_adrp[:-1])[0]
    a0 = ww[idx]
    a1 = ww[idx + 1]
    rd = a0 & 31
    immlo = (a0 >> 29) & 3
    immhi = (a0 >> 5) & 0x7ffff
    imm = (immhi << 2) | immlo
    imm = np.where(imm & (1 << 20), imm - (1 << 21), imm)
    page = ((idx * 4) & ~0xfff) + (imm << 12)
    is_add = ((a1 & 0xffc00000) == 0x91000000) & ((a1 & 31) == rd) & (((a1 >> 5) & 31) == rd)
    tgt = page + ((a1 >> 10) & 0xfff)
    xa = idx[is_add] * 4
    xt = tgt[is_add]
    order = np.argsort(xt, kind='stable')
    xt_s, xa_s = xt[order], xa[order]

    def xrefs(target):
        i0 = np.searchsorted(xt_s, target, 'left')
        i1 = np.searchsorted(xt_s, target, 'right')
        return xa_s[i0:i1].tolist()

    def func_of(a):
        i = bisect.bisect_right(starts_l, a) - 1
        return starts_l[i]

    P('\nruntime anchors (string -> adrp/add xref -> containing function [FDE start, size]):')
    anchors = {}
    for sname in ['IL2CPP Root Domain', 'global-metadata.dat', 'il2cpp_init', 'mscorlib.dll', 'Metadata/global-metadata.dat', 'Finalizer',
                  'InternalArray__', 'il2cpp::vm::Runtime::Init', 'Unresolved P/Invoke', 'System.Runtime.Remoting', 'No AOT code was generated',
                  'ExecutionEngineException', 'Il2CppDomain', 'IL2CPP_METADATA', 'Data/Managed', 'GC_INITIAL_HEAP_SIZE', 'GC Warning',
                  'Too many heap sections', 'CRI ', 'CriWare', 'CRIWARE', 'Unity Version', '2022.3.30f1', 'libil2cpp', 'UnityPlayer',
                  'il2cpp/Metadata', 'Resources/mscorlib.dll-resources.dat', 'il2cpp_data', 'Attempting to call method', 'Managed/Metadata']:
        needle = sname.encode()
        pos = n.rodata[0]
        hits = []
        while len(hits) < 6:
            pos = img.find(needle, pos)
            if pos < 0:
                break
            # start of the C string
            st = pos
            while img[st - 1] != 0 and pos - st < 200:
                st -= 1
            full = bytes(img[st:img.index(b'\0', pos)])[:80]
            xs = sorted(set(xrefs(st)) | (set(xrefs(pos)) if pos != st else set()))
            hits.append((st, full, xs))
            pos += 1
        for st, full, xs in hits:
            fs = sorted({func_of(x) for x in xs})
            P(f'  {sname!r}: str@{st:#x} {full!r} xrefs={[hex(x) for x in xs[:6]]} funcs={[(hex(f), size[f]) for f in fs[:6]]}')
            anchors.setdefault(sname, []).extend(fs)

    P('\nxrefs to registration structs:')
    for nm, a in (('CodeRegistration', H(layout['code_registration']['address'])), ('MetadataRegistration', H(layout['metadata_registration']['address'])),
                  ('codeGenModules array', H(layout['code_gen_modules_array']))):
        xs = xrefs(a)
        P(f'  {nm} @{a:#x}: xrefs {[hex(x) for x in xs]} in funcs {[(hex(func_of(x)), size[func_of(x)]) for x in xs]}')

    # bl targets from the registration function
    try:
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        xs = xrefs(H(layout['code_registration']['address']))
        if xs:
            f = func_of(xs[0])
            P(f'\ndisassembly of registration function {f:#x}:')
            for ins in md.disasm(bytes(img[f:f + min(size[f], 96)]), f):
                P(f'    {ins.address:#x}: {ins.mnemonic} {ins.op_str}')
    except ImportError:
        pass

    # ---- non-il2cpp map: big picture of .text by 1 MiB: fraction of functions in il2cpp tables
    P('\n.text map per 2 MiB (functions / in il2cpp tables):')
    step = 0x202300
    for b in range(0, text_hi, step):
        i0, i1 = bisect.bisect_left(starts_l, b), bisect.bisect_left(starts_l, b + step)
        fns = starts_l[i0:i1]
        k = sum(1 for a in fns if a in kind)
        P(f'  {b:#9x}: {len(fns):>6} funcs, {k:>6} il2cpp ({(k / len(fns) if fns else 0):.0%})')
    out.close()


if __name__ == '__main__':
    main()
