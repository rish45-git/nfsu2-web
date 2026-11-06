/* roam_win.c -- free-roam multiplayer: the Winsock platform layer under the
 * core, for NFSU2FreeRoam.asi.  It implements runtime/roam_plat.h exactly as
 * runtime/roam_bsd.c does on the Mac: one UDP socket, a network thread (or,
 * with NetThread=0 in the .ini, polling from the frame hook), the session
 * lock, the monotonic clock, the test-harness guard, the LAN discovery
 * targets.
 *
 * Test-harness guard (fail closed), as on the Mac: in test mode
 * (NFSU2_ROAM_HARNESS=1 in the environment, or TestMode=1 in the .ini) the
 * socket is bound to 127.0.0.1, every destination outside 127/8 and every
 * broadcast is refused before it reaches Winsock and logged as
 * "[roam] BLOCKED ...", and listed peers outside 127/8 are refused.
 *
 * Windows specifics: an ICMP "port unreachable" makes the next recvfrom of a
 * UDP socket fail with WSAECONNRESET (switched off with SIO_UDP_CONNRESET,
 * and ignored anyway); the interfaces for the directed broadcasts come from
 * SIO_GET_INTERFACE_LIST; the clock is QueryPerformanceCounter.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
/* windows.h still defines the 16-bit far and near as nothing: roam_core.h has fields of those names */
#undef far
#undef near
#include "roam_plat.h"
#include "switch.h"                       /* win/online: the client of the server's virtual LAN switch */
#include "vsock.h"                        /* win/online: the game's own Winsock on that switch (the Online menu) */

void roam_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *roam_opt(const char *name);
void roam_game_event(int ev, int peer, const char *text);
int snprintf(char *buf, size_t n, const char *fmt, ...);
uint32_t roam_win_mxcsr_default(void);

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

static SOCKET           g_fd = INVALID_SOCKET;
static HANDLE           g_thr;
static volatile LONG    g_stop;
static CRITICAL_SECTION g_mu;
static volatile LONG    g_mu_init;
static rc_session      *g_s;
static roam_plat_cfg    g_cfg;
static roam_plat_stats  g_st;          /* written under g_mu */
static int              g_wsa;
static int              g_threaded;
static int64_t          g_next_tick;
static LARGE_INTEGER    g_qpf;

/* Online (Server = ws://host:port/lan in the .ini): no UDP socket.  The datagrams go to the server's virtual LAN
 * switch as kind-1 frames (runtime/web/net_proto.h NET_KIND_UDP), the players near us come from its free-roam hub
 * (10.77.0.253), exactly as in the browser (runtime/web/roam_web.c) */
static sw              *g_sw;
static int              g_online;
static volatile LONG    g_vip, g_mask;     /* our virtual address and netmask, from the welcome */
static int64_t          g_sw_start_at;     /* the switch starts when the player's name is known (or after 45 s) */
static char             g_server[200];
static char             g_claim[100];      /* the claim token for g_server, once the server switch chose one (g_claim_set) */
static int              g_claim_set;
static volatile LONG    g_sw_busy;         /* online_start_switch running (free roam's thread or a game thread) */
/* the game's Online menu (and LAN screens) on the switch: win/online/vsock.c (vs_transport below) */
static int              g_vs_on;
static volatile LONG    g_lan_on, g_lan_udp;   /* LAN play open, its UDP ports, as vsock last said: for a switch not started yet */

static void mu_init(void){
    if (InterlockedCompareExchange(&g_mu_init, 1, 0) == 0){ InitializeCriticalSection(&g_mu); g_mu_init = 2; }
    while (g_mu_init != 2) Sleep(0);
}
void roam_plat_lock(void){ mu_init(); EnterCriticalSection(&g_mu); }
void roam_plat_unlock(void){ LeaveCriticalSection(&g_mu); }
int64_t roam_plat_now_us(void){
    LARGE_INTEGER c;
    if (!g_qpf.QuadPart && (!QueryPerformanceFrequency(&g_qpf) || !g_qpf.QuadPart)) g_qpf.QuadPart = 1000;
    QueryPerformanceCounter(&c);
    int64_t f = g_qpf.QuadPart, q = c.QuadPart / f, r = c.QuadPart % f;
    return q * 1000000 + r * 1000000 / f;
}
uint32_t roam_plat_random(void){
    /* test aid (tests/asi_harness.c): a fixed session id, to compare the
     * plugin's datagrams byte for byte with the Mac build's */
    const char *tid = roam_opt("NFSU2_ROAM_TEST_ID");
    if (tid && *tid){
        uint32_t v = 0;
        for (const char *p = tid[0] == '0' && (tid[1] == 'x' || tid[1] == 'X') ? tid + 2 : tid; *p; p++)
            v = v << 4 | (uint32_t)(*p >= 'a' ? *p - 'a' + 10 : *p >= 'A' ? *p - 'A' + 10 : *p - '0');
        if (v) return v;
    }
    /* a session id: unique per start, not a secret */
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    uint32_t h = 0x811C9DC5u, v[6] = { (uint32_t)c.QuadPart, (uint32_t)(c.QuadPart >> 32), GetTickCount(),
                                       GetCurrentProcessId(), GetCurrentThreadId(), (uint32_t)(uintptr_t)&c };
    static uint32_t salt;
    for (int i = 0; i < 6; i++) for (int k = 0; k < 4; k++){ h ^= (v[i] >> (8 * k)) & 255u; h *= 0x01000193u; }
    h ^= ++salt * 0x9E3779B9u;
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return h ? h : 1u;
}
const char *roam_ip_str(uint32_t ip, char *b){
    snprintf(b, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u); return b; }
