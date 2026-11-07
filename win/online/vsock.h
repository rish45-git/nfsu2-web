/* vsock.h -- a virtual Winsock for the original SPEED2.EXE: the game's sockets live on the server's virtual LAN
 * switch (web/server-rs/crates/lan), exactly as in the browser (runtime/winsock.c over runtime/web/net_lan.c).
 *
 *   game threads --IAT--> vs_* Winsock (vsock_ws2.c) --> virtual IP stack (vsock.c) --TX ring--> writer thread
 *       --transport.send--> the switch client (switch.c, WebSocket) --> the server: its Online host 10.77.0.1, players
 *   switch reader thread --vs_rx_frame--> vsock.c --> waiting game threads
 *
 * The stack decides everything a real host's IP stack would (ports, UDP delivery, the switch's own stream protocol
 * for TCP, ICMP echo); the switch only routes frames by virtual address.  Frames are the switch's (big-endian):
 *   [0] kind  [1..3] 0  [4..7] source vip  [8..11] destination vip  [12..13] source port  [14..15] destination port
 *   [16..23] the LAN header (op, ttl, 0, 0, connection id little-endian)  [24..] data
 * with kind 3 (UDP), 4 (TCP segment), 5 (ICMP) (runtime/web/net_proto.h).
 *
 * Freestanding: kernel32 only (no C runtime), like the rest of the plugin.  Vista or newer (condition variables). */
#ifndef NFSU2_VSOCK_H
#define NFSU2_VSOCK_H
#include <stdint.h>

typedef struct {
    /* the writer thread, without any lock of the stack's: a whole frame for the switch; 1 sent, 0 not (offline) */
    int  (*send)(void *u, const uint8_t *frame, int len);
    /* the writer thread, in order with the frames: LAN play open (a network session: the game's first WSAStartup to
     * its last WSACleanup) or shut, and its UDP ports (0x80000000 | bit port % 31 of each bound port) */
    void (*lan)(void *u, int on, uint32_t udp_ports);
    /* a session starts (WSAStartup) and the link is not up: start it (any game thread; no lock of the stack held);
     * the stack then waits up to 5 s for vs_link_up */
    void (*want)(void *u);
    void (*log)(void *u, const char *line);
    void *user;
} vs_transport;

/* once, before the game's first Winsock call (the IAT patch); 0 ok */
int  vs_init(const vs_transport *t);
/* the switch client's events (its reader thread) */
void vs_link_up(uint32_t vip, uint32_t mask);    /* every welcome (a reconnect too) */
void vs_link_down(void);                         /* the WebSocket is gone (reconnecting, or removed) */
void vs_rx_frame(const uint8_t *f, int len);     /* a binary frame of kind 3, 4 or 5 */
void vs_peer_gone(uint32_t vip);                 /* a peer delta's "gone": its TCP connections are reset */
int  vs_session_active(void);                    /* a WSAStartup without its last WSACleanup */
void vs_report(void);                            /* the counters, one log line */
void vs_exit_report(void);                       /* the game's calls since the last report (at exit: no lock taken) */
/* the plugin's own datagrams of a race (mprace.c's quit notice): to and from UDP port on a socket the game never sees;
 * the race's peers (UDP 3658 sent to or heard from within within_ms), host order */
int  vs_rt_send(uint16_t port, uint32_t ip_h, const void *p, int n);          /* 0 sent, -1 not */
int  vs_rt_recv(uint16_t port, uint32_t *ip_h, void *p, int cap);            /* its length, -1 none */
int  vs_race_peers(uint32_t *ip_h, int max, uint32_t within_ms);

/* The replacements, as the game imports them (ws2_32 by ordinal, WSAIoctl by name, netapi32!Netbios). */
typedef struct { const char *dll; int ord; const char *name; void *fn; } vs_import;
extern const vs_import vs_imports[];             /* ends with dll 0 */

/* SPEED2.EXE v1.2 NA: the lobby host "*pcnfs05.ea.com" at 0x7FBDBC without its '*' (no SSL 2.0 on 20921: plain
 * TCP to 20920), in memory; 1 done (or already), 0 not the expected bytes (online stays off) */
int  vs_patch_lobby_host(void);

#endif
