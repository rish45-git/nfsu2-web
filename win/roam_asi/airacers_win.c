/* airacers_win.c -- AI racers in multiplayer races (NFSU2FreeRoam.asi only; the player's list, item 5: "the races are
 * empty, add at least 3-4 other players as bots").
 *
 * The game's LAN race has its players only: its setup (0x526830, the network races' and split screen's) sets the race's
 * AI count [0x89E7C4] to 0 and adds no AI, and the network adds a racer record for each player as it hears of them
 * (0x601EB0: kind 1, +0x900 set, skill 0xFE).  The race's cars are made from the records at the track load
 * (0x6065B0, thiscall, from 0x60A774): grid places 1..6, each record's +3; kind 1 a human's car (a network player's,
 * +0x900, driven from the network), kind 2 an AI racer's (CreateCar 0x606480 with driver 3, the racer AI on the race's
 * route), kind 3 traffic.  So here, at the start of 0x6065B0 in a LAN race, the game's own quick-race AI adder runs
 * (0x53EE90, cdecl (count): AI names 0x4FE970, each one's car 0x4FEAF0 matched to racer 0's, its skill 0x4FED00 from
 * the difficulty [0x89E7F4], RacerInfo::Init 0x6373E0 kind 2, AddRacer 0x601600), and then:
 *  - the grid as it was: the adder ends with 0x532010, which places every racer anew; the players' records get their
 *    own places back (+0 and +3), and the AI the grid's free places after them, in order, the same on every game;
 *  - each AI racer's skill (+0x901) is AiSkill (0..9: 9 the game's top, what 0x610510 gives every AI under its own
 *    "best AI" switch [0x8650B0]);
 *  - the game's random numbers ([0x7EA624]) come from the lobby for the adder (so every game of the race picks the
 *    same names, and the same cars where racer 0's car is the same), and are as they were after.
 * Every game of the race adds its own: each one's AI race on that game alone (as the game's own split screen's
 * traffic), so they are not where they are on the other screens, and each game's results have its own.  The race
 * still has 6 places: AiRacers (4 by default, 0 off) is cut to what is free; they are Claude, ChatGPT, Gemini and
 * DeepSeek (the player's pick), and race in circuits, sprints, URL and Street X (no drag or drift).  Log lines "[ai]". */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "roam_host_win.h"

int mprace_phase(void);                  /* runtime/mprace.c */
uint32_t roam_game_lobby_id(void);       /* runtime/roam_game.c: the race lobby we are in (0 none) */

#define CARS_FN       0x006065b0u        /* the race's cars from the records: thiscall, no arguments */
#define FN_ADD_AI     0x0053ee90u        /* cdecl (count) */
#define G_GAMEMODE    0x00850078u        /* 0x200 LAN */
#define G_NETMODE     0x00850088u        /* the network race's mode: 0 circuit, 1 sprint, 2 drag, 3 drift, 4 URL, 10 Street X */
#define G_RECS        0x0089e810u        /* the race's racer records (0x940 each) */
#define G_NRECS       0x0089e814u
#define G_AI_N        0x0089e7c4u        /* the race's AI racers */
#define G_NOAI        0x0089e7e4u        /* byte: the adder adds none while it is set */
#define G_DIFFICULTY  0x0089e7f4u        /* the race's AI difficulty (the adder's skills' odds) */
#define G_RAND        0x007ea624u        /* the game's random numbers' state (0x43C1C0) */
#define REC_SZ        0x940u
#define REC_KIND      0x04u              /* 1 human, 2 AI, 3 traffic */
#define REC_GRID      0x03u              /* byte: its grid place, 1..6 */
#define REC_NET       0x900u             /* byte: a network player's */
#define REC_SKILL     0x901u             /* byte: 0..9; 0xFE a network player's */
#define REC_NAME      0x90cu             /* char[16] */
#define REC_MAX       22                 /* AddRacer's limit */
#define GRID_N        6

