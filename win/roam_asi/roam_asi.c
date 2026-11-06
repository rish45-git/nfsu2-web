/* roam_asi.c -- NFSU2FreeRoam.asi: free-roam multiplayer for the original PC
 * game (SPEED2.EXE v1.2 North America), the Windows companion of the Mac
 * port's feature.
 *
 * An Ultimate ASI Loader plugin (scripts\NFSU2FreeRoam.asi, settings in
 * scripts\NFSU2FreeRoam.ini, log in scripts\NFSU2FreeRoam.log).  The game
 * side is the Mac port's own runtime/roam_game.c and the protocol is the
 * same runtime/roam_core.c, built for 32-bit x86 (roam_host_win.h); the
 * network is roam_win.c (Winsock).
 *
 * What it changes in the game: one 5-byte call at 0x581475 in GameLoop (the
 * second of two empty debug calls between World::DoTimestep and the camera,
 * audio and render; the first, 0x581470, is ExtraOptions' hook) now calls
 * roam_frame_thunk, which runs the frame step and then jumps on to the
 * call's previous target (the game's empty 0x4022C0, or another mod's hook
 * that was there first).  Nothing else is patched; the game's own functions
 * are called from that hook only, on the game's main thread, exactly as the
 * Mac port calls them.
 *
 * Fail closed: the plugin checks the exe in memory first (entry point, the
 * code of every game function it calls, the instructions that use the game
 * globals it reads, the hook site; win/roam_asi/roam_fp.h, generated from
 * SPEED2.EXE by tools/roam_fingerprint.py) and does nothing on any mismatch,
 * with the reason in the log, never a message box.  A fault inside the frame
 * step (in the plugin, or in a game function it called) is caught by a
 * vectored exception handler: the game continues, the plugin switches
 * itself off and says so in the log.
 *
 * No C runtime: KERNEL32 and WS2_32 only (roam_libc.c has the few C
 * functions); built freestanding by tools/build_roam_asi.sh.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
/* windows.h still defines the 16-bit far and near as nothing: roam_core.h has fields of those names */
#undef far
#undef near
#include <stdarg.h>
#include "roam_core.h"
#include "roam_plat.h"
#include "roam_fp.h"
#include "vsock.h"                                 /* win/online: the game's own Winsock on the server's virtual LAN */

int  vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int  snprintf(char *buf, size_t n, const char *fmt, ...);
typedef struct CPU CPU;                           /* the port's guest CPU: always 0 here */
void roam_game_frame(CPU *c);                     /* runtime/roam_game.c */
void roam_game_hud(CPU *c);
void mprace_frame(CPU *c);                        /* runtime/mprace.c */
int  mprace_owns_input(void);
void mpcredit_frame(CPU *c);                      /* mprace_win.c */
void roam_keys_mute(int on);
void roam_keys_frame(void);                       /* roam_keys.c */
int  roam_keys_vk(const char *s);
void roam_keys_hotkey(int vk, void (*fn)(void), const char *what);
void roam_keys_report(void);
void roam_win_server_next(void);                  /* roam_win.c: the server switch */
void roam_game_join_key(void);                    /* runtime/roam_game.c: the race lobby's JoinKey */
void roam_win_server_frame(void);
int  roam_win_servers(void);
void roam_game_report(void);
void roam_win_poll(void);                         /* roam_win.c */
void roam_plat_fault_unlock(void);
int  roam_plat_quiesced(void);

#ifndef ROAM_ASI_VERSION
#define ROAM_ASI_VERSION "1.0"
#endif
void roam_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define HOOK_SITE     0x00581475u                 /* call 0x4022C0 in GameLoop */
#define HOOK_ORIG_TGT 0x004022C0u                 /* the empty debug function (ret) */
#define EXE_BASE      0x00400000u
#define EXE_TEXT_LO   0x00401000u
#define EXE_TEXT_HI   0x00783000u
#define EXE_END       0x00932000u

static HMODULE g_self;
static uint32_t g_self_lo, g_self_hi;             /* the plugin's image */
static char    g_ini[MAX_PATH], g_logpath[MAX_PATH];
static int     g_inited, g_ready, g_dead;
static long    g_frame;
/* test aid (tests/asi_harness.c): NFSU2_ROAM_TEST_FAULT=N makes the first
 * game function called with an object in frame N get a bad one (a fault in
 * the game's code), -N a bad write in the plugin's own code in frame N */
static int     g_test_fault;
static volatile uint32_t g_test_fault_ptr = 0x10u;
/* test aid: NFSU2_ROAM_TEST_FAULT_SEH=N makes the plugin call the game's
 * Car::SetDriverType (0x5EBA30, which puts an SEH frame into FS:[0] first)
 * with a bad object in frame N: a fault inside a game function's SEH frame */
static int     g_test_fault_seh;

