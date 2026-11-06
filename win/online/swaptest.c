/* swaptest.c -- the plugin's server switch (win/roam_asi/roam_win.c roam_win_server_next) against a running server,
 * without the game: the plugin's own roam_win.c, switch.c and the core, with stand-ins for roam_game.c's side.
 *
 *   swaptest.exe <servers> [seconds per server=12]
 *     servers: the plugin's Servers = value, e.g. "a=ws://127.0.0.1:8691/lan,b=ws://192.168.0.109:8691/lan"
 *
 * Starts free roam online on the first server (as the plugin with Server = the first), drives a car state at 0,0,0
 * (so roambot.exe there links with it), then presses the ServerKey twice (to the second server and back).  Checks:
 * a welcome on each server (a new address each time), the old server's players dropped at each switch, a friend
 * (a roambot) linked again on the new server, and the switch refused while "the game" says no
 * (roam_game_switch_block).  Exit 0 when all pass. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef far
#undef near
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "roam_core.h"
#include "roam_plat.h"

/* ---- what roam_win.c takes from the rest of the plugin ---- */
static CRITICAL_SECTION g_logmu; static int g_logok;
static char g_last_note[200]; static volatile LONG g_welcomes, g_drops, g_joins;
static unsigned g_vips[8]; static int g_nvips;
void roam_log(const char *fmt, ...){
    if (!g_logok){ InitializeCriticalSection(&g_logmu); g_logok = 1; }
    char b[1200]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    EnterCriticalSection(&g_logmu);
    printf("  log| %s", b); if (!*b || b[strlen(b) - 1] != '\n') putchar('\n');
    unsigned a, c, d, e;
    const char *w = strstr(b, "on the server's virtual LAN as ");
    if (w && sscanf(w + 31, "%u.%u.%u.%u", &a, &c, &d, &e) == 4){ if (g_nvips < 8) g_vips[g_nvips++] = a << 24 | c << 16 | d << 8 | e; InterlockedIncrement(&g_welcomes); }
    if (strstr(b, "player(s) of the old server dropped")) InterlockedIncrement(&g_drops);
    LeaveCriticalSection(&g_logmu);
    fflush(stdout);
}
static char g_servers[600], g_server0[200];
const char *roam_opt(const char *name){
    if (!strcmp(name, "NFSU2_ROAM_SERVERS")) return g_servers;
    if (!strcmp(name, "NFSU2_ROAM_SERVER")) return g_server0;
    if (!strcmp(name, "NFSU2_ROAM_NAME")) return "SwapTest";
    return 0;
}
void roam_game_event(int ev, int peer, const char *text){
    (void)peer;
    printf("  evt| %d %s\n", ev, text); fflush(stdout);
    if (ev == RC_EV_JOIN) InterlockedIncrement(&g_joins);
}
uint32_t roam_win_mxcsr_default(void){ return 0x1F80u; }
volatile int roam_win_exiting;
void roam_game_note(const char *t, int ms){ (void)ms; snprintf(g_last_note, sizeof g_last_note, "%s", t); printf("  top| %s\n", t); fflush(stdout); }
static const char *g_block;
const char *roam_game_switch_block(void){ return g_block; }
void roam_win_server_next(void);
void roam_win_server_frame(void);

static int64_t now_us(void){ return roam_plat_now_us(); }
static int fails;
static void check(int ok, const char *what){ printf("%s %s\n", ok ? "  ok  " : "  FAIL", what); fflush(stdout); if (!ok) fails++; }
static int wait_for(volatile LONG *v, LONG want, double s, rc_session *ses){
    int64_t t0 = now_us();
    while (*v < want && now_us() - t0 < (int64_t)(s * 1e6)){
        roam_plat_lock();
        rc_state st; memset(&st, 0, sizeof st);
        st.t_us = (uint32_t)now_us(); float f[3] = { 1, 0, 0 }, l[3] = { 0, 1, 0 }, u[3] = { 0, 0, 1 }; rc_m2q(f, l, u, st.q);
        for (int i = 0; i < 4; i++) st.wz[i] = 0.28f;
        st.ext = 1;
        rc_push_state(ses, &st, now_us());
        roam_plat_unlock();
        roam_win_server_frame();
        Sleep(50);
    }
    return *v >= want;
}

