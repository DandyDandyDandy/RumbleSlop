// RumbleSlop host runtime: internal interfaces.
// Design borrows from xc2-recomp's runtime (MIT, see include/LICENSE.xc2-recomp): the flat guest-address dispatch
// table (dispatch.c), host functions bound to guest addresses and rt_call (hostfn.c). Unlike that project no SDK
// code is lifted: imported SDK functions are host functions bound by symbol name.
#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cpu.h"
#include "eh.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GUEST_BASE 0x7000000000ull
#define SYN_BASE   0x7100000000ull          // synthetic guest addresses: 16 bytes per host-bound entry
#define SYN_MAX    (1u << 20)
#define RT_RET_HOST (SYN_BASE + 0)          // x30 sentinel of rt_call (entry 0)

typedef void (*HostFn)(Cpu *c);

// ---- log.c
extern int rt_trace;                         // BR_TRACE bit mask: 1 host fns, 2 api, 4 icalls, 8 fs, 16 threads
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_vlog(const char *fmt, va_list ap);
void rt_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void rt_fatal_c(Cpu *c, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
void rt_backtrace(Cpu *c);
const char *rt_guest_name(uint64_t pc, char *buf, int size);   // "0x70.. (main+0x.. Type::Method+0x..)"
const char *rt_exe_dir(void);                // directory of the player executable, with trailing backslash
const char *rt_user_dir(void);               // where save data and the port's settings files are kept (trailing backslash)
const char *rt_data_dir(void);               // <exe dir>\<Name>_Data\ (trailing backslash)
void rt_log_init(void);
void rt_ring_note(uint64_t addr);            // last-functions ring (fnprof)

// ---- dispatch.c
typedef struct SynEntry { HostFn fn; const char *name; void *user; uint64_t calls; } SynEntry;
uint64_t rt_syn_alloc(HostFn fn, const char *name, void *user);
SynEntry *rt_syn_entry(uint64_t addr);       // NULL when addr is not a live synthetic address
void rt_bind_addr(uint64_t guest_addr, HostFn fn, const char *name);   // host implementation of non-lifted code in main
HostFn rt_bound_lookup(uint64_t guest_addr, const char **name);
void rt_dispatch_init(void);
GuestFn rt_lifted_lookup(uint64_t pc);
void rt_boundary(Cpu *c, uint64_t addr);
const char *rt_boundary_name(uint64_t addr);
static inline bool rt_in_image(uint64_t a);

// ---- thread.c
typedef struct RtExc RtExc;
typedef struct RtThread {
    Cpu cpu;
    uint64_t stack_lo, stack_hi;             // guest stack
    uint8_t *tlr;                            // thread-local region (tpidrro)
    uint8_t *thread_type;                    // guest nn::os::ThreadType of this thread
    RtExc *caught;                           // stack of caught exceptions (eh.c)
    uint32_t uncaught;
    int thunk_depth;                         // guest->host thunks on the host stack that can catch a host unwind
    int call_depth;                          // rt_call nesting
    void *host_thread;                       // HANDLE (duplicated) for stop-the-world
    uint32_t host_tid;
    uint64_t host_stack_hi;                  // host stack base (highest address)
    uint64_t host_sp_at_gc;                  // approximate host sp published when blocking in the collector
    struct RtThread *next;
    int own_stack;
    uint64_t ring[64]; uint32_t ring_pos;
    const char *volatile in_host;            // name of the host function this thread is inside (BR_HANGDUMP), else NULL
} RtThread;
RtThread *rt_thread(void);                   // current host thread's guest context (created on first use)
static inline Cpu *rt_cpu(void) { return &rt_thread()->cpu; }
RtThread *rt_thread_create(uint64_t stack, uint64_t stack_size);   // context for a new guest thread (not yet bound)
void rt_thread_type_init(uint64_t thread_type, uint64_t stack, uint64_t stack_size);
void rt_thread_bind(RtThread *t);            // make t the calling host thread's context
void rt_thread_exit(RtThread *t);
extern RtThread *rt_threads;                 // list of live thread contexts (rt_threads_lock)
void rt_threads_lock(void);
void rt_threads_unlock(void);

// host -> guest. Integer arguments in x0.., returns x0. Preserves the callee-saved guest state even when the call
// returns unwinding. rt_call_checked additionally converts a guest exception that escapes into a host unwind to the
// innermost guest->host thunk (or a fatal error when there is none).
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...);
void rt_call_regs(Cpu *c, uint64_t fn);      // registers already set up
void rt_escape_check(Cpu *c, const char *what);

