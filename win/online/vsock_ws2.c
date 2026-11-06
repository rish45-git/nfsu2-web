/* vsock_ws2.c -- the Winsock calls SPEED2.EXE imports, on the virtual LAN (vsock.h): the port of runtime/winsock.c's
 * "vlan" code paths (the browser's), with the real stdcall signatures instead of the emulator's stack arguments.
 *
 * Every call answers as Winsock 2 on Windows would (structure layouts, error codes, the select/connect/close semantics
 * EA DirtySock relies on): a non-blocking TCP connect never completes inside the call and its failure is reported
 * asynchronously (exceptfds, SO_ERROR), as Windows does (the game's lobby code hangs on a synchronous failure); a reset
 * connection is writable and getpeername then fails with WSAENOTCONN, which is how DirtySock notices a drop; select
 * reads every set before it writes any (the same fd_set may be passed twice).
 *
 * Identity: the virtual address the switch gave (vsock.c ident_from_link), host name "VLAN-<host number>", adapter
 * 02:4E:56:4C:x:y (Netbios NCBASTAT: the lobby's MID).  Names: pcnfs05.ea.com is the server's Online host 10.77.0.1,
 * the own name and dotted quads resolve, every other name is WSAHOST_NOT_FOUND (nothing leaves for the real network).
 *
 * Threads: g_api (the emulator's guest lock) is held through every call; a call that waits (a blocking
 * recv/accept/connect/send, select with a timeout, WSAStartup for the link) lets it go while it waits, so the game's
 * other threads go on; closesocket() from another thread wakes it (WSAEINTR) and the last waiter frees the socket, so
 * its number cannot be reused under it.  Errors go through SetLastError: WSAGetLastError is GetLastError on Windows. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdarg.h>
#include "vsock_int.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
size_t strlen(const char *);
int strcasecmp(const char *, const char *);
int strncasecmp(const char *, const char *, size_t);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

NetIdent g_ws_id;
const vs_transport *g_vs_tr;
static CRITICAL_SECTION g_api;
static volatile LONG g_api_init;
static void api_init(void){
    if (InterlockedCompareExchange(&g_api_init, 1, 0) == 0){ InitializeCriticalSection(&g_api); InterlockedExchange(&g_api_init, 2); }
    while (g_api_init != 2) Sleep(0);
}
void guest_wait_begin(void){ LeaveCriticalSection(&g_api); }
void guest_wait_end(void){ EnterCriticalSection(&g_api); }

static uint64_t st_sockets, st_sent, st_recv, st_bytes_out, st_bytes_in, st_select, st_waits, st_dns_refused;

/* ---- what the game calls (diagnostics: vs_calls_tick, vs_calls_report) ----
 * Per function: calls, calls while no network session is open (before the first WSAStartup or after the last
 * WSACleanup: they fail at once with WSANOTINITIALISED, as on Windows), the time spent in them, the longest one, and
 * the first few threads that called (id and priority: a DirtySock thread runs at a high one).  Lock-free (32-bit
 * interlocked counters), reset at each report. */
enum { F_ACCEPT, F_BIND, F_CLOSESOCKET, F_CONNECT, F_GETPEERNAME, F_GETSOCKNAME, F_GETSOCKOPT, F_IOCTLSOCKET, F_LISTEN,
       F_RECV, F_RECVFROM, F_SELECT, F_SEND, F_SENDTO, F_SETSOCKOPT, F_SHUTDOWN, F_SOCKET, F_GETHOSTBYNAME,
       F_GETHOSTNAME, F_WSAGETLASTERROR, F_WSASETLASTERROR, F_WSASTARTUP, F_WSACLEANUP, F_WSAIOCTL, F_NETBIOS, F_N };
static const char *const k_fn[F_N] = {
    "accept", "bind", "closesocket", "connect", "getpeername", "getsockname", "getsockopt", "ioctlsocket", "listen",
    "recv", "recvfrom", "select", "send", "sendto", "setsockopt", "shutdown", "socket", "gethostbyname",
    "gethostname", "WSAGetLastError", "WSASetLastError", "WSAStartup", "WSACleanup", "WSAIoctl", "Netbios" };
static int g_startups;                    /* WSAStartup reference count (below, with the socket table) */
#define CALL_TIDS 4
static struct {
    volatile LONG calls, nosess, us, max_us;
    volatile LONG tid[CALL_TIDS], prio[CALL_TIDS];
} C[F_N];
static volatile LONG g_calls_nosess;                  /* since the last report, all functions */
static uint64_t g_calls_since;                        /* the start of the period calls_report covers */
static void call_in(int f){
    if (!g_calls_since) g_calls_since = ws_now_us();
    InterlockedIncrement(&C[f].calls);
    if (!g_startups && f != F_WSASTARTUP && f != F_NETBIOS){ InterlockedIncrement(&C[f].nosess); InterlockedIncrement(&g_calls_nosess); }
    LONG tid = (LONG)GetCurrentThreadId();
    for (int k = 0; k < CALL_TIDS; k++){
        LONG t = C[f].tid[k];
        if (t == tid) break;
        if (!t && InterlockedCompareExchange(&C[f].tid[k], tid, 0) == 0){
            InterlockedExchange(&C[f].prio[k], (LONG)GetThreadPriority(GetCurrentThread()));
            break;
        }
    }
}
static void call_out(int f, uint64_t t0){
    uint64_t d = ws_now_us() - t0;
    LONG us = d > 0x7FFFFFFFull ? 0x7FFFFFFF : (LONG)d;
    LONG old = InterlockedExchangeAdd(&C[f].us, us);
    if ((LONG)(old + us) < old) InterlockedExchange(&C[f].us, 0x7FFFFFFF);   /* saturate */
    for (LONG m = C[f].max_us; us > m; m = C[f].max_us)
        if (InterlockedCompareExchange(&C[f].max_us, us, m) == m) break;
}
/* one report of the calls since the last one (reset), over several lines if long; nothing if there were none */
static void calls_report(const char *why){
    uint64_t now = ws_now_us();
    double secs = g_calls_since ? (double)(now - g_calls_since) / 1e6 : 0.0;
    g_calls_since = now;
    char line[1000]; int k = 0, lines = 0, any = 0;
    for (int f = 0; f < F_N; f++){
        LONG n = InterlockedExchange(&C[f].calls, 0);
        LONG ns = InterlockedExchange(&C[f].nosess, 0), us = InterlockedExchange(&C[f].us, 0), mx = InterlockedExchange(&C[f].max_us, 0);
        char th[120]; int tk = 0;
        for (int t = 0; t < CALL_TIDS; t++){
            LONG id = InterlockedExchange(&C[f].tid[t], 0), pr = C[f].prio[t];
            if (id) tk += snprintf(th + tk, sizeof th - (size_t)tk, "%s%lx/%ld", tk ? " " : "", (unsigned long)id, (long)pr);
        }
        th[tk] = 0;
        if (!n) continue;
        char e[220], nsb[40] = "";
        if (ns) snprintf(nsb, sizeof nsb, " (%ld no session)", (long)ns);
        int el = snprintf(e, sizeof e, "%s %ld%s %.1f ms (max %.2f) [%s]", k_fn[f], (long)n, nsb, us / 1000.0, mx / 1000.0, th);
        if (!any){ k = snprintf(line, sizeof line, "calls in %.0f s%s%s: ", secs, why ? ", " : "", why ? why : ""); any = 1; }
        if (k + el + 2 >= (int)sizeof line){ ws_log("%s", line); lines++; k = snprintf(line, sizeof line, "  calls, more: "); }
        k += snprintf(line + k, sizeof line - (size_t)k, "%s%s", k > 0 && line[k - 2] != ':' ? "; " : "", e);
    }
    if (any) ws_log("%s", line);
    InterlockedExchange(&g_calls_nosess, 0);
    (void)lines;
}
/* the writer thread, every pass: a report every 60 s while the game calls, every 10 s while it calls with no
 * session open (the case being looked for: a thread that spins on calls that fail at once) */