/* ---- the log --------------------------------------------------------------------------------- */
static HANDLE           g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_mu;
static uint32_t         g_log_bytes;
#define LOG_MAX (8u << 20)
void roam_log(const char *fmt, ...){
    if (g_log == INVALID_HANDLE_VALUE) return;
    char b[1100];
    SYSTEMTIME t; GetLocalTime(&t);
    int k = snprintf(b, sizeof b, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b + k, sizeof b - (size_t)k - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    n += k; if (n > (int)sizeof b - 3) n = (int)sizeof b - 3;
    /* the core and the game side end their lines with \n: Windows wants \r\n */
    if (n > 0 && b[n - 1] == '\n') n--;
    b[n++] = '\r'; b[n++] = '\n';
    EnterCriticalSection(&g_log_mu);
    DWORD w = 0;
    if (g_log_bytes < LOG_MAX){
        WriteFile(g_log, b, (DWORD)n, &w, 0);
        g_log_bytes += (uint32_t)n;
        if (g_log_bytes >= LOG_MAX){ const char *m = "... log full (8 MB): nothing more is written\r\n"; WriteFile(g_log, m, (DWORD)lstrlenA(m), &w, 0); }
    }
    LeaveCriticalSection(&g_log_mu);
}
static void log_open(void){
    InitializeCriticalSection(&g_log_mu);
    g_log = CreateFileA(g_logpath, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
}

/* ---- options: the environment (tests), then scripts\NFSU2FreeRoam.ini [FreeRoam] -------------- */
static const struct { const char *name, *key, *def; } OPTS[] = {
    { "NFSU2_ROAM",             "Enable",     "1" },  /* installing the plugin is the opt-in */
    { "NFSU2_ROAM_NAME",        "Name",       ""  },
    { "NFSU2_ROAM_PORT",        "Port",       ""  },
    { "NFSU2_ROAM_PEERS",       "Peers",      ""  },
    { "NFSU2_ROAM_DUMPCAR",     "DumpCar",    ""  },  /* test aid: save the car's record as NFSU2FreeRoam.car (for roambot) */
    { "NFSU2_ROAM_SERVER",      "Server",     ""  },  /* ws://host:port/lan: play on a server's virtual LAN, not the local one */
    { "NFSU2_ROAM_CLAIM",       "Claim",      ""  },  /* the token of a claimed name (the server's page makes it) */
    { "NFSU2_ROAM_SERVERS",     "Servers",    ""  },  /* name=url[|claim]; ...: the servers the ServerKey goes through */
    { "NFSU2_ROAM_SERVERKEY",   "ServerKey",  "F8" }, /* the next server of Servers (0: none) */
    { "NFSU2_ROAM_LABELS",      "Labels",     ""  },  /* names over friends' cars (default 1) */
    { "NFSU2_ROAM_JOINKEY",     "JoinKey",    "J" },  /* join the race lobby the top line offers, from afar (0: none) */
    { "NFSU2_ROAM_FARJOIN",     "FarJoin",    "1" },  /* hosting: anyone in the room may join from afar (0: friends only) */
    { "NFSU2_ROAM_PARTNER",     "Partner",    ""  },  /* the other player's name: as a friend in race lobbies (its win counts) */
    { "NFSU2_ROAM_TOHOST",      "ToHost",     "1" },  /* after a race of a lobby we joined: our car next to the host's (0: stay) */
    { "NFSU2_ROAM_ONLINE",      "Online",     ""  },  /* with Server: the game's Online and LAN menus on its virtual LAN (default 1) */
    { "NFSU2_ROAM_COLLIDE",     "Collide",    ""  },
    { "NFSU2_ROAM_DISCOVERY",   "Discovery",  ""  },
    { "NFSU2_ROAM_ROOM",        "Room",       ""  },
    { "NFSU2_ROAM_CREW",        "Crew",       ""  },
    { "NFSU2_ROAM_CREW_RGB",    "CrewColours", "" },
    { "NFSU2_ROAM_MAX_PLAYERS", "MaxPlayers", ""  },
    { "NFSU2_ROAM_MAX_CARS",    "MaxCars",    ""  },
    { "NFSU2_ROAM_RATE",        "Rate",       ""  },
    { "NFSU2_ROAM_DELAY",       "Delay",      ""  },
    { "NFSU2_ROAM_EXTRAP",      "Extrap",     ""  },
    { "NFSU2_ROAM_CARPOOL_KB",  "CarPoolKB",  ""  },
    { "NFSU2_ROAM_NETTHREAD",   "NetThread",  ""  },
    { "NFSU2_ROAM_LOG",         "Log",        "1" },
    { "NFSU2_ROAM_HARNESS",     "TestMode",   ""  },  /* test aid: loopback only, no broadcast */
    { "NFSU2_ROAM_POSELOG",     "PoseLog",    ""  },  /* test aid: a CSV of every pose */
};
#define NOPT (int)(sizeof OPTS / sizeof OPTS[0])
static char g_optval[NOPT][600];
static int  g_opt_loaded;
static void trim(char *s){
    /* a comment after the value: ';' or '#' at the start or after a blank
     * (as on the Mac: "Team#1" stays a name) */
    for (char *c = s; *c; c++) if ((*c == ';' || *c == '#') && (c == s || c[-1] == ' ' || c[-1] == '\t')){ *c = 0; break; }
    int n = lstrlenA(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    int i = 0; while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) for (int j = 0; ; j++){ s[j] = s[i + j]; if (!s[j]) break; }
}
static void opts_load(void){
    for (int i = 0; i < NOPT; i++){
        char *v = g_optval[i];
        DWORD n = GetEnvironmentVariableA(OPTS[i].name, v, sizeof g_optval[i]);
        if (n > 0 && n < sizeof g_optval[i]){ trim(v); continue; }
        GetPrivateProfileStringA("FreeRoam", OPTS[i].key, OPTS[i].def, v, sizeof g_optval[i], g_ini);
        trim(v);
    }
    g_opt_loaded = 1;
}
const char *roam_opt(const char *name){
    if (!g_opt_loaded) opts_load();
    for (int i = 0; i < NOPT; i++) if (!lstrcmpA(OPTS[i].name, name)) return g_optval[i][0] ? g_optval[i] : 0;
    /* other names: test aids from the environment only */
    static char ev[4][160]; static int k;
    char *v = ev[k++ & 3];
    DWORD n = GetEnvironmentVariableA(name, v, 160);
    return n > 0 && n < 160 ? v : 0;
}
static int opt_int(const char *name, int def){
    const char *v = roam_opt(name);
    if (!v) return def;
    int neg = *v == '-', x = 0, nd = 0;
    for (const char *p = v + neg; *p >= '0' && *p <= '9'; p++, nd++) x = x * 10 + (*p - '0');
    if (!nd) return (!lstrcmpiA(v, "on") || !lstrcmpiA(v, "yes") || !lstrcmpiA(v, "true")) ? 1 :
                    (!lstrcmpiA(v, "off") || !lstrcmpiA(v, "no") || !lstrcmpiA(v, "false")) ? 0 : def;
    return neg ? -x : x;
}

/* ---- guest memory: VirtualQuery, cached ------------------------------------------------------- */
static struct { uint32_t lo, hi; } g_vq[32];
static int g_vqn, g_vqnext;
static void vq_flush(void){ g_vqn = 0; g_vqnext = 0; }
int roam_win_committed(uint32_t a){
    if (a >= EXE_BASE && a < EXE_END) return 1;               /* the exe image is always there */
    for (int i = 0; i < g_vqn; i++) if (a >= g_vq[i].lo && a < g_vq[i].hi) return 1;
    if (a < 0x10000u) return 0;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
    DWORD p = mi.Protect;
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    if (!(p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
               PAGE_EXECUTE_WRITECOPY))) return 0;
    uint32_t lo = (uint32_t)(uintptr_t)mi.BaseAddress, hi = lo + (uint32_t)mi.RegionSize;
    if (hi < lo) hi = 0xFFFFFFFFu;
    int k = g_vqn < 32 ? g_vqn++ : (g_vqnext++ & 31);
    g_vq[k].lo = lo; g_vq[k].hi = hi;
    return 1;
}

/* ---- calling the game's code ---------------------------------------------------------------- */
/* roam_call_asm(fn, ecx, edx, n, args): pushes args[n-1] .. args[0], loads
 * ecx and edx, calls fn, and restores esp from ebp afterwards, so that
 * thiscall/stdcall (callee pops) and cdecl (caller pops) both come back
 * balanced; ebx, esi, edi, ebp are saved here as well */
uint32_t roam_call_asm(uint32_t fn, uint32_t ecx, uint32_t edx, int n, const uint32_t *args);
__asm__(
    ".text\n"
    ".globl _roam_call_asm\n"
    "_roam_call_asm:\n"
    "    pushl %ebp\n"
    "    movl  %esp, %ebp\n"
    "    pushl %ebx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    movl  20(%ebp), %eax\n"
    "    movl  24(%ebp), %esi\n"
    "1:  testl %eax, %eax\n"
    "    jle   2f\n"
    "    decl  %eax\n"
    "    pushl (%esi,%eax,4)\n"
    "    jmp   1b\n"
    "2:  movl  12(%ebp), %ecx\n"
    "    movl  16(%ebp), %edx\n"
    "    call  *8(%ebp)\n"
    "    leal  -12(%ebp), %esp\n"
    "    popl  %edi\n"
    "    popl  %esi\n"
    "    popl  %ebx\n"
    "    popl  %ebp\n"
    "    ret\n");
static uint32_t g_game_mxcsr, g_our_mxcsr = 0x1F80u;   /* MXCSR default: round to nearest, all masked */
static uint16_t g_game_fcw;
static uint32_t g_x87_popped;
uint32_t roam_win_mxcsr_default(void){ return 0x1F80u; }
uint32_t roam_win_call(uint32_t fn, uint32_t ecx, uint32_t edx, int n, const uint32_t *args){
    uint16_t sw0, sw1;
    if (g_test_fault > 0 && g_frame >= g_test_fault && ecx){ g_test_fault = 0; ecx = g_test_fault_ptr; }
    __asm__ volatile ("fnstsw %0" : "=m"(sw0));
    __asm__ volatile ("ldmxcsr %0" :: "m"(g_game_mxcsr));          /* the game's code, the game's MXCSR */
    uint32_t r = roam_call_asm(fn, ecx, edx, n, args);
    __asm__ volatile ("ldmxcsr %0" :: "m"(g_our_mxcsr));
    __asm__ volatile ("fnstsw %0" : "=m"(sw1));
    /* a function that returns a float leaves it in st(0): pop it, so the
     * x87 stack is as the game left it */
    int t0 = (sw0 >> 11) & 7, t1 = (sw1 >> 11) & 7, k = 0;
    while (t1 != t0 && k < 8){ __asm__ volatile ("fstp %%st(0)" ::: "st"); t1 = (t1 + 1) & 7; k++; }
    g_x87_popped += (uint32_t)k;
    return r;
}

/* ---- the frame hook ---------------------------------------------------------------------------- */
uint32_t g_roam_chain = HOOK_ORIG_TGT;            /* where the patched call went before */
uint32_t g_roam_resume_esp, g_roam_resume_ebp;   /* the fault guard's way back */
uint32_t g_roam_resume_seh;                       /* the thread's SEH chain head (FS:[0]) at the hook */
void roam_frame_thunk(void);
void roam_fault_resume(void);
void roam_asi_frame(void);
void roam_asi_fault(void);
__asm__(
    ".text\n"
    ".globl _roam_frame_thunk\n"
    "_roam_frame_thunk:\n"
    "    pushal\n"
    "    pushfl\n"
    "    movl  %esp, %ebp\n"
    "    andl  $-16, %esp\n"
    "    cld\n"
    "    movl  %esp, _g_roam_resume_esp\n"
    "    movl  %ebp, _g_roam_resume_ebp\n"
    "    movl  %fs:0, %eax\n"
    "    movl  %eax, _g_roam_resume_seh\n"
    "    call  _roam_asi_frame\n"
    "3:  movl  %ebp, %esp\n"
    "    popfl\n"
    "    popal\n"
    "    jmp   *_g_roam_chain\n"
    /* back from a fault: the game functions the plugin calls (CreateCar,
     * SetRideInfo, SetDriverType, SetPhysicsMode, ...) put an SEH frame on
     * the stack and into FS:[0] at their start; a fault inside them leaves
     * FS:[0] pointing below the stack the plugin resumes on, which is then
     * reused, and the next exception on this thread would walk a garbage
     * chain (with SEHOP: the process is ended).  The chain as it was at the
     * hook goes back first. */
    ".globl _roam_fault_resume\n"
    "_roam_fault_resume:\n"
    "    movl  _g_roam_resume_esp, %esp\n"
    "    movl  _g_roam_resume_ebp, %ebp\n"
    "    movl  _g_roam_resume_seh, %eax\n"
    "    movl  %eax, %fs:0\n"
    "    call  _roam_asi_fault\n"
    "    jmp   3b\n");
/* The HUD step: Player::Update's HUD part (0x600590) ends, after its call of the HUD update (0x4EC5A0), in
 * "pop edi; pop esi; pop ebx; ret" at 0x6005E0 (where the browser build hooks roam_game_hud too): those 4 bytes and
 * the nop after them are a jmp to roam_hud_thunk, which runs the step (the line at the top of free roam's HUD, the
 * names over friends' cars) and does the 4 instructions itself.  Its own way back from a fault, as the frame's. */
#define HUD_SITE      0x006005E0u
uint32_t g_roam_hud_esp, g_roam_hud_ebp, g_roam_hud_seh;
void roam_hud_thunk(void);
void roam_hud_resume(void);
void roam_asi_hud(void);
__asm__(
    ".text\n"
    ".globl _roam_hud_thunk\n"
    "_roam_hud_thunk:\n"
    "    pushal\n"
    "    pushfl\n"
    "    movl  %esp, %ebp\n"
    "    andl  $-16, %esp\n"
    "    cld\n"
    "    movl  %esp, _g_roam_hud_esp\n"
    "    movl  %ebp, _g_roam_hud_ebp\n"
    "    movl  %fs:0, %eax\n"
    "    movl  %eax, _g_roam_hud_seh\n"
    "    call  _roam_asi_hud\n"
    "4:  movl  %ebp, %esp\n"
    "    popfl\n"
    "    popal\n"
    "    popl  %edi\n"
    "    popl  %esi\n"
    "    popl  %ebx\n"
    "    ret\n"
    ".globl _roam_hud_resume\n"
    "_roam_hud_resume:\n"
    "    movl  _g_roam_hud_esp, %esp\n"
    "    movl  _g_roam_hud_ebp, %ebp\n"
    "    movl  _g_roam_hud_seh, %eax\n"
    "    movl  %eax, %fs:0\n"
    "    call  _roam_asi_fault\n"
    "    jmp   4b\n");

static volatile LONG g_guard;                     /* inside the frame step (or the HUD step: g_guard_hud) */
static int    g_guard_hud;
static DWORD  g_guard_tid;
static DWORD  g_fault_code; static uint32_t g_fault_eip, g_fault_addr; static int g_fault_rw;
static LONG CALLBACK roam_veh(EXCEPTION_POINTERS *ep){
    if (!g_guard || GetCurrentThreadId() != g_guard_tid) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_IN_PAGE_ERROR && code != EXCEPTION_DATATYPE_MISALIGNMENT &&
        code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED && code != EXCEPTION_INT_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t eip = (uint32_t)ep->ContextRecord->Eip;
    /* only faults in our code or in the game's: anything else (a system
     * DLL probing memory under its own __try) is not ours to catch */
    if (!((eip >= g_self_lo && eip < g_self_hi) || (eip >= EXE_TEXT_LO && eip < EXE_TEXT_HI))) return EXCEPTION_CONTINUE_SEARCH;
    g_guard = 0;
    g_fault_code = code; g_fault_eip = eip;
    g_fault_addr = ep->ExceptionRecord->NumberParameters >= 2 ? (uint32_t)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    g_fault_rw = ep->ExceptionRecord->NumberParameters >= 1 ? (int)ep->ExceptionRecord->ExceptionInformation[0] : -1;
    ep->ContextRecord->Eip = (DWORD)(uintptr_t)(g_guard_hud ? roam_hud_resume : roam_fault_resume);
    return EXCEPTION_CONTINUE_EXECUTION;
}
/* back from a fault: the game's FPU state, the plugin off for good */
void roam_asi_fault(void){
    __asm__ volatile ("fninit");
    __asm__ volatile ("fldcw %0" :: "m"(g_game_fcw));
    __asm__ volatile ("ldmxcsr %0" :: "m"(g_game_mxcsr));
    g_dead = 1;
    roam_plat_fault_unlock();
    roam_log("[roam] FAULT %08lx at %08x (%s %08x) in frame %ld, in %s: free-roam multiplayer is OFF until the game restarts; "
             "the game continues. Please send this log to the developers.\n", (unsigned long)g_fault_code, g_fault_eip,
             g_fault_rw == 1 ? "writing" : g_fault_rw == 8 ? "executing" : "reading", g_fault_addr, g_frame,
             g_fault_eip >= g_self_lo && g_fault_eip < g_self_hi ? "the plugin" : "a game function the plugin called");
    roam_plat_stop(1);                            /* BYE: friends remove this car at once */
}
long roam_win_frame_no(void){ return g_frame; }
/* the car's look record to <the .ini's name>.car (DumpCar = 1), for win/online/roambot.c */
void roam_win_dump_car(const uint8_t *b, int n){
    char path[MAX_PATH + 8]; int k = lstrlenA(g_ini);
    if (k < 5 || k > MAX_PATH) return;
    for (int i = 0; i < k - 4; i++) path[i] = g_ini[i];
    path[k - 4] = '.'; path[k - 3] = 'c'; path[k - 2] = 'a'; path[k - 1] = 'r'; path[k] = 0;
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(h, b, (DWORD)n, &w, 0); CloseHandle(h);
    roam_log("[roam] the car's record (%d bytes) saved to %s\n", n, path);
}
uint64_t roam_win_ns(void){
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart && (!QueryPerformanceFrequency(&f) || !f.QuadPart)) f.QuadPart = 1000;
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000000ull + (uint64_t)(c.QuadPart % f.QuadPart) * 1000000000ull / (uint64_t)f.QuadPart;
}
static uint32_t g_world_last;
static void server_dll_watch(void);         /* (below, with the LoadLibraryA hook) */
void roam_asi_frame(void){
    if (g_dead || !g_ready) return;
    __asm__ volatile ("stmxcsr %0" : "=m"(g_game_mxcsr));
    __asm__ volatile ("fnstcw %0" : "=m"(g_game_fcw));
    __asm__ volatile ("ldmxcsr %0" :: "m"(g_our_mxcsr));
    g_guard_tid = GetCurrentThreadId(); g_guard = 1;
    g_frame++;
    /* the World object changes at every track load: forget the memory map */
    uint32_t w = *(volatile uint32_t *)(uintptr_t)0x00890080u;
    if (w != g_world_last || (g_frame & 1023) == 0){ vq_flush(); g_world_last = w; }
    if (g_test_fault < 0 && g_frame == -g_test_fault) *(volatile uint32_t *)(uintptr_t)g_test_fault_ptr = 1u;
    if (g_test_fault_seh > 0 && g_frame == g_test_fault_seh){ uint32_t a = 0; roam_win_call(0x005EBA30u, g_test_fault_ptr, 0, 1, &a); }
    roam_win_poll();                              /* NetThread = 0, or no thread: receive here */
    roam_keys_frame();                            /* the hotkeys; the game's keyboard hooked (roam_keys.c) */
    mprace_frame(0);                              /* a race trip from free roam (runtime/mprace.c), its career win */
    mpcredit_frame(0);
    roam_keys_mute(mprace_owns_input());          /* (the trip drives the game's menus: the player's keys wait) */
    roam_game_frame(0);
    roam_win_server_frame();
    server_dll_watch();
    g_guard = 0;
    __asm__ volatile ("ldmxcsr %0" :: "m"(g_game_mxcsr));
}

