/* switch.c -- see switch.h.  KERNEL32 and WINHTTP only; no C runtime (memcpy/memset/strlen come from the compiler's
 * builtins or the plugin's roam_libc.c). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include "switch.h"

#ifndef WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET
#define WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET 114
#endif

#define CLOSE_FULL    1013
#define CLOSE_KICKED  4001
#define CLOSE_BANNED  4003
#define CLOSE_RENAME  4004
#define RX_MAX        (2 * 1024 * 1024)     /* the whole peer list of a full switch is about 490 KB */

struct sw {
    sw_options o;
    sw_callbacks cb;
    char url[256], name[96], claim[96];
    char host[128], path[128];
    int port, secure;

    volatile LONG stop;
    volatile LONG state;                    /* sw_state */
    volatile LONG vip;
    CRITICAL_SECTION lock;                  /* the socket handle and its sends */
    HINTERNET hs, hc, ws;
    HANDLE thread, pinger;

    char token[64];                         /* from the welcome: resume with it after a lost link */
    uint32_t last_vip;
    char ticket[96];                        /* the place in line, kept for a reconnect */
    volatile LONG lan_on, lan_udp;          /* LAN play open, its UDP ports (sw_set_lan): said in every hello */

    LONG ping_id;
    struct { uint32_t id; LONGLONG t; } pings[8];
    LONGLONG qpf;
};

/* ---- little helpers ---- */
static int slen(const char *s){ int n = 0; while (s[n]) n++; return n; }
static void scopy(char *d, int cap, const char *s){ int i = 0; if (s) for (; s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0; }
static int sappend(char *d, int cap, int at, const char *s){ while (*s && at < cap - 1) d[at++] = *s++; d[at] = 0; return at; }
static int uappend(char *d, int cap, int at, uint32_t v){
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && at < cap - 1) d[at++] = t[--n];
    d[at] = 0; return at;
}
static void log_(sw *s, const char *line){ if (s->cb.on_log) s->cb.on_log(s->cb.user, line); }
static void logn(sw *s, const char *a, uint32_t n, const char *b){
    char l[200]; int k = sappend(l, sizeof l, 0, a); k = uappend(l, sizeof l, k, n); sappend(l, sizeof l, k, b); log_(s, l);
}
static void ipstr(char *d, uint32_t ip){
    int k = 0; for (int i = 3; i >= 0; i--){ k = uappend(d, 20, k, (ip >> (8 * i)) & 255); if (i) d[k++] = '.'; } d[k] = 0;
}
static uint32_t ipnum(const char *s){
    uint32_t r = 0; int part = 0, any = 0;
    for (;; s++){
        if (*s >= '0' && *s <= '9'){ part = part * 10 + (*s - '0'); any = 1; if (part > 255) return 0; }
        else if (*s == '.' || !*s){ if (!any) return 0; r = (r << 8) | (uint32_t)part; part = 0; any = 0; if (!*s) break; }
        else return 0;
    }
    return r;
}

/* the value after "key": in a flat JSON object (the server's control messages: no nesting where it matters) */
static const char *jfind(const char *j, int n, const char *key){
    int kl = slen(key);
    for (int i = 0; i + kl + 3 < n; i++){
        if (j[i] != '"') continue;
        int k = 0; while (k < kl && j[i + 1 + k] == key[k]) k++;
        if (k != kl || j[i + 1 + kl] != '"') continue;
        int p = i + 2 + kl; while (p < n && (j[p] == ' ' || j[p] == ':')) p++;
        return j + p;
    }
    return 0;
}
static int jnum(const char *j, int n, const char *key, double *out){
    const char *p = jfind(j, n, key); if (!p || *p == '"') return 0;
    int neg = 0; double v = 0, f = 0.1; if (*p == '-'){ neg = 1; p++; }
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if (*p == '.'){ p++; while (*p >= '0' && *p <= '9'){ v += (*p++ - '0') * f; f /= 10; } }
    *out = neg ? -v : v; return 1;
}
static int jstr(const char *j, int n, const char *key, char *out, int cap){
    const char *p = jfind(j, n, key); if (!p || *p != '"') return 0;
    int k = 0; p++;
    while (p < j + n && *p != '"' && k < cap - 1){
        if (*p == '\\' && p + 1 < j + n){ p++; out[k++] = *p == 'n' ? ' ' : *p; p++; }
        else out[k++] = *p++;
    }
    out[k] = 0; return 1;
}
/* a string for a JSON value: quotes, backslashes and control characters out, non-ASCII bytes as '?' */
static int jesc(char *d, int cap, int at, const char *s){
    for (; s && *s && at < cap - 2; s++){
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\'){ d[at++] = '\\'; d[at++] = (char)c; }
        else if (c < 32) d[at++] = ' ';
        else if (c > 126) d[at++] = '?';
        else d[at++] = (char)c;
    }
    d[at] = 0; return at;
}
static void wide(wchar_t *d, int cap, const char *s){ int i = 0; for (; s[i] && i < cap - 1; i++) d[i] = (unsigned char)s[i]; d[i] = 0; }

