"""Compute the lift set (managed side of the Switch `main` NSO) and census every edge crossing its boundary.

    python closure_build.py

Inputs : work/game/exefs/main (NSO), work/unity/{methods,generic_methods,pinvoke_by_assembly}.tsv, layout.json
Outputs: work/unity/closure/*.tsv, closure_report.txt, closure.npz (function table + masks, for follow-up scripts)

Method (all static):
  functions  = .eh_frame_hdr FDE ranges; code without FDE (12 gaps: asm routines, PLT) is split at branch targets
  edges      = BL (always), B / b.cond / cbz / tbz whose target is outside the containing function
  lift range = generated code (0x0-0x3d8480, 0x1f8acb0-end) + bdwgc + libil2cpp + baselib/os shim (0x3d8480-0x477650)
The addresses in this file are those of game version 2.20 (build id in port/paths.py).
  lift set L = closure of the roots over edges that stay inside the lift range, iterated with address-taken roots:
               (a) code addresses materialised by lifted code (adrp+add, adr, GOT loads),
               (b) every relocated pointer slot in rodata/data whose value is a function start inside the lift range
  boundary   = every edge L -> outside (PLT import / direct call into non-lifted code)
"""
from _env import GAME, REPO, TOOLS, UNITY_EDITOR, W  # noqa: E402,F401
import struct, sys, os, re, collections, bisect
import numpy as np
from closure_common import *

# ---------------------------------------------------------------- region map (addresses from layout_report + string sampling)
REGIONS = [  # (name, lo, hi, in_lift_range)
    ('crt', 0x0, 0x690, True),
    ('gen_glue', 0x690, 0x3d8480, True),            # invokers, adjustor thunks, unresolved-call thunks, (reverse) pinvoke wrappers
    ('bdwgc', 0x3d8480, 0x3e6c30, True),
    ('libil2cpp', 0x3e6c30, 0x474500, True),        # includes il2cpp's zlib (0x3e6c30-0x3ec5f0)
    ('baselib_os', 0x474500, 0x477650, True),       # nn::os wrappers (semaphore/thread/timer/error) shared by il2cpp and the engine
    ('criware', 0x477650, 0x580600, False),
    ('npln_plugins', 0x580600, 0xc72600, False),    # NPLN + gRPC + protobuf + OpenSSL + ngtcp2 + WebRTC + libc++ + Nintendo plugin glue (0x594800..)
    ('engine', 0xc72600, 0x1f4ee64, False),         # Unity player (MemoryManager starts ~0xc82600), FMOD, PhysX, curl, Switch platform layer
    ('asm_nofde', 0x1f4ee64, 0x1f8721c, False),     # 229 KB without unwind info
    ('plt', 0x1f8721c, 0x1f8acb0, False),
    ('gen_methods', 0x1f8acb0, 0x48780a8, True),
]
R_LO = np.array([r[1] for r in REGIONS], dtype=np.int64)
R_NAME = [r[0] for r in REGIONS]
R_LIFT = np.array([r[3] for r in REGIONS], dtype=bool)
IL2CPP_INIT = 0x4698c0
API_CLUSTER = (0x4698c0, 0x46a320)
CODEGEN_REG_FN = 0x17d350


def fde_lsda(im):
    """{function start: lsda address} for FDEs whose CIE has an 'L' augmentation and a non-null LSDA"""
    img = im.img
    h0, _ = im.n.eh_frame_hdr
    cnt = struct.unpack_from('<I', img, h0 + 8)[0]

    def uleb(p_):
        v = sh = 0
        while True:
            b = img[p_]; p_ += 1
            v |= (b & 0x7f) << sh; sh += 7
            if not b & 0x80:
                return v, p_
    cies = {}
    out = {}
    for i in range(cnt):
        start, fde = struct.unpack_from('<ii', img, h0 + 12 + 8 * i)
        start += h0; fde += h0
        cie = fde + 4 - struct.unpack_from('<I', img, fde + 4)[0]
        if cie not in cies:
            q_ = cie + 9
            e_ = img.index(b'\0', q_)
            aug = img[q_:e_].decode()
            q_ = e_ + 1
            _, q_ = uleb(q_); _, q_ = uleb(q_); _, q_ = uleb(q_)
            lenc = None
            if aug.startswith('z'):
                _, q_ = uleb(q_)
                for ch in aug[1:]:
                    if ch == 'P':
                        enc = img[q_]; q_ += 1
                        q_ += {0: 8, 2: 2, 3: 4, 4: 8, 0xb: 4, 0xc: 8, 0xa: 2}[enc & 0xf]
                    elif ch == 'L':
                        lenc = img[q_]; q_ += 1
                    elif ch == 'R':
                        q_ += 1
            cies[cie] = lenc
        lenc = cies[cie]
        if lenc is None or lenc == 0xff:
            continue
        q_ = fde + 16
        alen, q_ = uleb(q_)
        if not alen:
            continue
        if lenc & 0xf in (0xb, 3):
            v = struct.unpack_from('<i' if lenc & 0xf == 0xb else '<I', img, q_)[0]
        else:
            v = struct.unpack_from('<q', img, q_)[0]
        if v == 0:
            continue
        if lenc & 0x70 == 0x10:
            v += q_
        out[start] = v
    return out


def region_idx(a):
    return np.searchsorted(R_LO, a, side='right') - 1


