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
 * still has 6 places: AiRacers (2 by default, the frame rate the player asked for; up to 4, 0 off) is cut to what is free; they are Claude, ChatGPT, Gemini and
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
static uint32_t g_ai_n0, g_ai_added;      /* the race's records before ours, and how many we added */
static uint32_t g_trim_recs, g_trim_ai;   /* taken off the records' and the AI count at the results (hk_results puts them back
                                            * while the screen makes its rows) */
static void results_install(void);
static void preload_install(void);
static void report_install(void);
static void names_install(void);
static void put_rel(uint8_t *at, uint32_t target);
typedef void (__attribute__((thiscall)) *cars_t)(uint32_t);
static cars_t g_cars;
static uint8_t g_page[4096] __attribute__((aligned(4096)));

static void ai_add(void){
    int want = opt_num("NFSU2_ROAM_AIRACERS", 2, 0, 4), skill = opt_num("NFSU2_ROAM_AISKILL", 9, 0, 9);
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
    if (ai){
        /* ours, added before the cars' loading (ai_preload) when the players had no grid place yet, took places 1 and 2
         * (09:45: the race then had the two players only, "2nd /2", no AI): here the players' places are set, so ours
         * get the free ones, in order */
        int fp[GRID_N], nf = 0, k = 0, moved = 0;
        for (int p = 1; p <= GRID_N; p++){
            int u = 0;
            for (uint32_t i = 0; i < n0; i++){ uint32_t r = recs + i * REC_SZ; if (MR32(r + REC_KIND) == 1u && MR8(r + REC_GRID) == (uint32_t)p) u = 1; }
            if (!u) fp[nf++] = p;
        }
        for (uint32_t i = 0; i < n0 && k < nf; i++){
            uint32_t r = recs + i * REC_SZ;
            if (MR32(r + REC_KIND) != 2u || MR8(r + 1u) < 100u) continue;
            if (MR8(r + REC_GRID) != (uint32_t)fp[k]){ MR8(r + REC_GRID) = (uint8_t)fp[k]; moved++; }
            k++;
        }
        roam_log("[ai] the race has %d AI racer(s) already: none added; %d of ours moved to the grid's free places\n", ai, moved);
        for (uint32_t i = 0; i < n0; i++){
            uint32_t r = recs + i * REC_SZ;
            roam_log("[ai]   record %u: kind %u, grid place %u, id %u, car type %u\n", i, MR32(r + REC_KIND), MR8(r + REC_GRID), MR8(r + 1u), MR32(r + 0x10u));
        }
        return;
    }
    int freep[GRID_N], nfree = 0;
    for (int p = 1; p <= GRID_N; p++) if (!used[p]) freep[nfree++] = p;
    int n = want < nfree ? want : nfree;
    if (n > REC_MAX - (int)n0) n = REC_MAX - (int)n0;
    if (n <= 0){ roam_log("[ai] no free place on the grid (%d player(s)): no AI racers\n", humans); return; }
    /* the game's random numbers from the lobby (the same on every game of it), as they were after */
    uint32_t rnd = MR32(G_RAND), lid = roam_game_lobby_id();
    uint8_t noai = MR8(G_NOAI); MR8(G_NOAI) = 0;
    /* one at a time, each kept only with one of the game's own cars (its ride's type, +0x10, below 31: the 31 player
     * cars).  An Unlimiter add-on car as an AI opponent stalled the host's race load for good (06:03: an AI's
     * SENTRAB12, the 7.7 MB model that also never loads as a LAN opponent, item 7; the laptop lost the host), and the
     * add-ons' models are the heavy ones for the frame rate.  Each try its own seed from the lobby (the same tries on
     * every game), 12 at most; each one kept gets its racer index (+1: the car's id) 100 and up, one after another */
    int kept = 0;
    for (int tryn = 0; tryn < 12 && kept < n; tryn++){
        MR32(G_RAND) = ((lid ? lid : 0x5eed1234u) ^ 0x9e3779b9u) + (uint32_t)tryn * 0x85ebca6bu;
        uint32_t c0 = MR32(G_NRECS), a = 1;
        roam_win_call(FN_ADD_AI, 0, 0, 1, &a);
        recs = MR32(G_RECS);
        uint32_t c1 = MR32(G_NRECS);
        if (c1 != c0 + 1u || c1 > REC_MAX || !ok(recs, REC_SZ * c1)){ roam_log("[ai] the game's AI adder left %u records (was %u): stopped\n", c1, c0); break; }
        uint32_t r = recs + c0 * REC_SZ, type = MR32(r + 0x10u);
        if (type >= 31u){
            MR32(G_NRECS) = c0;                                           /* (not kept) */
            roam_log("[ai]   an AI racer's car not kept: type %u (not one of the game's own cars)\n", type);
            continue;
        }
        MR8(r + 1u) = (uint8_t)(100 + kept);
        kept++;
    }
    MR8(G_NOAI) = noai;
    MR32(G_RAND) = rnd;
    recs = MR32(G_RECS);
    uint32_t n1 = MR32(G_NRECS);
    if (n1 > REC_MAX || n1 < n0 || !ok(recs, REC_SZ * n1)){ roam_log("[ai] the race's records are %u (were %u): not touched\n", n1, n0); return; }
    for (uint32_t i = 0; i < n0; i++){ MR8(recs + i * REC_SZ) = keep0[i]; MR8(recs + i * REC_SZ + REC_GRID) = keep3[i]; }
    int k = 0;
    for (uint32_t i = n0; i < n1 && k < nfree; i++, k++){
        uint32_t r = recs + i * REC_SZ;
        MR8(r) = 0; MR8(r + REC_GRID) = (uint8_t)freep[k]; MR8(r + REC_SKILL) = (uint8_t)skill;
        /* their names (the player's pick, 2026-11-07) */
        /* (a new pick each race, fix phase 8: the 4 in an order from the lobby, the same on every PC of the race; racer
         * 1 was always Claude, 2 always ChatGPT) */
        static const char *const k_names[4] = { "Claude", "ChatGPT", "Gemini", "DeepSeek" };
        int order[4] = { 0, 1, 2, 3 };
        uint32_t sd = (lid ? lid : 0x5eed1234u) * 2654435761u + 0x9e3779b9u;
        for (int z = 3; z > 0; z--){ sd = sd * 1664525u + 1013904223u; int y = (int)((sd >> 16) % (uint32_t)(z + 1)); int tmp = order[z]; order[z] = order[y]; order[y] = tmp; }
        const char *pick = k_names[order[k & 3]];
        for (int q = 0; q < 16; q++) MR8(r + REC_NAME + (uint32_t)q) = 0;
        for (int q = 0; pick[q] && q < 15; q++) MR8(r + REC_NAME + (uint32_t)q) = (uint8_t)pick[q];
        char nm[17]; int j = 0;
        for (; j < 16 && MR8(r + REC_NAME + (uint32_t)j) >= 0x20 && MR8(r + REC_NAME + (uint32_t)j) < 0x7f; j++) nm[j] = (char)MR8(r + REC_NAME + (uint32_t)j);
        nm[j] = 0;
        uint32_t ty = MR32(r + 0x10u), tys = ok(0x008a1cccu, 4) ? MR32(0x008a1cccu) : 0, tn = tys && ty < 64u ? tys + ty * 0x890u : 0;
        char cn[16]; int q2 = 0;
        for (; tn && ok(tn, 16) && q2 < 15 && MR8(tn + (uint32_t)q2) >= 0x20 && MR8(tn + (uint32_t)q2) < 0x7f; q2++) cn[q2] = (char)MR8(tn + (uint32_t)q2);
        cn[q2] = 0;
        roam_log("[ai]   AI racer %d: %s, grid place %d, skill %d, car %s (type %u), car id %u\n", k + 1, nm[0] ? nm : "(no name)", freep[k], skill, cn, ty, MR8(r + 1u));
    }
    MR32(G_AI_N) += (uint32_t)(n1 - n0);
    if (n1 > n0){ g_ai_on = 1; g_ai_race = 0; g_ai_done = 0; g_ai_n0 = n0; g_ai_added = n1 - n0; }
    roam_log("[ai] the LAN race: %u AI racer(s) beside %d player(s) (AiRacers %d, AiSkill %d, difficulty %u, lobby %08x)\n",
             n1 - n0, humans, want, skill, MR32(G_DIFFICULTY), lid);
}
static void __attribute__((thiscall)) hk_cars(uint32_t self){
    g_trim_recs = g_trim_ai = 0;                 /* (a new race's cars: the last one's results are done) */
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
/* ---- the AI racers rise when they fall behind (fix phase 8, Rishi's rules, 2026-11-07) -----------------------------
 * Only while a human leads the race (a LAN race with ours in it).  The gap: a racer's progress is its record's +0x14
 * (laps, the whole race: 1.0 at the end of lap 1, seen in a career circuit 12:38), and an AI racer's gap to the leading
 * human is the progress between them over that human's pace (laps a second, over the last 5 s): the time it needs to
 * be where the leader is now.  An AI racer 15 s or more behind gets a set (acceleration +10 %, top speed +10 %, grip
 * +15 %); then every 5 s, one that did not close at least 2 s on the leader gets another (no most).  One that overtakes
 * the leading human gets no more and keeps its sets; fallen back to 15 s behind again (rammed), the sets go on.  At the
 * race's end (its state no longer racing, the world still up) every car is as it was.  The sets add up: n sets are
 * +10n % torque (the engine reads its curve +0x230 and nitrous +0x2DC in place, 0x594300), the drag +0x290 over
 * (1 + 0.1n)^2 (the top speed is where drag meets the push; the physics reads it live, 0x59A39C), and the 4 wheels'
 * grip factor (car +0x34 its physics object, +0x38..+0x44 its wheels, +0x188 the tyres' force factor, 1 normally,
 * 0x59D600) 1 + 0.15n.  The race balance (mprace.c) is told of each new curve (mprace_balance_touch), so it does not
 * take it for a car built again */
void mprace_balance_touch(uint32_t car);
#define RS_PROG      0x14u
#define RS_DONE      0x10u
#define CAR_ID       0x0au
#define CAR_RIDE     0x10u
#define CAR_PHYSOBJ  0x34u
#define WH_GRIP      0x188u
#define PI_TORQUE    0x230u
#define PI_DRAG      0x290u
#define PI_NITROUS   0x2dcu
#define RISE_N       6
typedef struct {
    uint32_t car, pi; int sets, armed, passed, based; float t[9], nos, drag, gap_ref, t0_set; DWORD check_at;
} Rise;
static Rise g_rise[RISE_N];
static int g_rise_n; static uint32_t g_rise_race;
static float g_lead_hist[8]; static DWORD g_lead_hist_at[8]; static int g_lead_hist_i; static DWORD g_lead_sample_at;
static uint32_t rise_pi(uint32_t car){ uint32_t r = ok(car, 0x14) ? MR32(car + CAR_RIDE) : 0; return ok(r + 0x10u, PI_NITROUS + 4u) ? r + 0x10u : 0; }
static void rise_wheels(uint32_t car, float g){
    uint32_t ph = ok(car, CAR_PHYSOBJ + 4u) ? MR32(car + CAR_PHYSOBJ) : 0;
    if (!ok(ph, 0x48)) return;
    for (uint32_t w = 0; w < 4u; w++){ uint32_t wh = MR32(ph + 0x38u + 4u * w); if (ok(wh, WH_GRIP + 4u)) MRF32(wh + WH_GRIP) = g; }
}
static void rise_apply(Rise *b){
    uint32_t pi = rise_pi(b->car);
    if (!pi) return;
    if (!b->based || MRF32(pi + PI_TORQUE) != b->t0_set){              /* (first, or the car built again: its curve anew) */
        for (int i = 0; i < 9; i++) b->t[i] = MRF32(pi + PI_TORQUE + 4u * (uint32_t)i);
        b->nos = MRF32(pi + PI_NITROUS); b->drag = MRF32(pi + PI_DRAG); b->pi = pi; b->based = 1;
    }
    float f = 1.f + 0.10f * (float)b->sets, g = 1.f + 0.15f * (float)b->sets;
    for (int i = 0; i < 9; i++) MRF32(pi + PI_TORQUE + 4u * (uint32_t)i) = b->t[i] * f;
    MRF32(pi + PI_NITROUS) = b->nos * f;
    MRF32(pi + PI_DRAG) = b->drag / (f * f);
    b->t0_set = MRF32(pi + PI_TORQUE);
    rise_wheels(b->car, g);
    mprace_balance_touch(b->car);
}
static void rise_undo(void){
    for (int k = 0; k < g_rise_n; k++){
        Rise *b = &g_rise[k];
        if (!b->based || !b->sets) continue;
        uint32_t pi = rise_pi(b->car);
        if (pi && pi == b->pi){
            for (int i = 0; i < 9; i++) MRF32(pi + PI_TORQUE + 4u * (uint32_t)i) = b->t[i];
            MRF32(pi + PI_NITROUS) = b->nos; MRF32(pi + PI_DRAG) = b->drag;
            rise_wheels(b->car, 1.f);
            mprace_balance_touch(b->car);
        }
    }
    if (g_rise_n) roam_log("[ai] rising: the race is over, the AI racers' sets taken off\n");
    g_rise_n = 0; g_rise_race = 0;
}
static void rise_frame(void){
    uint32_t race = ok(G_RACE, 4) ? MR32(G_RACE) : 0;
    int racing = MR32(G_GAMEMODE) == 0x200u && MR32(G_FLOW) == 6u && race && ok(race, 0xbf0) && MR32(race) == 4u;
    if (!racing){
        if (g_rise_race && MR32(G_FLOW) == 6u && race == g_rise_race) rise_undo();      /* (over, the world still up) */
        else if (g_rise_race){ g_rise_n = 0; g_rise_race = 0; }                        /* (the world gone: its cars too) */
        return;
    }
    if (race != g_rise_race){ g_rise_race = race; g_rise_n = 0; g_lead_hist_i = 0; g_lead_sample_at = 0; for (int i = 0; i < 8; i++) g_lead_hist_at[i] = 0; }
    uint32_t n = MR32(race + 0x24u);
    if (n > 8u) return;
    float lead_h = -1.f, lead_all = -1.f; int lead_is_ai = 0;
    for (uint32_t i = 0; i < n; i++){
        uint32_t r = MR32(race + 0xbd0u + 4u * i), car = ok(r, 0x18) ? MR32(r) : 0;
        if (!ok(car, 0x30)) continue;
        float p = MRF32(r + RS_PROG); uint16_t id = (uint16_t)MR16(car + CAR_ID);
        if (!(p > -1.f && p < 100.f)) continue;
        if (id < 100u && p > lead_h) lead_h = p;
        if (p > lead_all){ lead_all = p; lead_is_ai = id >= 100u; }
    }
    DWORD now = GetTickCount();
    if (lead_h < 0.f) return;
    if (now - g_lead_sample_at >= 1000u){                                             /* the leader's pace: 5 s back */
        g_lead_sample_at = now;
        g_lead_hist[g_lead_hist_i & 7] = lead_h; g_lead_hist_at[g_lead_hist_i & 7] = now; g_lead_hist_i++;
    }
    int back = g_lead_hist_i - 6; if (back < 0) return;
    float p5 = g_lead_hist[back & 7]; DWORD t5 = g_lead_hist_at[back & 7];
    float rate = t5 && now > t5 ? (lead_h - p5) / ((float)(now - t5) / 1000.f) : 0.f;   /* laps a second */
    if (!(rate > 0.0005f)) return;                                                    /* (the leader stands: no gap to tell) */
    for (uint32_t i = 0; i < n; i++){
        uint32_t r = MR32(race + 0xbd0u + 4u * i), car = ok(r, 0x18) ? MR32(r) : 0;
        if (!ok(car, 0x30) || (uint16_t)MR16(car + CAR_ID) < 100u || MR16(r + RS_DONE)) continue;
        Rise *b = 0;
        for (int k = 0; k < g_rise_n; k++) if (g_rise[k].car == car) b = &g_rise[k];
        if (!b){ if (g_rise_n >= RISE_N) continue; b = &g_rise[g_rise_n++]; memset(b, 0, sizeof *b); b->car = car; }
        unsigned cid = (unsigned)MR16(car + CAR_ID);
        float p = MRF32(r + RS_PROG), gap = (lead_h - p) / rate;
        /* test control (NFSU2_ROAM_TEST_RISE=1): every AI racer as if 20 s behind a human leader and closing nothing,
         * so a set every 5 s (6 at most): the sets' effect on the road, its speed in each line */
        static int test = -1;
        if (test < 0){ const char *e = roam_opt("NFSU2_ROAM_TEST_RISE"); test = e && *e == '1'; if (test) roam_log("[ai] rising: TEST, every AI racer 20 s behind, a set every 5 s\n"); }
        if (test){ if (b->sets >= 6) continue; p = lead_h - 1.f; gap = b->armed ? b->gap_ref : 20.f; lead_is_ai = 0; }
        if (b->sets && b->based && rise_pi(car) && MRF32(rise_pi(car) + PI_TORQUE) != b->t0_set) rise_apply(b);   /* (built again) */
        if (p > lead_h){
            if (b->armed && !b->passed) roam_log("[ai] rising: AI racer %u overtook the leading human with %d set(s): no more, kept\n", cid, b->sets);
            b->passed = 1; b->armed = 0; continue;
        }
        if (lead_is_ai) continue;                                                     /* only while a human leads */
        if (b->passed){
            if (gap < 15.f) continue;
            b->passed = 0; roam_log("[ai] rising: AI racer %u fell back %.0f s behind: the sets go on\n", cid, (double)gap);
        }
        if (!b->armed){
            if (gap < 15.f) continue;
            b->armed = 1; b->sets++; b->gap_ref = gap; b->check_at = now + 5000u;
            rise_apply(b);
            roam_log("[ai] rising: AI racer %u is %.1f s behind the leading human: set %d (acceleration and top speed +%d %%, grip +%d %%)\n",
                     cid, (double)gap, b->sets, 10 * b->sets, 15 * b->sets);
        } else if ((int)(now - b->check_at) >= 0){
            if (b->gap_ref - gap < 2.f){
                b->sets++; rise_apply(b);
                uint32_t pi = rise_pi(car);
                roam_log("[ai] rising: AI racer %u closed %.1f s in 5 s (%.1f s behind): set %d (%.0f km/h; torque at the top %.0f N m, drag %.4f, grip %.2f)\n", cid,
                         (double)(b->gap_ref - gap), (double)gap, b->sets, (double)(MRF32(car + 0x42cu) * 3.6f),
                         pi ? (double)(MRF32(pi + PI_TORQUE + 32u) * 1000.f) : 0.0, pi ? (double)MRF32(pi + PI_DRAG) : 0.0, 1.0 + 0.15 * b->sets);
            }
            b->gap_ref = gap; b->check_at = now + 5000u;
        }
    }
}
void airacers_win_frame(void){ ai_finish_frame(); rise_frame(); }

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

/* The results, in the order the race was run.  A network race's places are the network's: each player's game
 * reports its own finish, and an AI racer of ours gets the next free place as it crosses the line, so the rows came
 * mixed (the third try, 05:44: an AI 1st at 102.8 s, the laptop 2nd at 133.4 s, ours 3rd at 159.1 s, then three AI
 * at 109.7, 131.0 and 132.0 s).  So as the results screen makes its list (0x4D7240, thiscall screen, from 0x4E9697),
 * and as the trip reads them (mprace.c results_read), the rows (0x851044, [0x85105C] of them) are put in order: the
 * finished (as the trip counts them: done, +8, and the race's laps, +0xC8 against the race's +0xBE8) by their time
 * (+0xC), then the rest by how far they came; each row's place (+0) its new one.  Only in a race with ours in it.
 * And the race's records are the players' again there (0x89E814 back to its count before ours, the AI count with
 * it): the host's next session after the race ("Starting Game Session...", the results' Continue) waited for good,
 * all three tries with AI racers, the AI finished or not */
#define G_RESULTS     0x00851044u
#define G_RESULTS_N   0x0085105cu
#define RESULTS_FN    0x004d7240u        /* mov eax, [0x89E7A0] (5 bytes) */
static int row_done(uint32_t e, uint32_t laps){ return MR32(e + 8u) != 0 && laps && MRF32(e + 0xc8u) >= (float)laps - 0.01f; }
void airacers_win_sort_results(void){
    if (!g_ai_on) return;
    if (g_ai_added && MR32(G_NRECS) == g_ai_n0 + g_ai_added){
        MR32(G_NRECS) = g_ai_n0; g_trim_recs = g_ai_added; g_trim_ai = 0;
        if (MR32(G_AI_N) >= g_ai_added){ MR32(G_AI_N) -= g_ai_added; g_trim_ai = g_ai_added; }
        roam_log("[ai] the results: the race's records are the %u player(s)' again (its next session waits for them only)\n", g_ai_n0);
        g_ai_added = 0;
    }
    uint32_t n = MR32(G_RESULTS_N), race = MR32(G_RACE), laps = race && ok(race, 0xbf0) ? MR32(race + 0xbe8u) : 0, e[8];
    if (n < 2 || n > 8) return;
    for (uint32_t i = 0; i < n; i++){ e[i] = MR32(G_RESULTS + 4u * i); if (!ok(e[i], 0x114)) return; }
    int moved = 0;
    for (uint32_t i = 1; i < n; i++){
        uint32_t x = e[i]; int j = (int)i - 1;
        for (; j >= 0; j--){
            uint32_t y = e[j]; int dx = row_done(x, laps), dy = row_done(y, laps);
            int before = dx != dy ? dx > dy : dx ? MR32(x + 0xcu) < MR32(y + 0xcu) : MRF32(x + 0xc8u) > MRF32(y + 0xc8u);
            if (!before) break;
            e[j + 1] = y; moved = 1;
        }
        e[j + 1] = x;
    }
    if (!moved) return;
    for (uint32_t i = 0; i < n; i++){ MR32(G_RESULTS + 4u * i) = e[i]; MR32(e[i]) = i + 1u; }
    roam_log("[ai] the results put in the race's order (the finished by their times, then the rest)\n");
    for (uint32_t i = 0; i < n; i++)
        roam_log("[ai]   %u: car %04x, %s, %.2f s, %.3f laps\n", i + 1u, MR32(e[i] + 4u) & 0xffffu, row_done(e[i], laps) ? "finished" : "not finished",
                 (double)MR32(e[i] + 0xcu) / 4000.0, (double)MRF32(e[i] + 0xc8u));
}
typedef void (__attribute__((thiscall)) *results_t)(uint32_t);
static results_t g_results;
/* The screen's rows (0x4BF290, called from 0x4D7240) are as many as the race's racers of every kind ([0x89E7C0] +
 * [0x89E7C4], the AI, + [0x89E7C8] + [0x89E7CC]): with the AI count already back to the players' (the trip's
 * results_read can come first), only the first rows were filled and the rest kept the screen's own placeholders
 * (09:18, the desktop host: two rows "WWWWWWWWWWWWWWWW", a Lancer EVO VIII and an Escalade, place 6, "Opgeblazen
 * motor", where the two players were 3rd and 4th behind the AI).  So while the game makes the list, the counts are
 * the race's again, and the players' after */
static void __attribute__((thiscall)) hk_results(uint32_t self){
    airacers_win_sort_results();
    uint32_t nr = MR32(G_NRECS), na = MR32(G_AI_N);
    int put = (g_trim_recs || g_trim_ai) && nr == g_ai_n0;
    if (put){ MR32(G_NRECS) = nr + g_trim_recs; MR32(G_AI_N) = na + g_trim_ai; }
    g_results(self);
    if (put){ MR32(G_NRECS) = nr; MR32(G_AI_N) = na; }
}
static void results_install(void){
    static const uint8_t want[5] = { 0xa1, 0xa0, 0xe7, 0x89, 0x00 };
    uint8_t *site = (uint8_t *)(uintptr_t)RESULTS_FN, *t = g_page + 192;
    DWORD old;
    if (!ok(RESULTS_FN, 5) || memcmp(site, want, 5)){ roam_log("[ai] the results' list (0x%08x) is not the game's: in the network's order\n", RESULTS_FN); return; }
    memcpy(t, site, 5);
    t[5] = 0xe9; put_rel(t + 6, RESULTS_FN + 5u);
    FlushInstructionCache(GetCurrentProcess(), t, 16);
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    site[0] = 0xe9; put_rel(site + 1, (uint32_t)(uintptr_t)hk_results);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    g_results = (results_t)(void *)t;
    roam_log("[ai] the results' list (0x%08x): in the race's order\n", RESULTS_FN);
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
/* Their cars drawn.  The race's cars' models are asked of the car loader (0x8A3288, 0x63BC70 (ride, human, ...)) by a
 * loop over the race's records ([0x89E814]; 0x57F673..0x57F6F0, in the loading stages 0x57F2D0: each one's handle at
 * +0x8F8), before the track load makes the cars (0x6065B0).  The AI racers added there had no model asked for: they
 * raced, unseen (Rishi, 09:32: two Corollas 0.4 s ahead at 2.75 s, nothing on the road).  So in a LAN race they are
 * added just before that loop (its first instruction, mov eax,[0x89E814], goes to a stub that calls ai_preload and
 * does it), and the loop asks for their cars as for the players'; at 0x6065B0 the race has them already */
#define PRELOAD_SITE 0x0057f673u         /* mov eax, [0x89E814] (5 bytes), then xor edi, edi */
static void __attribute__((cdecl, used)) ai_preload(void){
    if (MR32(G_GAMEMODE) != 0x200u) return;
    g_trim_recs = g_trim_ai = 0;
    uint32_t n0 = MR32(G_NRECS);
    ai_add();
    if (MR32(G_NRECS) != n0) roam_log("[ai] the AI racers added before the race's cars are loaded (%u records -> %u): their cars load with the players'\n", n0, MR32(G_NRECS));
}
static void preload_install(void){
    static const uint8_t want[7] = { 0xa1, 0x14, 0xe8, 0x89, 0x00, 0x33, 0xff };
    uint8_t *site = (uint8_t *)(uintptr_t)PRELOAD_SITE, *t = g_page + 256;
    DWORD old;
    if (!ok(PRELOAD_SITE, 7) || memcmp(site, want, 7)){ roam_log("[ai] the race's car loading (0x%08x) is not the game's: the AI racers' cars may not be drawn\n", PRELOAD_SITE); return; }
    t[0] = 0x60; t[1] = 0x9c;                                    /* pushad; pushfd */
    t[2] = 0xe8; put_rel(t + 3, (uint32_t)(uintptr_t)ai_preload); /* call ai_preload */
    t[7] = 0x9d; t[8] = 0x61;                                    /* popfd; popad */
    memcpy(t + 9, site, 5);                                      /* mov eax, [0x89E814] */
    t[14] = 0xe9; put_rel(t + 15, PRELOAD_SITE + 5u);            /* jmp back */
    FlushInstructionCache(GetCurrentProcess(), t, 32);
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    site[0] = 0xe9; put_rel(site + 1, (uint32_t)(uintptr_t)t);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    roam_log("[ai] the race's car loading (0x%08x): the AI racers are added before it, so their cars are drawn\n", PRELOAD_SITE);
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
    if (!opt_num("NFSU2_ROAM_AIRACERS", 2, 0, 4)){ roam_log("[ai] AiRacers = 0: no AI racers in multiplayer races\n"); return; }
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
    results_install();
    preload_install();
}
