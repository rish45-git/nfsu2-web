/* roam_keys.c -- NFSU2FreeRoam.asi: the plugin's keys.
 *
 * 1. Its hotkeys (the server switch's ServerKey, the lobby's join key): GetAsyncKeyState, only while one of the game's
 *    windows is in front, read once a frame from the frame hook.  user32 is in every game process already; its
 *    functions are looked up at run time (GetProcAddress), so the plugin imports nothing more.
 * 2. Keys pressed for the game (mprace.c's taps: Enter, Escape, Left in the LAN screens) and the player's keys kept
 *    from it while a race trip drives (mprace_owns_input): through the game's own DirectInput keyboard.
 *
 * How SPEED2.EXE (v1.2 NA) reads its keyboard (runtime/dinput_kbd.c has it in full): one device, [0x86E5C0]; every
 * frame 0x5C1840 calls its GetDeviceData and acts on each event's uAppData (the action) and dwData (0x80 down); the
 * device has an action map (one of four DIACTIONFORMATs at 0x86F348 + 0x148 n: race, front end, ...; SetActionMap
 * switches them), and with one an event's dwOfs is 4 times the action's index in the map.  So the plugin patches the
 * device's GetDeviceData (vtable slot 10) and SetActionMap (slot 30) -- the class's vtable is system dinput8's, shared
 * by every device, so the hook checks the device -- and adds its own events after the device's: the action whose key
 * (dwObjID, DIDFT_GETINSTANCE = the DIK code, on the system keyboard) is the tapped one in the map set now.  While the
 * device is lost (the game in the background) the hook still hands out the plugin's events, so a trip goes on.
 * Nothing of the game's code is changed; the vtable entries go back at unload. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

void roam_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *roam_opt(const char *name);

#define G_KBD           0x0086e5c0u   /* the game's keyboard device (IDirectInputDevice8A *) */
#define G_ACTFMT        0x0086f348u   /* its four DIACTIONFORMATs, 0x148 bytes each */
#define ACTFMT_SIZE     0x148u
#define VT_GETDATA      10
#define VT_SETMAP       30
#define DIDFT_BUTTON    0x0000000Cu
#define DI_OK           0
#define DIGDD_PEEK      1u

typedef long (__stdcall *GetDeviceData_t)(void *self, DWORD cb, uint8_t *rg, DWORD *inout, DWORD flags);
typedef long (__stdcall *SetActionMap_t)(void *self, void *fmt, const char *user, DWORD flags);

static void           **g_vt;                 /* the device class's vtable, once hooked */
static GetDeviceData_t  g_gdd;
static SetActionMap_t   g_sam;
static void            *g_dev;                /* the game's keyboard */
static void            *g_map;                /* its action map, as last set (0: not seen) */
static volatile LONG    g_mute;
static DWORD            g_seq = 0x40000000u;
static CRITICAL_SECTION g_mu;
static int              g_mu_ok;
/* the plugin's key events, in order: a press, then its release after `hold` of the game's reads */
#define TAPS 16
static struct { uint8_t dik, down; int wait; } g_q[TAPS];
static int g_qn;
static uint32_t g_taps, g_unmapped, g_muted_events;