void vs_calls_tick(void){
    uint64_t now = ws_now_us();
    if (!g_calls_since){ g_calls_since = now; return; }
    uint64_t el = now - g_calls_since;
    int closed = !g_startups && g_calls_nosess;           /* calls now, with the session over (not before one) */
    if (el >= 60000000ull || (el >= 10000000ull && closed)) calls_report(closed ? "with no session open" : 0);
}
void vs_calls_report(const char *why){ calls_report(why); }
void vs_exit_report(void){ if (g_vs_tr) calls_report("at the game's exit"); }

/* ---- small helpers ---- */
uint64_t ws_now_us(void){
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart && (!QueryPerformanceFrequency(&f) || !f.QuadPart)) f.QuadPart = 1000;
    QueryPerformanceCounter(&c);
    uint64_t q = (uint64_t)c.QuadPart / (uint64_t)f.QuadPart, r = (uint64_t)c.QuadPart % (uint64_t)f.QuadPart;
    return q * 1000000ull + r * 1000000ull / (uint64_t)f.QuadPart;
}
void ws_log(const char *fmt, ...){
    char b[1200]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (g_vs_tr && g_vs_tr->log) g_vs_tr->log(g_vs_tr->user, b);
}
static const char *ipstr(uint32_t ip, char *b){      /* ip: network order */
    const uint8_t *q = (const uint8_t *)&ip;
    snprintf(b, 16, "%u.%u.%u.%u", q[0], q[1], q[2], q[3]); return b;
}
static uint32_t mkip(int a, int b, int c, int d){
    uint32_t v; uint8_t *q = (uint8_t *)&v; q[0] = (uint8_t)a; q[1] = (uint8_t)b; q[2] = (uint8_t)c; q[3] = (uint8_t)d;
    return v;
}
static int is_loop(uint32_t ip){ return ((const uint8_t *)&ip)[0] == 127; }
/* a.b.c.d (and, as inet_addr, a / a.b / a.b.c with the last part filling the rest), network order */
static int parse_ip(const char *s, uint32_t *out){
    uint32_t part[4]; int n = 0;
    if (!s || !*s) return 0;
    for (;;){
        if (n == 4) return 0;
        uint64_t v = 0; int nd = 0;
        while (*s >= '0' && *s <= '9'){ v = v * 10 + (uint64_t)(*s++ - '0'); if (v > 0xFFFFFFFFull) return 0; nd++; }
        if (!nd) return 0;
        part[n++] = (uint32_t)v;
        if (*s == '.'){ s++; continue; }
        if (*s) return 0;
        break;
    }
    uint32_t h;
    for (int i = 0; i < n - 1; i++) if (part[i] > 255) return 0;
    if (n == 1) h = part[0];
    else if (n == 2){ if (part[1] > 0xFFFFFFu) return 0; h = part[0] << 24 | part[1]; }
    else if (n == 3){ if (part[2] > 0xFFFFu) return 0; h = part[0] << 24 | part[1] << 16 | part[2]; }
    else { if (part[3] > 255) return 0; h = part[0] << 24 | part[1] << 16 | part[2] << 8 | part[3]; }
    *out = __builtin_bswap32(h); return 1;
}
static void set_err(int e){ SetLastError((DWORD)e); }
#define FAIL(e) do { set_err(e); return SOCKET_ERROR; } while (0)

/* ---- the online host (vsock.h) ---- */
#define ONLINE_HOST_VA 0x7FBDBCu                  /* SPEED2.EXE v1.2 NA: LobbyConnect's host, 0x588630 */
#define ONLINE_HOST    "pcnfs05.ea.com"
int vs_patch_lobby_host(void){
    static const char secure[16] = "*" ONLINE_HOST, plain[16] = ONLINE_HOST;
    uint8_t *p = (uint8_t *)(uintptr_t)ONLINE_HOST_VA;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
    int same = 1, sec = 1;
    for (int i = 0; i < 16; i++){ same &= p[i] == (uint8_t)plain[i]; sec &= p[i] == (uint8_t)secure[i]; }
    if (same) return 1;
    if (!sec){
        ws_log("online: the lobby host at 0x%08x is not \"*%s\" (another game version?): online stays off", ONLINE_HOST_VA, ONLINE_HOST);
        return 0;
    }
    DWORD old;
    if (!VirtualProtect(p, 16, PAGE_READWRITE, &old)){ ws_log("online: VirtualProtect failed (%lu)", GetLastError()); return 0; }
    memcpy(p, plain, 16);
    VirtualProtect(p, 16, old, &old);
    ws_log("online: the lobby link is plain TCP to %s:20920 (the '*' asking for SSL 2.0 is gone, in memory), "
           "which is the server's own host 10.77.0.1", ONLINE_HOST);
    return 1;
}

/* ======================================================================= */
/* the SOCKET table                                                         */
/* ======================================================================= */
#define NSOCK 1024
#define SOCK_BASE 0x1000u
static WsSock g_s[NSOCK];
static int g_s_next = 1;
static int g_startups;                    /* WSAStartup reference count */
static int g_vlan_up;                     /* the first session's hello done */

static WsSock *sget(SOCKET h){
    if (h < SOCK_BASE + 4u || (h - SOCK_BASE) & 3u) return 0;
    uint32_t i = (uint32_t)(h - SOCK_BASE) >> 2;
    if (i >= NSOCK || !g_s[i].used || g_s[i].closing) return 0;
    return &g_s[i];
}
static SOCKET shandle(WsSock *s){ return (SOCKET)(SOCK_BASE + 4u * (uint32_t)(s - g_s)); }
static WsSock *salloc(void){
    for (int k = 0; k < NSOCK - 1; k++){
        int i = g_s_next + k; if (i >= NSOCK) i -= NSOCK - 1;
        WsSock *s = &g_s[i];
        if (s->used) continue;
        memset(s, 0, sizeof *s);
        s->used = 1; s->fd = -1; s->ip_ttl = 128; s->sndbuf = s->rcvbuf = 65536;
        g_s_next = i + 1 >= NSOCK ? 1 : i + 1;          /* round robin: no immediate handle reuse */
        st_sockets++;
        return s;
    }
    return 0;
}
static void sfinal(WsSock *s){                         /* last reference gone */
    if (s->fd >= 0) vfd_close(s->fd);
    s->fd = -1; s->used = 0; s->closing = 0;
}
static void sclose(WsSock *s, int abortive){
    vlan_close(s, abortive);
    if (s->waiters){
        s->closing = 1;
        if (s->fd >= 0) vfd_shutdown(s->fd, 2);         /* wakes the waiters */
    } else sfinal(s);
}

/* 0 = ready, 1 = deadline passed, 2 = the socket was closed meanwhile */
static int swait(WsSock *s, short ev, uint64_t deadline){
    int fd = s->fd, rc = 1;
    s->waiters++; st_waits++;
    guest_wait_begin();
    for (;;){
        if (s->closing){ rc = 2; break; }
        uint64_t now = ws_now_us();
        if (deadline && now >= deadline){ rc = 1; break; }
        int ms = 50;
        if (deadline && (deadline - now) / 1000 < 50) ms = (int)((deadline - now + 999) / 1000);
        if (fd < 0){ Sleep((DWORD)ms); continue; }
        vpollfd p = { fd, ev, 0 };
        int n = vfd_poll(&p, 1, ms);
        if (n > 0){ rc = s->closing ? 2 : 0; break; }
    }
    guest_wait_end();
    s->waiters--;
    if (s->closing){ if (!s->waiters) sfinal(s); return 2; }
    return rc;
}
static uint64_t deadline_ms(uint32_t ms){ return ms ? ws_now_us() + (uint64_t)ms * 1000ull : 0; }

