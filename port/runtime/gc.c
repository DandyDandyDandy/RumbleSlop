// Host side of the lifted Boehm GC's platform layer: the three engine helpers it calls to stop the world, restart
// it and push the stacks of all threads, plus the bindings of the other engine helpers to host functions.
//
// Guest state of a thread lives in three places: its Cpu, its guest stack, and - because the lifted C keeps guest
// registers in C locals across calls - the host stack and host registers of the thread. All three are scanned
// conservatively for every thread that has a guest context.
#include <windows.h>
#include <setjmp.h>
#include <string.h>
#include "rt.h"

// The engine helper pushes through two small Boehm entry points that are not in the lift set (only the engine
// called them): GC_push_one (main+0x3e3b90) and GC_push_all_stack (main+0x3e3e00). Both reduce to
// GC_mark_and_push_stack (lifted, main+0x3e3bc0) for every word inside the plausible heap bounds; that is what the
// host does here, eagerly.
#define GC_MARK_AND_PUSH_STACK (GUEST_BASE + 0x3e3bc0)
#define GC_LEAST_PLAUSIBLE     (GUEST_BASE + 0x6496a48)
#define GC_GREATEST_PLAUSIBLE  (GUEST_BASE + 0x6cf47a0)
static void gc_push_word(Cpu *c, uint64_t w) {
    if (w >= LD64(GC_LEAST_PLAUSIBLE) && w < LD64(GC_GREATEST_PLAUSIBLE)) rt_call(c, GC_MARK_AND_PUSH_STACK, 1, w);
}

extern SRWLOCK rt_heap_gate, rt_obj_lock, rt_big_lock;
void rt_log_lock(void);
void rt_log_unlock(void);
volatile DWORD rt_world_owner;                    // thread id that has the world stopped (skips the heap gate)
static CONTEXT g_ctx[512];
static RtThread *g_stopped[512];
static int g_nstopped;
static uint64_t g_stops;

// Which threads stop. The Switch engine's helper (main+0x1c1a9f0) walks the table the lifted runtime keeps of the
// threads registered with the collector: 4096 slots of {u32 in use} and {nn::os::ThreadType *} (stride 8) behind
// the pointers at main+0x618d348 / +0x618d350, enabled by the byte behind main+0x618d358. The host does the same:
// only registered threads are suspended and scanned. Engine threads that merely have a guest context (they called
// an il2cpp API once) keep running - the engine relies on that, e.g. GarbageCollectSharedAssets stops the world and
// then waits for its own worker threads.
#define GC_THREAD_FLAGS   (GUEST_BASE + 0x618d348)
#define GC_THREAD_PTRS    (GUEST_BASE + 0x618d350)
#define GC_THREAD_ENABLE  (GUEST_BASE + 0x618d358)
#define GC_THREAD_SLOTS   4096
static int gc_registered(uint64_t thread_type) {
    uint64_t flags = LD64(GC_THREAD_FLAGS), ptrs = LD64(GC_THREAD_PTRS);
    for (uint32_t i = 0; i < GC_THREAD_SLOTS; i++)
        if (LD32(flags + 8ull * i) && LD64(ptrs + 8ull * i) == thread_type) return 1;
    return 0;
}

static void gc_stop_world(Cpu *c) {
    RtThread *me = (RtThread *)c;
    g_nstopped = 0;
    g_stops++;
    if (!LD8(LD64(GC_THREAD_ENABLE))) { rt_world_owner = GetCurrentThreadId(); return; }
    rt_threads_lock();
    // No thread may be suspended while it is inside the guest allocator or the process heap: the collector uses both.
    AcquireSRWLockExclusive(&rt_heap_gate);
    HANDLE heap = GetProcessHeap();
    HeapLock(heap);
    rt_log_lock();
    for (RtThread *t = rt_threads; t; t = t->next) {
        if (t == me || !t->host_thread) continue;
        uint64_t tt;
        memcpy(&tt, t->tlr + 0x1f8, 8);
        if (!gc_registered(tt)) continue;
        if (g_nstopped >= 512) rt_fatal("gc: more than 512 threads");
        for (;;) {
            SuspendThread(t->host_thread);
            CONTEXT *ctx = &g_ctx[g_nstopped];
            ctx->ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
            GetThreadContext(t->host_thread, ctx);    // also waits until the thread is really suspended
            // A thread stopped while holding one of the runtime's own locks would deadlock the collector.
            int ok = TryAcquireSRWLockExclusive(&rt_obj_lock);
            if (ok) {
                ReleaseSRWLockExclusive(&rt_obj_lock);
                ok = TryAcquireSRWLockExclusive(&rt_big_lock);
                if (ok) ReleaseSRWLockExclusive(&rt_big_lock);
            }
            if (ok) break;
            ResumeThread(t->host_thread);
            SwitchToThread();
        }
        g_stopped[g_nstopped++] = t;
    }
    rt_world_owner = GetCurrentThreadId();
    rt_log_unlock();
    HeapUnlock(heap);
    ReleaseSRWLockExclusive(&rt_heap_gate);
    rt_threads_unlock();
}

static void gc_start_world(Cpu *c) {
    (void)c;
    for (int i = 0; i < g_nstopped; i++) ResumeThread(g_stopped[i]->host_thread);
    g_nstopped = 0;
    rt_world_owner = 0;
}