int main(int argc, char **argv){
    if (argc < 2){ fprintf(stderr, "usage: swaptest <name=url,name=url> [seconds]\n"); return 2; }
    snprintf(g_servers, sizeof g_servers, "%s", argv[1]);
    double secs = argc > 2 ? atof(argv[2]) : 12.0;
    { const char *eq = strchr(g_servers, '='), *end = strpbrk(g_servers, ",;");
      size_t n = (size_t)((end ? end : g_servers + strlen(g_servers)) - (eq ? eq + 1 : g_servers));
      memcpy(g_server0, eq ? eq + 1 : g_servers, n < sizeof g_server0 - 1 ? n : sizeof g_server0 - 1); }
    printf("start on %s\n", g_server0);
    static rc_session s;
    rc_config cf; memset(&cf, 0, sizeof cf);
    cf.self_id = (GetTickCount() * 2654435761u) | 1u; cf.port = RC_DEFAULT_PORT; cf.platform = RC_PLAT_TOOL;
    snprintf(cf.name, sizeof cf.name, "SwapTest");
    cf.max_players = RC_MAX_PLAYERS; cf.rate_hz = 20; cf.snap_states = 1; cf.links = 5;
    rc_io io0 = { 0, 0, 0, 0 };
    rc_session_init(&s, &cf, &io0);
    static uint8_t car[RC_CFG_MAX]; rc_carcfg cc; memset(&cc, 0, sizeof cc);
    cc.type_hash = rc_fnv1a("350Z"); snprintf(cc.type_name, sizeof cc.type_name, "350Z");
    int carlen = rc_carcfg_encode(&cc, car, sizeof car);
    roam_plat_cfg pc = { RC_DEFAULT_PORT, 0, 1, 0 };
    if (roam_plat_start(&pc, &s) != 0){ printf("roam_plat_start failed\n"); return 1; }
    roam_plat_lock(); rc_set_status(&s, RC_ST_ROAM); rc_set_cfg(&s, car, carlen, now_us()); roam_plat_unlock();

    check(wait_for(&g_welcomes, 1, secs, &s), "welcomed on the first server");
    int bots = wait_for(&g_joins, 1, secs, &s);
    printf("  (a friend on the first server: %s)\n", bots ? "yes" : "none: run roambot.exe for that check");

    g_block = "Not now (test)";
    roam_win_server_next();
    check(strstr(g_last_note, "Not now (test)") != 0 && g_welcomes == 1, "the switch refused while the game says no");
    g_block = 0;

    for (int round = 1; round <= 2; round++){
        LONG w0 = g_welcomes, d0 = g_drops, j0 = g_joins;
        roam_win_server_next();
        check(strstr(g_last_note, "connecting") != 0, "the switch began (\"connecting\" on the top line)");
        roam_win_server_next();
        check(strstr(g_last_note, "Still") != 0, "a second key press while it switches is refused");
        check(wait_for(&g_welcomes, w0 + 1, secs, &s), round == 1 ? "welcomed on the second server" : "welcomed back on the first");
        check(g_drops == d0 + 1, "the old server's players dropped");
        check(g_nvips >= 2 && g_vips[g_nvips - 1] != g_vips[g_nvips - 2], "a new virtual address");
        for (int i = 0; i < 10; i++){ roam_win_server_frame(); Sleep(20); }
        check(strstr(g_last_note, "connected as") != 0, "\"connected as\" on the top line");
        if (bots) check(wait_for(&g_joins, j0 + 1, secs, &s), "the friend linked again on the new server");
    }
    roam_plat_stop(1);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
