/* guards_win.c -- guards against the game's own crashes the plugin's ways bring out (NFSU2FreeRoam.asi only).
 *
 * The sound library's lookup of a sound in its banks (0x73CA50, cdecl (id, variation)): the global sound system
 * [0x8B7BB0] has +0x38 an index by sound id, +0x34 the sounds (a dword each: bits 16..20 the bank's slot), +0x58 the
 * banks by slot.  When a bank was unloaded and a sound of it was still asked for, the slot's pointer was 0 and the
 * read of its +0x2C at 0x73CA78 crashed the game: the desktop, 2026-11-07 03:10:52, leaving free roam after the
 * career's stage-up movie the plugin played (Scene07, UpperClass: the movie's own sound took the banks' place).  The
 * guard: a bank slot of 0 counts as a bank that is not the sound's (the function's own "not this bank" way on,
 * 0x73CAB7: the sound's variations table, then 0x73C9F0's answer), so the game goes on.  Every slot that was 0 is
 * counted and the count logged ("[guard]").  Nothing changes while every bank is there. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "roam_host_win.h"

static int ok(uint32_t a, uint32_t n){ return a && roam_win_committed(a) && roam_win_committed(a + n); }

#define SND_SITE  0x0073ca74u   /* mov ecx,[eax+ecx*4+0x58]; movsx eax,word [ecx+0x2c]; cmp eax,ebx; lea edx,[esi+0x10]; jne 0x73cab7 */
#define SND_MATCH 0x0073ca83u   /* the bank is the sound's */
#define SND_OTHER 0x0073cab7u   /* not this bank: the variations' way */

static volatile LONG g_snd_hits;
static LONG g_snd_logged;
static int g_on;

static uint8_t g_page[4096] __attribute__((aligned(4096)));
static void load_watch_install(void);

static void put_rel(uint8_t *at, uint32_t target){ int32_t r = (int32_t)(target - ((uint32_t)(uintptr_t)at + 4u)); memcpy(at, &r, 4); }

/* roam_asi.c, at the start (with the plugin on) */
void guards_win_install(void){
    static const uint8_t want[15] = { 0x8b, 0x4c, 0x88, 0x58, 0x0f, 0xbf, 0x41, 0x2c, 0x3b, 0xc3, 0x8d, 0x56, 0x10, 0x75, 0x34 };
    uint8_t *site = (uint8_t *)(uintptr_t)SND_SITE;
    DWORD old;
    if (g_on) return;
    if (!ok(SND_SITE, sizeof want) || memcmp(site, want, sizeof want)){
        roam_log("[guard] the sound banks' lookup (0x%08x) is not the game's (another mod?): not guarded\n", SND_SITE);
        return;
    }
    if (!VirtualProtect(g_page, sizeof g_page, PAGE_EXECUTE_READWRITE, &old)) return;
    uint8_t *t = g_page;
    static const uint8_t head[] = { 0x8b, 0x4c, 0x88, 0x58,     /* mov ecx, [eax+ecx*4+0x58] */
                                    0x85, 0xc9,                 /* test ecx, ecx */
                                    0x74, 0x14,                 /* jz +20: the guard */
                                    0x0f, 0xbf, 0x41, 0x2c,     /* movsx eax, word [ecx+0x2c] */
                                    0x3b, 0xc3,                 /* cmp eax, ebx */
                                    0x8d, 0x56, 0x10,           /* lea edx, [esi+0x10] */
                                    0x0f, 0x85 };               /* jne SND_OTHER */
    memcpy(t, head, sizeof head);                       /* 0..18 */
    put_rel(t + 19, SND_OTHER);                         /* 19..22 */
    t[23] = 0xe9; put_rel(t + 24, SND_MATCH);           /* 23..27: jmp SND_MATCH */
    t[28] = 0xff; t[29] = 0x05;                         /* 28: inc dword [g_snd_hits] */
    uint32_t hits = (uint32_t)(uintptr_t)&g_snd_hits; memcpy(t + 30, &hits, 4);
    t[34] = 0x8d; t[35] = 0x56; t[36] = 0x10;           /* 34: lea edx, [esi+0x10] */
    t[37] = 0xe9; put_rel(t + 38, SND_OTHER);           /* 37..41: jmp SND_OTHER */
    FlushInstructionCache(GetCurrentProcess(), t, 64);
    if (!VirtualProtect(site, sizeof want, PAGE_EXECUTE_READWRITE, &old)) return;
    site[0] = 0xe9; put_rel(site + 1, (uint32_t)(uintptr_t)t);
    for (int i = 5; i < (int)sizeof want; i++) site[i] = 0x90;
    VirtualProtect(site, sizeof want, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof want);
    g_on = 1;
    roam_log("[guard] the sound banks' lookup (0x%08x): a sound whose bank is gone no longer crashes the game\n", SND_SITE);
    load_watch_install();
}
/* ---- what the game loads of the cars while a multiplayer race loads (a diagnostic; the player's list, item 7) ------
 * In a LAN race the laptop drew the desktop's SENTRAB12 (an Unlimiter add-on car, type 50) as its spoiler alone, while
 * the desktop drew the laptop's COROLLA (a stock car, type 2) whole; in free roam the plugin loads the SENTRAB12 fine.
 * So the race's loading is watched: each CARS file the game asks the resource loader for (0x57CFB0, cdecl: the file's
 * name first) from the trip's way into the LAN race to the race's end is logged ("[guard] the race loads ..."), at
 * most 200 lines a race.  The call goes on to the game's as it was. */
