// PC online play: transport for private rooms joined by code (netplay.c) and UPnP port mapping (upnp.c).
// Pure host code: nothing here touches guest or managed memory, so it is also built into the stand-alone tests.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Np Np;

enum { NP_IDLE, NP_CONNECTING, NP_INROOM };
enum {
    NP_EV_NONE,
    NP_EV_DATA,          // a game message: peer = sender, reliable, data/len (free with np_free)
    NP_EV_JOINED,        // the room is entered (state is NP_INROOM)
    NP_EV_FAILED,        // creating or joining failed (state is NP_IDLE); reason = NP_FAIL_*
    NP_EV_PEERS,         // peer list or master changed
    NP_EV_FLAGS,         // can-participate flag changed
    NP_EV_CLOSED,        // the room ended while in it (state is NP_IDLE); reason = NP_CLOSE_*
};
enum { NP_FAIL_CODE = 1, NP_FAIL_SOCKET, NP_FAIL_TIMEOUT, NP_FAIL_REJECTED, NP_FAIL_VERSION, NP_FAIL_FULL, NP_FAIL_CLOSED_ROOM };
enum { NP_CLOSE_HOST_LEFT = 1, NP_CLOSE_TIMEOUT, NP_CLOSE_ERROR };
enum { NP_TO_OTHERS, NP_TO_TARGETS };
#define NP_MAX_PEERS 16
#define NP_MAX_MESSAGE (256 * 1024)

typedef struct NpEvent {
    int type, reliable, reason;
    uint64_t peer;
    uint32_t len;
    uint8_t *data;
} NpEvent;

typedef struct NpConfig {
    uint16_t port;            // base UDP port (0 = default): a host listens on it or one of the next 9
    int upnp;                 // ask the router for a port mapping when hosting
    char public_ip[64];       // address to put in the code instead of the detected one ("" = detect)
    int steam;                // host over Steam when it is available (steam_net.c); joining follows the code
    // test knobs
    int loss_percent;         // drop this share of outgoing packets
    int no_thread;            // not used by the game
} NpConfig;
#define NP_DEFAULT_PORT 23711
#define NP_STEAM_PORT_INDEX 15            // the port index of a code that names a Steam account
#define NP_STEAM_ID_BASE 0x0110000100000000ull   // individual account in the public universe; + 32-bit account number

Np *np_create(void);
void np_destroy(Np *np);
// purpose: an arbitrary number both sides must agree on (the game's room purpose). Both return at once; the result
// arrives as NP_EV_JOINED or NP_EV_FAILED.
void np_host(Np *np, int purpose, const NpConfig *cfg);
void np_join(Np *np, const char *code, int purpose, const NpConfig *cfg);
void np_leave(Np *np);
int np_state(Np *np);
int np_poll(Np *np, NpEvent *ev);                 // 1 when an event was returned
void np_free(void *p);
// group NP_TO_OTHERS: every other peer; NP_TO_TARGETS: the listed peers (the local id is ignored). 1 on success.
int np_send(Np *np, const uint8_t *data, uint32_t len, int reliable, int group, const uint64_t *targets, int ntargets);
uint64_t np_local_id(Np *np);
uint64_t np_master_id(Np *np);
int np_peers(Np *np, uint64_t *out, int max);     // all peers in the room including the local one, host first
int np_rtt(Np *np, uint64_t peer);                // milliseconds, -1 when unknown
int np_can_participate(Np *np);
int np_set_can_participate(Np *np, int on);       // host only; 1 on success
int64_t np_time_ms(Np *np);                       // room clock (the host's UTC clock), Unix milliseconds
void np_code(Np *np, char *out, size_t n);        // "" unless hosting
// port_index: the host's port minus its base port (0..15); both sides must use the same base port
int np_code_parse(const char *code, uint32_t *ip, int *port_index, uint32_t *secret);
void np_code_format(uint32_t ip, int port_index, uint32_t secret, char *out, size_t n);
// Tag of the game content this build plays with (0 = the plain game). Peers with different tags cannot join each
// other: the join fails with NP_FAIL_VERSION. Call before hosting or joining.
void np_set_content_tag(uint32_t tag);

// upnp.c: 1 on success, ext_ip receives the router's external address ("" when unknown)
int upnp_map(uint16_t port, const char *local_ip, char *ext_ip, size_t n);
void upnp_unmap(uint16_t port);

// steam_net.c: Steam as the carrier (loaded at runtime; all of these fail softly without Steam)
int sn_available(void);                           // starts Steam if needed; 1 when it is up
uint64_t sn_self(void);
void sn_use(int on);                              // a session starts / stops using Steam (keeps it from shutting down)
int sn_send(uint64_t to, const void *p, int n);
int sn_recv(uint8_t *buf, int cap, uint64_t *from);
void sn_pump(void);
void sn_close(uint64_t peer);
void sn_presence(const char *value);              // NULL clears
int sn_friend_room(const char *prefix, char *code, size_t ncode, char *name, size_t nname);
void sn_tick(void);                               // once a frame from the game
void sn_shutdown(void);                           // before the process exits
void np_steam_prefix(char *out, size_t n);        // the presence prefix of this build's protocol

void np_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // provided by the embedding program

#ifdef __cplusplus
}
#endif
