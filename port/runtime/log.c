// Log file, fatal errors, guest backtraces with symbol names.
#include <windows.h>
#include <tlhelp32.h>
#include <initguid.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#ifndef GAME_NAME
#define GAME_NAME "RumbleSlop"
#endif

int rt_trace;
volatile int g_fnprof;
void rt_fnsample_init(void);
static FILE *g_log;
static CRITICAL_SECTION g_log_cs;
static volatile DWORD g_hang_tid;
// The collector holds the log lock while it suspends threads, so that no suspended thread owns it (gc.c).
void rt_log_lock(void) { EnterCriticalSection(&g_log_cs); }
void rt_log_unlock(void) { LeaveCriticalSection(&g_log_cs); }
static char g_exe_dir[MAX_PATH], g_data_dir[MAX_PATH];
static uint64_t g_log_lines, g_log_max = 2000000;

const char *rt_exe_dir(void) { return g_exe_dir; }
// Where the player's own files are kept (save data, the port's settings files), with a trailing backslash.
// A PC game keeps them in the user's profile: %USERPROFILE%\Saved Games\<Name>. The game folder itself is
// used instead ("portable") when a save\ folder or a file portable.txt is already next to the exe (so a game
// folder made before this existed, or one that is meant to be carried around, stays as it is), when
// BR_PROFILE names a test profile, or with BR_PORTABLE=1. BR_PORTABLE=0 forces the profile folder.
const char *rt_user_dir(void) {
    static char dir[MAX_PATH + 64];
    static volatile LONG state;                  // 0 not decided, 1 deciding, 2 done
    if (state == 2) return dir;
    while (InterlockedCompareExchange(&state, 1, 0) == 1) Sleep(1);
    if (state == 2) return dir;
    char probe[MAX_PATH + 32];
    const char *force = getenv("BR_PORTABLE"), *prof = getenv("BR_PROFILE");
    int portable = prof && *prof;
    snprintf(probe, sizeof probe, "%ssave", g_exe_dir);
    if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) portable = 1;
    snprintf(probe, sizeof probe, "%sportable.txt", g_exe_dir);
    if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) portable = 1;
    if (force && *force) portable = *force != '0';
    PWSTR saved = NULL;
    if (!portable && SHGetKnownFolderPath(&FOLDERID_SavedGames, KF_FLAG_CREATE, NULL, &saved) == S_OK && saved) {
        char base[MAX_PATH];
        if (WideCharToMultiByte(CP_ACP, 0, saved, -1, base, sizeof base, NULL, NULL) > 0) {
            snprintf(dir, sizeof dir, "%s\\%s\\", base, GAME_NAME);
            CreateDirectoryA(dir, NULL);
            if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) dir[0] = 0;
        }
    }
    if (saved) CoTaskMemFree(saved);
    if (!dir[0]) snprintf(dir, sizeof dir, "%s", g_exe_dir);
    rt_log("user files: %s%s", dir, portable ? " (portable: the game folder)" : "");
    state = 2;
    return dir;
}
const char *rt_data_dir(void) { return g_data_dir; }

void rt_log_init(void) {
    if (g_log) return;
    InitializeCriticalSection(&g_log_cs);
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *s = strrchr(exe, '\\');
    char base[MAX_PATH];
    snprintf(base, sizeof base, "%s", s ? s + 1 : exe);
    if (s) s[1] = 0;
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", exe);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    snprintf(g_data_dir, sizeof g_data_dir, "%s%s_Data\\", g_exe_dir, base);
    const char *e = getenv("BR_DATA_DIR");
    if (e && *e) snprintf(g_data_dir, sizeof g_data_dir, "%s\\", e);
    char path[MAX_PATH];
    e = getenv("BR_LOG");
    snprintf(path, sizeof path, "%s%s", e && strchr(e, ':') ? "" : g_exe_dir, e && *e ? e : "port.log");
    g_log = fopen(path, "w");
    if (!g_log) g_log = stderr;
    e = getenv("BR_TRACE");
    if (e) {
        rt_trace = (int)strtol(e, NULL, 0);
        if (strstr(e, "host")) rt_trace |= 1;
        if (strstr(e, "api")) rt_trace |= 2;
        if (strstr(e, "icall")) rt_trace |= 4;
        if (strstr(e, "fs")) rt_trace |= 8;
        if (strstr(e, "thread")) rt_trace |= 16;
        if (strstr(e, "all")) rt_trace |= 31;
        if (strstr(e, "fn")) g_fnprof = 2;       // log every lifted function entry (needs FN_ENTER in the generated code)
    }
    e = getenv("BR_FNRING");
    if (e && *e != '0' && !g_fnprof) g_fnprof = 1;   // keep a ring of the last entries per thread, dumped on fatal
    rt_fnsample_init();
    e = getenv("BR_LOG_MAX");
    if (e) g_log_max = strtoull(e, NULL, 0);
}

