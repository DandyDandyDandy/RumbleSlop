// Host implementations of the libc / libm imports of the lifted code and of the engine's allocation helpers.
// The guest is LP64 (musl): `long` is 64-bit, va_list is the AAPCS64 structure, FILE is opaque to guest code.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wctype.h>
#include <time.h>
#include "rt.h"

// ------------------------------------------------------------------------------------------------ heap
// One private heap for everything the guest allocates (malloc, operator new, libc++ strings, GC pages). It is
// private so that stopping the world for a collection (gc.c) can make sure no suspended thread holds its lock.
static HANDLE g_heap;
SRWLOCK rt_heap_gate = SRWLOCK_INIT;     // shared: inside the allocator; exclusive: world stopped
#define BIG_CAP 16384
static struct { void *p; void *base; size_t size; } g_big[BIG_CAP];   // blocks with alignment > 16
SRWLOCK rt_big_lock = SRWLOCK_INIT;
extern volatile DWORD rt_world_owner;
// Shared while inside the allocator; the collector takes it exclusively only while it suspends threads.
#define GATE_IN() AcquireSRWLockShared(&rt_heap_gate)
#define GATE_OUT() ReleaseSRWLockShared(&rt_heap_gate)
static volatile LONG g_nbig;

static inline HANDLE heap(void) {
    if (__builtin_expect(!g_heap, 0)) {
        HANDLE h = HeapCreate(0, 0, 0);
        if (InterlockedCompareExchangePointer(&g_heap, h, NULL)) HeapDestroy(h);
    }
    return g_heap;
}
static uint32_t big_hash(void *p) { return (uint32_t)((((uintptr_t)p >> 5) * 0x9E3779B97F4A7C15ull) >> 50); }
static int big_find(void *p) {   // lock held
    for (uint32_t h = big_hash(p), n = 0; n < BIG_CAP; h = (h + 1) & (BIG_CAP - 1), n++) {
        if (g_big[h].p == p) return (int)h;
        if (!g_big[h].p && !g_big[h].base) return -1;
    }
    return -1;
}
void *rt_malloc(size_t n) {
    GATE_IN();
    void *p = HeapAlloc(heap(), 0, n ? n : 1);
    GATE_OUT();
    if (!p) rt_fatal("guest heap: out of memory allocating %zu bytes", n);
    return p;
}
void *rt_memalign(size_t align, size_t n) {
    if (align <= 16) return rt_malloc(n);
    void *base, *p;
    GATE_IN();
    if (align >= 4096) {
        base = p = VirtualAlloc(NULL, n ? n : 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);   // 64 KiB aligned
        if (align > 65536) rt_fatal("memalign: alignment %zu not supported", align);
    } else {
        base = HeapAlloc(heap(), 0, n + align);
        p = base ? (void *)(((uintptr_t)base + align) & ~(uintptr_t)(align - 1)) : NULL;
    }
    GATE_OUT();
    if (!p) rt_fatal("guest heap: out of memory allocating %zu bytes aligned %zu", n, align);
    AcquireSRWLockExclusive(&rt_big_lock);
    if (g_nbig >= BIG_CAP / 2) rt_fatal("guest heap: too many over-aligned blocks");
    uint32_t h = big_hash(p);
    while (g_big[h].p) h = (h + 1) & (BIG_CAP - 1);
    g_big[h].p = p;
    g_big[h].base = base;
    g_big[h].size = n;
    g_nbig++;
    ReleaseSRWLockExclusive(&rt_big_lock);
    return p;
}
// Removes p from the over-aligned registry; returns its base (and size) or NULL when p is an ordinary block.
static void *big_take(void *p, size_t *size, int remove) {
    if (((uintptr_t)p & 31) || !g_nbig) return NULL;
    void *base = NULL;
    if (remove) AcquireSRWLockExclusive(&rt_big_lock); else AcquireSRWLockShared(&rt_big_lock);
    int i = big_find(p);
    if (i >= 0) {
        base = g_big[i].base;
        if (size) *size = g_big[i].size;
        if (remove) { g_big[i].p = NULL; g_big[i].base = (void *)1; g_nbig--; }   // tombstone keeps probe chains
    }
    if (remove) ReleaseSRWLockExclusive(&rt_big_lock); else ReleaseSRWLockShared(&rt_big_lock);
    return base;
}
void rt_free(void *p) {
    if (!p) return;
    void *base = big_take(p, NULL, 1);
    GATE_IN();
    if (base && base == p && !((uintptr_t)p & 0xffff)) VirtualFree(p, 0, MEM_RELEASE);
    else HeapFree(heap(), 0, base ? base : p);
    GATE_OUT();
}
static size_t rt_msize(void *p) {
    size_t n = 0;
    if (big_take(p, &n, 0)) return n;
    return HeapSize(heap(), 0, p);
}
void *rt_realloc(void *p, size_t n) {
    if (!p) return rt_malloc(n);
    size_t old = 0;
    if (big_take(p, &old, 0)) {
        void *q = rt_malloc(n);
        memcpy(q, p, old < n ? old : n);
        rt_free(p);
        return q;
    }
    GATE_IN();
    void *q = HeapReAlloc(heap(), 0, p, n ? n : 1);
    GATE_OUT();
    if (!q) rt_fatal("guest heap: out of memory reallocating to %zu bytes", n);
    return q;
}

