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

/* ---- the game runs on with its window not in front (RunInBackground) -----------------------------------------------
 * The game's message pump (0x5BF1C0, once a frame from GameLoop): with its window active ([0x870988], WM_ACTIVATEAPP's
 * wParam, 0x5CCF3C) or a game of its own network on (0x5F00F0) it peeks (PeekMessage, PM_REMOVE); otherwise it WAITS
 * (GetMessage) for the next window message, so the whole game, the plugin's frame and its network with it, stops.  The
 * laptop, 2026-11-07: its frame count went 77228 -> 77229 in 3.5 minutes with another window in front, its friends'
 * games heard nothing from it ("seems away" call-offs, invites not seen).  The fix: the pump always peeks (the je at
 * 0x5BF219, "not active: wait", is gone).  The game's own focus pause (WM_ACTIVATE, 0x5CCDA6: the world and its sound
 * held until the window is back) is not changed: the world waits, the plugin and its network go on. */
#define PUMP_SITE 0x005bf212u   /* mov eax,[0x870988]; test eax,eax; je 0x5bf22c (GetMessage) */
static void background_install(void){
    static const uint8_t want[9] = { 0xa1, 0x88, 0x09, 0x87, 0x00, 0x85, 0xc0, 0x74, 0x11 };
    uint8_t *site = (uint8_t *)(uintptr_t)PUMP_SITE;
    DWORD old;
    const char *v = roam_opt("NFSU2_ROAM_BACKGROUND");
    if (v && v[0] == '0'){ roam_log("[guard] RunInBackground = 0: the game stops while its window is not in front\n"); return; }
    if (!ok(PUMP_SITE, sizeof want)) return;
    if (site[7] == 0x90 && site[8] == 0x90 && !memcmp(site, want, 7)){ roam_log("[guard] the message pump already runs on in the background (another mod)\n"); return; }
    if (memcmp(site, want, sizeof want)){
        roam_log("[guard] the message pump (0x%08x) is not the game's (another mod?): the game still stops in the background\n", PUMP_SITE);
        return;
    }
    if (!VirtualProtect(site + 7, 2, PAGE_EXECUTE_READWRITE, &old)) return;
    site[7] = 0x90; site[8] = 0x90;
    VirtualProtect(site + 7, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site + 7, 2);
    roam_log("[guard] RunInBackground: the game goes on with its window not in front (friends keep hearing from it)\n");
}

/* ---- an add-on car in a LAN race: its description with no parts (AddOnStockRace = 1; off by default, to try) -------
 * The game's LAN car description (0x5F0320, thiscall (ride, the bit writer, with parts), ret 0xC; its callers
 * 0x5876A1 and 0x5FE5B4 pass 1) writes 0xAA and the ride's parts, or with the third argument 0 a count of 0, which
 * its decoder (0x5F04D0, 0x5F0625) takes as stock parts (0x637040(0) on the ride).  The laptop drew the desktop's
 * SENTRAB12 with its own parts as nothing (or its spoiler) in races, as in free roam (item 7; free roam's side is
 * roam_game.c's AddOnStockLook).  With AddOnStockRace = 1 a ride of type 46 and up (an Unlimiter add-on) goes without
 * parts: the others' games build it stock.  Every such description is counted ("[guard]") */
