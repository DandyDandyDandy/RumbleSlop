// Deliberate answers for P/Invoke natives of the Switch plug-ins that are neither lifted nor implemented: the
// NPLN online stack (gRPC/OpenSSL/WebRTC, 608 natives), and the maria-upload telemetry library. rt_boundary (dispatch.c) asks here before it raises EntryPointNotFoundException.
//
// A rule's pattern is matched against the method name that follows "native for " in rt_boundary_info; '*' matches
// any run of characters and the pattern must match the whole name. The first match wins, so specific rules go before
// general ones. Every native answered here is logged once.
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

enum {
    P_RET,        // return `value` in x0 (and 0.0 in v0)
    P_OUT0,       // *x0 = 0 (8 bytes) when x0 is not null, then return `value`
    P_OUT1,       // *x1 = 0 (8 bytes) when x1 is not null, then return `value`
};
typedef struct { const char *prefix; int kind; uint64_t value; const char *why; } Rule;

static const Rule g_rules[] = {
#include "pinvoke_rules.inc"
    {NULL, 0, 0, NULL},
};

static int glob(const char *p, const char *s) {
    for (; *p; p++, s++) {
        if (*p == '*') {
            for (const char *t = s;; t++) {
                if (glob(p + 1, t)) return 1;
                if (!*t) return 0;
            }
        }
        if (*p != *s) return 0;
    }
    return !*s;
}

static uint64_t g_calls, g_distinct;
static uint8_t *g_seen;
#define SEEN_LIMIT 0xc6e000u

int rt_pinvoke_policy(Cpu *c, uint64_t addr, const char *name) {
    const char *m = name + 11;   // after "native for "
    extern int rt_vib_pinvoke(Cpu *c, const char *method);      // imports_hid.c: the vibration-file player
    if (!strncmp(m, "nnUser.Util+Vibration::", 23) && rt_vib_pinvoke(c, m + 23)) return 1;
    const Rule *r = g_rules;
    for (; r->prefix; r++)
        if (glob(r->prefix, m)) break;
    if (!r->prefix) return 0;
    uint64_t off = addr - GUEST_BASE;
    if (!g_seen) g_seen = calloc(1, SEEN_LIMIT >> 2);
    g_calls++;
    if (off < SEEN_LIMIT && !g_seen[off >> 2]) {
        g_seen[off >> 2] = 1;
        g_distinct++;
        rt_log("P/Invoke %s (main+%#llx): policy '%s' -> %s%#llx", m, (unsigned long long)off, r->why,
               r->kind == P_OUT0 ? "*x0 = 0, " : r->kind == P_OUT1 ? "*x1 = 0, " : "", (unsigned long long)r->value);
    }
    if (r->kind == P_OUT0 && c->x[0]) ST64(c->x[0], 0);
    if (r->kind == P_OUT1 && c->x[1]) ST64(c->x[1], 0);
    c->x[0] = r->value;
    c->v[0] = 0;
    return 1;
}

void rt_pinvoke_policy_stats(uint64_t *calls, uint64_t *distinct) { *calls = g_calls; *distinct = g_distinct; }
