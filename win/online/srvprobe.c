/* srvprobe.c -- EA's LAN server (the game's own server.dll, from the player's game folder, never from this repo) on the
 * virtual LAN, without the game: what the plugin's LoadLibraryA hook does to it (win/roam_asi/roam_asi.c
 * server_dll_patch), then StartServer as the game's StartLANServer (0x5FA300) calls it, and a second process that
 * joins it over the switch.
 *
 *   srvprobe.exe host <ws://host:port/lan> <path\to\server.dll> [name=PROBEHOST] [seconds=40]
 *   srvprobe.exe join <ws://host:port/lan> <host vip a.b.c.d> [seconds=10]
 *
 * host: the server.dll's Winsock imports redirected to vsock (vs_imports, by name; htons & co. stay), StartServer(name,
 *       region 0), its listening sockets logged, StopServer at the end.  join: a TCP connect to the host's vip:9900 (the
 *       LAN server's port, mprace.c LAN_PORT), what the server sends first, and LAN broadcasts heard on the server's UDP
 *       ports while it runs.  Exit 0 when it passes. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vsock.h"
#include "switch.h"

static void *fn(const char *name){
    for (const vs_import *i = vs_imports; i->dll; i++) if (!strcmp(i->name, name)) return i->fn;
    printf("no replacement for %s\n", name); ExitProcess(2);
}
static sw *g_sw; static const char *g_url, *g_name = "srvprobe";
static volatile LONG g_vip;
static int tr_send(void *u, const uint8_t *f, int len){ (void)u; return g_sw && sw_send_frame(g_sw, f, len); }
static void tr_lan(void *u, int on, uint32_t udp){ (void)u; printf("  [lan state] on %d, udp ports 0x%08x\n", on, udp); if (g_sw) sw_set_lan(g_sw, on, udp); }
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

/* the plugin's server_dll_patch, as it does it */
static int patch(uint8_t *base){
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *id = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); id->Name; id++){
        const char *dll = (const char *)(base + id->Name);
        if (lstrcmpiA(dll, "ws2_32.dll") && lstrcmpiA(dll, "netapi32.dll")) continue;
        IMAGE_THUNK_DATA32 *ot = (IMAGE_THUNK_DATA32 *)(base + id->OriginalFirstThunk), *ft = (IMAGE_THUNK_DATA32 *)(base + id->FirstThunk);
        for (; ot->u1.AddressOfData; ot++, ft++){
            int byord = IMAGE_SNAP_BY_ORDINAL32(ot->u1.Ordinal), ord = (int)IMAGE_ORDINAL32(ot->u1.Ordinal);
            const char *nm = byord ? 0 : (const char *)((IMAGE_IMPORT_BY_NAME *)(base + ot->u1.AddressOfData))->Name;
            void *f = 0;
            for (const vs_import *v = vs_imports; v->dll && !f; v++){
                if (lstrcmpiA(v->dll, dll)) continue;
                if (byord ? (v->ord && v->ord == ord) : !strcmp(v->name, nm)) f = v->fn;
            }
            if (!f){ if (nm) printf("  (%s %s stays)\n", dll, nm); else printf("  (%s #%d stays)\n", dll, ord); continue; }
            DWORD old; VirtualProtect(&ft->u1.Function, 4, PAGE_READWRITE, &old);
            ft->u1.Function = (DWORD)(uintptr_t)f; VirtualProtect(&ft->u1.Function, 4, old, &old); n++;
        }
    }
    return n;
}

