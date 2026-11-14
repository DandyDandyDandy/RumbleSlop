// Internal calls: the engine registers (name, Win64 function) pairs; lifted managed code calls them with AAPCS64
// arguments. Each pair gets a synthetic guest address registered in the LIFTED runtime's table; its thunk marshals
// by the icall's signature (icall_sigs.inc, generated from the metadata by gen_icall_sigs.py).
//
// Signature strings: return token, ':' , argument tokens.
//   v void | b h w q integer of 1/2/4/8 bytes (q also pointers; arguments always use i) | i integer/pointer argument
//   f float | d double | A<n>. aggregate of n <= 16 bytes passed in integer registers | H<k><f|d> homogeneous
//   aggregate of k floats/doubles | X (return only) aggregate returned through x8
// Win64 side: aggregates of 1/2/4/8 bytes travel by value in a slot, all others by pointer to a copy; aggregate
// returns that are not 1/2/4/8 bytes use a hidden first argument.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api.h"

typedef struct { const char *name; const char *sig; } IcallSig;
static const IcallSig g_sigs[] = {
#include "icall_sigs.inc"
    {0, 0}};
#define NSIGS (sizeof g_sigs / sizeof g_sigs[0] - 1)

typedef struct Icall {
    char *name;
    void *host;
    const char *sig;
    uint64_t guest;
    uint64_t calls;
    int traced;                 // BR_ICALL_FILTER matched this name
    int plugin_event;           // takes a native callback pointer (IssuePluginEvent family)
    struct Icall *next;
} Icall;
#define ICALL_BUCKETS 16384
static Icall *g_icalls[ICALL_BUCKETS];
static SRWLOCK g_icall_lock = SRWLOCK_INIT;
static uint64_t g_n_icalls, g_n_nosig;

