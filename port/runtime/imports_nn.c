// nn::os / nn::fs / misc SDK imports on Win32. Guest-side objects (MutexType, SemaphoreType, ...) keep their guest
// layout untouched; the host object lives in a side table keyed by the guest address.
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

// ------------------------------------------------------------------------------------------------ side table
enum { K_MUTEX = 1, K_COND, K_SEM, K_EVENT, K_THREAD, K_ICS };
typedef struct HostObj {
    uint64_t key;
    int kind;
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    HANDLE h;
    int manual;
    // threads
    uint64_t fn, arg, stack, stack_size;
    RtThread *rt;
    char name[64];
    struct HostObj *next;
} HostObj;
#define OBJ_BUCKETS 4096
static HostObj *g_objs[OBJ_BUCKETS];
SRWLOCK rt_obj_lock = SRWLOCK_INIT;
static uint32_t obj_hash(uint64_t k) { return (uint32_t)(((k >> 3) * 0x9E3779B97F4A7C15ull) >> 52); }

static HostObj *obj_get(uint64_t key, int kind, int create) {
    uint32_t b = obj_hash(key);
    AcquireSRWLockShared(&rt_obj_lock);
    HostObj *o = g_objs[b];
    while (o && o->key != key) o = o->next;
    ReleaseSRWLockShared(&rt_obj_lock);
    if (o && (o->kind == kind || !create)) return o;
    if (!create) return NULL;
    AcquireSRWLockExclusive(&rt_obj_lock);
    for (o = g_objs[b]; o && o->key != key; o = o->next) {}
    if (!o) {
        o = calloc(1, sizeof *o);
        o->key = key;
        o->next = g_objs[b];
        g_objs[b] = o;
        o->kind = 0;
    }
    if (o->kind != kind) {   // new, or the guest reused the storage for another kind of object
        o->kind = kind;
        InitializeCriticalSection(&o->cs);
        InitializeConditionVariable(&o->cv);
    }
    ReleaseSRWLockExclusive(&rt_obj_lock);
    return o;
}
static void obj_drop(uint64_t key) {
    uint32_t b = obj_hash(key);
    AcquireSRWLockExclusive(&rt_obj_lock);
    for (HostObj **p = &g_objs[b]; *p; p = &(*p)->next)
        if ((*p)->key == key) {
            HostObj *o = *p;
            *p = o->next;
            if (o->h) CloseHandle(o->h);
            DeleteCriticalSection(&o->cs);
            free(o);
            break;
        }
    ReleaseSRWLockExclusive(&rt_obj_lock);
}
static DWORD ns_to_ms(int64_t ns) { return ns <= 0 ? 0 : ns > 0x7fffffffll * 1000000 ? INFINITE - 1 : (DWORD)((ns + 999999) / 1000000); }

