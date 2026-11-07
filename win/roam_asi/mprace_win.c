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
 *    margin, RaceWon, the open races again), in the crib on the way back, then the sponsor offers and the race's
 *    movie (the offers with the game's own Yes / No labels, the question in English).  A win in free roam waits for
 *    the next crib in
 *    memory (not across a restart). */
#include "mprace_host_win.h"
#include "mprace.h"
#include "mpcredit.h"
#include "racenet.h"
#include <stdarg.h>

#define WIN32_LEAN_AND_MEAN
typedef unsigned long DWORD;
int __stdcall VirtualProtect(void *a, unsigned long n, DWORD prot, DWORD *old);
char *__stdcall lstrcpyA(char *d, const char *s);
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
/* ---- the quit notices of a race (mprace.c quit_tell / mprace_quit_heard, "N2QT"): on the virtual LAN's UDP 3659
 * between the race's games (vsock.c vs_rt_*; the race's peers are those of its link, UDP 3658), as the browser's
 * net_lan.c carries them: a player who quits at the pause menu tells the others, and one left alone wins ---- */
int  vs_rt_send(uint16_t port, uint32_t ip_h, const void *p, int n);
int  vs_rt_recv(uint16_t port, uint32_t *ip_h, void *p, int cap);
int  vs_race_peers(uint32_t *ip_h, int max, uint32_t within_ms);
int  vs_session_active(void);
int vlan_race_peers(uint32_t *ip_h, int max, uint32_t within_ms){ return vs_race_peers(ip_h, max, within_ms); }
int vlan_rt_send(uint16_t port, uint32_t ip_h, const void *p, int n){ return vs_rt_send(port, ip_h, p, n); }
void mprace_quit_heard(uint8_t *m, int n, uint32_t from);
#define QT_PORT_WIN     3659u          /* mprace.c QT_PORT */
/* once a frame (roam_asi.c): what came to the port, while the game's network is up */
void mprace_win_net_frame(void){
    if (!vs_session_active()) return;
    uint8_t m[64]; uint32_t from;
    for (int k = 0; k < 16; k++){
        int n = vs_rt_recv(QT_PORT_WIN, &from, m, sizeof m);
        if (n < 0) break;
        if (n >= 4 && !memcmp(m, "N2QT", 4)) mprace_quit_heard(m, n, from);
    }
}

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
/* the sponsor offers and the race's movie after a win (runtime/mpcredit.c 5 and 6) */
#define SR_INFO         0x100u        /* the stage's info: +1 its sponsors, +8 them, +0x1C the wins each asks for (int16) */
#define SP_REP          0x0eu         /* a sponsor: the reputation it asks for (int16) */
#define SP_BONUS        0x0cu         /* ...its signing bonus (int16) */
#define FN_WANTS_SP     0x00500610u   /* thiscall stage record -> al: it wants a sponsor */
#define FN_SIGN         0x00532cf0u   /* thiscall Career (sponsor), ret 4: signed (with its bonus) when none is */
#define FN_SP_NAMEHASH  0x005129f0u   /* thiscall sponsor: the hash of "SPONSOR_<its name>" */
#define FN_TEXT         0x004ffa80u   /* cdecl (hash): the game's text for it */
#define FN_MALLOC       0x00575620u
#define FN_DIALOG2      0x00558750u   /* cdecl (from package, dialog package, label1, label2, msg1, msg2, back msg, first, fmt, ...) */
#define G_DLG           0x00838974u   /* the dialog up now (0 none) */
#define FN_CRIB_MSG     0x004af3a0u   /* UI_CareerCrib's message handler (thiscall msg, obj, a3, a4; ret 0x10) */
#define VT_CRIB         0x00797550u   /* ...its vtable: entry 0 is it (the only caller) */
#define G_FEMGR_LIST    0x008637b4u   /* the FE manager's screens: n = [it]; until n == it: screen [n - 4 + 0x14], n = [n] */
#define G_MOVIES        0x008382a0u   /* the movies to play: 4 names of 0x40 */
#define G_MOVIE_AT      0x0083856cu   /* ...the one playing */
#define G_MOVIE_MODE    0x00838568u   /* what follows the movie screen (0x54AF80): 5 nothing */
#define G_FE            0x008384c4u   /* the FE manager */
#define FN_FE_ADD       0x00555e80u   /* cdecl (package, 0): the package added, over the others */
#define FN_MOVIE_PREP   0x0043b640u   /* thiscall 0x827E88: as 0x53D7A0 before its movie */
#define MSG_CRIB_GO     0x451e768eu   /* to the crib: "Return to Explore Mode?" Yes */
#define MSG_SAVE_WORLD  0x461a18eeu   /* to the crib from the memory card, its save over: on to the world (first) */
#define MSG_SAVE_DONE   0x251bf83eu   /* to the crib: its save is over: on to the world */
#define MSG_CRIB_WORLD  0x9a3b571du   /* to the crib: no save to make: the world */
#define S_PLAYMOVIE     0x00799ef0u   /* "IG_PlayMovie.fng" */
#define G_MOVIES_DONE   0x008383a8u   /* 1: the movie screen played the last of them */
#define RACE_LAST       0x04cce3e1u   /* S5_CIRCUIT_36, the career's last race (Scene15) */
#define LBL_YES         0x0000f770u   /* the left button of the game's own two-button questions ("Yes") */
#define LBL_NO          0x0000061cu   /* ...the right one ("No") */
#define MSG_SIGN        0x4d504301u   /* our answers (not the game's messages: its screens ignore them) */
#define MSG_NEXT        0x4d504302u
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
static float stage_rep(uint32_t sr){ int runs = (int)rd(sr + SR_RUNS); return runs > 0 ? MRF32(sr + SR_POINTS) / (float)runs : 0.f; }
static void gs(uint32_t a, const char *s, uint32_t cap){ uint32_t i = 0; for (; s[i] && i + 1 < cap; i++) MR8(a + i) = (uint8_t)s[i]; MR8(a + i) = 0; }

/* ---- after the win, in the crib: the sponsor offers and the race's movie (runtime/mpcredit.c 5 and 6) ----------
 * The sponsors the game's post-race list would offer now (0x4A4610's test: the career, the current stage past the
 * first wants one, those of the stage info's sponsors whose wins the stage has and whose reputation the stage before's
 * average is): asked one after the other over the crib with the game's generic dialog, the game's own Yes / No
 * labels (the plugin can add no texts to the game's table): "Sponsor offer 1 of 3: <name>. Signing bonus X, its races
 * pay Y. Sign with them? Yes: sign. No: the next offer."  Yes signs (Career 0x532CF0, the bonus into the bank).  The
 * answers come to the crib's message handler (0x4AF3A0, its vtable entry ours).  Then the race's movie (the race
 * table's scene, Scene07 and UpperClass into the second stage, Scene15 after the last race), as after a Solo race:
 * between Return to Explore Mode's save and the world (the crib's messages on to the world held, the movie played
 * with IG_PlayMovie.fng, then the held messages to the crib's handler).  While the question is up or the movie
 * plays, the trip waits (mpcredit_busy) and the keys are the player's. */
#define OFFERS_MAX 5
static int sponsor_offers(uint32_t *o){
    uint32_t st = rd(G_CAREER + C_STAGE), sr = stage_rec(st), info = sr ? rd(sr + SR_INFO) : 0;
    if (rd(G_GAMEMODE) != 1u || st <= 1 || st >= 8 || !info || !roam_win_committed(info + 0x30)) return 0;
    if (!(gcall(FN_WANTS_SP, sr, 0, 0) & 0xffu)) return 0;
    float rep = stage_rep(sr - SR_SIZE); int wins = stage_wins(sr), n = (int8_t)MR8(info + 1u), k = 0;
    for (int i = 0; i < n && i < OFFERS_MAX; i++){
        uint32_t sp = rd(info + 8u + 4u * (uint32_t)i);
        if (sp && roam_win_committed(sp + 0x10) && wins >= (int16_t)MR16(info + 0x1cu + 2u * (uint32_t)i) && (float)(int16_t)MR16(sp + SP_REP) <= rep) k++;
    }
    for (int i = 0; i < k; i++) o[i] = rd(info + 8u + 4u * (uint32_t)i);
    return k;
}
enum { UI_NONE, UI_OFFER, UI_ASKED, UI_WAIT, UI_MOVIE, UI_PLAYING };
static struct {
    int      st;
    uint32_t offer[OFFERS_MAX]; int n, i;      /* the sponsors offered, the one asked about */
    char     scene[4][16]; int nscene;         /* the races' movies (the game plays up to 4 in a row) */
    volatile uint32_t answer;                  /* MSG_SIGN / MSG_NEXT, from the crib's handler */
    long     at; int seen;
    uint32_t str; int patched;
    volatile int leaving;                      /* the crib's Return to Explore Mode went */
    uint32_t held_this, held[3][4]; volatile int nheld;   /* the crib's messages on to the world, held for the movie */
} g_ui;
int mpcredit_busy(void){ return g_ui.st != UI_NONE && g_ui.st != UI_WAIT; }
static int movies_on(void){ const char *o = roam_opt("NFSU2_ROAM_MOVIES"); return !(o && o[0] == '0' && !o[1]); }
static void race_scene(uint32_t r, uint32_t race_stage, uint32_t stage_now){
    char nm[16] = "";
    uint32_t s = rd(G_STRPOOL) + MR16(r + 4u);
    if (roam_win_committed(s) && roam_win_committed(s + 15) && MR8(s)){ for (int i = 0; i < 15 && MR8(s + (uint32_t)i); i++) nm[i] = (char)MR8(s + (uint32_t)i); }
    else if (race_stage != stage_now && stage_now == 2u) lstrcpyA(nm, "Scene07");
    else if (MR32(r + 8u) == RACE_LAST) lstrcpyA(nm, "Scene15");
    if (!nm[0] || g_ui.nscene >= 4) return;
    lstrcpyA(g_ui.scene[g_ui.nscene++], nm);
    if (!strcasecmp(nm, "Scene07") && g_ui.nscene < 4) lstrcpyA(g_ui.scene[g_ui.nscene++], "UpperClass");
}
/* a sponsor's name as the sponsor screen shows it (the game's text, its own letters) */
static void sponsor_name(uint32_t sp, char *out, int n){
    uint32_t h = gcall(FN_SP_NAMEHASH, sp, 0, 0), t = gcall(FN_TEXT, 0, 1, &h);
    int a = 0;
    for (uint32_t i = 0; t && roam_win_committed(t + i) && MR8(t + i) && i < 40; i++){
        uint8_t ch = MR8(t + i);
        if (ch < 0x20 || ch == '%') ch = ' ';
        if (a + 1 < n) out[a++] = (char)ch;
    }
    out[a] = 0;
}
static uint32_t crib_name(void){
    uint32_t n = rd(G_FEMGR_LIST);
    for (int k = 0; n && n != G_FEMGR_LIST && k < 256; k++, n = rd(n)){
        uint32_t sc = rd(n - 4u + 0x14u), nm = sc ? rd(sc + 4u) : 0;
        if (nm && roam_win_committed(nm + 18) && !strcasecmp((const char *)GPTR(nm), "UI_CareerCrib.fng")) return nm;
    }
    gs(g_ui.str + 0x200u, "UI_CareerCrib.fng", 0x20);
    return g_ui.str + 0x200u;
}
/* the crib's handler: our answers while we ask, and its way to the world held while a movie waits; the rest the game's */
typedef uint32_t (__attribute__((thiscall)) *crib_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static uint32_t __attribute__((thiscall)) crib_msg(uint32_t self, uint32_t m, uint32_t o, uint32_t a3, uint32_t a4){
    if ((m == MSG_SIGN || m == MSG_NEXT) && g_ui.st == UI_ASKED){ g_ui.answer = m; return 0; }
    if (g_ui.st == UI_WAIT && m == MSG_CRIB_GO) g_ui.leaving = 1;
    if ((g_ui.st == UI_WAIT || (g_ui.st == UI_MOVIE && g_ui.nheld)) && g_ui.leaving && g_ui.nheld < 3
        && (m == MSG_SAVE_WORLD || m == MSG_SAVE_DONE || m == MSG_CRIB_WORLD)){
        if (!g_ui.nheld){ g_ui.held_this = self; g_ui.at = roam_win_frame_no(); }
        g_ui.held[g_ui.nheld][0] = m; g_ui.held[g_ui.nheld][1] = o; g_ui.held[g_ui.nheld][2] = a3; g_ui.held[g_ui.nheld][3] = a4;
        g_ui.nheld++;
        g_ui.st = UI_MOVIE;
        roam_log("[mpcredit] f=%ld the crib's way to the world (%08x) held: the race's movie first\n", roam_win_frame_no(), m);
        return 0;
    }
    return ((crib_t)(uintptr_t)FN_CRIB_MSG)(self, m, o, a3, a4);
}
static int ui_ready(void){
    if (!g_ui.patched){ g_ui.patched = roam_win_vtpatch(VT_CRIB, FN_CRIB_MSG, (uint32_t)(uintptr_t)crib_msg) ? 1 : -1; }
    if (!g_ui.str){ uint32_t sz = 0x240u; g_ui.str = gcall(FN_MALLOC, 0, 1, &sz); }
    return g_ui.patched > 0 && g_ui.str != 0;
}
static int dialog_up(void){ return career_fe_active("GenericDialog_MED.fng") || career_fe_active("GenericDialog_SMALL.fng") || rd(G_DLG); }
static int ui_ask(void){
    if (!ui_ready() || dialog_up()) return 0;
    uint32_t sp = g_ui.offer[g_ui.i];
    char nm[48], t[0x1c0];
    sponsor_name(sp, nm, sizeof nm);
    snprintf(t, sizeof t, "Sponsor offer %d of %d: %s. Signing bonus %d, its races pay %u. Sign with them?  Yes: sign.  No: the next offer.",
             g_ui.i + 1, g_ui.n, nm, (int)(int16_t)MR16(sp + SP_BONUS), MR16(sp + SP_PRIZE));
    for (char *p = t; *p; p++) if (*p == '%') *p = '?';
    uint32_t dl = g_ui.str, fmt = g_ui.str + 0x38u, txt = g_ui.str + 0x40u;
    gs(dl, "GenericDialog_MED.fng", 0x38); gs(fmt, "%s", 8); gs(txt, t, 0x1c0);     /* (the front end's; IG_ is the world's) */
    uint32_t a[10] = { crib_name(), dl, LBL_YES, LBL_NO, MSG_SIGN, MSG_NEXT, MSG_NEXT, 0, fmt, txt };
    gcall(FN_DIALOG2, 0, 10, a);
    roam_log("[mpcredit] f=%ld asked: \"%s\"\n", roam_win_frame_no(), t);
    return 1;
}
static int ui_movie(void){
    if (!ui_ready() || !rd(G_FE)) return 0;
    for (uint32_t k = 0; k < 4; k++) MR8(G_MOVIES + 0x40u * k) = 0;
    for (int k = 0; k < g_ui.nscene; k++) gs(G_MOVIES + 0x40u * (uint32_t)k, g_ui.scene[k], 0x40);
    MR32(G_MOVIE_AT) = 0; MR32(G_MOVIE_MODE) = 5; MR32(G_MOVIES_DONE) = 0;
    gcall(FN_MOVIE_PREP, 0x00827e88u, 0, 0);
    uint32_t a[2] = { S_PLAYMOVIE, 0 };
    gcall(FN_FE_ADD, 0, 2, a);
    roam_log("[mpcredit] f=%ld the race's movie: %s%s%s\n", roam_win_frame_no(), g_ui.scene[0], g_ui.nscene > 1 ? ", then " : "",
             g_ui.nscene > 1 ? g_ui.scene[1] : "");
    return 1;
}
static void ui_frame(void){
    long f = roam_win_frame_no();
    int crib = career_fe_active("UI_CareerCrib.fng"), movie = career_fe_active("IG_PlayMovie.fng");
    switch (g_ui.st){
    case UI_OFFER:
        if (crib && !career_fe_active("MC_Main.fng") && ui_ask()){ g_ui.st = UI_ASKED; g_ui.at = f; g_ui.answer = 0; }
        break;
    case UI_ASKED:
        if (g_ui.answer == MSG_SIGN){
            uint32_t sp = g_ui.offer[g_ui.i], b0 = rd(G_CAREER + C_BANK), st = rd(G_CAREER + C_STAGE);
            gcall(FN_SIGN, G_CAREER, 1, &sp);
            if (rd(stage_rec(st) + SR_SPONSOR) == sp){
                g_last.sponsor = sp; g_last.bonus = (int)(rd(G_CAREER + C_BANK) - b0);
                sponsor_name(sp, g_last.sponsor_name, sizeof g_last.sponsor_name);
            }
            roam_log("[mpcredit] f=%ld signed with %s (offer %d of %d): bonus $%d, bank $%u -> $%u%s\n", f, g_last.sponsor_name,
                     g_ui.i + 1, g_ui.n, g_last.bonus, b0, rd(G_CAREER + C_BANK), g_last.sponsor == sp ? "" : " (the game did not sign it)");
            g_ui.st = g_ui.nscene ? UI_WAIT : UI_NONE;
        } else if (g_ui.answer == MSG_NEXT){
            roam_log("[mpcredit] f=%ld the answer: the next offer\n", f);
            g_ui.i = (g_ui.i + 1) % g_ui.n; g_ui.st = UI_OFFER;
        } else if (!dialog_up() && f - g_ui.at > 30) g_ui.st = UI_OFFER;     /* (gone another way: again) */
        g_ui.answer = 0;
        break;
    case UI_WAIT:
        break;
    case UI_MOVIE:
        if (ui_movie()){ g_ui.st = UI_PLAYING; g_ui.at = f; g_ui.seen = 0; }
        else if (f - g_ui.at > 60){ g_ui.st = UI_PLAYING; g_ui.at = f - 100000; }   /* (could not: on to the world) */
        break;
    case UI_PLAYING:
        if (movie) g_ui.seen = 1;
        if ((!movie && (g_ui.seen || rd(G_MOVIES_DONE) == 1u || f - g_ui.at > 300)) || f - g_ui.at > 60 * 600){
            roam_log("[mpcredit] f=%ld the race's movie %s: the world, as the crib goes there\n", f,
                     g_ui.seen || rd(G_MOVIES_DONE) == 1u ? "is over" : "did not play");
            g_ui.st = UI_NONE; g_ui.leaving = 0; g_ui.nscene = 0;
            int n = g_ui.nheld; g_ui.nheld = 0;
            for (int i = 0; i < n; i++) gcall(FN_CRIB_MSG, g_ui.held_this, 4, g_ui.held[i]);
        }
        break;
    }
}
/* after win_now: the offers and the movie to come (in the crib, mpcredit_frame) */
static void ui_after_win(uint32_t r, uint32_t race_stage, uint32_t stage_now){
    if (g_ui.st != UI_NONE && g_ui.st != UI_WAIT) return;
    if (g_ui.st == UI_NONE){ g_ui.nscene = 0; g_ui.leaving = 0; }
    g_ui.n = sponsor_offers(g_ui.offer); g_ui.i = 0;
    int k = g_ui.nscene;
    if (movies_on()) race_scene(r, race_stage, stage_now);
    g_ui.st = g_ui.n ? UI_OFFER : g_ui.nscene ? UI_WAIT : UI_NONE;
    g_last.offers = g_ui.n;
    if (g_ui.nscene > k) lstrcpyA(g_last.movie, g_ui.scene[k]);
    /* (the crib's handler ours now: the trip sends its Return to Explore Mode in this same frame) */
    if (g_ui.st != UI_NONE) ui_ready();
}
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
    ui_after_win(r, rs, st1);                                             /* 5, 6: the sponsor offers, the race's movie */
    roam_log("[mpcredit] f=%ld %s (kind %u, stage %u, %s): won %d, still open %d; bank $%u -> $%u (prize $%u%s); stage %u record: "
             "%.0f points of %u races -> %.0f of %u (margin %.2f); wins of stage %u %d -> %d; stage %u -> %u; %d sponsor offers%s%s\n",
             roam_win_frame_no(), race_name(r), kind, rs, open ? "open" : "not open", event_won(h), race_open(r), bank0, rd(G_CAREER + C_BANK),
             prize, sp ? ", the sponsor's" : "", rs, (double)pts0, runs0, (double)MRF32(sr + SR_POINTS), rd(sr + SR_RUNS), (double)margin,
             st0, wins0, stage_wins(cur), st0, st1, g_last.offers, g_last.movie[0] ? "; the movie " : "", g_last.movie);
    return event_won(h);
}
/* ---- wins kept across a restart (runtime/mpcredit.c's MPWINS.DAT, the plugin's way) --------------------------------
 * <the .ini's name>.wins, one line per win: W (waits for the next crib) or G (given in a crib, not known to be in the
 * profile's save yet), the profile's name, the race's hash, the margin's bits, and for G when it was given (a FILETIME),
 * tab-separated.  A win in free roam (the trip missed the crib) is W; a win given in the crib (the trip's, or a W one) is
 * G.  Once a second (in the career, out of a trip): a G whose race the live career has won and whose profile's save on
 * disk is newer than its giving is in the save: gone from the file; a G whose race the live career has not won (the
 * profile loaded again from a save without it: the game closed, or no save made since) waits again, W.  The W ones of
 * the profile in play are given in the crib, one at a time (after its snapshot of the profile).  So a win is lost only
 * if the file cannot be written.  The save is found under the game folder's SAVEGAMES, else Documents
 * (roam_win_save_mtime); with neither found a G stays until its race shows won after a load. */
int      roam_win_file_read(const char *ext, char *buf, int cap);
int      roam_win_file_write(const char *ext, const char *buf, int n);
uint64_t roam_win_filetime_now(void);
uint64_t roam_win_save_mtime(const char *name, char *where, int cap);
#define PEND_MAX        32
#define G_P1_NAME       0x0083a9e0u   /* player 1's profile (FEDatabase +0x10): its name, 32 bytes, first */
typedef struct { char prof[33]; uint32_t hash; float margin; uint8_t given; uint64_t at; } Pend;
static struct { Pend e[PEND_MAX]; int n, loaded; long since, check_at; } g_pend;
static void live_profile(char out[33]){
    unsigned i = 0;
    for (; i < 32u; i++){ uint8_t ch = rd(G_P1_NAME + (i & ~3u)) >> (8u * (i & 3u)) & 0xffu; if (ch < 0x20u || ch == 0x7fu || ch == '\t') break; out[i] = (char)ch; }
    out[i] = 0;
}
static uint32_t race_by_hash(uint32_t h){
    uint32_t t = rd(G_RACES), n = rd(G_NRACES);
    for (uint32_t i = 0; t && i < n && i < 1024; i++) if (rd(t + i * RACE_SZ + 8u) == h) return t + i * RACE_SZ;
    return 0;
}
static uint32_t hexu(const char **p){
    uint32_t v = 0; const char *s = *p;
    for (;; s++){ char c = *s; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; if (d < 0) break; v = v << 4 | (uint32_t)d; }
    *p = s; return v;
}
static void pend_write(void){
    char b[PEND_MAX * 96 + 8]; int n = 0;
    for (int i = 0; i < g_pend.n; i++){
        const Pend *e = &g_pend.e[i];
        n += snprintf(b + n, sizeof b - (size_t)n, "%c\t%s\t%08x\t%08x\t%08x%08x\n", e->given ? 'G' : 'W', e->prof, e->hash, fbits(e->margin),
                      (uint32_t)(e->at >> 32), (uint32_t)e->at);
        if (n >= (int)sizeof b - 1) break;
    }
    if (!roam_win_file_write("wins", b, n)) roam_log("[mpcredit] the waiting wins' file could not be written: they are in memory only\n");
}
static void pend_load(void){
    g_pend.loaded = 1;
    char b[PEND_MAX * 96 + 8];
    int k = roam_win_file_read("wins", b, sizeof b);
    if (k <= 0) return;
    for (const char *s = b; *s && g_pend.n < PEND_MAX; ){
        const char *eol = s; while (*eol && *eol != '\n') eol++;
        Pend e; memset(&e, 0, sizeof e);
        if ((s[0] == 'W' || s[0] == 'G') && s[1] == '\t'){
            e.given = s[0] == 'G';
            const char *p = s + 2; int j = 0;
            while (p < eol && *p != '\t' && j < 32) e.prof[j++] = *p++;
            e.prof[j] = 0;
            if (p < eol && *p == '\t'){ p++; e.hash = hexu(&p); }
            if (p < eol && *p == '\t'){ p++; uint32_t m = hexu(&p); memcpy(&e.margin, &m, 4); }
            if (p < eol && *p == '\t'){ p++; const char *q = p; uint32_t hi = 0, lo = 0;
                for (int d = 0; d < 8 && q < eol; d++) { char t[2] = { *q++, 0 }; const char *tp = t; hi = hi << 4 | hexu(&tp); }
                for (int d = 0; d < 8 && q < eol; d++) { char t[2] = { *q++, 0 }; const char *tp = t; lo = lo << 4 | hexu(&tp); }
                e.at = (uint64_t)hi << 32 | lo; }
            if (e.prof[0] && e.hash) g_pend.e[g_pend.n++] = e;
        }
        s = *eol ? eol + 1 : eol;
    }
    int w = 0, g = 0; for (int i = 0; i < g_pend.n; i++) { if (g_pend.e[i].given) g++; else w++; }
    roam_log("[mpcredit] %s.wins: %d win(s) wait for a crib, %d given but not known to be saved yet\n", "NFSU2FreeRoam", w, g);
}
static int pend_find(const char *prof, uint32_t h){ for (int i = 0; i < g_pend.n; i++) if (g_pend.e[i].hash == h && !strcasecmp(g_pend.e[i].prof, prof)) return i; return -1; }
static void pend_drop(int i){ for (int k = i + 1; k < g_pend.n; k++) g_pend.e[k - 1] = g_pend.e[k]; g_pend.n--; }
/* a win given in the crib now (G, from now) */
static void pend_given(uint32_t race, float margin){
    char pn[33]; live_profile(pn);
    if (!pn[0]) return;
    uint32_t h = MR32(race + 8u);
    int i = pend_find(pn, h);
    if (i < 0){ if (g_pend.n >= PEND_MAX) return; i = g_pend.n++; memset(&g_pend.e[i], 0, sizeof g_pend.e[i]); lstrcpyA(g_pend.e[i].prof, pn); g_pend.e[i].hash = h; }
    g_pend.e[i].margin = margin; g_pend.e[i].given = 1; g_pend.e[i].at = roam_win_filetime_now();
    pend_write();
}
int mpcredit_win(struct CPU *c, uint32_t race, float margin, int in_roam){
    (void)c;
    if (!g_pend.loaded) pend_load();
    if (!race_in_table(race)){ roam_log("[mpcredit] f=%ld race 0x%08x is not in the race table: left\n", roam_win_frame_no(), race); return 0; }
    if (event_won(MR32(race + 8u))){ roam_log("[mpcredit] f=%ld %s is won already: left\n", roam_win_frame_no(), race_name(race)); return 0; }
    if (rd(G_GAMEMODE) != 1u){ roam_log("[mpcredit] f=%ld %s: not in the career (mode 0x%x): left\n", roam_win_frame_no(), race_name(race), rd(G_GAMEMODE)); return 0; }
    if (in_roam){
        char pn[33]; live_profile(pn);
        uint32_t h = MR32(race + 8u);
        if (!pn[0]){ roam_log("[mpcredit] f=%ld %s: no profile name: the win is not kept\n", roam_win_frame_no(), race_name(race)); return 0; }
        if (pend_find(pn, h) >= 0) return 1;
        if (g_pend.n >= PEND_MAX) return 0;
        Pend *e = &g_pend.e[g_pend.n++]; memset(e, 0, sizeof *e);
        lstrcpyA(e->prof, pn); e->hash = h; e->margin = margin;
        pend_write();
        roam_log("[mpcredit] f=%ld %s: in free roam, the win waits for the next crib (kept in NFSU2FreeRoam.wins)\n", roam_win_frame_no(), race_name(race));
        return 1;
    }
    int ok = win_now(race, margin);
    if (ok) pend_given(race, margin);
    return ok;
}
int mpcredit_pending(void){
    char pn[33]; live_profile(pn); int n = 0;
    for (int i = 0; i < g_pend.n; i++) if (!g_pend.e[i].given && !strcasecmp(g_pend.e[i].prof, pn)) n++;
    return n;
}
void mpcredit_profile_written(const char *path){ (void)path; }
void mpcredit_profile_deleted(const char *path){ (void)path; }
int mpcredit_test(const char *name, float margin){ (void)name; (void)margin; return 0; }
/* once a second, in the career: the given ones saved or lost (above) */
static void pend_check(long f){
    if (f - g_pend.check_at < 60) return;
    g_pend.check_at = f;
    char pn[33]; live_profile(pn);
    if (!pn[0] || rd(G_GAMEMODE) != 1u || !rd(G_RACES)) return;
    char where[300] = "";
    uint64_t saved = 0; int looked = 0, changed = 0;
    for (int i = 0; i < g_pend.n; i++){
        Pend *e = &g_pend.e[i];
        if (!e->given || strcasecmp(e->prof, pn)) continue;
        uint32_t r = race_by_hash(e->hash);
        if (!r){ roam_log("[mpcredit] f=%ld a kept win: its race is not in the race table: dropped\n", f); pend_drop(i--); changed = 1; continue; }
        if (!event_won(e->hash)){
            e->given = 0; e->at = 0; changed = 1;
            roam_log("[mpcredit] f=%ld %s: given before, not in the career now (loaded from a save without it): waits for the crib again\n", f, race_name(r));
            continue;
        }
        if (!looked){ looked = 1; saved = roam_win_save_mtime(pn, where, sizeof where); }
        if (saved && saved > e->at){
            roam_log("[mpcredit] f=%ld %s: in the profile's save now (%s): no longer kept\n", f, race_name(r), where);
            pend_drop(i--); changed = 1;
        }
    }
    if (changed) pend_write();
}
/* per frame: the sponsor question and the movie of a win; the kept wins; a waiting win in the crib, once it took its
 * snapshot of the profile (180 frames at most) */
void mpcredit_frame(struct CPU *c){
    (void)c;
    if (!g_pend.loaded) pend_load();
    /* the sponsor question and the movie of a win; a question or a movie whose crib went away is dropped */
    if (g_ui.st != UI_NONE){
        int gone = rd(G_GAMEMODE) != 1u || ((g_ui.st == UI_OFFER || g_ui.st == UI_ASKED || (g_ui.st == UI_WAIT && !g_ui.leaving))
                                            && !career_fe_active("UI_CareerCrib.fng"));
        if (gone){
            roam_log("[mpcredit] f=%ld the crib is gone: its %s left\n", roam_win_frame_no(), g_ui.st == UI_WAIT ? "movie" : "sponsor question");
            g_ui.st = UI_NONE; g_ui.leaving = 0; g_ui.nheld = 0;
            return;
        }
        if (g_ui.st != UI_WAIT){ ui_frame(); return; }
    }
    long f = roam_win_frame_no();
    if (g_pend.n) pend_check(f);
    char pn[33]; live_profile(pn);
    int w = -1;
    for (int i = 0; i < g_pend.n && w < 0; i++) if (!g_pend.e[i].given && pn[0] && !strcasecmp(g_pend.e[i].prof, pn)) w = i;
    if (w < 0) return;
    if (rd(G_GAMEMODE) != 1u || !career_fe_active("UI_CareerCrib.fng") || career_fe_active("MC_Main.fng")){ g_pend.since = 0; return; }
    if (!g_pend.since) g_pend.since = f;
    if (!rd(G_SNAPSHOT) && f - g_pend.since < 180) return;
    g_pend.since = 0;
    Pend e = g_pend.e[w];
    uint32_t r = race_by_hash(e.hash);
    if (!r || event_won(e.hash)){
        roam_log("[mpcredit] f=%ld a waiting win: won already or gone, left\n", f);
        pend_drop(w); pend_write(); return;
    }
    roam_log("[mpcredit] f=%ld the waiting win of %s, in the crib\n", f, race_name(r));
    if (win_now(r, e.margin)) pend_given(r, e.margin);
    else { pend_drop(w); pend_write(); }
}

/* ---- what roam_game.c's race lobbies take from the port ---- */
#undef floorf
float floorf(float x){ return mp_floorf(x); }
const char *language_name(void){ return "english"; }      /* (the port's language.c reads the game's; the plugin's texts are English) */
void pad_lobby_offer(int left, int right){ (void)left; (void)right; }   /* (the port's controller keys for the lobby card: none here) */
int  pad_lobby_take(void){ return 0; }
