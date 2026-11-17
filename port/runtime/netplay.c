// PC online play transport: private rooms over UDP, joined by a code.
//
// Topology: a star. The player who creates the room (the host, always the game's "master") listens on one UDP port;
// guests connect to it and the host relays between guests. There is no server of ours; direct rooms use no
// third-party service at all.
// Steam rooms (steam_net.c): the same protocol, but each datagram goes to a Steam account through Valve's relay
// network instead of a UDP address. The code then holds the host's Steam account number instead of its address
// (port index 15, which a UDP host never uses), so it has the same length and the same screens take it.
//
// Room code: 12 characters = 60 bits = host IPv4 address, port offset and a 24-bit secret. A guest proves knowledge of the
// secret in the handshake (SHA-256 over secret and both nonces), so somebody who only finds the open port cannot join
// or get anything but a 21-byte cookie back (never more than they sent: no amplification). After the handshake every
// packet carries the 64-bit connection token and must come from the address that made the handshake.
// Traffic is not encrypted: somebody on the network path can read or alter it, as with most direct-connect games.
//
// Channels per connection: reliable ordered (sequence numbers, cumulative + selective acks, retransmission,
// fragmentation up to NP_MAX_MESSAGE) and unreliable datagrams (sent reliably when too big for one packet).
// Every length and index that comes from the network is checked here; payloads then go to the game's own readers.
//
// Threading: one thread per session does the socket work; the game's main thread calls the np_* functions. All state
// is guarded by one lock. A session object belongs to its thread, which frees it after np_leave detached it.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netplay.h"

#define MAGIC 0x314e5242u   // "BRN1"
#define PROTO_BASE 1
// The protocol number both sides must share. A build whose game tables differ from the plain game's (extra
// courses and stages) mixes a tag of that content in, so that only players with the same content meet: course
// and stage numbers sent in a room mean nothing to a game with other tables. Tag 0 is the plain game.
static uint16_t g_proto = PROTO_BASE;
#define PROTO g_proto
void np_set_content_tag(uint32_t tag) { g_proto = (uint16_t)(PROTO_BASE ^ (tag ? (uint16_t)(0x8000u | ((tag ^ (tag >> 15)) & 0x7fffu)) : 0)); }
enum { PT_HELLO = 1, PT_CHALLENGE, PT_JOIN, PT_ACCEPT, PT_REJECT, PT_REL = 16, PT_UNREL, PT_PING, PT_PONG, PT_BYE, PT_ACK };
enum { MSG_GAME = 1, MSG_PEERS };
#define HDR 21              // magic, type, token, ack, ack bits
#define FRAG 1100           // payload bytes per reliable packet
#define MAX_UNREL 1150
#define WINDOW 256
#define HELLO_SIZE 48
#define TIMEOUT_MS 10000
#define CONNECT_MS 8000
#define MAX_EVENT_BYTES (32u << 20)

typedef struct Addr { struct sockaddr_in in; uint64_t sid; } Addr;   // UDP address, or a Steam id (sid != 0)
typedef struct Out { struct Out *next; uint32_t seq; uint64_t sent; int tries; uint16_t len; uint8_t data[]; } Out;
typedef struct In { uint8_t *data; uint16_t len; uint8_t present; } In;
typedef struct Conn {
    int used;
    Addr addr;
    uint64_t token, peer, cnonce;
    Out *q_head, *q_tail;
    uint32_t snd_next, snd_una, rcv_next;
    In ring[WINDOW];
    uint8_t *asm_buf;
    uint32_t asm_len, asm_cap;
    uint64_t last_recv, last_ping, ack_due;
    int rtt, need_ack, pongs, order;
} Conn;
typedef struct Ev { struct Ev *next; NpEvent e; } Ev;
typedef struct Sess {
    Np *np;
    volatile LONG stop;
    int host, purpose, state, steam;
    NpConfig cfg;
    SOCKET s;
    uint16_t port;
    uint32_t ip, secret;
    uint8_t cookie_key[16];
    Conn conns[NP_MAX_PEERS];
    uint64_t local_id, master_id, next_peer;
    struct { uint64_t id; int rtt; } peers[NP_MAX_PEERS];
    int npeers, can_participate, order, mapped;
    uint32_t peers_rev;
    uint64_t last_peers;
    int64_t clock0;          // room clock at mono0 (host: own UTC clock; guest: estimated host clock)
    uint64_t mono0;
    int sync_rtt;
    char code[32];
    // guest handshake
    int hs, got_peers;
    Ev *early_head, *early_tail;   // game messages that arrived before the room was entered (delivered after NP_EV_JOINED)
    uint64_t cnonce, hnonce, hs_start, hs_last;
    // host rate limits
    uint64_t rl_second;
    int rl_hello, rl_fail;
} Sess;
struct Np {
    CRITICAL_SECTION lock;
    Sess *cur;
    Ev *ev_head, *ev_tail;
    size_t ev_bytes;
    volatile LONG threads;
};

