// Steam as a carrier for online play (netplay.c): datagrams to a Steam account through Valve's relay network
// (ISteamNetworkingMessages). Nothing else of Steam is used: no friends list, rich presence, lobbies, achievements,
// stats or overlay. A Steam room's code holds the host's account number; players pass codes to each other.
//
// The port runs under Spacewar (app 480), Valve's shared test app, so Steam reports "playing Spacewar" and nothing
// goes to any publisher. Steam is started only when online play needs it (hosting or joining a Steam room) and shut down again a minute after the last room ended, so offline play never shows as playing.
//
// steam_api64.dll is Valve's; it is not part of the repo. It is looked for next to the exe, then (copied from there
// once) in the player's own Steam games: any game that ships a recent enough steam_api64.dll will do.
// Everything is loaded at runtime through the flat C API (no SDK headers): the few structures used are written out
// below with their Windows layouts.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netplay.h"

#define APP_ID "480"
#define CB_SESSION_REQUEST 1251             // SteamNetworkingMessagesSessionRequest_t { SteamNetworkingIdentity }
#define CB_SESSION_FAILED 1252
#define SEND_FLAGS (0 /* unreliable */ | 1 /* no Nagle */ | 32 /* restart a broken session */)
#define CHANNEL 0
#define IDLE_SHUTDOWN_MS 60000

typedef struct { uint8_t b[136]; } Ident;   // SteamNetworkingIdentity: type, size, 128-byte union
typedef struct { int32_t user, callback; uint8_t *param; int32_t size; } CbMsg;   // CallbackMsg_t
// SteamNetworkingMessage_t (Windows layout): data, size, connection, peer identity at +16, ..., release function
#define MSG_DATA 0
#define MSG_SIZE 8
#define MSG_PEER 16

static struct {
    HMODULE dll;
    int (*Init)(void);                       // SDKs up to 1.57
    int (*InitFlat)(char *err1024);          // 1.58 and later: 0 = OK
    void (*Shutdown)(void);
    int32_t (*GetPipe)(void);
    void (*MdInit)(void), (*MdRunFrame)(int32_t), (*MdFree)(int32_t);
    int (*MdNext)(int32_t, CbMsg *);
    void *(*User)(void), *(*Msgs)(void), *(*NUtils)(void);
    uint64_t (*GetSteamID)(void *);
    int (*Send)(void *, const Ident *, const void *, uint32_t, int, int);
    int (*Recv)(void *, int, void **, int);
    int (*Accept)(void *, const Ident *);
    int (*CloseSession)(void *, const Ident *);
    void (*Release)(void *);
    void (*IdSet)(Ident *, uint64_t);
    uint64_t (*IdGet)(const Ident *);
    void (*InitRelay)(void *);
} S;
static CRITICAL_SECTION g_lock;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static int g_loaded = -1;                    // -1 not tried, 0 no usable dll, 1 loaded
static volatile int g_up;                    // SteamAPI_Init succeeded and not shut down
static uint64_t g_self, g_last_use;
static volatile LONG g_users;                // sessions using Steam right now

static BOOL CALLBACK once_init(PINIT_ONCE o, PVOID p, PVOID *ctx) { (void)o; (void)p; (void)ctx; InitializeCriticalSection(&g_lock); return TRUE; }
static uint64_t now_ms(void) { return GetTickCount64(); }