static int parse_url(sw *s){
    const char *u = s->o.url;
    if (u[0] == 'w' && u[1] == 's' && u[2] == 's' && u[3] == ':' && u[4] == '/' && u[5] == '/'){ s->secure = 1; u += 6; }
    else if (u[0] == 'w' && u[1] == 's' && u[2] == ':' && u[3] == '/' && u[4] == '/'){ s->secure = 0; u += 5; }
    else return 0;
    int k = 0;
    while (*u && *u != ':' && *u != '/' && k < (int)sizeof s->host - 1) s->host[k++] = *u++;
    s->host[k] = 0;
    s->port = s->secure ? 443 : 80;
    if (*u == ':'){ u++; s->port = 0; while (*u >= '0' && *u <= '9') s->port = s->port * 10 + (*u++ - '0'); }
    scopy(s->path, sizeof s->path, *u ? u : "/lan");
    return k > 0 && s->port > 0 && s->port < 65536;
}

static void close_ws(sw *s){
    EnterCriticalSection(&s->lock);
    HINTERNET h = s->ws; s->ws = 0;
    LeaveCriticalSection(&s->lock);
    if (h) WinHttpCloseHandle(h);
}
static void set_state(sw *s, sw_state st){ InterlockedExchange(&s->state, (LONG)st); }