/* SOCKADDR_IN: u16 family, u16 port (big endian), u32 address (network order), 8 bytes sin_zero (ignored on input:
 * DirtySock keeps a TTL and a receive time stamp there) */
static int ga_in(const struct sockaddr *p, int len, uint32_t *ip, uint16_t *port){
    const uint8_t *b = (const uint8_t *)p;
    if (!p || len < 16) return VE_FAULT;
    if ((b[0] | b[1] << 8) != 2) return VE_AFNOSUPPORT;
    *port = (uint16_t)(b[2] << 8 | b[3]);
    memcpy(ip, b + 4, 4);
    return 0;
}
static int ga_out(struct sockaddr *p, int *plen, uint32_t ip, uint16_t port){
    if (!p) return 0;
    if (!plen || *plen < 16) return VE_FAULT;
    uint8_t *b = (uint8_t *)p;
    b[0] = 2; b[1] = 0; b[2] = (uint8_t)(port >> 8); b[3] = (uint8_t)port;
    memcpy(b + 4, &ip, 4); memset(b + 8, 0, 8);
    *plen = 16;
    return 0;
}
static int is_bcast(uint32_t ip){
    if (ip == 0xFFFFFFFFu) return 1;
    if (g_ws_id.mask && g_ws_id.ip && ip == (g_ws_id.ip | ~g_ws_id.mask)) return 1;
    return 0;
}

/* ======================================================================= */
/* the calls                                                                */
/* ======================================================================= */
static int need_startup(void){ if (!g_startups){ set_err(VE_NOTINITIALISED); return 1; } return 0; }
int winsock_session_active(void){ return g_startups > 0; }
int vs_session_active(void){ return g_startups > 0; }

static int vlan_up(void){
    if (g_vlan_up) return vlan_alive() ? 0 : VE_NETDOWN;
    int e = vlan_hello(&g_ws_id);
    if (e) return e;
    g_vlan_up = 1;
    char b[16];
    ws_log("WSAStartup: the virtual LAN, address %s, host name %s, adapter %02x:%02x:%02x:%02x:%02x:%02x",
           ipstr(g_ws_id.ip, b), g_ws_id.name, g_ws_id.mac[0], g_ws_id.mac[1], g_ws_id.mac[2],
           g_ws_id.mac[3], g_ws_id.mac[4], g_ws_id.mac[5]);
    return 0;
}

static int ws_startup(WORD req, LPWSADATA p){
    uint8_t maj = (uint8_t)req, min = (uint8_t)(req >> 8);
    if (!maj) return WSAVERNOTSUPPORTED;
    if (!p) return WSAEFAULT;
    if (!g_startups){ int e = vlan_up(); if (e) return WSASYSNOTREADY; }   /* a session starts: the link */
    uint16_t ver = (maj > 2 || (maj == 2 && min >= 2)) ? 0x0202 : (uint16_t)req;
    uint8_t *b = (uint8_t *)p;
    memset(b, 0, sizeof *p);
    b[0] = (uint8_t)ver; b[1] = (uint8_t)(ver >> 8); b[2] = 2; b[3] = 2;
    memcpy(b + 4, "WinSock 2.0", 12);
    memcpy(b + 4 + 257, "Running", 8);
    g_startups++;
    if (g_startups == 1){ static int n; if (n++ < 8) ws_log("WSAStartup(%u.%u) -> %u.%u: a network session", maj, min, ver & 0xFF, ver >> 8); }
    return 0;
}
static int ws_cleanup(void){
    if (need_startup()) return SOCKET_ERROR;
    if (--g_startups == 0){
        int n = 0;
        for (int i = 1; i < NSOCK; i++) if (g_s[i].used && !g_s[i].closing){ sclose(&g_s[i], 0); n++; }
        ws_log("WSACleanup: the network session ends%s", n ? "" : "");
        if (n) ws_log("WSACleanup closed %d socket(s)", n);
        if (g_vlan_up) vlan_session_end();
    }
    return 0;
}

static SOCKET ws_socket(int af, int type, int proto){
    if (need_startup()) return INVALID_SOCKET;
    if (af != 2){ set_err(VE_AFNOSUPPORT); return INVALID_SOCKET; }
    if (type == 1){ if (proto && proto != 6){ set_err(VE_PROTONOSUPPORT); return INVALID_SOCKET; } }
    else if (type == 2){ if (proto && proto != 17){ set_err(VE_PROTONOSUPPORT); return INVALID_SOCKET; } }
    else if (type == 3){ if (proto != 1){ set_err(VE_PROTONOSUPPORT); return INVALID_SOCKET; } }   /* raw: ICMP only (ProtoPing) */
    else { set_err(VE_SOCKTNOSUPPORT); return INVALID_SOCKET; }
    WsSock *s = salloc();
    if (!s){ set_err(VE_MFILE); return INVALID_SOCKET; }
    s->type = type; s->proto = type == 1 ? 6 : type == 2 ? 17 : 1;
    int e = vlan_open(s);
    if (e){ s->used = 0; set_err(e); return INVALID_SOCKET; }
    return shandle(s);
}

static int ws_closesocket(SOCKET h){
    if (need_startup()) return SOCKET_ERROR;
    WsSock *s = sget(h); if (!s) FAIL(VE_NOTSOCK);
    sclose(s, s->linger_on && !s->linger_s);
    return 0;
}

static int implicit_bind(WsSock *s){
    if (s->bound) return 0;
    int e = vlan_bind(s, 0, 0);
    if (!e){ s->bound = 1; if (s->state == ST_NEW) s->state = ST_BOUND; }
    return e;
}

static int ws_bind(SOCKET h, const struct sockaddr *a, int al){
    if (need_startup()) return SOCKET_ERROR;
    WsSock *s = sget(h); if (!s) FAIL(VE_NOTSOCK);
    uint32_t ip; uint16_t port;
    int e = ga_in(a, al, &ip, &port); if (e) FAIL(e);
    if (s->bound) FAIL(VE_INVAL);
    e = vlan_bind(s, ip, port);
    if (e) FAIL(e);
    s->bound = 1; s->laddr = ip;
    if (s->state == ST_NEW) s->state = ST_BOUND;
    return 0;
}

static int ws_listen(SOCKET h, int bl){
    if (need_startup()) return SOCKET_ERROR;
    WsSock *s = sget(h); if (!s) FAIL(VE_NOTSOCK);
    if (s->type != 1) FAIL(VE_OPNOTSUPP);
    if (!s->bound) FAIL(VE_INVAL);
    if (s->state == ST_CONNECTED || s->state == ST_CONNECTING) FAIL(VE_ISCONN);
    if (s->state == ST_LISTEN) return 0;
    if (bl <= 0) bl = 1;
    if (bl > 128) bl = 128;
    int e = vlan_listen(s, bl);
    if (e) FAIL(e);
    s->state = ST_LISTEN;
    return 0;
}

static SOCKET ws_accept(SOCKET h, struct sockaddr *pa, int *pl){
    if (need_startup()) return INVALID_SOCKET;
    WsSock *ls = sget(h); if (!ls){ set_err(VE_NOTSOCK); return INVALID_SOCKET; }
    if (ls->state != ST_LISTEN){ set_err(VE_INVAL); return INVALID_SOCKET; }
    if (pa && (!pl || *pl < 16)){ set_err(VE_FAULT); return INVALID_SOCKET; }
    for (;;){
        WsSock *ns = salloc(); if (!ns){ set_err(VE_MFILE); return INVALID_SOCKET; }
        ns->type = 1; ns->proto = 6;
        int e = vlan_accept(ls, ns);
        if (!e){
            ns->state = ST_CONNECTED; ns->bound = 1; ns->nb = ls->nb;      /* Windows: properties of the listener */
            ns->so_keepalive = ls->so_keepalive; ns->so_oobinline = ls->so_oobinline;
            if (pa) ga_out(pa, pl, ns->raddr, ns->rport);
            return shandle(ns);
        }
        ns->used = 0;
        if (e != VE_WOULDBLOCK || ls->nb){ set_err(e); return INVALID_SOCKET; }
        int w = swait(ls, VP_IN, 0);
        ls = sget(h);
        if (w == 2 || !ls){ set_err(VE_INTR); return INVALID_SOCKET; }
    }
}