/* the HUD step (roam_hud_thunk), on the game's thread, once a frame while a HUD updates */
static int g_hud_on;
void roam_asi_hud(void){
    if (g_dead || !g_ready || !g_hud_on) return;
    __asm__ volatile ("stmxcsr %0" : "=m"(g_game_mxcsr));
    __asm__ volatile ("fnstcw %0" : "=m"(g_game_fcw));
    __asm__ volatile ("ldmxcsr %0" :: "m"(g_our_mxcsr));
    g_guard_tid = GetCurrentThreadId(); g_guard_hud = 1; g_guard = 1;
    roam_game_hud(0);
    g_guard = 0; g_guard_hud = 0;
    __asm__ volatile ("ldmxcsr %0" :: "m"(g_game_mxcsr));
}

/* ---- the pose log (test aid) ------------------------------------------------------------------- */
typedef struct rh_file rh_file;
struct rh_file { HANDLE h; uint32_t n; char b[60000]; };
rh_file *rh_fopen(const char *path){
    static rh_file f;
    if (f.h && f.h != INVALID_HANDLE_VALUE) return 0;
    f.h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    f.n = 0;
    return f.h == INVALID_HANDLE_VALUE ? 0 : &f;
}
void rh_fflush(rh_file *f){ if (f && f->n){ DWORD w; WriteFile(f->h, f->b, f->n, &w, 0); f->n = 0; } }
int rh_fprintf(rh_file *f, const char *fmt, ...){
    if (!f) return 0;
    if (f->n > sizeof f->b - 600) rh_fflush(f);
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(f->b + f->n, sizeof f->b - f->n, fmt, ap);
    va_end(ap);
    if (n > 0) f->n += (uint32_t)n < sizeof f->b - f->n ? (uint32_t)n : (uint32_t)(sizeof f->b - f->n - 1);
    return n;
}