void roam_plat_get_stats(roam_plat_stats *o){ roam_plat_lock(); *o = g_st; roam_plat_unlock(); }

static int opt_on(const char *k){ const char *v = roam_opt(k); return v && *v && !(v[0] == '0' && !v[1]); }
int roam_plat_test_mode(void){
    return opt_on("NFSU2_ROAM_HARNESS") || opt_on("NFSU2_NET_HARNESS");
}

/* ---- the guard ---------------------------------------------------------------- */
static void blocked(const char *what, uint32_t ip){
    char b[16];
    g_st.blocked++;
    if (g_st.blocked <= 20 || g_st.blocked % 1000 == 0)
        roam_log("[roam] BLOCKED %s %s (test harness: loopback only; %llu refused so far)\n",
                 what, roam_ip_str(ip, b), (unsigned long long)g_st.blocked);
}
int roam_plat_guard(int test_mode, uint32_t ip, int is_bcast){
    if (!test_mode) return 1;
    if (is_bcast){ blocked("broadcast to", ip); return 0; }
    if ((ip >> 24) != 127){ blocked("sendto", ip); return 0; }
    return 1;
}

/* ---- sending (with the lock held, from either thread) ------------------------------ */
static uint16_t be16(uint16_t v){ return (uint16_t)(v >> 8 | v << 8); }
static uint32_t be32(uint32_t v){ return v >> 24 | (v >> 8 & 0xFF00u) | (v << 8 & 0xFF0000u) | v << 24; }
static void do_send_online(uint32_t ip, uint16_t port, const uint8_t *buf, int len, int is_bcast){
    uint32_t vip = (uint32_t)g_vip, mask = (uint32_t)g_mask, dst = ip;
    if (!vip || !g_sw) return;                                      /* no address yet: dropped, as UDP would */
    if (is_bcast) dst = SW_BROADCAST;
    else {
        if ((ip & mask) != (vip & mask)){
            g_st.send_err++;
            if (g_st.send_err <= 5 || g_st.send_err % 1000 == 0){
                char b[16]; roam_log("[roam] sendto %s:%u failed: not an address on the virtual LAN (Peers takes the "
                                     "virtual 10.77.x.x addresses online)\n", roam_ip_str(ip, b), port);
            }
            return;
        }
        if (ip == ((vip & mask) | ~mask)) dst = SW_BROADCAST;
    }
    if (sw_send_udp(g_sw, SW_KIND_UDP, dst, g_cfg.port, port, buf, len)){ g_st.sent++; g_st.sent_bytes += (uint64_t)len; }
    else g_st.send_err++;
}
static void do_send(uint32_t ip, uint16_t port, const uint8_t *buf, int len, int is_bcast){
    if (g_online){ do_send_online(ip, port, buf, len, is_bcast); return; }
    if (!roam_plat_guard(g_cfg.test_mode, ip, is_bcast) || g_fd == INVALID_SOCKET) return;
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = be16(port); sa.sin_addr.s_addr = be32(ip);
    int n = sendto(g_fd, (const char *)buf, len, 0, (struct sockaddr *)&sa, sizeof sa);
    if (n == len){ g_st.sent++; g_st.sent_bytes += (uint64_t)len; return; }
    g_st.send_err++;
    if (g_st.send_err <= 5 || g_st.send_err % 1000 == 0){
        char b[16];
        int e = WSAGetLastError();
        roam_log("[roam] sendto %s:%u failed: Winsock error %d%s\n", roam_ip_str(ip, b), port, e,
                 e == WSAEACCES ? " (broadcast not allowed on this socket)" :
                 e == WSAENETUNREACH || e == WSAEHOSTUNREACH ? " (no route: is the PC on the same network?)" :
                 e == WSAEWOULDBLOCK ? " (send buffer full)" : "");
    }
}
static void io_send(void *u, uint32_t ip, uint16_t port, const uint8_t *buf, int len){ (void)u; do_send(ip, port, buf, len, 0); }