/* non-blocking TCP connect completion */
static void conn_poll(WsSock *s){
    if (s->state != ST_CONNECTING) return;
    vlan_conn_update(s);         /* the other end's answer, if it came */
    if (ws_now_us() < s->conn_ready_us) return;
    if (s->conn_result){ s->state = ST_FAILED; s->pending_err = s->conn_result; }
    else s->state = ST_CONNECTED;
}

static int ws_connect(SOCKET h, const struct sockaddr *a, int al){
    if (need_startup()) return SOCKET_ERROR;
    WsSock *s = sget(h); if (!s) FAIL(VE_NOTSOCK);
    uint32_t ip; uint16_t port;
    int e = ga_in(a, al, &ip, &port); if (e) FAIL(e);
    if (s->type != 1){
        /* UDP (and raw): a default destination.  No packet. */
        if (!ip && !port){ s->has_peer = 0; return 0; }
        e = implicit_bind(s); if (e) FAIL(e);
        s->has_peer = 1; s->raddr = ip; s->rport = port; s->udp_reset = 0;
        return 0;
    }
    if (s->state == ST_CONNECTING) FAIL(VE_ALREADY);
    if (s->state == ST_CONNECTED) FAIL(VE_ISCONN);
    if (s->state == ST_LISTEN) FAIL(VE_INVAL);
    if (!ip || ip == 0xFFFFFFFFu || !port) FAIL(VE_ADDRNOTAVAIL);
    s->raddr = ip; s->rport = port;
    e = vlan_connect(s, ip, port);          /* result known now or later, reported at conn_ready_us */
    if (e && e != VE_CONNREFUSED && e != VE_TIMEDOUT && e != VE_HOSTUNREACH) FAIL(e);
    s->bound = 1; s->state = ST_CONNECTING; s->conn_result = e;
    if (s->nb) FAIL(VE_WOULDBLOCK);         /* Windows: a non-blocking TCP connect never completes inside the call */
    for (;;){
        s = sget(h); if (!s) FAIL(VE_INTR);
        if (s->state == ST_CONNECTING){
            uint64_t now = ws_now_us();
            /* events 0: the stream fd is "ready" once the answer came (vsock.c revents_of) */
            if (now < s->conn_ready_us){ int w = swait(s, 0, s->conn_ready_us); if (w == 2) FAIL(VE_INTR); }
            s = sget(h); if (!s) FAIL(VE_INTR);
            conn_poll(s);
            continue;
        }
        if (s->state == ST_FAILED){ int r = s->conn_result; s->state = ST_BOUND; s->pending_err = 0; FAIL(r); }
        return 0;
    }
}

/* ---- data ---- */
static int send_one(WsSock *s, const uint8_t *buf, int len, uint32_t ip, uint16_t port, int has_addr, int *sent){
    *sent = 0;
    if (s->type == 1){
        if (s->state != ST_CONNECTED) return VE_NOTCONN;
        if (s->shut_wr) return VE_SHUTDOWN;
        int n = vfd_send(s->fd, buf, len);
        if (n < 0){
            if (n == VE_PIPE) return VE_CONNRESET;     /* reset by the other end, or our own abortive close */
            return -n;
        }
        *sent = n; return 0;
    }
    if (!has_addr){
        if (!s->has_peer) return VE_NOTCONN;
        ip = s->raddr; port = s->rport;
    }
    if (s->shut_wr) return VE_SHUTDOWN;
    if (s->type == 2 && len > 65507) return VE_MSGSIZE;
    if (s->type == 2 && is_bcast(ip) && !s->so_broadcast) return VE_ACCES;
    int e = implicit_bind(s); if (e) return e;
    return vlan_sendto(s, buf, len, ip, port, sent);
}

static int do_send(SOCKET h, const char *buf, int len, int fl, const struct sockaddr *to, int tol, int has_addr){
    if (need_startup()) return SOCKET_ERROR;
    WsSock *s = sget(h); if (!s) FAIL(VE_NOTSOCK);
    if (len < 0 || (len && !buf)) FAIL(VE_FAULT);
    uint32_t ip = 0; uint16_t port = 0;
    if (has_addr && s->type != 1 && to){
        int e = ga_in(to, tol, &ip, &port); if (e) FAIL(e);
    } else if (has_addr && s->type != 1) has_addr = 0;  /* sendto without an address: the connected peer */
    if (fl & ~0x5){ static int said; if (!said++) ws_log("send flags 0x%x ignored", fl); }
    int total = 0;
    uint64_t dl = deadline_ms(s->sndtimeo);
    for (;;){
        int sent = 0;
        int e = send_one(s, (const uint8_t *)buf + total, len - total, ip, port, has_addr && s->type != 1, &sent);
        if (!e){
            total += sent; st_sent++; st_bytes_out += (uint64_t)sent;
            if (s->type != 1 || total >= len || s->nb) return total;
            continue;                            /* blocking stream send: everything */
        }
        if (e != VE_WOULDBLOCK || s->nb){ if (total) return total; FAIL(e); }
        int w = swait(s, VP_OUT, dl);
        s = sget(h);
        if (w == 2 || !s) FAIL(VE_INTR);
        if (w == 1){ if (total) return total; FAIL(VE_TIMEDOUT); }
    }
}

static int recv_one(WsSock *s, uint8_t *buf, int len, int peek, uint32_t *ip, uint16_t *port, int *got){
    *got = 0;
    if (s->type == 1){
        if (s->state != ST_CONNECTED) return VE_NOTCONN;
        if (s->shut_rd) return VE_SHUTDOWN;
        int n = vfd_recv(s->fd, buf, len, peek);
        if (n < 0) return n == VE_PIPE ? VE_CONNRESET : -n;
        if (n == 0 && len > 0 && vlan_conn_was_reset(s)) return VE_CONNRESET;
        *got = n; *ip = s->raddr; *port = s->rport;
        return 0;
    }
    if (!s->bound) return VE_INVAL;
    if (s->shut_rd) return VE_SHUTDOWN;
    if (s->udp_reset && s->has_peer){ s->udp_reset = 0; return VE_CONNRESET; }
    return vlan_recvfrom(s, buf, len, peek, ip, port, got);
}

static int do_recv(SOCKET h, char *buf, int len, int fl, struct sockaddr *pf, int *pfl, int has_from){
    if (need_startup()) return SOCKET_ERROR;
    WsSock *s = sget(h); if (!s) FAIL(VE_NOTSOCK);
    if (len < 0 || (len && !buf)) FAIL(VE_FAULT);
    if (!has_from){ pf = 0; pfl = 0; }
    if (pf && (!pfl || *pfl < 16)) FAIL(VE_FAULT);
    if (fl & ~0x3){ static int said; if (!said++) ws_log("recv flags 0x%x ignored", fl); }
    if ((fl & 1) && s->type != 1) FAIL(VE_OPNOTSUPP);
    uint64_t dl = deadline_ms(s->rcvtimeo);
    for (;;){
        uint32_t ip = 0; uint16_t port = 0; int got = 0;
        int e = recv_one(s, (uint8_t *)buf, len, (fl & 2) != 0, &ip, &port, &got);
        if (!e || e == VE_MSGSIZE){
            st_recv++; st_bytes_in += (uint64_t)got;
            if (pf && s->type != 1) ga_out(pf, pfl, ip, port);
            if (e) FAIL(e);
            return got;
        }
        if (e != VE_WOULDBLOCK || s->nb) FAIL(e);
        int w = swait(s, VP_IN, dl);
        s = sget(h);
        if (w == 2 || !s) FAIL(VE_INTR);
        if (w == 1) FAIL(VE_TIMEDOUT);
    }
}