static LONGLONG now_ticks(void){ LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

/* ---- sending ---- */
static int send_ws(sw *s, const void *buf, DWORD len, WINHTTP_WEB_SOCKET_BUFFER_TYPE type){
    int ok = 0;
    EnterCriticalSection(&s->lock);
    if (s->ws && WinHttpWebSocketSend(s->ws, type, (PVOID)buf, len) == ERROR_SUCCESS) ok = 1;
    LeaveCriticalSection(&s->lock);
    return ok;
}
int sw_send_text(sw *s, const char *json){
    return send_ws(s, json, (DWORD)slen(json), WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE);
}
int sw_send_frame(sw *s, const uint8_t *f, int len){
    if (s->state != SW_ONLINE || len < 12) return 0;
    return send_ws(s, f, (DWORD)len, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE);
}
int sw_send_udp(sw *s, uint8_t kind, uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t *data, int len){
    uint8_t f[1536];
    if (len < 0 || len > (int)sizeof f - 16) return 0;
    for (int i = 0; i < 16; i++) f[i] = 0;
    f[0] = kind;
    f[8] = (uint8_t)(dst >> 24); f[9] = (uint8_t)(dst >> 16); f[10] = (uint8_t)(dst >> 8); f[11] = (uint8_t)dst;
    f[12] = (uint8_t)(sport >> 8); f[13] = (uint8_t)sport; f[14] = (uint8_t)(dport >> 8); f[15] = (uint8_t)dport;
    for (int i = 0; i < len; i++) f[16 + i] = data[i];
    return sw_send_frame(s, f, 16 + len);
}
int sw_set_lan(sw *s, int on, uint32_t udp){
    on = on != 0; if (!on) udp = 0;
    if (s->lan_on == on && (uint32_t)s->lan_udp == udp) return 1;
    InterlockedExchange(&s->lan_on, on); InterlockedExchange(&s->lan_udp, (LONG)udp);
    if (s->state != SW_ONLINE) return 0;                  /* the next hello says it */
    char m[64]; int k = sappend(m, sizeof m, 0, "{\"t\":\"lan\",\"on\":"); k = sappend(m, sizeof m, k, on ? "1" : "0");
    if (on && (udp & 0x80000000u)){ k = sappend(m, sizeof m, k, ",\"udp\":"); k = uappend(m, sizeof m, k, udp); }
    sappend(m, sizeof m, k, "}");
    return sw_send_text(s, m);
}
int sw_ping(sw *s){
    if (s->state != SW_ONLINE) return 0;
    LONG id = InterlockedIncrement(&s->ping_id);
    s->pings[id & 7].id = (uint32_t)id; s->pings[id & 7].t = now_ticks();
    char m[48]; int k = sappend(m, sizeof m, 0, "{\"t\":\"ping\",\"id\":");
    k = uappend(m, sizeof m, k, (uint32_t)id); sappend(m, sizeof m, k, "}");
    return sw_send_text(s, m);
}

/* ---- control messages ---- */
static void control(sw *s, const char *j, int n){
    char t[24];
    if (s->cb.on_text) s->cb.on_text(s->cb.user, j, n);
    if (!jstr(j, n, "t", t, sizeof t)) return;
    if (!lstrcmpA(t, "welcome")){
        double v = 0, m = 0; char ms[24], tok[64];
        if (!jnum(j, n, "vip", &v)) return;
        uint32_t mask = 0xffffc000u;
        if (jnum(j, n, "mask", &m)) mask = (uint32_t)m; else if (jstr(j, n, "mask", ms, sizeof ms) && ipnum(ms)) mask = ipnum(ms);
        tok[0] = 0; jstr(j, n, "token", tok, sizeof tok);
        s->last_vip = (uint32_t)v; scopy(s->token, sizeof s->token, tok);
        s->ticket[0] = 0;
        InterlockedExchange(&s->vip, (LONG)(uint32_t)v);
        set_state(s, SW_ONLINE);
        if (s->cb.on_welcome) s->cb.on_welcome(s->cb.user, (uint32_t)v, mask, tok);
    } else if (!lstrcmpA(t, "queue")){
        double pos = 0, of = 0; jnum(j, n, "pos", &pos); jnum(j, n, "of", &of);
        jstr(j, n, "ticket", s->ticket, sizeof s->ticket);
        set_state(s, SW_QUEUED);
        if (s->cb.on_queue) s->cb.on_queue(s->cb.user, (int)pos, (int)of);
    } else if (!lstrcmpA(t, "kick") || !lstrcmpA(t, "ban") || !lstrcmpA(t, "rename")){
        char msg[200]; msg[0] = 0; jstr(j, n, "msg", msg, sizeof msg);
        set_state(s, SW_REMOVED);                         /* its close follows; no reconnecting */
        if (s->cb.on_removed) s->cb.on_removed(s->cb.user, t[0] == 'b', msg);
    } else if (!lstrcmpA(t, "peers") || !lstrcmpA(t, "delta")){
        if (s->cb.on_peers) s->cb.on_peers(s->cb.user, j, n);
    } else if (!lstrcmpA(t, "pong")){
        double id = 0; if (!jnum(j, n, "id", &id)) return;
        uint32_t i = (uint32_t)id;
        if (s->pings[i & 7].id == i){
            double rtt = (double)(now_ticks() - s->pings[i & 7].t) * 1000.0 / (double)s->qpf;
            s->pings[i & 7].id = 0;
            if (s->cb.on_pong) s->cb.on_pong(s->cb.user, i, rtt);
        }
    }
}

/* ---- one connection: connect, hello, read until it ends.  Returns the close code (0: never connected) ---- */
static int run_once(sw *s, int resume){
    wchar_t whost[128], wpath[192];
    char path[192]; int k = sappend(path, sizeof path, 0, s->path);
    k = sappend(path, sizeof path, k, "?queue");          /* wait in line when the server is full */
    if (resume) sappend(path, sizeof path, k, "&resume");
    wide(whost, 128, s->host); wide(wpath, 192, path);
    int code = 0;
    HINTERNET hr = 0;
    if (!s->hs) s->hs = WinHttpOpen(L"NFSU2Native/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, 0, 0, 0);
    if (!s->hs){ log_(s, "WinHttpOpen failed"); return 0; }
    s->hc = WinHttpConnect(s->hs, whost, (INTERNET_PORT)s->port, 0);
    if (!s->hc){ log_(s, "WinHttpConnect failed"); return 0; }
    hr = WinHttpOpenRequest(s->hc, L"GET", wpath, 0, 0, 0, s->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hr) goto out;
    if (!WinHttpSetOption(hr, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, 0, 0)) goto out;
    if (!WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0, 0, 0, 0, 0) || !WinHttpReceiveResponse(hr, 0)){
        logn(s, "cannot reach the server (error ", GetLastError(), ")"); goto out;
    }
    HINTERNET w = WinHttpWebSocketCompleteUpgrade(hr, 0);
    if (!w){
        DWORD st = 0, sz = sizeof st;
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &st, &sz, 0);
        logn(s, "the server did not upgrade to a WebSocket (HTTP ", st, ")");
        code = (int)st; if (code < 100) code = 0; goto out;
    }
    WinHttpCloseHandle(hr); hr = 0;
    EnterCriticalSection(&s->lock); s->ws = w; LeaveCriticalSection(&s->lock);

    /* the hello (switch.h: no bundle/compact/delta) */
    {
        char h[600]; int n = sappend(h, sizeof h, 0, "{\"t\":\"hello\",\"name\":\"");
        n = jesc(h, sizeof h, n, s->name); n = sappend(h, sizeof h, n, "\",\"lan\":"); n = sappend(h, sizeof h, n, s->lan_on ? "1" : "0");
        if (s->lan_on && ((uint32_t)s->lan_udp & 0x80000000u)){ n = sappend(h, sizeof h, n, ",\"udp\":"); n = uappend(h, sizeof h, n, (uint32_t)s->lan_udp); }
        if (s->claim[0]){ n = sappend(h, sizeof h, n, ",\"claim\":\""); n = jesc(h, sizeof h, n, s->claim); n = sappend(h, sizeof h, n, "\""); }
        if (resume && s->last_vip && s->token[0]){
            char ip[20]; ipstr(ip, s->last_vip);
            n = sappend(h, sizeof h, n, ",\"vip\":\""); n = sappend(h, sizeof h, n, ip);
            n = sappend(h, sizeof h, n, "\",\"token\":\""); n = jesc(h, sizeof h, n, s->token); n = sappend(h, sizeof h, n, "\"");
        }
        if (s->ticket[0]){ n = sappend(h, sizeof h, n, ",\"ticket\":\""); n = jesc(h, sizeof h, n, s->ticket); n = sappend(h, sizeof h, n, "\""); }
        sappend(h, sizeof h, n, "}");
        if (!sw_send_text(s, h)){ log_(s, "cannot send the hello"); goto out; }
    }
    code = -1;                                            /* connected; the close code replaces it */

    {
        HANDLE heap = GetProcessHeap();
        DWORD cap = 64 * 1024, len = 0;
        uint8_t *buf = HeapAlloc(heap, 0, cap);
        while (buf && !s->stop){
            DWORD got = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE ty;
            DWORD r = WinHttpWebSocketReceive(w, buf + len, cap - len, &got, &ty);
            if (r != ERROR_SUCCESS) break;
            len += got;
            if (ty == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE){
                USHORT cs = 0; DWORD rl = 0; char reason[16];
                if (WinHttpWebSocketQueryCloseStatus(w, &cs, reason, sizeof reason, &rl) == ERROR_SUCCESS) code = cs;
                break;
            }
            if (ty == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || ty == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE){
                if (len == cap){
                    if (cap >= RX_MAX) break;
                    cap *= 2; uint8_t *nb = HeapReAlloc(heap, 0, buf, cap); if (!nb) break; buf = nb;
                }
                continue;
            }
            if (ty == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE){
                if (len < cap) buf[len] = 0;
                control(s, (const char *)buf, (int)len);
            } else if (s->cb.on_frame && s->state == SW_ONLINE){
                s->cb.on_frame(s->cb.user, buf, (int)len);
            }
            len = 0;
        }
        if (buf) HeapFree(heap, 0, buf);
    }
out:
    if (hr) WinHttpCloseHandle(hr);
    close_ws(s);
    if (s->hc){ WinHttpCloseHandle(s->hc); s->hc = 0; }
    return code;
}