// ------------------------------------------------------------------------------------------------ mutex / cv
HOSTFN(_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi) { obj_get(X(0), K_MUTEX, 1); }
HOSTFN(_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE) { obj_drop(X(0)); }
HOSTFN(_ZN2nn2os9LockMutexEPNS0_9MutexTypeE) { EnterCriticalSection(&obj_get(X(0), K_MUTEX, 1)->cs); }
HOSTFN(_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE) { RET(TryEnterCriticalSection(&obj_get(X(0), K_MUTEX, 1)->cs) != 0); }
HOSTFN(_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE) { LeaveCriticalSection(&obj_get(X(0), K_MUTEX, 1)->cs); }
HOSTFN(_ZN2nn2os27InitializeConditionVariableEPNS0_21ConditionVariableTypeE) { obj_get(X(0), K_COND, 1); }
HOSTFN(_ZN2nn2os25FinalizeConditionVariableEPNS0_21ConditionVariableTypeE) { obj_drop(X(0)); }
HOSTFN(_ZN2nn2os21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE) {
    SleepConditionVariableCS(&obj_get(X(0), K_COND, 1)->cv, &obj_get(X(1), K_MUTEX, 1)->cs, INFINITE);
}
HOSTFN(_ZN2nn2os26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE) {
    BOOL ok = SleepConditionVariableCS(&obj_get(X(0), K_COND, 1)->cv, &obj_get(X(1), K_MUTEX, 1)->cs, ns_to_ms((int64_t)X(2)));
    RET(ok ? 1 : 0);   // ConditionVariableStatus: 0 timeout, 1 no timeout
}
HOSTFN(_ZN2nn2os26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE) { WakeAllConditionVariable(&obj_get(X(0), K_COND, 1)->cv); }
HOSTFN(_ZN2nn2os23SignalConditionVariableEPNS0_21ConditionVariableTypeE) { WakeConditionVariable(&obj_get(X(0), K_COND, 1)->cv); }
// nn::os::detail::InternalCriticalSectionImplByHorizon: a 4-byte guest object.
HOSTFN(_ZN2nn2os6detail36InternalCriticalSectionImplByHorizonC1Ev) { ST32(X(0), 0); obj_get(X(0), K_ICS, 1); }
HOSTFN(_ZN2nn2os6detail36InternalCriticalSectionImplByHorizon10InitializeEv) { ST32(X(0), 0); obj_get(X(0), K_ICS, 1); }
HOSTFN(_ZN2nn2os6detail36InternalCriticalSectionImplByHorizon5EnterEv) { EnterCriticalSection(&obj_get(X(0), K_ICS, 1)->cs); }
HOSTFN(_ZN2nn2os6detail36InternalCriticalSectionImplByHorizon5LeaveEv) { LeaveCriticalSection(&obj_get(X(0), K_ICS, 1)->cs); }

// ------------------------------------------------------------------------------------------------ semaphore / event
static HostObj *sem_get(uint64_t key) {
    HostObj *o = obj_get(key, K_SEM, 1);
    if (!o->h) o->h = CreateSemaphoreA(NULL, 0, 0x7fffffff, NULL);
    return o;
}
HOSTFN(_ZN2nn2os19InitializeSemaphoreEPNS0_13SemaphoreTypeEii) {
    HostObj *o = obj_get(X(0), K_SEM, 1);
    if (o->h) CloseHandle(o->h);
    o->h = CreateSemaphoreA(NULL, (LONG)W(1), 0x7fffffff, NULL);
}
HOSTFN(_ZN2nn2os17FinalizeSemaphoreEPNS0_13SemaphoreTypeE) { obj_drop(X(0)); }
HOSTFN(_ZN2nn2os16AcquireSemaphoreEPNS0_13SemaphoreTypeE) { WaitForSingleObject(sem_get(X(0))->h, INFINITE); }
HOSTFN(_ZN2nn2os19TryAcquireSemaphoreEPNS0_13SemaphoreTypeE) { RET(WaitForSingleObject(sem_get(X(0))->h, 0) == WAIT_OBJECT_0); }
HOSTFN(_ZN2nn2os21TimedAcquireSemaphoreEPNS0_13SemaphoreTypeENS_8TimeSpanE) {
    RET(WaitForSingleObject(sem_get(X(0))->h, ns_to_ms((int64_t)X(1))) == WAIT_OBJECT_0);
}
HOSTFN(_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeE) { ReleaseSemaphore(sem_get(X(0))->h, 1, NULL); }
HOSTFN(_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeEi) { ReleaseSemaphore(sem_get(X(0))->h, (LONG)W(1), NULL); }