static int ws_shutdown(SOCKET h, int how){
    if (need_startup()) return SOCKET_ERROR;
    WsSock *s = sget(h); if (!s) FAIL(VE_NOTSOCK);
    if (how < 0 || how > 2) FAIL(VE_INVAL);
    if (s->type == 1){
        if (s->state != ST_CONNECTED) FAIL(VE_NOTCONN);
        vfd_shutdown(s->fd, how);
    }
    if (how != 1) s->shut_rd = 1;
    if (how != 0) s->shut_wr = 1;
    return 0;
}

static int ws_getname(SOCKET h, struct sockaddr *p, int *pl, int peer){
    if (need_startup()) return SOCKET_ERROR;
    WsSock *s = sget(h); if (!s) FAIL(VE_NOTSOCK);
    if (!p || !pl || *pl < 16) FAIL(VE_FAULT);
    uint32_t ip = 0; uint16_t port = 0;
    if (peer){
        if (s->type == 1){
            if (s->state == ST_CONNECTING) conn_poll(s);
            if (s->state != ST_CONNECTED) FAIL(VE_NOTCONN);
            if (vlan_conn_was_reset(s)) FAIL(VE_NOTCONN);          /* reset: no longer connected */
        } else if (!s->has_peer) FAIL(VE_NOTCONN);
        ip = s->raddr; port = s->rport;
    } else {
        if (!s->bound) FAIL(VE_INVAL);
        ip = s->laddr; port = s->lport;
    }
    ga_out(p, pl, ip, port);
    return 0;
}

/* ---- options ---- */
static int optint(const char *p, int l, int *v){
    if (!p || l < 1) return VE_FAULT;
    *v = l >= 4 ? *(const int *)p : (int)*(const uint8_t *)p;
    return 0;
}
static int ws_setsockopt(SOCKET h, int lvl, int opt, const char *p, int l){
    if (need_startup()) return SOCKET_ERROR;
    WsSock *s = sget(h); if (!s) FAIL(VE_NOTSOCK);
    int v = 0, e = 0;
    if (lvl == 0xFFFF){
        switch ((uint32_t)opt){
        case 0x0004: if ((e = optint(p, l, &v))) FAIL(e); s->so_reuse = v != 0; return 0;           /* SO_REUSEADDR */
        case 0xFFFFFFFBu: if ((e = optint(p, l, &v))) FAIL(e); return 0;                           /* SO_EXCLUSIVEADDRUSE */
        case 0x0020:                                                                                /* SO_BROADCAST */
            if ((e = optint(p, l, &v))) FAIL(e);
            if (s->type == 1) FAIL(VE_NOPROTOOPT);
            s->so_broadcast = v != 0; return 0;
        case 0x0008: if ((e = optint(p, l, &v))) FAIL(e); s->so_keepalive = v != 0; return 0;      /* SO_KEEPALIVE */
        case 0x0010: if ((e = optint(p, l, &v))) FAIL(e); s->so_dontroute = v != 0; return 0;      /* SO_DONTROUTE */
        case 0x0100: if ((e = optint(p, l, &v))) FAIL(e); s->so_oobinline = v != 0; return 0;      /* SO_OOBINLINE */
        case 0x0080:                                                  /* SO_LINGER: struct linger { u_short l_onoff, l_linger } */
            if (!p || l < 4) FAIL(VE_FAULT);
            if (s->type != 1) FAIL(VE_NOPROTOOPT);
            s->linger_on = *(const uint16_t *)p != 0; s->linger_s = *(const uint16_t *)(p + 2);
            return 0;
        case 0xFFFFFF7Fu:                                             /* SO_DONTLINGER */
            if ((e = optint(p, l, &v))) FAIL(e);
            if (s->type != 1) FAIL(VE_NOPROTOOPT);
            if (v) s->linger_on = 0;
            return 0;
        case 0x1001: case 0x1002:                                     /* SO_SNDBUF / SO_RCVBUF */
            if ((e = optint(p, l, &v))) FAIL(e);
            if (opt == 0x1001) s->sndbuf = v; else s->rcvbuf = v;
            return 0;
        case 0x1005: case 0x1006:                                     /* SO_SNDTIMEO / SO_RCVTIMEO: DWORD ms */
            if ((e = optint(p, l, &v))) FAIL(e);
            if (opt == 0x1005) s->sndtimeo = (uint32_t)v; else s->rcvtimeo = (uint32_t)v;
            return 0;
        default: break;
        }
    } else if (lvl == 0 /* IPPROTO_IP */){
        if (opt == 4){ if ((e = optint(p, l, &v))) FAIL(e); if (v < 0 || v > 255) FAIL(VE_INVAL); s->ip_ttl = v; return 0; }   /* IP_TTL */
        if (opt == 3){ if ((e = optint(p, l, &v))) FAIL(e); return 0; }                                                       /* IP_TOS */
    } else if (lvl == 6 && opt == 1){                                 /* TCP_NODELAY */
        if ((e = optint(p, l, &v))) FAIL(e);
        if (s->type != 1) FAIL(VE_NOPROTOOPT);
        return 0;
    }
    { static uint32_t seen[16]; static int ns; int k; uint32_t key = (uint32_t)lvl << 16 ^ (uint32_t)opt;
      for (k = 0; k < ns && seen[k] != key; k++) {}
      if (k == ns && ns < 16){ seen[ns++] = key; ws_log("setsockopt(level 0x%x, option 0x%x) not supported -> WSAENOPROTOOPT", lvl, opt); } }
    FAIL(lvl == 0xFFFF || lvl == 0 || lvl == 6 ? VE_NOPROTOOPT : VE_INVAL);
}

static int ws_getsockopt(SOCKET h, int lvl, int opt, char *p, int *pl){
    if (need_startup()) return SOCKET_ERROR;
    WsSock *s = sget(h); if (!s) FAIL(VE_NOTSOCK);
    if (!p || !pl || *pl < 1) FAIL(VE_FAULT);
    int cap = *pl;
    uint32_t v = 0; int bytes = 4;
    if (lvl == 0xFFFF){
        switch ((uint32_t)opt){
        case 0x0002: v = s->state == ST_LISTEN; break;              /* SO_ACCEPTCONN */
        case 0x0004: v = (uint32_t)s->so_reuse; break;
        case 0x0008: v = (uint32_t)s->so_keepalive; break;
        case 0x0010: v = (uint32_t)s->so_dontroute; break;
        case 0x0020: v = (uint32_t)s->so_broadcast; break;
        case 0x0100: v = (uint32_t)s->so_oobinline; break;
        case 0x0080:                                                  /* SO_LINGER (4 bytes) */
            if (cap < 4) FAIL(VE_FAULT);
            ((uint16_t *)p)[0] = (uint16_t)s->linger_on; ((uint16_t *)p)[1] = (uint16_t)s->linger_s; *pl = 4; return 0;
        case 0xFFFFFF7Fu: v = !s->linger_on; break;
        case 0x1001: v = (uint32_t)s->sndbuf; break;
        case 0x1002: v = (uint32_t)s->rcvbuf; break;
        case 0x1005: v = s->sndtimeo; break;
        case 0x1006: v = s->rcvtimeo; break;
        case 0x1007:                                                  /* SO_ERROR, then cleared */
            if (s->type == 1 && s->state == ST_CONNECTING) conn_poll(s);
            v = (uint32_t)s->pending_err; s->pending_err = 0;
            break;
        case 0x1008: v = (uint32_t)s->type; break;                   /* SO_TYPE */
        default: goto unk;
        }
    } else if (lvl == 0 && opt == 4) v = (uint32_t)s->ip_ttl;
    else if (lvl == 6 && opt == 1) v = 0;
    else goto unk;
    /* BOOL and int options: Windows accepts a 1-byte buffer for BOOLs */
    if (cap < 4) bytes = 1;
    if (bytes == 4) *(uint32_t *)p = v; else *(uint8_t *)p = (uint8_t)v;
    *pl = bytes;
    return 0;
unk:
    { static int said; if (said++ < 8) ws_log("getsockopt(level 0x%x, option 0x%x) not supported -> WSAENOPROTOOPT", lvl, opt); }
    FAIL(VE_NOPROTOOPT);
}