#define DESC_SITE 0x005f0320u   /* sub esp,0x14; push ebx; push ebp */
static volatile LONG g_desc_hits;
static LONG g_desc_logged;
static void addon_race_install(void){
    static const uint8_t want[5] = { 0x83, 0xec, 0x14, 0x53, 0x55 };
    uint8_t *site = (uint8_t *)(uintptr_t)DESC_SITE;
    DWORD old;
    const char *v = roam_opt("NFSU2_ROAM_ADDONRACE");
    if (!v || v[0] != '1') return;
    if (!ok(DESC_SITE, sizeof want) || memcmp(site, want, sizeof want)){
        roam_log("[guard] the LAN car description (0x%08x) is not the game's (another mod?): AddOnStockRace off\n", DESC_SITE);
        return;
    }
    if (!VirtualProtect(g_page, sizeof g_page, PAGE_EXECUTE_READWRITE, &old)) return;
    uint8_t *t = g_page + 128, *q = t;
    uint8_t *j1, *j2, *j3;
    *q++ = 0x8b; *q++ = 0x44; *q++ = 0x24; *q++ = 0x04;            /* mov eax, [esp+4]: the ride */
    *q++ = 0x85; *q++ = 0xc0;                                      /* test eax, eax */
    *q++ = 0x74; j1 = q++;                                         /* jz orig */
    *q++ = 0x8b; *q++ = 0x00;                                      /* mov eax, [eax]: its car type */
    *q++ = 0x83; *q++ = 0xf8; *q++ = 0x2e;                         /* cmp eax, 46 */
    *q++ = 0x72; j2 = q++;                                         /* jb orig */
    *q++ = 0x3d; *q++ = 0x00; *q++ = 0x01; *q++ = 0x00; *q++ = 0x00;  /* cmp eax, 256 */
    *q++ = 0x73; j3 = q++;                                         /* jae orig */
    *q++ = 0xc6; *q++ = 0x44; *q++ = 0x24; *q++ = 0x0c; *q++ = 0x00;  /* mov byte [esp+0xC], 0: no parts */
    *q++ = 0xf0; *q++ = 0xff; *q++ = 0x05;                         /* lock inc dword [g_desc_hits] */
    { uint32_t a = (uint32_t)(uintptr_t)&g_desc_hits; memcpy(q, &a, 4); q += 4; }
    uint8_t *orig = q;
    *j1 = (uint8_t)(orig - (j1 + 1)); *j2 = (uint8_t)(orig - (j2 + 1)); *j3 = (uint8_t)(orig - (j3 + 1));
    memcpy(q, want, sizeof want); q += sizeof want;                /* the 5 bytes moved */
    *q++ = 0xe9; put_rel(q, DESC_SITE + 5u); q += 4;               /* jmp 0x5F0325 */
    FlushInstructionCache(GetCurrentProcess(), t, (SIZE_T)(q - t));
    if (!VirtualProtect(site, sizeof want, PAGE_EXECUTE_READWRITE, &old)) return;
    site[0] = 0xe9; put_rel(site + 1, (uint32_t)(uintptr_t)t);
    VirtualProtect(site, sizeof want, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof want);
    roam_log("[guard] AddOnStockRace: an add-on car goes to the others' games in a LAN race with stock parts\n");
}

