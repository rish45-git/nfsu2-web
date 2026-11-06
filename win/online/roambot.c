/* roambot.c -- simulated free-roam players that speak the REAL protocol (runtime/roam_core.c, the same core as the
 * game and the browser), for testing the native plugin against a server without a second game.
 *
 *   roambot.exe <ws://host:port/lan> <car record file> [n=3] [x=0] [y=0] [z=0] [seconds=600] [room name]
 *
 * Each bot joins the server's switch as its own player (own virtual address), talks to the free-roam hub like a game
 * (HELLO, SNAPs, the hub's PEERS), shows the car in the record file (the plugin saves yours with DumpCar = 1 in its
 * .ini: scripts\NFSU2FreeRoam.car) and drives round a circle of 30 m (staggered) round x,y,z, so a real game next to
 * that spot sees friends, the world map and the minimap show players.  Unlike web/server-rs/crates/roamload (a
 * load generator for the server, whose states are not the protocol's), a game accepts these.
 *
 * Normal C runtime; the core and switch.c are the plugin's own files. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef far                              /* (16-bit leftovers: roam_core.h has fields of those names) */
#undef near
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "roam_core.h"
#include "switch.h"

#define PORT RC_DEFAULT_PORT

typedef struct {
    int n;
    char name[24];
    rc_session *s;
    sw *sw;
    CRITICAL_SECTION mu;
    volatile LONG vip, mask;
    unsigned long long frames_in;
    float cx, cy, cz, phase;
} bot;

static int64_t now_us(void){
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (c.QuadPart / f.QuadPart) * 1000000 + (c.QuadPart % f.QuadPart) * 1000000 / f.QuadPart;
}

static void io_send(void *u, uint32_t ip, uint16_t port, const uint8_t *buf, int len){
    bot *b = u; uint32_t vip = (uint32_t)b->vip, mask = (uint32_t)b->mask;
    if (!vip || (ip & mask) != (vip & mask)) return;
    uint32_t dst = ip == ((vip & mask) | ~mask) ? SW_BROADCAST : ip;
    sw_send_udp(b->sw, SW_KIND_UDP, dst, PORT, port, buf, len);
}
static void io_broadcast(void *u, uint16_t port, const uint8_t *buf, int len){ (void)u; (void)port; (void)buf; (void)len; }
static void io_event(void *u, int ev, int peer, const char *text){
    bot *b = u; (void)ev; (void)peer;
    if (b->n == 0) printf("[%s] %s\n", b->name, text);
}

static void on_welcome(void *u, uint32_t vip, uint32_t mask, const char *tok){
    bot *b = u; (void)tok;
    InterlockedExchange(&b->mask, (LONG)mask); InterlockedExchange(&b->vip, (LONG)vip);
    printf("[%s] on the switch as %u.%u.%u.%u\n", b->name, vip >> 24, (vip >> 16) & 255, (vip >> 8) & 255, vip & 255);
}
static void on_frame(void *u, const uint8_t *f, int len){
    bot *b = u;
    if (len < 16 || f[0] != SW_KIND_UDP) return;
    uint32_t src = (uint32_t)f[4] << 24 | (uint32_t)f[5] << 16 | (uint32_t)f[6] << 8 | f[7];
    uint32_t dst = (uint32_t)f[8] << 24 | (uint32_t)f[9] << 16 | (uint32_t)f[10] << 8 | f[11];
    uint16_t sport = (uint16_t)(f[12] << 8 | f[13]), dport = (uint16_t)(f[14] << 8 | f[15]);
    uint32_t vip = (uint32_t)b->vip;
    if (dport != PORT || !vip || src == vip || (dst != vip && dst != SW_BROADCAST)) return;
    EnterCriticalSection(&b->mu);
    b->frames_in++;
    rc_receive(b->s, src, sport, f + 16, len - 16, now_us());
    LeaveCriticalSection(&b->mu);
}
static void on_log(void *u, const char *l){ bot *b = u; if (b->n == 0) printf("[switch] %s\n", l); }

/* the state of bot b at time t (s): a circle of 30 m at 12 m/s round (cx, cy, cz) */
static void state_at(const bot *b, double t, rc_state *st){
    const double R = 30.0, V = 12.0, w = V / R;
    double a = b->phase + t * w;
    memset(st, 0, sizeof *st);
    st->t_us = (uint32_t)now_us();
    st->p[0] = b->cx + (float)(R * cos(a)); st->p[1] = b->cy + (float)(R * sin(a)); st->p[2] = b->cz;
    st->v[0] = (float)(-V * sin(a)); st->v[1] = (float)(V * cos(a)); st->v[2] = 0.f;
    float yaw = (float)(a + 1.5707963267948966);                 /* heading along the circle */
    float f[3] = { cosf(yaw), sinf(yaw), 0.f }, l[3] = { -sinf(yaw), cosf(yaw), 0.f }, u[3] = { 0.f, 0.f, 1.f };
    rc_m2q(f, l, u, st->q);
    st->w[2] = (float)w;
    st->steer = 0.12f; st->throttle = 0.5f; st->rpm = 3500.f; st->speed = (float)V; st->gear = 3;
    st->ext = 1;
    for (int i = 0; i < 4; i++) st->wz[i] = 0.28f;                 /* a car at rest: 0.26 .. 0.29 in the game (car +0x168..) */
}