static int ws_ioctlsocket(SOCKET h, long cmd, u_long *p){
    if (need_startup()) return SOCKET_ERROR;
    WsSock *s = sget(h); if (!s) FAIL(VE_NOTSOCK);
    if (!p) FAIL(VE_FAULT);
    if ((uint32_t)cmd == 0x8004667Eu){ s->nb = *p != 0; return 0; }               /* FIONBIO */
    if ((uint32_t)cmd == 0x4004667Fu){ uint32_t v = 0; if (s->fd >= 0) vlan_fionread(s, &v); *p = v; return 0; }  /* FIONREAD */
    if ((uint32_t)cmd == 0x40047307u){ *p = 1; return 0; }                        /* SIOCATMARK: no OOB data */
    { static int said; if (said++ < 8) ws_log("ioctlsocket(0x%08lx) not supported -> WSAEINVAL", (unsigned long)cmd); }
    FAIL(VE_INVAL);
}

static int ws_wsaioctl(SOCKET h, DWORD code, LPVOID in, DWORD inl, LPVOID out, DWORD outl, LPDWORD pret, LPWSAOVERLAPPED ov){
    if (need_startup()) return SOCKET_ERROR;
    WsSock *s = sget(h); if (!s) FAIL(VE_NOTSOCK);
    if (ov){ static int said; if (!said++) ws_log("WSAIoctl with OVERLAPPED: completed synchronously"); }
    if (code == 0xC8000014u){                                   /* SIO_ROUTING_INTERFACE_QUERY */
        uint32_t ip; uint16_t port;
        int e = ga_in(in, (int)inl, &ip, &port); if (e) FAIL(e);
        if (!out || outl < 16u){ if (pret) *pret = 16; FAIL(VE_FAULT); }
        uint32_t loc = is_loop(ip) ? mkip(127, 0, 0, 1) : g_ws_id.ip;
        if (!loc) FAIL(VE_NETUNREACH);
        uint8_t *b = out; memset(b, 0, 16); b[0] = 2; memcpy(b + 4, &loc, 4);
        if (pret) *pret = 16;
        return 0;
    }
    if (code == 0x8004667Eu){                                    /* FIONBIO */
        if (!in || inl < 4) FAIL(VE_FAULT);
        s->nb = *(const uint32_t *)in != 0; if (pret) *pret = 0; return 0;
    }
    if (code == 0x4004667Fu){                                    /* FIONREAD */
        if (!out || outl < 4) FAIL(VE_FAULT);
        uint32_t v = 0; if (s->fd >= 0) vlan_fionread(s, &v); *(uint32_t *)out = v; if (pret) *pret = 4; return 0;
    }
    if (code == 0x9800000Cu){ if (pret) *pret = 0; return 0; }   /* SIO_UDP_CONNRESET */
    { static uint32_t seen[8]; static int ns; int k;
      for (k = 0; k < ns && seen[k] != code; k++) {}
      if (k == ns && ns < 8){ seen[ns++] = code; ws_log("WSAIoctl(0x%08lx) not supported -> WSAEOPNOTSUPP", (unsigned long)code); } }
    FAIL(VE_OPNOTSUPP);
}

/* ---- select ---- */
#define SEL_MAX 4096                    /* server.dll builds fd_sets of up to 0x1000 entries */
typedef struct { WsSock *s; int fd; short ev; } SelEnt;
static int ws_select(fd_set *rs, fd_set *wsf, fd_set *es, const struct timeval *tv){
    if (need_startup()) return SOCKET_ERROR;
    fd_set *set[3] = { rs, wsf, es };
    uint32_t cnt[3] = { 0, 0, 0 }, total = 0;
    for (int k = 0; k < 3; k++){
        if (!set[k]) continue;
        cnt[k] = set[k]->fd_count;
        if (cnt[k] > SEL_MAX) FAIL(VE_INVAL);
        total += cnt[k];
    }
    if (!total) FAIL(VE_INVAL);
    /* every set is read before any is written back: the same fd_set may be passed as write and except set */
    SOCKET *hs = vs_alloc(sizeof *hs * total);
    uint8_t *rdy = vs_alloc(total);
    int *map = vs_alloc(sizeof *map * total);
    SelEnt *pe = vs_alloc(sizeof *pe * total);
    vpollfd *pf = vs_alloc(sizeof *pf * total);
    int r = SOCKET_ERROR, err = 0;
    if (!hs || !rdy || !map || !pe || !pf){ err = VE_NOBUFS; goto out; }
    { uint32_t i = 0;
      for (int k = 0; k < 3; k++) for (uint32_t j = 0; j < cnt[k]; j++){
          hs[i] = set[k]->fd_array[j];
          if (!sget(hs[i])){ err = VE_NOTSOCK; goto out; }
          i++; } }
    uint64_t dl = 0; int forever = 1;
    if (tv){
        int32_t sec = tv->tv_sec, usec = tv->tv_usec;
        if (sec < 0) sec = 0;
        if (usec < 0) usec = 0;
        dl = ws_now_us() + (uint64_t)sec * 1000000ull + (uint64_t)usec; forever = 0;
    }
    st_select++;
    for (;;){
        int np = 0;
        { uint32_t i = 0;
          for (int k = 0; k < 3; k++) for (uint32_t j = 0; j < cnt[k]; j++, i++){
              WsSock *s = sget(hs[i]);
              if (!s){ err = VE_NOTSOCK; goto out; }
              int q; for (q = 0; q < np && pe[q].s != s; q++) {}
              if (q == np){ pe[np].s = s; pe[np].fd = s->fd; pe[np].ev = 0; np++; }
              pe[q].ev |= k == 0 ? VP_IN : k == 1 ? VP_OUT : 0;
              if (s->type == 1 && s->state == ST_CONNECTING) pe[q].ev |= VP_OUT;
              map[i] = q;
          } }
        for (int q = 0; q < np; q++){ pf[q].fd = pe[q].fd; pf[q].events = pe[q].fd >= 0 ? pe[q].ev : 0; pf[q].revents = 0; }
        vfd_poll(pf, np, 0);
        int ready = 0; uint64_t next_conn = 0;
        { uint32_t i = 0;
          for (int k = 0; k < 3; k++) for (uint32_t j = 0; j < cnt[k]; j++, i++){
              int q = map[i]; WsSock *s = pe[q].s;
              short rev = pf[q].fd >= 0 ? pf[q].revents : 0;
              int rd = 0, wr = 0, ex = 0;
              if (s->type == 1){
                  if (s->state == ST_CONNECTING){
                      conn_poll(s);
                      if (s->state == ST_CONNECTING && (!next_conn || s->conn_ready_us < next_conn)) next_conn = s->conn_ready_us;
                      if (s->state == ST_CONNECTED){ wr = 1; rev &= (short)~(VP_IN | VP_HUP); }
                  }
                  if (s->state == ST_LISTEN) rd = (rev & VP_IN) != 0;
                  else if (s->state == ST_CONNECTED){
                      rd = (rev & (VP_IN | VP_HUP | VP_ERR)) != 0;
                      /* a reset connection is writable on Windows (send fails at once instead of blocking): DirtySock
                       * notices a drop by "writable, then getpeername fails with WSAENOTCONN" */
                      wr = wr || (rev & (VP_OUT | VP_HUP | VP_ERR)) != 0;
                  } else if (s->state == ST_FAILED) ex = 1;   /* failed connect: exceptfds only */
              } else {
                  rd = (rev & (VP_IN | VP_ERR)) != 0 || (s->udp_reset && s->has_peer);
                  wr = pf[q].fd < 0 || (rev & VP_OUT) != 0;
              }
              rdy[i] = (uint8_t)(k == 0 ? rd : k == 1 ? wr : ex);
              ready += rdy[i];
          } }
        uint64_t now = ws_now_us();
        if (ready || (!forever && now >= dl)){
            uint32_t i = 0;
            for (int k = 0; k < 3; k++){
                if (!set[k]){ continue; }
                uint32_t n = 0;
                for (uint32_t j = 0; j < cnt[k]; j++, i++) if (rdy[i]) set[k]->fd_array[n++] = hs[i];
                set[k]->fd_count = n;
            }
            r = ready; goto out;
        }
        /* wait with g_api released, in slices of at most 50 ms */
        uint64_t until = forever ? now + 50000ull : dl;
        if (until > now + 50000ull) until = now + 50000ull;
        if (next_conn && next_conn < until) until = next_conn;
        int ms = (int)((until - now + 999) / 1000);
        if (ms < 1) ms = 1;
        for (int q = 0; q < np; q++){
            pe[q].s->waiters++;
            /* a pending connect is waited on with events 0: its stream reports ready once the answer (SYNACK or RST)
             * came (vsock.c revents_of), which ends this wait early.  Any other entry without events (a socket only
             * in exceptfds) is left out: events 0 would report a connected stream ready at once, a busy loop */
            if (pe[q].s->type == 1 && pe[q].s->state == ST_CONNECTING) pf[q].events = 0;
            else if (!pf[q].events) pf[q].fd = -1;
            pf[q].revents = 0;
        }
        guest_wait_begin();
        vfd_poll(pf, np, ms);
        guest_wait_end();
        int closed = 0;
        for (int q = 0; q < np; q++){
            pe[q].s->waiters--;
            if (pe[q].s->closing){ closed = 1; if (!pe[q].s->waiters) sfinal(pe[q].s); }
        }
        if (closed){ err = VE_INTR; goto out; }
    }
out:
    vs_free(hs); vs_free(rdy); vs_free(map); vs_free(pe); vs_free(pf);
    if (err) FAIL(err);
    return r;
}

