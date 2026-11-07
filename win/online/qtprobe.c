/* qtprobe.c -- a race's quit notice between two games on the virtual LAN, without the game: what the plugin's
 * mprace_win.c does with vsock.c's vs_race_peers / vs_rt_send / vs_rt_recv ("N2QT" on UDP 3659 to the peers of the
 * race link, UDP 3658).
 *
 *   qtprobe.exe b <ws://host:port/lan> [seconds=30]          prints its VIP, answers race datagrams, waits for N2QT
 *   qtprobe.exe a <ws://host:port/lan> <b's vip a.b.c.d>      race datagrams to b, then the quit notice
 *
 * Each opens a UDP socket on 3658 through the plugin's own Winsock stand-ins (vs_imports), as the game's race link
 * does, so the stack notes the other as a race peer; a then sends N2QT to every race peer it has.  b passes when it
 * has a as a race peer and the notice came from a on 3659; a when b was its race peer and the send went.  Exit 0 when
 * it passes. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vsock.h"
#include "switch.h"

static void *fn(const char *name){
    for (const vs_import *i = vs_imports; i->dll; i++) if (i->name && !strcmp(i->name, name)) return i->fn;
    printf("no replacement for %s\n", name); ExitProcess(2);
}
static sw *g_sw; static const char *g_url, *g_name = "qtprobe";
static volatile LONG g_vip;
static int tr_send(void *u, const uint8_t *f, int len){ (void)u; return g_sw && sw_send_frame(g_sw, f, len); }
static void tr_lan(void *u, int on, uint32_t udp){ (void)u; if (g_sw) sw_set_lan(g_sw, on, udp); }
static void sw_welcome(void *u, uint32_t vip, uint32_t mask, const char *t){
    (void)u; (void)t; InterlockedExchange(&g_vip, (LONG)vip);
    printf("VIP %u.%u.%u.%u\n", vip >> 24, (vip >> 16) & 255, (vip >> 8) & 255, vip & 255); fflush(stdout);
    vs_link_up(vip, mask);
}
static void sw_frame(void *u, const uint8_t *f, int len){ (void)u; if (len >= 16 && f[0] >= 3 && f[0] <= 5) vs_rx_frame(f, len); }
static void sw_down(void *u){ (void)u; vs_link_down(); }
static void sw_logcb(void *u, const char *l){ (void)u; printf("  [switch] %s\n", l); }
static void tr_want(void *u){
    (void)u;
    if (g_sw) return;
    sw_options o = { g_url, g_name, 0, 1, 10000 };
    sw_callbacks cb = { sw_welcome, sw_frame, 0, 0, 0, 0, 0, sw_logcb, 0, sw_down };
    g_sw = sw_start(&o, &cb);
}
static void tr_log(void *u, const char *l){ (void)u; printf("  [vsock] %s\n", l); fflush(stdout); }
static const vs_transport TR = { tr_send, tr_lan, tr_want, tr_log, 0 };
static int fails;
#define CHECK(what, cond) do { int ok_ = (cond); printf("%s  %s\n", ok_ ? "PASS" : "FAIL", what); fflush(stdout); if (!ok_) fails++; } while (0)

typedef int (WSAAPI *startup_t)(WORD, LPWSADATA);
typedef SOCKET (WSAAPI *socket_t)(int, int, int);
typedef int (WSAAPI *bind_t)(SOCKET, const struct sockaddr *, int);
typedef int (WSAAPI *sendto_t)(SOCKET, const char *, int, int, const struct sockaddr *, int);
typedef int (WSAAPI *recvfrom_t)(SOCKET, char *, int, int, struct sockaddr *, int *);
typedef int (WSAAPI *ioctl_t)(SOCKET, long, u_long *);
static SOCKET race_sock(void){
    WSADATA wd; ((startup_t)fn("WSAStartup"))(0x0202, &wd);
    for (int i = 0; i < 100 && !g_vip; i++) Sleep(100);
    SOCKET s = ((socket_t)fn("socket"))(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons(3658);
    int r = ((bind_t)fn("bind"))(s, (struct sockaddr *)&a, sizeof a);
    u_long nb = 1; ((ioctl_t)fn("ioctlsocket"))(s, FIONBIO, &nb);
    CHECK("a UDP socket on 3658 (the race link's port)", s != INVALID_SOCKET && r == 0);
    return s;
}
static uint32_t ip_of(const char *s){ unsigned a, b, c, d; return sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 ? (a << 24 | b << 16 | c << 8 | d) : 0; }

int main(int argc, char **argv){
    setvbuf(stdout, 0, _IONBF, 0);
    if (argc < 3){ printf("usage: qtprobe b <url> [secs] | qtprobe a <url> <b's vip>\n"); return 2; }
    g_url = argv[2];
    int b = !strcmp(argv[1], "b");
    g_name = b ? "qtprobe-b" : "qtprobe-a";
    if (vs_init(&TR) != 0){ printf("vs_init failed\n"); return 2; }
    SOCKET s = race_sock();
    sendto_t st = (sendto_t)fn("sendto"); recvfrom_t rf = (recvfrom_t)fn("recvfrom");
    char buf[256];
    if (b){
        int secs = argc > 3 ? atoi(argv[3]) : 30, got = 0, peer = 0; uint32_t from = 0, a_ip = 0;
        for (int t = 0; t < secs * 20 && !got; t++){
            struct sockaddr_in f; int fl = sizeof f;
            int n = rf(s, buf, sizeof buf, 0, (struct sockaddr *)&f, &fl);
            if (n > 0){ a_ip = ntohl(f.sin_addr.s_addr); st(s, buf, n, 0, (struct sockaddr *)&f, fl); }   /* the race's answer */
            uint32_t p[16]; int np = vs_race_peers(p, 16, 5000);
            for (int i = 0; i < np; i++) if (a_ip && p[i] == a_ip) peer = 1;
            uint8_t m[64]; int k = vs_rt_recv(3659, &from, m, sizeof m);
            if (k >= 24 && !memcmp(m, "N2QT", 4)){ got = 1; printf("N2QT from %u.%u.%u.%u: \"%.15s\"\n", from >> 24, (from >> 16) & 255, (from >> 8) & 255, from & 255, (const char *)m + 8); }
            Sleep(50);
        }
        CHECK("b: a is a race peer (its 3658 datagrams noted)", peer);
        CHECK("b: the quit notice came, from a, on 3659", got && from == a_ip);
    } else {
        uint32_t bip = argc > 3 ? ip_of(argv[3]) : 0;
        struct sockaddr_in to = { 0 }; to.sin_family = AF_INET; to.sin_port = htons(3658); to.sin_addr.s_addr = htonl(bip);
        int answered = 0;
        for (int t = 0; t < 40 && !answered; t++){
            st(s, "RACE", 4, 0, (struct sockaddr *)&to, sizeof to);
            Sleep(50);
            struct sockaddr_in f; int fl = sizeof f;
            if (rf(s, buf, sizeof buf, 0, (struct sockaddr *)&f, &fl) > 0) answered = 1;
        }
        CHECK("a: b answered on the race link", answered);
        uint32_t p[16]; int np = vs_race_peers(p, 16, 5000), found = 0;
        for (int i = 0; i < np; i++) if (p[i] == bip) found = 1;
        CHECK("a: b is a race peer", found);
        uint8_t m[24] = { 'N', '2', 'Q', 'T', 1 }; memcpy(m + 8, "PROBE-A", 7);
        int sent = 0;
        for (int k = 0; k < 3; k++) for (int i = 0; i < np; i++) sent += vs_rt_send(3659, p[i], m, sizeof m) == 0;
        CHECK("a: the quit notice sent to the race's peers (3 times)", sent >= 3);
        Sleep(1000);
    }
    printf(fails ? "FAILED: %d\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