/* ---- the exe check ------------------------------------------------------------------------------- */
/* readable without a fault (at load there is no fault guard yet: a packed or
 * different exe must not crash the game here) */
static int readable(uint32_t a, uint32_t n){
    for (uint32_t p = a & ~0xFFFu; p < a + n; p += 0x1000u){
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((LPCVOID)(uintptr_t)p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return 0;
    }
    return 1;
}
static int exe_check(void){
    if ((uint32_t)(uintptr_t)GetModuleHandleA(0) != EXE_BASE){
        roam_log("[roam] the game is not loaded at 0x400000: not SPEED2.EXE v1.2\n"); return 0; }
    const uint8_t *mz = (const uint8_t *)(uintptr_t)EXE_BASE;
    if (!readable(EXE_BASE, 0x400)){ roam_log("[roam] the exe's header is not readable\n"); return 0; }
    uint32_t pe = *(const uint32_t *)(mz + 0x3C);
    if (mz[0] != 'M' || mz[1] != 'Z' || pe > 0x300 || *(const uint32_t *)(mz + pe) != 0x00004550u){
        roam_log("[roam] no PE header at 0x400000\n"); return 0; }
    uint32_t entry = *(const uint32_t *)(mz + pe + 0x28), stamp = *(const uint32_t *)(mz + pe + 8),
             size = *(const uint32_t *)(mz + pe + 0x50);
    roam_log("[roam] exe: entry point 0x%08x, time stamp 0x%08x, image size 0x%x\n", EXE_BASE + entry, stamp, size);
    if (EXE_BASE + entry != RF_ENTRY_VA){
        roam_log("[roam] entry point 0x%08x is not v1.2 North America's 0x%08x\n", EXE_BASE + entry, RF_ENTRY_VA); return 0; }
    int bad = 0, hooked = 0, nfn = 0, nref = 0;
    for (unsigned i = 0; i < sizeof RF / sizeof RF[0]; i++){
        const uint8_t *p = (const uint8_t *)(uintptr_t)RF[i].va;
        if (!readable(RF[i].va, RF[i].len)){ roam_log("[roam]   0x%08x %s: not readable\n", RF[i].va, RF[i].what); bad++; continue; }
        uint32_t crc = rc_crc32(p, RF[i].len);
        if (RF[i].kind == RF_FN) nfn++; else nref++;
        if (crc == RF[i].crc) continue;
        /* a function another mod has replaced with a jump to its own code
         * (Unlimiter does this with UpdatePartsEnabled): still the same
         * function to call */
        if (RF[i].kind == RF_FN && (p[0] == 0xE9 || (p[0] == 0xFF && p[1] == 0x25) || (p[0] == 0x68 && p[5] == 0xC3))){
            roam_log("[roam]   0x%08x %s: replaced by another mod (a jump at its start): fine\n", RF[i].va, RF[i].what);
            hooked++; continue;
        }
        roam_log("[roam]   0x%08x %s: different code (crc %08x, v1.2 NA has %08x)\n", RF[i].va, RF[i].what, crc, RF[i].crc);
        bad++;
    }
    if (bad){ roam_log("[roam] %d of %u checks differ: this is not SPEED2.EXE v1.2 (North America)\n", bad, (unsigned)(sizeof RF / sizeof RF[0])); return 0; }
    roam_log("[roam] SPEED2.EXE v1.2 (North America) recognised: %d game functions (%d replaced by other mods) and %d "
             "uses of game globals checked\n", nfn, hooked, nref);
    return 1;
}

/* ---- the hook ------------------------------------------------------------------------------------- */
static int hook_install(void){
    uint8_t *site = (uint8_t *)(uintptr_t)HOOK_SITE;
    if (!readable(HOOK_SITE, 5)) return 0;
    if (site[0] != 0xE8){
        roam_log("[roam] the frame hook site 0x%08x is %02x %02x %02x %02x %02x, not a call: another mod changed it? Plugin off\n",
                 HOOK_SITE, site[0], site[1], site[2], site[3], site[4]);
        return 0;
    }
    int32_t rel; memcpy(&rel, site + 1, 4);
    uint32_t tgt = HOOK_SITE + 5u + (uint32_t)rel;
    uint32_t mine = (uint32_t)(uintptr_t)roam_frame_thunk;
    if (tgt == mine){ roam_log("[roam] the frame hook is already installed\n"); return 1; }
    if (tgt != HOOK_ORIG_TGT) roam_log("[roam] another mod hooks 0x%08x too (it calls 0x%08x): it is called after this plugin's step\n", HOOK_SITE, tgt);
    g_roam_chain = tgt;
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)){ roam_log("[roam] VirtualProtect failed (%lu)\n", GetLastError()); return 0; }
    int32_t nrel = (int32_t)(mine - (HOOK_SITE + 5u));
    uint8_t code[5] = { 0xE8 }; memcpy(code + 1, &nrel, 4);
    memcpy(site, code, 5);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    roam_log("[roam] frame hook: call at 0x%08x now goes to the plugin (0x%08x), then on to 0x%08x\n", HOOK_SITE, mine, tgt);
    return 1;
}