def main():
    os.makedirs(OUT, exist_ok=True)
    rep = open(OUT + r'\closure_report.txt', 'w', encoding='utf-8', newline='\n')

    def P(*a):
        s = ' '.join(str(x) for x in a)
        print(s)
        rep.write(s + '\n')

    im = Image()
    kind, name, img_of = load_tables(im)
    w = im.w
    bl_s, bl_t, b_s, b_t = im.branches()

    # ------------------------------------------------------------ function table (split FDE-less code at branch targets)
    real = ~im.isgap
    starts = im.fs[real].tolist()
    ends = im.fe[real].tolist()
    nofde = [False] * len(starts)
    gaps = [(int(a), int(b)) for a, b in zip(im.fs[im.gapcode], im.fe[im.gapcode])]
    for g0, g1 in gaps:
        ext_b = b_t[(b_s < g0) | (b_s >= g1)]            # B from outside the gap = tail call into it
        tg = np.concatenate([bl_t, ext_b])
        pts = set([g0]) | set(np.unique(tg[(tg > g0) & (tg < g1)]).tolist())
        if g0 <= 0x1f8721c < g1:   # inside the PLT every 16-byte stub is its own function
            pts = set([g0]) | {p for p in im.plt}
        pts = sorted(pts)
        for a, b in zip(pts, pts[1:] + [g1]):
            starts.append(a); ends.append(b); nofde.append(True)
    o = np.argsort(np.array(starts))
    fs = np.array(starts, dtype=np.int64)[o]
    fe = np.array(ends, dtype=np.int64)[o]
    nofde = np.array(nofde)[o]
    N = len(fs)
    size = fe - fs
    freg = region_idx(fs)
    inrange = R_LIFT[freg]
    P(f"zero-length FDEs ignored: {[hex(x) for x in im.fde_zero]}")
    P(f'functions: {int((~nofde).sum())} FDE ranges + {int(nofde.sum())} pieces of FDE-less code ({len(gaps)} gaps, {int(size[nofde].sum())} bytes); PLT stubs {len(im.plt)}')

    def fidx(a):
        a = np.asarray(a, dtype=np.int64)
        i = np.searchsorted(fs, a, side='right') - 1
        i = np.where((i >= 0) & (a < fe[np.maximum(i, 0)]), i, -1)
        return i

    # ------------------------------------------------------------ edges
    pc = np.arange(len(w), dtype=np.int64) * 4
    # conditional branches: b.cond / cbz / cbnz (imm19), tbz / tbnz (imm14)
    c19 = ((w & 0xff000010) == 0x54000000) | ((w & 0x7e000000) == 0x34000000)
    i19 = ((w >> 5) & 0x7ffff).astype(np.int64); i19 = np.where(i19 & (1 << 18), i19 - (1 << 19), i19)
    c14 = (w & 0x7e000000) == 0x36000000
    i14 = ((w >> 5) & 0x3fff).astype(np.int64); i14 = np.where(i14 & (1 << 13), i14 - (1 << 14), i14)
    cb_s = np.concatenate([pc[c19], pc[c14]])
    cb_t = np.concatenate([(pc + i19 * 4)[c19], (pc + i14 * 4)[c14]])
    e_src_pc, e_dst_pc, e_type = [], [], []
    for sp, tp, ty in ((bl_s, bl_t, 0), (b_s, b_t, 1), (cb_s, cb_t, 2)):
        si = fidx(sp)
        ok = si >= 0
        if ty:
            ok &= (tp < fs[np.maximum(si, 0)]) | (tp >= fe[np.maximum(si, 0)])
        e_src_pc.append(sp[ok]); e_dst_pc.append(tp[ok]); e_type.append(np.full(int(ok.sum()), ty, dtype=np.int8))
    e_src_pc = np.concatenate(e_src_pc); e_dst_pc = np.concatenate(e_dst_pc); e_type = np.concatenate(e_type)
    e_s = fidx(e_src_pc); e_d = fidx(e_dst_pc)
    P(f'direct edges: BL {int((e_type == 0).sum())}, B leaving its function {int((e_type == 1).sum())}, conditional branches leaving their function {int((e_type == 2).sum())}')
    bad = e_d < 0
    P(f'  edges whose target is not inside any function (dropped): {int(bad.sum())}; by type {dict(collections.Counter(e_type[bad].tolist()))}; '
      f'e.g. {[(hex(int(a)), hex(int(b))) for a, b in list(zip(e_src_pc[bad], e_dst_pc[bad]))[:6]]}')
    notstart = (~bad) & (fs[np.maximum(e_d, 0)] != e_dst_pc)
    P(f'  edges whose target is not a function start (mapped to containing function): {int(notstart.sum())}'
      + (f'  e.g. {[hex(int(x)) for x in np.unique(e_dst_pc[notstart])[:8]]}' if notstart.any() else ''))
    keep = ~bad
    e_src_pc, e_dst_pc, e_type, e_s, e_d = e_src_pc[keep], e_dst_pc[keep], e_type[keep], e_s[keep], e_d[keep]
    G = Graph(N, e_s, e_d)

    # ------------------------------------------------------------ roots
    roots = {}
    for a, k in kind.items():
        roots[a] = k
    roots.setdefault(CODEGEN_REG_FN, 's_Il2CppCodegenRegistration')
    for a in fs[(fs >= API_CLUSTER[0]) & (fs <= API_CLUSTER[1])].tolist():
        roots.setdefault(a, 'il2cpp_api_cluster')
    # static constructors / destructors registered in .init_array/.fini_array that live in the lift range
    n_init = 0
    for tag_a, tag_s in ((0x19, 0x1b), (0x1a, 0x1c)):
        t, s = im.n.dt.get(tag_a), im.n.dt.get(tag_s, 0)
        for p in range(t, t + s, 8):
            a = im.q(p)
            i = int(fidx([a])[0])
            if i >= 0 and inrange[i]:
                roots.setdefault(a, 'init_array'); n_init += 1
    root_idx = fidx(np.array(sorted(roots), dtype=np.int64))
    assert (root_idx >= 0).all()
    P(f'roots: {len(roots)} addresses; by kind {dict(collections.Counter(roots.values()))}')
    P(f'  root addresses that are function starts: {int((fs[root_idx] == np.array(sorted(roots))).sum())}/{len(roots)}; init/fini_array entries inside the lift range: {n_init}')

    # ------------------------------------------------------------ unrestricted closure (where direct calls lead if nothing is cut)
    U = G.closure(root_idx)
    # ------------------------------------------------------------ restricted closure + address-taken roots
    L = G.closure(root_idx, inrange)
    L_direct = L.copy()

    # address materialisations
    a_pc, a_t, a_k = im.adrp_refs()
    a_f = fidx(a_pc)
    ok = a_f >= 0
    a_pc, a_t, a_k, a_f = a_pc[ok], a_t[ok], a_k[ok], a_f[ok]
    # GOT extent: slots relocated contiguously below DT_PLTGOT
    relset = set(im.relr) | set(im.slot_sym)
    pltgot = im.n.dt.get(3)
    p = pltgot - 8
    while p not in relset:                       # padding between .got and .got.plt
        p -= 8
    hi = p + 8
    miss = 0
    lo = p
    while p > im.data[0] and miss < 16:
        if p in relset:
            lo = p; miss = 0
        else:
            miss += 1
        p -= 8
    got = (lo, hi)
    P(f'GOT (heuristic: run of relocated slots below DT_PLTGOT {pltgot:#x}; .dynamic is at {im.n.dynamic:#x}): {got[0]:#x}-{got[1]:#x} ({(got[1] - got[0]) // 8} slots)')
    relr_map = dict(zip(im.relr_arr.tolist(), im.relr_val.tolist()))
    # resolve adrp+ldr64 through relocated slots: pointee
    isld = a_k == 5
    slot = a_t[isld]
    pointee = np.array([relr_map.get(s, -1) for s in slot.tolist()], dtype=np.int64)
    symslot = np.array([s in im.slot_sym for s in slot.tolist()], dtype=bool)
    ingot = (slot >= got[0]) & (slot < got[1])
    pointee = np.where(ingot, pointee, -1)
    symslot &= ingot
    ld_pc, ld_f, ld_slot, ld_val, ld_sym = a_pc[isld], a_f[isld], slot, pointee, symslot

    # code pointers: (pc, func, target, how)
    tx = im.text_hi
    m = np.isin(a_k, (0, 2)) & (a_t < tx)
    cp_pc = np.concatenate([a_pc[m], ld_pc[(ld_val >= 0) & (ld_val < tx)]])
    cp_f = np.concatenate([a_f[m], ld_f[(ld_val >= 0) & (ld_val < tx)]])
    cp_t = np.concatenate([a_t[m], ld_val[(ld_val >= 0) & (ld_val < tx)]])
    cp_how = np.concatenate([np.where(a_k[m] == 0, 0, 1), np.full(int(((ld_val >= 0) & (ld_val < tx)).sum()), 2)])
    cp_ti = fidx(cp_t)
    isstart = (cp_ti >= 0) & (fs[np.maximum(cp_ti, 0)] == cp_t)
    P(f'code addresses materialised in .text (adrp+add {int((cp_how == 0).sum())}, adr {int((cp_how == 1).sum())}, pointer loaded from a GOT slot {int((cp_how == 2).sum())}); '
      f'pointing at a function start: {int(isstart.sum())}, mid-function (jump tables / labels, ignored): {int((~isstart).sum())}')

    # data slots holding code pointers
    dv = im.relr_val
    dm = (dv >= 0) & (dv < tx)
    ds_slot = im.relr_arr[dm]; ds_val = dv[dm]
    ds_ti = fidx(ds_val)
    ds_start = (ds_ti >= 0) & (fs[np.maximum(ds_ti, 0)] == ds_val)
    P(f'relocated data slots holding a .text address: {int(dm.sum())} ({int(ds_start.sum())} are function starts); of those, into the lift range: {int((ds_start & inrange[np.maximum(ds_ti, 0)]).sum())}')

    added_code, added_data = set(), set()
    # (b) catch-all: every data slot pointing at a function start in the lift range
    cand = np.unique(ds_ti[ds_start & inrange[np.maximum(ds_ti, 0)]])
    new = cand[~L[cand]]
    added_data.update(new.tolist())
    rounds = 0
    while True:
        rounds += 1
        # (a) code pointers taken by lifted code that land in the lift range
        mm = isstart & L[np.maximum(cp_f, 0)] & inrange[np.maximum(cp_ti, 0)]
        c2 = np.unique(cp_ti[mm])
        new2 = c2[~L[c2]]
        new2 = np.array([x for x in new2.tolist() if x not in added_data], dtype=np.int64)
        added_code.update(new2.tolist())
        allnew = np.concatenate([new, new2]).astype(np.int64)
        if len(allnew) == 0:
            break
        L |= G.closure(allnew, inrange)
        new = np.empty(0, dtype=np.int64)
    P(f'address-taken roots added: {len(added_data)} via data slots (not already in direct closure), {len(added_code)} via code materialisation; fixpoint after {rounds} rounds')

    def breakdown(mask, title):
        P(title)
        P(f'  {"region":14} {"range":24} {"funcs":>8} {"bytes":>10}   (of region: funcs, bytes)')
        for r, (nm, lo_, hi_, lift) in enumerate(REGIONS):
            sel = freg == r
            n_, b_ = int((mask & sel).sum()), int(size[mask & sel].sum())
            tn, tb = int(sel.sum()), int(size[sel].sum())
            if n_ or lift:
                P(f'  {nm:14} {lo_:#9x}-{hi_:#9x}   {n_:8} {b_:10}   ({tn}, {tb}; {100 * b_ / max(tb, 1):.1f}%)')
        P(f'  {"TOTAL":14} {"":24} {int(mask.sum()):8} {int(size[mask].sum()):10}')

    P('')
    breakdown(U, 'UNRESTRICTED closure over direct branches from the roots (nothing cut; shows where direct calls lead):')
    P('')
    breakdown(L_direct, 'LIFT SET, direct edges only (closure confined to the lift range):')
    P('')
    breakdown(L, 'LIFT SET, final (direct closure + address-taken callbacks/data-table pointers inside the lift range):')
    dead = inrange & ~L
    P('')
    breakdown(dead, 'functions inside the lift range NOT reached (dead, or reachable only through computed pointers we cannot see):')

    # ------------------------------------------------------------ control flow we cannot follow statically
    P('')
    P('INDIRECT CONTROL FLOW inside the lift set (not followed by the closure):')
    wf = fidx(pc)
    inL = (wf >= 0) & L[np.maximum(wf, 0)]
    isblr = (w & 0xfffffc1f) == 0xd63f0000
    isbr = (w & 0xfffffc1f) == 0xd61f0000
    for rn in R_NAME:
        sel = inL & (freg[np.maximum(wf, 0)] == R_NAME.index(rn))
        if sel.any():
            P(f'  {rn:12} BLR (indirect call) {int((isblr & sel).sum()):8}   BR (indirect jump / tail call / switch) {int((isbr & sel).sum()):7}')
    # exception landing pads: FDEs with an LSDA
    lsda = fde_lsda(im)
    haslsda = np.array([int(a) in lsda for a in fs.tolist()])
    P(f'  functions with an LSDA (C++ exception landing pads / cleanups entered by the unwinder, not by a branch): '
      f'{int((haslsda & L).sum())} of {int(L.sum())} lift-set functions; by region '
      + ', '.join(f'{rn}={int((haslsda & L & (freg == i)).sum())}' for i, rn in enumerate(R_NAME) if (haslsda & L & (freg == i)).any()))

    # ------------------------------------------------------------ save function table
    fkind = np.array([roots.get(int(a), '') for a in fs.tolist()], dtype=object)
    np.savez_compressed(OUT + r'\closure.npz', fs=fs, fe=fe, lift=L, lift_direct=L_direct, unrestricted=U, inrange=inrange, region=freg, nofde=nofde)
    why = {}
    for i in added_data:
        why[i] = 'data_slot'
    for i in added_code:
        why[i] = 'addr_taken_in_code'
    with open(OUT + r'\lift_functions.tsv', 'w', encoding='utf-8', newline='\n') as f:
        f.write('start\tend\tsize\tregion\ttable_kind\troot_reason\timage\tname\n')
        for i in np.nonzero(L)[0].tolist():
            a = int(fs[i])
            rr = 'table' if a in kind else roots.get(a, '') or why.get(i, '')
            f.write(f'{a:#x}\t{int(fe[i]):#x}\t{int(size[i])}\t{R_NAME[freg[i]]}\t{kind.get(a, "")}\t{rr}\t{img_of.get(a, "")}\t{name.get(a, "")}\n')
    with open(OUT + r'\roots_addr_taken.tsv', 'w', encoding='utf-8', newline='\n') as f:
        f.write('function\tsize\tregion\thow\texample_ref\n')
        ex = {}
        for s_, t_ in zip(ds_slot[ds_start].tolist(), ds_ti[ds_start].tolist()):
            ex.setdefault(t_, f'slot {s_:#x}')
        exc = {}
        mm = isstart & L[np.maximum(cp_f, 0)]
        for p_, t_ in zip(cp_pc[mm].tolist(), cp_ti[mm].tolist()):
            exc.setdefault(t_, f'code {p_:#x}')
        for i in sorted(added_data | added_code):
            f.write(f'{int(fs[i]):#x}\t{int(size[i])}\t{R_NAME[freg[i]]}\t{why[i]}\t{ex.get(i) if why[i] == "data_slot" else exc.get(i)}\n')
    with open(OUT + r'\unreached_in_range.tsv', 'w', encoding='utf-8', newline='\n') as f:
        f.write('start\tsize\tregion\n')
        for i in np.nonzero(dead)[0].tolist():
            f.write(f'{int(fs[i]):#x}\t{int(size[i])}\t{R_NAME[freg[i]]}\n')

    # ------------------------------------------------------------ boundary: edges L -> outside lift range
    out = L[e_s] & ~inrange[e_d]
    o_spc, o_tpc, o_s, o_d, o_ty = e_src_pc[out], e_dst_pc[out], e_s[out], e_d[out], e_type[out]
    isplt = freg[o_d] == R_NAME.index('plt')
    P('')
    P(f'BOUNDARY: {int(out.sum())} direct edges leave the lift set: {int(isplt.sum())} to PLT stubs (imports), {int((~isplt).sum())} into non-lifted code in main')
    # sanity: does anything in the lift range call out of L into the lift range? (no by construction)

    # ---- 2a imports
    sdk = {l.split('\t')[5].rstrip('\n') for l in open(W + r'\work\re\sdk.syms.tsv', encoding='utf-8') if l.startswith('exp')}
    sub = {l.split('\t')[5].rstrip('\n') for l in open(W + r'\work\re\subsdk0.syms.tsv', encoding='utf-8') if l.startswith('exp')}
    LIBM = set('acos acosf asin asinf atan atan2 atan2f atanf cbrt cbrtf ceil ceilf cos cosf cosh coshf exp exp2 exp2f expf expm1 fabs fabsf floor floorf fmod fmodf frexp frexpf hypot hypotf ldexp ldexpf log log10 log10f log1p log2 log2f logf lround lroundf modf modff nearbyint nearbyintf pow powf remainder remainderf rint rintf round roundf scalbn scalbnf sin sincos sincosf sinf sinh sinhf sqrt sqrtf tan tanf tanh tanhf trunc truncf fmax fmaxf fmin fminf copysign copysignf nan nanf lrint lrintf llrint llround fegetenv fesetenv fegetround fesetround feclearexcept feraiseexcept fetestexcept'.split())

    def comps(n):
        if not n.startswith('_Z'):
            return None
        m = re.match(r'_ZNK?', n)
        i = len(m.group(0)) if m else 2
        if n.startswith('_ZT') or n.startswith('_ZG'):
            mm = re.match(r'_Z(TV|TI|TS|GV|Tv\w+?_|Th\w+?_)N?', n); i = len(mm.group(0)) if mm else 3
        outc = []
        while True:
            mm = re.match(r'(\d+)', n[i:])
            if not mm:
                break
            ln = int(mm.group(1)); i += len(mm.group(1)); outc.append(n[i:i + ln]); i += ln
        return outc

    def group(n):
        c = comps(n)
        if c and c[0] == 'nn' and len(c) > 1:
            return 'nn::' + c[1]
        if n.startswith(('_Zn', '_Zd', '__cxa', '_Unwind', '__gxx', '_ZSt', '_ZNSt', '_ZNKSt', '_ZTVSt', '_ZTISt', '_ZTVN10__cxxabiv', '_ZTIN10__cxxabiv', '__dynamic_cast')) or (c and c[0] in ('std', '__cxxabiv1')):
            return 'C++ runtime (libc++/libc++abi/unwind)'
        if c:
            return 'c++ other: ' + c[0]
        if n.startswith('_Z'):
            return 'c++ other'
        if n.startswith('nvn'):
            return 'nvn'
        if n in LIBM:
            return 'libm'
        if n.startswith('_nnmusl') or n.startswith('nnmusl'):
            return 'nnmusl'
        return 'libc'

    plt_i = R_NAME.index('plt')
    all_plt = freg[e_d] == plt_i
    tot_cs = collections.Counter(e_dst_pc[all_plt].tolist())
    lift_cs = collections.Counter()
    lift_cs_reg = collections.defaultdict(collections.Counter)
    for t_, s_ in zip(o_tpc[isplt].tolist(), o_s[isplt].tolist()):
        lift_cs[t_] += 1
        lift_cs_reg[t_][R_NAME[freg[s_]]] += 1
    imp_func = {s['name'] for s in im.n.syms if s['shndx'] == 0 and s['name']}
    plt_syms = {nm for nm, _ in im.plt.values()}
    # data imports reached through GOT/relocated slots by lifted code
    data_imp = collections.Counter()
    data_imp_all = collections.Counter()
    for f_, s_, isym in zip(ld_f.tolist(), ld_slot.tolist(), ld_sym.tolist()):
        if isym:
            ty, sym, add = im.slot_sym[s_]
            data_imp_all[sym['name']] += 1
            if L[f_]:
                data_imp[sym['name']] += 1
    rows = []
    for stub, (nm, slot_) in im.plt.items():
        rows.append((group(nm), nm, 'sdk' if nm in sdk else 'subsdk0' if nm in sub else '?', lift_cs.get(stub, 0), tot_cs.get(stub, 0), dict(lift_cs_reg.get(stub, {}))))
    rows.sort(key=lambda r: (r[0], -r[3], r[1]))
    with open(OUT + r'\boundary_imports.tsv', 'w', encoding='utf-8', newline='\n') as f:
        f.write('group\tsymbol\tmodule\tcallsites_lift\tcallsites_main_total\tlift_callsites_by_region\tneeded_by_lift\n')
        for g, nm, mod, c, t, br in rows:
            f.write(f'{g}\t{nm}\t{mod}\t{c}\t{t}\t{";".join(f"{k}={v}" for k, v in sorted(br.items()))}\t{"yes" if c else "no"}\n')
        for nm, c in sorted(data_imp_all.items()):
            f.write(f'DATA via GOT: {group(nm)}\t{nm}\t{"sdk" if nm in sdk else "subsdk0" if nm in sub else "main/?"}\t{data_imp.get(nm, 0)}\t{c}\t\t{"yes" if data_imp.get(nm) else "no"}\n')
    used = [r for r in rows if r[3]]
    P('')
    P(f'2a. IMPORTS (PLT): main has {len(imp_func)} undefined dynsyms, {len(im.plt)} PLT stubs ({len(plt_syms)} distinct symbols).')
    P(f'    lift set calls {len(used)} distinct imported functions through the PLT ({100 * len(used) / len(im.plt):.1f}% of PLT symbols), {sum(r[3] for r in used)} call sites '
      f'(all of main: {sum(tot_cs.values())} PLT call sites)')
    gcount = collections.defaultdict(lambda: [0, 0, 0])
    for g, nm, mod, c, t, br in rows:
        gcount[g][1] += 1
        if c:
            gcount[g][0] += 1; gcount[g][2] += c
    P(f'    {"group":44} {"used/total syms":>16} {"lift call sites":>16}')
    for g in sorted(gcount, key=lambda g: -gcount[g][2]):
        u, t, c = gcount[g]
        P(f'    {g:44} {u:>7}/{t:<8} {c:>16}')
    for g in sorted(gcount, key=lambda g: -gcount[g][2]):
        us = [(nm, c) for gg, nm, mod, c, t, br in rows if gg == g and c]
        if us:
            P(f'    [{g}] ' + ', '.join(f'{nm}({c})' for nm, c in us))
    # by caller region
    regc = collections.Counter()
    regs = collections.defaultdict(set)
    for stub, br in lift_cs_reg.items():
        for k, v in br.items():
            regc[k] += v; regs[k].add(im.plt[stub][0])
    P('    PLT call sites / distinct symbols by calling region: ' + ', '.join(f'{k}: {regc[k]}/{len(regs[k])}' for k in sorted(regc)))
    only_gen = (regs.get('gen_methods', set()) | regs.get('gen_glue', set())) - regs.get('libil2cpp', set()) - regs.get('bdwgc', set()) - regs.get('baselib_os', set())
    P(f'    imported functions called directly from GENERATED code: {len(regs.get("gen_methods", set()) | regs.get("gen_glue", set()))}: '
      + ', '.join(sorted(regs.get('gen_methods', set()) | regs.get('gen_glue', set()))))
    P(f'    imported DATA symbols loaded by lifted code via relocated slots: ' + (', '.join(f'{k}({v})' for k, v in sorted(data_imp.items())) or 'none'))

    # symbol-bound data relocations (ABS64/GLOB_DAT): vtable / typeinfo slots that need an imported symbol at load time
    symrel = collections.defaultdict(lambda: [0, 0])
    lift_pages = None
    P('    (symbol-bound data relocations are attributed after the page-ownership pass, see 2d)')

    # ---- 2b direct calls into non-lifted main code
    pinv = set()
    for l in open(UNITY + r'\pinvoke_by_assembly.tsv', encoding='utf-8'):
        p_ = l.rstrip('\n').split('\t')
        if len(p_) >= 3:
            pinv.add((p_[0], p_[1].split('+')[-1].split('/')[-1], p_[2]))
    exp_name = {}
    for s in im.n.syms:
        if s['shndx'] != 0 and s['name'] and s['value'] < tx:
            exp_name[s['value']] = s['name']
    def is_pinvoke(a):
        nm_ = name.get(a)
        if not nm_ or a not in img_of or '::' not in nm_:
            return False
        ty_, me_ = nm_.rsplit('::', 1)
        return (img_of[a], ty_.split('+')[-1], me_) in pinv

    d_s, d_d, d_spc, d_tpc = o_s[~isplt], o_d[~isplt], o_spc[~isplt], o_tpc[~isplt]
    bytgt = collections.defaultdict(list)
    for s_, t_, sp_ in zip(d_s.tolist(), d_d.tolist(), d_spc.tolist()):
        bytgt[t_].append((s_, sp_))
    eng_i = R_NAME.index('engine')
    # closure of each cut target over the full graph (what lifting it instead of cutting would drag in)
    full_allowed = None
    rows = []
    union_support = np.zeros(N, bool)
    for t_ in sorted(bytgt):
        callers = bytgt[t_]
        cl = G.closure(np.array([t_]))
        cl_out = cl & ~L
        nf, nb = int(cl_out.sum()), int(size[cl_out].sum())
        regs_hit = collections.Counter(R_NAME[r] for r in freg[cl_out].tolist())
        regs_hit.pop('plt', None)
        cal_gen = [c for c in callers if R_NAME[freg[c[0]]] in ('gen_methods', 'gen_glue', 'crt')]
        cal_rt = [c for c in callers if R_NAME[freg[c[0]]] not in ('gen_methods', 'gen_glue', 'crt')]
        images = collections.Counter(img_of.get(int(fs[c[0]]), kind.get(int(fs[c[0]]), 'gen:untabled')) for c in cal_gen)
        cnames = []
        for c in callers:
            a = int(fs[c[0]])
            nmc = name.get(a) or f'{R_NAME[freg[c[0]]]}:{a:#x}'
            if nmc not in cnames:
                cnames.append(nmc)
        pv = [name[int(fs[c[0]])] for c in cal_gen if is_pinvoke(int(fs[c[0]]))]
        is_pinv = bool(pv)
        ta = int(fs[t_])
        ident = exp_name.get(ta, '')
        if not ident and cal_gen and not cal_rt:
            ident = 'native for ' + (pv[0] if pv else cnames[0] + ' (caller not a listed P/Invoke)')
        rows.append(dict(t=t_, addr=ta, size=int(size[t_]), region=R_NAME[freg[t_]], n=len(callers), n_gen=len(cal_gen), n_rt=len(cal_rt),
                         images=images, callers=cnames, pinv=is_pinv, ident=ident, cl_funcs=nf, cl_bytes=nb, cl_regs=regs_hit,
                         eng=regs_hit.get('engine', 0), rt_regs=collections.Counter(R_NAME[freg[c[0]]] for c in cal_rt)))
    with open(OUT + r'\boundary_direct.tsv', 'w', encoding='utf-8', newline='\n') as f:
        f.write('target\tsize\tregion\tcallsites\tfrom_generated\tfrom_runtime\tcaller_images\tcaller_is_pinvoke_method\tidentification\t'
                'own_closure_funcs\town_closure_bytes\town_closure_regions\tFLAG\texample_callers\n')
        for r in rows:
            flag = ''
            if r['region'] == 'engine':
                flag = 'GENERATED->ENGINE' if r['n_gen'] else 'RUNTIME->ENGINE (cut)'
            elif r['eng']:
                flag = 'native reaches engine code'
            f.write(f"{r['addr']:#x}\t{r['size']}\t{r['region']}\t{r['n']}\t{r['n_gen']}\t{r['n_rt']}\t"
                    f"{';'.join(f'{k}={v}' for k, v in r['images'].most_common())}\t{'yes' if r['pinv'] else ''}\t{r['ident']}\t"
                    f"{r['cl_funcs']}\t{r['cl_bytes']}\t{';'.join(f'{k}={v}' for k, v in r['cl_regs'].most_common())}\t{flag}\t{' | '.join(r['callers'][:4])}\n")
    P('')
    P(f'2b. DIRECT CALLS INTO NON-LIFTED CODE: {len(d_s)} call sites, {len(rows)} distinct targets')
    P(f'    {"target region":14} {"targets":>8} {"sites":>7} {"from generated":>15} {"from runtime":>13}  caller images (generated call sites)')
    for rn in R_NAME:
        rr = [r for r in rows if r['region'] == rn]
        if not rr:
            continue
        imgs = collections.Counter()
        for r in rr:
            imgs.update(r['images'])
        P(f'    {rn:14} {len(rr):8} {sum(r["n"] for r in rr):7} {sum(r["n_gen"] for r in rr):15} {sum(r["n_rt"] for r in rr):13}  ' + ', '.join(f'{k}={v}' for k, v in imgs.most_common(12)))
    gen_targets = [r for r in rows if r['n_gen']]
    P(f'    targets called from generated code: {len(gen_targets)}; of those whose caller is a method listed in pinvoke_by_assembly.tsv: {sum(1 for r in gen_targets if r["pinv"])}')
    np_ = [r for r in gen_targets if not r['pinv']]
    P(f'    generated-code targets whose callers are NOT listed P/Invoke methods: {len(np_)}')
    for r in np_[:60]:
        P(f"      {r['addr']:#x} [{r['region']}] sites={r['n']} callers: {' | '.join(r['callers'][:3])}")
    ge = [r for r in rows if r['region'] == 'engine' and r['n_gen']]
    P(f'    PLAN CHECK - generated code calling directly into the engine region: {len(ge)} targets')
    for r in ge:
        P(f"      {r['addr']:#x} sites={r['n_gen']} {r['ident']} callers: {' | '.join(r['callers'][:3])}")
    re_ = [r for r in rows if r['n_rt']]
    P(f'    targets called from runtime/GC/baselib (non-generated lifted code): {len(re_)}')

    def hint(t_):
        """imports called directly by the function + first strings it references"""
        mm_ = (e_s == t_)
        imps = []
        for d_ in e_dst_pc[mm_].tolist():
            if d_ in im.plt and im.plt[d_][0] not in imps:
                imps.append(im.plt[d_][0])
        strs = []
        sel_ = (a_f == t_) & (a_k == 0) & (a_t >= im.rodata[0]) & (a_t < im.rodata[1])
        for x_ in a_t[sel_].tolist()[:6]:
            s_ = im.img[x_:x_ + 48].split(b'\0')[0]
            if len(s_) >= 4 and all(32 <= c < 127 for c in s_):
                strs.append(s_.decode())
        return ('imports: ' + ','.join(imps[:6]) if imps else '') + (' strings: ' + repr(strs[:3]) if strs else '')
    with open(OUT + r'\boundary_runtime_to_engine.tsv', 'w', encoding='utf-8', newline='\n') as f:
        f.write('target\tsize\texport_name\tcallsites\tcaller_regions\tcaller_functions\town_closure_funcs\town_closure_bytes\town_closure_engine_funcs\thint\n')
        for r in re_:
            cf = sorted({int(fs[c[0]]) for c in bytgt[r['t']]})
            f.write(f"{r['addr']:#x}\t{r['size']}\t{exp_name.get(r['addr'], '')}\t{r['n']}\t{';'.join(f'{k}={v}' for k, v in r['rt_regs'].items())}\t"
                    f"{','.join(hex(x) for x in cf[:12])}{'...' if len(cf) > 12 else ''}\t{r['cl_funcs']}\t{r['cl_bytes']}\t{r['eng']}\t{hint(r['t'])}\n")
            r['hint'] = hint(r['t'])
    for r in re_:
        P(f"      {r['addr']:#x} [{r['region']}] size={r['size']} sites={r['n_rt']} ({dict(r['rt_regs'])}){' +gen ' + str(r['n_gen']) if r['n_gen'] else ''} "
          f"{('export ' + r['ident']) if r['addr'] in exp_name else ''} own-closure={r['cl_funcs']} funcs/{r['cl_bytes']} B {dict(r['cl_regs'])} {r['hint']}")

    # ---- 2c what the cut targets would drag in
    P('')
    P('2c. SUPPORT CODE / LEAK ANALYSIS (closure of cut targets over the full graph, excluding functions already in the lift set)')
    for label, sel in (('all cut targets', rows), ('targets called from runtime/GC/baselib', re_), ('targets called from generated code (P/Invoke natives)', gen_targets)):
        if not sel:
            continue
        cl = G.closure(np.array([r['t'] for r in sel])) & ~L
        rc = collections.Counter(); rb = collections.Counter()
        for i in np.nonzero(cl)[0].tolist():
            rc[R_NAME[freg[i]]] += 1; rb[R_NAME[freg[i]]] += int(size[i])
        P(f'    {label}: {len(sel)} targets -> {int(cl.sum())} extra functions, {int(size[cl].sum())} bytes: ' + ', '.join(f'{k}={rc[k]} funcs/{rb[k]} B' for k in R_NAME if rc[k]))
    # first engine functions entered from the P/Invoke natives' closure (why CRI/NPLN natives touch the engine)
    ncl = G.closure(np.array([r['t'] for r in gen_targets])) & ~L
    em = ncl[e_s] & (freg[e_s] != eng_i) & (freg[e_d] == eng_i)
    ec = collections.Counter(e_d[em].tolist())
    P('    engine functions entered directly from CRI/NPLN native code reachable from P/Invoke targets (top 12 by call sites): '
      + ', '.join(f'{int(fs[t]):#x}{"=" + exp_name[int(fs[t])] if int(fs[t]) in exp_name else ""}({c})' for t, c in ec.most_common(12))
      + f'; {len(ec)} distinct')
    leak = U & ~L
    P(f'    unrestricted closure minus lift set = {int(leak.sum())} functions / {int(size[leak].sum())} bytes of non-managed code reachable by direct calls')
    # attribute engine leak to first-hop helpers: for each cut target, engine functions in its closure
    P('    cut targets whose own closure enters the engine region, ranked by engine functions reached (these are the helpers to replace with host functions):')
    for r in sorted([r for r in rows if r['eng']], key=lambda r: -r['eng'])[:40]:
        P(f"      {r['addr']:#x} [{r['region']}] {r['ident'] or ''} sites={r['n']} (gen {r['n_gen']}, rt {r['n_rt']}) -> engine funcs {r['eng']}, total {r['cl_funcs']} funcs/{r['cl_bytes']} B")
    P(f"    cut targets whose closure enters the engine: {sum(1 for r in rows if r['eng'])} of {len(rows)}")

    # ---- address-taken edges leaving the lift set (lifted code materialising the address of a non-lifted function)
    mm = isstart & L[np.maximum(cp_f, 0)] & ~inrange[np.maximum(cp_ti, 0)]
    at = collections.defaultdict(list)
    for p_, f_, t_, h_ in zip(cp_pc[mm].tolist(), cp_f[mm].tolist(), cp_ti[mm].tolist(), cp_how[mm].tolist()):
        at[t_].append((p_, f_, h_))
    # (b') data tables: from every non-GOT data address referenced by lifted code, walk consecutive relocated slots (max 32)
    #      and collect code pointers that leave the lift range
    ref_l = a_t[L[a_f] & np.isin(a_k, (0, 1, 5)) & (a_t >= im.rodata[0])]
    ref_l = np.unique(ref_l[(ref_l < got[0]) | (ref_l >= got[1])])
    tab = collections.defaultdict(set)      # target function idx -> slots
    fs_set = dict(zip(fs.tolist(), range(N)))
    for base in ref_l.tolist():
        q_ = base & ~7
        for k_ in range(32):
            v_ = relr_map.get(q_ + 8 * k_)
            if v_ is None:
                break
            ti_ = fs_set.get(v_)
            if ti_ is not None and v_ < tx and not inrange[ti_]:
                tab[ti_].add(q_ + 8 * k_)
    with open(OUT + r'\boundary_addr_taken.tsv', 'w', encoding='utf-8', newline='\n') as f:
        f.write('target\tsize\tregion\texport_name\trefs\thow\texample_ref_pc\texample_ref_function\n')
        for t_ in sorted(at):
            l_ = at[t_]
            hows = collections.Counter(('adrp+add', 'adr', 'got/slot load')[h] for _, _, h in l_)
            a = int(fs[l_[0][1]])
            f.write(f'{int(fs[t_]):#x}\t{int(size[t_])}\t{R_NAME[freg[t_]]}\t{exp_name.get(int(fs[t_]), "")}\t{len(l_)}\t{";".join(f"{k}={v}" for k, v in hows.items())}\t{l_[0][0]:#x}\t{name.get(a) or R_NAME[freg[l_[0][1]]] + f":{a:#x}"}\n')
        for t_ in sorted(tab):
            sl = sorted(tab[t_])
            f.write(f'{int(fs[t_]):#x}\t{int(size[t_])}\t{R_NAME[freg[t_]]}\t{exp_name.get(int(fs[t_]), "")}\t{len(sl)}\tdata table slot (heuristic walk)\t{sl[0]:#x}\t\n')
    P('')
    P(f'    non-lifted function addresses materialised by lifted code (function pointers passed/called indirectly): {len(at)} targets, {sum(len(v) for v in at.values())} refs; '
      f'by region {dict(collections.Counter(R_NAME[freg[t]] for t in at))}')
    for t_ in sorted(at)[:40]:
        l_ = at[t_]
        a = int(fs[l_[0][1]])
        P(f'      {int(fs[t_]):#x} [{R_NAME[freg[t_]]}] {exp_name.get(int(fs[t_]), "")} refs={len(l_)} e.g. from {name.get(a) or R_NAME[freg[l_[0][1]]] + f":{a:#x}"} @ {l_[0][0]:#x}')

    P(f'    code pointers to non-lifted functions stored in data tables that lifted code references (heuristic slot walk): {len(tab)} targets; '
      f'by region {dict(collections.Counter(R_NAME[freg[t]] for t in tab))}')
    for t_ in sorted(tab)[:40]:
        P(f'      {int(fs[t_]):#x} [{R_NAME[freg[t_]]}] {exp_name.get(int(fs[t_]), "")} slots {[hex(x) for x in sorted(tab[t_])[:4]]}')

    # ------------------------------------------------------------ 2d data references
    P('')
    P('2d. DATA REFERENCES (ADRP-based; GOT loads resolved one level to the pointee)')
    in_got = (ld_slot >= got[0]) & (ld_slot < got[1])
    # build unified (func, addr) list: adrp+add, adrp+mem (non-GOT), GOT pointee; kind 4 = page only
    notgot_ld = np.ones(len(a_pc), bool)
    idx_ld = np.nonzero(isld)[0]
    notgot_ld[idx_ld[in_got]] = False
    da_f = np.concatenate([a_f[notgot_ld & (a_k != 2) & (a_k != 3)], ld_f[in_got & (ld_val >= 0)]])
    da_t = np.concatenate([a_t[notgot_ld & (a_k != 2) & (a_k != 3)], ld_val[in_got & (ld_val >= 0)]])
    isdata = da_t >= im.rodata[0]
    da_f, da_t = da_f[isdata], da_t[isdata]
    lf = L[da_f]
    fr = freg[da_f]
    bss0, bss1 = im.bss
    segs = [('rodata', im.rodata[0], im.rodata[1]), ('data', im.data[0], bss0), ('bss', bss0, bss1 + 0x1000)]
    P(f'    segments: rodata {im.rodata[0]:#x}-{im.rodata[1]:#x}, data {im.data[0]:#x}-{bss0:#x}, bss {bss0:#x}-{bss1:#x}')
    page = da_t >> 12
    npg = int(page.max()) + 1
    pl = np.bincount(page[lf], minlength=npg)
    pn = np.bincount(page[~lf], minlength=npg)
    for nm, lo_, hi_ in segs:
        s0, s1 = lo_ >> 12, min(hi_ >> 12, npg - 1) + 1
        a_l, a_n = pl[s0:s1], pn[s0:s1]
        only_l = int(((a_l > 0) & (a_n == 0)).sum()); only_n = int(((a_l == 0) & (a_n > 0)).sum()); mixed = int(((a_l > 0) & (a_n > 0)).sum())
        P(f'    {nm:7} pages referenced only by lifted code: {only_l}, only by non-lifted code: {only_n}, by both: {mixed}; '
          f'lift refs: {int(a_l.sum())} (into mixed pages: {int(a_l[(a_n > 0)].sum())})')
    # contiguous runs of page ownership in data+bss
    with open(OUT + r'\data_page_runs.tsv', 'w', encoding='utf-8', newline='\n') as f:
        f.write('start\tend\tsegment\towner\tpages\tlift_refs\tnonlift_refs\n')
        P('    page-ownership runs in data/bss (owner by who references the page; "mixed" pages sit at ownership seams or hold shared globals):')
        for nm, lo_, hi_ in segs[1:]:
            s0, s1 = lo_ >> 12, min(hi_ >> 12, npg - 1) + 1
            runs = []
            for pg in range(s0, s1):
                a, b = int(pl[pg]), int(pn[pg])
                if a == 0 and b == 0:
                    continue
                ow = 'lift' if b == 0 else 'nonlift' if a == 0 else 'mixed'
                if runs and runs[-1][2] == ow:
                    runs[-1][1] = pg + 1; runs[-1][3] += 1; runs[-1][4] += a; runs[-1][5] += b
                else:
                    runs.append([pg, pg + 1, ow, 1, a, b])
            for r in runs:
                f.write(f'{r[0] << 12:#x}\t{r[1] << 12:#x}\t{nm}\t{r[2]}\t{r[3]}\t{r[4]}\t{r[5]}\n')
            # compress for the report: merge runs into super-runs by dominant owner
            big = [r for r in runs if r[3] >= 8]
            P(f'      {nm}: {len(runs)} runs; runs of >= 8 pages:')
            for r in big:
                P(f'        {r[0] << 12:#x}-{r[1] << 12:#x} {r[2]:8} pages={r[3]} lift_refs={r[4]} nonlift_refs={r[5]}')
    # symbol-bound data relocations by ownership of the page they sit in
    own = lambda a: 'lift' if pl[a >> 12] and not pn[a >> 12] else 'nonlift' if pn[a >> 12] and not pl[a >> 12] else 'mixed' if pl[a >> 12] else 'unreferenced'
    sr = collections.defaultdict(collections.Counter)
    for slot_, (ty_, sym_, add_) in im.slot_sym.items():
        if ty_ == 0x402:
            continue
        sr[sym_['name']][own(slot_) if (slot_ >> 12) < npg else 'unreferenced'] += 1
    with open(OUT + r'\data_symbol_relocs.tsv', 'w', encoding='utf-8', newline='\n') as f:
        f.write('symbol\tslots\tin_lift_only_pages\tin_mixed_pages\tin_nonlift_pages\tin_unreferenced_pages\n')
        for nm_, c_ in sorted(sr.items(), key=lambda kv: -sum(kv[1].values())):
            f.write(f"{nm_}\t{sum(c_.values())}\t{c_['lift']}\t{c_['mixed']}\t{c_['nonlift']}\t{c_['unreferenced']}\n")
    lp = [(nm_, c_['lift'], c_['mixed']) for nm_, c_ in sr.items() if c_['lift'] or c_['mixed']]
    P(f'    symbol-bound data relocations (ABS64/GLOB_DAT, e.g. vtable slots): {sum(sum(c.values()) for c in sr.values())} slots, {len(sr)} symbols; '
      f'in pages referenced by lifted code (lift-only, mixed): ' + ', '.join(f'{a}({b},{c})' for a, b, c in sorted(lp, key=lambda x: -(x[1] + x[2]))[:25]))
    # exact shared addresses: targets referenced by both lifted and non-lifted code (data/bss only, fully resolved refs)
    exact_l = collections.Counter(da_t[lf & (da_t >= im.data[0])].tolist())
    exact_n = collections.Counter(da_t[~lf & (da_t >= im.data[0])].tolist())
    shared = sorted(set(exact_l) & set(exact_n))
    # who (non-lifted region) shares
    nl_reg = collections.defaultdict(collections.Counter)
    for t_, r_ in zip(da_t[~lf].tolist(), fr[~lf].tolist()):
        if t_ in exact_l:
            nl_reg[t_][R_NAME[r_]] += 1
    l_reg = collections.defaultdict(collections.Counter)
    for t_, r_ in zip(da_t[lf].tolist(), fr[lf].tolist()):
        if t_ in exact_n:
            l_reg[t_][R_NAME[r_]] += 1
    with open(OUT + r'\data_shared_addrs.tsv', 'w', encoding='utf-8', newline='\n') as f:
        f.write('address\tsegment\tlift_refs\tlift_ref_regions\tnonlift_refs\tnonlift_ref_regions\n')
        for t_ in shared:
            seg = 'data' if t_ < bss0 else 'bss'
            f.write(f'{t_:#x}\t{seg}\t{exact_l[t_]}\t{";".join(f"{k}={v}" for k, v in l_reg[t_].items())}\t{exact_n[t_]}\t{";".join(f"{k}={v}" for k, v in nl_reg[t_].items())}\n')
    P(f'    exact data/bss addresses referenced by BOTH lifted and non-lifted code: {len(shared)} '
      f'(data {sum(1 for t in shared if t < bss0)}, bss {sum(1 for t in shared if t >= bss0)}); lift refs to them: {sum(exact_l[t] for t in shared)}')
    byreg = collections.Counter()
    for t_ in shared:
        for k in nl_reg[t_]:
            byreg[k] += 1
    P(f'      shared addresses by the non-lifted region that also references them: {dict(byreg)}')
    bylreg = collections.Counter()
    for t_ in shared:
        for k in l_reg[t_]:
            bylreg[k] += 1
    P(f'      shared addresses by the lifted region that references them: {dict(bylreg)}')
    genshared = [t for t in shared if any(k.startswith('gen') for k in l_reg[t])]
    P(f'      shared addresses referenced from GENERATED code: {len(genshared)}')
    for t_ in genshared[:40]:
        P(f'        {t_:#x} lift {dict(l_reg[t_])} nonlift {dict(nl_reg[t_])}')
    # lift refs by region x segment
    P('    lift-set data refs by referencing region and segment (refs / distinct pages):')
    for rn in R_NAME:
        sel = lf & (fr == R_NAME.index(rn))
        if not sel.any():
            continue
        parts = []
        for nm, lo_, hi_ in segs:
            s2 = sel & (da_t >= lo_) & (da_t < hi_)
            parts.append(f'{nm} {int(s2.sum())}/{len(np.unique(page[s2]))}')
        P(f'      {rn:12} ' + ', '.join(parts))

    # ------------------------------------------------------------ 3. reverse direction: non-lifted code -> lift set
    P('')
    P('3. ENGINE -> MANAGED: direct branches from non-lifted code into lift-set functions')
    rev = ~L[e_s] & L[e_d]
    r_s, r_d, r_ty, r_spc = e_s[rev], e_d[rev], e_type[rev], e_src_pc[rev]
    rv = collections.defaultdict(list)
    for s_, t_, ty_, sp_ in zip(r_s.tolist(), r_d.tolist(), r_ty.tolist(), r_spc.tolist()):
        rv[t_].append((s_, ty_, sp_))
    # address-taken from non-lifted code
    mm = isstart & ~L[np.maximum(cp_f, 0)] & L[np.maximum(cp_ti, 0)] & (cp_f >= 0)
    rat = collections.defaultdict(list)
    for p_, f_, t_ in zip(cp_pc[mm].tolist(), cp_f[mm].tolist(), cp_ti[mm].tolist()):
        rat[t_].append((p_, f_))
    alltg = sorted(set(rv) | set(rat))
    with open(OUT + r'\engine_to_lift.tsv', 'w', encoding='utf-8', newline='\n') as f:
        f.write('target\tsize\tregion\ttable_kind\tname\tin_api_cluster\tcall_sites\tdistinct_caller_funcs\ttail_or_cond_branches\taddr_taken_refs\tcaller_regions\tcallers_in_lift_range_but_unreached\texample_caller_pcs\n')
        for t_ in alltg:
            l_ = rv.get(t_, [])
            a = int(fs[t_])
            cr = collections.Counter(R_NAME[freg[s]] for s, _, _ in l_)
            cr.update({'addr:' + R_NAME[freg[f_]]: 1 for _, f_ in rat.get(t_, [])})
            dead_callers = sum(1 for s, _, _ in l_ if inrange[s])
            f.write(f'{a:#x}\t{int(size[t_])}\t{R_NAME[freg[t_]]}\t{kind.get(a, "")}\t{name.get(a, "")}\t{"yes" if API_CLUSTER[0] <= a <= API_CLUSTER[1] else ""}\t'
                    f'{sum(1 for _, ty, _ in l_ if ty == 0)}\t{len({s for s, _, _ in l_})}\t{sum(1 for _, ty, _ in l_ if ty)}\t{len(rat.get(t_, []))}\t'
                    f'{";".join(f"{k}={v}" for k, v in cr.most_common())}\t{dead_callers}\t{",".join(hex(sp) for _, _, sp in l_[:3])}\n')
    # only count callers that are truly outside the lift range (callers inside the range but unreached are dead managed/runtime code)
    def ext_callers(t_):
        return [c for c in rv.get(t_, []) if not inrange[c[0]]]
    ext = [t for t in alltg if ext_callers(t) or any(not inrange[f_] for _, f_ in rat.get(t, []))]
    ext_call = [t for t in alltg if ext_callers(t)]
    P(f'    lift-set functions branched to from outside the lift set: {len(rv)}; address-taken from outside: {len(rat)}; union {len(alltg)}')
    P(f'    ... counting only callers outside the lift RANGE (i.e. excluding unreached code inside the range): called {len(ext_call)}, called-or-address-taken {len(ext)}')
    byr = collections.Counter(R_NAME[freg[t]] for t in ext_call)
    P(f'    called targets by region: {dict(byr)}')
    callers_reg = collections.Counter()
    for t in ext_call:
        for s, _, _ in ext_callers(t):
            callers_reg[R_NAME[freg[s]]] += 1
    P(f'    call sites by calling region: {dict(callers_reg)}')
    for rn in ('libil2cpp', 'bdwgc', 'baselib_os', 'gen_glue', 'gen_methods', 'crt'):
        tg = [t for t in ext_call if R_NAME[freg[t]] == rn]
        if not tg:
            continue
        inapi = [t for t in tg if API_CLUSTER[0] <= fs[t] <= API_CLUSTER[1]]
        P(f'    [{rn}] {len(tg)} targets' + (f' ({len(inapi)} inside the il2cpp API cluster {API_CLUSTER[0]:#x}-{API_CLUSTER[1]:#x}, {len(tg) - len(inapi)} outside)' if rn == 'libil2cpp' else ''))
        for t in tg:
            cs = ext_callers(t)
            cr = collections.Counter(R_NAME[freg[s]] for s, _, _ in cs)
            a = int(fs[t])
            P(f'      {a:#x} size={int(size[t])} sites={len(cs)} {dict(cr)} {name.get(a, "")}')
    # functions inside the lift range that the lift set never reaches but non-lifted code calls (engine-side users of GC/baselib/COMDAT code)
    um = ~inrange[e_s] & inrange[e_d] & ~L[e_d]
    uc = collections.defaultdict(list)
    for s_, t_ in zip(e_s[um].tolist(), e_d[um].tolist()):
        uc[t_].append(s_)
    P(f'    functions inside the lift range that are NOT in the lift set but are called from non-lifted code: {len(uc)} (needed only if their callers are re-hosted)')
    with open(OUT + r'\inrange_unreached_called_from_outside.tsv', 'w', encoding='utf-8', newline='\n') as f:
        f.write('target\tsize\tregion\tcall_sites\tcaller_regions\tcaller_functions\n')
        for t_ in sorted(uc):
            cr = collections.Counter(R_NAME[freg[s]] for s in uc[t_])
            cf = sorted({int(fs[s]) for s in uc[t_]})
            f.write(f'{int(fs[t_]):#x}\t{int(size[t_])}\t{R_NAME[freg[t_]]}\t{len(uc[t_])}\t{";".join(f"{k}={v}" for k, v in cr.items())}\t{",".join(hex(x) for x in cf[:8])}\n')
            P(f'      {int(fs[t_]):#x} [{R_NAME[freg[t_]]}] size={int(size[t_])} sites={len(uc[t_])} {dict(cr)} callers {[hex(x) for x in cf[:4]]}')
    api_funcs = fs[(fs >= API_CLUSTER[0]) & (fs <= API_CLUSTER[1])]
    called_api = {int(fs[t]) for t in ext_call if API_CLUSTER[0] <= fs[t] <= API_CLUSTER[1]}
    P(f'    il2cpp API cluster functions never called from non-lifted code: {[hex(int(a)) for a in api_funcs if int(a) not in called_api]}')
    P(f'    lowest / highest libil2cpp function called from non-lifted code inside the cluster: {min(called_api):#x} / {max(called_api):#x}')
    P(f'    il2cpp API cluster: {len(api_funcs)} functions; called from non-lifted code: {len(called_api)}; never called from outside: {len(api_funcs) - len(called_api)}')
    refs = [l.strip() for l in open(UNITY + r'\unityplayer_eleven_il2cpp_refs.txt') if l.strip()]
    P(f'    cross-check: Windows UnityPlayer binds {len(refs)} il2cpp_* names; Switch engine directly calls {sum(1 for t in ext_call if R_NAME[freg[t]] == "libil2cpp")} libil2cpp functions '
      f'({len(called_api)} in the API cluster)')
    rep.close()


if __name__ == '__main__':
    main()
