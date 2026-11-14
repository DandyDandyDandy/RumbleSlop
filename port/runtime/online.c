// PC online play: private rooms joined by a code, for the game's Friend Match modes (Adventure and Battle).
//
// The game talks to three facade objects (GCS1.Network.NPLN: NplnAPIFacade, SessionAPI, NplnMatchmakingAPI) that sit
// on Nintendo's NPLN stack, which is not part of the port. The entry hooks below answer those facades from the
// transport in netplay.c while a room is being entered or is entered. While idle every hook lets the original run:
// with no session server the originals take their own "not online" paths, so offline play is untouched.
//
// What the game sees:
//   RequestMatchmaking(FriendHostMatchmakingContext)      -> create a room; MultiplayAPI.SessionCode is the code
//   RequestMatchmaking(ShortSessionIdMatchmakingContext)  -> join the room the typed code points to
//   any other context (random matchmaking, room lists, local wireless) -> fails at once ("could not connect")
//   IsProcessing while connecting, then IsInRoom or not; observers are notified as the NPLN layer would
//   events / streams: SendInternal, SendStreamUnreliable -> np_send; received data -> SessionAPI.OnEventReceived
// The creator of a room is its master for the room's whole life; when it leaves, the room ends for everybody.
//
// Everything managed is touched on the game's main thread only: the hooks, and online_tick (called once a frame
// from the MultiPlatformManager.LateUpdate hook in display_opt.c), which delivers what the network thread queued.
// Settings: <exe dir>port_net.txt, lines "port=23711", "upnp=1", "public_ip=1.2.3.4" (all optional).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "netplay.h"

#define FN(off) (GUEST_BASE + (off))
#define F_INIT_METADATA 0x44c0d0ull         // il2cpp_codegen_initialize_runtime_metadata(uintptr_t *slot)
#define F_CLASS_INIT 0x44c310ull            // il2cpp_codegen_runtime_class_init(klass)
#define F_SZARRAY_NEW 0x44c230ull           // SZArrayNew(array class, length)
#define F_WRITE_BARRIER 0x44c010ull         // Il2CppCodeGenWriteBarrier(void **slot, void *value)
#define F_STRING_NEW_UTF16 0x456994ull      // il2cpp::vm::String::NewUtf16(const Il2CppChar *, int32_t)
#define F_ON_EVENT_RECEIVED 0x227c010ull    // static SessionAPI.OnEventReceived(byte[] data, PeerID from, bool reliable)
#define F_NOTIFY_READY_INTERNET 0x2298aa0ull   // OnlineEventObserverExtension.NotifyReadyToUserInternet(list)
#define F_NOTIFY_DISCONNECT 0x2298e10ull       // ...NotifyOnDisconnect(list)
#define F_NOTIFY_MASTER_CHANGED 0x2298ff0ull   // SessionEventObserverExtension.NotifyMasterChanged(list, PeerID)
#define F_NOTIFY_MASTER_LEFT 0x2299120ull      // ...NotifyMasterLeftRoom(list, MasterLeftReason)
#define F_NOTIFY_PEERS 0x2299250ull            // ...NotifyPeerListChanged(list, PeerID[])
#define F_UNITASK_FROM_RESULT_BOOL 0x256ae40ull   // UniTask.FromResult<bool>(bool, MethodInfo *)
// metadata usage pointer slots (each holds the address of the slot that receives the class / method)
#define M_CLASS_SM 0x60d21e8ull             // GCS1.Network.NPLN.NplnMultiplaySM
#define M_CLASS_MULTIPLAY 0x60d0a98ull      // GCS1.Network.MultiplayAPI
#define M_CLASS_PEERID_ARR 0x60d2a40ull     // GCS1.Network.PeerID[]
#define M_CLASS_BYTE_ARR 0x60cf328ull       // byte[]
#define M_CLASS_STREAM_DISPATCHER 0x60d4418ull   // GCS1.Network.NetworkStreamDispatcher {static +0 int DispatchRatePerSec, +4 int DivideGroupCount}
#define M_METHOD_FROM_RESULT_BOOL 0x60d3bc0ull
// object layouts
#define SM_STATIC_INSTANCE 0x30
#define SM_ONLINE_OBSERVERS 0x40
#define SM_SESSION_OBSERVERS 0x58
#define MULTIPLAY_STATIC_SESSION_CODE 0x8
#define ARR_LEN 0x18
#define ARR_DATA 0x20
#define STR_LEN 0x10
#define STR_CHARS 0x14