static int hud_install(void){
    uint8_t *site = (uint8_t *)(uintptr_t)HUD_SITE;
    static const uint8_t want[5] = { 0x5F, 0x5E, 0x5B, 0xC3, 0x90 };
    if (!readable(HUD_SITE, 5)) return 0;
    if (memcmp(site, want, 5)){
        roam_log("[roam] the HUD hook site 0x%08x is %02x %02x %02x %02x %02x, not the game's: another mod changed it? "
                 "No line at the top of the HUD and no names over cars\n", HUD_SITE, site[0], site[1], site[2], site[3], site[4]);
        return 0;
    }
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)roam_hud_thunk - (HUD_SITE + 5u));
    uint8_t code[5] = { 0xE9 }; memcpy(code + 1, &rel, 4);
    memcpy(site, code, 5);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    roam_log("[roam] HUD hook: 0x%08x now jumps to the plugin (0x%08x)\n", HUD_SITE, (uint32_t)(uintptr_t)roam_hud_thunk);
    return 1;
}

/* ---- the game's network on the server's virtual LAN (Server = in the .ini) ---------------------------- */
/* SPEED2.EXE's imports of ws2_32 (by ordinal, WSAIoctl by name) and netapi32!Netbios now point at the plugin's
 * virtual Winsock (win/online/vsock_ws2.c): its Online menu talks to the server's stand-in for EA at 10.77.0.1, its LAN
 * screens to the other players on the switch, as in the browser.  Only the exe's import table changes, in memory (the
 * plugin's own WinHTTP and Winsock imports are its own); the file is never touched. */