/* ---- names ---- */
/* hostent per thread, as Winsock keeps it (no TLS in the plugin: a small table by thread id) */
#define GBUF_N 32
static struct { DWORD tid; struct hostent h; char *aliases[1]; char *list[9]; uint32_t addrs[8]; char name[256]; } g_gbuf[GBUF_N];
static struct hostent *make_hostent(const char *name, const uint32_t *ips, int n){
    DWORD tid = GetCurrentThreadId();
    int i, fr = -1;
    for (i = 0; i < GBUF_N; i++){ if (g_gbuf[i].tid == tid) break; if (!g_gbuf[i].tid && fr < 0) fr = i; }
    if (i == GBUF_N){ i = fr >= 0 ? fr : (int)(tid % GBUF_N); g_gbuf[i].tid = tid; }
    if (n > 8) n = 8;
    snprintf(g_gbuf[i].name, sizeof g_gbuf[i].name, "%s", name);
    g_gbuf[i].aliases[0] = 0;
    for (int k = 0; k < n; k++){ g_gbuf[i].addrs[k] = ips[k]; g_gbuf[i].list[k] = (char *)&g_gbuf[i].addrs[k]; }
    g_gbuf[i].list[n] = 0;
    struct hostent *h = &g_gbuf[i].h;
    h->h_name = g_gbuf[i].name; h->h_aliases = g_gbuf[i].aliases; h->h_addrtype = 2; h->h_length = 4; h->h_addr_list = g_gbuf[i].list;
    return h;
}
static int own_name(const char *n){
    if (!*n) return 1;
    size_t l = strlen(g_ws_id.name);
    if (l && !strncasecmp(n, g_ws_id.name, l) && (!n[l] || !strcasecmp(n + l, ".local") || !strcasecmp(n + l, ".lan"))) return 1;
    return 0;
}
static struct hostent *ws_gethostbyname(const char *p){
    if (need_startup()) return 0;
    char name[256] = "";
    if (p){ int k = 0; while (k < 255 && p[k]){ name[k] = p[k]; k++; } name[k] = 0; }
    uint32_t ips[8];
    if (own_name(name)){ ips[0] = g_ws_id.ip ? g_ws_id.ip : mkip(127, 0, 0, 1); return make_hostent(g_ws_id.name, ips, 1); }
    if (!strcasecmp(name, "localhost")){ ips[0] = mkip(127, 0, 0, 1); return make_hostent(g_ws_id.name, ips, 1); }
    if (parse_ip(name, &ips[0])) return make_hostent(name, ips, 1);
    if (!strcasecmp(name, ONLINE_HOST)){
        static int said;
        if (!said++) ws_log("online: %s is the server's online host 10.77.0.1", ONLINE_HOST);
        ips[0] = mkip(10, 77, 0, 1); return make_hostent(ONLINE_HOST, ips, 1);
    }
    st_dns_refused++;
    if (st_dns_refused <= 50) ws_log("dns \"%s\": not on the virtual LAN (WSAHOST_NOT_FOUND)", name);
    set_err(VE_HOST_NOT_FOUND); return 0;
}
static int ws_gethostname(char *p, int len){
    if (need_startup()) return SOCKET_ERROR;
    size_t l = strlen(g_ws_id.name);
    if (!p || len <= (int)l) FAIL(VE_FAULT);
    memcpy(p, g_ws_id.name, l + 1);
    return 0;
}

/* ======================================================================= */
/* netapi32!Netbios: NCBRESET / NCBASTAT / NCBENUM on one LANA              */
/* ======================================================================= */
static UCHAR ws_netbios(uint8_t *ncb){
    if (!ncb) return 0x01;                                /* NRC_BUFLEN */
    uint8_t cmd = ncb[0] & 0x7F, lana = ncb[0x30];
    uint8_t *buf = *(uint8_t **)(ncb + 4); uint16_t blen = *(uint16_t *)(ncb + 8);
    uint8_t rc = 0;
    if (!g_vlan_up && !g_startups) vlan_up();             /* the adapter's address is the virtual one */
    switch (cmd){
    case 0x32: rc = lana == 0 ? 0 : 0x23; break;          /* NCBRESET; NRC_BRIDGE for another LANA */
    case 0x33: {                                          /* NCBASTAT */
        if (lana != 0){ rc = 0x23; break; }
        if (ncb[0xA] != '*'){ rc = 0x05; break; }         /* remote adapter status: NRC_CMDTMO */
        uint8_t as[60]; memset(as, 0, sizeof as);
        memcpy(as, g_ws_id.mac, 6);
        as[6] = 3;  as[8] = 0xFE;                          /* rev_major 3, adapter_type Ethernet */
        uint16_t w[] = { 255, 255, 255, 0, 1464, 0, 64, 64, 1700, 0 };  /* free_ncbs .. name_count */
        for (int i = 0; i < 10; i++){ as[40 + 2 * i] = (uint8_t)w[i]; as[41 + 2 * i] = (uint8_t)(w[i] >> 8); }
        uint32_t n = blen < 60 ? blen : 60;
        if (n && !buf){ rc = 0x01; break; }
        if (n) memcpy(buf, as, n);
        *(uint16_t *)(ncb + 8) = (uint16_t)n;
        rc = n < 60 ? 0x06 : 0;                           /* NRC_INCOMP */
        break; }
    case 0x37:                                            /* NCBENUM: LANA_ENUM { length, lana[] } */
        if (blen < 2 || !buf){ rc = 0x01; break; }
        buf[0] = 1; buf[1] = 0; *(uint16_t *)(ncb + 8) = 2; break;
    default:
        { static int said; if (said++ < 8) ws_log("Netbios command 0x%02x not supported -> NRC_ILLCMD", cmd); }
        rc = 0x03; break;
    }
    if (ncb[0] & 0x80){ static int said; if (!said++) ws_log("Netbios ASYNCH: completed synchronously"); }
    ncb[1] = rc; ncb[0x31] = rc;
    return rc;
}