HOSTFN(malloc) { RET(rt_malloc(X(0))); }
HOSTFN(free) { rt_free(PTR(0)); }
HOSTFN(calloc) { size_t n = X(0) * X(1); void *p = rt_malloc(n); memset(p, 0, n); RET(p); }
HOSTFN(realloc) { RET(rt_realloc(PTR(0), X(1))); }
HOSTFN(aligned_alloc) { RET(rt_memalign(X(0), X(1))); }
HOSTALIAS(memalign, aligned_alloc)
HOSTFN(posix_memalign) { ST64(X(0), (uint64_t)(uintptr_t)rt_memalign(X(1), X(2))); RET(0); }
HOSTFN(malloc_usable_size) { RET(PTR(0) ? rt_msize(PTR(0)) : 0); }
HOSTFN(_Znwm) { RET(rt_malloc(X(0))); }
HOSTALIAS(_Znam, _Znwm)
HOSTFN(_ZdlPv) { rt_free(PTR(0)); }
HOSTALIAS(_ZdaPv, _ZdlPv)
HOSTFN(engine_gc_page_alloc) { void *p = rt_memalign(0x1000, X(0)); memset(p, 0, X(0)); RET(p); }   // main+0x10f6260

// ------------------------------------------------------------------------------------------------ string / memory
HOSTFN(memcpy) { memcpy(PTR(0), PTR(1), X(2)); }
HOSTFN(memmove) { memmove(PTR(0), PTR(1), X(2)); }
HOSTFN(memset) { memset(PTR(0), (int)W(1), X(2)); }
HOSTFN(memcmp) { RET((int64_t)memcmp(PTR(0), PTR(1), X(2))); }
HOSTFN(memchr) { RET(memchr(PTR(0), (int)W(1), X(2))); }
HOSTFN(strcat) { RET(strcat(PTR(0), STR(1))); }
HOSTFN(strncat) { RET(strncat(PTR(0), STR(1), X(2))); }
HOSTFN(strlen) { RET(strlen(STR(0))); }
HOSTFN(strcmp) { RET((int64_t)strcmp(STR(0), STR(1))); }
HOSTFN(strncmp) { RET((int64_t)strncmp(STR(0), STR(1), X(2))); }
HOSTFN(strcpy) { strcpy(PTR(0), STR(1)); }
HOSTFN(strncpy) { strncpy(PTR(0), STR(1), X(2)); }
HOSTFN(strchr) { RET(strchr(STR(0), (int)W(1))); }
HOSTFN(strrchr) { RET(strrchr(STR(0), (int)W(1))); }
// The lifted il2cpp path check (main+0x44da18) rejects any path with one of "<>*?:|" after the mount name. The engine
// hands out Windows paths, so a drive specification at the start ("D:/...") is exempted from that one check.
HOSTFN(strpbrk) {
    const char *s = STR(0);
    if (!strcmp(STR(1), "<>*?:|") && ((s[0] | 32) >= 'a' && (s[0] | 32) <= 'z') && s[1] == ':') s += 2;
    RET(strpbrk(s, STR(1)));
}
HOSTFN(strstr) { RET(strstr(STR(0), STR(1))); }
HOSTFN(strdup) { size_t n = strlen(STR(0)) + 1; char *p = rt_malloc(n); memcpy(p, STR(0), n); RET(p); }
HOSTFN(strlcpy) {
    size_t n = strlen(STR(1)), cap = X(2);
    if (cap) { size_t k = n < cap - 1 ? n : cap - 1; memcpy(PTR(0), STR(1), k); ((char *)PTR(0))[k] = 0; }
    RET(n);
}
HOSTFN(atoi) { RET((int64_t)atoi(STR(0))); }
HOSTFN(atol) { RET(strtoll(STR(0), NULL, 10)); }
HOSTFN(strtol) { char *e; long long v = strtoll(STR(0), &e, (int)W(2)); if (X(1)) ST64(X(1), (uint64_t)(uintptr_t)e); RET(v); }
HOSTFN(strtoul) { char *e; unsigned long long v = strtoull(STR(0), &e, (int)W(2)); if (X(1)) ST64(X(1), (uint64_t)(uintptr_t)e); RET(v); }
HOSTFN(strtod) { char *e; double v = strtod(STR(0), &e); if (X(1)) ST64(X(1), (uint64_t)(uintptr_t)e); rt_setd(c, 0, v); }
HOSTFN(tolower) { int ch = (int)W(0); RET((int64_t)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch)); }
HOSTFN(toupper) { int ch = (int)W(0); RET((int64_t)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch)); }
HOSTFN(isalpha) { int ch = (int)W(0); RET((ch | 32) >= 'a' && (ch | 32) <= 'z'); }
HOSTFN(towlower) { uint32_t ch = W(0); RET(ch < 0x10000 ? (uint32_t)towlower((wint_t)ch) : ch); }
HOSTFN(htonl) { RET(__builtin_bswap32(W(0))); }
HOSTALIAS(ntohl, htonl)
HOSTFN(htons) { RET(__builtin_bswap16((uint16_t)W(0))); }
HOSTALIAS(ntohs, htons)
HOSTFN(strerror_r) { snprintf(PTR(1), X(2), "error %d", (int)W(0)); RET(0); }

