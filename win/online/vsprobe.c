/* vsprobe.c -- the virtual Winsock (vsock.h) against a running server, without the game: the replacement functions
 * themselves (from vs_imports, as the plugin's IAT patch takes them) over the real switch client.
 *   out/win/vsprobe.exe [ws://127.0.0.1:8691/lan] [name]
 * Checks what the game's DirtySock code relies on: WSAStartup bringing the link up, the names, Netbios' adapter,
 * a non-blocking connect to the Online host reported through select, the redirector's @dir answer and its FIN, a
 * refused connect in exceptfds with SO_ERROR, a blocking connect to the lobby, getsockname = the virtual address,
 * UDP port unreachable -> WSAECONNRESET, ICMP echo with the IP header in front, a loopback listen/accept/stream,
 * the own broadcast looped back, closesocket waking a blocking recv on another thread, WSACleanup. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "vsock.h"
#include "switch.h"

static int (WSAAPI *pWSAStartup)(WORD, LPWSADATA);
static int (WSAAPI *pWSACleanup)(void);
static int (WSAAPI *pWSAGetLastError)(void);
static SOCKET (WSAAPI *psocket)(int, int, int);
static int (WSAAPI *pclosesocket)(SOCKET);
static int (WSAAPI *pconnect)(SOCKET, const struct sockaddr *, int);
static int (WSAAPI *pbind)(SOCKET, const struct sockaddr *, int);
static int (WSAAPI *plisten)(SOCKET, int);
static SOCKET (WSAAPI *paccept)(SOCKET, struct sockaddr *, int *);
static int (WSAAPI *psend)(SOCKET, const char *, int, int);
static int (WSAAPI *precv)(SOCKET, char *, int, int);
static int (WSAAPI *psendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);
static int (WSAAPI *precvfrom)(SOCKET, char *, int, int, struct sockaddr *, int *);
static int (WSAAPI *pselect)(int, fd_set *, fd_set *, fd_set *, const struct timeval *);
static int (WSAAPI *pioctlsocket)(SOCKET, long, u_long *);
static int (WSAAPI *psetsockopt)(SOCKET, int, int, const char *, int);
static int (WSAAPI *pgetsockopt)(SOCKET, int, int, char *, int *);
static int (WSAAPI *pgetpeername)(SOCKET, struct sockaddr *, int *);
static int (WSAAPI *pgetsockname)(SOCKET, struct sockaddr *, int *);
static struct hostent *(WSAAPI *pgethostbyname)(const char *);
static int (WSAAPI *pgethostname)(char *, int);
static int (WSAAPI *pWSAIoctl)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static UCHAR (WSAAPI *pNetbios)(void *);

static void *fn(const char *name){
    for (const vs_import *i = vs_imports; i->dll; i++) if (!strcmp(i->name, name)) return i->fn;
    printf("no replacement for %s\n", name); ExitProcess(2);
}

/* ---- the transport: the switch client ---- */
static sw *g_sw; static const char *g_url, *g_name;
static int tr_send(void *u, const uint8_t *f, int len){ (void)u; return g_sw && sw_send_frame(g_sw, f, len); }
static void tr_lan(void *u, int on, uint32_t udp){ (void)u; printf("  [lan state] on %d, udp ports 0x%08x\n", on, udp); if (g_sw) sw_set_lan(g_sw, on, udp); }
static void sw_welcome(void *u, uint32_t vip, uint32_t mask, const char *t){ (void)u; (void)t; vs_link_up(vip, mask); }
static void sw_frame(void *u, const uint8_t *f, int len){ (void)u; if (len >= 16 && f[0] >= 3 && f[0] <= 5) vs_rx_frame(f, len); }
static void sw_down(void *u){ (void)u; vs_link_down(); }
static void sw_peers(void *u, const char *j, int n){ (void)u; (void)j; (void)n; }
static void sw_logcb(void *u, const char *l){ (void)u; printf("  [switch] %s\n", l); }
static void tr_want(void *u){
    (void)u;
    if (g_sw) return;
    sw_options o = { g_url, g_name, 0, 0, 10000 };
    sw_callbacks cb = { sw_welcome, sw_frame, sw_peers, 0, 0, 0, 0, sw_logcb, 0, sw_down };
    g_sw = sw_start(&o, &cb);
    printf("  [want] the switch starts: %s\n", g_sw ? "ok" : "FAILED");
}
/* the diagnostics' lines the checks below wait for */
static volatile LONG g_saw_stall, g_saw_nosess, g_saw_cleanup_calls;
static void tr_log(void *u, const char *l){
    (void)u; printf("  [vsock] %s\n", l);
    if (strstr(l, "the game is not reading this connection") && strstr(l, ":20920")) InterlockedExchange(&g_saw_stall, 1);
    if (strstr(l, "with no session open") && strstr(l, "select ")) InterlockedExchange(&g_saw_nosess, 1);
    if (strstr(l, "up to the last WSACleanup") && strstr(l, "WSACleanup 1")) InterlockedExchange(&g_saw_cleanup_calls, 1);
}
static const vs_transport TR = { tr_send, tr_lan, tr_want, tr_log, 0 };

