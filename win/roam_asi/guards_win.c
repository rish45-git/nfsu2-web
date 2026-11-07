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
}
/* roam_asi.c, each frame: the guard's count when it grew */
void guards_win_frame(void){
    LONG n = g_snd_hits;
    if (n == g_snd_logged) return;
    g_snd_logged = n;
    roam_log("[guard] a sound whose bank is gone (its slot empty): skipped, %ld time(s) so far (it would have crashed the game)\n", (long)n);
}