static __thread struct { Cpu *c; uint64_t cmp; } t_sort;
static int sort_cmp(const void *a, const void *b) {
    return (int)(int32_t)rt_call(t_sort.c, t_sort.cmp, 2, (uint64_t)(uintptr_t)a, (uint64_t)(uintptr_t)b);
}
HOSTFN(qsort) {
    typeof(t_sort) saved = t_sort;
    t_sort.c = c; t_sort.cmp = X(3);
    qsort(PTR(0), X(1), X(2), sort_cmp);
    t_sort = saved;
}
HOSTFN(bsearch) {
    typeof(t_sort) saved = t_sort;
    t_sort.c = c; t_sort.cmp = X(4);
    void *r = bsearch(PTR(0), PTR(1), X(2), X(3), sort_cmp);
    t_sort = saved;
    RET(r);
}

// ------------------------------------------------------------------------------------------------ printf family
typedef struct {
    Cpu *c;
    int va;                       // 0: registers of a variadic call, 1: guest va_list
    int ngr, nvr;                 // next x / v register
    uint64_t stack;               // next stack argument
    uint64_t gr_top, vr_top;
    int32_t gr_offs, vr_offs;
} GArgs;
static void ga_regs(GArgs *a, Cpu *c, int named_ints) { memset(a, 0, sizeof *a); a->c = c; a->ngr = named_ints; a->stack = c->sp; }
static void ga_valist(GArgs *a, Cpu *c, uint64_t va) {
    memset(a, 0, sizeof *a);
    a->c = c; a->va = 1;
    a->stack = LD64(va); a->gr_top = LD64(va + 8); a->vr_top = LD64(va + 16);
    a->gr_offs = (int32_t)LD32(va + 24); a->vr_offs = (int32_t)LD32(va + 28);
}
static uint64_t ga_int(GArgs *a) {
    uint64_t v;
    if (!a->va) {
        if (a->ngr < 8) return a->c->x[a->ngr++];
    } else if (a->gr_offs < 0) {
        v = LD64(a->gr_top + (int64_t)a->gr_offs);
        a->gr_offs += 8;
        return v;
    }
    v = LD64(a->stack);
    a->stack += 8;
    return v;
}
static double ga_dbl(GArgs *a) {
    uint64_t u;
    double d;
    if (!a->va && a->nvr < 8) u = (uint64_t)a->c->v[a->nvr++];
    else if (a->va && a->vr_offs < 0) { u = LD64(a->vr_top + (int64_t)a->vr_offs); a->vr_offs += 16; }
    else { u = LD64(a->stack); a->stack += 8; }
    memcpy(&d, &u, 8);
    return d;
}
typedef struct { char *p; size_t n, cap; } SBuf;
static void sb_put(SBuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
// Formats a guest printf format with guest arguments. Returns a malloc'd string (host heap), length in *len.
static char *guest_format(const char *fmt, GArgs *a, size_t *len) {
    SBuf b = {0};
    sb_put(&b, "", 0);
    while (*fmt) {
        const char *pct = strchr(fmt, '%');
        if (!pct) { sb_put(&b, fmt, strlen(fmt)); break; }
        sb_put(&b, fmt, (size_t)(pct - fmt));
        const char *p = pct + 1;
        char spec[64] = "%", tmp[512];
        int n = 1, lng = 0;
        while (*p && strchr("-+ #0'", *p)) { if (n < 40 && *p != '\'') spec[n++] = *p; p++; }
        if (*p == '*') { n += snprintf(spec + n, 16, "%d", (int)ga_int(a)); p++; }
        else while (*p >= '0' && *p <= '9') { if (n < 40) spec[n++] = *p; p++; }
        if (*p == '.') {
            spec[n++] = *p++;
            if (*p == '*') { n += snprintf(spec + n, 16, "%d", (int)ga_int(a)); p++; }
            else while (*p >= '0' && *p <= '9') { if (n < 50) spec[n++] = *p; p++; }
        }
        for (;; p++) {
            if (*p == 'l' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'q') lng = 1;
            else if (*p == 'L') lng = 2;
            else if (*p != 'h') break;
        }
        char cv = *p;
        if (!cv) break;
        p++;
        fmt = p;
        switch (cv) {
        case '%': sb_put(&b, "%", 1); break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
            uint64_t v = ga_int(a);
            if (cv == 'c') { spec[n++] = 'c'; spec[n] = 0; snprintf(tmp, sizeof tmp, spec, (int)v); }
            else {
                if (!lng) v = (cv == 'd' || cv == 'i') ? (uint64_t)(int64_t)(int32_t)v : (uint64_t)(uint32_t)v;
                spec[n++] = 'l'; spec[n++] = 'l'; spec[n++] = cv; spec[n] = 0;
                snprintf(tmp, sizeof tmp, spec, (unsigned long long)v);
            }
            sb_put(&b, tmp, strlen(tmp));
            break;
        }
        case 'p': snprintf(tmp, sizeof tmp, "0x%llx", (unsigned long long)ga_int(a)); sb_put(&b, tmp, strlen(tmp)); break;
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            double d = ga_dbl(a);   // long double (binary128) is not expected in this code
            spec[n++] = cv; spec[n] = 0;
            snprintf(tmp, sizeof tmp, spec, d);
            sb_put(&b, tmp, strlen(tmp));
            break;
        }
        case 's': {
            const char *s = (const char *)(uintptr_t)ga_int(a);
            if (!s) s = "(null)";
            if (lng) s = "(wide string)";
            spec[n++] = 's'; spec[n] = 0;
            int need = snprintf(NULL, 0, spec, s);
            char *big = malloc((size_t)need + 1);
            snprintf(big, (size_t)need + 1, spec, s);
            sb_put(&b, big, (size_t)need);
            free(big);
            break;
        }
        case 'n': ga_int(a); break;
        default: sb_put(&b, pct, (size_t)(p - pct)); break;
        }
    }
    *len = b.n;
    return b.p;
}