static int ok(uint32_t a, uint32_t n){ return a && roam_win_committed(a) && roam_win_committed(a + n); }
static int opt_num(const char *name, int def, int lo, int hi){
    const char *v = roam_opt(name);
    if (!v || !*v) return def;
    int x = 0, nd = 0;
    for (const char *p = v; *p >= '0' && *p <= '9' && nd < 6; p++, nd++) x = x * 10 + (*p - '0');
    return nd && x >= lo && x <= hi ? x : def;
}

static int g_ai_on;
static uint32_t g_ai_race;
static uint32_t g_ai_done;               /* the AI racers finished in this race: a bit each, by car id - 100 */
static void report_install(void);
static void names_install(void);
static void put_rel(uint8_t *at, uint32_t target);
typedef void (__attribute__((thiscall)) *cars_t)(uint32_t);
static cars_t g_cars;
static uint8_t g_page[4096] __attribute__((aligned(4096)));

static void ai_add(void){
    int want = opt_num("NFSU2_ROAM_AIRACERS", 4, 0, 4), skill = opt_num("NFSU2_ROAM_AISKILL", 9, 0, 9);
    if (!want) return;
    /* (circuits, sprints, URL and Street X: a drag race's AI needs the game's drag driver, a drift has no cars to race) */
    uint32_t mode = MR32(G_NETMODE);
    if (mode == 2u || mode == 3u){ roam_log("[ai] a %s race: no AI racers\n", mode == 2u ? "drag" : "drift"); return; }
    uint32_t recs = MR32(G_RECS), n0 = MR32(G_NRECS);
    if (!recs || n0 > REC_MAX || !ok(recs, REC_SZ * (n0 ? n0 : 1))) return;
    /* the players' places, kept */
    uint8_t keep0[REC_MAX], keep3[REC_MAX]; int used[GRID_N + 1] = { 0 }, humans = 0, ai = 0;
    for (uint32_t i = 0; i < n0; i++){
        uint32_t r = recs + i * REC_SZ, kind = MR32(r + REC_KIND);
        keep0[i] = MR8(r); keep3[i] = MR8(r + REC_GRID);
        if (kind == 1u) humans++;
        if (kind == 2u) ai++;
        if ((kind == 1u || kind == 2u) && keep3[i] >= 1 && keep3[i] <= GRID_N) used[keep3[i]] = 1;
    }
    if (ai){ roam_log("[ai] the race has %d AI racer(s) already: none added\n", ai); return; }
    int freep[GRID_N], nfree = 0;
    for (int p = 1; p <= GRID_N; p++) if (!used[p]) freep[nfree++] = p;
    int n = want < nfree ? want : nfree;
    if (n > REC_MAX - (int)n0) n = REC_MAX - (int)n0;
    if (n <= 0){ roam_log("[ai] no free place on the grid (%d player(s)): no AI racers\n", humans); return; }
    /* the game's random numbers from the lobby (the same on every game of it), as they were after */
    uint32_t rnd = MR32(G_RAND), lid = roam_game_lobby_id();
    MR32(G_RAND) = (lid ? lid : 0x5eed1234u) ^ 0x9e3779b9u;
    uint8_t noai = MR8(G_NOAI); MR8(G_NOAI) = 0;
    uint32_t a = (uint32_t)n;
    roam_win_call(FN_ADD_AI, 0, 0, 1, &a);
    MR8(G_NOAI) = noai;
    MR32(G_RAND) = rnd;
    recs = MR32(G_RECS);
    uint32_t n1 = MR32(G_NRECS);
    if (n1 > REC_MAX || n1 < n0 || !ok(recs, REC_SZ * n1)){ roam_log("[ai] the game's AI adder left %u records (was %u): not touched\n", n1, n0); return; }
    for (uint32_t i = 0; i < n0; i++){ MR8(recs + i * REC_SZ) = keep0[i]; MR8(recs + i * REC_SZ + REC_GRID) = keep3[i]; }
    int k = 0;
    for (uint32_t i = n0; i < n1 && k < nfree; i++, k++){
        uint32_t r = recs + i * REC_SZ;
        MR8(r) = 0; MR8(r + REC_GRID) = (uint8_t)freep[k]; MR8(r + REC_SKILL) = (uint8_t)skill;
        /* their names (the player's pick, 2026-11-07) */
        static const char *const k_names[4] = { "Claude", "ChatGPT", "Gemini", "DeepSeek" };
        for (int q = 0; q < 16; q++) MR8(r + REC_NAME + (uint32_t)q) = 0;
        for (int q = 0; k_names[k & 3][q] && q < 15; q++) MR8(r + REC_NAME + (uint32_t)q) = (uint8_t)k_names[k & 3][q];
        char nm[17]; int j = 0;
        for (; j < 16 && MR8(r + REC_NAME + (uint32_t)j) >= 0x20 && MR8(r + REC_NAME + (uint32_t)j) < 0x7f; j++) nm[j] = (char)MR8(r + REC_NAME + (uint32_t)j);
        nm[j] = 0;
        roam_log("[ai]   AI racer %d: %s, grid place %d, skill %d (kind %u)\n", k + 1, nm[0] ? nm : "(no name)", freep[k], skill, MR32(r + REC_KIND));
    }
    MR32(G_AI_N) += (uint32_t)(n1 - n0);
    if (n1 > n0){ g_ai_on = 1; g_ai_race = 0; g_ai_done = 0; }
    roam_log("[ai] the LAN race: %u AI racer(s) beside %d player(s) (AiRacers %d, AiSkill %d, difficulty %u, lobby %08x)\n",
             n1 - n0, humans, want, skill, MR32(G_DIFFICULTY), lid);
}
static void __attribute__((thiscall)) hk_cars(uint32_t self){
    if (MR32(G_GAMEMODE) == 0x200u) ai_add();
    g_cars(self);
}