static HostObj *ev_get(uint64_t key) {
    HostObj *o = obj_get(key, K_EVENT, 1);
    if (!o->h) { o->h = CreateEventA(NULL, TRUE, FALSE, NULL); o->manual = 1; }
    return o;
}
HOSTFN(_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE) {   // clear mode: 0 manual, 1 auto
    HostObj *o = obj_get(X(0), K_EVENT, 1);
    if (o->h) CloseHandle(o->h);
    o->manual = W(2) == 0;
    o->h = CreateEventA(NULL, o->manual, (W(1) & 1) != 0, NULL);
}
HOSTFN(_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE) { obj_drop(X(0)); }
HOSTFN(_ZN2nn2os11SignalEventEPNS0_9EventTypeE) { SetEvent(ev_get(X(0))->h); }
HOSTFN(_ZN2nn2os10ClearEventEPNS0_9EventTypeE) { ResetEvent(ev_get(X(0))->h); }
HOSTFN(_ZN2nn2os9WaitEventEPNS0_9EventTypeE) { WaitForSingleObject(ev_get(X(0))->h, INFINITE); }
HOSTFN(_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE) { RET(WaitForSingleObject(ev_get(X(0))->h, 0) == WAIT_OBJECT_0); }
HOSTFN(_ZN2nn2os14TimedWaitEventEPNS0_9EventTypeENS_8TimeSpanE) {
    RET(WaitForSingleObject(ev_get(X(0))->h, ns_to_ms((int64_t)X(1))) == WAIT_OBJECT_0);
}

// nn::os::SystemEvent / SystemEventType: the class holds its SystemEventType at offset 0, so both are keyed by the
// same address. The host creates them (audio output, device notifications); the guest only waits and destroys.
void rt_os_event_create(uint64_t key, int manual) {
    HostObj *o = obj_get(key, K_EVENT, 1);
    if (o->h) CloseHandle(o->h);
    o->manual = manual;
    o->h = CreateEventA(NULL, manual, FALSE, NULL);
}
void rt_os_event_signal(uint64_t key) { HostObj *o = obj_get(key, K_EVENT, 0); if (o && o->h) SetEvent(o->h); }
HOSTFN(_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE) { WaitForSingleObject(ev_get(X(0))->h, INFINITE); }
HOSTFN(_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE) { RET(WaitForSingleObject(ev_get(X(0))->h, 0) == WAIT_OBJECT_0); }
HOSTFN(_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE) { obj_drop(X(0)); }

// MultiWait over events: the MultiWaitType and each MultiWaitHolderType are keys; a holder remembers its event.
// WaitAny must not consume the signal (the caller waits on the event itself afterwards), so an auto-clear event
// that satisfied the wait is signalled again.
enum { K_MWAIT = 16, K_MHOLDER };
HOSTFN(_ZN2nn2os19InitializeMultiWaitEPNS0_13MultiWaitTypeE) { obj_get(X(0), K_MWAIT, 1); }
HOSTFN(_ZN2nn2os17FinalizeMultiWaitEPNS0_13MultiWaitTypeE) { obj_drop(X(0)); }
HOSTFN(_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE) {
    HostObj *o = obj_get(X(0), K_MHOLDER, 1);
    o->arg = X(1);   // the event
    o->fn = 0;       // the multi-wait it is linked to
}
HOSTFN(_ZN2nn2os23FinalizeMultiWaitHolderEPNS0_19MultiWaitHolderTypeE) { obj_drop(X(0)); }
HOSTFN(_ZN2nn2os19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE) { obj_get(X(1), K_MHOLDER, 1)->fn = X(0); }
HOSTFN(_ZN2nn2os21UnlinkMultiWaitHolderEPNS0_19MultiWaitHolderTypeE) { obj_get(X(0), K_MHOLDER, 1)->fn = 0; }
HOSTFN(_ZN2nn2os7WaitAnyEPNS0_13MultiWaitTypeE) {
    HANDLE hs[MAXIMUM_WAIT_OBJECTS];
    HostObj *holders[MAXIMUM_WAIT_OBJECTS];
    DWORD n = 0;
    AcquireSRWLockShared(&rt_obj_lock);
    for (int b = 0; b < OBJ_BUCKETS; b++)
        for (HostObj *o = g_objs[b]; o; o = o->next)
            if (o->kind == K_MHOLDER && o->fn == X(0) && n < MAXIMUM_WAIT_OBJECTS) holders[n++] = o;
    ReleaseSRWLockShared(&rt_obj_lock);
    if (!n) rt_fatal_c(c, "nn::os::WaitAny: no holders linked");
    for (DWORD i = 0; i < n; i++) hs[i] = ev_get(holders[i]->arg)->h;
    DWORD r = WaitForMultipleObjects(n, hs, FALSE, INFINITE);
    if (r >= n) rt_fatal_c(c, "nn::os::WaitAny: wait failed (%lu)", GetLastError());
    if (!ev_get(holders[r]->arg)->manual) SetEvent(hs[r]);
    RET(holders[r]->key);
}