enum { ST_IDLE, ST_CONNECTING, ST_INROOM };
static Np *g_np;
static int g_state;                          // what the game is shown; changes only in hooks and in online_tick
static int g_online;                         // "ready to use internet" was announced and not yet "disconnected"
static double g_frame_time;                  // network time of this frame, seconds
static int g_purpose;                        // RoomPurpose of the room being entered: 1 Adventure, 2 Battle
static int g_adv_hz = 30;                    // port_net.txt adventure_rate=: position updates per second in Adventure rooms

void np_logf(const char *fmt, ...) {
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    rt_log("%s", b);
}

// ---------------------------------------------------------------------------------------------------- managed helpers
static uint64_t meta(Cpu *c, uint64_t slot_ptr) {
    uint64_t p = LD64(FN(slot_ptr));
    if (LD64(p) & 1) rt_call(c, FN(F_INIT_METADATA), 1, p);
    return LD64(p);
}
static uint64_t statics(Cpu *c, uint64_t slot_ptr) {
    uint64_t k = meta(c, slot_ptr);
    if (!LD32(k + 0xe0)) rt_call(c, FN(F_CLASS_INIT), 1, k);
    return LD64(k + 0xb8);
}
static uint64_t sm(Cpu *c) { return LD64(statics(c, M_CLASS_SM) + SM_STATIC_INSTANCE); }
static uint64_t new_str(Cpu *c, const char *s) {
    uint16_t buf[128];
    int n = 0;
    for (; s[n] && n < 127; n++) buf[n] = (uint8_t)s[n];
    return rt_call(c, FN(F_STRING_NEW_UTF16), 2, (uint64_t)(uintptr_t)buf, (uint64_t)n);
}
static uint64_t new_bytes(Cpu *c, const uint8_t *d, uint32_t n) {
    uint64_t a = rt_call(c, FN(F_SZARRAY_NEW), 2, meta(c, M_CLASS_BYTE_ARR), (uint64_t)n);
    if (a && n) memcpy((void *)(uintptr_t)(a + ARR_DATA), d, n);
    return a;
}
static uint64_t new_peers(Cpu *c) {
    uint64_t ids[NP_MAX_PEERS];
    int n = g_state == ST_INROOM ? np_peers(g_np, ids, NP_MAX_PEERS) : 0;
    uint64_t a = rt_call(c, FN(F_SZARRAY_NEW), 2, meta(c, M_CLASS_PEERID_ARR), (uint64_t)n);
    for (int i = 0; a && i < n; i++) {
        ST64(a + ARR_DATA + 16 * (uint64_t)i, ids[i]);
        ST64(a + ARR_DATA + 16 * (uint64_t)i + 8, 1);
    }
    return a;
}
static void set_session_code(Cpu *c, const char *code) {
    uint64_t slot = statics(c, M_CLASS_MULTIPLAY) + MULTIPLAY_STATIC_SESSION_CODE;
    uint64_t s = new_str(c, code);
    ST64(slot, s);
    rt_call(c, FN(F_WRITE_BARRIER), 2, slot, s);
}
// BR_NET_LOG=1: every 5 seconds, how many messages of each event code (first payload byte) went out and came in.
static uint32_t g_stat[2][2][256];           // [out / in][unreliable / reliable][code]
static int g_stat_on = -1;
static void stat_note(int in, int reliable, uint64_t bytes) {
    if (g_stat_on > 0 && bytes && LD32(bytes + ARR_LEN)) g_stat[in][reliable != 0][LD8(bytes + ARR_DATA)]++;
}
static void stat_log(void) {
    static uint64_t next;
    uint64_t now = GetTickCount64();
    if (g_stat_on < 0) g_stat_on = getenv("BR_NET_LOG") != NULL;
    if (g_stat_on <= 0 || now < next) return;
    next = now + 5000;
    for (int in = 0; in < 2; in++)
        for (int rel = 0; rel < 2; rel++) {
            char line[900];
            int n = 0;
            for (int k = 0; k < 256 && n < 860; k++)
                if (g_stat[in][rel][k]) n += snprintf(line + n, sizeof line - (size_t)n, " %02x:%u", k, g_stat[in][rel][k]);
            if (n) rt_log("online stats: %s %s%s", in ? "received" : "sent", rel ? "reliable" : "unreliable", line);
        }
    memset(g_stat, 0, sizeof g_stat);
}
static void deliver(Cpu *c, uint64_t bytes, uint64_t from, int reliable) {
    stat_note(1, reliable, bytes);
    rt_call(c, FN(F_ON_EVENT_RECEIVED), 5, bytes, from, 1ull, (uint64_t)(reliable != 0), 0ull);
}
static void notify_peers(Cpu *c) {
    uint64_t list = LD64(sm(c) + SM_SESSION_OBSERVERS);
    if (list) rt_call(c, FN(F_NOTIFY_PEERS), 3, list, new_peers(c), 0ull);
}
static void drop_events(void) {
    NpEvent e;
    while (np_poll(g_np, &e)) np_free(e.data);
}
static void went_offline(Cpu *c) {
    g_state = ST_IDLE;
    set_session_code(c, "");
    if (g_online) {
        g_online = 0;
        uint64_t list = LD64(sm(c) + SM_ONLINE_OBSERVERS);
        if (list) rt_call(c, FN(F_NOTIFY_DISCONNECT), 2, list, 0ull);
    }
}

