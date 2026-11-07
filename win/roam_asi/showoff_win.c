/* showoff_win.c -- the cars' open parts (hood, trunk, doors) for NFSU2FreeRoam.asi: each friend's car shows its own,
 * and the player's are sent to friends.  The native counterpart of runtime/showoff.c (the browser port's), without
 * its O key: the player opens parts with the game's own way or a mod's (the AnimationController mod's keys, which call
 * the game's 0x61F820, the cover shoot's).
 *
 * The game keeps a part's state per car TYPE: a node (0x28 bytes) on its type's animation bank's list (bank +0xC: the
 * list's head, next and previous; node +0xC the part, 0 hood, 1 trunk, 2 left door, 3 right door; +0x10 the state, 1
 * closed, 2 opening, 0 open, 3 closing; +0x20 the car type's hash), and drawing a car (0x620AE0, its lights 0x615A10)
 * poses its parts by its type's list.  So a part opened on the player's car opened on every car of its type, friends'
 * cars too (a player's report, 2026-11-07: both COROLLAs' doors open on one screen, none on the other's).
 *
 * Here:
 *  - showoff_win_mine: the player's open parts as RC_OPEN_* bits, read from its type's list (what the player has open
 *    or opening), sent with its car's state (roam_game.c local_state);
 *  - showoff_win_car: each friend's car shown (roam_game.c showoff_peers, once a frame) and the parts its player has
 *    open: nodes of its own (in g_nodes, made and moved by the game's own 0x433A80 and 0x433D50, from the frame step),
 *    opened and closed to match;
 *  - the drawing: while a friend's car is drawn, its type's list holds that car's own nodes only (none: all closed),
 *    and the list is as it was again right after (0x620AE0 and 0x615A10 go through the plugin first: a jump at each
 *    one's start, the 6 bytes it replaces run from a block of ours, then on into the game's).
 * The game has one car type's animations in at a time (the player's own); a friend's car of another type gets its
 * type's loaded once one of its parts opens (bank_ready, as runtime/showoff.c), and 0x438140, the game's throwing away
 * of every bank, is ours (hk_banks_drop: our files unloaded first).  With the animations thrown away (a garage, a
 * shop, a race trip) and loaded again, the bank is a new one: the friend's nodes are closed then and opened again
 * (showoff.c drop_ours: a node turned while its type has no bank calls through 0). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "roam_host_win.h"
#define RC_OPEN_ALL    0x0fu         /* roam_core.h RC_OPEN_*: the hood 1, the trunk 2, the left door 4, the right door 8 */

#define G_BANKS        0x00827c9cu   /* the animation banks: a list, each entry 4 into its bank (0x433A30); the end 0x827C98 */
#define G_PLAYER1      0x008900acu   /* Player*: +4 its car (SPLIT_PLAYERS_INDEX, the native address) */
#define G_FLOW         0x008654a4u   /* 6: in the world */
#define FN_NODE_INIT   0x00433a80u   /* thiscall node (type, speed, part, kind, style 22), ret 0x14 */
#define FN_NODE_GO     0x00433d50u   /* cdecl (node, 2 open / 3 close) */
#define FN_ATTR        0x0060fef0u   /* thiscall part (attribute's hash, 0) -> the attribute (+4 its value) */
#define FN_HASH        0x0043db50u   /* cdecl (string) */
#define FN_HASH2       0x0043db80u   /* cdecl (string, seed) */
#define S_ATTR_DOOR    0x007a8750u
#define S_ATTR_HOOD    0x007a8780u
#define S_STYLE22      0x007a8770u
#define S_STYLE22CF    0x007a875cu
#define CAR_RI         0x554u        /* car: its render info */
#define RI_RIDE        0x04u
#define RI_TYPE        0x08u         /* render info: its CarTypeInfo (+0 the name, +0xD0 its hash) */
#define RIDE_HOOD      0x5b4u
#define RIDE_WIDE      0x5a8u
#define RIDE_DOORS     0x810u
#define NODE_SZ        0x28u
#define SO_FRIENDS     64            /* roam_game.c's slots */
#define DRAW_FN        0x00620ae0u   /* the car's drawing: thiscall render info, 12 arguments (ret 0x30) */
#define LIGHTS_FN      0x00615a10u   /* the car's lights: thiscall render info, 3 arguments (ret 0xC) */

static int ok(uint32_t a, uint32_t n){ return a && roam_win_committed(a) && roam_win_committed(a + n); }
static uint32_t rd32(uint32_t a){ return ok(a, 3) ? MR32(a) : 0u; }
static uint32_t call(uint32_t fn, uint32_t ecx, int n, const uint32_t *a){ return roam_win_call(fn, ecx, 0, n, a); }