static int fails;
#define CHECK(what, cond) do { int ok_ = (cond); printf("%s  %s\n", ok_ ? "PASS" : "FAIL", what); if (!ok_) fails++; } while (0)

static struct sockaddr_in sin_(uint32_t ip_h, uint16_t port){
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(ip_h); return a;
}
#define SA(a) ((struct sockaddr *)&(a))
#define IP(a,b,c,d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
static int sel1(SOCKET s, int rd, int wr, int ex, int ms, int *r, int *w, int *e){
    fd_set R, Wt, E; FD_ZERO(&R); FD_ZERO(&Wt); FD_ZERO(&E);
    if (rd) FD_SET(s, &R);
    if (wr) FD_SET(s, &Wt);
    if (ex) FD_SET(s, &E);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    int n = pselect(0, rd ? &R : 0, wr ? &Wt : 0, ex ? &E : 0, &tv);
    *r = rd && R.fd_count; *w = wr && Wt.fd_count; *e = ex && E.fd_count;
    return n;
}
/* one Aries message: kind, code 0, length (big-endian, the header included), body */
static int aries(char *b, const char *kind, const char *body){
    int n = 12 + (int)strlen(body) + 1;
    memcpy(b, kind, 4); memset(b + 4, 0, 4);
    b[8] = (char)(n >> 24); b[9] = (char)(n >> 16); b[10] = (char)(n >> 8); b[11] = (char)n;
    memcpy(b + 12, body, strlen(body) + 1);
    return n;
}

static SOCKET g_blk; static volatile LONG g_blk_r, g_blk_e;
static DWORD WINAPI blocking_reader(LPVOID u){
    (void)u; char b[16];
    int r = precv(g_blk, b, sizeof b, 0);
    g_blk_e = pWSAGetLastError(); g_blk_r = r;
    return 0;
}