/* ======================================================================= */
/* the exported replacements                                                */
/* ======================================================================= */
#define W WSAAPI
/* each call: counted and timed (calls_report), then g_api held through it */
#define IN_(f)   int f_ = (f); uint64_t t0_ = ws_now_us(); call_in(f_); api_init(); EnterCriticalSection(&g_api)
#define OUT_()   LeaveCriticalSection(&g_api); call_out(f_, t0_)
static SOCKET W vs_accept(SOCKET s, struct sockaddr *a, int *l){ IN_(F_ACCEPT); SOCKET r = ws_accept(s, a, l); OUT_(); return r; }
static int W vs_bind(SOCKET s, const struct sockaddr *a, int l){ IN_(F_BIND); int r = ws_bind(s, a, l); OUT_(); return r; }
static int W vs_closesocket(SOCKET s){ IN_(F_CLOSESOCKET); int r = ws_closesocket(s); OUT_(); return r; }
static int W vs_connect(SOCKET s, const struct sockaddr *a, int l){ IN_(F_CONNECT); int r = ws_connect(s, a, l); OUT_(); return r; }
static int W vs_getpeername(SOCKET s, struct sockaddr *a, int *l){ IN_(F_GETPEERNAME); int r = ws_getname(s, a, l, 1); OUT_(); return r; }
static int W vs_getsockname(SOCKET s, struct sockaddr *a, int *l){ IN_(F_GETSOCKNAME); int r = ws_getname(s, a, l, 0); OUT_(); return r; }
static int W vs_getsockopt(SOCKET s, int lv, int o, char *p, int *l){ IN_(F_GETSOCKOPT); int r = ws_getsockopt(s, lv, o, p, l); OUT_(); return r; }
static int W vs_ioctlsocket(SOCKET s, long c, u_long *p){ IN_(F_IOCTLSOCKET); int r = ws_ioctlsocket(s, c, p); OUT_(); return r; }
static int W vs_listen(SOCKET s, int b){ IN_(F_LISTEN); int r = ws_listen(s, b); OUT_(); return r; }
static int W vs_recv(SOCKET s, char *b, int n, int f){ IN_(F_RECV); int r = do_recv(s, b, n, f, 0, 0, 0); OUT_(); return r; }
static int W vs_recvfrom(SOCKET s, char *b, int n, int f, struct sockaddr *a, int *l){ IN_(F_RECVFROM); int r = do_recv(s, b, n, f, a, l, 1); OUT_(); return r; }
static int W vs_select(int nfds, fd_set *r_, fd_set *w_, fd_set *e_, const struct timeval *t){ (void)nfds; IN_(F_SELECT); int r = ws_select(r_, w_, e_, t); OUT_(); return r; }
static int W vs_send(SOCKET s, const char *b, int n, int f){ IN_(F_SEND); int r = do_send(s, b, n, f, 0, 0, 0); OUT_(); return r; }
static int W vs_sendto(SOCKET s, const char *b, int n, int f, const struct sockaddr *a, int l){ IN_(F_SENDTO); int r = do_send(s, b, n, f, a, l, 1); OUT_(); return r; }
static int W vs_setsockopt(SOCKET s, int lv, int o, const char *p, int l){ IN_(F_SETSOCKOPT); int r = ws_setsockopt(s, lv, o, p, l); OUT_(); return r; }
static int W vs_shutdown(SOCKET s, int h){ IN_(F_SHUTDOWN); int r = ws_shutdown(s, h); OUT_(); return r; }
static SOCKET W vs_socket(int a, int t, int p){ IN_(F_SOCKET); SOCKET r = ws_socket(a, t, p); OUT_(); return r; }
static struct hostent *W vs_gethostbyname(const char *n){ IN_(F_GETHOSTBYNAME); struct hostent *r = ws_gethostbyname(n); OUT_(); return r; }
static int W vs_gethostname(char *n, int l){ IN_(F_GETHOSTNAME); int r = ws_gethostname(n, l); OUT_(); return r; }
/* the error code is the thread's own: these two take no lock (and must not change it: counting does not) */
static int W vs_WSAGetLastError(void){ DWORD e = GetLastError(); call_in(F_WSAGETLASTERROR); SetLastError(e); return (int)e; }
static void W vs_WSASetLastError(int e){ call_in(F_WSASETLASTERROR); SetLastError((DWORD)e); }
static int W vs_WSAStartup(WORD v, LPWSADATA d){
    IN_(F_WSASTARTUP); int r = ws_startup(v, d);
    if (!r && g_startups == 1) InterlockedExchange(&g_calls_nosess, 0);   /* a session: "no session" counts from its end */
    OUT_(); return r;
}
static int W vs_WSACleanup(void){
    IN_(F_WSACLEANUP); int r = ws_cleanup(); int ended = !r && !g_startups; OUT_();
    if (ended) calls_report("up to the last WSACleanup");     /* what came after it gets its own report */
    return r;
}
static int W vs_WSAIoctl(SOCKET s, DWORD c, LPVOID i, DWORD il, LPVOID o, DWORD ol, LPDWORD ret, LPWSAOVERLAPPED ov,
                         LPWSAOVERLAPPED_COMPLETION_ROUTINE cr){
    (void)cr; IN_(F_WSAIOCTL); int r = ws_wsaioctl(s, c, i, il, o, ol, ret, ov); OUT_(); return r; }
static UCHAR W vs_Netbios(void *ncb){ IN_(F_NETBIOS); UCHAR r = ws_netbios(ncb); OUT_(); return r; }

const vs_import vs_imports[] = {
    { "ws2_32.dll", 1, "accept", (void *)vs_accept },           { "ws2_32.dll", 2, "bind", (void *)vs_bind },
    { "ws2_32.dll", 3, "closesocket", (void *)vs_closesocket }, { "ws2_32.dll", 4, "connect", (void *)vs_connect },
    { "ws2_32.dll", 5, "getpeername", (void *)vs_getpeername }, { "ws2_32.dll", 6, "getsockname", (void *)vs_getsockname },
    { "ws2_32.dll", 7, "getsockopt", (void *)vs_getsockopt },   { "ws2_32.dll", 10, "ioctlsocket", (void *)vs_ioctlsocket },
    { "ws2_32.dll", 13, "listen", (void *)vs_listen },          { "ws2_32.dll", 16, "recv", (void *)vs_recv },
    { "ws2_32.dll", 17, "recvfrom", (void *)vs_recvfrom },      { "ws2_32.dll", 18, "select", (void *)vs_select },
    { "ws2_32.dll", 19, "send", (void *)vs_send },              { "ws2_32.dll", 20, "sendto", (void *)vs_sendto },
    { "ws2_32.dll", 21, "setsockopt", (void *)vs_setsockopt },  { "ws2_32.dll", 22, "shutdown", (void *)vs_shutdown },
    { "ws2_32.dll", 23, "socket", (void *)vs_socket },          { "ws2_32.dll", 52, "gethostbyname", (void *)vs_gethostbyname },
    { "ws2_32.dll", 57, "gethostname", (void *)vs_gethostname },{ "ws2_32.dll", 111, "WSAGetLastError", (void *)vs_WSAGetLastError },
    { "ws2_32.dll", 112, "WSASetLastError", (void *)vs_WSASetLastError },
    { "ws2_32.dll", 115, "WSAStartup", (void *)vs_WSAStartup }, { "ws2_32.dll", 116, "WSACleanup", (void *)vs_WSACleanup },
    { "ws2_32.dll", 0, "WSAIoctl", (void *)vs_WSAIoctl },
    { "netapi32.dll", 0, "Netbios", (void *)vs_Netbios },
    { 0, 0, 0, 0 } };