typedef struct {
    uint32_t car, ri, type, bank;     /* its car, render info, type's hash, and that type's bank when its nodes were made */
    uint32_t node[4];                 /* each part's node (0: none yet) */
    uint8_t  want, has;               /* RC_OPEN_*: what its player has open; what we opened */
} Car;
static Car g_car[SO_FRIENDS];
static uint8_t g_nodes[SO_FRIENDS * 4 * NODE_SZ] __attribute__((aligned(16)));
static int g_on;                      /* the drawing hooks are in */
static int g_said_mine;
static const char *const k_part[4] = { "hood", "trunk", "left door", "right door" };

/* the bank of a car type (0x433A30's walk; 0: its animations are not in) */
static uint32_t bank_of(uint32_t type){
    uint32_t e = rd32(G_BANKS), b = e ? e - 4u : 0;
    for (int n = 0; type && b && b != G_BANKS - 4u && n < 256 && ok(b, 0x28); n++){
        if (MR32(b + 0x24u) == type) return b;
        e = MR32(b + 4u); b = e ? e - 4u : 0;
    }
    return 0;
}
static uint32_t type_of_ri(uint32_t ri){
    uint32_t ti = ok(ri, 0x10) ? rd32(ri + RI_TYPE) : 0;
    return ok(ti, 0xd4) ? MR32(ti + 0xd0u) : 0;
}

/* ---- the player's open parts, for friends ------------------------------------------------------------------- */
uint8_t showoff_win_mine(void){
    if (rd32(G_FLOW) != 6u) return 0;
    uint32_t pl = rd32(G_PLAYER1), car = pl ? rd32(pl + 4u) : 0, ri = ok(car, 0x560) ? rd32(car + CAR_RI) : 0;
    uint32_t b = bank_of(type_of_ri(ri));
    if (!b) return 0;
    uint8_t open = 0;
    uint32_t head = b + 0xcu, n = rd32(head);
    for (int i = 0; n && n != head && i < 64 && ok(n, NODE_SZ); i++, n = MR32(n)){
        uint32_t part = MR32(n + 0xcu), st = MR32(n + 0x10u);
        if (part < 4u && st != 1u && st != 3u) open |= (uint8_t)(1u << part);   /* opening or open */
    }
    if (open && !g_said_mine){ g_said_mine = 1; roam_log("[showoff] my car's open parts go to friends (0x%x, from the game's list)\n", open); }
    return open;
}

/* ---- friends' cars -------------------------------------------------------------------------------------------- */
/* a part's kind as the game picks it (runtime/showoff.c so_kind) */
static uint32_t so_kind(uint32_t ri, int part, uint32_t *mark){
    uint32_t ride = rd32(ri + RI_RIDE), type = type_of_ri(ri), kind = 0;
    *mark = 0;
    if (!ok(ride, 0x8f0)) return 0;
    if (part == 0){
        uint32_t h = rd32(ride + RIDE_HOOD);
        if (!ok(h, 0xe) || (MR8(h + 5u) & 0x1fu) != 5u) return 0;
        uint32_t s = S_ATTR_HOOD, ah = call(FN_HASH, 0, 1, &s), aa[2] = { ah, 0 }, at = call(FN_ATTR, h, 2, aa);
        kind = ok(at, 8) && MR32(at + 4u) ? 4u : 2u;
        uint32_t key = (uint32_t)MR16(h + 2u) << 16 | MR16(h), s1[2] = { S_STYLE22, type }, s2[2] = { S_STYLE22CF, type };
        *mark = key == call(FN_HASH2, 0, 2, s1) || key == call(FN_HASH2, 0, 2, s2);
        return kind;
    }
    if (part == 1) return 0;
    uint32_t d = rd32(ride + RIDE_DOORS);
    if (!ok(d, 0xe)) return 0;
    uint32_t s = S_ATTR_DOOR, ah = call(FN_HASH, 0, 1, &s), aa[2] = { ah, 0 }, at = call(FN_ATTR, d, 2, aa);
    if (!ok(at, 8)) return 0;
    kind = MR32(at + 4u);
    if (kind == 1u){
        uint32_t w = rd32(ride + RIDE_WIDE);
        if (ok(w, 6)){ uint8_t b = MR8(w + 5u); if ((b >> 5) && (b & 0x1fu)) kind = (b & 0x1fu) + 2u; }
    }
    return kind;
}
static void car_close(Car *k){ for (int p = 0; p < 4; p++) if (k->node[p]) MR32(k->node[p] + 0x10u) = 1u; k->has = 0; }