// guest -> host (tramp.S): slots[i] goes to both the integer and the SSE register of position i (i < 4) and to the
// stack beyond; nslots is rounded up as needed and at least 4 slots must be readable. Returns rax, *xmm0 = xmm0.
uint64_t rt_win64_call(void *fn, const uint64_t *slots, uint64_t nslots, uint64_t *xmm0);
// Call a raw host function with x0..x7 as integer arguments from a guest call site (thunk.cpp; catches host unwinds).
void rt_raw_host_call(Cpu *c, void *fn);
// Run fn(c, user) inside a frame that catches the host unwind (thunk.cpp).
typedef void (*ThunkBody)(Cpu *c, void *user);
void rt_thunk_run(Cpu *c, ThunkBody body, void *user);
void rt_host_unwind(void) __attribute__((noreturn));   // throw to the innermost thunk

// ---- loader.c
extern uint64_t rt_image_size;               // bytes mapped at GUEST_BASE (0 before the image is loaded)
void rt_load_image(void);
void rt_run_init_array(void);
uint64_t rt_sym_addr(const char *name);      // address an undefined dynsym was bound to (0 when not imported)
const char *rt_syn_data_name(uint64_t addr); // name of the import data block containing addr, or NULL
static inline bool rt_in_image(uint64_t a) { return a - GUEST_BASE < rt_image_size; }

// ---- host function tables (imports_*.c): linker-sorted section of {symbol name, function}
typedef struct RtHostDef { const char *name; HostFn fn; } RtHostDef;
#define HOSTFN(sym)                                                                        \
    static void h_##sym(Cpu *c);                                                           \
    __attribute__((section(".hfn$m"), used)) static const RtHostDef hd_##sym = {#sym, h_##sym}; \
    static void h_##sym(Cpu *c)
// Same function under another symbol name.
#define HOSTALIAS(sym, target) \
    __attribute__((section(".hfn$m"), used)) static const RtHostDef hd_##sym = {#sym, h_##target};
HostFn rt_hostfn_find(const char *name);

// argument access
#define X(i) (c->x[i])
#define W(i) ((uint32_t)c->x[i])
#define PTR(i) ((void *)(uintptr_t)c->x[i])
#define STR(i) ((const char *)(uintptr_t)c->x[i])
static inline double rt_getd(const Cpu *c, int i) { double d; uint64_t u = (uint64_t)c->v[i]; memcpy(&d, &u, 8); return d; }
static inline float rt_getf(const Cpu *c, int i) { float f; uint32_t u = (uint32_t)c->v[i]; memcpy(&f, &u, 4); return f; }
static inline void rt_setd(Cpu *c, int i, double d) { uint64_t u; memcpy(&u, &d, 8); c->v[i] = u; }
static inline void rt_setf(Cpu *c, int i, float f) { uint32_t u; memcpy(&u, &f, 4); c->v[i] = u; }
#define RET(v) (c->x[0] = (uint64_t)(v))

// ---- memory (imports_libc.c): the one heap behind malloc, operator new and the string helpers
void *rt_malloc(size_t n);
void *rt_memalign(size_t align, size_t n);
void *rt_realloc(void *p, size_t n);
void rt_free(void *p);

// ---- eh.c
void rt_eh_throw(Cpu *c, uint64_t obj, uint64_t tinfo, uint64_t dtor);
const char *rt_eh_describe(Cpu *c, char *buf, int size);   // type name of the exception in c->eh_exc

// ---- icall.c / api
void rt_init_once(void);                     // load the image, bind, run static initialisers (first API call)
void *rt_guest_callback(uint64_t guest_fn, const char *sig);   // host-callable entry for a guest function (int args)
uint64_t rt_api_addr(const char *name);
void rt_gc_init(void);

#ifdef __cplusplus
}
#endif