// How often the game sends its position streams (NetworkStreamDispatcher.LateUpdate). The game's own values are
// 10 per second in 3 groups: a send every 33 ms, each to every third peer, which with the frame grid comes to 7 or 8
// updates a second per peer. That suits a Battle with 16 players; an Adventure room has at most 4, so there every
// send goes to all peers, adventure_rate times a second (30 unless set; 10 to 60, 0 = the game's own values).
// The receiver (OnlinePlayerBall.SetNetworkPos) extrapolates from position, velocity and send time and blends
// over 0.2 s whatever the rate, and each player only sets its own sending, so mixed settings work together.
// The dispatcher sends on the first frame later than 1000 / rate ms (whole ms) after the last send: the rate
// written is 4/3 of the wanted one, which puts that span 3/4 of the way to the frame that should send at 60 fps.
static void set_stream_rate(Cpu *c) {
    int hz = g_purpose == 1 ? g_adv_hz : 0;
    uint64_t st = statics(c, M_CLASS_STREAM_DISPATCHER);
    ST32(st, hz ? (uint32_t)(hz * 4 / 3) : 10u);
    ST32(st + 4, hz ? 1u : 3u);
    if (hz) rt_log("online: %d position updates a second to each player", hz);
    else rt_log("online: position updates at the game's own rate");
}