typedef struct { bot *b; const uint8_t *car; int carlen; double seconds; } run;
static DWORD WINAPI bot_main(LPVOID p){
    run *r = p; bot *b = r->b;
    int64_t t0 = now_us(), next_tick = 0, next_push = 0;
    EnterCriticalSection(&b->mu);
    rc_set_status(b->s, RC_ST_ROAM);
    rc_set_cfg(b->s, r->car, r->carlen, now_us());
    LeaveCriticalSection(&b->mu);
    while ((now_us() - t0) < (int64_t)(r->seconds * 1e6)){
        int64_t now = now_us();
        EnterCriticalSection(&b->mu);
        if (now >= next_tick){ rc_tick(b->s, now); next_tick = now + 10000; }
        if (now >= next_push && b->vip){
            rc_state st; state_at(b, (double)(now - t0) / 1e6, &st);
            rc_push_state(b->s, &st, now);
            next_push = now + 50000;                               /* 20 a second */
        }
        LeaveCriticalSection(&b->mu);
        Sleep(5);
    }
    return 0;
}

int main(int argc, char **argv){
    if (argc < 3){ fprintf(stderr, "usage: roambot <ws://host:port/lan> <car record file> [n] [x] [y] [z] [seconds] [room]\n"); return 2; }
    const char *url = argv[1];
    int n = argc > 3 ? atoi(argv[3]) : 3; if (n < 1) n = 1; if (n > 16) n = 16;
    float x = argc > 4 ? (float)atof(argv[4]) : 0.f, y = argc > 5 ? (float)atof(argv[5]) : 0.f, z = argc > 6 ? (float)atof(argv[6]) : 0.f;
    double seconds = argc > 7 ? atof(argv[7]) : 600.0;
    const char *room = argc > 8 ? argv[8] : "";
    static uint8_t car[RC_CFG_MAX]; int carlen = 0;
    rc_carcfg cc; int e = 0;
    if (!strcmp(argv[2], "synth")){       /* a made-up record: the bots see each other, a game may not know the car */
        memset(&cc, 0, sizeof cc); cc.type_hash = rc_fnv1a("350Z"); snprintf(cc.type_name, sizeof cc.type_name, "350Z");
        carlen = rc_carcfg_encode(&cc, car, sizeof car);
    } else {
        FILE *fp = fopen(argv[2], "rb");
        if (!fp){ fprintf(stderr, "cannot open the car record %s (the plugin writes it with DumpCar = 1)\n", argv[2]); return 2; }
        carlen = (int)fread(car, 1, sizeof car, fp); fclose(fp);
        e = rc_carcfg_decode(car, carlen, &cc);
    }
    if (e || carlen <= 0){ fprintf(stderr, "%s is not a car record (error %d)\n", argv[2], e); return 2; }
    printf("car: %s, %d bytes; %d bot(s) round %.0f,%.0f,%.0f for %.0f s%s%s\n", cc.type_name, carlen, n, x, y, z, seconds, *room ? ", room " : "", room);

    bot *bots = calloc((size_t)n, sizeof *bots); run *runs = calloc((size_t)n, sizeof *runs); HANDLE *th = calloc((size_t)n, sizeof *th);
    for (int i = 0; i < n; i++){
        bot *b = &bots[i]; b->n = i; snprintf(b->name, sizeof b->name, "RoamBot%d", i + 1);
        b->cx = x; b->cy = y; b->cz = z; b->phase = (float)(i * 6.2831853 / n);
        InitializeCriticalSection(&b->mu);
        b->s = calloc(1, sizeof *b->s);
        rc_config cf; memset(&cf, 0, sizeof cf);
        cf.self_id = (uint32_t)(GetTickCount() * 2654435761u + (uint32_t)(i + 1) * 40503u) | 1u;
        cf.port = PORT; cf.room = rc_room_hash(room); cf.platform = RC_PLAT_TOOL;
        snprintf(cf.name, sizeof cf.name, "%s", b->name);
        cf.max_players = RC_MAX_PLAYERS; cf.rate_hz = 20; cf.snap_states = 1; cf.links = 5;
        cf.hub_ip = SW_VIP_ROAM_HUB; cf.hub_port = PORT;
        rc_io io = { b, io_send, io_broadcast, io_event };
        rc_session_init(b->s, &cf, &io);
        sw_options o = { url, b->name, 0, 0, 10000 };
        sw_callbacks cb = { on_welcome, on_frame, 0, 0, 0, 0, 0, on_log, b };
        b->sw = sw_start(&o, &cb);
        if (!b->sw){ fprintf(stderr, "bad URL\n"); return 2; }
        runs[i] = (run){ b, car, carlen, seconds };
        th[i] = CreateThread(0, 0, bot_main, &runs[i], 0, 0);
        Sleep(150);
    }
    WaitForMultipleObjects((DWORD)n, th, TRUE, INFINITE);
    for (int i = 0; i < n; i++){
        bot *b = &bots[i];
        EnterCriticalSection(&b->mu);
        printf("[%s] frames in %llu, friends now:", b->name, b->frames_in);
        int k = 0; for (int j = 0; j < RC_TABLE; j++) if (b->s->peer[j].state == RC_PS_EST){ printf(" %s", b->s->peer[j].name); k++; }
        printf("%s; rx %u, bad %u, tx %u\n", k ? "" : " none", b->s->rx, b->s->rx_bad, b->s->tx);
        LeaveCriticalSection(&b->mu);
        sw_stop(b->sw);
    }
    return 0;
}