void rt_vlog(const char *fmt, va_list ap) {
    if (!g_log) rt_log_init();
    if (g_hang_tid && g_hang_tid == GetCurrentThreadId()) {
        // The hang dump must come out even when the log lock is held by a stuck or suspended thread: it then
        // goes to port_hang.log beside the log.
        int got = 0;
        for (int i = 0; i < 50 && !(got = TryEnterCriticalSection(&g_log_cs)); i++) Sleep(10);
        if (!got) {
            static FILE *hf;
            if (!hf) { char path[MAX_PATH]; snprintf(path, sizeof path, "%sport_hang.log", g_exe_dir); hf = fopen(path, "w"); }
            if (hf) { fprintf(hf, "(log lock unavailable) "); vfprintf(hf, fmt, ap); fputc('\n', hf); fflush(hf); }
            return;
        }
    } else
    EnterCriticalSection(&g_log_cs);
    if (g_log_lines++ < g_log_max) {
        fprintf(g_log, "[%5lu] ", GetCurrentThreadId());
        vfprintf(g_log, fmt, ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_log_cs);
}

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlog(fmt, ap);
    va_end(ap);
}

// ---- symbols: optional TSV files in <data>\il2cpp_data (methods.tsv: image,type,method,address; runtime_symbols.tsv:
// address,name). Loaded on first use.
typedef struct { uint64_t addr; const char *name; } Sym;
static Sym *g_syms;
static size_t g_nsyms, g_capsyms;
static int g_syms_loaded;

static void sym_add(uint64_t a, const char *name) {
    if (g_nsyms == g_capsyms) {
        g_capsyms = g_capsyms ? g_capsyms * 2 : 1 << 16;
        g_syms = realloc(g_syms, g_capsyms * sizeof *g_syms);
    }
    g_syms[g_nsyms].addr = a;
    g_syms[g_nsyms++].name = _strdup(name);
}
static int sym_cmp(const void *a, const void *b) {
    uint64_t x = ((const Sym *)a)->addr, y = ((const Sym *)b)->addr;
    return x < y ? -1 : x > y;
}
static void syms_file(const char *name) {
    char path[MAX_PATH], line[2048];
    snprintf(path, sizeof path, "%sil2cpp_data\\%s", g_data_dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return;
    int col_addr = -1, col_n1 = -1, col_n2 = -1, first = 1;
    while (fgets(line, sizeof line, f)) {
        char *cols[16];
        int n = 0;
        for (char *p = line; n < 16;) {
            cols[n++] = p;
            char *t = strpbrk(p, "\t\r\n");
            if (!t) break;
            int end = *t != '\t';
            *t = 0;
            if (end) break;
            p = t + 1;
        }
        if (first) {
            first = 0;
            for (int i = 0; i < n; i++) {
                if (!strcmp(cols[i], "address")) col_addr = i;
                else if (!strcmp(cols[i], "type")) col_n1 = i;
                else if (!strcmp(cols[i], "method") || !strcmp(cols[i], "name")) col_n2 = i;
            }
            if (col_addr < 0) break;
            continue;
        }
        if (col_addr >= n || !cols[col_addr][0]) continue;
        uint64_t a = strtoull(cols[col_addr], NULL, 16);
        char nm[1024];
        if (col_n1 >= 0 && col_n1 < n) snprintf(nm, sizeof nm, "%s::%s", cols[col_n1], col_n2 < n ? cols[col_n2] : "?");
        else snprintf(nm, sizeof nm, "%s", col_n2 >= 0 && col_n2 < n ? cols[col_n2] : "?");
        sym_add(a, nm);
    }
    fclose(f);
}
static void syms_load(void) {
    if (g_syms_loaded) return;
    g_syms_loaded = 1;
    syms_file("methods.tsv");
    syms_file("generic_methods.tsv");
    syms_file("runtime_symbols.tsv");
    if (g_nsyms) qsort(g_syms, g_nsyms, sizeof *g_syms, sym_cmp);
}
static const Sym *sym_find(uint64_t off) {
    syms_load();
    size_t lo = 0, hi = g_nsyms;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_syms[mid].addr <= off) lo = mid + 1; else hi = mid;
    }
    if (!lo) return NULL;
    const Sym *s = &g_syms[lo - 1];
    return off - s->addr < 0x40000 ? s : NULL;
}