/* the system keyboard: GUID_SysKeyboard {6F1D2B61-D5A0-11CF-BFC7-444553540000} */
static const uint8_t SYSKBD[16] = { 0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static int mem_ok(const void *p, uint32_t n){
    MEMORY_BASIC_INFORMATION mi;
    for (uintptr_t a = (uintptr_t)p & ~(uintptr_t)0xFFF; a < (uintptr_t)p + n; a += 0x1000){
        if (!VirtualQuery((LPCVOID)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
    }
    return 1;
}
/* the action of key dik in action map fmt (a DIACTIONFORMATA: +0xC actions, +0x10 the DIACTIONA array, 40 bytes each:
 * +0 uAppData, +0x10 guidInstance, +0x20 dwObjID, +0x24 dwHow): its index, -1 none */
static int action_of(const uint8_t *fmt, unsigned dik, uint32_t *app){
    if (!fmt || !mem_ok(fmt, 0x14)) return -1;
    uint32_t n = *(const uint32_t *)(fmt + 0xC); const uint8_t *a = *(const uint8_t *const *)(fmt + 0x10);
    if (!a || n > 512 || !mem_ok(a, n * 40u)) return -1;
    for (uint32_t i = 0; i < n; i++){
        const uint8_t *x = a + 40u * i;
        uint32_t obj = *(const uint32_t *)(x + 0x20), how = *(const uint32_t *)(x + 0x24);
        if (!how || !(obj & DIDFT_BUTTON) || ((obj >> 8) & 0xFFFFu) != dik) continue;
        int kb = 1; for (int k = 0; k < 16; k++) if (x[0x10 + k] != SYSKBD[k]) kb = 0;
        if (!kb) continue;
        *app = *(const uint32_t *)x;
        return (int)i;
    }
    return -1;
}
/* the map to look a key up in: the one set last, else the front end's, then the others */
static int lookup(unsigned dik, uint32_t *ofs, uint32_t *app){
    int i = action_of((const uint8_t *)g_map, dik, app);
    if (i >= 0){ *ofs = 4u * (uint32_t)i; return 1; }
    static const int order[4] = { 1, 0, 2, 3 };
    for (int k = 0; k < 4; k++){
        i = action_of((const uint8_t *)(uintptr_t)(G_ACTFMT + ACTFMT_SIZE * (uint32_t)order[k]), dik, app);
        if (i >= 0){ *ofs = 4u * (uint32_t)i; return 1; }
    }
    return 0;
}
static long __stdcall hk_setmap(void *self, void *fmt, const char *user, DWORD flags){
    long r = g_sam(self, fmt, user, flags);
    if (self == g_dev && r >= 0) g_map = fmt;
    return r;
}
static long __stdcall hk_getdata(void *self, DWORD cb, uint8_t *rg, DWORD *inout, DWORD flags){
    if (self != g_dev || !inout) return g_gdd(self, cb, rg, inout, flags);
    DWORD want = *inout;
    long r = g_gdd(self, cb, rg, inout, flags);
    if (!g_mu_ok) return r;
    EnterCriticalSection(&g_mu);
    if (r >= 0 && g_mute){                                          /* the trip drives: the player's keys go nowhere */
        if (!(flags & DIGDD_PEEK)) g_muted_events += *inout;
        *inout = 0;
    }
    if (r < 0 && g_qn){ *inout = 0; r = DI_OK; }                    /* lost (in the background): ours still go */
    if (r >= 0 && rg && cb >= 16u && g_qn){
        DWORD n = *inout;
        int k = 0;
        while (k < g_qn && n < want){
            if (g_q[k].wait > 0){ if (!(flags & DIGDD_PEEK)) g_q[k].wait--; break; }   /* (a release waits its turn) */
            uint32_t ofs, app;
            if (!lookup(g_q[k].dik, &ofs, &app)){ g_unmapped++; k++; continue; }
            uint8_t *e = rg + n * cb;
            for (DWORD b = 0; b < cb; b++) e[b] = 0;
            ((uint32_t *)e)[0] = ofs; ((uint32_t *)e)[1] = g_q[k].down ? 0x80u : 0u;
            ((uint32_t *)e)[2] = GetTickCount(); ((uint32_t *)e)[3] = g_seq++;
            if (cb >= 20u) ((uint32_t *)e)[4] = app;
            n++; k++;
        }
        if (!(flags & DIGDD_PEEK) && k){ for (int j = k; j < g_qn; j++) g_q[j - k] = g_q[j]; g_qn -= k; }
        *inout = n;
    }
    LeaveCriticalSection(&g_mu);
    return r;
}
static int vt_write(void **slot, void *fn){
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *slot = fn;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return 1;
}
/* the game's keyboard exists (it makes it at start, again after a lost device): the hooks on its class */
static void hook_frame(void){
    void *dev = *(void *volatile *)(uintptr_t)G_KBD;
    if (!dev || !mem_ok(dev, 4)) return;
    void **vt = *(void ***)dev;
    if (!vt || !mem_ok(vt, 4 * (VT_SETMAP + 1))) return;
    if (g_vt == vt){ g_dev = dev; return; }
    if (g_vt) return;                                               /* another class: keep the first (logged once) */
    g_gdd = (GetDeviceData_t)vt[VT_GETDATA]; g_sam = (SetActionMap_t)vt[VT_SETMAP];
    if (!vt_write(&vt[VT_GETDATA], (void *)hk_getdata) || !vt_write(&vt[VT_SETMAP], (void *)hk_setmap)){
        roam_log("[keys] the game's keyboard could not be hooked (VirtualProtect %lu): no key presses for race trips\n", GetLastError());
        g_vt = (void **)1; return;
    }
    g_vt = vt; g_dev = dev;
    roam_log("[keys] the game's keyboard (%p) hooked: GetDeviceData %p, SetActionMap %p\n", dev, (void *)g_gdd, (void *)g_sam);
}
int  roam_keys_ready(void){ return g_vt && g_vt != (void **)1 && g_dev; }
/* a key press for the game (a DIK code), held for `hold` of its reads (frames) */
void roam_keys_tap(unsigned dik, int hold){
    if (!g_mu_ok) return;
    EnterCriticalSection(&g_mu);
    if (g_qn + 2 <= TAPS){
        g_q[g_qn].dik = (uint8_t)dik; g_q[g_qn].down = 1; g_q[g_qn].wait = 0; g_qn++;
        g_q[g_qn].dik = (uint8_t)dik; g_q[g_qn].down = 0; g_q[g_qn].wait = hold > 0 ? hold : 1; g_qn++;
        g_taps++;
    }
    LeaveCriticalSection(&g_mu);
}
void roam_keys_mute(int on){
    if (InterlockedExchange(&g_mute, on ? 1 : 0) != (on ? 1 : 0))
        roam_log("[keys] the player's keys %s\n", on ? "go to the race trip now (it drives the game's menus)" : "are the player's again");
}

/* ---- hotkeys ---------------------------------------------------------------------------------------- */
typedef short (__stdcall *GetAsyncKeyState_t)(int vk);
typedef HWND  (__stdcall *GetForegroundWindow_t)(void);
typedef DWORD (__stdcall *GetWindowThreadProcessId_t)(HWND, DWORD *);
static GetAsyncKeyState_t         p_gaks;
static GetForegroundWindow_t      p_gfw;
static GetWindowThreadProcessId_t p_gwtp;
static int g_user_ok;
#define HOTKEYS 4
static struct { int vk; int held; void (*fn)(void); const char *what; } g_hk[HOTKEYS];
static int g_nhk;
/* "F1".."F24", a letter or a digit; 0 none */
int roam_keys_vk(const char *s){
    if (!s || !*s) return 0;
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9'){
        int n = s[1] - '0';
        if (s[2] >= '0' && s[2] <= '9' && !s[3]) n = n * 10 + (s[2] - '0'); else if (s[2]) return 0;
        return n >= 1 && n <= 24 ? 0x70 + n - 1 : 0;
    }
    if (!s[1] && s[0] >= 'a' && s[0] <= 'z') return s[0] - 'a' + 'A';
    if (!s[1] && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) return s[0];
    return 0;
}
void roam_keys_hotkey(int vk, void (*fn)(void), const char *what){
    if (!vk || g_nhk >= HOTKEYS) return;
    g_hk[g_nhk].vk = vk; g_hk[g_nhk].fn = fn; g_hk[g_nhk].what = what; g_hk[g_nhk].held = 1;   /* (held at start: no press yet) */
    g_nhk++;
}
static int game_in_front(void){
    if (!p_gfw || !p_gwtp) return 0;
    HWND w = p_gfw(); DWORD pid = 0;
    if (!w) return 0;
    p_gwtp(w, &pid);
    return pid == GetCurrentProcessId();
}
static void hotkeys_frame(void){
    if (!g_user_ok){
        g_user_ok = -1;
        HMODULE u = GetModuleHandleA("user32.dll");
        if (u){
            p_gaks = (GetAsyncKeyState_t)(void *)GetProcAddress(u, "GetAsyncKeyState");
            p_gfw = (GetForegroundWindow_t)(void *)GetProcAddress(u, "GetForegroundWindow");
            p_gwtp = (GetWindowThreadProcessId_t)(void *)GetProcAddress(u, "GetWindowThreadProcessId");
        }
        if (p_gaks && p_gfw && p_gwtp) g_user_ok = 1;
        else roam_log("[keys] no user32 key functions: the plugin's hotkeys are off\n");
    }
    if (g_user_ok != 1 || !g_nhk) return;
    int front = game_in_front();
    for (int i = 0; i < g_nhk; i++){
        int down = front && (p_gaks(g_hk[i].vk) & 0x8000);
        if (down && !g_hk[i].held){ roam_log("[keys] %s\n", g_hk[i].what); g_hk[i].fn(); }
        g_hk[i].held = down;
    }
}

/* every frame, from the frame hook */
void roam_keys_frame(void){
    if (!g_mu_ok){ InitializeCriticalSection(&g_mu); g_mu_ok = 1; }
    hook_frame();
    hotkeys_frame();
}
void roam_keys_report(void){
    if (g_taps || g_unmapped || g_muted_events)
        roam_log("[keys] %u key presses for the game (%u with no action in the game's map), %u of the player's events held back\n",
                 g_taps, g_unmapped, g_muted_events);
}
/* at unload (not at process exit: dinput8 may be gone then) */
void roam_keys_unhook(void){
    if (g_vt && g_vt != (void **)1){ vt_write(&g_vt[VT_GETDATA], (void *)g_gdd); vt_write(&g_vt[VT_SETMAP], (void *)g_sam); g_vt = 0; }
}
