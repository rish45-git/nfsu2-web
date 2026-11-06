/* mprace_host_win.h -- what runtime/mprace.c (a multiplayer race from free roam: the trip to the game's LAN race and
 * back) needs from its host, for the Windows plugin NFSU2FreeRoam.asi: the names the port's cpu.h, stdio, libm and
 * its other files give it there (mprace.c includes this instead of them with -DROAM_HOST_WIN).
 *
 * Guest memory is the process's own (roam_host_win.h), guest calls are roam_win_call, the log is the plugin's
 * (fprintf to stderr is collected into lines), getenv is the plugin's options (environment, then the .ini), the clock
 * QueryPerformanceCounter's.  The port's other files mprace.c calls are win/roam_asi/mprace_win.c: the front end's
 * screens, the key taps (roam_keys.c), the career car's lookup, the career credit (a native mpcredit), and stand-ins
 * for what only the browser has (its race statistics, its pause menu's Save, its quit datagrams). */
#ifndef MPRACE_HOST_WIN_H
#define MPRACE_HOST_WIN_H
#include "roam_host_win.h"
#include <stdint.h>

#define mem_is_committed(a)  roam_win_committed(a)
/* guest address a as a host pointer: g_mem + a */
#define g_mem                ((uint8_t *)0)

/* ---- the log: fprintf(stderr, ...) pieces, one line each '\n' ---- */
#define stderr               ((void *)0)
void mp_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void mp_logc(int ch);
#define fprintf(f, ...)      ((void)(f), mp_logf(__VA_ARGS__))
#define fputc(ch, f)         ((void)(f), mp_logc(ch))

/* ---- options and test aids ---- */
#define getenv(name)         roam_opt(name)
#define sscanf(...)          0              /* (only a test aid's "UP,DOWN,..." uses it: off here) */

/* ---- the clock ---- */
struct mp_timespec { long long tv_sec; long tv_nsec; };
#define timespec             mp_timespec
#define CLOCK_MONOTONIC      1
static inline int mp_clock_gettime(int id, struct mp_timespec *ts){
    (void)id; uint64_t ns = roam_win_ns();
    ts->tv_sec = (long long)(ns / 1000000000ull); ts->tv_nsec = (long)(ns % 1000000000ull); return 0;
}
#define clock_gettime(id, ts) mp_clock_gettime((id), (ts))

/* ---- libm: the few functions the cars' evening out takes (mprace_win.c: plain single-precision code, to within an
 * ulp or two of a C library's; each game evens out its own car, so a last bit apart between two games moves no car of
 * the other's) ---- */
float mp_logf_(float x);
float mp_expf(float x);
float mp_powf(float x, float y);
float mp_floorf(float x);
#define logf(x)              mp_logf_(x)
#define expf(x)              mp_expf(x)
#define powf(x, y)           mp_powf((x), (y))
#define floorf(x)            mp_floorf(x)

/* ---- the C library's string functions roam_host_win.h does not declare ---- */
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
#endif