const char *rt_guest_name(uint64_t pc, char *buf, int size) {
    if (rt_in_image(pc)) {
        uint64_t off = pc - GUEST_BASE;
        const Sym *s = sym_find(off);
        const char *bn = rt_boundary_name(pc);
        if (bn) snprintf(buf, size, "main+%#llx [%s]", (unsigned long long)off, bn);
        else if (s) snprintf(buf, size, "main+%#llx (%s+%#llx)", (unsigned long long)off, s->name, (unsigned long long)(off - s->addr));
        else snprintf(buf, size, "main+%#llx", (unsigned long long)off);
    } else {
        SynEntry *e = rt_syn_entry(pc);
        if (e) snprintf(buf, size, "host:%s", e->name ? e->name : "?");
        else snprintf(buf, size, "%#llx", (unsigned long long)pc);
    }
    return buf;
}

void rt_ring_note(uint64_t addr) {
    RtThread *t = rt_thread();
    t->ring[t->ring_pos++ & 63] = addr;
}

// BR_FNSAMPLE=<start seconds>,<duration seconds>[,<max lines>]: count every lifted function entry during that window
// after start-up and log the functions entered, most frequent first (what the game is doing while it seems idle).
static uint32_t *g_sample;
#define SAMPLE_SLOTS (0x4840000u >> 2)
static DWORD WINAPI sample_main(void *p) {
    double start = 0, dur = 1;
    int max = 200;
    sscanf((const char *)p, "%lf,%lf,%d", &start, &dur, &max);
    Sleep((DWORD)(start * 1000));
    int before = g_fnprof;
    g_fnprof = 3;
    Sleep((DWORD)(dur * 1000));
    g_fnprof = before;
    Sleep(200);
    uint32_t distinct = 0;
    for (uint32_t i = 0; i < SAMPLE_SLOTS; i++) distinct += g_sample[i] != 0;
    rt_log("fnsample: %u distinct functions entered between %.1f s and %.1f s", distinct, start, start + dur);
    for (int n = 0; n < max; n++) {
        uint32_t best = 0, bi = 0;
        for (uint32_t i = 0; i < SAMPLE_SLOTS; i++)
            if (g_sample[i] > best) { best = g_sample[i]; bi = i; }
        if (!best) break;
        char a[300];
        rt_log("fnsample: %9u  %s", best, rt_guest_name(GUEST_BASE + 4ull * bi, a, sizeof a));
        g_sample[bi] = 0;
    }
    return 0;
}
// BR_HANGDUMP=<seconds>[,<seconds>...]: at those times after start-up log, for every thread with a guest context,
// the host function it is inside (if any) and its guest backtrace. The threads are suspended while they are read.
int rt_threads_trylock(void);
int rt_gc_thread_registered(RtThread *t);
extern volatile DWORD rt_world_owner;
static int readable(uint64_t a, size_t n);
// Host stack of a suspended thread as module+offset frames (resolve GameAssembly offsets with llvm-symbolizer).
static void host_backtrace(HANDLE th) {
    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(th, &ctx)) return;
    char line[1500];
    int n = 0;
    for (int i = 0; i < 28 && ctx.Rip; i++) {
        HMODULE m = NULL;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)ctx.Rip, &m))
            GetModuleFileNameA(m, path, sizeof path);
        const char *base = strrchr(path, '\\');
        n += snprintf(line + n, sizeof line - (size_t)n, " %s+%#llx", base ? base + 1 : path, (unsigned long long)(ctx.Rip - (uint64_t)(uintptr_t)m));
        if (n > 1400) break;
        DWORD64 image;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
        if (!rf) {
            if (!readable(ctx.Rsp, 8)) break;
            ctx.Rip = *(DWORD64 *)(uintptr_t)ctx.Rsp;
            ctx.Rsp += 8;
        } else {
            void *hd; DWORD64 ef;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, rf, &ctx, &hd, &ef, NULL);
        }
    }
    rt_log("  host stack:%s", line);
}
static DWORD WINAPI hang_main(void *p) {
    const char *e = p;
    double done = 0;
    g_hang_tid = GetCurrentThreadId();
    while (*e) {
        double at = strtod(e, (char **)&e);
        if (*e == ',') e++;
        if (at > done) Sleep((DWORD)((at - done) * 1000));
        done = at;
        rt_log("hangdump at %.1f s:", at);
        int locked = rt_threads_trylock();
        if (!locked) rt_log("the thread list is locked (a collection is in progress; world owner thread %lu)", (unsigned long)rt_world_owner);
        for (RtThread *t = rt_threads; t; t = t->next) {
            if (!t->host_thread || t->host_tid == GetCurrentThreadId()) continue;
            SuspendThread(t->host_thread);
            rt_log("thread %lu: call depth %d, %s with the collector, in host function %s", (unsigned long)t->host_tid, t->call_depth,
                   rt_gc_thread_registered(t) ? "registered" : "NOT registered", t->in_host ? t->in_host : "(none)");
            host_backtrace(t->host_thread);
            ResumeThread(t->host_thread);
        }
        for (RtThread *t = rt_threads; t; t = t->next)
            if (t->host_thread && t->call_depth > 0 && (locked || t->host_tid == rt_world_owner)) rt_backtrace(&t->cpu);
        if (getenv("BR_HANGDUMP_ALL")) {      // also the threads that never ran guest code (engine workers, render thread)
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = {.dwSize = sizeof te};
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
                int guest = 0;
                for (RtThread *t = rt_threads; t; t = t->next)
                    if (t->host_tid == te.th32ThreadID) guest = 1;
                if (guest) continue;
                HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (!th) continue;
                SuspendThread(th);
                rt_log("engine thread %lu:", (unsigned long)te.th32ThreadID);
                host_backtrace(th);
                ResumeThread(th);
                CloseHandle(th);
            }
            CloseHandle(snap);
        }
        if (locked) rt_threads_unlock();
    }
    return 0;
}
// BR_STATUS=<seconds>: every that many seconds log the counters of the host platform layer.
void rt_audio_stats(uint64_t *appended_bytes, uint64_t *released);
uint64_t rt_hid_reads(void);
void rt_gc_stats(uint64_t *stops);
void rt_pinvoke_policy_stats(uint64_t *calls, uint64_t *distinct);
static DWORD WINAPI status_main(void *p) {
    double every = atof((const char *)p);
    if (every < 0.5) every = 5;
    for (int n = 1;; n++) {
        Sleep((DWORD)(every * 1000));
        uint64_t ab, ar, gs, pc, pd;
        int threads = 0;
        rt_audio_stats(&ab, &ar);
        rt_gc_stats(&gs);
        rt_pinvoke_policy_stats(&pc, &pd);
        for (RtThread *t = rt_threads; t; t = t->next) threads++;
        rt_log("status %.0f s: audio %llu bytes appended in %llu released buffers; npad reads %llu; collections %llu; "
               "policy-answered P/Invoke calls %llu (%llu natives); guest thread contexts %d", n * every, (unsigned long long)ab,
               (unsigned long long)ar, (unsigned long long)rt_hid_reads(), (unsigned long long)gs, (unsigned long long)pc,
               (unsigned long long)pd, threads);
    }
    return 0;
}
void rt_fnsample_init(void) {
    const char *st = getenv("BR_STATUS");
    if (st && *st) CreateThread(NULL, 0, status_main, (void *)st, 0, NULL);
    const char *h = getenv("BR_HANGDUMP");
    if (h && *h) CreateThread(NULL, 0, hang_main, (void *)h, 0, NULL);
    const char *e = getenv("BR_FNSAMPLE");
    if (!e || !*e) return;
    g_sample = VirtualAlloc(NULL, (size_t)SAMPLE_SLOTS * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CreateThread(NULL, 0, sample_main, (void *)e, 0, NULL);
}

void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) {
    if (g_fnprof == 3) {
        uint64_t i = (addr - GUEST_BASE) >> 2;
        if (i < SAMPLE_SLOTS) g_sample[i]++;
        return;
    }
    rt_ring_note(addr);
    if (g_fnprof >= 2) {
        char a[300], b[300];
        rt_log("fn %s lr %s x0 %#llx", rt_guest_name(addr, a, sizeof a), rt_guest_name(lr, b, sizeof b), (unsigned long long)x0);
    }
}