/* The AI racers' finish.  In a LAN race the game finishes its players only (each game its own, the others' from the
 * network): an AI racer went over the line and on, lap after lap (the first try, 2026-11-07 04:49: 4.3 to 4.5 laps
 * of a 3-lap circuit when the race ended), and the results had every row "not finished", the players' too (a
 * finisher has as many whole laps as the farthest).  So each frame of a LAN race with ours in it, an AI racer (its car
 * id 100 and up: RacerInfo::Init's index from the adder) that has its laps (the racer's +9, the race's +0xBE8) and no
 * finish yet (+0x10) is finished as the game finishes one crossing the line: FinishRacer 0x606CB0 (thiscall race:
 * its place in the race's order, 0 the next place, 0, 1 a normal finish), one a frame */
#define G_FLOW        0x008654a4u
#define G_RACE        0x00890118u        /* the race: +0 state (4 racing), +0x24 racers, +0xBD0 their order, +0xBE8 laps */
#define FN_FINISH     0x00606cb0u
/* (g_ai_on, above: ours were added to the race being loaded or run) */
static void ai_finish_frame(void){
    if (!g_ai_on) return;
    uint32_t race = ok(G_RACE, 4) ? MR32(G_RACE) : 0;
    if (g_ai_race && (MR32(G_FLOW) != 6u || race != g_ai_race)){ g_ai_on = 0; g_ai_race = 0; return; }   /* (the race is over) */
    if (MR32(G_FLOW) != 6u || !race || !ok(race, 0xbf0) || MR32(race) != 4u) return;
    g_ai_race = race;
    uint32_t n = MR32(race + 0x24u), laps = MR32(race + 0xbe8u);
    if (!laps || laps > 20u || n > 8u) return;
    for (uint32_t i = 0; i < n; i++){
        uint32_t r = MR32(race + 0xbd0u + 4u * i), car = ok(r, 0x1d8) ? MR32(r) : 0;
        if (!ok(car, 0x30)) continue;
        uint16_t id = (uint16_t)MR16(car + 0xau);
        if (id < 100u || id > 131u || (g_ai_done >> (id - 100u) & 1u) || MR16(r + 0x10u)) continue;
        if (MR8(r + 9u) < laps && !(MRF32(r + 0x14u) >= (float)laps)) continue;
        g_ai_done |= 1u << (id - 100u);
        /* its time as FinishRacer's own cases give it (the world clock less the race's start, 0x606CF2) */
        if (!MR32(r + 0x198u) && ok(0x0086518cu, 4)) MR32(r + 0x198u) = MR32(0x0086518cu) - MR32(race + 0x10u);
        uint32_t a[4] = { i, 0, 0, 1 };
        roam_win_call(FN_FINISH, race, 0, 4, a);
        roam_log("[ai] an AI racer (car %04x) finished: %u laps (%.3f), its place %u, %.2f s, kept %u\n", id, laps, (double)MRF32(r + 0x14u),
                 MR8(r + 0xcu), (double)MR32(r + 0x198u) / 4000.0, MR16(r + 0x10u));
        return;
    }
}
/* roam_asi.c, each frame */
void airacers_win_frame(void){ ai_finish_frame(); }

