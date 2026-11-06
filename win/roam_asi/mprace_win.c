/* mprace_win.c -- the rest of what runtime/mprace.c needs in the Windows plugin (mprace_host_win.h has the names):
 *
 *  - its log (fprintf(stderr) pieces gathered into the plugin's log lines);
 *  - logf, expf, powf, floorf for the cars' evening out (double-precision series: within an ulp of a C library's
 *    single-precision results, which is all the evening out needs: each game evens out its own car only);
 *  - the front end's screens as the port's autotest.c / career.c read them (the FE manager 0x8384C4: +8 the package
 *    manager, its lists at +0xE4 .. +0xF4, a package's name at +0xC), the key taps (roam_keys.c), the game's car
 *    database lookup (garage.c's, the game's 91 records only: no extra slots here);
 *  - a vtable entry patched (UI_Pause's handler, mprace.c pause_msg_win);
 *  - stand-ins for the browser's alone: its pause menu's Save (savegame.c), its race statistics (racenet.c), the
 *    quit datagrams to the race's peers (net_lan.c);
 *  - the career credit of a multiplayer win (the port's mpcredit.c, its core: the event entry, the prize and the
 *    margin, RaceWon, the open races again), in the crib on the way back.  Not here: the sponsor offers after such a
 *    win and the race's movie (they need the port's own texts in the game's language table); the game offers the
 *    sponsors again after the player's next Solo race of the stage.  A win in free roam waits for the next crib in
 *    memory (not across a restart). */
#include "mprace_host_win.h"
#include "mprace.h"
#include "mpcredit.h"
#include "racenet.h"
#include <stdarg.h>

#define WIN32_LEAN_AND_MEAN
typedef unsigned long DWORD;
int __stdcall VirtualProtect(void *a, unsigned long n, DWORD prot, DWORD *old);
#define PAGE_EXECUTE_READWRITE 0x40

/* ---- the log ---- */
static char g_lb[1100]; static int g_ln;
void mp_logc(int ch){
    if (ch == '\n'){ g_lb[g_ln] = 0; if (g_ln) roam_log("%s\n", g_lb); g_ln = 0; return; }
    if (g_ln < (int)sizeof g_lb - 1) g_lb[g_ln++] = (char)ch;
}
void mp_logf(const char *fmt, ...){
    char b[1100]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
    for (int i = 0; i < n; i++) mp_logc(b[i]);
}