int roam_bcast_targets(const roam_if *ifs, int n, uint32_t *out, int max){
    int k = 0;
    if (k < max) out[k++] = 0xFFFFFFFFu;
    for (int i = 0; i < n && k < max; i++){
        const roam_if *f = &ifs[i];
        if (!f->up || f->loopback || !f->can_broadcast || !f->mask || f->mask == 0xFFFFFFFFu) continue;
        if (!rc_addr_private(f->addr)) continue;            /* only private LANs */
        uint32_t b = (f->addr & f->mask) | ~f->mask;
        int dup = 0; for (int j = 0; j < k; j++) if (out[j] == b) dup = 1;
        if (!dup) out[k++] = b;
    }
    return k;
}
static void io_broadcast(void *u, uint16_t port, const uint8_t *buf, int len){
    (void)u;
    if (g_online){ if (g_cfg.discovery) do_send_online(0xFFFFFFFFu, port, buf, len, 1); return; }   /* every browser on the switch */
    if (g_cfg.test_mode || !g_cfg.discovery){ roam_plat_guard(1, 0xFFFFFFFFu, 1); return; }
    roam_if ifs[32]; int n = 0;
    INTERFACE_INFO il[32]; DWORD got = 0;
    if (WSAIoctl(g_fd, SIO_GET_INTERFACE_LIST, 0, 0, il, sizeof il, &got, 0, 0) == 0){
        int m = (int)(got / sizeof il[0]);
        for (int i = 0; i < m && n < 32; i++){
            if (il[i].iiAddress.Address.sa_family != AF_INET) continue;
            roam_if *f = &ifs[n++];
            memset(f, 0, sizeof *f);
            snprintf(f->name, sizeof f->name, "if%d", i);
            f->addr = be32(il[i].iiAddress.AddressIn.sin_addr.s_addr);
            f->mask = be32(il[i].iiNetmask.AddressIn.sin_addr.s_addr);
            f->up = !!(il[i].iiFlags & IFF_UP);
            f->loopback = !!(il[i].iiFlags & IFF_LOOPBACK);
            f->can_broadcast = !!(il[i].iiFlags & IFF_BROADCAST);
        }
    }
    uint32_t t[16]; int k = roam_bcast_targets(ifs, n, t, 16);
    for (int i = 0; i < k; i++) do_send(t[i], port, buf, len, 1);
}
static void io_event(void *u, int ev, int peer, const char *text){ (void)u; roam_game_event(ev, peer, text); }

/* ---- receiving and the timers (network thread, or the frame hook) ---------------------- */
static void online_start_switch(void);
static void pump(void){
    uint8_t buf[1500];
    int64_t now = roam_plat_now_us();
    if (g_online && !g_sw) online_start_switch();
    for (int k = 0; k < 256 && !g_online; k++){
        struct sockaddr_in sa; int sl = sizeof sa;
        int n = recvfrom(g_fd, (char *)buf, sizeof buf, 0, (struct sockaddr *)&sa, &sl);
        if (n < 0){
            int e = WSAGetLastError();
            if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;  /* an ICMP for an earlier datagram; an oversized one */
            break;                                                   /* WSAEWOULDBLOCK: nothing more */
        }
        if (sa.sin_family != AF_INET) continue;
        uint32_t ip = be32(sa.sin_addr.s_addr); uint16_t port = be16(sa.sin_port);
        roam_plat_lock();
        g_st.recv++; g_st.recv_bytes += (uint64_t)n;
        if (n > RC_MAX_DGRAM){ g_s->rx++; g_s->rx_bad++; }
        else rc_receive(g_s, ip, port, buf, n, now);
        roam_plat_unlock();
    }
    if (now >= g_next_tick){ roam_plat_lock(); rc_tick(g_s, now); roam_plat_unlock(); g_next_tick = now + 10000; }
}
static DWORD WINAPI net_main(LPVOID u){
    (void)u;
    uint32_t mx = roam_win_mxcsr_default();            /* the core's floats: round to nearest, no FTZ/DAZ */
    __asm__ volatile ("ldmxcsr %0" :: "m"(mx));
    while (!g_stop){
        if (g_online) Sleep(5);                          /* the switch's reader thread feeds the datagrams */
        else {
            fd_set rs; FD_ZERO(&rs); FD_SET(g_fd, &rs);
            struct timeval tv = { 0, 5000 };
            (void)select(0, &rs, 0, 0, &tv);
        }
        pump();
    }
    return 0;
}
/* NetThread=0: called by the frame hook instead */
void roam_win_poll(void){ if ((g_fd != INVALID_SOCKET || g_online) && !g_threaded && g_s) pump(); }