#define LOAD_FN 0x0057cfb0u
int mprace_phase(void);                     /* runtime/mprace.c: 2 on the way into the LAN race, 3 the race */
typedef uint32_t (__attribute__((cdecl)) *load_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static load_t g_load;
static int g_load_logged, g_load_phase;
static uint32_t __attribute__((cdecl)) hk_load(uint32_t file, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5){
    int ph = mprace_phase();
    if (ph != g_load_phase){ g_load_phase = ph; if (ph == 2) g_load_logged = 0; }
    if ((ph == 2 || ph == 3) && g_load_logged < 200 && ok(file, 6)){
        char nm[72]; int i = 0;
        for (; i < 71 && ok(file + (uint32_t)i, 1) && MR8(file + (uint32_t)i) >= 0x20 && MR8(file + (uint32_t)i) < 0x7f; i++) nm[i] = (char)MR8(file + (uint32_t)i);
        nm[i] = 0;
        if ((nm[0] == 'C' || nm[0] == 'c') && (nm[1] == 'A' || nm[1] == 'a') && (nm[4] == 0x5c || nm[4] == '/')){
            g_load_logged++;
            roam_log("[guard] the race loads %s (trip phase %d)\n", nm, ph);
        }
    }
    return g_load(file, a1, a2, a3, a4, a5);
}
static void load_watch_install(void){
    static const uint8_t want[6] = { 0x8b, 0x4c, 0x24, 0x0c, 0x33, 0xc0 };   /* mov ecx, [esp+0xc]; xor eax, eax */
    uint8_t *site = (uint8_t *)(uintptr_t)LOAD_FN;
    DWORD old;
    if (!ok(LOAD_FN, 6) || memcmp(site, want, 6)){ roam_log("[guard] the resource loader (0x%08x) is not the game's: its race loads not logged\n", LOAD_FN); return; }
    uint8_t *t = g_page + 64;                                      /* (the sound guard's code is in the first 42 bytes) */
    memcpy(t, site, 6);
    t[6] = 0xe9; put_rel(t + 7, LOAD_FN + 6u);
    FlushInstructionCache(GetCurrentProcess(), t, 16);
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) return;
    site[0] = 0xe9; put_rel(site + 1, (uint32_t)(uintptr_t)hk_load); site[5] = 0x90;
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    g_load = (load_t)(void *)t;
    roam_log("[guard] the resource loader (0x%08x): the cars' files a multiplayer race loads are logged\n", LOAD_FN);
}
/* roam_asi.c, each frame: the guard's count when it grew */
void guards_win_frame(void){
    LONG n = g_snd_hits;
    if (n == g_snd_logged) return;
    g_snd_logged = n;
    roam_log("[guard] a sound whose bank is gone (its slot empty): skipped, %ld time(s) so far (it would have crashed the game)\n", (long)n);
}
