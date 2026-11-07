/* mathtest.c -- the plugin's logf, expf, powf, floorf (win/roam_asi/mprace_win.c, for mprace.c's evening out of the
 * cars) against the C library's, over the ranges the evening out uses and beyond.  Exit 0 when every result is within
 * 2 ulps (floorf exact). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
float mp_logf_(float x); float mp_expf(float x); float mp_powf(float x, float y); float mp_floorf(float x);
/* what mprace_win.o needs besides them (not called here) */
void roam_log(const char *f, ...){ (void)f; }
int roam_win_committed(uint32_t a){ (void)a; return 0; }
uint32_t roam_win_call(uint32_t fn, uint32_t ecx, uint32_t edx, int n, const uint32_t *a){ (void)fn; (void)ecx; (void)edx; (void)n; (void)a; return 0; }
long roam_win_frame_no(void){ return 0; }
void roam_keys_tap(unsigned d, int h){ (void)d; (void)h; }
/* (mprace_win.c's quit notices: vsock.c and mprace.c are not linked here) */
int vs_session_active(void){ return 0; }
const char *roam_opt(const char *n){ (void)n; return 0; }
int roam_win_file_read(const char *e, char *b, int c){ (void)e; (void)b; (void)c; return -1; }
int roam_win_file_write(const char *e, const char *b, int n){ (void)e; (void)b; (void)n; return 0; }
uint64_t roam_win_filetime_now(void){ return 0; }
uint64_t roam_win_save_mtime(const char *n, char *w, int c){ (void)n; (void)w; (void)c; return 0; }
void roam_game_note(const char *t, int ms){ (void)t; (void)ms; }
int vs_rt_send(uint16_t p, uint32_t i, const void *m, int n){ (void)p; (void)i; (void)m; (void)n; return -1; }
int vs_rt_recv(uint16_t p, uint32_t *i, void *m, int c){ (void)p; (void)i; (void)m; (void)c; return -1; }
int vs_race_peers(uint32_t *i, int m, uint32_t w){ (void)i; (void)m; (void)w; return 0; }
void mprace_quit_heard(uint8_t *m, int n, uint32_t f){ (void)m; (void)n; (void)f; }
static int32_t ulps(float a, float b){
    if (a == b) return 0;
    if (a != a || b != b) return (a != a && b != b) ? 0 : 1 << 30;
    int32_t x, y; memcpy(&x, &a, 4); memcpy(&y, &b, 4);
    if (x < 0) x = (int32_t)0x80000000 - x;
    if (y < 0) y = (int32_t)0x80000000 - y;
    int32_t d = x - y; return d < 0 ? -d : d;
}
int main(void){
    int bad = 0, n = 0; int32_t wl = 0, we = 0, wp = 0;
    for (float x = 1e-6f; x < 1e7f; x *= 1.0137f){ int32_t u = ulps(mp_logf_(x), logf(x)); if (u > wl) wl = u; if (u > 2){ if (bad++ < 5) printf("logf(%g): %.9g vs %.9g\n", x, mp_logf_(x), logf(x)); } n++; }
    for (float x = -80.f; x < 80.f; x += 0.0173f){ int32_t u = ulps(mp_expf(x), expf(x)); if (u > we) we = u; if (u > 2){ if (bad++ < 10) printf("expf(%g): %.9g vs %.9g\n", x, mp_expf(x), expf(x)); } n++; }
    for (float x = 0.01f; x < 1000.f; x *= 1.31f) for (float y = -3.f; y < 3.f; y += 0.0371f){
        int32_t u = ulps(mp_powf(x, y), powf(x, y)); if (u > wp) wp = u; if (u > 2){ if (bad++ < 15) printf("powf(%g, %g): %.9g vs %.9g\n", x, y, mp_powf(x, y), powf(x, y)); } n++; }
    /* the evening out's own: (target / r)^close, and its 1.16 */
    for (float r = 100.f; r < 2000.f; r += 7.3f){ int32_t u = ulps(mp_powf(1300.f / r, 0.9f), powf(1300.f / r, 0.9f)); if (u > wp) wp = u; if (u > 2) bad++; u = ulps(mp_powf(r / 900.f, 1.f / 1.16f), powf(r / 900.f, 1.f / 1.16f)); if (u > wp) wp = u; if (u > 2) bad++; n += 2; }
    for (float x = -1000.f; x < 1000.f; x += 0.37f){ if (mp_floorf(x) != floorf(x)){ if (bad++ < 20) printf("floorf(%g): %g vs %g\n", x, mp_floorf(x), floorf(x)); } n++; }
    printf("%d values: logf within %d ulps, expf %d, powf %d; %d off by more than 2\n", n, (int)wl, (int)we, (int)wp, bad);
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