/* ---- online: the switch client's callbacks (its reader thread) ---------------------------------- */
static void sw_log(void *u, const char *line){ (void)u; roam_log("[roam] switch: %s\n", line); }
static void sw_welcome(void *u, uint32_t vip, uint32_t mask, const char *token){
    (void)u; (void)token;
    char b[16];
    InterlockedExchange(&g_mask, (LONG)mask); InterlockedExchange(&g_vip, (LONG)vip);
    roam_log("[roam] on the server's virtual LAN as %s\n", roam_ip_str(vip, b));
    if (g_vs_on) vs_link_up(vip, mask);
}
static void sw_down(void *u){ (void)u; if (g_vs_on) vs_link_down(); }
/* a peer delta's "gone": those players' TCP connections to us are reset (the page's NET_LAN_TCP_GONE) */
static void sw_peers(void *u, const char *j, int n){
    (void)u;
    if (!g_vs_on) return;
    for (int i = 0; i + 8 < n; i++){
        if (j[i] != '"' || j[i + 1] != 'g' || j[i + 2] != 'o' || j[i + 3] != 'n' || j[i + 4] != 'e' || j[i + 5] != '"') continue;
        int p = i + 6; while (p < n && (j[p] == ':' || j[p] == ' ')) p++;
        if (p >= n || j[p] != '[') return;
        for (p++; p < n && j[p] != ']'; ){
            uint32_t v = 0; int nd = 0;
            while (p < n && j[p] >= '0' && j[p] <= '9'){ v = v * 10u + (uint32_t)(j[p++] - '0'); nd++; }
            if (nd) vs_peer_gone(v);
            while (p < n && j[p] != ']' && (j[p] < '0' || j[p] > '9')) p++;
        }
        return;
    }
}
static void sw_frame(void *u, const uint8_t *f, int len){
    (void)u;
    if (len >= 16 && f[0] >= SW_KIND_LAN_UDP && f[0] <= SW_KIND_LAN_ICMP){ if (g_vs_on) vs_rx_frame(f, len); return; }
    uint32_t mx = roam_win_mxcsr_default();            /* the core's floats, on this thread too */
    __asm__ volatile ("ldmxcsr %0" :: "m"(mx));
    if (len < 16 || f[0] != SW_KIND_UDP || !g_s) return;
    uint32_t src = (uint32_t)f[4] << 24 | (uint32_t)f[5] << 16 | (uint32_t)f[6] << 8 | f[7];
    uint32_t dst = (uint32_t)f[8] << 24 | (uint32_t)f[9] << 16 | (uint32_t)f[10] << 8 | f[11];
    uint16_t sport = (uint16_t)(f[12] << 8 | f[13]), dport = (uint16_t)(f[14] << 8 | f[15]);
    uint32_t vip = (uint32_t)g_vip;
    int n = len - 16;
    if (dport != g_cfg.port || !vip || src == vip || (dst != vip && dst != SW_BROADCAST)) return;
    int64_t now = roam_plat_now_us();
    roam_plat_lock();
    g_st.recv++; g_st.recv_bytes += (uint64_t)n;
    if (n > RC_MAX_DGRAM){ g_s->rx++; g_s->rx_bad++; }
    else rc_receive(g_s, src, sport, f + 16, n, now);
    roam_plat_unlock();
}
static void sw_removed(void *u, int ban, const char *msg){
    (void)u;
    roam_log("[roam] the server's operator %s this player%s%s%s: no more online play this session\n", ban ? "banned" : "kicked",
             *msg ? " (\"" : "", msg, *msg ? "\")" : "");
}
static void sw_queue(void *u, int pos, int of){ (void)u; roam_log("[roam] the server is full: in line, %d of %d\n", pos, of); }
/* free roam (its thread) or the game's Winsock (a WSAStartup, force: now, whatever the name) */
static void online_start_switch_(int force){
    char name[40]; int i = 0;
    if (g_s){ roam_plat_lock(); for (; g_s->conf.name[i] && i < 39; i++) name[i] = g_s->conf.name[i]; name[i] = 0; roam_plat_unlock(); }
    else { const char *n = roam_opt("NFSU2_ROAM_NAME"); for (; n && n[i] && i < 24; i++) name[i] = n[i]; name[i] = 0; if (!i) lstrcpyA(name, "Player"); }
    int named = lstrcmpA(name, "Player") != 0;
    int64_t now = roam_plat_now_us();
    if (!g_sw_start_at) g_sw_start_at = now + 45000000;
    if (!force && !named && now < g_sw_start_at) return;   /* the profile's name comes with the first frames */
    sw_options o = { g_server, name, g_claim_set ? (g_claim[0] ? g_claim : 0) : roam_opt("NFSU2_ROAM_CLAIM"), (int)g_lan_on, 10000 };
    sw_callbacks cb = { sw_welcome, sw_frame, sw_peers, sw_queue, sw_removed, 0, 0, sw_log, 0, sw_down };
    sw *n = sw_start(&o, &cb);
    if (n && g_lan_on) sw_set_lan(n, 1, (uint32_t)g_lan_udp);   /* its UDP ports in the first hello too */
    g_sw = n;
    roam_log("[roam] joining the server's switch %s as \"%s\"%s%s\n", g_server, name, force ? " (for the game's network)" : "",
             g_sw ? "" : " FAILED (bad URL?)");
    if (!g_sw){ g_online = 0; g_vs_on = 0; }
}
static void online_start_switch_f(int force){
    if (g_sw || !g_server[0]) return;
    if (InterlockedCompareExchange(&g_sw_busy, 1, 0) != 0) return;   /* the other thread is at it */
    if (!g_sw) online_start_switch_(force);
    InterlockedExchange(&g_sw_busy, 0);
}
static void online_start_switch(void){ online_start_switch_f(0); }