// Guest FILE objects: the three standard streams are small fake objects; guest code only passes them back.
static uint64_t g_std_files[3][4], g_std_ptrs[3];
uint64_t rt_import_data(const char *name) {
    static const char *names[3] = {"stdin", "stdout", "stderr"};
    for (int i = 0; i < 3; i++)
        if (!strcmp(name, names[i])) {
            g_std_ptrs[i] = (uint64_t)(uintptr_t)g_std_files[i];
            return (uint64_t)(uintptr_t)&g_std_ptrs[i];
        }
    return 0;
}
static int std_index(uint64_t f) {
    for (int i = 0; i < 3; i++)
        if (f == (uint64_t)(uintptr_t)g_std_files[i]) return i;
    return -1;
}
static void guest_out(uint64_t file, const char *s, size_t n) {
    int i = std_index(file);
    if (i < 0) { fwrite(s, 1, n, (FILE *)(uintptr_t)file); return; }
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    rt_log("guest %s: %.*s", i == 2 ? "stderr" : "stdout", (int)n, s);
}
HOSTFN(fprintf) { GArgs a; ga_regs(&a, c, 2); size_t n; char *s = guest_format(STR(1), &a, &n); guest_out(X(0), s, n); free(s); RET(n); }
HOSTFN(printf) { GArgs a; ga_regs(&a, c, 1); size_t n; char *s = guest_format(STR(0), &a, &n); guest_out((uint64_t)(uintptr_t)g_std_files[1], s, n); free(s); RET(n); }
HOSTFN(vfprintf) { GArgs a; ga_valist(&a, c, X(2)); size_t n; char *s = guest_format(STR(1), &a, &n); guest_out(X(0), s, n); free(s); RET(n); }
static void put_n(Cpu *c, char *dst, size_t cap, char *s, size_t n) {
    if (cap) { size_t k = n < cap - 1 ? n : cap - 1; memcpy(dst, s, k); dst[k] = 0; }
    free(s);
    RET(n);
}
HOSTFN(snprintf) { GArgs a; ga_regs(&a, c, 3); size_t n; char *s = guest_format(STR(2), &a, &n); put_n(c, PTR(0), X(1), s, n); }
HOSTFN(vsnprintf) { GArgs a; ga_valist(&a, c, X(3)); size_t n; char *s = guest_format(STR(2), &a, &n); put_n(c, PTR(0), X(1), s, n); }
HOSTFN(sprintf) { GArgs a; ga_regs(&a, c, 2); size_t n; char *s = guest_format(STR(1), &a, &n); put_n(c, PTR(0), n + 1, s, n); }
HOSTFN(fwrite) { size_t n = X(1) * X(2); guest_out(X(3), PTR(0), n); RET(X(2)); }
HOSTFN(fputs) { guest_out(X(1), STR(0), strlen(STR(0))); RET(0); }
HOSTFN(puts) { guest_out((uint64_t)(uintptr_t)g_std_files[1], STR(0), strlen(STR(0))); RET(0); }
HOSTFN(fflush) { if (X(0) && std_index(X(0)) < 0) fflush((FILE *)PTR(0)); RET(0); }
HOSTFN(fclose) { RET(std_index(X(0)) < 0 ? (int64_t)fclose((FILE *)PTR(0)) : 0); }
HOSTFN(fread) { RET(std_index(X(3)) < 0 ? fread(PTR(0), X(1), X(2), (FILE *)PTR(3)) : 0); }
HOSTFN(fseeko) { RET((int64_t)_fseeki64((FILE *)PTR(0), (int64_t)X(1), (int)W(2))); }
HOSTFN(ftello) { RET(_ftelli64((FILE *)PTR(0))); }