// ---------------------------------------------------------------------------------------------------- helpers
static uint64_t mono_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / f.QuadPart) * 1000 + (uint64_t)(t.QuadPart % f.QuadPart) * 1000 / (uint64_t)f.QuadPart;
}
static int64_t unix_ms(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (int64_t)((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000) - 11644473600000ll;
}
static void rnd(void *p, size_t n) {
    if (BCryptGenRandom(NULL, (PUCHAR)p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        np_logf("net: no system random numbers");
        abort();
    }
}
static void sha256(const uint8_t *d, size_t n, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE a = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    memset(out, 0, 32);
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) abort();
    if (BCryptCreateHash(a, &h, NULL, 0, NULL, 0, 0) != 0 || BCryptHashData(h, (PUCHAR)d, (ULONG)n, 0) != 0 || BCryptFinishHash(h, out, 32, 0) != 0) abort();
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(a, 0);
}
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint16_t get16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static int same_addr(const Addr *a, const Addr *b) {
    return a->sid == b->sid && a->in.sin_addr.s_addr == b->in.sin_addr.s_addr && a->in.sin_port == b->in.sin_port;
}
// tag: 'j' join proof (guest -> host), 'a' accept proof (host -> guest)
static void proof(uint32_t secret, uint64_t cn, uint64_t hn, char tag, uint8_t out[16]) {
    uint8_t b[32], h[32];
    memcpy(b, "BRN1 proof ", 11);
    b[11] = (uint8_t)tag;
    put32(b + 12, secret);
    put64(b + 16, cn);
    put64(b + 24, hn);
    sha256(b, sizeof b, h);
    memcpy(out, h, 16);
}
static uint64_t cookie(Sess *s, const Addr *from, uint64_t cn) {
    uint8_t b[38], h[32];
    memcpy(b, s->cookie_key, 16);
    memcpy(b + 16, &from->in.sin_addr.s_addr, 4);
    memcpy(b + 20, &from->in.sin_port, 2);
    put64(b + 22, cn);
    put64(b + 30, from->sid);
    sha256(b, sizeof b, h);
    return get64(h);
}

// ---------------------------------------------------------------------------------------------------- room code
// 12 characters of 5 bits: address (32 bits, scrambled with the secret so the code does not read as an address),
// port as an offset from the base port (4 bits) and the secret (24 bits). Shown as XXXX-XXXX-XXXX.
static const char ALPHA[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
static uint32_t scramble(uint32_t secret) { return (secret + 0x7f4a7c15u) * 0x9e3779b1u; }
void np_code_format(uint32_t ip, int port_index, uint32_t secret, char *out, size_t n) {
    char raw[13];
    secret &= 0xffffff;
    uint64_t v = ((uint64_t)(ip ^ scramble(secret)) << 28) | ((uint64_t)(port_index & 15) << 24) | secret;
    for (int i = 0; i < 12; i++) raw[i] = ALPHA[(v >> (55 - 5 * i)) & 31];
    raw[12] = 0;
    snprintf(out, n, "%.4s-%.4s-%.4s", raw, raw + 4, raw + 8);
}
int np_code_parse(const char *code, uint32_t *ip, int *port_index, uint32_t *secret) {
    uint64_t v = 0;
    int n = 0;
    for (const char *p = code; *p; p++) {
        char ch = *p;
        if (ch == '-' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        if (ch == 'O') ch = '0';
        if (ch == 'I' || ch == 'L') ch = '1';
        if (ch == 'U') ch = 'V';
        const char *q = strchr(ALPHA, ch);
        if (!q || n >= 12) return 0;
        v = (v << 5) | (uint64_t)(q - ALPHA);
        n++;
    }
    if (n != 12) return 0;
    *secret = (uint32_t)(v & 0xffffff);
    *port_index = (int)((v >> 24) & 15);
    *ip = (uint32_t)(v >> 28) ^ scramble(*secret);
    return *ip != 0;
}
// ---------------------------------------------------------------------------------------------------- events
static void push_ev(Sess *s, int type, uint64_t peer, int reliable, int reason, const uint8_t *data, uint32_t len) {
    Np *np = s->np;
    if (np->cur != s) return;
    if (np->ev_bytes + len > MAX_EVENT_BYTES) return;       // the game stopped polling: drop instead of growing
    Ev *e = calloc(1, sizeof *e);
    if (!e) return;
    e->e.type = type; e->e.peer = peer; e->e.reliable = reliable; e->e.reason = reason;
    if (len) {
        e->e.data = malloc(len);
        if (!e->e.data) { free(e); return; }
        memcpy(e->e.data, data, len);
        e->e.len = len;
        np->ev_bytes += len;
    }
    if (np->ev_tail) np->ev_tail->next = e; else np->ev_head = e;
    np->ev_tail = e;
}

// ---------------------------------------------------------------------------------------------------- packets
static void tx(Sess *s, const Addr *to, const uint8_t *p, int n) {
    if (s->cfg.loss_percent && rand() % 100 < s->cfg.loss_percent) return;
    if (s->steam) sn_send(to->sid, p, n);
    else sendto(s->s, (const char *)p, n, 0, (const struct sockaddr *)&to->in, sizeof to->in);
}
static int hdr(Conn *c, uint8_t *p, int type) {
    uint32_t bits = 0;
    for (int i = 0; i < 32; i++)
        if (c->ring[(c->rcv_next + 1 + (uint32_t)i) % WINDOW].present) bits |= 1u << i;
    put32(p, MAGIC);
    p[4] = (uint8_t)type;
    put64(p + 5, c->token);
    put32(p + 13, c->rcv_next);
    put32(p + 17, bits);
    c->need_ack = 0;
    c->ack_due = 0;
    return HDR;
}
static void tx_out(Sess *s, Conn *c, Out *o, uint64_t now) {
    uint8_t p[HDR + 4 + 1 + FRAG];
    int n = hdr(c, p, PT_REL);
    put32(p + n, o->seq);
    memcpy(p + n + 4, o->data, o->len);
    tx(s, &c->addr, p, n + 4 + o->len);
    o->sent = now;
    o->tries++;
}
static void flush(Sess *s, Conn *c, uint64_t now) {
    int rto = c->rtt < 0 ? 200 : c->rtt * 2 + 30;
    if (rto < 60) rto = 60;
    for (Out *o = c->q_head; o && o->seq - c->snd_una < WINDOW; o = o->next) {
        int wait = rto << (o->tries > 4 ? 4 : o->tries > 0 ? o->tries - 1 : 0);
        if (wait > 1500) wait = 1500;
        if (!o->tries || now - o->sent >= (uint64_t)wait) tx_out(s, c, o, now);
    }
}
static void rel_send(Sess *s, Conn *c, const uint8_t *msg, uint32_t len) {
    uint32_t off = 0;
    do {
        uint32_t n = len - off > FRAG ? FRAG : len - off;
        Out *o = malloc(sizeof *o + 1 + n);
        if (!o) return;
        o->next = NULL; o->seq = c->snd_next++; o->sent = 0; o->tries = 0; o->len = (uint16_t)(1 + n);
        o->data[0] = off + n == len;
        memcpy(o->data + 1, msg + off, n);
        if (c->q_tail) c->q_tail->next = o; else c->q_head = o;
        c->q_tail = o;
        off += n;
    } while (off < len);
    flush(s, c, mono_ms());
}
static void msg_send(Sess *s, Conn *c, const uint8_t *msg, uint32_t len, int reliable) {
    if (reliable || len > MAX_UNREL) { rel_send(s, c, msg, len); return; }
    uint8_t p[HDR + MAX_UNREL];
    int n = hdr(c, p, PT_UNREL);
    memcpy(p + n, msg, len);
    tx(s, &c->addr, p, n + (int)len);
}
static void on_ack(Conn *c, uint32_t ack, uint32_t bits) {
    if (ack - c->snd_una > c->snd_next - c->snd_una) return;        // outside [snd_una, snd_next]: never sent
    Out **pp = &c->q_head, *prev = NULL;
    while (*pp) {
        Out *o = *pp;
        int32_t d = (int32_t)(o->seq - ack);                         // < 0: below the cumulative ack
        if (d > 32) break;                                           // the queue is in sequence order
        if (d < 0 || (d > 0 && ((bits >> (d - 1)) & 1))) {
            *pp = o->next;
            if (c->q_tail == o) c->q_tail = prev;
            free(o);
        } else {
            prev = o;
            pp = &o->next;
        }
    }
    c->snd_una = c->q_head ? c->q_head->seq : c->snd_next;
}
// ---------------------------------------------------------------------------------------------------- room state
static Conn *conn_by_peer(Sess *s, uint64_t id) {
    for (int i = 0; i < NP_MAX_PEERS; i++)
        if (s->conns[i].used && s->conns[i].peer == id) return &s->conns[i];
    return NULL;
}
static void conn_free(Conn *c) {
    for (Out *o = c->q_head; o;) { Out *n = o->next; free(o); o = n; }
    for (int i = 0; i < WINDOW; i++) free(c->ring[i].data);
    free(c->asm_buf);
    memset(c, 0, sizeof *c);
}
static int64_t room_clock(Sess *s) { return s->clock0 + (int64_t)(mono_ms() - s->mono0); }
static void host_rebuild_peers(Sess *s) {
    int n = 0;
    s->peers[n].id = s->local_id; s->peers[n++].rtt = 0;
    for (int order = 1; order <= s->order; order++)
        for (int i = 0; i < NP_MAX_PEERS; i++)
            if (s->conns[i].used && s->conns[i].order == order && n < NP_MAX_PEERS) {
                s->peers[n].id = s->conns[i].peer;
                s->peers[n++].rtt = s->conns[i].rtt;
            }
    s->npeers = n;
}
static void host_send_peers(Sess *s, int reliable) {
    uint8_t m[16 + NP_MAX_PEERS * 10];
    host_rebuild_peers(s);
    m[0] = MSG_PEERS;
    put32(m + 1, s->peers_rev);
    put64(m + 5, s->master_id);
    m[13] = (uint8_t)s->can_participate;
    m[14] = (uint8_t)s->npeers;
    for (int i = 0; i < s->npeers; i++) {
        put64(m + 15 + i * 10, s->peers[i].id);
        put16(m + 23 + i * 10, (uint16_t)(s->peers[i].rtt < 0 ? 0xffff : s->peers[i].rtt > 0xfffe ? 0xfffe : s->peers[i].rtt));
    }
    for (int i = 0; i < NP_MAX_PEERS; i++)
        if (s->conns[i].used) msg_send(s, &s->conns[i], m, (uint32_t)(15 + s->npeers * 10), reliable);
    s->last_peers = mono_ms();
}
static void host_peers_changed(Sess *s) {
    s->peers_rev++;
    host_send_peers(s, 1);
    push_ev(s, NP_EV_PEERS, 0, 0, 0, NULL, 0);
}
static void host_drop(Sess *s, Conn *c, const char *why) {
    np_logf("net: peer %016llx left (%s)", (unsigned long long)c->peer, why);
    conn_free(c);
    host_peers_changed(s);
}
static void sess_close(Sess *s, int ev, int reason) {
    if (s->state == NP_IDLE) return;
    s->state = NP_IDLE;
    push_ev(s, ev, 0, 0, reason, NULL, 0);
    InterlockedExchange(&s->stop, 1);
}

// A complete message from connection c (reliable: reassembled; unreliable: one datagram).
static void on_message(Sess *s, Conn *c, const uint8_t *m, uint32_t len, int reliable) {
    if (len < 1) return;
    if (m[0] == MSG_GAME) {
        if (len < 11) return;
        uint64_t src = get64(m + 1);
        int mode = m[9], nt = m[10];
        if (len < 11u + 8u * (uint32_t)nt || mode > NP_TO_TARGETS) return;
        const uint8_t *pay = m + 11 + 8 * nt;
        uint32_t plen = len - 11 - 8 * (uint32_t)nt;
        if (!s->host) {                                              // from the host: it already routed this
            if (s->state == NP_INROOM) push_ev(s, NP_EV_DATA, src, reliable, 0, pay, plen);
            else if (reliable && s->state == NP_CONNECTING) {        // the host greets a guest before it is quite in
                Ev *e = calloc(1, sizeof *e);
                uint8_t *d = e ? malloc(plen ? plen : 1) : NULL;
                if (!d) { free(e); return; }
                memcpy(d, pay, plen);
                e->e.type = NP_EV_DATA; e->e.peer = src; e->e.reliable = 1; e->e.data = d; e->e.len = plen;
                if (s->early_tail) s->early_tail->next = e; else s->early_head = e;
                s->early_tail = e;
            }
            return;
        }
        if (src != c->peer) return;                                  // a guest may only speak for itself
        if (mode == NP_TO_OTHERS) {
            push_ev(s, NP_EV_DATA, src, reliable, 0, pay, plen);
            for (int i = 0; i < NP_MAX_PEERS; i++)
                if (s->conns[i].used && &s->conns[i] != c) msg_send(s, &s->conns[i], m, len, reliable);
        } else {
            for (int k = 0; k < nt; k++) {
                uint64_t t = get64(m + 11 + 8 * k);
                int dup = 0;
                for (int j = 0; j < k; j++) dup |= get64(m + 11 + 8 * j) == t;
                if (dup) continue;
                if (t == s->local_id) push_ev(s, NP_EV_DATA, src, reliable, 0, pay, plen);
                else {
                    Conn *d = conn_by_peer(s, t);
                    if (d && d != c) msg_send(s, d, m, len, reliable);
                }
            }
        }
    } else if (m[0] == MSG_PEERS && !s->host) {
        if (len < 15) return;
        uint32_t rev = get32(m + 1);
        int n = m[14];
        if (n < 1 || n > NP_MAX_PEERS || len < 15u + 10u * (uint32_t)n) return;
        if (s->got_peers && (int32_t)(rev - s->peers_rev) < 0) return;
        uint64_t master = get64(m + 5);
        int cp = m[13] != 0, changed = !s->got_peers || master != s->master_id || n != s->npeers;
        for (int i = 0; i < n; i++) {
            uint64_t id = get64(m + 15 + i * 10);
            uint16_t r = get16(m + 23 + i * 10);
            changed |= s->peers[i].id != id;
            s->peers[i].id = id;
            s->peers[i].rtt = r == 0xffff ? -1 : r;
        }
        int flag = s->got_peers && cp != s->can_participate;
        s->npeers = n; s->master_id = master; s->can_participate = cp; s->peers_rev = rev; s->got_peers = 1;
        if (s->state == NP_INROOM) {
            if (changed) push_ev(s, NP_EV_PEERS, 0, 0, 0, NULL, 0);
            if (flag) push_ev(s, NP_EV_FLAGS, 0, 0, 0, NULL, 0);
        }
    }
}

static void on_rel(Sess *s, Conn *c, const uint8_t *p, int n, uint64_t now) {
    if (n < 5) return;
    uint32_t seq = get32(p);
    c->need_ack = 1;
    if (!c->ack_due) c->ack_due = now + 10;
    if (seq - c->rcv_next >= WINDOW) return;                         // old (already delivered) or too far ahead
    In *in = &c->ring[seq % WINDOW];
    if (in->present) return;
    in->data = malloc((size_t)n - 4);
    if (!in->data) return;
    memcpy(in->data, p + 4, (size_t)n - 4);
    in->len = (uint16_t)(n - 4);
    in->present = 1;
    while (c->used && c->ring[c->rcv_next % WINDOW].present) {
        in = &c->ring[c->rcv_next % WINDOW];
        uint8_t *d = in->data;
        uint32_t dl = in->len;
        in->data = NULL; in->present = 0;
        c->rcv_next++;
        if (c->asm_len + dl - 1 > NP_MAX_MESSAGE) {                   // a peer that sends this is broken or hostile
            free(d);
            if (s->host) host_drop(s, c, "message too large"); else sess_close(s, NP_EV_CLOSED, NP_CLOSE_ERROR);
            return;
        }
        if (c->asm_len + dl - 1 > c->asm_cap) {
            uint32_t cap = (c->asm_len + dl) * 2;
            uint8_t *nb = realloc(c->asm_buf, cap);
            if (!nb) { free(d); return; }
            c->asm_buf = nb; c->asm_cap = cap;
        }
        memcpy(c->asm_buf + c->asm_len, d + 1, dl - 1);
        c->asm_len += dl - 1;
        int last = d[0] & 1;
        free(d);
        if (last) {
            uint32_t ml = c->asm_len;
            c->asm_len = 0;
            on_message(s, c, c->asm_buf, ml, 1);
        }
    }
}

static void send_simple(Sess *s, Conn *c, int type) {
    uint8_t p[HDR];
    tx(s, &c->addr, p, hdr(c, p, type));
}
static void send_ping(Sess *s, Conn *c, uint64_t now) {
    uint8_t p[HDR + 4];
    int n = hdr(c, p, PT_PING);
    put32(p + n, (uint32_t)now);
    tx(s, &c->addr, p, n + 4);
    c->last_ping = now;
}

static void on_connected(Sess *s, Conn *c, int type, const uint8_t *p, int n, uint64_t now) {
    c->last_recv = now;
    on_ack(c, get32(p + 13), get32(p + 17));
    p += HDR; n -= HDR;
    switch (type) {
    case PT_REL: on_rel(s, c, p, n, now); break;
    case PT_UNREL: on_message(s, c, p, (uint32_t)n, 0); break;
    case PT_PING: {
        if (n < 4) break;
        uint8_t r[HDR + 12];
        int k = hdr(c, r, PT_PONG);
        memcpy(r + k, p, 4);
        put64(r + k + 4, (uint64_t)room_clock(s));
        tx(s, &c->addr, r, k + 12);
        break;
    }
    case PT_PONG: {
        if (n < 12) break;
        uint32_t r = (uint32_t)now - get32(p);
        if (r > 5000) break;
        c->rtt = c->rtt < 0 ? (int)r : (c->rtt * 7 + (int)r) / 8;
        c->pongs++;
        if (!s->host) {                                              // estimate the host's clock from the best sample
            if (c->pongs <= 4 ? (int)r <= s->sync_rtt || c->pongs == 1 : (int)r < s->sync_rtt) {
                s->clock0 = (int64_t)get64(p + 4) + r / 2;
                s->mono0 = now;
                s->sync_rtt = (int)r;
            } else if (c->pongs % 8 == 0) s->sync_rtt += 1;          // let a later sample win eventually
        }
        break;
    }
    case PT_BYE:
        if (s->host) host_drop(s, c, "left"); else sess_close(s, NP_EV_CLOSED, NP_CLOSE_HOST_LEFT);
        break;
    default: break;
    }
}

static void send_reject(Sess *s, const Addr *to, uint64_t cn, int reason) {
    uint8_t r[14];
    put32(r, MAGIC); r[4] = PT_REJECT; put64(r + 5, cn); r[13] = (uint8_t)reason;
    tx(s, to, r, sizeof r);
}
static void send_accept(Sess *s, Conn *c, uint64_t hn) {
    uint8_t r[53];
    put32(r, MAGIC); r[4] = PT_ACCEPT; put64(r + 5, c->cnonce);
    proof(s->secret, c->cnonce, hn, 'a', r + 13);
    put64(r + 29, c->token); put64(r + 37, c->peer); put64(r + 45, s->master_id);
    tx(s, &c->addr, r, sizeof r);
}

static void on_packet(Sess *s, const uint8_t *p, int n, const Addr *from, uint64_t now) {
    if (n < 5 || get32(p) != MAGIC) return;
    int type = p[4];
    if (type >= PT_REL) {
        if (n < HDR) return;
        uint64_t token = get64(p + 5);
        for (int i = 0; i < NP_MAX_PEERS; i++) {
            Conn *c = &s->conns[i];
            if (c->used && c->token == token && same_addr(&c->addr, from)) { on_connected(s, c, type, p, n, now); return; }
        }
        return;
    }
    if (s->host) {
        if (now / 1000 != s->rl_second) { s->rl_second = now / 1000; s->rl_hello = s->rl_fail = 0; }
        if (type == PT_HELLO && n >= HELLO_SIZE) {
            if (++s->rl_hello > 100) return;
            uint8_t r[21];
            put32(r, MAGIC); r[4] = PT_CHALLENGE;
            memcpy(r + 5, p + 7, 8);
            put64(r + 13, cookie(s, from, get64(p + 7)));
            tx(s, from, r, sizeof r);
        } else if (type == PT_JOIN && n >= 40) {
            uint64_t cn = get64(p + 8), hn = get64(p + 16);
            uint8_t want[16];
            if (s->rl_fail > 10 || hn != cookie(s, from, cn)) return;
            proof(s->secret, cn, hn, 'j', want);
            if (memcmp(want, p + 24, 16)) { s->rl_fail++; return; }
            for (int i = 0; i < NP_MAX_PEERS; i++)                   // our accept was lost: repeat it
                if (s->conns[i].used && s->conns[i].cnonce == cn && same_addr(&s->conns[i].addr, from)) { send_accept(s, &s->conns[i], hn); return; }
            if (get16(p + 5) != PROTO || p[7] != s->purpose) { send_reject(s, from, cn, NP_FAIL_VERSION); return; }
            if (!s->can_participate) { send_reject(s, from, cn, NP_FAIL_CLOSED_ROOM); return; }
            Conn *c = NULL;
            int used = 0;
            for (int i = 0; i < NP_MAX_PEERS; i++) {
                if (s->conns[i].used) used++;
                else if (!c) c = &s->conns[i];
            }
            if (!c || used >= NP_MAX_PEERS - 1) { send_reject(s, from, cn, NP_FAIL_FULL); return; }
            memset(c, 0, sizeof *c);
            c->used = 1; c->addr = *from; c->cnonce = cn; c->rtt = -1; c->last_recv = now; c->order = ++s->order;
            rnd(&c->token, 8);
            c->peer = ++s->next_peer;
            np_logf("net: peer %016llx joined", (unsigned long long)c->peer);
            send_accept(s, c, hn);
            host_peers_changed(s);
        }
        return;
    }
    // guest: only the host's address may talk to us
    Conn *c = &s->conns[0];
    if (!same_addr(&c->addr, from) || n < 13 || get64(p + 5) != s->cnonce) return;
    if (type == PT_CHALLENGE && n >= 21 && s->hs == 0) {
        s->hnonce = get64(p + 13);
        s->hs = 1;
        s->hs_last = 0;
    } else if (type == PT_ACCEPT && n >= 53 && s->hs == 1) {
        uint8_t want[16];
        proof(s->secret, s->cnonce, s->hnonce, 'a', want);
        if (memcmp(want, p + 13, 16)) return;
        c->token = get64(p + 29); c->peer = get64(p + 45); c->used = 1; c->rtt = -1; c->last_recv = now;
        s->local_id = get64(p + 37); s->master_id = c->peer;
        s->hs = 2;
    } else if (type == PT_REJECT && n >= 14 && s->hs == 1) {
        np_logf("net: the host refused the join (reason %d)", p[13]);
        sess_close(s, NP_EV_FAILED, p[13] >= NP_FAIL_REJECTED && p[13] <= NP_FAIL_CLOSED_ROOM ? p[13] : NP_FAIL_REJECTED);
    }
}

static void tick(Sess *s, uint64_t now) {
    if (!s->host && s->state == NP_CONNECTING) {
        Conn *c = &s->conns[0];
        if (now - s->hs_start > CONNECT_MS) { np_logf("net: no answer from the host"); sess_close(s, NP_EV_FAILED, NP_FAIL_TIMEOUT); return; }
        if (s->hs < 2 && now - s->hs_last >= 250) {
            uint8_t p[HELLO_SIZE] = {0};
            s->hs_last = now;
            put32(p, MAGIC);
            if (s->hs == 0) {
                p[4] = PT_HELLO; put16(p + 5, PROTO); put64(p + 7, s->cnonce);
                tx(s, &c->addr, p, HELLO_SIZE);
            } else {
                p[4] = PT_JOIN; put16(p + 5, PROTO); p[7] = (uint8_t)s->purpose; put64(p + 8, s->cnonce); put64(p + 16, s->hnonce);
                proof(s->secret, s->cnonce, s->hnonce, 'j', p + 24);
                tx(s, &c->addr, p, 40);
            }
        }
        if (s->hs == 2 && s->got_peers && c->pongs >= 3) {
            s->state = NP_INROOM;
            np_logf("net: joined the room as %016llx (%d peers, ping %d ms)", (unsigned long long)s->local_id, s->npeers, c->rtt);
            push_ev(s, NP_EV_JOINED, 0, 0, 0, NULL, 0);
            while (s->early_head) {
                Ev *e = s->early_head;
                s->early_head = e->next;
                push_ev(s, NP_EV_DATA, e->e.peer, 1, 0, e->e.data, e->e.len);
                free(e->e.data);
                free(e);
            }
            s->early_tail = NULL;
        }
    }
    for (int i = 0; i < NP_MAX_PEERS; i++) {
        Conn *c = &s->conns[i];
        if (!c->used) continue;
        if (now - c->last_recv > TIMEOUT_MS) {
            if (s->host) host_drop(s, c, "timed out");
            else { np_logf("net: connection to the host timed out"); sess_close(s, s->state == NP_INROOM ? NP_EV_CLOSED : NP_EV_FAILED, s->state == NP_INROOM ? NP_CLOSE_TIMEOUT : NP_FAIL_TIMEOUT); return; }
            continue;
        }
        flush(s, c, now);
        if (now - c->last_ping >= (uint64_t)(c->pongs < 5 ? 60 : 500)) send_ping(s, c, now);
        if (c->need_ack && now >= c->ack_due) { send_simple(s, c, PT_ACK); c->ack_due = 0; }
    }
    if (s->host && s->state == NP_INROOM && now - s->last_peers >= 1000) host_send_peers(s, 0);
}

// ---------------------------------------------------------------------------------------------------- session thread
static uint32_t local_ip(void) {
    // the address the default route would use; connect() on a UDP socket sends nothing
    SOCKET t = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {0};
    int len = sizeof a;
    uint32_t ip = 0;
    a.sin_family = AF_INET; a.sin_port = htons(53); a.sin_addr.s_addr = htonl(0x08080808);
    if (t != INVALID_SOCKET) {
        if (!connect(t, (struct sockaddr *)&a, sizeof a) && !getsockname(t, (struct sockaddr *)&a, &len)) ip = ntohl(a.sin_addr.s_addr);
        closesocket(t);
    }
    return ip ? ip : 0x7f000001;
}
static int host_setup(Sess *s) {
    if (s->steam) {
        uint64_t me = sn_self();
        if (!me) return 0;
        np_code_format((uint32_t)me, NP_STEAM_PORT_INDEX, s->secret, s->code, sizeof s->code);
        np_logf("net: hosting over Steam (account %u)", (unsigned)me);
        return 1;
    }
    struct sockaddr_in a = {0};
    uint16_t base = s->cfg.port ? s->cfg.port : NP_DEFAULT_PORT;
    int ok = 0;
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    for (int i = 0; i < 10 && !ok; i++) {
        s->port = (uint16_t)(base + i);
        a.sin_port = htons(s->port);
        ok = !bind(s->s, (struct sockaddr *)&a, sizeof a);
    }
    if (!ok) { np_logf("net: cannot bind UDP port %u..%u (error %d)", base, base + 9, WSAGetLastError()); return 0; }
    uint32_t ip = local_ip();
    char lip[32], ext[64] = "";
    snprintf(lip, sizeof lip, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    if (s->cfg.public_ip[0]) snprintf(ext, sizeof ext, "%s", s->cfg.public_ip);
    else if (s->cfg.upnp) {
        s->mapped = upnp_map(s->port, lip, ext, sizeof ext);
        np_logf("net: UPnP port mapping %s%s%s", s->mapped ? "made, external address " : "not available", s->mapped ? ext : "",
                s->mapped ? "" : " (players outside this network need the port forwarded by hand)");
    }
    struct in_addr ia;
    if (ext[0] && inet_pton(AF_INET, ext, &ia) == 1 && ia.s_addr) ip = ntohl(ia.s_addr);
    s->ip = ip;
    np_code_format(ip, s->port - base, s->secret, s->code, sizeof s->code);
    np_logf("net: hosting on UDP port %u, address in the code %u.%u.%u.%u", s->port, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return 1;
}

static DWORD WINAPI sess_thread(LPVOID arg) {
    Sess *s = arg;
    Np *np = s->np;
    int ok = s->steam || s->s != INVALID_SOCKET;
    if (ok && s->host) ok = host_setup(s);                           // may take seconds (UPnP); the lock is not held
    EnterCriticalSection(&np->lock);
    if (!ok) sess_close(s, NP_EV_FAILED, NP_FAIL_SOCKET);
    else if (s->host && !s->stop) {
        s->state = NP_INROOM;
        s->npeers = 1; s->peers[0].id = s->local_id; s->peers[0].rtt = 0;
        push_ev(s, NP_EV_JOINED, 0, 0, 0, NULL, 0);
    }
    LeaveCriticalSection(&np->lock);
    uint8_t *buf = malloc(2048);
    while (!s->stop && buf && s->steam) {
        sn_pump();
        EnterCriticalSection(&np->lock);
        uint64_t now = mono_ms();
        for (int k = 0; k < 512 && !s->stop; k++) {
            Addr from = {0};
            int n = sn_recv(buf, 2048, &from.sid);
            if (n < 0) break;
            on_packet(s, buf, n, &from, now);
        }
        if (!s->stop) tick(s, now);
        LeaveCriticalSection(&np->lock);
        Sleep(1);
    }
    while (!s->stop && buf && !s->steam) {
        fd_set fds;
        struct timeval tv = {0, 4000};
        FD_ZERO(&fds);
        FD_SET(s->s, &fds);
        int r = select(0, &fds, NULL, NULL, &tv);
        EnterCriticalSection(&np->lock);
        uint64_t now = mono_ms();
        for (int k = 0; r > 0 && k < 512 && !s->stop; k++) {
            Addr from = {0};
            int fl = sizeof from.in;
            int n = recvfrom(s->s, (char *)buf, 2048, 0, (struct sockaddr *)&from.in, &fl);
            if (n < 0) {
                int e = WSAGetLastError();
                if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;  // ICMP port unreachable from an earlier send
                break;
            }
            if (fl == sizeof from.in && from.in.sin_family == AF_INET) on_packet(s, buf, n, &from, now);
        }
        if (!s->stop) tick(s, now);
        LeaveCriticalSection(&np->lock);
    }
    // leaving: tell the others (np_leave already did when the game asked), then clean up
    EnterCriticalSection(&np->lock);
    for (int i = 0; i < NP_MAX_PEERS; i++)
        if (s->conns[i].used) {
            for (int k = 0; k < 3; k++) send_simple(s, &s->conns[i], PT_BYE);
            conn_free(&s->conns[i]);
        }
    if (np->cur == s) np->cur = NULL;
    LeaveCriticalSection(&np->lock);
    if (s->steam) sn_use(0);
    while (s->early_head) { Ev *e = s->early_head; s->early_head = e->next; free(e->e.data); free(e); }
    if (s->s != INVALID_SOCKET) closesocket(s->s);
    if (s->mapped) upnp_unmap(s->port);
    free(buf);
    free(s);
    InterlockedDecrement(&np->threads);
    return 0;
}

static Sess *sess_new(Np *np, int host, int purpose, const NpConfig *cfg, int steam) {
    static int wsa;
    if (!wsa) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); wsa = 1; }
    Sess *s = calloc(1, sizeof *s);
    if (!s) abort();
    s->np = np; s->host = host; s->purpose = purpose; s->state = NP_CONNECTING;
    if (cfg) s->cfg = *cfg;
    s->s = INVALID_SOCKET;
    if (steam) { s->steam = 1; sn_use(1); }
    else s->s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s->s != INVALID_SOCKET) {
        u_long nb = 1;
        int sz = 1 << 20;
        ioctlsocket(s->s, FIONBIO, &nb);
        setsockopt(s->s, SOL_SOCKET, SO_RCVBUF, (const char *)&sz, sizeof sz);
        setsockopt(s->s, SOL_SOCKET, SO_SNDBUF, (const char *)&sz, sizeof sz);
    }
    s->mono0 = mono_ms();
    s->clock0 = unix_ms();
    s->sync_rtt = 1 << 30;
    s->can_participate = 1;
    return s;
}
static void sess_start(Np *np, Sess *s) {
    np_leave(np);
    EnterCriticalSection(&np->lock);
    np->cur = s;
    InterlockedIncrement(&np->threads);
    LeaveCriticalSection(&np->lock);
    HANDLE h = CreateThread(NULL, 0, sess_thread, s, 0, NULL);
    if (h) CloseHandle(h);
    else {
        EnterCriticalSection(&np->lock);
        sess_close(s, NP_EV_FAILED, NP_FAIL_SOCKET);
        np->cur = NULL;
        LeaveCriticalSection(&np->lock);
        if (s->s != INVALID_SOCKET) closesocket(s->s);
        if (s->steam) sn_use(0);
        free(s);
        InterlockedDecrement(&np->threads);
    }
}

// ---------------------------------------------------------------------------------------------------- API
Np *np_create(void) {
    Np *np = calloc(1, sizeof *np);
    if (!np) abort();
    InitializeCriticalSection(&np->lock);
    return np;
}
void np_destroy(Np *np) {
    NpEvent e;
    np_leave(np);
    while (np->threads) Sleep(5);
    while (np_poll(np, &e)) np_free(e.data);
    DeleteCriticalSection(&np->lock);
    free(np);
}
void np_host(Np *np, int purpose, const NpConfig *cfg) {
    int steam = cfg && cfg->steam && sn_available();
    if (cfg && cfg->steam && !steam) np_logf("net: Steam is not available: the room is a direct (UDP) one");
    Sess *s = sess_new(np, 1, purpose, cfg, steam);
    rnd(&s->secret, 4);
    s->secret &= 0xffffff;
    rnd(s->cookie_key, sizeof s->cookie_key);
    s->local_id = 1;                 // peer ids are small numbers: the game multiplies them by 1000 into 32-bit view ids
    s->next_peer = 1;
    s->master_id = s->local_id;
    sess_start(np, s);
}
static void join_fail(Np *np, Sess *s, int reason, const char *why) {
    np_leave(np);
    EnterCriticalSection(&np->lock);
    np->cur = s;
    np_logf("net: %s", why);
    sess_close(s, NP_EV_FAILED, reason);
    np->cur = NULL;
    LeaveCriticalSection(&np->lock);
    if (s->s != INVALID_SOCKET) closesocket(s->s);
    if (s->steam) sn_use(0);
    free(s);
}
void np_join(Np *np, const char *code, int purpose, const NpConfig *cfg) {
    uint32_t ip = 0, secret = 0;
    int idx = 0, ok = np_code_parse(code, &ip, &idx, &secret);
    int steam = ok && idx == NP_STEAM_PORT_INDEX;
    Sess *s = sess_new(np, 0, purpose, cfg, steam);
    Conn *c = &s->conns[0];
    s->secret = secret;
    if (!ok) { join_fail(np, s, NP_FAIL_CODE, "that is not a room code"); return; }
    if (steam) {
        if (!sn_available()) { join_fail(np, s, NP_FAIL_SOCKET, "the code is a Steam room, but Steam is not available"); return; }
        c->addr.sid = NP_STEAM_ID_BASE | ip;
        if (c->addr.sid == sn_self()) { join_fail(np, s, NP_FAIL_REJECTED, "that is this Steam account's own room"); return; }
        rnd(&s->cnonce, 8);
        s->hs_start = mono_ms();
        np_logf("net: joining a Steam room (account %u)", (unsigned)ip);
        sess_start(np, s);
        return;
    }
    uint16_t port = (uint16_t)((s->cfg.port ? s->cfg.port : NP_DEFAULT_PORT) + idx);
    c->addr.in.sin_family = AF_INET;
    c->addr.in.sin_addr.s_addr = htonl(ip);
    c->addr.in.sin_port = htons(port);
    rnd(&s->cnonce, 8);
    s->hs_start = mono_ms();
    np_logf("net: joining a room at %u.%u.%u.%u:%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port);
    sess_start(np, s);
}
void np_leave(Np *np) {
    EnterCriticalSection(&np->lock);
    Sess *s = np->cur;
    if (s) {
        for (int i = 0; i < NP_MAX_PEERS; i++)
            if (s->conns[i].used) send_simple(s, &s->conns[i], PT_BYE);
        s->state = NP_IDLE;
        np->cur = NULL;                                              // the thread frees the session
        InterlockedExchange(&s->stop, 1);
    }
    LeaveCriticalSection(&np->lock);
}
int np_state(Np *np) {
    EnterCriticalSection(&np->lock);
    int st = np->cur ? np->cur->state : NP_IDLE;
    LeaveCriticalSection(&np->lock);
    return st;
}
int np_poll(Np *np, NpEvent *ev) {
    EnterCriticalSection(&np->lock);
    Ev *e = np->ev_head;
    if (e) {
        np->ev_head = e->next;
        if (!np->ev_head) np->ev_tail = NULL;
        np->ev_bytes -= e->e.len;
        *ev = e->e;
        free(e);
    }
    LeaveCriticalSection(&np->lock);
    return e != NULL;
}
void np_free(void *p) { free(p); }
int np_send(Np *np, const uint8_t *data, uint32_t len, int reliable, int group, const uint64_t *targets, int ntargets) {
    int ok = 0;
    if (len > NP_MAX_MESSAGE - 256 || ntargets < 0 || ntargets > NP_MAX_PEERS) return 0;
    if (group != NP_TO_TARGETS) ntargets = 0;
    uint8_t *m = malloc(11 + 8 * (size_t)ntargets + len);
    if (!m) return 0;
    EnterCriticalSection(&np->lock);
    Sess *s = np->cur;
    if (s && s->state == NP_INROOM) {
        uint32_t ml = 11 + 8 * (uint32_t)ntargets + len;
        m[0] = MSG_GAME;
        put64(m + 1, s->local_id);
        m[9] = (uint8_t)(group == NP_TO_TARGETS ? NP_TO_TARGETS : NP_TO_OTHERS);
        m[10] = (uint8_t)ntargets;
        for (int i = 0; i < ntargets; i++) put64(m + 11 + 8 * i, targets[i]);
        memcpy(m + 11 + 8 * ntargets, data, len);
        if (!s->host) msg_send(s, &s->conns[0], m, ml, reliable);
        else if (group != NP_TO_TARGETS) {
            for (int i = 0; i < NP_MAX_PEERS; i++)
                if (s->conns[i].used) msg_send(s, &s->conns[i], m, ml, reliable);
        } else {
            for (int i = 0; i < ntargets; i++) {
                Conn *c = conn_by_peer(s, targets[i]);
                int dup = 0;
                for (int j = 0; j < i; j++) dup |= targets[j] == targets[i];
                if (c && !dup) msg_send(s, c, m, ml, reliable);
            }
        }
        ok = 1;
    }
    LeaveCriticalSection(&np->lock);
    free(m);
    return ok;
}
uint64_t np_local_id(Np *np) {
    EnterCriticalSection(&np->lock);
    uint64_t v = np->cur && np->cur->state == NP_INROOM ? np->cur->local_id : 0;
    LeaveCriticalSection(&np->lock);
    return v;
}
uint64_t np_master_id(Np *np) {
    EnterCriticalSection(&np->lock);
    uint64_t v = np->cur && np->cur->state == NP_INROOM ? np->cur->master_id : 0;
    LeaveCriticalSection(&np->lock);
    return v;
}
int np_peers(Np *np, uint64_t *out, int max) {
    int n = 0;
    EnterCriticalSection(&np->lock);
    Sess *s = np->cur;
    if (s && s->state == NP_INROOM) {
        if (s->host) host_rebuild_peers(s);
        for (; n < s->npeers && n < max; n++) out[n] = s->peers[n].id;
    }
    LeaveCriticalSection(&np->lock);
    return n;
}
int np_rtt(Np *np, uint64_t peer) {
    int r = -1;
    EnterCriticalSection(&np->lock);
    Sess *s = np->cur;
    if (s && s->state == NP_INROOM) {
        if (peer == s->local_id) r = 0;
        else if (s->host) { Conn *c = conn_by_peer(s, peer); if (c) r = c->rtt; }
        else if (peer == s->master_id) r = s->conns[0].rtt;
        else
            for (int i = 0; i < s->npeers; i++)
                if (s->peers[i].id == peer && s->peers[i].rtt >= 0 && s->conns[0].rtt >= 0) r = s->peers[i].rtt + s->conns[0].rtt;
    }
    LeaveCriticalSection(&np->lock);
    return r;
}
int np_can_participate(Np *np) {
    EnterCriticalSection(&np->lock);
    int v = np->cur && np->cur->state == NP_INROOM ? np->cur->can_participate : 0;
    LeaveCriticalSection(&np->lock);
    return v;
}
int np_set_can_participate(Np *np, int on) {
    int ok = 0;
    EnterCriticalSection(&np->lock);
    Sess *s = np->cur;
    if (s && s->state == NP_INROOM && s->host) {
        if (s->can_participate != !!on) {
            s->can_participate = !!on;
            s->peers_rev++;
            host_send_peers(s, 1);
            push_ev(s, NP_EV_FLAGS, 0, 0, 0, NULL, 0);
        }
        ok = 1;
    }
    LeaveCriticalSection(&np->lock);
    return ok;
}
int64_t np_time_ms(Np *np) {
    EnterCriticalSection(&np->lock);
    int64_t t = np->cur && np->cur->state == NP_INROOM ? room_clock(np->cur) : unix_ms();
    LeaveCriticalSection(&np->lock);
    return t;
}
void np_code(Np *np, char *out, size_t n) {
    EnterCriticalSection(&np->lock);
    snprintf(out, n, "%s", np->cur && np->cur->state == NP_INROOM && np->cur->host ? np->cur->code : "");
    LeaveCriticalSection(&np->lock);
}
