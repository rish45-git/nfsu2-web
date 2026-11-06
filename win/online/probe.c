/* probe.c -- a console test of switch.c against a server (the local one in WSL, or any other the operator allows):
 *
 *   switch_probe.exe [ws://127.0.0.1:8691/lan] [name] [seconds]
 *
 * Joins the switch, prints the welcome, the peer list and the round trips, then checks the server's own host
 * 10.77.0.1 with the frames the game's Winsock layer will send: an ICMP echo (answered), a TCP SYN to the
 * redirector 20920 (SYNACK) and to a port nothing listens on (RST), and a UDP datagram (port unreachable).
 * Exit code 0 when every check passed.  Built with a normal C runtime; switch.c itself needs none. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "switch.h"

static volatile LONG g_welcomed, g_pongs, g_echo, g_synack, g_rst, g_unreach, g_other, g_peers;

static void on_log(void *u, const char *l){ (void)u; printf("[switch] %s\n", l); fflush(stdout); }
static void on_welcome(void *u, uint32_t vip, uint32_t mask, const char *tok){
    (void)u; printf("welcome: vip %u.%u.%u.%u mask %08x token %zu chars\n", vip >> 24, (vip >> 16) & 255, (vip >> 8) & 255, vip & 255, mask, strlen(tok));
    InterlockedExchange(&g_welcomed, 1);
}
static void on_peers(void *u, const char *j, int n){ (void)u; InterlockedIncrement(&g_peers); printf("peers: %.*s\n", n > 300 ? 300 : n, j); }
static void on_pong(void *u, uint32_t id, double rtt){ (void)u; printf("pong %u: %.2f ms\n", id, rtt); InterlockedIncrement(&g_pongs); }
static void on_removed(void *u, int ban, const char *m){ (void)u; printf("removed (%s): %s\n", ban ? "ban" : "kick", m); }
static void on_queue(void *u, int pos, int of){ (void)u; printf("in line: %d of %d\n", pos, of); }
static void on_frame(void *u, const uint8_t *f, int n){
    (void)u;
    unsigned kind = f[0], op = n > 16 ? f[16] : 0;
    printf("frame kind %u from %u.%u.%u.%u, %d bytes, op %u\n", kind, f[4], f[5], f[6], f[7], n, op);
    if (kind == SW_KIND_LAN_ICMP && n > 24 && f[24] == 0) InterlockedIncrement(&g_echo);
    else if (kind == SW_KIND_LAN_TCP && op == 2) InterlockedIncrement(&g_synack);
    else if (kind == SW_KIND_LAN_TCP && op == 3) InterlockedIncrement(&g_rst);
    else if (kind == SW_KIND_LAN_UDP && op == 1) InterlockedIncrement(&g_unreach);
    else InterlockedIncrement(&g_other);
}

/* a LAN frame (net_proto.h): 16 bytes of header, the 8-byte LAN header (op, ttl, 0, 0, connection id LE), data */
static int lan_frame(sw *s, uint8_t kind, uint16_t sport, uint16_t dport, uint8_t op, uint32_t cid, const uint8_t *d, int n){
    uint8_t f[256]; memset(f, 0, sizeof f);
    f[0] = kind; f[8] = 10; f[9] = 77; f[10] = 0; f[11] = 1;        /* 10.77.0.1 */
    f[12] = (uint8_t)(sport >> 8); f[13] = (uint8_t)sport; f[14] = (uint8_t)(dport >> 8); f[15] = (uint8_t)dport;
    f[16] = op; f[17] = 64; memcpy(f + 20, &cid, 4);
    if (n) memcpy(f + 24, d, n);
    return sw_send_frame(s, f, 24 + n);
}
static int wait_for(volatile LONG *v, int ms){ for (int t = 0; t < ms && !*v; t += 20) Sleep(20); return *v != 0; }

int main(int argc, char **argv){
    const char *url = argc > 1 ? argv[1] : "ws://127.0.0.1:8691/lan";
    const char *name = argc > 2 ? argv[2] : "probe";
    int secs = argc > 3 ? atoi(argv[3]) : 3;
    sw_options o = { url, name, 0, 1, 1000 };
    sw_callbacks cb = { on_welcome, on_frame, on_peers, on_queue, on_removed, on_pong, 0, on_log, 0 };
    sw *s = sw_start(&o, &cb);
    if (!s){ printf("FAIL: bad URL\n"); return 2; }
    int fails = 0;
#define CHECK(what, cond) do { int ok_ = (cond); printf("%s  %s\n", ok_ ? "PASS" : "FAIL", what); if (!ok_) fails++; } while (0)

    CHECK("welcome (a virtual address)", wait_for(&g_welcomed, 10000) && sw_vip(s));
    if (g_welcomed){
        CHECK("peer list (lan:1: the server's hosts)", wait_for(&g_peers, 3000));
        /* ICMP echo request: type 8, code 0, checksum (the server recomputes its own), id 1, seq 1 */
        uint8_t echo[12] = { 8, 0, 0, 0, 0, 1, 0, 1, 'p', 'i', 'n', 'g' };
        lan_frame(s, SW_KIND_LAN_ICMP, 0, 0, 0, 0, echo, sizeof echo);
        CHECK("ICMP echo to 10.77.0.1 answered", wait_for(&g_echo, 3000));
        lan_frame(s, SW_KIND_LAN_TCP, 40001, 20920, 1, 0x1234, 0, 0);       /* SYN to the redirector */
        CHECK("TCP SYN to 20920 (redirector) gets SYNACK", wait_for(&g_synack, 3000));
        lan_frame(s, SW_KIND_LAN_TCP, 40002, 9, 1, 0x5678, 0, 0);           /* SYN to a closed port */
        CHECK("TCP SYN to a closed port gets RST", wait_for(&g_rst, 3000));
        lan_frame(s, SW_KIND_LAN_UDP, 40003, 5555, 0, 0, (const uint8_t *)"x", 1);
        CHECK("UDP to 10.77.0.1:5555 gets port unreachable", wait_for(&g_unreach, 3000));
        Sleep(secs * 1000);
        CHECK("JSON ping/pong round trips", g_pongs > 0);
    }
    sw_stop(s);
    printf(fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", fails);
    return fails ? 1 : 0;
}