int roam_win_online_init(void);                     /* roam_win.c */
static int iat_patch_of(uint8_t *base, const char *who){
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !readable((uint32_t)(uintptr_t)base + dir.VirtualAddress, dir.Size)){ roam_log("[lan] %s: its import table is not readable\n", who); return 0; }
    int want = 0, n = 0;
    for (const vs_import *v = vs_imports; v->dll; v++) want++;
    for (IMAGE_IMPORT_DESCRIPTOR *id = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); id->Name; id++){
        const char *dll = (const char *)(base + id->Name);
        int ours = 0;
        for (const vs_import *v = vs_imports; v->dll && !ours; v++) ours = !lstrcmpiA(v->dll, dll);
        if (!ours) continue;
        if (!id->OriginalFirstThunk){ roam_log("[lan] %s: no import names (a bound or packed exe?): not redirected\n", dll); continue; }
        IMAGE_THUNK_DATA32 *ot = (IMAGE_THUNK_DATA32 *)(base + id->OriginalFirstThunk), *ft = (IMAGE_THUNK_DATA32 *)(base + id->FirstThunk);
        for (; ot->u1.AddressOfData; ot++, ft++){
            const vs_import *hit = 0;
            for (const vs_import *v = vs_imports; v->dll && !hit; v++){
                if (lstrcmpiA(v->dll, dll)) continue;
                if (IMAGE_SNAP_BY_ORDINAL32(ot->u1.Ordinal)){ if (v->ord && (int)IMAGE_ORDINAL32(ot->u1.Ordinal) == v->ord) hit = v; }
                else if (!lstrcmpA((const char *)((IMAGE_IMPORT_BY_NAME *)(base + ot->u1.AddressOfData))->Name, v->name)) hit = v;
            }
            if (!hit){
                const char *nm = IMAGE_SNAP_BY_ORDINAL32(ot->u1.Ordinal) ? 0 : (const char *)((IMAGE_IMPORT_BY_NAME *)(base + ot->u1.AddressOfData))->Name;
                /* (the byte-order helpers need no network: server.dll's htons, htonl, ntohs, ntohl stay Windows') */
                if (nm && (!lstrcmpA(nm, "htons") || !lstrcmpA(nm, "htonl") || !lstrcmpA(nm, "ntohs") || !lstrcmpA(nm, "ntohl"))) continue;
                if (!nm && !lstrcmpiA(dll, "ws2_32.dll")){                     /* (#8 htonl, #9 htons, #14 ntohl, #15 ntohs) */
                    unsigned o = (unsigned)(ot->u1.Ordinal & 0xFFFFu);
                    if (o == 8 || o == 9 || o == 14 || o == 15) continue;
                }
                if (nm) roam_log("[lan]   %s: %s import %s not redirected (the plugin has no stand-in)\n", who, dll, nm);
                else roam_log("[lan]   %s: %s import #%u not redirected (the plugin has no stand-in)\n", who, dll, (unsigned)(ot->u1.Ordinal & 0xFFFFu));
                continue; }
            DWORD old;
            if (!VirtualProtect(&ft->u1.Function, 4, PAGE_READWRITE, &old)){ roam_log("[lan] VirtualProtect failed (%lu)\n", GetLastError()); return n; }
            ft->u1.Function = (DWORD)(uintptr_t)hit->fn;
            VirtualProtect(&ft->u1.Function, 4, old, &old);
            n++;
        }
    }
    roam_log("[lan] %s's network goes to the server's virtual LAN: %d of its imports redirected "
             "(the plugin has %d stand-ins)\n", who, n, want);
    return n;
}
static int iat_patch(void){ return iat_patch_of((uint8_t *)(uintptr_t)EXE_BASE, "SPEED2.EXE"); }