/* ---- the game's Winsock on the switch (vsock.h): its transport ------------------------------------- */
/* (g_sw_users: the vsock writer thread is using the switch; the server switch below waits for it before it stops one) */
static volatile LONG g_sw_users;
static int vt_send(void *u, const uint8_t *f, int len){
    (void)u;
    InterlockedIncrement(&g_sw_users);
    sw *w = g_sw; int r = w && sw_send_frame(w, f, len);
    InterlockedDecrement(&g_sw_users);
    return r;
}
static void vt_lan(void *u, int on, uint32_t udp){
    (void)u;
    InterlockedExchange(&g_lan_on, on); InterlockedExchange(&g_lan_udp, (LONG)udp);
    InterlockedIncrement(&g_sw_users);
    sw *w = g_sw; if (w) sw_set_lan(w, on, udp);
    InterlockedDecrement(&g_sw_users);
}
static void vt_want(void *u){ (void)u; online_start_switch_f(1); }
static void vt_log(void *u, const char *line){ (void)u; roam_log("[lan] %s\n", line); }
static const vs_transport g_vt = { vt_send, vt_lan, vt_want, vt_log, 0 };
/* at load (roam_asi.c): with Server = (and Online = 1, the default), the game's own network goes to the server's
 * virtual LAN; 1 = the caller patches the game's Winsock imports */
int roam_win_online_init(void){
    const char *srv = roam_opt("NFSU2_ROAM_SERVER"), *on = roam_opt("NFSU2_ROAM_ONLINE");
    if (!srv || !*srv) return 0;
    if (on && on[0] == '0' && !on[1]){ roam_log("[lan] Online = 0: the game's own network (LAN and Online menus) stays the real one\n"); return 0; }
    if (roam_plat_test_mode()){ roam_log("[lan] test harness: the game's own network is not redirected\n"); return 0; }
    int i = 0; for (; srv[i] && i < (int)sizeof g_server - 1; i++) g_server[i] = srv[i];
    g_server[i] = 0;
    if (vs_init(&g_vt) != 0) return 0;
    g_vs_on = 1;
    return 1;
}

int roam_plat_start(const roam_plat_cfg *cfg, rc_session *s){
    g_cfg = *cfg; g_s = s;
    memset(&g_st, 0, sizeof g_st);
    mu_init();
    if (g_cfg.test_mode && g_cfg.discovery){
        roam_log("[roam] LAN discovery DISABLED: automated run (test harness), no broadcast will be sent; "
                 "only loopback peers\n");
        g_cfg.discovery = 0;
    }
    if (g_cfg.impair && *g_cfg.impair) roam_log("[roam] NFSU2_ROAM_IMPAIR is a Mac test aid: ignored here\n");
    const char *srv = roam_opt("NFSU2_ROAM_SERVER");
    if (srv && !g_cfg.test_mode){
        int i = 0; for (; srv[i] && i < (int)sizeof g_server - 1; i++) g_server[i] = srv[i];
        g_server[i] = 0;
        g_online = 1; g_threaded = 1; g_stop = 0; g_next_tick = 0; g_sw_start_at = 0;
        roam_plat_lock();
        s->io = (rc_io){ 0, io_send, io_broadcast, io_event };
        s->conf.hub_ip = SW_VIP_ROAM_HUB; s->conf.hub_port = g_cfg.port;       /* the players near us, from the server */
        s->conf.snap_states = 1;                           /* the WebSocket loses nothing: only the newest state */
        roam_plat_unlock();
        g_thr = CreateThread(0, 64 * 1024, net_main, 0, 0, 0);
        if (!g_thr){ roam_log("[roam] CreateThread failed (%lu): online play needs its thread\n", GetLastError()); g_online = 0; return -1; }
        roam_log("[roam] online: the server %s (virtual LAN UDP port %u), the players near us from its free-roam hub\n", g_server, g_cfg.port);
        return 0;
    }
    if (!g_wsa){
        WSADATA wd;
        int e = WSAStartup(MAKEWORD(2, 2), &wd);
        if (e){ roam_log("[roam] WSAStartup failed: %d\n", e); return -1; }
        g_wsa = 1;
    }
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET){ roam_log("[roam] socket: Winsock error %d\n", WSAGetLastError()); return -1; }
    u_long nb = 1; ioctlsocket(fd, FIONBIO, &nb);
    BOOL no = FALSE; DWORD got = 0;
    WSAIoctl(fd, SIO_UDP_CONNRESET, &no, sizeof no, 0, 0, &got, 0, 0);
    BOOL excl = TRUE; setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
    if (g_cfg.discovery){ BOOL one = TRUE; setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one); }
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = be16(g_cfg.port);
    sa.sin_addr.s_addr = be32(g_cfg.test_mode ? 0x7F000001u : 0u);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0){
        roam_log("[roam] cannot use UDP port %u (Winsock error %d): another program has it? Set the same other "
                 "Port in NFSU2FreeRoam.ini on every machine\n", g_cfg.port, WSAGetLastError());
        closesocket(fd); return -1;
    }
    g_fd = fd;
    rc_io io = { 0, io_send, io_broadcast, io_event };
    roam_plat_lock(); s->io = io; roam_plat_unlock();
    g_stop = 0; g_next_tick = 0;
    const char *nt = roam_opt("NFSU2_ROAM_NETTHREAD");
    g_threaded = !(nt && nt[0] == '0' && !nt[1]);
    if (g_threaded){
        g_thr = CreateThread(0, 64 * 1024, net_main, 0, 0, 0);
        if (!g_thr){ roam_log("[roam] CreateThread failed (%lu): polling from the game's frames instead\n", GetLastError()); g_threaded = 0; }
    }
    roam_log("[roam] UDP %s:%u open%s%s\n", g_cfg.test_mode ? "127.0.0.1" : "0.0.0.0", g_cfg.port,
             g_cfg.test_mode ? " (test harness: loopback only)" : g_cfg.discovery ? ", LAN discovery on" : "",
             g_threaded ? "" : ", polled from the frame hook");
    return 0;
}
/* exiting: whether the network thread may still hold the lock (at process
 * exit Windows has already stopped every other thread) */