int main(int argc, char **argv){
    g_url = argc > 1 ? argv[1] : "ws://127.0.0.1:8691/lan";
    g_name = argc > 2 ? argv[2] : "VsProbe";
    setvbuf(stdout, 0, _IONBF, 0);
    pWSAStartup = fn("WSAStartup"); pWSACleanup = fn("WSACleanup"); pWSAGetLastError = fn("WSAGetLastError");
    psocket = fn("socket"); pclosesocket = fn("closesocket"); pconnect = fn("connect"); pbind = fn("bind");
    plisten = fn("listen"); paccept = fn("accept"); psend = fn("send"); precv = fn("recv"); psendto = fn("sendto");
    precvfrom = fn("recvfrom"); pselect = fn("select"); pioctlsocket = fn("ioctlsocket"); psetsockopt = fn("setsockopt");
    pgetsockopt = fn("getsockopt"); pgetpeername = fn("getpeername"); pgetsockname = fn("getsockname");
    pgethostbyname = fn("gethostbyname"); pgethostname = fn("gethostname"); pWSAIoctl = fn("WSAIoctl"); pNetbios = fn("Netbios");

    CHECK("vs_init", vs_init(&TR) == 0);
    CHECK("socket before WSAStartup: WSANOTINITIALISED", psocket(2, 1, 0) == INVALID_SOCKET && pWSAGetLastError() == WSANOTINITIALISED);
    WSADATA wd; DWORD t0 = GetTickCount();
    int e = pWSAStartup(MAKEWORD(2, 2), &wd);
    printf("  WSAStartup: %d in %lu ms, version %u.%u, \"%s\"\n", e, GetTickCount() - t0, wd.wVersion & 255, wd.wVersion >> 8, wd.szDescription);
    CHECK("WSAStartup(2.2) brings the link up", e == 0 && wd.wVersion == 0x0202);
    if (e) return 1;

    char hn[64] = ""; pgethostname(hn, sizeof hn);
    CHECK("gethostname is VLAN-<host>", !strncmp(hn, "VLAN-", 5));
    struct hostent *h = pgethostbyname("pcnfs05.ea.com");
    CHECK("gethostbyname(pcnfs05.ea.com) = 10.77.0.1", h && h->h_length == 4 && ntohl(*(uint32_t *)h->h_addr_list[0]) == IP(10, 77, 0, 1));
    h = pgethostbyname(hn);
    uint32_t me = h ? ntohl(*(uint32_t *)h->h_addr_list[0]) : 0;
    printf("  own address: %u.%u.%u.%u (%s)\n", me >> 24, me >> 16 & 255, me >> 8 & 255, me & 255, hn);
    CHECK("gethostbyname(own name) = a 10.77 address", (me >> 16) == 0x0A4D);
    CHECK("gethostbyname(www.ea.com) fails WSAHOST_NOT_FOUND", !pgethostbyname("www.ea.com") && pWSAGetLastError() == WSAHOST_NOT_FOUND);
    {
        uint8_t ncb[64], as[60]; memset(ncb, 0, sizeof ncb);
        ncb[0] = 0x33; ncb[0xA] = '*'; *(uint8_t **)(ncb + 4) = as; *(uint16_t *)(ncb + 8) = sizeof as;
        UCHAR rc = pNetbios(ncb);
        CHECK("Netbios NCBASTAT: adapter 02:4e:56:4c:x:y from the address", rc == 0 && as[0] == 2 && as[1] == 0x4E && as[4] == (uint8_t)(me >> 8) && as[5] == (uint8_t)me);
    }

    /* the redirector, as LobbyConnect does it: non-blocking, select for writable */
    SOCKET s = psocket(AF_INET, SOCK_STREAM, 0);
    u_long one = 1; pioctlsocket(s, FIONBIO, &one);
    struct sockaddr_in a = sin_(IP(10, 77, 0, 1), 20920);
    int r = pconnect(s, SA(a), sizeof a);
    CHECK("non-blocking connect: SOCKET_ERROR + WSAEWOULDBLOCK", r == SOCKET_ERROR && pWSAGetLastError() == WSAEWOULDBLOCK);
    int rd, wr, ex; t0 = GetTickCount();
    int n = sel1(s, 0, 1, 1, 5000, &rd, &wr, &ex);
    printf("  select: %d after %lu ms (writable %d, except %d)\n", n, GetTickCount() - t0, wr, ex);
    CHECK("select reports the connect writable", n == 1 && wr && !ex);
    struct sockaddr_in pa; int pal = sizeof pa;
    CHECK("getpeername = 10.77.0.1:20920", pgetpeername(s, SA(pa), &pal) == 0 && ntohl(pa.sin_addr.s_addr) == IP(10, 77, 0, 1) && ntohs(pa.sin_port) == 20920);
    struct sockaddr_in la; int lal = sizeof la;
    CHECK("getsockname = our address", pgetsockname(s, SA(la), &lal) == 0 && ntohl(la.sin_addr.s_addr) == me && ntohs(la.sin_port) >= 49152);
    char msg[256]; int ml = aries(msg, "@dir", "PROD=NFS-PC-2005\nVERS=\"PC/NFSU2-2004\"\nLANG=en\nSLUS=SLUS_20811\n");
    CHECK("send @dir", psend(s, msg, ml, 0) == ml);
    char rb[1024]; int got = 0, eof = 0; t0 = GetTickCount();
    while (GetTickCount() - t0 < 5000 && !eof){
        n = sel1(s, 1, 0, 0, 1000, &rd, &wr, &ex);
        if (n <= 0) continue;
        r = precv(s, rb + got, (int)sizeof rb - 1 - got, 0);
        if (r > 0) got += r; else if (r == 0) eof = 1; else break;
    }
    rb[got] = 0;
    printf("  @dir answer: %d bytes: %.4s ... %s\n", got, rb, got > 12 ? rb + 12 : "");
    CHECK("@dir answers ADDR=10.77.0.1 PORT=20922", got > 12 && !memcmp(rb, "@dir", 4) && strstr(rb + 12, "ADDR=10.77.0.1") && strstr(rb + 12, "PORT=20922"));
    CHECK("then the redirector closes (recv 0)", eof);
    CHECK("closesocket", pclosesocket(s) == 0);

    /* refused: exceptfds and SO_ERROR */
    s = psocket(AF_INET, SOCK_STREAM, 0); pioctlsocket(s, FIONBIO, &one);
    a = sin_(IP(10, 77, 0, 1), 9);
    pconnect(s, SA(a), sizeof a);
    n = sel1(s, 0, 1, 1, 5000, &rd, &wr, &ex);
    int so = 0, sol = sizeof so; pgetsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so, &sol);
    CHECK("a refused connect: exceptfds, SO_ERROR WSAECONNREFUSED", n == 1 && ex && !wr && so == WSAECONNREFUSED);
    pclosesocket(s);

    /* the lobby, blocking */
    s = psocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    a = sin_(IP(10, 77, 0, 1), 20922);
    t0 = GetTickCount();
    r = pconnect(s, SA(a), sizeof a);
    printf("  blocking connect to the lobby: %d (%d) in %lu ms\n", r, r ? pWSAGetLastError() : 0, GetTickCount() - t0);
    CHECK("blocking connect to 10.77.0.1:20922", r == 0);
    {
        DWORD ret = 0; struct sockaddr_in q = sin_(IP(10, 77, 0, 1), 0), o; memset(&o, 0, sizeof o);
        r = pWSAIoctl(s, SIO_ROUTING_INTERFACE_QUERY, &q, sizeof q, &o, sizeof o, &ret, 0, 0);
        CHECK("SIO_ROUTING_INTERFACE_QUERY = our address", r == 0 && ret == 16 && ntohl(o.sin_addr.s_addr) == me);
    }
    pclosesocket(s);

    /* UDP: port unreachable on a connected socket */
    s = psocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    a = sin_(IP(10, 77, 0, 1), 5555);
    pconnect(s, SA(a), sizeof a);
    psend(s, "x", 1, 0);
    n = sel1(s, 1, 0, 0, 3000, &rd, &wr, &ex);
    r = precv(s, rb, sizeof rb, 0);
    CHECK("UDP to a closed port: the next recv fails WSAECONNRESET", n == 1 && r == SOCKET_ERROR && pWSAGetLastError() == WSAECONNRESET);
    pclosesocket(s);

    /* ICMP echo (ProtoPing): the IP header in front */
    s = psocket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    uint8_t echo[16] = { 8, 0, 0, 0, 0x12, 0x34, 0, 1, 'n', 'f', 's', 'u', '2', 0, 0, 0 };
    { uint32_t sum = 0; for (int i = 0; i < 16; i += 2) sum += (uint32_t)(echo[i] << 8 | echo[i + 1]);
      while (sum >> 16){ sum = (sum & 0xFFFF) + (sum >> 16); }
      sum = ~sum & 0xFFFF; echo[2] = (uint8_t)(sum >> 8); echo[3] = (uint8_t)sum; }
    a = sin_(IP(10, 77, 0, 1), 0);
    r = psendto(s, (char *)echo, sizeof echo, 0, SA(a), sizeof a);
    n = sel1(s, 1, 0, 0, 3000, &rd, &wr, &ex);
    struct sockaddr_in fa; int fal = sizeof fa;
    r = n == 1 ? precvfrom(s, rb, sizeof rb, 0, SA(fa), &fal) : -1;
    CHECK("ICMP echo from 10.77.0.1: IP header + echo reply", r == 20 + 16 && (uint8_t)rb[0] == 0x45 && rb[20] == 0 && rb[24] == 0x12 &&
          ntohl(fa.sin_addr.s_addr) == IP(10, 77, 0, 1));
    pclosesocket(s);

    /* in-process TCP: listen, a connect to our own address, accept, both ways, close */
    SOCKET ls = psocket(AF_INET, SOCK_STREAM, 0);
    a = sin_(0, 7777);
    CHECK("bind 0.0.0.0:7777 + listen", pbind(ls, SA(a), sizeof a) == 0 && plisten(ls, 4) == 0);
    SOCKET ls2 = psocket(AF_INET, SOCK_STREAM, 0);
    CHECK("a second bind of 7777: WSAEADDRINUSE", pbind(ls2, SA(a), sizeof a) == SOCKET_ERROR && pWSAGetLastError() == WSAEADDRINUSE);
    pclosesocket(ls2);
    SOCKET c = psocket(AF_INET, SOCK_STREAM, 0); pioctlsocket(c, FIONBIO, &one);
    a = sin_(me, 7777);
    pconnect(c, SA(a), sizeof a);
    n = sel1(ls, 1, 0, 0, 2000, &rd, &wr, &ex);
    struct sockaddr_in aa; int aal = sizeof aa;
    SOCKET as_ = n == 1 ? paccept(ls, SA(aa), &aal) : INVALID_SOCKET;
    CHECK("listener readable, accept gives the connector's address", as_ != INVALID_SOCKET && ntohl(aa.sin_addr.s_addr) == me);
    n = sel1(c, 0, 1, 0, 2000, &rd, &wr, &ex);
    CHECK("the local connect is writable", n == 1 && wr);
    psend(c, "hello", 5, 0);
    r = precv(as_, rb, sizeof rb, 0);
    CHECK("bytes arrive in order", r == 5 && !memcmp(rb, "hello", 5));
    u_long avail = 0; psend(as_, "world!", 6, 0); pioctlsocket(c, FIONREAD, &avail);
    CHECK("FIONREAD counts the queued bytes", avail == 6);
    pclosesocket(as_);
    r = precv(c, rb, sizeof rb, 0);
    int r2 = precv(c, rb + 6, sizeof rb - 6, 0);
    CHECK("the data before the close, then recv 0", r == 6 && r2 == 0);
    pclosesocket(c); pclosesocket(ls);

    /* a broadcast comes back to the sender's machine (Windows does that) */
    s = psocket(AF_INET, SOCK_DGRAM, 0);
    BOOL b1 = TRUE; psetsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *)&b1, sizeof b1);
    a = sin_(0, 9999); pbind(s, SA(a), sizeof a);
    a = sin_(0xFFFFFFFFu, 9999);
    r = psendto(s, "advert", 6, 0, SA(a), sizeof a);
    fal = sizeof fa;
    n = sel1(s, 1, 0, 0, 1000, &rd, &wr, &ex);
    r = n == 1 ? precvfrom(s, rb, sizeof rb, 0, SA(fa), &fal) : -1;
    CHECK("our own broadcast on 9999 comes back from our address", r == 6 && ntohl(fa.sin_addr.s_addr) == me);
    SOCKET s2 = psocket(AF_INET, SOCK_DGRAM, 0);
    r = psendto(s2, "x", 1, 0, SA(a), sizeof a);
    CHECK("a broadcast without SO_BROADCAST: WSAEACCES", r == SOCKET_ERROR && pWSAGetLastError() == WSAEACCES);
    pclosesocket(s2); pclosesocket(s);

    /* closesocket wakes a blocking recv on another thread */
    g_blk = psocket(AF_INET, SOCK_DGRAM, 0);
    a = sin_(0, 0); pbind(g_blk, SA(a), sizeof a);
    g_blk_r = 99;
    HANDLE th = CreateThread(0, 0, blocking_reader, 0, 0, 0);
    Sleep(300);
    CHECK("a blocking recv waits (the other thread is not stuck)", g_blk_r == 99);
    pclosesocket(g_blk);
    DWORD w = WaitForSingleObject(th, 2000);
    CHECK("closesocket wakes it: SOCKET_ERROR, WSAEINTR", w == WAIT_OBJECT_0 && g_blk_r == SOCKET_ERROR && g_blk_e == WSAEINTR);

    /* diagnostics: data the game leaves unread is reported after 20 s, once */
    {
        SOCKET u = psocket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in ra = sin_(IP(10, 77, 0, 1), 20920);
        int ok = pconnect(u, SA(ra), sizeof ra) == 0;
        char m2[256]; int l2 = aries(m2, "@dir", "PROD=NFS-PC-2005\n");
        ok = ok && psend(u, m2, l2, 0) == l2;
        printf("  waiting up to 22 s with the @dir answer unread ...\n");
        for (int i = 0; i < 220 && !g_saw_stall; i++) Sleep(100);
        CHECK("no \"with no session open\" report during the session", !g_saw_nosess);
    CHECK("an answer left unread for 20 s is reported (\"not reading this connection\")", ok && g_saw_stall);
        pclosesocket(u);
    }

    CHECK("WSACleanup", pWSACleanup() == 0);
    CHECK("the calls up to the last WSACleanup are reported", g_saw_cleanup_calls);
    vs_report();
    /* diagnostics: a thread that spins on select with no session open (every call fails at once) is reported
     * within about 10 s */
    {
        InterlockedExchange(&g_saw_nosess, 0);             /* this spin's own report, not an earlier line */
        fd_set R; FD_ZERO(&R); FD_SET((SOCKET)0x1004, &R);
        struct timeval tv = { 0, 100000 };
        long n = 0; DWORD t1 = GetTickCount();
        int fast = 1;
        while (GetTickCount() - t1 < 11500 && !g_saw_nosess){
            DWORD c0 = GetTickCount();
            if (pselect(0, &R, 0, 0, &tv) != SOCKET_ERROR || pWSAGetLastError() != WSANOTINITIALISED) fast = 0;
            if (GetTickCount() - c0 > 50) fast = 0;   /* it must fail at once, not wait its timeout */
            n++;
        }
        printf("  %ld selects with no session in %lu ms\n", n, GetTickCount() - t1);
        CHECK("select with no session fails at once with WSANOTINITIALISED", fast);
        CHECK("a spin on select with no session is reported (\"with no session open\")", g_saw_nosess);
    }
    Sleep(300);                                              /* the writer tells the switch LAN play is shut */
    printf(fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", fails);
    if (g_sw) sw_stop(g_sw);
    return fails ? 1 : 0;
}