/* EA's LAN server (server.dll, the game's Host LAN Server and mprace.c's host): the game loads it with LoadLibraryA
 * when a LAN game is hosted (StartLANServer 0x5FA300) and frees it on leaving LAN (UnloadServer 0x5F08F0).  It has
 * Winsock imports of its own (by name), so the exe's LoadLibraryA import goes through the plugin: each time
 * server.dll loads, its import table is redirected to the virtual Winsock too (before the game calls into it), so the
 * host's LAN server listens on its virtual address, its LAN broadcast reaches the other players on the switch, and
 * they connect to it there.  Nothing else loaded is touched. */
typedef HMODULE (WINAPI *LoadLibraryA_t)(LPCSTR);
static LoadLibraryA_t g_real_loadlib;
static HMODULE g_srv_patched;
static int is_server_dll(const char *path){
    if (!path) return 0;
    const char *b = path;
    for (const char *q = path; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
    return !lstrcmpiA(b, "server.dll") || !lstrcmpiA(b, "server");
}
static void server_dll_patch(HMODULE h, const char *how){
    if (!h || h == g_srv_patched) return;
    if (iat_patch_of((uint8_t *)h, "server.dll") > 0){ g_srv_patched = h; roam_log("[lan] server.dll (%s at 0x%08x): the LAN server is on the virtual LAN\n", how, (uint32_t)(uintptr_t)h); }
}
static HMODULE WINAPI hk_LoadLibraryA(LPCSTR name){
    HMODULE h = g_real_loadlib(name);
    if (h && is_server_dll(name)) server_dll_patch(h, "loaded");
    return h;
}
/* StartLANServer's own load of server.dll: `push "server"; call [IAT LoadLibraryA]` at 0x5FA30C/0x5FA311 (the only
 * load of "server" in the exe).  Hooking the IAT slot alone was not enough: in the first live test (2026-11-06) the
 * slot had been re-hooked by the time the player hosted (another module, after the plugin's start: the ASI loader
 * hooks LoadLibraryA too), so server.dll loaded without the plugin seeing it and ran on the real network, and every
 * player's connect to the host's 10.77.x.x:9900 was refused.  So that call's operand points at a cell of the
 * plugin's own (the call reads it each time; nothing else writes it), which calls kernel32's LoadLibraryA itself;
 * the IAT hook stays for any other path. */
#define LANSRV_LOADCALL 0x005fa311u
#define IAT_LOADLIB     0x00783198u
static LoadLibraryA_t g_srv_loadcell = hk_LoadLibraryA;
static int srv_callsite_patch(void){
    uint8_t *p = (uint8_t *)(uintptr_t)LANSRV_LOADCALL;
    static const uint8_t want[6] = { 0xff, 0x15, 0x98, 0x31, 0x78, 0x00 };   /* call [0x783198] */
    for (int i = 0; i < 6; i++) if (p[i] != want[i]){
        roam_log("[lan] StartLANServer's load of server.dll (0x%08x) is not the call expected (another mod?): only the IAT hook\n", LANSRV_LOADCALL);
        return 0;
    }
    DWORD old;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(uint32_t *)(p + 2) = (uint32_t)(uintptr_t)&g_srv_loadcell;
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    roam_log("[lan] StartLANServer's load of server.dll (0x%08x) goes through the plugin\n", LANSRV_LOADCALL);
    return 1;
}
/* once a frame: server.dll loaded (the game's handle [0x89011C]) but not patched: say so once per load, and who has the
 * exe's LoadLibraryA slot now (for the log; the call site above should make this never happen) */
static void server_dll_watch(void){
    static HMODULE said;
    if (!g_real_loadlib) return;                  /* (online off: nothing hooked) */
    HMODULE h = *(HMODULE volatile *)(uintptr_t)0x0089011cu;
    if (!h || h == g_srv_patched || h == said) return;
    said = h;
    uint32_t f = *(uint32_t volatile *)(uintptr_t)IAT_LOADLIB;
    MEMORY_BASIC_INFORMATION mi; char who[MAX_PATH]; who[0] = 0;
    if (VirtualQuery((void *)(uintptr_t)f, &mi, sizeof mi) && mi.AllocationBase) GetModuleFileNameA((HMODULE)mi.AllocationBase, who, sizeof who);
    roam_log("[lan] server.dll (0x%08x) loaded without the plugin: it is on the REAL network (no one can join); the exe's LoadLibraryA slot is 0x%08x (%s), the plugin's 0x%08x\n",
             (uint32_t)(uintptr_t)h, f, who[0] ? who : "?", (uint32_t)(uintptr_t)hk_LoadLibraryA);
}
static void loadlib_hook(void){
    uint8_t *base = (uint8_t *)(uintptr_t)EXE_BASE;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (IMAGE_IMPORT_DESCRIPTOR *id = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); id->Name; id++){
        if (lstrcmpiA((const char *)(base + id->Name), "kernel32.dll") || !id->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA32 *ot = (IMAGE_THUNK_DATA32 *)(base + id->OriginalFirstThunk), *ft = (IMAGE_THUNK_DATA32 *)(base + id->FirstThunk);
        for (; ot->u1.AddressOfData; ot++, ft++){
            if (IMAGE_SNAP_BY_ORDINAL32(ot->u1.Ordinal)) continue;
            if (lstrcmpA((const char *)((IMAGE_IMPORT_BY_NAME *)(base + ot->u1.AddressOfData))->Name, "LoadLibraryA")) continue;
            DWORD old;
            if (!VirtualProtect(&ft->u1.Function, 4, PAGE_READWRITE, &old)) return;
            g_real_loadlib = (LoadLibraryA_t)(uintptr_t)ft->u1.Function;
            ft->u1.Function = (DWORD)(uintptr_t)hk_LoadLibraryA;
            VirtualProtect(&ft->u1.Function, 4, old, &old);
            roam_log("[lan] the exe's LoadLibraryA goes through the plugin: server.dll (the LAN server) will be on the virtual LAN too\n");
            server_dll_patch(GetModuleHandleA("server.dll"), "loaded already");
            srv_callsite_patch();
            return;
        }
    }
    roam_log("[lan] no LoadLibraryA import in the exe: a hosted LAN server would stay off the virtual LAN\n");
}
static void online_setup(void){
    if (!roam_win_online_init()) return;
    if (!vs_patch_lobby_host()) roam_log("[lan] the Online menu will not reach the server (see above); LAN play still goes to the virtual LAN\n");
    iat_patch();
    loadlib_hook();
}

/* ---- start-up ---------------------------------------------------------------------------------------- */
static void init_once(void){
    if (g_inited) return;
    g_inited = 1;
    char me[MAX_PATH];
    DWORD n = GetModuleFileNameA(g_self, me, sizeof me);
    if (!n || n >= sizeof me - 4) return;
    lstrcpyA(g_ini, me); lstrcpyA(g_logpath, me);
    int dot = -1; for (int i = 0; g_ini[i]; i++) if (g_ini[i] == '.') dot = i; else if (g_ini[i] == '\\' || g_ini[i] == '/') dot = -1;
    if (dot < 0){ dot = lstrlenA(g_ini); }
    lstrcpyA(g_ini + dot, ".ini"); lstrcpyA(g_logpath + dot, ".log");
    opts_load();
    if (opt_int("NFSU2_ROAM_LOG", 1)) log_open();
    {
        IMAGE_DOS_HEADER *d = (IMAGE_DOS_HEADER *)g_self; IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)g_self + d->e_lfanew);
        g_self_lo = (uint32_t)(uintptr_t)g_self; g_self_hi = g_self_lo + nt->OptionalHeader.SizeOfImage;
    }
    roam_log("[roam] NFSU2FreeRoam.asi %s (protocol %d) at 0x%08x; settings %s\n", ROAM_ASI_VERSION, RC_PROTO_VER, g_self_lo, g_ini);
    if (!opt_int("NFSU2_ROAM", 1)){ roam_log("[roam] Enable = 0 in the .ini: the plugin does nothing\n"); return; }
    if (!exe_check()){ roam_log("[roam] free-roam multiplayer stays OFF; the game runs as without the plugin\n"); return; }
    g_test_fault = opt_int("NFSU2_ROAM_TEST_FAULT", 0);
    g_test_fault_seh = opt_int("NFSU2_ROAM_TEST_FAULT_SEH", 0);
    if (!AddVectoredExceptionHandler(1, roam_veh)) roam_log("[roam] no fault guard (AddVectoredExceptionHandler failed)\n");
    online_setup();                                 /* before the game's first Winsock call (its LAN or Online menu) */
    if (!hook_install()) return;
    g_hud_on = hud_install();
    {
        const char *k = roam_opt("NFSU2_ROAM_SERVERKEY");
        int vk = roam_keys_vk(k);
        if (roam_opt("NFSU2_ROAM_SERVER") && roam_win_servers() > 0){
            if (vk) roam_keys_hotkey(vk, roam_win_server_next, "ServerKey: the next server");
            roam_log("[roam] server switch: %s goes to the next of the %d server(s) in Servers\n", vk ? k : "no key (ServerKey)", roam_win_servers());
        }
        const char *j = roam_opt("NFSU2_ROAM_JOINKEY");
        int jv = roam_keys_vk(j);
        if (jv && jv != vk) roam_keys_hotkey(jv, roam_game_join_key, "JoinKey: join (or leave) the race lobby offered from afar");
    }
    g_ready = 1;
    roam_log("[roam] ready: the network starts with the game's first frame\n");
}