void rt_gc_stats(uint64_t *stops) { *stops = g_stops; }
int rt_gc_thread_registered(RtThread *t) { uint64_t tt; memcpy(&tt, t->tlr + 0x1f8, 8); return gc_registered(tt); }

static void push_range(Cpu *c, uint64_t lo, uint64_t hi) {
    lo = (lo + 7) & ~7ull;
    hi &= ~7ull;
    uint64_t least = LD64(GC_LEAST_PLAUSIBLE), greatest = LD64(GC_GREATEST_PLAUSIBLE);
    for (uint64_t p = lo; p < hi; p += 8) {
        uint64_t w = LD64(p);
        if (w >= least && w < greatest) rt_call(c, GC_MARK_AND_PUSH_STACK, 1, w);
    }
}
static void push_cpu(Cpu *c, RtThread *t, int exact_sp) {
    for (int i = 0; i < 31; i++)
        gc_push_word(c, t->cpu.x[i]);
    uint64_t sp = t->cpu.sp;
    if (sp < t->stack_lo || sp > t->stack_hi) sp = t->stack_lo;
    else if (!exact_sp) sp = sp - t->stack_lo > 0x10000 ? sp - 0x10000 : t->stack_lo;   // a leaf frame may sit below c->sp
    push_range(c, sp, t->stack_hi);
}

__attribute__((noinline)) static void gc_push_stacks(Cpu *c) {
    RtThread *me = (RtThread *)c;
    jmp_buf jb;
    // Spill the callee-saved host registers of this thread into its stack, then scan from here up.
    volatile uint64_t marker = 0;
    __builtin_unwind_init();
    memset(&jb, 0, sizeof jb);
    if (setjmp(jb)) return;
    uint64_t here = (uint64_t)(uintptr_t)&marker;
    if ((uint64_t)(uintptr_t)&jb < here) here = (uint64_t)(uintptr_t)&jb;
    // x0..x7 of the collecting thread are clobbered by the calls below; save and restore around them.
    uint64_t saved[8];
    memcpy(saved, c->x, sizeof saved);
    push_range(c, here, me->host_stack_hi);
    memcpy(c->x, saved, sizeof saved);
    push_cpu(c, me, 1);
    for (int i = 0; i < g_nstopped; i++) {
        RtThread *t = g_stopped[i];
        CONTEXT *ctx = &g_ctx[i];
        uint64_t regs[16] = {ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx, ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->R8, ctx->R9, ctx->R10,
                             ctx->R11, ctx->R12, ctx->R13, ctx->R14, ctx->R15, ctx->Rsp};
        for (int k = 0; k < 16; k++)
            gc_push_word(c, regs[k]);
        if (ctx->Rsp && ctx->Rsp < t->host_stack_hi) push_range(c, ctx->Rsp, t->host_stack_hi);
        push_cpu(c, t, 0);
    }
    memcpy(c->x, saved, sizeof saved);
}

void rt_gc_init(void) {}

// ---- engine helpers called by the lifted runtime (work/unity/closure/boundary_runtime_to_engine.tsv)
static const struct { uint64_t off; const char *host; HostFn fn; } g_helpers[] = {
    {0xc82840, "_ZdlPv", 0},   {0xc842c0, "_Znwm", 0},  {0xc845f0, "_Znam", 0},     {0xc848f0, "_ZdaPv", 0},
    {0x10f6fd0, "malloc", 0},  {0x10f6ff0, "free", 0},  {0x10f7010, "calloc", 0},   {0x10f7060, "realloc", 0},
    {0x10f7080, "aligned_alloc", 0},
    {0x10f6260, "engine_gc_page_alloc", 0},
    {0x10f6290, "engine_gc_stack_base", 0},
    {0x10fc5c0, "engine_flag_10f7fc0", 0},
    {0x10fd1a0, "engine_is_mounted", 0},
    {0x10fca10, "engine_datetime_ticks", 0},
    {0x1109540, "engine_register_plugin", 0},
    {0x1c1a9f0, "gc_stop_world", gc_stop_world},
    {0x1c1aa70, "gc_start_world", gc_start_world},
    {0x1c1aae0, "gc_push_stacks", gc_push_stacks},
};

void rt_bind_engine_helpers(void) {
    for (size_t i = 0; i < sizeof g_helpers / sizeof g_helpers[0]; i++) {
        HostFn f = g_helpers[i].fn ? g_helpers[i].fn : rt_hostfn_find(g_helpers[i].host);
        if (!f) rt_fatal("engine helper %s has no host function", g_helpers[i].host);
        rt_bind_addr(GUEST_BASE + g_helpers[i].off, f, g_helpers[i].host);
    }
}

// _setjmp has one caller in the lift set (Boehm's GC_with_callee_saves_pushed): it only wants the callee-saved
// registers in memory so that the conservative scan sees them. Layout: x19..x30, sp, d8..d15. Never longjmp'ed.
HOSTFN(_setjmp) {
    uint64_t jb = X(0);
    for (int i = 0; i < 12; i++) ST64(jb + 8 * i, c->x[19 + i]);
    ST64(jb + 96, c->sp);
    for (int i = 0; i < 8; i++) ST64(jb + 104 + 8 * i, (uint64_t)c->v[8 + i]);
    RET(0);
}