static int host(const char *dllpath, const char *name, int secs){
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s", dllpath);
    char *sl = strrchr(dir, '\\'); if (!sl) sl = strrchr(dir, '/');
    if (sl){ *sl = 0; SetCurrentDirectoryA(dir); }                 /* (server.cfg is read from the game's folder) */
    HMODULE h = LoadLibraryA(dllpath);
    CHECK("server.dll loaded", h != 0);
    if (!h) return 1;
    int n = patch((uint8_t *)h);
    printf("  %d of server.dll's imports redirected\n", n);
    CHECK("server.dll's Winsock imports redirected (21 or more)", n >= 21);
    typedef char (__cdecl *start_t)(const char *, int);
    typedef char (__cdecl *running_t)(void);
    typedef void (__cdecl *stop_t)(void);
    start_t st = (start_t)(void *)GetProcAddress(h, "StartServer");
    running_t ir = (running_t)(void *)GetProcAddress(h, "IsServerRunning");
    stop_t sp = (stop_t)(void *)GetProcAddress(h, "StopServer");
    CHECK("its exports", st && ir && sp);
    if (!st || !ir || !sp) return 1;
    char r = st(name, 0);
    printf("  StartServer(\"%s\", 0) = %d\n", name, r);
    CHECK("StartServer took", r != 0);
    for (int i = 0; i < 100 && !g_vip; i++) Sleep(50);
    CHECK("on the switch (its WSAStartup brought the link up)", g_vip != 0);
    for (int t = 0; t < secs; t++){
        Sleep(1000);
        if (t % 10 == 9){ printf("  running %d s: IsServerRunning %d\n", t + 1, ir()); vs_report(); }
    }
    CHECK("still running at the end", ir() != 0);
    sp();
    printf("  StopServer\n");
    return 0;
}

static int join(const char *hostip, int secs){
    int (WSAAPI *pWSAStartup)(WORD, LPWSADATA) = fn("WSAStartup");
    SOCKET (WSAAPI *psocket)(int, int, int) = fn("socket");
    int (WSAAPI *pconnect)(SOCKET, const struct sockaddr *, int) = fn("connect");
    int (WSAAPI *precv)(SOCKET, char *, int, int) = fn("recv");
    int (WSAAPI *psend)(SOCKET, const char *, int, int) = fn("send");
    int (WSAAPI *pselect)(int, fd_set *, fd_set *, fd_set *, const struct timeval *) = fn("select");
    int (WSAAPI *pclosesocket)(SOCKET) = fn("closesocket");
    int (WSAAPI *pWSAGetLastError)(void) = fn("WSAGetLastError");
    WSADATA wd; CHECK("WSAStartup", pWSAStartup(0x202, &wd) == 0);
    for (int i = 0; i < 100 && !g_vip; i++) Sleep(50);
    unsigned a, b, c, d;
    if (sscanf(hostip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4){ printf("bad host address\n"); return 1; }
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(9900); sa.sin_addr.s_addr = htonl(a << 24 | b << 16 | c << 8 | d);
    SOCKET s = psocket(AF_INET, SOCK_STREAM, 0);
    int r = pconnect(s, (struct sockaddr *)&sa, sizeof sa);
    printf("  connect %s:9900 = %d (error %d)\n", hostip, r, r ? pWSAGetLastError() : 0);
    CHECK("a TCP connect to the host's LAN server (9900) on the virtual LAN", r == 0);
    /* say something as a client would ("@tic" is DirtySock's first message; whatever it answers shows it reads us) */
    char hello[32] = { '@', 't', 'i', 'c', 0, 0, 0, 0, 0, 0, 0, 12 };
    if (!r) psend(s, hello, 12, 0);
    fd_set R; FD_ZERO(&R); FD_SET(s, &R); struct timeval tv = { secs, 0 };
    int n = r ? 0 : pselect(0, &R, 0, 0, &tv), got = 0; char buf[512];
    if (n > 0) got = precv(s, buf, sizeof buf, 0);
    printf("  the server sent %d bytes", got);
    if (got > 0){ printf(": "); for (int i = 0; i < got && i < 48; i++) printf("%c", buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.'); }
    printf("\n");
    CHECK("the connection stays up or the server answers (no reset)", r == 0 && got >= 0);
    pclosesocket(s);
    vs_report();
    return 0;
}

int main(int argc, char **argv){
    if (argc < 4){ fprintf(stderr, "usage: srvprobe host <url> <server.dll> [name] [seconds] | join <url> <vip> [seconds]\n"); return 2; }
    g_url = argv[2];
    if (vs_init(&TR) != 0){ printf("vs_init failed\n"); return 1; }
    if (!strcmp(argv[1], "host")){ g_name = "srvhost"; host(argv[3], argc > 4 ? argv[4] : "PROBEHOST", argc > 5 ? atoi(argv[5]) : 40); }
    else { g_name = "srvjoin"; join(argv[3], argc > 4 ? atoi(argv[4]) : 10); }
    printf("%s (%d failed)\n", fails ? "FAIL" : "DONE", fails);
    fflush(stdout);
    if (g_sw) sw_stop(g_sw);
    return fails ? 1 : 0;
}