static void read_config(NpConfig *cfg) {
    char path[600], line[200];
    memset(cfg, 0, sizeof *cfg);
    cfg->upnp = 1;
    snprintf(path, sizeof path, "%sport_net.txt", rt_exe_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *v = strchr(line, '=');
        if (!v || line[0] == '#') continue;
        *v++ = 0;
        v[strcspn(v, "\r\n \t")] = 0;
        if (!strcmp(line, "port")) cfg->port = (uint16_t)atoi(v);
        else if (!strcmp(line, "upnp")) cfg->upnp = atoi(v) != 0;
        else if (!strcmp(line, "public_ip")) snprintf(cfg->public_ip, sizeof cfg->public_ip, "%s", v);
        else if (!strcmp(line, "adventure_rate")) { int r = atoi(v); g_adv_hz = r <= 0 ? 0 : r < 10 ? 10 : r > 60 ? 60 : r; }
    }
    fclose(f);
}

// ---------------------------------------------------------------------------------------------------- per-frame
static double network_seconds(void) {
    // as the game computes it: DateTime ticks / 10000 (ms since year 1), as seconds
    return (double)(np_time_ms(g_np) + 62135596800000ll) / 1000.0;
}
void online_tick(Cpu *c) {
    NpEvent e;
    if (!g_np) return;
    if (g_state != ST_IDLE) { g_frame_time = network_seconds(); stat_log(); }
    while (np_poll(g_np, &e)) {
        switch (e.type) {
        case NP_EV_JOINED: {
            char code[32];
            if (g_state != ST_CONNECTING) break;
            g_state = ST_INROOM;
            g_frame_time = network_seconds();
            np_code(g_np, code, sizeof code);
            set_session_code(c, code);
            set_stream_rate(c);
            rt_log("online: in a room as %016llx%s%s", (unsigned long long)np_local_id(g_np), code[0] ? ", code " : "", code);
            uint64_t s = sm(c), list = LD64(s + SM_ONLINE_OBSERVERS);
            if (!g_online) {
                g_online = 1;
                if (list) rt_call(c, FN(F_NOTIFY_READY_INTERNET), 2, list, 0ull);
            }
            if (g_state != ST_INROOM) break;                         // an observer left again
            list = LD64(s + SM_SESSION_OBSERVERS);
            if (list) rt_call(c, FN(F_NOTIFY_MASTER_CHANGED), 4, list, np_master_id(g_np), 1ull, 0ull);
            if (g_state == ST_INROOM) notify_peers(c);
            break;
        }
        case NP_EV_FAILED:
            if (g_state != ST_CONNECTING) break;
            rt_log("online: could not enter the room (reason %d)", e.reason);
            g_state = ST_IDLE;
            break;
        case NP_EV_PEERS:
            if (g_state == ST_INROOM) notify_peers(c);
            break;
        case NP_EV_FLAGS: break;                                     // the game polls CanParticipate
        case NP_EV_CLOSED: {
            if (g_state != ST_INROOM) break;
            rt_log("online: the room ended (reason %d)", e.reason);
            uint64_t list = LD64(sm(c) + SM_SESSION_OBSERVERS);
            if (list) rt_call(c, FN(F_NOTIFY_MASTER_LEFT), 3, list, (uint64_t)(e.reason == NP_CLOSE_HOST_LEFT ? 0 : 1), 0ull);
            went_offline(c);
            break;
        }
        case NP_EV_DATA:
            if (g_state == ST_INROOM && e.len >= 2) deliver(c, new_bytes(c, e.data, e.len), e.peer, e.reliable);
            np_free(e.data);
            break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------- matchmaking
// NplnMatchmakingAPI.RequestMatchmaking(IMatchmakingContext context)
int rt_hook_2262740(Cpu *c) {
    uint64_t ctx = X(1);
    const char *name = ctx ? (const char *)(uintptr_t)LD64(LD64(ctx) + 0x10) : "";
    NpConfig cfg;
    if (!g_np) g_np = np_create();
    read_config(&cfg);
    np_leave(g_np);
    drop_events();
    if (g_state == ST_INROOM) went_offline(c);
    g_state = ST_IDLE;
    if (!strcmp(name, "FriendHostMatchmakingContext")) {             // {+0x10 int SubUserCount, +0x14 RoomPurpose}
        rt_log("online: creating a room (purpose %d)", (int)LD32(ctx + 0x14));
        g_purpose = (int)LD32(ctx + 0x14);
        g_state = ST_CONNECTING;
        np_host(g_np, (int)LD32(ctx + 0x14), &cfg);
    } else if (!strcmp(name, "ShortSessionIdMatchmakingContext")) {  // {+0x10 string id, +0x18 int SubUserCount, +0x1c RoomPurpose}
        char code[64] = "";
        uint64_t s = LD64(ctx + 0x10);
        int n = s ? (int)LD32(s + STR_LEN) : 0;
        for (int i = 0; i < n && i < 63; i++) {
            uint16_t ch = LD16(s + STR_CHARS + 2 * (uint64_t)i);
            code[i] = ch < 128 ? (char)ch : '?';
            code[i + 1] = 0;
        }
        rt_log("online: joining a room by code (purpose %d)", (int)LD32(ctx + 0x1c));
        g_purpose = (int)LD32(ctx + 0x1c);
        g_state = ST_CONNECTING;
        np_join(g_np, code, (int)LD32(ctx + 0x1c), &cfg);
    } else
        rt_log("online: %s is not available in the PC port (only rooms joined by code are)", name);
    return 1;
}
// NplnMatchmakingAPI.RequestLeaveRoom()
int rt_hook_2262910(Cpu *c) {
    if (g_state == ST_IDLE) return 0;
    rt_log("online: leaving the room");
    np_leave(g_np);
    drop_events();
    g_state = ST_IDLE;
    set_session_code(c, "");
    return 1;
}

// ---------------------------------------------------------------------------------------------------- NplnAPIFacade
int rt_hook_2263240(Cpu *c) {   // double get_NetworkTime()
    if (g_state == ST_IDLE) return 0;
    rt_setd(c, 0, g_frame_time);
    return 1;
}
int rt_hook_2263430(Cpu *c) {   // int get_ServerRTT()
    if (g_state != ST_INROOM) return 0;
    int r = np_rtt(g_np, np_master_id(g_np));
    RET(r < 0 ? 0 : r);
    return 1;
}
static int is_peer(uint64_t id) {
    uint64_t ids[NP_MAX_PEERS];
    int n = np_peers(g_np, ids, NP_MAX_PEERS);
    for (int i = 0; i < n; i++)
        if (ids[i] == id) return 1;
    return 0;
}
int rt_hook_2263440(Cpu *c) {   // bool IsConnected(PeerID)
    if (g_state != ST_INROOM) return 0;
    RET((X(2) & 1) && is_peer(X(1)));
    return 1;
}
int rt_hook_2263560(Cpu *c) {   // bool IsConnectionServer(PeerID)
    if (g_state != ST_INROOM) return 0;
    RET((X(2) & 1) && X(1) == np_master_id(g_np));
    return 1;
}
int rt_hook_2263680(Cpu *c) {   // bool get_IsMatchmaking()
    if (g_state == ST_IDLE) return 0;
    RET(0);
    return 1;
}
int rt_hook_2263830(Cpu *c) {   // bool get_IsInRoom()
    if (g_state == ST_IDLE) return 0;
    RET(g_state == ST_INROOM);
    return 1;
}
int rt_hook_2263a70(Cpu *c) {   // bool get_IsLocalNetwork()
    if (g_state == ST_IDLE) return 0;
    RET(0);
    return 1;
}
int rt_hook_2263bc0(Cpu *c) {   // bool get_IsProcessing()
    if (g_state == ST_IDLE) return 0;
    RET(g_state == ST_CONNECTING);
    return 1;
}
int rt_hook_2263db0(Cpu *c) {   // string get_CurrentState()
    if (g_state == ST_IDLE) return 0;
    RET(new_str(c, g_state == ST_INROOM ? "PC room" : "PC connecting"));
    return 1;
}
int rt_hook_2263ec0(Cpu *c) {   // string get_GameID()
    if (g_state == ST_IDLE) return 0;
    RET(new_str(c, "pc"));
    return 1;
}
static int leave_all(Cpu *c, const char *what) {
    if (g_state == ST_IDLE && !g_online) return 0;
    rt_log("online: %s", what);
    np_leave(g_np);
    drop_events();
    went_offline(c);
    return 1;
}
int rt_hook_2264110(Cpu *c) { return leave_all(c, "disconnect"); }                    // void Disconnect()
int rt_hook_22641c0(Cpu *c) {                                                         // void Cancel()
    if (g_state != ST_CONNECTING) return 0;
    return leave_all(c, "cancelled while connecting");
}

// ---------------------------------------------------------------------------------------------------- SessionAPI
static int send_ids(Cpu *c, uint64_t payload, int reliable, const uint64_t *ids, int n) {
    uint64_t me = np_local_id(g_np), out[NP_MAX_PEERS];
    int k = 0, self = 0;
    stat_note(0, reliable, payload);
    for (int i = 0; i < n; i++) {
        if (ids[i] == me) self = 1;
        else if (k < NP_MAX_PEERS) out[k++] = ids[i];
    }
    int ok = 1;
    if (k) ok = np_send(g_np, (const uint8_t *)(uintptr_t)(payload + ARR_DATA), LD32(payload + ARR_LEN), reliable, NP_TO_TARGETS, out, k);
    if (self) deliver(c, payload, me, reliable);
    return ok;
}
static int send_group(Cpu *c, uint64_t payload, int reliable, int group) {
    if (!payload) return 0;
    if (group == 2) {                                                // MasterClient
        uint64_t m = np_master_id(g_np);
        return send_ids(c, payload, reliable, &m, 1);
    }
    if (group > 2) return 0;
    stat_note(0, reliable, payload);
    int ok = np_send(g_np, (const uint8_t *)(uintptr_t)(payload + ARR_DATA), LD32(payload + ARR_LEN), reliable, NP_TO_OTHERS, NULL, 0);
    if (group == 1) deliver(c, payload, np_local_id(g_np), reliable);   // All: the sender gets it too, at once
    return ok;
}
static int send_array(Cpu *c, uint64_t payload, int reliable, uint64_t targets) {
    uint64_t ids[NP_MAX_PEERS];
    int n = 0, len = targets ? (int)LD32(targets + ARR_LEN) : 0;
    if (!payload) return 0;
    for (int i = 0; i < len && n < NP_MAX_PEERS; i++)
        if (LD8(targets + ARR_DATA + 16 * (uint64_t)i + 8) & 1) ids[n++] = LD64(targets + ARR_DATA + 16 * (uint64_t)i);
    return send_ids(c, payload, reliable, ids, n);
}
int rt_hook_227f940(Cpu *c) {   // bool SendInternal(byte code, byte[] payload, bool sendReliable, ReceiverGroup receiver)
    if (g_state != ST_INROOM) return 0;
    RET(send_group(c, X(2), (int)(X(3) & 1), (int)W(4)));
    return 1;
}
int rt_hook_227fb50(Cpu *c) {   // bool SendInternal(byte code, byte[] payload, PeerID peerId, bool sendReliable)
    if (g_state != ST_INROOM) return 0;
    uint64_t id = X(3);
    RET(X(2) && (X(4) & 1) ? send_ids(c, X(2), (int)(X(5) & 1), &id, 1) : 0);
    return 1;
}
int rt_hook_227fda0(Cpu *c) {   // bool SendStreamUnreliable(byte[] payload)
    if (g_state != ST_INROOM) return 0;
    RET(send_group(c, X(1), 0, 0));
    return 1;
}
int rt_hook_227ffa0(Cpu *c) {   // bool SendStreamUnreliable(byte[] payload, PeerID[] targets)
    if (g_state != ST_INROOM) return 0;
    RET(send_array(c, X(1), 0, X(2)));
    return 1;
}
int rt_hook_22801b0(Cpu *c) {   // bool get_CanParticipate()
    if (g_state != ST_INROOM) return 0;
    RET(np_can_participate(g_np));
    return 1;
}
static int from_result(Cpu *c, int v) {
    rt_call(c, FN(F_UNITASK_FROM_RESULT_BOOL), 2, (uint64_t)v, meta(c, M_METHOD_FROM_RESULT_BOOL));   // UniTask<bool> in x0, x1
    return 1;
}
int rt_hook_22802c0(Cpu *c) {   // UniTask<bool> SetCanParticipateAsync(bool canParticipate)
    if (g_state != ST_INROOM) return 0;
    int ok = np_set_can_participate(g_np, (int)(X(1) & 1));
    rt_log("online: room %s for new players%s", (X(1) & 1) ? "open" : "closed", ok ? "" : " (refused: not the host)");
    return from_result(c, ok);
}
int rt_hook_2280410(Cpu *c) {   // PeerID get_LocalPeerID()
    if (g_state != ST_INROOM) return 0;
    c->x[0] = np_local_id(g_np);
    c->x[1] = 1;
    return 1;
}
int rt_hook_2280520(Cpu *c) {   // PeerID get_MasterPeerID()
    if (g_state != ST_INROOM) return 0;
    c->x[0] = np_master_id(g_np);
    c->x[1] = 1;
    return 1;
}
int rt_hook_2280630(Cpu *c) {   // UniTask<bool> RequestPromoteToMasterAsync(): the creator stays master
    if (g_state != ST_INROOM) return 0;
    return from_result(c, np_master_id(g_np) == np_local_id(g_np));
}
int rt_hook_2280700(Cpu *c) {   // PeerID[] get_Peers()
    if (g_state != ST_INROOM) return 0;
    RET(new_peers(c));
    return 1;
}
int rt_hook_2280830(Cpu *c) {   // int GetRtt(PeerID)
    if (g_state != ST_INROOM) return 0;
    int r = np_rtt(g_np, X(1));
    RET(r < 0 ? 0 : r);
    return 1;
}
int rt_hook_2280c80(Cpu *c) {   // string get_RoomID()
    char code[32];
    if (g_state != ST_INROOM) return 0;
    snprintf(code, sizeof code, "pc-%08x", (unsigned)(np_master_id(g_np) & 0xffffffff));
    RET(new_str(c, code));
    return 1;
}

// ---------------------------------------------------------------------------------------------------- room lists
// NplnRoomFinder.FindAsync(FindRoomContext context) -> UniTask<RoomInfo[]>: the lists of public Adventure rooms and
// of local-wireless rooms. There is no such service in the PC port: the search finishes at once with no rooms.
// FindRoomContext {+0 Type: 0 local Adventure, 1 local Battle, 2 by world}; a completed UniTask<T> is
// {source = null, result, token = 0}, returned through x8.
#define M_CLASS_LOCAL_ROOM_ARR 0x60d37c8ull    // GCS1.Network.LocalRoomInfo[]
#define M_CLASS_WORLD_ROOM_ARR 0x60d38a8ull    // GCS1.Network.WorldNumRoomInfo[]
int rt_hook_2272270(Cpu *c) {
    uint64_t out = c->x[8], ctx = X(1);
    int type = ctx ? (int)LD32(ctx) : 2;
    uint64_t arr = rt_call(c, FN(F_SZARRAY_NEW), 2, meta(c, type == 2 ? M_CLASS_WORLD_ROOM_ARR : M_CLASS_LOCAL_ROOM_ARR), 0ull);
    rt_log("online: room search (type %d) answered with an empty list", type);
    ST64(out, 0);
    ST64(out + 8, arr);
    ST64(out + 16, 0);
    c->x[8] = out;
    return 1;
}