/* The network's report of a racer's finish (0x602000, thiscall 0x89CF48 (car id, place, ..., how), ret 0x10): it looks the
 * network player up by the car id and tells the others; with none (al 0) the finish is taken back (FinishRacer
 * 0x606E8E, and the laps' and places' reports 0x607496, 0x607F7D, 0x608473, 0x6087B8 set +0x10 to 0 again).  An AI
 * racer of ours is no network player: its report succeeds at once, nothing sent (the second try, 05:10: every finish
 * of ours undone, every frame) */
#define REPORT_FN   0x00602000u          /* mov eax,[esp+4]; push ebx; push ebp */
static void report_install(void){
    static const uint8_t want[6] = { 0x8b, 0x44, 0x24, 0x04, 0x53, 0x55 };
    uint8_t *site = (uint8_t *)(uintptr_t)REPORT_FN, *t = g_page + 128;
    DWORD old;
    if (!ok(REPORT_FN, 6) || memcmp(site, want, 6)){ roam_log("[ai] the network's finish report (0x%08x) is not the game's: the AI racers may not finish\n", REPORT_FN); return; }
    static const uint8_t code[] = { 0x8b, 0x44, 0x24, 0x04,      /* mov eax, [esp+4]: the car id */
                                    0x83, 0xf8, 0x64,            /* cmp eax, 100 */
                                    0x7c, 0x05,                  /* jl +5: the game's */
                                    0xb0, 0x01,                  /* mov al, 1 */
                                    0xc2, 0x10, 0x00,            /* ret 0x10 */
                                    0x53, 0x55,                  /* push ebx; push ebp (the game's first bytes) */
                                    0xe9 };                      /* jmp REPORT_FN + 6 */
    memcpy(t, code, sizeof code);                                /* 0..16 */
    put_rel(t + 17, REPORT_FN + 6u);
    FlushInstructionCache(GetCurrentProcess(), t, 32);
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) return;
    site[0] = 0xe9; put_rel(site + 1, (uint32_t)(uintptr_t)t); site[5] = 0x90;
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    roam_log("[ai] the network's finish report (0x%08x): an AI racer's finish is its own\n", REPORT_FN);
}

/* Their names on the results screen.  Its row of a racer other than our own player (0x4BF3C0) takes, in a network
 * race (0x5F00F0), the name of the network player with the row's car id (0x5F0220 +0x12), else the racer record's
 * (+0x90C): the AI racers' rows had no name (the first try, 04:54).  So a car id of 100 and up (ours: RacerInfo's
 * index from the adder) takes the record's name, the network race's or not */