static int readable(uint64_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!a || !VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return a + n <= (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

void rt_backtrace(Cpu *c) {
    char buf[400];
    RtThread *t = (RtThread *)c;
    rt_log("guest state: pc %s", rt_guest_name(c->pc, buf, sizeof buf));
    rt_log("  lr %s  sp %#llx fp %#llx", rt_guest_name(c->x[30], buf, sizeof buf), (unsigned long long)c->sp, (unsigned long long)c->x[29]);
    for (int i = 0; i < 28; i += 4)
        rt_log("  x%d %#llx x%d %#llx x%d %#llx x%d %#llx", i, (unsigned long long)c->x[i], i + 1, (unsigned long long)c->x[i + 1],
               i + 2, (unsigned long long)c->x[i + 2], i + 3, (unsigned long long)c->x[i + 3]);
    rt_log("  x28 %#llx", (unsigned long long)c->x[28]);
    rt_log("guest backtrace (frame-pointer chain):");
    uint64_t fp = c->x[29];
    for (int i = 0; i < 48 && fp && !(fp & 7) && readable(fp, 16); i++) {
        uint64_t next = LD64(fp), lr = LD64(fp + 8);
        if (!lr) break;
        rt_log("  #%d %s", i, rt_guest_name(lr - 4, buf, sizeof buf));
        if (next <= fp) break;
        fp = next;
    }
    if (g_fnprof) {
        rt_log("last lifted function entries (newest first):");
        for (int i = 1; i <= 64; i++) {
            uint64_t a = t->ring[(t->ring_pos - i) & 63];
            if (a) rt_log("  %s", rt_guest_name(a, buf, sizeof buf));
        }
    }
}

__attribute__((noreturn)) static void die(const char *msg) {
    static volatile LONG dying;
    if (InterlockedExchange(&dying, 1)) Sleep(INFINITE);
    if (g_log && g_log != stderr) fflush(g_log);
    fprintf(stderr, "RumbleSlop: %s\n", msg);
    // A player gets a message; a scripted run (BR_NO_REAL_INPUT, or BR_FATAL_BOX=0) must not wait for a click.
    const char *box = getenv("BR_FATAL_BOX"), *test = getenv("BR_NO_REAL_INPUT");
    if (box ? *box != '0' : !(test && *test && *test != '0')) {
        static char text[2600];
        snprintf(text, sizeof text, "The game has stopped because of an error.\n\n%.1800s\n\nDetails are in the file port.log in the game folder:\n%s",
                 msg, rt_exe_dir());
        MessageBoxA(NULL, text, GAME_NAME, MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
    if (getenv("BR_FATAL_BREAK")) DebugBreak();
    TerminateProcess(GetCurrentProcess(), 70);
    for (;;) Sleep(1000);
}

void rt_fatal(const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    g_log_max = ~0ull;
    rt_log("FATAL: %s", msg);
    if (rt_image_size) rt_backtrace(rt_cpu());
    die(msg);
}

void rt_fatal_c(Cpu *c, const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    g_log_max = ~0ull;
    rt_log("FATAL: %s", msg);
    rt_backtrace(c);
    die(msg);
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    char b[300];
    c->pc = pc;
    rt_fatal_c(c, "guest BRK #%#x at %s", imm, rt_guest_name(pc, b, sizeof b));
}
void rt_udf(Cpu *c, uint64_t pc) {
    char b[300];
    c->pc = pc;
    rt_fatal_c(c, "guest reached undefined/unlifted code at %s", rt_guest_name(pc, b, sizeof b));
}
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    char b[300];
    c->pc = pc;
    rt_fatal_c(c, "guest SVC #%#x at %s (no kernel is emulated)", imm, rt_guest_name(pc, b, sizeof b));
}
uint64_t rt_ticks(void) {
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    return (uint64_t)(((__uint128_t)q.QuadPart * 19200000u) / (uint64_t)f.QuadPart);   // Switch tick: 19.2 MHz
}
void rt_spin_gate(Cpu *c, uint64_t head) { (void)c; (void)head; YieldProcessor(); }

// What a fault in lifted code leaves to look at: the memory behind every guest register (with the class name when
// the first word looks like a managed object's class pointer), and what every other guest thread is doing.
static void crash_text(uint64_t a, char *out, int size) {
    int n = 0;
    for (; n < size - 1 && readable(a + (uint64_t)n, 1); n++) {
        char ch = *(const char *)(uintptr_t)(a + (uint64_t)n);
        if (ch < 0x20 || ch > 0x7e) break;
        out[n] = ch;
    }
    out[n] = 0;
}
static void crash_words(const char *what, int i, uint64_t p) {
    if (p < 0x10000 || (p & 7) || !readable(p, 64)) return;
    const uint64_t *w = (const uint64_t *)(uintptr_t)p;
    char name[64] = "", ns[64] = "";
    uint64_t k = w[0] & ~1ull;                           // Il2CppClass: name at +0x10, namespace at +0x18
    if (k > 0x10000 && !(k & 7) && readable(k, 0x20)) {
        crash_text(LD64(k + 0x10), name, sizeof name);
        crash_text(LD64(k + 0x18), ns, sizeof ns);
    }
    rt_log("  [%s%d] %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %s%s%s", what, i, (unsigned long long)w[0],
           (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3], (unsigned long long)w[4],
           (unsigned long long)w[5], (unsigned long long)w[6], (unsigned long long)w[7], ns, *ns ? "." : "", name);
}
static void crash_report(Cpu *c) {
    for (int i = 0; i < 29; i++) {
        uint64_t p = c->x[i];
        crash_words("x", i, p);
        if (i < 19 || p < 0x10000 || (p & 7) || !readable(p, 64)) continue;
        for (int k = 0; k < 8; k++) {                    // one level further for the callee-saved registers
            char tag[16];
            snprintf(tag, sizeof tag, "x%d+", i);
            crash_words(tag, 8 * k, LD64(p + 8 * (uint64_t)k));
        }
    }
    int locked = rt_threads_trylock();
    rt_log("guest threads (world owner thread %lu%s):", (unsigned long)rt_world_owner, locked ? "" : "; thread list locked");
    for (RtThread *t = rt_threads; t; t = t->next) {
        if (!t->host_thread || t->host_tid == GetCurrentThreadId()) continue;
        DWORD before = SuspendThread(t->host_thread);
        char b[300];
        rt_log("thread %lu: suspend count %lu, call depth %d, %s with the collector, in host function %s, pc %s", (unsigned long)t->host_tid,
               (unsigned long)before, t->call_depth, rt_gc_thread_registered(t) ? "registered" : "NOT registered",
               t->in_host ? t->in_host : "(none)", rt_guest_name(t->cpu.pc, b, sizeof b));
        host_backtrace(t->host_thread);
        if (t->call_depth > 0 && !before) {              // running guest code right now: its registers as last stored
            rt_log("  x0 %#llx x1 %#llx x19 %#llx x20 %#llx x21 %#llx x22 %#llx x23 %#llx lr %#llx", (unsigned long long)t->cpu.x[0],
                   (unsigned long long)t->cpu.x[1], (unsigned long long)t->cpu.x[19], (unsigned long long)t->cpu.x[20],
                   (unsigned long long)t->cpu.x[21], (unsigned long long)t->cpu.x[22], (unsigned long long)t->cpu.x[23],
                   (unsigned long long)t->cpu.x[30]);
            for (int i = 19; i < 24; i++) crash_words("other x", i, t->cpu.x[i]);
        }
    }
    if (locked) rt_threads_unlock();
}

// First-chance report of host faults inside the port (wild guest pointers show up as access violations in lifted code).
static void *g_mod_lo, *g_mod_hi;
static HANDLE g_raw;
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void *ip = ep->ExceptionRecord->ExceptionAddress;
    if (g_raw && code >= 0xC0000000u) {   // raw record first: needs no CRT and almost no stack
        char b[256];
        DWORD w;
        int n = wsprintfA(b, "veh: code %08lx ip %p (GameAssembly %p) addr %p rsp %p tid %lu\r\n", code, ip, g_mod_lo,
                          (void *)ep->ExceptionRecord->ExceptionInformation[1], (void *)ep->ContextRecord->Rsp, GetCurrentThreadId());
        WriteFile(g_raw, b, (DWORD)n, &w, NULL);
    }
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
         code == EXCEPTION_STACK_OVERFLOW) && ip >= g_mod_lo && ip < g_mod_hi) {
        static volatile LONG once;
        if (code == EXCEPTION_STACK_OVERFLOW) { die("host stack overflow in lifted code"); }
        if (!InterlockedExchange(&once, 1)) {
            g_log_max = ~0ull;
            rt_log("FATAL: host exception %#lx at GameAssembly+%#llx, fault address %#llx (%s)", code,
                   (unsigned long long)((char *)ip - (char *)g_mod_lo),
                   (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1],
                   ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
            rt_backtrace(rt_cpu());
            host_backtrace(GetCurrentThread());
            crash_report(rt_cpu());
            die("host exception in lifted code (see port.log)");
        }
    }
    if (code != 0x406D1388 && code != DBG_PRINTEXCEPTION_C && code != 0x4001000A && code != 0x20474343) {   // thread names, debug output, our C++ throw
        static volatile LONG n;
        if (InterlockedIncrement(&n) <= 4) {
            HMODULE m = NULL;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, ip, &m))
                GetModuleFileNameA(m, name, sizeof name);
            rt_log("host exception %#lx outside the port: ip %p (%s+%#llx), fault address %#llx", code, ip, name,
                   (unsigned long long)((char *)ip - (char *)m), (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
            if (code == 0xc0000005 || code == 0xc0000374) { extern void heapdbg_sweep(const char *why); heapdbg_sweep("crash"); }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void rt_log_install_veh(void *module) {
    MEMORY_BASIC_INFORMATION mbi;
    g_mod_lo = module;
    char *p = module;
    while (VirtualQuery(p, &mbi, sizeof mbi) && mbi.AllocationBase == module) p = (char *)mbi.BaseAddress + mbi.RegionSize;
    g_mod_hi = p;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sport_crash.log", g_exe_dir);
    g_raw = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH, NULL);
    AddVectoredExceptionHandler(1, veh);
    { extern void heapdbg_init(void); heapdbg_init(); }
}