void roam_plat_stop(uint8_t reason){
    if (g_online){
        extern volatile int roam_win_exiting;
        if (roam_win_exiting){                    /* process exit: every thread of ours and WinHTTP's is gone; the server
                                                   * drops this player after a few seconds without us */
            g_stop = 1; g_sw = 0; g_online = 0; g_vs_on = 0;
            return;
        }
        mu_init();
        if (TryEnterCriticalSection(&g_mu)){ rc_shutdown(g_s, reason); LeaveCriticalSection(&g_mu); }   /* BYE to the hub and peers */
        g_stop = 1;
        if (g_thr){ WaitForSingleObject(g_thr, 200); CloseHandle(g_thr); g_thr = 0; }
        if (g_sw && !(g_vs_on && vs_session_active())){ sw_stop(g_sw); g_sw = 0; }   /* the game's network may still need it */
        g_online = 0;
        return;
    }
    if (g_fd == INVALID_SOCKET) return;
    mu_init();
    if (TryEnterCriticalSection(&g_mu)){ rc_shutdown(g_s, reason); LeaveCriticalSection(&g_mu); }
    g_stop = 1;
    if (g_thr){ WaitForSingleObject(g_thr, 200); CloseHandle(g_thr); g_thr = 0; }
    closesocket(g_fd); g_fd = INVALID_SOCKET;
}

/* after a fault inside the frame step: the game thread may hold the
 * session lock (any depth); let it go, or the network thread waits forever */
void roam_plat_fault_unlock(void){
    if (g_mu_init != 2) return;
    for (int i = 0; i < 64 && g_mu.OwningThread == (HANDLE)(uintptr_t)GetCurrentThreadId(); i++) LeaveCriticalSection(&g_mu);
}
/* 1 when nobody else holds the session lock (process exit: then nobody can) */
int roam_plat_quiesced(void){
    if (g_mu_init != 2) return 1;
    if (!TryEnterCriticalSection(&g_mu)) return 0;
    LeaveCriticalSection(&g_mu);
    return 1;
}

/* "a.b.c.d[:port], ..." without sscanf; IPv4 literals only (no DNS) */
static int num(const char **p, unsigned *v, unsigned max){
    const char *q = *p; unsigned x = 0; int nd = 0;
    while (*q >= '0' && *q <= '9' && nd < 6){ x = x * 10u + (unsigned)(*q - '0'); q++; nd++; }
    if (!nd || x > max) return 0;
    *v = x; *p = q; return 1;
}
int roam_plat_parse_peers(const char *list, int test_mode, uint16_t defport, uint32_t *ip, uint16_t *port, int max){
    int n = 0;
    if (!list) return 0;
    const char *p = list;
    while (*p && n < max){
        while (*p == ' ' || *p == ',' || *p == ';' || *p == '\t') p++;
        if (!*p) break;
        char tok[64]; int k = 0;
        while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t' && k < 63) tok[k++] = *p++;
        tok[k] = 0;
        const char *t = tok; unsigned a, b, c, d, pt = defport;
        int good = num(&t, &a, 255) && *t++ == '.' && num(&t, &b, 255) && *t++ == '.' && num(&t, &c, 255) && *t++ == '.' &&
                   num(&t, &d, 255);
        if (good && *t == ':'){ t++; good = num(&t, &pt, 65535) && pt != 0; }
        if (!good || *t){
            roam_log("[roam] peer \"%s\" ignored: write a.b.c.d or a.b.c.d:port (IPv4 numbers, no names)\n", tok);
            continue;
        }
        uint32_t v = a << 24 | b << 16 | c << 8 | d;
        if (test_mode && a != 127){ roam_log("[roam] peer %s REFUSED: test harness, only 127.x.x.x peers\n", tok); continue; }
        ip[n] = v; port[n] = (uint16_t)pt; n++;
    }
    return n;
}