static DWORD WINAPI reader_main(LPVOID p){
    sw *s = p; DWORD backoff = 250; int failed = 0;
    while (!s->stop){
        int resume = (s->last_vip && s->token[0]) || s->ticket[0];
        set_state(s, SW_CONNECTING);
        int code = run_once(s, resume);
        int was_online = s->vip != 0;
        InterlockedExchange(&s->vip, 0);
        if (was_online && s->cb.on_down) s->cb.on_down(s->cb.user);
        if (s->stop) break;
        if (s->state == SW_REMOVED || code == CLOSE_KICKED || code == CLOSE_BANNED || code == CLOSE_RENAME){
            if (s->state != SW_REMOVED && s->cb.on_removed) s->cb.on_removed(s->cb.user, code == CLOSE_BANNED, "");
            set_state(s, SW_REMOVED);
            log_(s, "removed by the operator: not reconnecting");
            break;
        }
        if (code == -1 || code > 0) { backoff = 250; failed = 0; } else failed++;
        if (code == CLOSE_FULL) log_(s, "the server is full; trying again");
        else if (code == -1) log_(s, "lost the switch; reconnecting");
        else if (failed == 1 || failed % 12 == 0) logn(s, "no switch yet (", (uint32_t)failed, " attempts); trying again");
        set_state(s, SW_DOWN);
        for (DWORD waited = 0; waited < backoff && !s->stop; waited += 50) Sleep(50);
        backoff = backoff * 2 > 5000 ? 5000 : backoff * 2;
    }
    return 0;
}