#define NAME_SITE   0x004bf3ceu          /* mov ecx,[ebp+4]; push ecx; mov ecx,0x89CF48; call 0x5F0220; add eax,0x12; push eax; jmp 0x4BF3ED */
#define NAME_NET    0x004bf3edu
#define NAME_REC    0x004bf3e2u          /* mov edx,[esp+0x18]; add edx,0x90C; push edx */
#define FN_NETPLAYER 0x005f0220u
static void names_install(void){
    static const uint8_t want[20] = { 0x8b, 0x4d, 0x04, 0x51, 0xb9, 0x48, 0xcf, 0x89, 0x00, 0xe8, 0x44, 0x0e, 0x13, 0x00, 0x83, 0xc0, 0x12, 0x50, 0xeb, 0x0b };
    uint8_t *site = (uint8_t *)(uintptr_t)NAME_SITE, *t = g_page + 64;
    DWORD old;
    if (!ok(NAME_SITE, 20) || memcmp(site, want, 20)){ roam_log("[ai] the results' names (0x%08x) are not the game's: the AI racers' rows have none\n", NAME_SITE); return; }
    static const uint8_t head[] = { 0x8b, 0x4d, 0x04,            /* mov ecx, [ebp+4]: the row's car id */
                                    0x83, 0xf9, 0x64,            /* cmp ecx, 100 */
                                    0x7d, 0x14,                  /* jge +20: the record's name */
                                    0x51,                        /* push ecx */
                                    0xb9, 0x48, 0xcf, 0x89, 0x00, /* mov ecx, 0x89CF48 */
                                    0xe8 };                      /* call 0x5F0220 */
    memcpy(t, head, sizeof head);                                /* 0..14 */
    put_rel(t + 15, FN_NETPLAYER);                               /* 15..18 */
    t[19] = 0x83; t[20] = 0xc0; t[21] = 0x12;                    /* add eax, 0x12 */
    t[22] = 0x50;                                                /* push eax */
    t[23] = 0xe9; put_rel(t + 24, NAME_NET);                     /* 23..27: jmp 0x4BF3ED */
    t[28] = 0xe9; put_rel(t + 29, NAME_REC);                     /* 28..32: jmp 0x4BF3E2 */
    FlushInstructionCache(GetCurrentProcess(), t, 40);
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    site[0] = 0xe9; put_rel(site + 1, (uint32_t)(uintptr_t)t);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    roam_log("[ai] the results' names (0x%08x): an AI racer's row has its name\n", NAME_SITE);
}
static void put_rel(uint8_t *at, uint32_t target){ int32_t r = (int32_t)(target - ((uint32_t)(uintptr_t)at + 4u)); memcpy(at, &r, 4); }
/* roam_asi.c, at the start (with the plugin on) */
void airacers_win_install(void){
    static int done;
    static const uint8_t want[6] = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0 };   /* push ebp; mov ebp, esp; and esp, -16 */
    uint8_t *site = (uint8_t *)(uintptr_t)CARS_FN;
    DWORD old;
    if (done) return;
    done = 1;
    if (!opt_num("NFSU2_ROAM_AIRACERS", 4, 0, 4)){ roam_log("[ai] AiRacers = 0: no AI racers in multiplayer races\n"); return; }
    if (!ok(CARS_FN, 6) || memcmp(site, want, 6)){ roam_log("[ai] the race's car making (0x%08x) is not the game's (another mod?): no AI racers\n", CARS_FN); return; }
    if (!VirtualProtect(g_page, sizeof g_page, PAGE_EXECUTE_READWRITE, &old)) return;
    uint8_t *t = g_page;
    memcpy(t, site, 6);
    t[6] = 0xe9; put_rel(t + 7, CARS_FN + 6u);
    FlushInstructionCache(GetCurrentProcess(), t, 16);
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) return;
    site[0] = 0xe9; put_rel(site + 1, (uint32_t)(uintptr_t)hk_cars); site[5] = 0x90;
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    g_cars = (cars_t)(void *)t;
    roam_log("[ai] the race's car making (0x%08x) goes through the plugin: AI racers in LAN races\n", CARS_FN);
    names_install();
    report_install();
}