/* ---- libm ---- */
static double d_log(double x){
    /* x > 0: x = m 2^e with m in [sqrt(1/2), sqrt(2)); log m = 2 atanh((m - 1) / (m + 1)) */
    union { double d; uint64_t u; } v = { x };
    int e = (int)((v.u >> 52) & 0x7ff) - 1023;
    v.u = (v.u & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
    double m = v.d;
    if (m > 1.4142135623730951){ m *= 0.5; e++; }
    double s = (m - 1.0) / (m + 1.0), s2 = s * s, t = s, sum = 0.0;
    for (int k = 1; k < 30; k += 2){ sum += t / k; t *= s2; }
    return 2.0 * sum + (double)e * 0.6931471805599453;
}
static double d_exp(double x){
    if (x > 709.0) x = 709.0;
    if (x < -745.0) return 0.0;
    double kf = x * 1.4426950408889634; int k = (int)(kf < 0 ? kf - 0.5 : kf + 0.5);
    double r = x - (double)k * 0.6931471805599453, term = 1.0, sum = 1.0;
    for (int i = 1; i < 20; i++){ term *= r / i; sum += term; }
    union { double d; uint64_t u; } p;
    int bk = k + 1023;
    if (bk <= 0){ p.u = (uint64_t)1 << 52 >> (1 - bk); if (!p.u) return 0.0; return sum * p.d; }   /* (subnormal: not reached here) */
    p.u = (uint64_t)bk << 52;
    return sum * p.d;
}
float mp_logf_(float x){
    if (!(x > 0.f)) return x == 0.f ? -__builtin_inff() : __builtin_nanf("");
    if (x == __builtin_inff()) return x;
    return (float)d_log((double)x);
}
float mp_expf(float x){
    if (x != x) return x;
    if (x > 88.8f) return __builtin_inff();
    return (float)d_exp((double)x);
}
float mp_powf(float x, float y){
    if (y == 0.f) return 1.f;
    if (x == 1.f) return 1.f;
    if (x == 0.f) return y > 0.f ? 0.f : __builtin_inff();
    if (x < 0.f){
        /* (an integer power only; the evening out never asks for one) */
        int n = (int)y; if ((float)n != y) return __builtin_nanf("");
        float r = (float)d_exp((double)y * d_log(-(double)x));
        return (n & 1) ? -r : r;
    }
    return (float)d_exp((double)y * d_log((double)x));
}
float mp_floorf(float x){
    if (!(x == x) || x >= 8388608.f || x <= -8388608.f) return x;
    int i = (int)x; float f = (float)i;
    return f > x ? f - 1.f : f;
}

/* ---- the front end's screens ---- */
static uint32_t rd(uint32_t a){ return a && roam_win_committed(a) && roam_win_committed(a + 3) ? MR32(a) : 0; }
static uint32_t fe_mgr(void){
    uint32_t fe = rd(0x008384c4u);
    if (!fe || !roam_win_committed(fe + 11)) return 0;
    uint32_t mg = rd(fe + 8u);
    return mg && roam_win_committed(mg + 0xf7u) ? mg : 0;
}
static const char *pkg_name(uint32_t p){
    uint32_t n = rd(p + 0xcu);
    if (!n || !roam_win_committed(n) || !roam_win_committed(n + 47)) return 0;
    const char *q = (const char *)GPTR(n); int k = 0;
    while (k < 47 && q[k] >= 0x20 && q[k] < 0x7f) k++;
    return (k > 4 && !q[k]) ? q : 0;
}
int career_fe_active(const char *name){
    uint32_t mg = fe_mgr();
    if (!mg) return 0;
    for (uint32_t head = 0xe4u; head <= 0xf4u; head += 0x10u){
        uint32_t p = rd(mg + head);
        for (int k = 0; p && k < 64 && roam_win_committed(p) && roam_win_committed(p + 0x10); k++){
            const char *nm = pkg_name(p);
            if (nm && !strcasecmp(nm, name)) return 1;
            p = rd(p + 4u);
        }
    }
    return 0;
}
const char *autotest_top_fng(void){
    uint32_t mg = fe_mgr(), pk = mg ? rd(mg + 0xe4u) : 0;
    if (!pk) return 0;
    const char *s = pkg_name(pk);
    if (!s) return 0;
    int n = (int)strlen(s);
    return n >= 5 && !strncasecmp(s + n - 4, ".fng", 4) ? s : 0;
}
long autotest_frame(void){ return roam_win_frame_no(); }
void roam_keys_tap(unsigned dik, int hold);
void autotest_tap(unsigned dik, long frames){ roam_keys_tap(dik, (int)frames); }

/* ---- the game's car database (FEDatabase): its 91 records' table at +0x9BD8, a record's handle at +8 ---- */
uint32_t garage_find(uint32_t db, uint32_t h){
    if (!h) return 0;
    for (uint32_t i = 0; i < 91u; i++){ uint32_t r = rd(db + 0x9bd8u + 4u * i); if (r && rd(r + 8u) == h) return r; }
    return 0;
}

/* ---- a vtable entry: in the first 64 entries of vt, the one that is `want` becomes fn (1 done) ---- */
int roam_win_vtpatch(uint32_t vt, uint32_t want, uint32_t fn){
    for (uint32_t i = 0; i < 64; i++){
        uint32_t *slot = (uint32_t *)GPTR(vt + 4u * i);
        if (!roam_win_committed(vt + 4u * i) || *slot != want) continue;
        DWORD old;
        if (!VirtualProtect(slot, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
        *slot = fn;
        VirtualProtect(slot, 4, old, &old);
        roam_log("[mprace] vtable 0x%08x entry %u: 0x%08x now goes through the plugin (0x%08x)\n", vt, i, want, fn);
        return 1;
    }
    roam_log("[mprace] vtable 0x%08x: no entry 0x%08x (another mod?): not patched\n", vt, want);
    return 0;
}

/* ---- the browser's alone ---- */
int savegame_busy(void){ return 0; }
float racenet_jump(const char *name){ (void)name; return 0.f; }
float racenet_farthest(char name[16]){ name[0] = 0; return 0.f; }
int vlan_race_peers(uint32_t *ip_h, int max, uint32_t within_ms){ (void)ip_h; (void)max; (void)within_ms; return 0; }
int vlan_rt_send(uint16_t port, uint32_t ip_h, const void *p, int n){ (void)port; (void)ip_h; (void)p; (void)n; return 0; }
void mprace_quit_heard(uint8_t *m, int n, uint32_t from);

/* ---- the career credit (the port's mpcredit.c, its core) ---- */
#define G_CAREER        0x0085ad40u
#define G_GAMEMODE      0x00850078u
#define G_RACES         0x008383e0u
#define G_NRACES        0x008383e4u
#define G_STRPOOL       0x00838428u
#define G_SNAPSHOT      0x00851148u
#define G_POINTS        0x007f7ac8u
#define RACE_SZ         0x88u
#define C_RESULTS       0xd24u
#define C_BANK          0x7134u
#define C_OPEN          0x71c8u
#define C_STAGEREC      0x72d8u
#define C_STAGE         0x7af8u
#define C_NEVENTS       0x7afcu
#define C_EVENTS        0x7b00u
#define C_STARTED       0x872cu
#define SR_SIZE         0x104u
#define SR_POINTS       0xe0u
#define SR_RUNS         0xe4u
#define SR_SPONSOR      0xe8u
#define SP_PRIZE        0x02u
#define FN_ENTRY        0x00512f10u
#define FN_RACEWON      0x00541420u
#define FN_MARGIN_REC   0x00513430u
#define FN_RACE_TYPE    0x00512ae0u
#define FN_OPEN_LIST    0x00527d30u
static uint32_t gcall(uint32_t fn, uint32_t ecx, int n, const uint32_t *a){ return roam_win_call(fn, ecx, 0, n, a); }
static const char *race_name(uint32_t r){
    uint32_t s = rd(G_STRPOOL) + MR16(r);
    return roam_win_committed(s) && roam_win_committed(s + 63) ? (const char *)GPTR(s) : "?";
}
static int race_in_table(uint32_t r){
    uint32_t t = rd(G_RACES), n = rd(G_NRACES);
    return t && n <= 1024 && r >= t && r < t + n * RACE_SZ && (r - t) % RACE_SZ == 0 && roam_win_committed(r + RACE_SZ);
}
static int event_won(uint32_t h){
    uint32_t n = rd(G_CAREER + C_NEVENTS);
    for (uint32_t i = 0; i < n && i < 384; i++) if (MR32(G_CAREER + C_EVENTS + 8u * i) == h) return MR8(G_CAREER + C_EVENTS + 8u * i + 6u) == 1;
    return 0;
}
static int race_open(uint32_t r){ for (uint32_t i = 0; i < 64; i++) if (rd(G_CAREER + C_OPEN + 4u * i) == r) return 1; return 0; }
static uint32_t stage_rec(uint32_t s){ return s < 8 ? G_CAREER + C_STAGEREC + s * SR_SIZE : 0; }
static int stage_wins(uint32_t sr){ int n = 0; for (uint32_t i = 0; i < 35; i++) n += rd(sr + 4u * i) != 0; return n; }
static uint32_t fbits(float f){ uint32_t u; memcpy(&u, &f, 4); return u; }
static MpCredit g_last;
int mpcredit_last(MpCredit *o){ *o = g_last; return g_last.race != 0; }
static int win_now(uint32_t r, float margin){
    uint32_t h = MR32(r + 8u), kind = MR8(r + 0x34u), rs = MR8(r + 0x37u), st0 = rd(G_CAREER + C_STAGE);
    uint32_t sr = stage_rec(rs), cur = stage_rec(st0);
    if (!sr || !cur){ roam_log("[mpcredit] f=%ld %s: stage %u, no stage record: left\n", roam_win_frame_no(), race_name(r), rs); return 0; }
    int open = race_open(r), wins0 = stage_wins(cur);
    uint32_t bank0 = rd(G_CAREER + C_BANK), runs0 = rd(sr + SR_RUNS);
    float pts0 = MRF32(sr + SR_POINTS);
    uint32_t e = gcall(FN_ENTRY, G_CAREER, 1, &h);                        /* 1: the event entry, as StartRace */
    if (!e || !roam_win_committed(e + 8) || MR32(e) != h){ roam_log("[mpcredit] f=%ld %s: no event entry (0x%08x): left\n", roam_win_frame_no(), race_name(r), e); return 0; }
    MR32(G_CAREER + C_STARTED) += 1u;
    uint32_t sp = kind == 1u ? rd(cur + SR_SPONSOR) : 0, prize = sp && roam_win_committed(sp + 4) ? MR16(sp + SP_PRIZE) : MR32(r + 0x30u);
    if (prize > 1000000u) prize = 0;
    MR32(G_CAREER + C_BANK) = bank0 + prize;                              /* 2: the prize and the margin, as RaceWon pays */
    if (margin < 0.f){
        uint32_t ty = gcall(FN_RACE_TYPE, 0, 1, &r);
        margin = 0.f;
        for (uint32_t k = 0; k < 7; k++) if (rd(G_POINTS + 0x1cu * k) == ty){ margin = MRF32(G_POINTS + 0x1cu * k + 4u); break; }
    }
    uint32_t a[2] = { fbits(margin), r };
    gcall(FN_MARGIN_REC, sr, 2, a);
    uint8_t pay = MR8(G_CAREER + C_RESULTS + 2u);                         /* 3: RaceWon, its paying part off */
    MR8(G_CAREER + C_RESULTS + 2u) = 0;
    gcall(FN_RACEWON, G_CAREER, 1, &h);
    MR8(G_CAREER + C_RESULTS + 2u) = pay;
    gcall(FN_OPEN_LIST, G_CAREER, 0, 0);                                  /* 4: the open races again */
    uint32_t st1 = rd(G_CAREER + C_STAGE);
    memset(&g_last, 0, sizeof g_last);
    g_last.race = r; g_last.prize = (int)prize; g_last.stage_from = (int)st0; g_last.stage_to = (int)st1;
    g_last.points = (int)(MRF32(sr + SR_POINTS) - pts0 + 0.5f);
    roam_log("[mpcredit] f=%ld %s (kind %u, stage %u, %s): won %d, still open %d; bank $%u -> $%u (prize $%u%s); stage %u record: "
             "%.0f points of %u races -> %.0f of %u (margin %.2f); wins of stage %u %d -> %d; stage %u -> %u (sponsor offers and the "
             "race's movie: not in the plugin; the game offers sponsors after the next race)\n",
             roam_win_frame_no(), race_name(r), kind, rs, open ? "open" : "not open", event_won(h), race_open(r), bank0, rd(G_CAREER + C_BANK),
             prize, sp ? ", the sponsor's" : "", rs, (double)pts0, runs0, (double)MRF32(sr + SR_POINTS), rd(sr + SR_RUNS), (double)margin,
             st0, wins0, stage_wins(cur), st0, st1);
    return event_won(h);
}
#define PEND_MAX 16
static struct { uint32_t race[PEND_MAX]; float margin[PEND_MAX]; int n; long since; } g_pend;
int mpcredit_win(struct CPU *c, uint32_t race, float margin, int in_roam){
    (void)c;
    if (!race_in_table(race)){ roam_log("[mpcredit] f=%ld race 0x%08x is not in the race table: left\n", roam_win_frame_no(), race); return 0; }
    if (event_won(MR32(race + 8u))){ roam_log("[mpcredit] f=%ld %s is won already: left\n", roam_win_frame_no(), race_name(race)); return 0; }
    if (rd(G_GAMEMODE) != 1u){ roam_log("[mpcredit] f=%ld %s: not in the career (mode 0x%x): left\n", roam_win_frame_no(), race_name(race), rd(G_GAMEMODE)); return 0; }
    if (in_roam){
        for (int i = 0; i < g_pend.n; i++) if (g_pend.race[i] == race) return 1;
        if (g_pend.n >= PEND_MAX) return 0;
        g_pend.race[g_pend.n] = race; g_pend.margin[g_pend.n++] = margin;
        roam_log("[mpcredit] f=%ld %s: in free roam, the win waits for the next crib (until the game ends)\n", roam_win_frame_no(), race_name(race));
        return 1;
    }
    return win_now(race, margin);
}
int mpcredit_pending(void){ return g_pend.n; }
int mpcredit_busy(void){ return 0; }
void mpcredit_profile_written(const char *path){ (void)path; }
void mpcredit_profile_deleted(const char *path){ (void)path; }
int mpcredit_test(const char *name, float margin){ (void)name; (void)margin; return 0; }
/* per frame: a waiting win, in the crib, once it took its snapshot of the profile (180 frames at most) */
void mpcredit_frame(struct CPU *c){
    (void)c;
    if (!g_pend.n) return;
    if (rd(G_GAMEMODE) != 1u || !career_fe_active("UI_CareerCrib.fng") || career_fe_active("MC_Main.fng")){ g_pend.since = 0; return; }
    long f = roam_win_frame_no();
    if (!g_pend.since) g_pend.since = f;
    if (!rd(G_SNAPSHOT) && f - g_pend.since < 180) return;
    uint32_t r = g_pend.race[0]; float m = g_pend.margin[0];
    for (int i = 1; i < g_pend.n; i++){ g_pend.race[i - 1] = g_pend.race[i]; g_pend.margin[i - 1] = g_pend.margin[i]; }
    g_pend.n--;
    if (!race_in_table(r) || event_won(MR32(r + 8u))){ roam_log("[mpcredit] f=%ld a waiting win: won already or gone, left\n", f); return; }
    roam_log("[mpcredit] f=%ld the waiting win of %s, in the crib\n", f, race_name(r));
    win_now(r, m);
}

/* ---- what roam_game.c's race lobbies take from the port ---- */
#undef floorf
float floorf(float x){ return mp_floorf(x); }
const char *language_name(void){ return "english"; }      /* (the port's language.c reads the game's; the plugin's texts are English) */
void pad_lobby_offer(int left, int right){ (void)left; (void)right; }   /* (the port's controller keys for the lobby card: none here) */
int  pad_lobby_take(void){ return 0; }