static DWORD WINAPI pinger_main(LPVOID p){
    sw *s = p; DWORD every = (DWORD)(s->o.ping_ms > 0 ? s->o.ping_ms : 10000), t = 0;
    while (!s->stop){
        Sleep(100); t += 100;
        if (t >= every){ t = 0; sw_ping(s); }
    }
    return 0;
}

sw *sw_start(const sw_options *o, const sw_callbacks *cb){
    sw *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s);
    if (!s) return 0;
    s->o = *o; if (cb) s->cb = *cb;
    scopy(s->url, sizeof s->url, o->url); s->o.url = s->url;
    scopy(s->name, sizeof s->name, o->name ? o->name : "player");
    scopy(s->claim, sizeof s->claim, o->claim);
    s->lan_on = o->lan != 0;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); s->qpf = f.QuadPart ? f.QuadPart : 1;
    InitializeCriticalSection(&s->lock);
    if (!parse_url(s)){ log_(s, "the URL must be ws://host[:port]/lan or wss://host/lan"); DeleteCriticalSection(&s->lock); HeapFree(GetProcessHeap(), 0, s); return 0; }
    set_state(s, SW_CONNECTING);
    s->thread = CreateThread(0, 0, reader_main, s, 0, 0);
    s->pinger = CreateThread(0, 0, pinger_main, s, 0, 0);
    return s;
}

void sw_stop(sw *s){
    if (!s) return;
    InterlockedExchange(&s->stop, 1);
    close_ws(s);                                          /* ends the reader's pending receive */
    if (s->thread){ WaitForSingleObject(s->thread, 5000); CloseHandle(s->thread); }
    if (s->pinger){ WaitForSingleObject(s->pinger, 2000); CloseHandle(s->pinger); }
    if (s->hs) WinHttpCloseHandle(s->hs);
    DeleteCriticalSection(&s->lock);
    HeapFree(GetProcessHeap(), 0, s);
}
sw_state sw_status(const sw *s){ return (sw_state)s->state; }
uint32_t sw_vip(const sw *s){ return (uint32_t)s->vip; }