/* ---- other car types' animations (the player's list, item 12: doors between different car models) ---------------
 * The game has one type's part animations in at a time (its car loader's one record), so a friend's car of another
 * type showed its parts closed.  Here, as runtime/showoff.c bank_ready: once a friend's car of a type with no bank has
 * a part open, CARS\<type>\PARTS_ANIMATIONS.BIN is loaded for it (0x57CFB0, as 0x61C600 loads the game's; out in free
 * roam only), and its bank comes onto the list; not in after 10 s, it is loaded again, 3 tries at most.  The game
 * throws every bank away (0x438140 with 0, right after unloading its record's file: a garage, a shop, a cover shoot, a
 * race trip): ours are unloaded first, the game's way, and the walk reads the next bank before it deletes one (the
 * browser's crash of 2026-11-02 when ours were still on it: the walk went on from freed memory).  Their cars' parts
 * show closed until their banks are in again */
#define FN_LOAD        0x0057cfb0u   /* cdecl (file, 0, 0, 1, callback, its argument) -> the resource file */
#define FN_UNLOAD      0x00580e90u   /* cdecl (resource file) */
#define FN_NOP         0x004022c0u   /* ret: the load's callback */
#define FN_BANK_FREE   0x004380f0u   /* thiscall bank: lets go of its animations */
#define FN_BANKS_DROP  0x00438140u   /* cdecl (a car type's hash, 0: every bank) */
#define G_RACE         0x00890118u
#define G_CAREER_RACE  0x00863470u
#define G_RACEKIND     0x0089e7b0u
#define G_GAMEMODE     0x00850078u
#define SO_TYPES       16
typedef struct { uint32_t type, file; int st, tries; DWORD since; char path[64]; } Bank;
static Bank g_bank[SO_TYPES];
static int g_nbank, g_banks_hooked;
static int roam_world(void){
    uint32_t r = rd32(G_RACE), pl = rd32(G_PLAYER1);
    return rd32(G_FLOW) == 6u && ok(r, 0x28) && MR32(r + 0x24u) == 1u && (MR32(r) == 4u || MR32(r) == 6u) &&
           !rd32(G_CAREER_RACE) && rd32(G_RACEKIND) == 0u && !(rd32(G_GAMEMODE) & 0x208u) && pl && rd32(pl + 4u);
}
/* a car type's animations in (1), on the way (0), or not to be had (-1) */
static int bank_ready(uint32_t ri){
    uint32_t ti = ok(ri, 0x10) ? rd32(ri + RI_TYPE) : 0, type = ok(ti, 0xd4) ? MR32(ti + 0xd0u) : 0;
    if (!type) return -1;
    if (bank_of(type)) return 1;
    if (!g_banks_hooked || !roam_world()) return 0;
    Bank *k = 0;
    for (int i = 0; i < g_nbank && !k; i++) if (g_bank[i].type == type) k = &g_bank[i];
    if (!k){
        if (g_nbank == SO_TYPES) return -1;
        k = &g_bank[g_nbank++]; memset(k, 0, sizeof *k); k->type = type;
    }
    DWORD now = GetTickCount();
    if (k->st == 1 && now - k->since < 10000u) return 0;
    if (k->st){
        if (k->file) roam_log("[showoff] %s: the part animations are not in: loading them again (the first file left loaded)\n", k->path);
        k->file = 0; k->st = 0;
    }
    if (k->tries >= 3) return -1;
    char nm[24]; int i = 0;
    for (; i < 23 && ok(ti + (uint32_t)i, 1) && MR8(ti + (uint32_t)i); i++) nm[i] = (char)MR8(ti + (uint32_t)i);
    nm[i] = 0;
    if (!nm[0]) return -1;
    snprintf(k->path, sizeof k->path, "CARS\\%s\\PARTS_ANIMATIONS.BIN", nm);
    uint32_t a[6] = { (uint32_t)(uintptr_t)k->path, 0, 0, 1, FN_NOP, 0 };
    k->file = call(FN_LOAD, 0, 6, a);
    k->st = 1; k->since = now; k->tries++;
    roam_log("[showoff] %s: loading a friend's car type's part animations (%s)\n", k->path, k->file ? "under way" : "FAILED");
    return 0;
}
/* the game's 0x438140, ours in its place: our files unloaded first, then its walk, the next bank read first */
static void __attribute__((cdecl)) hk_banks_drop(uint32_t type){
    int n = 0;
    for (int i = 0; i < g_nbank; i++){
        Bank *k = &g_bank[i];
        if (!k->file || !bank_of(k->type)) continue;                  /* (still loading: not on the list yet) */
        uint32_t a = k->file;
        call(FN_UNLOAD, 0, 1, &a);
        k->file = 0; k->st = 0; k->tries = 0; n++;
    }
    if (n){
        for (int i = 0; i < SO_FRIENDS; i++) car_close(&g_car[i]);  /* (opened again once their banks are back) */
        roam_log("[showoff] the game throws the part animations away: %d of ours unloaded first\n", n);
    }
    uint32_t e = rd32(G_BANKS), b = e ? e - 4u : 0;
    for (int k = 0; b && b != G_BANKS - 4u && k < 4096 && ok(b, 0x28); k++){
        uint32_t ne = MR32(b + 4u), next = ne ? ne - 4u : 0;          /* read before the bank is deleted */
        if (!type || MR32(b + 0x24u) == type){
            uint32_t pv = MR32(b + 8u), nx = MR32(b + 4u), one = 1;
            MR32(pv) = nx; MR32(nx + 4u) = pv;
            call(FN_BANK_FREE, b, 0, 0);
            call(MR32(MR32(b)), b, 1, &one);
        }
        b = next;
    }
}
/* the car's parts to what its player has open (runtime/showoff.c car_apply): only with its type's bank in, and the
 * same bank as when its nodes were made (else closed at once, and made again) */