static void exe_dir(char *out, size_t n) {
    GetModuleFileNameA(NULL, out, (DWORD)n);
    char *s = strrchr(out, '\\');
    if (s) s[1] = 0; else out[0] = 0;
}
#define LOAD(field, name) if (!(*(FARPROC *)&S.field = GetProcAddress(h, name))) { np_logf("steam: %s lacks %s", path, name); goto bad; }
// the first of two export names (interface versions differ between SDK releases; the flat signatures used are equal)
#define LOAD2(field, a, b) if (!(*(FARPROC *)&S.field = GetProcAddress(h, a)) && !(*(FARPROC *)&S.field = GetProcAddress(h, b))) { np_logf("steam: %s lacks %s", path, a); goto bad; }
static int try_dll(const char *path) {
    HMODULE h = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) return 0;
    *(FARPROC *)&S.Init = GetProcAddress(h, "SteamAPI_Init");
    *(FARPROC *)&S.InitFlat = GetProcAddress(h, "SteamAPI_InitFlat");
    if (!S.Init && !S.InitFlat) { np_logf("steam: %s has no SteamAPI_Init", path); goto bad; }
    LOAD(Shutdown, "SteamAPI_Shutdown");
    LOAD(GetPipe, "SteamAPI_GetHSteamPipe");
    LOAD(MdInit, "SteamAPI_ManualDispatch_Init");
    LOAD(MdRunFrame, "SteamAPI_ManualDispatch_RunFrame");
    LOAD(MdNext, "SteamAPI_ManualDispatch_GetNextCallback");
    LOAD(MdFree, "SteamAPI_ManualDispatch_FreeLastCallback");
    LOAD2(User, "SteamAPI_SteamUser_v023", "SteamAPI_SteamUser_v021");
    LOAD(Msgs, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    LOAD2(NUtils, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004", "SteamAPI_SteamNetworkingUtils_SteamAPI_v003");
    LOAD(GetSteamID, "SteamAPI_ISteamUser_GetSteamID");
    LOAD(Send, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    LOAD(Recv, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    LOAD(Accept, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    LOAD(CloseSession, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
    LOAD(Release, "SteamAPI_SteamNetworkingMessage_t_Release");
    LOAD(IdSet, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    LOAD(IdGet, "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
    LOAD(InitRelay, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    S.dll = h;
    return 1;
bad:
    FreeLibrary(h);
    memset(&S, 0, sizeof S);
    return 0;
}

// Depth-limited search of a Steam library's games for steam_api64.dll that has every function used here.
static int search(const char *dir, int depth, char *found, size_t n) {
    char pat[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\steam_api64.dll", dir);
    if (GetFileAttributesA(pat) != INVALID_FILE_ATTRIBUTES && try_dll(pat)) { snprintf(found, n, "%s", pat); return 1; }
    if (depth <= 0) return 0;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        char sub[MAX_PATH * 2];
        snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
        ok = search(sub, depth - 1, found, n);
    } while (!ok && FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}
static int search_steam_libraries(char *found, size_t n) {
    char steam[MAX_PATH] = "", vdf[MAX_PATH * 2];
    DWORD sz = sizeof steam;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, NULL, steam, &sz) != ERROR_SUCCESS) return 0;
    for (char *p = steam; *p; p++) if (*p == '/') *p = '\\';
    char libs[16][MAX_PATH];
    int nlib = 0;
    snprintf(libs[nlib++], MAX_PATH, "%s", steam);
    snprintf(vdf, sizeof vdf, "%s\\steamapps\\libraryfolders.vdf", steam);
    FILE *f = fopen(vdf, "r");
    if (f) {                                                         // lines like:  "path"  "D:\\SteamLibrary"
        char line[1024];
        while (nlib < 16 && fgets(line, sizeof line, f)) {
            char *k = strstr(line, "\"path\"");
            if (!k) continue;
            char *a = strchr(k + 6, '"'), *b = a ? strchr(a + 1, '"') : NULL;
            if (!a || !b) continue;
            char *o = libs[nlib];
            for (char *q = a + 1; q < b && o < libs[nlib] + MAX_PATH - 1; q++) { if (q[0] == '\\' && q[1] == '\\') q++; *o++ = *q; }
            *o = 0;
            if (_stricmp(libs[nlib], steam)) nlib++;
        }
        fclose(f);
    }
    for (int i = 0; i < nlib; i++) {
        char common[MAX_PATH * 2];
        snprintf(common, sizeof common, "%s\\steamapps\\common", libs[i]);
        if (search(common, 5, found, n)) return 1;
    }
    return 0;
}
static int load(void) {
    if (g_loaded >= 0) return g_loaded;
    char dir[MAX_PATH], path[MAX_PATH * 2], found[MAX_PATH * 2];
    exe_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%ssteam_api64.dll", dir);
    g_loaded = try_dll(path);
    if (g_loaded) np_logf("steam: using %s", path);
    else if (search_steam_libraries(found, sizeof found)) {
        // keep a copy next to the game so the search runs once; the loaded one stays in use this time
        np_logf("steam: found %s, %s", found, CopyFileA(found, path, TRUE) ? "copied next to the game" : "(could not copy it next to the game)");
        g_loaded = 1;
    } else np_logf("steam: no usable steam_api64.dll next to the game or in the Steam libraries: online play over Steam is off");
    return g_loaded;
}

// ---------------------------------------------------------------------------------------------------- the worker
// One thread makes every Steam call (start, dispatch, send, receive, shut down). Steam's client
// reports internal assertions through OutputDebugString, which can stall for seconds; on its own thread that only
// delays packets, while a stall inside the room lock froze the game. The rest of the port only touches the queues.
typedef struct Pkt { struct Pkt *next; uint64_t peer; int len; uint8_t data[]; } Pkt;
static Pkt *g_out_head, *g_out_tail, *g_in_head, *g_in_tail;
static int g_in_count;
static HANDLE g_thread, g_ready;
static volatile LONG g_stop;

static void q_push(Pkt **head, Pkt **tail, Pkt *p) {
    p->next = NULL;
    if (*tail) (*tail)->next = p; else *head = p;
    *tail = p;
}
static Pkt *q_pop(Pkt **head, Pkt **tail) {
    Pkt *p = *head;
    if (p) { *head = p->next; if (!*head) *tail = NULL; }
    return p;
}
static void q_free(Pkt **head, Pkt **tail) { Pkt *p; while ((p = q_pop(head, tail))) free(p); }

static void worker_pump(void) {
    int32_t pipe = S.GetPipe();
    CbMsg m;
    S.MdRunFrame(pipe);
    while (S.MdNext(pipe, &m)) {
        // Accept every message session: who may join is decided by the room handshake (the code's secret), as on UDP.
        if (m.callback == CB_SESSION_REQUEST && m.param && m.size >= (int)sizeof(Ident)) S.Accept(S.Msgs(), (const Ident *)m.param);
        else if (m.callback == CB_SESSION_FAILED) {                // the peer is unreachable (also: one's own account)
            static int n;
            if (n++ < 3) np_logf("steam: a message session failed%s", n == 3 ? " (not logged again)" : "");
        }
        S.MdFree(pipe);
    }
}
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    char err[1024] = "";
    int up = 0;
    if (load()) {
        SetEnvironmentVariableA("SteamAppId", APP_ID);
        SetEnvironmentVariableA("SteamGameId", APP_ID);
        if (S.InitFlat ? S.InitFlat(err) == 0 : S.Init()) {
            S.MdInit();
            g_self = S.GetSteamID(S.User());
            S.InitRelay(S.NUtils());
            up = 1;
            np_logf("steam: started (app " APP_ID ", account %llu)", (unsigned long long)(g_self & 0xffffffffull));
        } else np_logf("steam: could not start (is Steam running and signed in?) %s", err);
    }
    g_up = up;
    SetEvent(g_ready);
    uint8_t *buf = malloc(4096);
    while (up && buf && !g_stop) {
        worker_pump();
        // out: everything queued, without holding the lock during the calls
        Pkt *list;
        EnterCriticalSection(&g_lock);
        list = g_out_head; g_out_head = g_out_tail = NULL;
        int idle = !g_users && now_ms() - g_last_use > IDLE_SHUTDOWN_MS;
        LeaveCriticalSection(&g_lock);
        while (list) {
            Pkt *p = list;
            list = p->next;
            Ident id;
            memset(&id, 0, sizeof id);
            S.IdSet(&id, p->peer);
            S.Send(S.Msgs(), &id, p->data, (uint32_t)p->len, SEND_FLAGS, CHANNEL);
            free(p);
        }
        // in
        for (int k = 0; k < 256; k++) {
            void *m = NULL;
            if (S.Recv(S.Msgs(), CHANNEL, &m, 1) != 1 || !m) break;
            const uint8_t *mb = m;
            const void *data;
            int32_t size;
            memcpy(&data, mb + MSG_DATA, sizeof data);
            memcpy(&size, mb + MSG_SIZE, 4);
            uint64_t from = S.IdGet((const Ident *)(mb + MSG_PEER));
            Pkt *p = data && size > 0 && size <= 2048 && from ? malloc(sizeof *p + (size_t)size) : NULL;
            if (p) { p->peer = from; p->len = size; memcpy(p->data, data, (size_t)size); }
            S.Release(m);
            if (!p) continue;
            EnterCriticalSection(&g_lock);
            if (g_in_count < 4096) { q_push(&g_in_head, &g_in_tail, p); g_in_count++; p = NULL; }   // nobody reads: drop
            LeaveCriticalSection(&g_lock);
            free(p);
        }
        if (idle) break;
        Sleep(1);
    }
    if (up) {
        S.Shutdown();
        np_logf("steam: shut down%s", g_stop ? "" : " (online play not used for a minute)");
    }
    free(buf);
    EnterCriticalSection(&g_lock);
    g_up = 0;
    q_free(&g_out_head, &g_out_tail);
    q_free(&g_in_head, &g_in_tail);
    g_in_count = 0;
    LeaveCriticalSection(&g_lock);
    return 0;
}

// ---------------------------------------------------------------------------------------------------- interface
// Starts the worker when it is not running; wait: for Steam to come up (at most 5 s). 1 when Steam is up.
static int start(int wait) {
    InitOnceExecuteOnce(&g_once, once_init, NULL, NULL);
    EnterCriticalSection(&g_lock);
    g_last_use = now_ms();
    int running = g_thread && WaitForSingleObject(g_thread, 0) == WAIT_TIMEOUT;
    if (!running) {
        static uint64_t last_try;
        if (g_thread) { CloseHandle(g_thread); g_thread = NULL; }
        if (!last_try || now_ms() - last_try > 5000) {             // do not retry a failing start every frame
            last_try = now_ms();
            if (!g_ready) g_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
            ResetEvent(g_ready);
            g_up = 0;
            g_stop = 0;
            g_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        }
    }
    HANDLE ready = g_thread ? g_ready : NULL;
    LeaveCriticalSection(&g_lock);
    if (ready && wait) WaitForSingleObject(ready, 5000);
    return g_up;
}
int sn_available(void) { return start(1); }
uint64_t sn_self(void) { return g_up ? g_self : 0; }
void sn_use(int on) { InterlockedAdd(&g_users, on ? 1 : -1); g_last_use = now_ms(); }

int sn_send(uint64_t to, const void *p, int n) {
    if (!g_up || n <= 0 || n > 2048) return 0;
    Pkt *k = malloc(sizeof *k + (size_t)n);
    if (!k) return 0;
    k->peer = to; k->len = n;
    memcpy(k->data, p, (size_t)n);
    EnterCriticalSection(&g_lock);
    q_push(&g_out_head, &g_out_tail, k);
    LeaveCriticalSection(&g_lock);
    return 1;
}
// One datagram into buf (at most cap bytes; longer ones are dropped). Returns its length, or -1 when none waits.
int sn_recv(uint8_t *buf, int cap, uint64_t *from) {
    if (!g_up) return -1;
    for (;;) {
        EnterCriticalSection(&g_lock);
        Pkt *p = q_pop(&g_in_head, &g_in_tail);
        if (p) g_in_count--;
        LeaveCriticalSection(&g_lock);
        if (!p) return -1;
        int n = p->len;
        if (n <= cap) { memcpy(buf, p->data, (size_t)n); *from = p->peer; free(p); return n; }
        free(p);
    }
}
void sn_pump(void) {}
void sn_close(uint64_t peer) { (void)peer; }

// Once a frame from the game: nothing to do (the worker runs and stops by itself); kept for the interface.
void sn_tick(void) {}
// Before the process exits (il2cpp_shutdown): Steam must be shut down by its own thread while threads still run;
// a process that exits with Steam up hangs in Steam's unload.
void sn_shutdown(void) {
    HANDLE t = g_thread;
    if (!t || WaitForSingleObject(t, 0) != WAIT_TIMEOUT) return;
    g_stop = 1;
    if (WaitForSingleObject(t, 3000) != WAIT_OBJECT_0) np_logf("steam: did not shut down in time");
}