// ------------------------------------------------------------------------------------------------ TLS
#define TLS_SLOTS 256
static uint64_t g_tls_dtor[TLS_SLOTS];
static DWORD g_tls_index[TLS_SLOTS];
static volatile LONG g_tls_used[TLS_SLOTS];
HOSTFN(_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE) {
    for (uint32_t i = 0; i < TLS_SLOTS; i++)
        if (!InterlockedCompareExchange(&g_tls_used[i], 1, 0)) {
            g_tls_index[i] = TlsAlloc();
            g_tls_dtor[i] = X(1);
            ST32(X(0), i);
            RET(0);
            return;
        }
    rt_fatal_c(c, "nn::os::AllocateTlsSlot: out of slots");
}
HOSTFN(_ZN2nn2os11FreeTlsSlotENS0_7TlsSlotE) { uint32_t i = W(0); if (i < TLS_SLOTS && g_tls_used[i]) { TlsFree(g_tls_index[i]); g_tls_used[i] = 0; } }
HOSTFN(_ZN2nn2os11GetTlsValueENS0_7TlsSlotE) { uint32_t i = W(0); RET(i < TLS_SLOTS && g_tls_used[i] ? (uint64_t)(uintptr_t)TlsGetValue(g_tls_index[i]) : 0); }
HOSTFN(_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm) { uint32_t i = W(0); if (i < TLS_SLOTS && g_tls_used[i]) TlsSetValue(g_tls_index[i], (void *)(uintptr_t)X(1)); }

