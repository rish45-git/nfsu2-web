/* switch.h -- a client of the server's virtual LAN switch (web/server-rs/crates/lan, /lan), for the native
 * Windows plugin: what web/src/net/index.ts is in the browser.
 *
 * One WebSocket (WinHTTP, so ws:// and wss:// need no other DLL).  Binary messages are the game's network frames
 * ([0] kind, [4..7] source vip, [8..11] destination vip, [12..] ports + data, big-endian); text messages are
 * JSON control messages.  This client sends a plain hello (no "bundle", "compact" or "delta": the server then sends
 * and takes one frame per message, which is all the first version needs).
 *
 * Freestanding: kernel32 and winhttp only, no C runtime (like the rest of the plugin), so it can be built into the
 * ASI.  One reader thread runs the callbacks; sw_send* may be called from any thread.
 */
#ifndef NFSU2_SWITCH_H
#define NFSU2_SWITCH_H
#include <stdint.h>

#define SW_KIND_UDP        1u   /* free roam's datagrams (net_proto.h) */
#define SW_KIND_LAN_UDP    3u
#define SW_KIND_LAN_TCP    4u
#define SW_KIND_LAN_ICMP   5u
#define SW_BROADCAST       0xFFFFFFFFu
#define SW_VIP_ONLINE      0x0A4D0001u   /* 10.77.0.1: the server's Online host */
#define SW_VIP_ROAM_HUB    0x0A4D00FDu   /* 10.77.0.253: the free-roam hub */

typedef enum { SW_IDLE, SW_CONNECTING, SW_ONLINE, SW_QUEUED, SW_DOWN, SW_REMOVED } sw_state;

typedef struct sw sw;

typedef struct {
    /* all on the reader thread; any may be 0 */
    void (*on_welcome)(void *u, uint32_t vip, uint32_t mask, const char *token);
    void (*on_frame)(void *u, const uint8_t *frame, int len);                 /* a binary message */
    void (*on_peers)(void *u, const char *json, int len);                     /* "peers" and "delta", as received */
    void (*on_queue)(void *u, int pos, int of);
    void (*on_removed)(void *u, int ban, const char *msg);                    /* kick or ban: the close follows */
    void (*on_pong)(void *u, uint32_t id, double rtt_ms);
    void (*on_text)(void *u, const char *json, int len);                      /* every text message, before the above */
    void (*on_log)(void *u, const char *line);
    void *user;
} sw_callbacks;

typedef struct {
    const char *url;        /* ws://host:port/lan or wss://host/lan (the client adds ?queue, and ?resume to a retry) */
    const char *name;       /* the player's name (the server cleans it: at most 32 printable characters) */
    const char *claim;      /* a claimed name's keyphrase token, or 0 */
    int lan;                /* 1: LAN play is open: the peer list with the server's hosts, LAN broadcasts */
    int ping_ms;            /* 0: 10000 */
} sw_options;

sw *sw_start(const sw_options *o, const sw_callbacks *cb);   /* connects in the background, reconnects itself */
void sw_stop(sw *s);
sw_state sw_status(const sw *s);
uint32_t sw_vip(const sw *s);                                /* 0 until the welcome */

/* a whole frame as the switch takes it (the source is rewritten there); 1 sent, 0 not (offline) */
int sw_send_frame(sw *s, const uint8_t *frame, int len);
/* a frame made here: kind, source (ignored by the switch), destination, ports, then data */
int sw_send_udp(sw *s, uint8_t kind, uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t *data, int len);
int sw_send_text(sw *s, const char *json);
int sw_ping(sw *s);                                          /* a ping: on_pong follows */

#endif