static void car_apply(Car *k, int slot){
    uint32_t b = bank_of(k->type);
    if (k->has && b != k->bank){ car_close(k); }
    if (!b && k->want) bank_ready(k->ri);                             /* (another type's: loaded for it) */
    if (!b){ k->bank = 0; return; }
    k->bank = b;
    if (k->want == k->has) return;
    for (int p = 0; p < 4; p++){
        uint8_t bit = (uint8_t)(1u << p);
        int w = (k->want & bit) != 0, h = (k->has & bit) != 0;
        if (w == h) continue;
        uint32_t mark = 0, kind = w ? so_kind(k->ri, p, &mark) : 0, n = k->node[p];
        if (w && (!n || MR32(n + 0x20u) != k->type || MR32(n + 0x10u) == 1u)){
            n = k->node[p] = (uint32_t)(uintptr_t)(g_nodes + ((uint32_t)slot * 4u + (uint32_t)p) * NODE_SZ);
            uint32_t a[5] = { k->type, 0x3f800000u, (uint32_t)p, kind, mark };
            call(FN_NODE_INIT, n, 5, a);                              /* opening, from now */
        } else if (!w && n){
            uint32_t a[2] = { n, 3u };
            call(FN_NODE_GO, 0, 2, a);                                /* closing */
        }
        k->has ^= bit;
        roam_log("[showoff] slot %d: %s %s (kind %u)\n", slot, k_part[p], w ? "opens" : "closes", kind);
    }
}
/* roam_game.c, once a frame: slot k's car shown (0: none) and the parts its player has open */
void showoff_win_car(int k, uint32_t car, uint8_t open){
    if (k < 0 || k >= SO_FRIENDS) return;
    Car *e = &g_car[k];
    uint32_t ri = ok(car, 0x560) ? rd32(car + CAR_RI) : 0, type = type_of_ri(ri);
    if (!type){
        if (e->car){ car_close(e); e->car = e->ri = e->type = e->bank = 0; e->want = 0; }
        return;
    }
    if (e->car != car || e->ri != ri || e->type != type){ car_close(e); e->car = car; e->ri = ri; e->type = type; e->bank = 0; }
    e->want = open & RC_OPEN_ALL;
    if (g_on) car_apply(e, k);
}

/* ---- the drawing: a friend's car with its own nodes on its type's list ------------------------------------ */
static Car *drawn(uint32_t ri, uint32_t *bank){
    for (int i = 0; i < SO_FRIENDS; i++){
        Car *k = &g_car[i];
        if (k->car && k->ri == ri){ uint32_t b = bank_of(k->type); if (!b) return 0; *bank = b; return k; }
    }
    return 0;
}
static void put_on(Car *k, uint32_t b, uint32_t *keep){
    uint32_t head = b + 0xcu, prev = head;
    keep[0] = MR32(head); keep[1] = MR32(head + 4u);
    if (k->bank == b)
        for (int p = 0; p < 4; p++){
            uint32_t n = k->node[p];
            if (!n || MR32(n + 0x10u) == 1u) continue;
            MR32(prev) = n; MR32(n + 4u) = prev; prev = n;
        }
    MR32(prev) = head; MR32(head + 4u) = prev;
}
static void take_off(uint32_t b, const uint32_t *keep){ MR32(b + 0xcu) = keep[0]; MR32(b + 0x10u) = keep[1]; }