__declspec(dllexport) void InitializeASI(void){ init_once(); }

volatile int roam_win_exiting;                   /* the process is ending (DLL_PROCESS_DETACH with reserved): roam_win.c */
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved){
    if (reason == DLL_PROCESS_ATTACH){
        g_self = (HMODULE)h;
        DisableThreadLibraryCalls(h);
        /* like ExtraOptions and Unlimiter: everything at load (only kernel32
         * calls: the .ini, the log, the exe check, one 5-byte patch); the
         * network starts later, at the first frame.  InitializeASI (Ultimate
         * ASI Loader calls it next) finds it done. */
        init_once();
    } else if (reason == DLL_PROCESS_DETACH){
        /* at process exit every other thread is gone already; if the network
         * thread died holding the session lock, skip the report (and the
         * BYE: friends drop this car after 2 s anyway) */
        /* (online: no WinHTTP call from here on: its threads are gone too, and webio.dll fail-fasts) */
        if (reserved) roam_win_exiting = 1;
        if (g_ready && !g_dead){ g_dead = 1; if (roam_plat_quiesced()) roam_game_report(); }
        vs_exit_report();                          /* the game's Winsock calls since the last report (no lock) */
        roam_keys_report();
        if (g_x87_popped) roam_log("[roam] %u float result(s) of game functions popped off the x87 stack\n", g_x87_popped);
        roam_log("[roam] unloaded%s\n", reserved ? " (game exit)" : "");
        if (g_log != INVALID_HANDLE_VALUE){ CloseHandle(g_log); g_log = INVALID_HANDLE_VALUE; }
    }
    return TRUE;
}