/* ---- the server switch: another server at run time (ServerKey, F8 by default) ----------------------------------
 * Servers = name=url[|claim token], name=url ... in the .ini (',' or ';' between them): the key goes on to the next one (after the last, the
 * first), from the server in use (Server =, or the last one switched to).  Only between servers (the plugin started
 * with Server =), only in career free roam with no race lobby or race trip (roam_game_switch_block) and no network
 * session of the game's own open (vs_session_active: its LAN or Online menus): a swap drops every virtual connection.
 * On its own thread (sw_stop waits for the switch's threads): the old switch stopped (its "down" tells vsock, whose
 * remote connections end), the new one started without a resume token (a token is one server's), with the entry's
 * claim token (none: none); free roam's session drops its old peers by their silence and its HELLO reaches the new
 * server's hub.  The choice is not written to the .ini. */
void roam_game_note(const char *t, int ms);          /* runtime/roam_game.c: the line at the top of free roam's HUD */
const char *roam_game_switch_block(void);
#define SERVERS_MAX 8
typedef struct { char name[24], url[200], claim[100]; } server_entry;
static server_entry g_srv[SERVERS_MAX];
static int          g_nsrv = -1;
static volatile LONG g_swapping;
static int64_t      g_swap_at;                   /* when the last swap began (its welcome is reported) */
static char         g_swap_name[24];
static int scmp(const char *a, const char *b){ return lstrcmpiA(a, b); }
static void servers_parse(void){
    g_nsrv = 0;
    const char *s = roam_opt("NFSU2_ROAM_SERVERS");
    while (s && *s && g_nsrv < SERVERS_MAX){
        while (*s == ' ' || *s == ';' || *s == ',' || *s == '\t') s++;
        if (!*s) break;
        server_entry *e = &g_srv[g_nsrv]; memset(e, 0, sizeof *e);
        char tok[340]; int k = 0;
        while (*s && *s != ';' && *s != ',' && k < (int)sizeof tok - 1) tok[k++] = *s++;
        while (k && (tok[k - 1] == ' ' || tok[k - 1] == '\t')) k--;
        tok[k] = 0;
        const char *t = tok, *eq = 0, *bar = 0;
        for (const char *p = tok; *p; p++){ if (*p == '=' && !eq) eq = p; if (*p == '|' && !bar) bar = p; }
        if (eq && (!bar || eq < bar) && eq - t > 0 && eq - t < (int)sizeof e->name){
            for (int i = 0; i < eq - t; i++) e->name[i] = t[i];
            t = eq + 1;
        }
        int i = 0;
        for (; *t && t != bar && i < (int)sizeof e->url - 1; t++) if (*t != ' ') e->url[i++] = *t;
        e->url[i] = 0;
        if (bar){ i = 0; for (t = bar + 1; *t && i < (int)sizeof e->claim - 1; t++) if (*t != ' ') e->claim[i++] = *t; e->claim[i] = 0; }
        int ws = (e->url[0] == 'w' && e->url[1] == 's' && e->url[2] == ':') || (e->url[0] == 'w' && e->url[1] == 's' && e->url[2] == 's' && e->url[3] == ':');
        if (!ws){ roam_log("[roam] Servers: \"%s\" ignored: write name=ws://host:port/lan or name=wss://host/lan\n", tok); continue; }
        if (!e->name[0]) snprintf(e->name, sizeof e->name, "server %d", g_nsrv + 1);
        roam_log("[roam] Servers: %d \"%s\" %s%s\n", g_nsrv + 1, e->name, e->url, e->claim[0] ? " (with a claim token)" : "");
        g_nsrv++;
    }
}
int roam_win_servers(void){ if (g_nsrv < 0) servers_parse(); return g_nsrv; }
static DWORD WINAPI swap_main(LPVOID p){
    server_entry *e = (server_entry *)p;
    uint32_t mx = roam_win_mxcsr_default(); __asm__ volatile ("ldmxcsr %0" :: "m"(mx));
    sw *old;
    roam_plat_lock(); old = g_sw; g_sw = 0; roam_plat_unlock();        /* free roam's sends stop here (do_send_online) */
    for (int i = 0; i < 2000 && g_sw_users; i++) Sleep(1);             /* vsock's writer is done with it */
    InterlockedExchange(&g_vip, 0);
    if (old) sw_stop(old);                                             /* (its reader's "down" tells vsock: sw_down) */
    if (g_s){ roam_plat_lock(); int n = rc_new_server(g_s); roam_plat_unlock(); roam_log("[roam] server switch: %d player(s) of the old server dropped\n", n); }
    if (g_vs_on) vs_link_down();
    int i = 0; for (; e->url[i] && i < (int)sizeof g_server - 1; i++) g_server[i] = e->url[i];
    g_server[i] = 0;
    i = 0; for (; e->claim[i] && i < (int)sizeof g_claim - 1; i++) g_claim[i] = e->claim[i];
    g_claim[i] = 0; g_claim_set = 1;
    roam_log("[roam] server switch: left the old server; joining \"%s\" %s\n", e->name, g_server);
    online_start_switch_(1);
    if (!g_sw){ char t[120]; snprintf(t, sizeof t, "Server %s: could not start (see the log)", e->name); roam_game_note(t, 6000); }
    InterlockedExchange(&g_sw_busy, 0);
    InterlockedExchange(&g_swapping, 0);
    return 0;
}
/* the hotkey (the game's thread, from the frame hook) */
void roam_win_server_next(void){
    char t[320];
    if (roam_win_servers() < 1){ roam_game_note("No other servers: add Servers = name=url, ... to NFSU2FreeRoam.ini", 6000); return; }
    if (!g_online || !g_server[0]){ roam_game_note("The server switch needs Server = in NFSU2FreeRoam.ini at start", 6000); return; }
    if (g_swapping){ roam_game_note("Still switching servers", 3000); return; }
    const char *why = roam_game_switch_block();
    if (!why && g_vs_on && vs_session_active()) why = "Leave the game's LAN or Online menus first";
    if (why){ snprintf(t, sizeof t, "%s: the server stays %s", why, g_server); roam_game_note(t, 5000); return; }
    int cur = -1;
    for (int i = 0; i < g_nsrv; i++) if (!scmp(g_srv[i].url, g_server)) cur = i;
    int next = (cur + 1) % g_nsrv;
    if (cur == next){ snprintf(t, sizeof t, "Only one server in Servers =: still on %s", g_srv[cur].name); roam_game_note(t, 5000); return; }
    if (InterlockedCompareExchange(&g_sw_busy, 1, 0) != 0){ roam_game_note("Still joining the server: try again", 3000); return; }
    InterlockedExchange(&g_swapping, 1);
    g_swap_at = roam_plat_now_us();
    snprintf(g_swap_name, sizeof g_swap_name, "%s", g_srv[next].name);
    snprintf(t, sizeof t, "Server: %s (%s): connecting", g_srv[next].name, g_srv[next].url);
    roam_game_note(t, 15000);
    HANDLE h = CreateThread(0, 64 * 1024, swap_main, &g_srv[next], 0, 0);
    if (!h){ InterlockedExchange(&g_sw_busy, 0); InterlockedExchange(&g_swapping, 0); roam_game_note("The server switch could not start a thread", 5000); return; }
    CloseHandle(h);
}
/* each frame: the new server's welcome, said on the top line */
void roam_win_server_frame(void){
    if (!g_swap_at || g_swapping) return;
    uint32_t vip = (uint32_t)g_vip;
    int64_t now = roam_plat_now_us();
    char t[120], b[16];
    if (vip){ snprintf(t, sizeof t, "Server: %s: connected as %s", g_swap_name, roam_ip_str(vip, b)); roam_game_note(t, 6000); g_swap_at = 0; }
    else if (now - g_swap_at > 15000000){ snprintf(t, sizeof t, "Server: %s: no answer yet (still trying; the key again for the next)", g_swap_name); roam_game_note(t, 8000); g_swap_at = 0; }
}