// ------------------------------------------------------------------------------------------------ misc
HOSTFN(__errno_location) { RET(((RtThread *)c)->tlr + 0x800); }
HOSTFN(getenv) {
    char name[256];
    snprintf(name, sizeof name, "BR_GUESTENV_%s", STR(0));   // the guest sees only variables forwarded on purpose
    static char *vals[64]; static int nvals;
    const char *v = getenv(name);
    if (v && nvals < 64) { vals[nvals] = _strdup(v); RET(vals[nvals++]); } else RET(0);
}
HOSTFN(setlocale) { RET("C"); }
HOSTFN(atexit) { RET(0); }
HOSTFN(__cxa_atexit) { RET(0); }
HOSTFN(exit) { rt_log("guest called exit(%d)", (int)W(0)); rt_backtrace(c); TerminateProcess(GetCurrentProcess(), W(0)); }
HOSTFN(abort) { rt_fatal_c(c, "guest called abort()"); }
HOSTFN(time) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    int64_t t = (int64_t)((((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull) / 10000000ull);
    if (X(0)) ST64(X(0), (uint64_t)t);
    RET(t);
}
HOSTFN(clock_gettime) {
    uint64_t ns;
    if (W(0) == 0) {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        ns = (((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull) * 100;
    } else {
        LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
        ns = (uint64_t)(((__uint128_t)q.QuadPart * 1000000000u) / (uint64_t)f.QuadPart);
    }
    ST64(X(1), ns / 1000000000u);
    ST64(X(1) + 8, ns % 1000000000u);
    RET(0);
}
HOSTFN(_ZNSt3__16chrono12system_clock3nowEv) {   // microseconds since the epoch
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    RET((((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull) / 10);
}

// ------------------------------------------------------------------------------------------------ libm
#define M1F(n) HOSTFN(n) { rt_setf(c, 0, n(rt_getf(c, 0))); }
#define M1D(n) HOSTFN(n) { rt_setd(c, 0, n(rt_getd(c, 0))); }
#define M2F(n) HOSTFN(n) { rt_setf(c, 0, n(rt_getf(c, 0), rt_getf(c, 1))); }
#define M2D(n) HOSTFN(n) { rt_setd(c, 0, n(rt_getd(c, 0), rt_getd(c, 1))); }
M1F(sinf) M1F(cosf) M1F(tanf) M1F(asinf) M1F(acosf) M1F(atanf) M1F(expf) M1F(exp2f) M1F(logf) M1F(log10f) M1F(sqrtf)
M1F(log2f) M1F(floorf) M1F(ceilf) M1F(roundf) M1F(truncf) M1F(sinhf) M1F(coshf) M1F(tanhf) M1F(fabsf) M1F(cbrtf)
M1D(sin) M1D(cos) M1D(tan) M1D(asin) M1D(acos) M1D(atan) M1D(exp) M1D(exp2) M1D(log) M1D(log10) M1D(log2) M1D(sqrt)
M1D(floor) M1D(ceil) M1D(round) M1D(trunc) M1D(sinh) M1D(cosh) M1D(tanh) M1D(fabs) M1D(cbrt) M1D(logb)
M1D(asinh) M1D(acosh) M1D(atanh) M1D(expm1) M1D(log1p)
M2F(powf) M2F(atan2f) M2F(fmodf) M2F(hypotf) M2F(fminf) M2F(fmaxf)
M2D(pow) M2D(atan2) M2D(fmod) M2D(hypot) M2D(fmin) M2D(fmax) M2D(remainder)
HOSTFN(modf) { double ip; double r = modf(rt_getd(c, 0), &ip); memcpy(PTR(0), &ip, 8); rt_setd(c, 0, r); }
HOSTFN(modff) { float ip; float r = modff(rt_getf(c, 0), &ip); memcpy(PTR(0), &ip, 4); rt_setf(c, 0, r); }
HOSTFN(ldexpf) { rt_setf(c, 0, ldexpf(rt_getf(c, 0), (int)W(0))); }
HOSTFN(ldexp) { rt_setd(c, 0, ldexp(rt_getd(c, 0), (int)W(0))); }
HOSTFN(scalbn) { rt_setd(c, 0, scalbn(rt_getd(c, 0), (int)W(0))); }
HOSTFN(scalbnf) { rt_setf(c, 0, scalbnf(rt_getf(c, 0), (int)W(0))); }
HOSTFN(frexp) { int e; double r = frexp(rt_getd(c, 0), &e); ST32(X(0), (uint32_t)e); rt_setd(c, 0, r); }