static uint32_t str_hash(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}
static Icall *icall_find(const char *name, size_t n) {
    for (Icall *e = g_icalls[str_hash(name, n) & (ICALL_BUCKETS - 1)]; e; e = e->next)
        if (!strncmp(e->name, name, n) && !e->name[n]) return e;
    return NULL;
}
static const char *sig_find(const char *name) {
    size_t lo = 0, hi = NSIGS;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = strcmp(g_sigs[mid].name, name);
        if (!r) return g_sigs[mid].sig;
        if (r < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

// ---- AAPCS64 -> Win64
typedef struct { Cpu *c; int ngr, nvr; uint64_t stack; } AArgs;
static uint64_t next_gr(AArgs *a) {
    if (a->ngr < 8) return a->c->x[a->ngr++];
    uint64_t v = LD64(a->stack);
    a->stack += 8;
    return v;
}
static uint64_t next_vr(AArgs *a) {
    if (a->nvr < 8) return (uint64_t)a->c->v[a->nvr++];
    uint64_t v = LD64(a->stack);
    a->stack += 8;
    return v;
}
static int by_value(unsigned size) { return size == 1 || size == 2 || size == 4 || size == 8; }

void rt_icall_marshal(Cpu *c, void *host, const char *sig) {
    uint64_t slots[40], copies[24][4], retbuf[8] = {0};
    int ns = 0, ncopy = 0;
    AArgs a = {c, 0, 0, c->sp};
    const char *p = sig;
    char rk = *p++;
    unsigned rsize = 0, rcount = 0;
    char relem = 0;
    if (rk == 'A') { rsize = (unsigned)strtoul(p, (char **)&p, 10); p++; }
    else if (rk == 'H') { rcount = (unsigned)(*p++ - '0'); relem = *p++; rsize = rcount * (relem == 'f' ? 4 : 8); }
    if (rk == 'X') slots[ns++] = c->x[8];
    else if ((rk == 'A' || rk == 'H') && !by_value(rsize)) slots[ns++] = (uint64_t)(uintptr_t)retbuf;
    if (*p == ':') p++;
    while (*p) {
        if (ns >= 38 || ncopy >= 24) rt_fatal_c(c, "icall thunk: too many arguments in signature %s", sig);
        switch (*p++) {
        case 'i': slots[ns++] = next_gr(&a); break;
        case 'f': case 'd': slots[ns++] = next_vr(&a); break;
        case 'A': {
            unsigned size = (unsigned)strtoul(p, (char **)&p, 10), words = (size + 7) / 8;
            p++;
            uint64_t *cp = copies[ncopy++];
            cp[0] = cp[1] = 0;
            if (a.ngr + (int)words > 8) a.ngr = 8;       // an aggregate is never split between registers and stack
            for (unsigned i = 0; i < words; i++) cp[i] = next_gr(&a);
            slots[ns++] = by_value(size) ? cp[0] : (uint64_t)(uintptr_t)cp;
            break;
        }
        case 'H': {
            unsigned k = (unsigned)(*p++ - '0');
            char el = *p++;
            uint64_t *cp = copies[ncopy++];
            cp[0] = cp[1] = cp[2] = cp[3] = 0;
            unsigned esz = el == 'f' ? 4 : 8;
            if (a.nvr + (int)k <= 8) {
                for (unsigned i = 0; i < k; i++) { uint64_t v = (uint64_t)c->v[a.nvr++]; memcpy((char *)cp + i * esz, &v, esz); }
            } else {
                a.nvr = 8;                                 // on the stack as memory, rounded up to 8 bytes
                unsigned bytes = k * esz;
                memcpy(cp, (void *)(uintptr_t)a.stack, bytes);
                a.stack += (bytes + 7) & ~7u;
            }
            slots[ns++] = by_value(k * esz) ? cp[0] : (uint64_t)(uintptr_t)cp;
            break;
        }
        default: rt_fatal_c(c, "icall thunk: bad signature %s", sig);
        }
    }
    while (ns < 4) slots[ns++] = 0;
    uint64_t x0 = 0, rax = rt_win64_call(host, slots, (uint64_t)ns, &x0);
    switch (rk) {
    case 'v': case 'X': break;
    case 'b': c->x[0] = rax & 0xff; break;
    case 'h': c->x[0] = rax & 0xffff; break;
    case 'w': c->x[0] = rax & 0xffffffffu; break;
    case 'q': c->x[0] = rax; break;
    case 'f': c->v[0] = (uint32_t)x0; break;
    case 'd': c->v[0] = x0; break;
    case 'A':
        if (by_value(rsize)) c->x[0] = rsize == 8 ? rax : rax & ((1ull << (rsize * 8)) - 1);
        else { c->x[0] = retbuf[0]; c->x[1] = retbuf[1]; }
        break;
    case 'H': {
        if (by_value(rsize)) retbuf[0] = rax;
        unsigned esz = relem == 'f' ? 4 : 8;
        for (unsigned i = 0; i < rcount; i++) { uint64_t v = 0; memcpy(&v, (char *)retbuf + i * esz, esz); c->v[i] = v; }
        break;
    }
    default: rt_fatal_c(c, "icall thunk: bad return in signature %s", sig);
    }
}

// For traced icalls: the text of the first argument that looks like a managed string (Il2CppString: length at
// +0x10, UTF-16 at +0x14), or "".
static const char *mstr_arg(Cpu *c, char *out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t p = c->x[i];
        MEMORY_BASIC_INFORMATION mi;
        if (p < 0x10000 || (p & 7) || !VirtualQuery((void *)(uintptr_t)p, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
            !(mi.Protect & (PAGE_READWRITE | PAGE_READONLY)) || (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize < p + 0x18)
            continue;
        uint32_t len = LD32(p + 0x10);
        if (!len || len > 300 || (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize < p + 0x14 + 2ull * len) continue;
        size_t n = 0;
        for (uint32_t k = 0; k < len && n + 1 < cap; k++) {
            uint16_t ch = LD16(p + 0x14 + 2ull * k);
            if (ch < 0x20 || ch > 0x7e) { n = 0; break; }
            out[n++] = (char)ch;
        }
        out[n] = 0;
        if (n) break;
    }
    return out;
}

static void icall_body(Cpu *c, void *user) {
    Icall *e = user;
    e->calls++;
    if (__builtin_expect((rt_trace & 4) || e->traced, 0))
    {
        char text[320];
        rt_log("icall %s [%s] (%#llx, %#llx, %#llx, %#llx) \"%s\" t=%.1fs", e->name, e->sig ? e->sig : "ints", (unsigned long long)c->x[0],
               (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3], mstr_arg(c, text, sizeof text),
               (double)rt_ticks() / 19200000.0);
    }
    // A guest code address handed to the engine as a native callback (GL.IssuePluginEvent and the CommandBuffer
    // variants: the CRI Mana render-event function) becomes a host-callable stub: the engine calls it as a Win64
    // function on its render thread.
    if (__builtin_expect(e->plugin_event, 0))
        for (int i = 0; i < 3; i++)
            if (c->x[i] - GUEST_BASE < 0x487b000ull && !(c->x[i] & 3) && rt_lifted_lookup(c->x[i])) {
                static volatile LONG logged;
                if (InterlockedIncrement(&logged) <= 4)
                    rt_log("icall %s: argument %d is guest code main+%#llx, passing a host stub", e->name, i, (unsigned long long)(c->x[i] - GUEST_BASE));
                c->x[i] = (uint64_t)(uintptr_t)rt_guest_callback(c->x[i], NULL);
            }
    if (e->sig) rt_icall_marshal(c, e->host, e->sig);
    else {   // no signature known: integer arguments only
        uint64_t slots[12], x0 = 0;
        for (int i = 0; i < 8; i++) slots[i] = c->x[i];
        for (int i = 0; i < 4; i++) slots[8 + i] = LD64(c->sp + 8 * i);
        c->x[0] = rt_win64_call(e->host, slots, 12, &x0);
        c->v[0] = x0;
    }
}
static void icall_thunk(Cpu *c) { rt_thunk_run(c, icall_body, rt_syn_entry(c->pc)->user); }

EXPORT void il2cpp_add_internal_call(const char *name, void *method) {
    Cpu *c = api_enter("il2cpp_add_internal_call");
    size_t n = strlen(name);
    AcquireSRWLockExclusive(&g_icall_lock);
    Icall *e = icall_find(name, n);
    if (!e) {
        e = calloc(1, sizeof *e);
        e->name = _strdup(name);
        uint32_t b = str_hash(name, n) & (ICALL_BUCKETS - 1);
        e->next = g_icalls[b];
        g_icalls[b] = e;
        e->sig = sig_find(name);
        e->plugin_event = strstr(name, "IssuePlugin") != NULL;
        // BR_ICALL_FILTER=<substring>[,<substring>...]: log every call of the icalls whose name contains one of them.
        static const char *filter;
        if (!filter) { filter = getenv("BR_ICALL_FILTER"); if (!filter) filter = ""; }
        for (const char *f = filter; *f;) {
            const char *end = strchr(f, ',');
            size_t len = end ? (size_t)(end - f) : strlen(f);
            for (const char *q = name; len && *q; q++)
                if (!strncmp(q, f, len)) { e->traced = 1; break; }
            f += len + (end != NULL);
        }
        e->guest = rt_syn_alloc(icall_thunk, e->name, e);
        g_n_icalls++;
        if (!e->sig) {
            g_n_nosig++;
            if (rt_trace & 4) rt_log("icall %s: no signature in the metadata table, integer-only marshalling", name);
        }
    }
    extern void *rt_interp_wrap(const char *name, void *method);   // interp.c: Transform calls go through the port
    e->host = rt_interp_wrap(name, method);
    ReleaseSRWLockExclusive(&g_icall_lock);
    c->x[0] = (uint64_t)(uintptr_t)e->name;
    c->x[1] = e->guest;
    api_call(c, api_guest("il2cpp_add_internal_call") - GUEST_BASE, "il2cpp_add_internal_call");
}

// Internal calls of the Switch engine that the Windows engine does not have (UnityEngine.Switch.*, ...): host
// functions with the guest calling convention (icall_host.c), registered straight into the lifted runtime's table.
typedef struct { const char *name; HostFn fn; } HostIcall;
extern const HostIcall rt_host_icalls[];
void rt_icall_report(void) {
    Cpu *c = rt_cpu();
    uint64_t added = 0, missing = 0;
    for (const HostIcall *h = rt_host_icalls; h->name; h++) {
        if (icall_find(h->name, strlen(h->name))) continue;   // the engine has its own
        c->x[0] = (uint64_t)(uintptr_t)h->name;
        c->x[1] = rt_syn_alloc(h->fn, h->name, NULL);
        api_call(c, api_guest("il2cpp_add_internal_call") - GUEST_BASE, "il2cpp_add_internal_call");
        Icall *e = calloc(1, sizeof *e);   // remembered so that the census below and il2cpp_resolve_icall see it
        e->name = _strdup(h->name);
        uint32_t b = str_hash(h->name, strlen(h->name)) & (ICALL_BUCKETS - 1);
        e->next = g_icalls[b];
        g_icalls[b] = e;
        added++;
    }
    int list = getenv("BR_LIST_MISSING_ICALLS") != NULL;
    for (size_t i = 0; i < NSIGS; i++)
        if (strncmp(g_sigs[i].name, "System.", 7) && strncmp(g_sigs[i].name, "Mono.", 5) &&
            !icall_find(g_sigs[i].name, strlen(g_sigs[i].name))) {
            missing++;
            if (list) rt_log("  icall not provided by the engine or the host: %s [%s]", g_sigs[i].name, g_sigs[i].sig);
        }
    rt_log("icalls: %llu registered by the engine, %llu without a signature (integer-only marshalling); %llu host icalls added; "
           "%llu names of the metadata table have no implementation outside the lifted runtime (BR_LIST_MISSING_ICALLS=1 lists them)",
           (unsigned long long)g_n_icalls, (unsigned long long)g_n_nosig, (unsigned long long)added, (unsigned long long)missing);
}

// The engine gets its own function back; an icall implemented inside the lifted runtime gets a host-callable stub.
EXPORT void *il2cpp_resolve_icall(const char *name) {
    Cpu *c = api_enter("il2cpp_resolve_icall");
    const char *paren = strchr(name, '(');
    AcquireSRWLockShared(&g_icall_lock);
    Icall *e = icall_find(name, strlen(name));
    if (!e && paren) e = icall_find(name, (size_t)(paren - name));
    ReleaseSRWLockShared(&g_icall_lock);
    if (e) return e->host;
    c->x[0] = (uint64_t)(uintptr_t)name;
    api_call(c, api_guest("il2cpp_resolve_icall") - GUEST_BASE, "il2cpp_resolve_icall");
    uint64_t g = c->x[0];
    if (!g) return NULL;
    SynEntry *s = rt_syn_entry(g);
    if (s && s->fn == icall_thunk) return ((Icall *)s->user)->host;
    rt_log("il2cpp_resolve_icall(%s): runtime-internal icall, handing out a host stub (integer arguments)", name);
    return rt_guest_callback(g, NULL);
}

// ---- host-callable entries for guest functions (Win64 -> AAPCS64). Stub: mov r10, desc; mov rax, entry; jmp rax.
typedef struct { uint64_t guest; const char *sig; } CbDesc;
extern void rt_callback_entry(void);
static uint8_t *g_cb_page;
static size_t g_cb_used;
#define CB_PAGE (1u << 20)
static struct { uint64_t guest; void *stub; } *g_cb_map;
static size_t g_cb_count, g_cb_cap;
static SRWLOCK g_cb_lock = SRWLOCK_INIT;

void *rt_guest_callback(uint64_t guest_fn, const char *sig) {
    void *r = NULL;
    AcquireSRWLockExclusive(&g_cb_lock);
    for (size_t i = 0; i < g_cb_count; i++)
        if (g_cb_map[i].guest == guest_fn) { r = g_cb_map[i].stub; break; }
    if (!r) {
        if (!g_cb_page || g_cb_used + 48 > CB_PAGE) {
            g_cb_page = VirtualAlloc(NULL, CB_PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            g_cb_used = 0;
        }
        CbDesc *d = malloc(sizeof *d);
        d->guest = guest_fn;
        d->sig = sig;
        uint8_t *s = g_cb_page + g_cb_used;
        g_cb_used += 32;
        uint64_t entry = (uint64_t)(uintptr_t)rt_callback_entry, dv = (uint64_t)(uintptr_t)d;
        s[0] = 0x49; s[1] = 0xba; memcpy(s + 2, &dv, 8);
        s[10] = 0x48; s[11] = 0xb8; memcpy(s + 12, &entry, 8);
        s[20] = 0xff; s[21] = 0xe0;
        FlushInstructionCache(GetCurrentProcess(), s, 32);
        if (g_cb_count == g_cb_cap) { g_cb_cap = g_cb_cap ? g_cb_cap * 2 : 256; g_cb_map = realloc(g_cb_map, g_cb_cap * sizeof *g_cb_map); }
        g_cb_map[g_cb_count].guest = guest_fn;
        g_cb_map[g_cb_count++].stub = s;
        r = s;
    }
    ReleaseSRWLockExclusive(&g_cb_lock);
    return r;
}

// Called by rt_callback_entry (tramp.S). iargs[0..3] are the register arguments, iargs[4..] the stack arguments;
// fargs[0..3] the SSE registers. Signature tokens i/f/d (default: eight integers).
uint64_t rt_callback_dispatch(CbDesc *d, uint64_t *iargs, uint64_t *fargs, uint64_t *xmm0) {
    Cpu *c = rt_cpu();
    uint64_t saved_x[9];
    memcpy(saved_x, c->x, sizeof saved_x);
    const char *sig = d->sig;
    char rk = 'q';
    if (!sig) {
        for (int i = 0; i < 8; i++) c->x[i] = iargs[i];
    } else {
        rk = *sig++;
        if (*sig == ':') sig++;
        int ngr = 0, nvr = 0;
        for (int i = 0; sig[i]; i++) {
            if (sig[i] == 'i') { if (ngr < 8) c->x[ngr++] = iargs[i]; }
            else if (sig[i] == 'f') { if (nvr < 8) c->v[nvr++] = (uint32_t)(i < 4 ? fargs[i] : iargs[i]); }
            else if (sig[i] == 'd') { if (nvr < 8) c->v[nvr++] = i < 4 ? fargs[i] : iargs[i]; }
            else rt_fatal_c(c, "callback signature %s not supported", d->sig);
        }
    }
    rt_call_regs(c, d->guest);
    rt_escape_check(c, "a guest callback");
    uint64_t r = c->x[0];
    *xmm0 = (uint64_t)c->v[0];
    if (rk == 'f' || rk == 'd') r = *xmm0;
    (void)saved_x;
    return r;
}