// ------------------------------------------------------------------------------------------------ threads
static DWORD WINAPI thread_main(void *p) {
    HostObj *o = p;
    RtThread *t = o->rt;
    rt_thread_bind(t);
    memcpy(t->tlr + 0x1f8, &o->key, 8);     // this thread's ThreadType is the guest's own object
    if (rt_trace & 16) rt_log("thread: guest thread '%s' starts at main+%#llx", o->name, (unsigned long long)(o->fn - GUEST_BASE));
    rt_call(&t->cpu, o->fn, 1, o->arg);
    if (t->cpu.eh_unwinding) {
        char b[512];
        rt_fatal_c(&t->cpu, "unhandled guest exception (%s) in thread '%s'", rt_eh_describe(&t->cpu, b, sizeof b), o->name);
    }
    for (uint32_t i = 0; i < TLS_SLOTS; i++) {   // TLS destructors
        void *v;
        if (g_tls_used[i] && g_tls_dtor[i] && (v = TlsGetValue(g_tls_index[i])) != NULL) {
            TlsSetValue(g_tls_index[i], NULL);
            rt_call(&t->cpu, g_tls_dtor[i], 1, (uint64_t)(uintptr_t)v);
        }
    }
    if (rt_trace & 16) rt_log("thread: guest thread '%s' exits", o->name);
    return 0;
}
static void create_thread(Cpu *c) {   // (ThreadType*, fn, arg, stack, stackSize, priority[, core])
    HostObj *o = obj_get(X(0), K_THREAD, 1);
    o->fn = X(1); o->arg = X(2); o->stack = X(3); o->stack_size = X(4);
    o->rt = rt_thread_create(o->stack, o->stack_size);
    rt_thread_type_init(X(0), o->stack, o->stack_size);
    snprintf(o->name, sizeof o->name, "guest-%llx", (unsigned long long)X(0));
    o->h = CreateThread(NULL, 16u << 20, thread_main, o, CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!o->h) rt_fatal_c(c, "nn::os::CreateThread: CreateThread failed (%lu)", GetLastError());
    RET(0);
}
HOSTFN(_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi) { create_thread(c); }
HOSTFN(_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii) { create_thread(c); }
HOSTFN(_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE) { HostObj *o = obj_get(X(0), K_THREAD, 0); if (o && o->h) ResumeThread(o->h); }
HOSTFN(_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE) { HostObj *o = obj_get(X(0), K_THREAD, 0); if (o && o->h) WaitForSingleObject(o->h, INFINITE); }
HOSTFN(_ZN2nn2os13TryWaitThreadEPNS0_10ThreadTypeE) { HostObj *o = obj_get(X(0), K_THREAD, 0); RET(!o || !o->h || WaitForSingleObject(o->h, 0) == WAIT_OBJECT_0); }
HOSTFN(_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE) {
    HostObj *o = obj_get(X(0), K_THREAD, 0);
    if (o && o->h) WaitForSingleObject(o->h, INFINITE);
    obj_drop(X(0));
}
HOSTFN(_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc) { HostObj *o = obj_get(X(0), K_THREAD, 0); if (o && X(1)) snprintf(o->name, sizeof o->name, "%s", STR(1)); }
HOSTFN(_ZN2nn2os20GetThreadNamePointerEPKNS0_10ThreadTypeE) { HostObj *o = obj_get(X(0), K_THREAD, 0); RET(o ? o->name : "MainThread"); }
HOSTFN(_ZN2nn2os17SetThreadCoreMaskEPNS0_10ThreadTypeEim) { (void)c; }
HOSTFN(_ZN2nn2os20ChangeThreadPriorityEPNS0_10ThreadTypeEi) { RET(0); }
HOSTFN(_ZN2nn2os16GetCurrentThreadEv) { uint64_t tt; memcpy(&tt, ((RtThread *)c)->tlr + 0x1f8, 8); RET(tt); }
HOSTFN(_ZN2nn2os11YieldThreadEv) { (void)c; SwitchToThread(); }
HOSTFN(_ZN2nn2os11SleepThreadENS_8TimeSpanE) { Sleep(ns_to_ms((int64_t)X(0))); }
HOSTFN(_ZN2nn2os19GetCurrentStackInfoEPmS1_) { RtThread *t = (RtThread *)c; ST64(X(0), t->stack_lo); ST64(X(1), t->stack_hi - t->stack_lo); }
HOSTFN(engine_gc_stack_base) { RET(((RtThread *)c)->stack_hi); }   // main+0x10f6290: stack address + size

// ------------------------------------------------------------------------------------------------ time / misc
HOSTFN(_ZN2nn2os13GetSystemTickEv) { RET(rt_ticks()); }
HOSTALIAS(_ZN2nn2os20GetSystemTickOrderedEv, _ZN2nn2os13GetSystemTickEv)
HOSTFN(_ZN2nn2os22GetSystemTickFrequencyEv) { RET(19200000); }
HOSTFN(_ZN2nn2os17ConvertToTimeSpanENS0_4TickE) { RET((uint64_t)(((__int128)(int64_t)X(0) * 1000000000) / 19200000)); }
HOSTFN(_ZN2nn2os19GenerateRandomBytesEPvm) { BCryptGenRandom(NULL, PTR(0), (ULONG)X(1), BCRYPT_USE_SYSTEM_PREFERRED_RNG); }
HOSTFN(_ZN2nn2os21AllocateAddressRegionEPmm) {
    void *p = VirtualAlloc(NULL, X(1), MEM_RESERVE, PAGE_NOACCESS);
    ST64(X(0), (uint64_t)(uintptr_t)p);
    RET(p ? 0 : 0x1003);
}
HOSTFN(_ZN2nn2os17FreeAddressRegionEm) { VirtualFree(PTR(0), 0, MEM_RELEASE); RET(0); }
HOSTFN(_ZN2nn2os19AllocateMemoryPagesEmm) { RET(VirtualAlloc(PTR(0), X(1), MEM_COMMIT, PAGE_READWRITE) ? 0 : 0x1003); }
HOSTFN(_ZN2nn2os15FreeMemoryPagesEmm) { VirtualFree(PTR(0), X(1), MEM_DECOMMIT); RET(0); }
HOSTFN(_ZN2nn2os19SetMemoryPermissionEmmNS0_16MemoryPermissionE) {   // 0 none, 1 read, 3 read/write
    DWORD old;
    VirtualProtect(PTR(0), X(1), W(2) == 0 ? PAGE_NOACCESS : W(2) == 1 ? PAGE_READONLY : PAGE_READWRITE, &old);
}
HOSTFN(_ZN2nn4diag6detail9AbortImplEv) { rt_fatal_c(c, "guest abort (nn::diag::detail::AbortImpl)"); }
HOSTFN(_ZN2nn4diag6detail9AbortImplEPKNS_6ResultE) { rt_fatal_c(c, "guest abort with nn::Result %#x", X(0) ? LD32(X(0)) : 0); }
HOSTFN(_ZN2nn4diag12GetBacktraceEPmi) { RET(0); }