/* ---- race lobbies (runtime/roam_game.c mp_*): no page here ------------------------------------------------------
 * roam_plat_lobby 0: the lobby is on free roam's top line (mp_line); a notice for the page goes there too; the lobby
 * actions are the plugin's hotkeys' (roam_game_join_key), one queue (the game's thread only) */
int roam_plat_lobby(const char *json){ (void)json; return 0; }
void roam_plat_notice(const char *name, const char *what){
    char t[200];
    if (name && *name) snprintf(t, sizeof t, "%s: %s", name, what ? what : ""); else snprintf(t, sizeof t, "%s", what ? what : "");
    roam_game_note(t, 6000);
}
static struct { int act; uint32_t a, b, r; } g_lbq[8];
static int g_lbn;
void roam_win_lobby_push(int act, uint32_t a, uint32_t b, uint32_t r){
    if (g_lbn >= 8) return;
    g_lbq[g_lbn].act = act; g_lbq[g_lbn].a = a; g_lbq[g_lbn].b = b; g_lbq[g_lbn].r = r; g_lbn++;
}
int roam_plat_lobby_act(uint32_t *host, uint32_t *lobby, uint32_t *race){
    if (!g_lbn) return ROAM_LB_NONE;
    int act = g_lbq[0].act; *host = g_lbq[0].a; *lobby = g_lbq[0].b; *race = g_lbq[0].r;
    for (int i = 1; i < g_lbn; i++) g_lbq[i - 1] = g_lbq[i];
    g_lbn--;
    return act;
}
