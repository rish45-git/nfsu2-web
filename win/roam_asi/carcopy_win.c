/* carcopy_win.c -- a car the front end shows, copied into the career's garage (NFSU2FreeRoam.asi only; Rishi's ask,
 * 2026-11-07: "I need this car in the desktop save file", the RacingHart Supra the main menu shows a profile without a
 * career car of its own).
 *
 * The game's cars are records of one class (vtable 0x79B12C; FEDatabase's 91, table CARS0 0x83AD90 +0x9BD8): the
 * customized ones, the 6 presets, and the career's 5 (+0x5954, 0x7F8 each: +8 its handle "CAREER_SLOT_<n>", +0x14 4,
 * +0x7F0 the money spent on it, +0x7F4 locked, +0x7F5 a car in it).  A record's car is its body at +0x18 (to +0x434,
 * the size of a customized one): the builder 0x516CA0 hands it to 0x516AC0, which makes the ride from it.  The other
 * way, 0x516B90 (a customized record from a car) makes the body from a ride with 0x503950 (thiscall body (ride)),
 * after the body's constructor 0x516A60, then resets the 5 drift setups at +0x360 (0x29 each, 0x59AFE0 (-1)).
 *
 * The front end's car is a ride of its own: 0x4A7890(n) gives 0x836750 (n 1) or 0x8371B0, the main menu's car (a
 * profile with no career car: the Supra, type 15, 77 parts, read at 10:30).  So:
 *  - CarSaveKey, in the main menu: a body made from that ride, as 0x516B90 makes one, goes to <the .ini's name>.car2
 *    (the body's bytes, after a header: CC_MAGIC, its size, the car type);
 *  - CarPutKey, in the main menu with a profile: that body goes into the career's first slot with no car in it (one
 *    still locked is opened); the slot's handle, flags, money and its own parts (+0x434 on) stay.  The career drives
 *    its old car until you pick the new one in the garage; the game's next save keeps it (Save in the main menu, or
 *    the career's autosave). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "roam_host_win.h"

#define FE_RIDE        0x008371b0u       /* the main menu's car (0x4A7890(n != 1)) */
#define G_GAMEMODE     0x00850078u       /* 1 career, 2 main menu, 0x200 LAN */
#define G_P1_NAME      0x0083a9e0u       /* player 1's profile name (FEDatabase +0x10) */
#define CARS0          0x0083ad90u
#define C_SLOTS        0x5954u
#define REC_SZ         0x7f8u
#define REC_VT         0x0079b12cu
#define BODY_AT        0x18u
#define BODY_END       0x434u
#define BODY_N         (BODY_END - BODY_AT)
#define FN_BODY_CTOR   0x00516a60u       /* thiscall body */
#define FN_BODY_RIDE   0x00503950u       /* thiscall body (ride) */
#define FN_SETUP_RESET 0x0059afe0u       /* thiscall setup (-1) */
#define G_CARTYPES     0x008a1cccu       /* CarTypeInfo[] (0x890): +0 name, +0xD0 its hash */
#define CC_MAGIC       0x32524143u       /* "CAR2" */

int roam_win_file_read(const char *ext, char *buf, int cap);
int roam_win_file_write(const char *ext, const char *buf, int n);
static int ok(uint32_t a, uint32_t n){ return a && roam_win_committed(a) && roam_win_committed(a + n); }

static uint8_t g_rec[REC_SZ] __attribute__((aligned(16)));   /* a scratch record the body is made in */

static int in_main_menu(void){ return ok(G_GAMEMODE, 4) && MR32(G_GAMEMODE) == 2u; }
static void type_name(uint32_t type, char out[24]){
    uint32_t t = ok(G_CARTYPES, 4) ? MR32(G_CARTYPES) : 0;
    out[0] = 0;
    if (!t || type >= 128u || !ok(t + type * 0x890u, 24)) return;
    int i = 0; for (; i < 23 && MR8(t + type * 0x890u + (uint32_t)i) >= 0x20 && MR8(t + type * 0x890u + (uint32_t)i) < 0x7f; i++) out[i] = (char)MR8(t + type * 0x890u + (uint32_t)i);
    out[i] = 0;
}
static void profile(char out[33]){
    int i = 0; for (; i < 32 && MR8(G_P1_NAME + (uint32_t)i) >= 0x20 && MR8(G_P1_NAME + (uint32_t)i) < 0x7f; i++) out[i] = (char)MR8(G_P1_NAME + (uint32_t)i);
    out[i] = 0;
}