/* roam_asi.c, at the start (with the plugin on) */
static void traffic_install(void);
static void carpool_install(void);
void guards_win_install(void){
    static const uint8_t want[15] = { 0x8b, 0x4c, 0x88, 0x58, 0x0f, 0xbf, 0x41, 0x2c, 0x3b, 0xc3, 0x8d, 0x56, 0x10, 0x75, 0x34 };
    uint8_t *site = (uint8_t *)(uintptr_t)SND_SITE;
    DWORD old;
    if (g_on) return;
    background_install();
    traffic_install();
    addon_race_install();
    carpool_install();
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
/* ---- less traffic in career races (CareerTraffic = percent; fix phase 7, a player's complaint) ----------------------
 * The race's traffic cars come from 0x4FE9F0 (the race's setups call it: 0x526956, 0x5324FA, 0x540424, 0x540718): its
 * count is the track's table entry for the traffic level [0x89E7A8] (movsx edi, byte [... + level*2 + 0xEC]), at most
 * 16 (cmp edi,0x10 at 0x4FEA51), and that many are added.  With a career race being set up ([0x863470], Career +0x8730)
 * the count goes through traffic_count: x CareerTraffic / 100 (free roam and LAN races as the game has them).  No log
 * in there (the game's floating point, phase 6): guards_win_frame writes it */
#define TRAFFIC_SITE 0x004fea51u         /* cmp edi,0x10; jl 0x4FEA5B */
#define TRAFFIC_ON   0x004fea5bu
static volatile int g_traffic_pct = 100, g_traffic_from = -1, g_traffic_to = -1;
static int __attribute__((cdecl, used)) traffic_count(int n){
    if (g_traffic_pct == 100 || !MR32(0x00863470u)) return n;
    int m = n * g_traffic_pct / 100;
    g_traffic_from = n; g_traffic_to = m < 16 ? m : 16;
    return m;
}
static void traffic_install(void){
    static const uint8_t want[5] = { 0x83, 0xff, 0x10, 0x7c, 0x05 };
    uint8_t *site = (uint8_t *)(uintptr_t)TRAFFIC_SITE, *t = g_page + 256;
    DWORD old;
    const char *v = roam_opt("NFSU2_ROAM_CAREERTRAFFIC");
    int pct = 100; if (v && *v >= '0' && *v <= '9'){ pct = 0; for (; *v >= '0' && *v <= '9' && pct < 1000; v++) pct = pct * 10 + (*v - '0'); }
    if (pct == 100) return;
    if (!ok(TRAFFIC_SITE, sizeof want) || memcmp(site, want, sizeof want)){
        roam_log("[guard] the race's traffic count (0x%08x) is not the game's (another mod?): CareerTraffic not applied\n", TRAFFIC_SITE);
        return;
    }
    if (!VirtualProtect(g_page, sizeof g_page, PAGE_EXECUTE_READWRITE, &old)) return;
    uint8_t *q = t;
    *q++ = 0x60;                                                   /* pushad */
    *q++ = 0x57;                                                   /* push edi (the count) */
    *q++ = 0xe8; put_rel(q, (uint32_t)(uintptr_t)traffic_count); q += 4;
    *q++ = 0x83; *q++ = 0xc4; *q++ = 0x04;                         /* add esp, 4 */
    *q++ = 0x89; *q++ = 0x04; *q++ = 0x24;                         /* mov [esp], eax: pushad's edi */
    *q++ = 0x61;                                                   /* popad */
    *q++ = 0x83; *q++ = 0xff; *q++ = 0x10;                         /* cmp edi, 0x10 */
    *q++ = 0x0f; *q++ = 0x8c; put_rel(q, TRAFFIC_ON); q += 4;      /* jl 0x4FEA5B */
    *q++ = 0xbf; *q++ = 0x10; *q++ = 0; *q++ = 0; *q++ = 0;        /* mov edi, 16 */
    *q++ = 0xe9; put_rel(q, TRAFFIC_ON); q += 4;                   /* jmp 0x4FEA5B */
    FlushInstructionCache(GetCurrentProcess(), t, (SIZE_T)(q - t));
    if (!VirtualProtect(site, sizeof want, PAGE_EXECUTE_READWRITE, &old)) return;
    site[0] = 0xe9; put_rel(site + 1, (uint32_t)(uintptr_t)t);
    VirtualProtect(site, sizeof want, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof want);
    g_traffic_pct = pct;
    roam_log("[guard] CareerTraffic = %d %%: a career race's traffic cars x %d.%02d (0x%08x)\n", pct, pct / 100, pct % 100, TRAFFIC_SITE);
}

/* ---- the Cars pool's extra memory: given back only when it is free (fix phase 9: the 0x5CE83E crash) --------------
 * A track load raises the Cars pool to free roam's size (G_CARPOOL_KB) by one block from the main pool (0x610750: car
 * loader +0x6C, +0x70), and the track's unload (0x580EF0) has the car loader give it back (0x63A6F0): it frees its
 * unused rides, takes the block out of the pool (0x43FBA0) and FREES IT TO THE MAIN POOL (0x440540) whatever the
 * removal did.  With rides still in the block (friends' cars: the World is gone, their rides are unloaded a frame
 * later; and the pool's own fragments) the removal does nothing and the block is the main pool's and the Cars
 * pool's at once; the track's next unloads (InGameMemoryFile, InGameCommon.bun: 0x580E90, right after) and loads
 * then write over car models still linked in the renderer's mesh list, and a solid of InGameCommon.bun whose mesh
 * pointer now points at a freed car mesh crashes in 0x5CE7D0 (0x5CE83E: the laptop, 08:37 and 08:59, a friend's
 * stock-look SENTRAB12 in slot 0; the desktop, 14:10 and 14:17, a roambot's SENTRAB12, 2026-11-07).  The Mac port
 * has had the cure since 10-21 (runtime/roam.c, sub_0063a6f0 / sub_00610750): the plugin's roam_game_loader_release
 * unloads friends' rides and the loader's unused rides first and lets the game's give-back run only when the block
 * is one free block again; otherwise it stays in the pool, and the next track load adds nothing while the pool is
 * big enough (roam_game_pool_grow).  The Windows plugin never installed those two hooks; here they are.
 * 0x63A6F0: thiscall CarLoader, no arguments (ret).  0x610750: thiscall CarLoader (size), ret 4. */
#define GIVEBACK_SITE 0x0063a6f0u   /* push ecx; push ebp; push esi; mov esi,ecx; mov ebp,[esi+0x6C] */
#define GROW_SITE     0x00610750u   /* push esi; mov esi,[esp+8]; push edi; mov edi,ecx */
int roam_game_loader_release(CPU *c);            /* runtime/roam_game.c: 1 = run the game's own */
int roam_game_pool_grow(uint32_t size);           /* runtime/roam_game.c: 1 = let it add the block */
static int __attribute__((cdecl, used)) giveback_ask(void){ return roam_game_loader_release(0); }
static int __attribute__((cdecl, used)) grow_ask(uint32_t size){ return roam_game_pool_grow(size); }
static void carpool_install(void){
    static const uint8_t want_g[8] = { 0x51, 0x55, 0x56, 0x8b, 0xf1, 0x8b, 0x6e, 0x6c };
    static const uint8_t want_a[8] = { 0x56, 0x8b, 0x74, 0x24, 0x08, 0x57, 0x8b, 0xf9 };
    uint8_t *sg = (uint8_t *)(uintptr_t)GIVEBACK_SITE, *sa = (uint8_t *)(uintptr_t)GROW_SITE;
    DWORD old;
    if (!ok(GIVEBACK_SITE, 8) || memcmp(sg, want_g, 8) || !ok(GROW_SITE, 8) || memcmp(sa, want_a, 8)){
        roam_log("[guard] the car loader's give-back (0x%08x) or pool growth (0x%08x) is not the game's (another mod?): "
                 "the Cars pool's extra memory may be freed with rides in it\n", GIVEBACK_SITE, GROW_SITE);
        return;
    }
    if (!VirtualProtect(g_page, sizeof g_page, PAGE_EXECUTE_READWRITE, &old)) return;
    uint8_t *t = g_page + 512, *q = t;                             /* the give-back */
    *q++ = 0x51; *q++ = 0x52;                                      /* push ecx; push edx */
    *q++ = 0xe8; put_rel(q, (uint32_t)(uintptr_t)giveback_ask); q += 4;
    *q++ = 0x5a; *q++ = 0x59;                                      /* pop edx; pop ecx */
    *q++ = 0x85; *q++ = 0xc0;                                      /* test eax, eax */
    *q++ = 0x74; *q++ = 10;                                        /* jz: ours did it all -> ret */
    memcpy(q, want_g, 5); q += 5;                                  /* the 5 bytes moved */
    *q++ = 0xe9; put_rel(q, GIVEBACK_SITE + 5u); q += 4;           /* jmp 0x63A6F5 */
    *q++ = 0xc3;                                                   /* ret */
    uint8_t *u = g_page + 576; q = u;                              /* the growth */
    *q++ = 0x51; *q++ = 0x52;                                      /* push ecx; push edx */
    *q++ = 0xff; *q++ = 0x74; *q++ = 0x24; *q++ = 0x0c;            /* push [esp+0xC]: the size */
    *q++ = 0xe8; put_rel(q, (uint32_t)(uintptr_t)grow_ask); q += 4;
    *q++ = 0x83; *q++ = 0xc4; *q++ = 0x04;                         /* add esp, 4 */
    *q++ = 0x5a; *q++ = 0x59;                                      /* pop edx; pop ecx */
    *q++ = 0x85; *q++ = 0xc0;                                      /* test eax, eax */
    *q++ = 0x74; *q++ = 10;                                        /* jz: the pool is big enough -> ret 4 */
    memcpy(q, want_a, 5); q += 5;                                  /* the 5 bytes moved */
    *q++ = 0xe9; put_rel(q, GROW_SITE + 5u); q += 4;               /* jmp 0x610755 */
    *q++ = 0xc2; *q++ = 0x04; *q++ = 0x00;                         /* ret 4 */
    FlushInstructionCache(GetCurrentProcess(), g_page, sizeof g_page);
    if (!VirtualProtect(sg, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    sg[0] = 0xe9; put_rel(sg + 1, (uint32_t)(uintptr_t)t);
    VirtualProtect(sg, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), sg, 5);
    if (!VirtualProtect(sa, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    sa[0] = 0xe9; put_rel(sa + 1, (uint32_t)(uintptr_t)u);
    VirtualProtect(sa, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), sa, 5);
    roam_log("[guard] the Cars pool's extra memory (0x%08x, 0x%08x): friends' rides go first, and it is given back only once "
             "it is one free block (else it stays, and the next track adds none)\n", GIVEBACK_SITE, GROW_SITE);
}

void guards_win_frame(void){
    if (g_traffic_from >= 0){
        roam_log("[guard] a career race's traffic: %d car(s) -> %d (CareerTraffic = %d %%)\n", g_traffic_from, g_traffic_to, g_traffic_pct);
        g_traffic_from = -1;
    }
    LONG d = g_desc_hits;
    if (d != g_desc_logged){ g_desc_logged = d; roam_log("[guard] AddOnStockRace: an add-on car's LAN description went with stock parts (%ld so far)\n", (long)d); }
    LONG n = g_snd_hits;
    if (n == g_snd_logged) return;
    g_snd_logged = n;
    roam_log("[guard] a sound whose bank is gone (its slot empty): skipped, %ld time(s) so far (it would have crashed the game)\n", (long)n);
}