typedef uint32_t (__attribute__((thiscall)) *draw_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                                      uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t (__attribute__((thiscall)) *lights_t)(uint32_t, uint32_t, uint32_t, uint32_t);
static draw_t g_draw;
static lights_t g_lights;
static uint32_t __attribute__((thiscall)) hk_draw(uint32_t ri, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                                                  uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9, uint32_t a10,
                                                  uint32_t a11){
    uint32_t b = 0, keep[2]; Car *k = drawn(ri, &b);
    if (!k) return g_draw(ri, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    put_on(k, b, keep);
    uint32_t r = g_draw(ri, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    take_off(b, keep);
    return r;
}
static uint32_t __attribute__((thiscall)) hk_lights(uint32_t ri, uint32_t a0, uint32_t a1, uint32_t a2){
    uint32_t b = 0, keep[2]; Car *k = drawn(ri, &b);
    if (!k) return g_lights(ri, a0, a1, a2);
    put_on(k, b, keep);
    uint32_t r = g_lights(ri, a0, a1, a2);
    take_off(b, keep);
    return r;
}

/* a jump at fn's start to mine; the n bytes it replaces (want: the game's, else another mod's: left alone) run from a
 * block of ours, then on at fn + n.  Returns the block (what calls the game's function), 0: not hooked */
static void *detour(uint32_t fn, const uint8_t *want, int n, void *mine, const char *what){
    uint8_t *site = (uint8_t *)(uintptr_t)fn;
    if (!ok(fn, (uint32_t)n) || memcmp(site, want, (size_t)n)){
        roam_log("[showoff] %s (0x%08x) is not the game's (another mod?): friends' cars may show the player's open parts\n", what, fn);
        return 0;
    }
    /* the blocks: a page of the plugin's own data made executable (asi_check allows VirtualProtect, not VirtualAlloc) */
    static uint8_t page[4096] __attribute__((aligned(4096)));
    static int used, prot;
    DWORD old;
    if (!prot){ if (!VirtualProtect(page, sizeof page, PAGE_EXECUTE_READWRITE, &old)) return 0; prot = 1; }
    if (used + 32 > (int)sizeof page) return 0;
    uint8_t *t = page + used;
    memcpy(t, site, (size_t)n);
    int32_t back = (int32_t)((fn + (uint32_t)n) - ((uint32_t)(uintptr_t)t + (uint32_t)n + 5u));
    t[n] = 0xE9; memcpy(t + n + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), t, 32);
    if (!VirtualProtect(site, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    used += 32;
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)mine - (fn + 5u));
    site[0] = 0xE9; memcpy(site + 1, &rel, 4);
    for (int i = 5; i < n; i++) site[i] = 0x90;
    VirtualProtect(site, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, (SIZE_T)n);
    roam_log("[showoff] %s (0x%08x) goes through the plugin (0x%08x)\n", what, fn, (uint32_t)(uintptr_t)mine);
    return t;
}
/* roam_asi.c, at the start (with the plugin on) */
void showoff_win_install(void){
    if (g_on) return;
    static const uint8_t w_draw[6] = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0 };     /* push ebp; mov ebp, esp; and esp, -16 */
    static const uint8_t w_lights[6] = { 0x83, 0xec, 0x0c, 0x56, 0x8b, 0xf1 };   /* sub esp, 12; push esi; mov esi, ecx */
    void *d = detour(DRAW_FN, w_draw, 6, (void *)hk_draw, "the cars' drawing");
    if (!d) return;
    g_draw = (draw_t)d;
    /* (with no lights' hook the drawing's stays: a friend's car's hood and trunk lights follow the type's list then,
     * its body its own; nothing breaks) */
    void *l = detour(LIGHTS_FN, w_lights, 6, (void *)hk_lights, "the cars' lights");
    if (l) g_lights = (lights_t)l;
    /* (with no hook here no other type's animations are loaded: a friend's car of another type shows its parts closed) */
    static const uint8_t w_drop[5] = { 0xa1, 0x9c, 0x7c, 0x82, 0x00 };              /* mov eax, [0x827C9C] */
    g_banks_hooked = detour(FN_BANKS_DROP, w_drop, 5, (void *)hk_banks_drop, "the animation banks' drop") != 0;
    g_on = 1;
}