void carcopy_win_save(void){
    if (!in_main_menu()){ roam_log("[car] CarSaveKey: only in the main menu (the car it shows is the one saved)\n"); return; }
    uint32_t type = ok(FE_RIDE, 0x900) ? MR32(FE_RIDE) : 0xffffffffu;
    int parts = 0;
    for (uint32_t s = 0; s < 170u && type < 128u; s++) if (MR32(FE_RIDE + 0x590u + 4u * s)) parts++;
    char tn[24]; type_name(type, tn);
    if (type >= 128u || !parts || !tn[0]){ roam_log("[car] CarSaveKey: the main menu shows no car (type %u, %d parts)\n", type, parts); return; }
    uint32_t r = (uint32_t)(uintptr_t)g_rec, one = (uint32_t)-1;
    memset(g_rec, 0, sizeof g_rec);
    roam_win_call(FN_BODY_CTOR, r + BODY_AT, 0, 0, 0);
    uint32_t a = FE_RIDE;
    roam_win_call(FN_BODY_RIDE, r + BODY_AT, 0, 1, &a);
    for (uint32_t k = 0; k < 5u; k++) roam_win_call(FN_SETUP_RESET, r + 0x360u + k * 0x29u, 0, 1, &one);
    static uint8_t out[12 + BODY_N];
    uint32_t hdr[3] = { CC_MAGIC, BODY_N, type };
    memcpy(out, hdr, sizeof hdr);
    memcpy(out + sizeof hdr, g_rec + BODY_AT, BODY_N);
    if (!roam_win_file_write("car2", (const char *)out, (int)sizeof out)){ roam_log("[car] CarSaveKey: NFSU2FreeRoam.car2 could not be written\n"); return; }
    roam_log("[car] CarSaveKey: the main menu's car saved: %s (type %u, %d parts), %u bytes in NFSU2FreeRoam.car2; its first words %08x %08x %08x %08x\n",
             tn, type, parts, (unsigned)BODY_N, MR32(r + BODY_AT), MR32(r + BODY_AT + 4u), MR32(r + BODY_AT + 8u), MR32(r + BODY_AT + 12u));
}

void carcopy_win_put(void){
    if (!in_main_menu()){ roam_log("[car] CarPutKey: only in the main menu\n"); return; }
    char pn[33]; profile(pn);
    if (!pn[0]){ roam_log("[car] CarPutKey: no profile loaded\n"); return; }
    static uint8_t in[12 + BODY_N + 1];
    int n = roam_win_file_read("car2", (char *)in, (int)sizeof in);
    uint32_t hdr[3]; if (n >= 12) memcpy(hdr, in, sizeof hdr);
    if (n != (int)(12 + BODY_N) || hdr[0] != CC_MAGIC || hdr[1] != BODY_N){ roam_log("[car] CarPutKey: no saved car (NFSU2FreeRoam.car2 missing or not this build's: %d bytes)\n", n); return; }
    int slot = -1, opened = 0;
    for (int pass = 0; pass < 2 && slot < 0; pass++)
        for (int i = 0; i < 5; i++){
            uint32_t rec = CARS0 + C_SLOTS + (uint32_t)i * REC_SZ;
            if (!ok(rec, REC_SZ) || MR32(rec) != REC_VT || MR32(rec + 0x14u) != 4u || MR8(rec + 0x7f5u)) continue;
            if (pass == 0 && MR8(rec + 0x7f4u)) continue;            /* (an open free slot first, then a locked one) */
            slot = i; opened = MR8(rec + 0x7f4u) != 0; break;
        }
    char tn[24]; type_name(hdr[2], tn);
    if (slot < 0){ roam_log("[car] CarPutKey: %s's career has no free slot of its 5\n", pn); return; }
    uint32_t rec = CARS0 + C_SLOTS + (uint32_t)slot * REC_SZ;
    memcpy((void *)(uintptr_t)(rec + BODY_AT), in + 12, BODY_N);
    MR8(rec + 0x7f4u) = 0; MR8(rec + 0x7f5u) = 1;
    roam_log("[car] CarPutKey: %s (type %u) put into %s's career slot %d%s (handle %08x): pick it in the garage, then save the profile\n",
             tn[0] ? tn : "the saved car", hdr[2], pn, slot + 1, opened ? ", opened for it" : "", MR32(rec + 8u));
}