// nn::time::CalendarTime {s16 year; s8 month, day, hour, minute, second}; PosixTime {s64}.
HOSTFN(_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE) {
    int64_t t = (int64_t)LD64(X(2));
    FILETIME ft, lft;
    SYSTEMTIME st;
    uint64_t v = (uint64_t)t * 10000000ull + 116444736000000000ull;
    ft.dwLowDateTime = (DWORD)v; ft.dwHighDateTime = (DWORD)(v >> 32);
    FileTimeToLocalFileTime(&ft, &lft);
    FileTimeToSystemTime(&lft, &st);
    if (X(0)) {
        ST16(X(0), st.wYear); ST8(X(0) + 2, st.wMonth); ST8(X(0) + 3, st.wDay); ST8(X(0) + 4, st.wHour); ST8(X(0) + 5, st.wMinute);
        ST8(X(0) + 6, st.wSecond);
    }
    if (X(1)) {   // {s32 dayOfWeek; s32 yearDay; TimeZone {char name[8]; bool isDst; s32 utcOffsetSeconds}}
        TIME_ZONE_INFORMATION tz;
        DWORD r = GetTimeZoneInformation(&tz);
        memset(PTR(1), 0, 0x18);
        ST32(X(1), st.wDayOfWeek);
        memcpy((char *)PTR(1) + 8, "LOC", 4);
        ST8(X(1) + 16, r == TIME_ZONE_ID_DAYLIGHT);
        ST32(X(1) + 20, (uint32_t)(-(tz.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : tz.StandardBias)) * 60));
    }
    RET(0);
}
HOSTFN(_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeE) {
    SYSTEMTIME st = {0}, ust;
    FILETIME ft;
    st.wYear = (WORD)(int16_t)LD16(X(3)); st.wMonth = LD8(X(3) + 2); st.wDay = LD8(X(3) + 3); st.wHour = LD8(X(3) + 4);
    st.wMinute = LD8(X(3) + 5); st.wSecond = LD8(X(3) + 6);
    TzSpecificLocalTimeToSystemTime(NULL, &st, &ust);
    SystemTimeToFileTime(&ust, &ft);
    int64_t t = (int64_t)((((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull) / 10000000ull);
    if (X(0)) ST32(X(0), 1);
    if (W(2) > 0) ST64(X(1), (uint64_t)t);
    RET(0);
}

// ------------------------------------------------------------------------------------------------ engine helpers
HOSTFN(engine_flag_10f7fc0) { RET(0); }   // main+0x10fc5c0: engine-side boolean read by the path helper (result unused there)

// main+0x10fca10: current UTC time in 100 ns ticks since 0001-01-01 (the lifted caller subtracts the 1601 offset).
HOSTFN(engine_datetime_ticks) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    RET(((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) + 504911232000000000ull);
}
