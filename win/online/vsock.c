/* vsock.c -- the virtual IP stack under the native Winsock replacement (vsock.h): the port of runtime/web/net_lan.c.
 *
 * Everything below is net_lan.c's rule for rule (read the two side by side): Windows bind semantics (a second binder
 * of a port needs SO_REUSEADDR), ephemeral ports 49152-65534, UDP unicast by address and port, broadcast to every
 * socket bound to that port on every machine *including the sender* (Windows loops its own broadcasts back), a "port
 * unreachable" notice for a unicast nobody listens to, delivered to the socket that sent it, TCP over the switch's own
 * stream protocol (SYN -> SYNACK | RST, then DATA, FIN, RST, each frame naming the connection by its 4-tuple and the
 * connector's random id; refused when there is no listener or the backlog + 4 is full; no answer in 3 s: timed out),
 * ICMP echo answered here, raw sockets seeing every ICMP message, traffic to the own address or 127/8 never leaving
 * the process, the limits on the answers this "host" gives by itself.
 *
 * What changed for a native process:
 *   - the page and its rings: the TX ring is in this process; a writer thread sends its frames to the switch client
 *     (vs_transport.send) without the lock, in order, after telling it the LAN state when that changed (as the page
 *     sends {"t":"lan"} before the frames queued after a bind); the switch client's reader thread hands received
 *     frames to vs_rx_frame, which handles them at once (the RX ring's job);
 *   - the page's link state (page_state, generation) is vs_link_up / vs_link_down; frames queued for an old link are
 *     dropped (each slot remembers its link), as the page drops them while offline;
 *   - waiting: one condition variable that every change wakes (net_lan.c's event word g_seq);
 *   - the race statistics and the runtime's own UDP port (racehorn.c, racenet.c) are browser features: not here.
 *
 * Threads and locks: vsock_ws2.c calls in with its own lock (g_api) held; this file's state is under g_mu, which the
 * switch's reader thread and the writer thread take too.  g_mu is never held while taking g_api or calling the
 * transport.  Addresses: WsSock and the Winsock side use network order (in memory a.b.c.d), frames host order. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include "vsock_int.h"

void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int snprintf(char *buf, size_t n, const char *fmt, ...);

/* the switch's frame (net_proto.h) */
#define NET_KIND_LAN_UDP   3u
#define NET_KIND_LAN_TCP   4u
#define NET_KIND_LAN_ICMP  5u
#define NET_BROADCAST      0xFFFFFFFFu
#define NET_MAX_PAYLOAD    1472u
#define NET_LAN_HDR        8u
#define NET_LAN_UDP_DATA     0u
#define NET_LAN_UDP_UNREACH  1u
#define NET_LAN_TCP_SYN      1u
#define NET_LAN_TCP_SYNACK   2u
#define NET_LAN_TCP_RST      3u
#define NET_LAN_TCP_DATA     4u
#define NET_LAN_TCP_FIN      5u
#define FRAME_HDR          16u

/* ---- freestanding helpers --------------------------------------------------------------- */
void *vs_alloc(size_t n){ return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
void *vs_realloc(void *p, size_t n){
    return p ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, n ? n : 1) : vs_alloc(n);
}
void vs_free(void *p){ if (p) HeapFree(GetProcessHeap(), 0, p); }
static uint32_t g_rnd;
uint32_t vs_random(void){                                /* ids and ports: unique, not secret (g_mu held) */
    if (!g_rnd){
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        g_rnd = (uint32_t)c.QuadPart ^ (uint32_t)(c.QuadPart >> 32) * 0x9E3779B9u ^ GetCurrentProcessId() << 16 ^ GetTickCount();
        if (!g_rnd) g_rnd = 0x6E667332u;
    }
    g_rnd ^= g_rnd << 13; g_rnd ^= g_rnd >> 17; g_rnd ^= g_rnd << 5;
    uint32_t h = g_rnd * 0x846CA68Bu; return h ^ h >> 16;
}

/* ---- state ------------------------------------------------------------------- */
static CRITICAL_SECTION g_mu;
static CONDITION_VARIABLE g_cv;
static volatile LONG g_inited;
static uint32_t g_vip, g_mask;              /* host order, taken from the link at vlan_hello (a session's address) */
static int g_hello;
/* the link (the switch client's): up, its address, which link (+1 per welcome) */
static int g_up; static uint32_t g_link_vip, g_link_mask, g_gen;
/* the LAN state for the switch: want (a session is open) and the UDP ports; g_lan_seq +1 per change */
static uint32_t g_want, g_udp_ports = 0x80000000u, g_lan_seq;

#define LOCAL_CAP   (1u << 20)              /* a local stream's receive buffer before the writer gets WOULDBLOCK */
#define REMOTE_CAP  (16u << 20)             /* a remote stream's receive buffer before it is reset */
#define DGQ_MAX     1024                    /* datagrams queued per socket */
#define DGQ_BYTES   (1u << 20)
#define CONNECT_TMO 3000000ull              /* us: no answer to a SYN (vlanhub: host unreachable after 3 s) */
#define TCP_CHUNK   (NET_MAX_PAYLOAD - NET_LAN_HDR)
#define UDP_MAX     (NET_MAX_PAYLOAD - NET_LAN_HDR)

static struct {
    uint64_t tx_frames, rx_frames, tx_full, tx_offline;
    uint64_t udp_out, udp_in, udp_local, udp_noport, udp_outside, udp_qfull, udp_oversize, udp_bcast_out;
    uint64_t tcp_out, tcp_in, tcp_local, tcp_refused, tcp_unreach, tcp_timeout, tcp_rst_in, tcp_rst_out, tcp_bytes_out, tcp_bytes_in;
    uint64_t tcp_reset_link, tcp_overflow, icmp_out, icmp_in, echo_answered, bad;
    uint64_t unreach_sender, unreach_conn, unreach_first, unreach_none, ctl_waited, ctl_lost, auto_held, tx_old_link;
} S;

static uint64_t now_us(void){ return ws_now_us(); }
static inline uint32_t H(uint32_t n){ return __builtin_bswap32(n); }   /* network <-> host order (x86 is little-endian) */
static void bump(void){ WakeAllConditionVariable(&g_cv); }
static const char *ipn(uint32_t n, char *b){ const uint8_t *q = (const uint8_t *)&n; snprintf(b, 16, "%u.%u.%u.%u", q[0], q[1], q[2], q[3]); return b; }
static int is_loop_n(uint32_t n){ return ((const uint8_t *)&n)[0] == 127; }
static uint32_t own_n(void){ return H(g_vip); }
static int online(void){ return g_up && g_link_vip == g_vip; }
static int in_subnet_h(uint32_t h){ return g_mask && (h & g_mask) == (g_vip & g_mask); }
static int bcast_h(uint32_t h){ return h == 0xFFFFFFFFu || (g_mask && h == ((g_vip & g_mask) | ~g_mask)); }
/* the browser asks the page's peer bitmap; the native client does not keep the peer list (an address nobody holds:
 * the SYN finds nobody and the connect times out after the same 3 s, WSAETIMEDOUT instead of WSAEHOSTUNREACH) */
static int peer_present_h(uint32_t h){ return in_subnet_h(h); }

/* ---- virtual sockets (one per winsock SOCKET: s->vid) ------------------------- */
typedef struct Dgram { struct Dgram *next; uint32_t ip; uint16_t port, flags; int len; uint8_t data[]; } Dgram;
typedef struct VConn VConn;
#define SENT_N 8
typedef struct { uint32_t ip; uint16_t port; uint32_t seq; } Sent;
typedef struct {
    int used, kind;                         /* 1 tcp, 2 udp, 3 icmp (raw) */
    uint32_t bip; uint16_t bport; int bound, reuse;   /* bip network order */
    Dgram *qh, *qt; int qn; uint32_t qbytes;
    int listening, backlog, pending; VConn *acc_h, *acc_t;
    int vfd;                                /* udp/icmp: always; tcp: the listener's, once listening; -1 none */
    Sent sent[SENT_N];                      /* udp: the last unicast destinations on the switch (network order) */
    int has_peer; uint32_t raddr; uint16_t rport;   /* udp: the default destination, as of the last call */
} VSock;
static uint32_t g_sent_seq;
#define NVS 1024
static VSock g_vs[NVS];

enum { C_SYN_SENT = 1, C_EST, C_REFUSED, C_FAILED };
struct VConn {
    VConn *next, *prev;                     /* all connections */
    uint32_t cid;                           /* the connector's id */
    uint32_t lip, rip;                      /* network order */
    uint16_t lport, rport;
    int state, local;
    VConn *peer;                            /* local: the other end (NULL once it closed) */
    int rx_fin, rx_rst, tx_fin, rst_sent;
    uint8_t *rb; uint32_t roff, rlen, rcap;
    VSock *listener; VConn *acc_next;       /* waiting in a listener's accept queue */
    uint64_t deadline;                      /* SYN_SENT: give up then */
    int vfd;
    /* diagnostics (conn_report): bytes each way, and when the game last polled it for reading, called recv on it,
     * and got data from it; stall_said: "unread for 20 s" logged */
    uint64_t t0, polled_rd, recv_call, read_at;
    uint32_t bytes_in, bytes_out;
    int stall_said;
};
static VConn *g_conns;

/* ---- virtual fds --------------------------------------------------------------- */
enum { VF_FREE = 0, VF_DGRAM, VF_LISTEN, VF_STREAM, VF_DEAD };
#define NVFD 4096
static struct { int type; void *p; } g_vf[NVFD];
static int g_vf_next;
static int vfd_new(int type, void *p){
    for (int k = 0; k < NVFD; k++){
        int i = (g_vf_next + k) % NVFD;
        if (g_vf[i].type != VF_FREE) continue;
        g_vf[i].type = type; g_vf[i].p = p; g_vf_next = (i + 1) % NVFD;
        return VFD_BASE + i;
    }
    return -1;
}
static int vfd_ix(int fd){ return fd >= VFD_BASE && fd < VFD_BASE + NVFD && g_vf[fd - VFD_BASE].type != VF_FREE ? fd - VFD_BASE : -1; }
static VConn *stream_of(int fd){ int i = vfd_ix(fd); return i >= 0 && g_vf[i].type == VF_STREAM ? (VConn *)g_vf[i].p : 0; }
static VSock *vs_of(uint32_t vid){ return vid && vid < NVS && g_vs[vid].used ? &g_vs[vid] : 0; }

/* the LAN state changed (g_mu held): the writer tells the switch before the frames queued after this */
static HANDLE g_tx_ev;
static void lan_note(void){ g_lan_seq++; if (g_tx_ev) SetEvent(g_tx_ev); }
/* udp_ports: which UDP ports a socket is bound to here, a bit each (port % 31), for the switch: a LAN broadcast goes
 * only to the machines with a socket on its port (net_lan.c ports_note) (g_mu held) */
static void ports_note(void){
    uint32_t m = 0x80000000u;
    for (int i = 1; i < NVS; i++)
        if (g_vs[i].used && g_vs[i].kind == 2 && g_vs[i].bound) m |= 1u << (g_vs[i].bport % 31u);
    if (g_udp_ports == m) return;
    g_udp_ports = m; lan_note();
}

/* ---- the TX ring (the page's, in this process) ---------------------------------------- */
#define NET_TX_SLOTS 128u
typedef struct { uint32_t len, kind, dst_h, gen; uint16_t sport, dport; uint8_t data[NET_MAX_PAYLOAD]; } TxSlot;
static TxSlot *g_tx;
static volatile LONG g_tx_head, g_tx_tail;  /* free-running; head written under g_mu, tail by the writer under g_mu */
static uint32_t tx_free(void){ return g_tx ? NET_TX_SLOTS - ((uint32_t)g_tx_head - (uint32_t)g_tx_tail) : 0; }
#define TX_RESERVE 4u
typedef struct { uint32_t dst_h; uint16_t sport, dport; uint8_t op; uint32_t cid; } CtlFrame;
#define CTLQ_N 1024
static CtlFrame g_ctlq[CTLQ_N]; static uint32_t g_ctlq_h, g_ctlq_n;
/* one frame to the switch (g_mu held); dst_h host order, NET_BROADCAST for everyone else */
static int tx_raw(uint32_t kind, uint32_t dst_h, uint16_t sport, uint16_t dport, uint8_t op, uint8_t ttl, uint32_t cid,
                  const uint8_t *data, uint32_t n){
    if (!online()){ S.tx_offline++; return 0; }
    if (n > NET_MAX_PAYLOAD - NET_LAN_HDR){ S.bad++; return 0; }
    uint32_t h = (uint32_t)g_tx_head;
    if (h - (uint32_t)g_tx_tail >= NET_TX_SLOTS){ S.tx_full++; return 0; }
    TxSlot *s = &g_tx[h % NET_TX_SLOTS];
    s->len = NET_LAN_HDR + n; s->kind = kind; s->dst_h = dst_h; s->gen = g_gen;
    s->sport = sport; s->dport = dport;
    s->data[0] = op; s->data[1] = ttl; s->data[2] = s->data[3] = 0;
    s->data[4] = (uint8_t)cid; s->data[5] = (uint8_t)(cid >> 8); s->data[6] = (uint8_t)(cid >> 16); s->data[7] = (uint8_t)(cid >> 24);
    if (n) memcpy(s->data + NET_LAN_HDR, data, n);
    InterlockedExchange(&g_tx_head, (LONG)(h + 1));
    SetEvent(g_tx_ev);
    S.tx_frames++;
    return 1;
}
static void ctl_flush(void){
    while (g_ctlq_n && online() && tx_free()){
        const CtlFrame *q = &g_ctlq[g_ctlq_h];
        tx_raw(NET_KIND_LAN_TCP, q->dst_h, q->sport, q->dport, q->op, 64, q->cid, 0, 0);
        g_ctlq_h = (g_ctlq_h + 1) % CTLQ_N; g_ctlq_n--;
    }
}
static int tx(uint32_t kind, uint32_t dst_h, uint16_t sport, uint16_t dport, uint8_t op, uint8_t ttl, uint32_t cid,
              const uint8_t *data, uint32_t n){
    if (g_ctlq_n){ ctl_flush(); if (g_ctlq_n && online()){ S.tx_full++; return 0; } }
    return tx_raw(kind, dst_h, sport, dport, op, ttl, cid, data, n);
}
static void tx_ctl(uint32_t dst_h, uint16_t sport, uint16_t dport, uint8_t op, uint32_t cid){
    ctl_flush();
    if (!online()){ S.tx_offline++; return; }
    if (!g_ctlq_n && tx_free()){ tx_raw(NET_KIND_LAN_TCP, dst_h, sport, dport, op, 64, cid, 0, 0); return; }
    if (g_ctlq_n == CTLQ_N){
        if (!S.ctl_lost++) ws_log("virtual LAN: %u TCP control frames wait for the TX ring: one dropped", CTLQ_N);
        return;
    }
    S.tx_full++; S.ctl_waited++;
    g_ctlq[(g_ctlq_h + g_ctlq_n++) % CTLQ_N] = (CtlFrame){ dst_h, sport, dport, op, cid };
}
static void tcp_ctl(VConn *k, uint8_t op){ tx_ctl(H(k->rip), k->lport, k->rport, op, k->cid); }

/* ---- sockets: allocation, ports ------------------------------------------------------ */
static void dg_free_all(VSock *v){
    while (v->qh){ Dgram *d = v->qh; v->qh = d->next; vs_free(d); }
    v->qt = 0; v->qn = 0; v->qbytes = 0;
}
static int port_used(int tcp, uint16_t port, uint32_t ip, VSock *except, int *reuse_all){
    int used = 0; *reuse_all = 1;
    for (int i = 1; i < NVS; i++){
        VSock *o = &g_vs[i];
        if (!o->used || o == except || !o->bound || (o->kind == 1) != tcp || o->bport != port) continue;
        if (o->bip == ip || !o->bip || !ip){ used = 1; if (!o->reuse) *reuse_all = 0; }
    }
    return used;
}
static uint16_t ephemeral(int tcp){
    for (int k = 0; k < 20000; k++){
        uint16_t p = (uint16_t)(49152u + vs_random() % (65535u - 49152u));
        int ra; if (!port_used(tcp, p, 0, 0, &ra)) return p;
    }
    return 0;
}

/* ---- connections ---------------------------------------------------------------------- */
static VConn *conn_new(uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport, uint32_t cid, int state){
    VConn *k = vs_alloc(sizeof *k);
    if (!k) return 0;
    k->vfd = vfd_new(VF_STREAM, k);
    if (k->vfd < 0){ vs_free(k); return 0; }
    k->lip = lip; k->lport = lport; k->rip = rip; k->rport = rport; k->cid = cid; k->state = state;
    k->t0 = now_us();
    k->next = g_conns; if (g_conns) g_conns->prev = k; g_conns = k;
    return k;
}
static void conn_free(VConn *k){
    if (k->peer){ k->peer->peer = 0; }
    if (k->prev) k->prev->next = k->next; else g_conns = k->next;
    if (k->next) k->next->prev = k->prev;
    int i = vfd_ix(k->vfd);
    if (i >= 0 && g_vf[i].p == k){ g_vf[i].type = VF_FREE; g_vf[i].p = 0; }
    vs_free(k->rb); vs_free(k);
}
static VConn *conn_find(uint32_t rip, uint16_t rport, uint16_t lport, uint32_t cid){
    for (VConn *k = g_conns; k; k = k->next)
        if (!k->local && k->rip == rip && k->rport == rport && k->lport == lport && k->cid == cid) return k;
    return 0;
}
static int rb_put(VConn *k, const uint8_t *p, uint32_t n, uint32_t cap){
    if (!n) return 1;
    if (k->rlen + n > cap) return 0;
    if (k->roff && k->roff + k->rlen + n > k->rcap){ memmove(k->rb, k->rb + k->roff, k->rlen); k->roff = 0; }
    if (k->roff + k->rlen + n > k->rcap){
        uint32_t nc = k->rcap ? k->rcap : 4096;
        while (nc < k->roff + k->rlen + n) nc *= 2;
        uint8_t *nb = vs_realloc(k->rb, nc); if (!nb) return 0;
        k->rb = nb; k->rcap = nc;
    }
    memcpy(k->rb + k->roff + k->rlen, p, n); k->rlen += n;
    return 1;
}
static void got_rst(VConn *k){
    k->rx_rst = 1; k->rlen = 0; k->roff = 0;           /* a reset discards what was not read */
    if (k->state == C_SYN_SENT) k->state = C_REFUSED;
}
static void send_rst(VConn *k){
    if (k->rst_sent || k->rx_rst) return;
    k->rst_sent = 1;
    if (k->local){ if (k->peer) got_rst(k->peer); }
    else if (k->state == C_EST || k->state == C_SYN_SENT){ tcp_ctl(k, NET_LAN_TCP_RST); S.tcp_rst_out++; }
}
static void send_fin(VConn *k){
    if (k->tx_fin || k->rst_sent || k->rx_rst || k->state != C_EST) return;
    k->tx_fin = 1;
    if (k->local){ if (k->peer) k->peer->rx_fin = 1; }
    else tcp_ctl(k, NET_LAN_TCP_FIN);
}
static int reset_remote(uint32_t only_rip, const char *why){
    int n = 0;
    for (VConn *k = g_conns; k; k = k->next){
        if (k->local || k->rx_rst || (only_rip && k->rip != only_rip)) continue;
        if (k->state == C_SYN_SENT){ k->state = C_FAILED; n++; continue; }
        if (k->state != C_EST) continue;
        got_rst(k); n++;
    }
    if (n){ S.tcp_reset_link += (uint64_t)n; ws_log("virtual LAN: %d TCP connection(s) reset: %s", n, why); }
    return n;
}
static VSock *find_listener(uint32_t dst_n, uint16_t port){
    for (int i = 1; i < NVS; i++){
        VSock *o = &g_vs[i];
        if (o->used && o->kind == 1 && o->listening && o->bport == port &&
            (!o->bip || o->bip == dst_n || (is_loop_n(dst_n) && is_loop_n(o->bip)))) return o;
    }
    return 0;
}
static void acc_push(VSock *l, VConn *k){
    k->listener = l; k->acc_next = 0;
    if (l->acc_t) l->acc_t->acc_next = k; else l->acc_h = k;
    l->acc_t = k; l->pending++;
}

/* ---- datagrams --------------------------------------------------------------------- */
static void dg_push(VSock *v, uint32_t src_n, uint16_t sport, uint16_t flags, const uint8_t *p, int n){
    if (v->qn >= DGQ_MAX || v->qbytes + (uint32_t)n > DGQ_BYTES){ S.udp_qfull++; return; }
    Dgram *d = vs_alloc(sizeof *d + (size_t)n);
    if (!d){ S.udp_qfull++; return; }
    d->next = 0; d->ip = src_n; d->port = sport; d->flags = flags; d->len = n;
    if (n) memcpy(d->data, p, (size_t)n);
    if (v->qt) v->qt->next = d; else v->qh = d;
    v->qt = d; v->qn++; v->qbytes += (uint32_t)n;
}
static int udp_deliver(uint32_t src_n, uint16_t sport, uint32_t dst_n, uint16_t dport, const uint8_t *p, int n, int bc){
    VSock *first = 0, *exact = 0; int k = 0;
    for (int i = 1; i < NVS; i++){
        VSock *v = &g_vs[i];
        if (!v->used || v->kind != 2 || !v->bound || v->bport != dport) continue;
        if (!(!v->bip || v->bip == dst_n || (bc && v->bip == own_n()) || (is_loop_n(dst_n) && is_loop_n(v->bip)))) continue;
        if (bc){ dg_push(v, src_n, sport, 0, p, n); k++; continue; }
        if (!first) first = v;
        if (!exact && v->bip == dst_n) exact = v;
    }
    if (!bc && (exact || first)){ dg_push(exact ? exact : first, src_n, sport, 0, p, n); k = 1; }
    return k;
}
static void icmp_deliver(uint32_t src_n, const uint8_t *p, int n){
    for (int i = 1; i < NVS; i++){
        VSock *v = &g_vs[i];
        if (v->used && v->kind == 3) dg_push(v, src_n, 0, 0, p, n);
    }
}
static void icmp_cksum(uint8_t *m, int n){
    m[2] = m[3] = 0;
    uint32_t sum = 0;
    for (int i = 0; i + 1 < n; i += 2) sum += (uint32_t)(m[i] << 8 | m[i + 1]);
    if (n & 1) sum += (uint32_t)m[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    sum = ~sum & 0xFFFF; m[2] = (uint8_t)(sum >> 8); m[3] = (uint8_t)sum;
}
static void unreach_deliver(uint32_t src_n, uint16_t sport, uint16_t dport){
    VSock *sender = 0, *conn = 0, *first = 0; uint32_t best = 0;
    for (int i = 1; i < NVS; i++){
        VSock *v = &g_vs[i];
        if (!v->used || v->kind != 2 || !v->bound || v->bport != dport) continue;
        for (int j = 0; j < SENT_N; j++)
            if (v->sent[j].seq && v->sent[j].ip == src_n && v->sent[j].port == sport &&
                (!sender || (int32_t)(v->sent[j].seq - best) > 0)){ sender = v; best = v->sent[j].seq; }
        if (!conn && v->has_peer && v->raddr == src_n && v->rport == sport) conn = v;
        if (!first && !v->has_peer) first = v;
    }
    VSock *to = sender ? sender : conn ? conn : first;
    if (sender) S.unreach_sender++; else if (conn) S.unreach_conn++; else if (first) S.unreach_first++; else S.unreach_none++;
    if (to) dg_push(to, src_n, sport, 1, 0, 0);
}
static void sent_note(VSock *v, uint32_t ip, uint16_t port){
    int k = -1;
    for (int j = 0; j < SENT_N && k < 0; j++) if (v->sent[j].seq && v->sent[j].ip == ip && v->sent[j].port == port) k = j;
    for (int j = 0; j < SENT_N && k < 0; j++) if (!v->sent[j].seq) k = j;
    if (k < 0){ k = 0; for (int j = 1; j < SENT_N; j++) if ((int32_t)(v->sent[j].seq - v->sent[k].seq) < 0) k = j; }
    if (!++g_sent_seq) ++g_sent_seq;
    v->sent[k] = (Sent){ ip, port, g_sent_seq };
}

/* ---- received frames ------------------------------------------------------------------ */
#define AUTO_COST  64u
#define AUTO_SRC   4096u
#define AUTO_ALL   32768u
typedef struct { uint64_t t, v; int set; } AutoBucket;
static AutoBucket g_auto_src[256], g_auto_all;
static int auto_has(AutoBucket *b, uint64_t rate, uint64_t n, uint64_t now){
    uint64_t cap = 2u * rate * 1000000u;
    if (!b->set){ b->set = 1; b->v = cap; }
    else if (now > b->t){ b->v += (now - b->t) * rate; if (b->v > cap) b->v = cap; }
    b->t = now;
    return b->v >= n * 1000000u;
}
static int auto_ok(uint32_t src_h, uint32_t len){
    uint64_t now = now_us(), n = (uint64_t)len + AUTO_COST;
    AutoBucket *b = &g_auto_src[src_h & 255u];
    if (!auto_has(b, AUTO_SRC, n, now) | !auto_has(&g_auto_all, AUTO_ALL, n, now)){ S.auto_held++; return 0; }
    b->v -= n * 1000000u; g_auto_all.v -= n * 1000000u;
    return 1;
}
typedef struct { uint32_t kind, src_ip, dst_ip, len; uint16_t src_port, dst_port; const uint8_t *data; } RxFrame;
static int rst_back(const RxFrame *sl, uint32_t cid){
    if (!auto_ok(sl->src_ip, NET_LAN_HDR)) return 0;
    tx_ctl(sl->src_ip, sl->dst_port, sl->src_port, NET_LAN_TCP_RST, cid);
    return 1;
}
static void rx_frame(const RxFrame *sl){
    uint32_t n = sl->len;
    if (n < NET_LAN_HDR || n > NET_MAX_PAYLOAD){ S.bad++; return; }
    const uint8_t *d = sl->data;
    uint8_t op = d[0];
    uint32_t cid = (uint32_t)d[4] | (uint32_t)d[5] << 8 | (uint32_t)d[6] << 16 | (uint32_t)d[7] << 24;
    const uint8_t *p = d + NET_LAN_HDR; int pn = (int)(n - NET_LAN_HDR);
    uint32_t src_n = H(sl->src_ip), dst_h = sl->dst_ip;
    uint16_t sport = sl->src_port, dport = sl->dst_port;
    int bc = dst_h == NET_BROADCAST || bcast_h(dst_h);
    if (!bc && dst_h != g_vip){ S.bad++; return; }
    S.rx_frames++;
    char b1[16], b2[16];
    switch (sl->kind){
    case NET_KIND_LAN_UDP:
        if (op == NET_LAN_UDP_UNREACH){ unreach_deliver(src_n, sport, dport); break; }
        S.udp_in++;
        if (!udp_deliver(src_n, sport, bc ? 0xFFFFFFFFu : own_n(), dport, p, pn, bc) && !bc){
            S.udp_noport++;
            if (auto_ok(sl->src_ip, NET_LAN_HDR))
                tx(NET_KIND_LAN_UDP, sl->src_ip, dport, sport, NET_LAN_UDP_UNREACH, 64, 0, 0, 0);
        }
        break;
    case NET_KIND_LAN_ICMP:
        if (pn < 8) break;
        S.icmp_in++;
        icmp_deliver(src_n, p, pn);
        if (p[0] == 8 && !bc && auto_ok(sl->src_ip, n)){
            uint8_t rep[NET_MAX_PAYLOAD];
            memcpy(rep, p, (size_t)pn); rep[0] = 0; rep[1] = 0; icmp_cksum(rep, pn);
            if (tx(NET_KIND_LAN_ICMP, sl->src_ip, 0, 0, 0, 64, 0, rep, (uint32_t)pn)) S.echo_answered++;
        }
        break;
    case NET_KIND_LAN_TCP: {
        if (bc) break;
        VConn *k = conn_find(src_n, sport, dport, cid);
        switch (op){
        case NET_LAN_TCP_SYN: {
            if (k) break;
            VSock *l = find_listener(own_n(), dport);
            if (!l || l->pending >= l->backlog + 4){
                S.tcp_refused++;
                if (rst_back(sl, cid))
                    ws_log("virtual LAN: tcp %s:%u -> :%u refused (%s)", ipn(src_n, b1), sport, dport, l ? "backlog full" : "no listener");
                break;
            }
            k = conn_new(own_n(), dport, src_n, sport, cid, C_EST);
            if (!k){ rst_back(sl, cid); break; }
            acc_push(l, k);
            S.tcp_in++;
            tx_ctl(sl->src_ip, dport, sport, NET_LAN_TCP_SYNACK, cid);
            ws_log("virtual LAN: tcp %s:%u -> %s:%u accepted (queued on the listener)", ipn(src_n, b1), sport, ipn(own_n(), b2), dport);
            break; }
        case NET_LAN_TCP_SYNACK:
            if (k && k->state == C_SYN_SENT) k->state = C_EST;
            else if (!k || k->state != C_EST) rst_back(sl, cid);
            break;
        case NET_LAN_TCP_RST:
            if (k){ S.tcp_rst_in++; if (k->state == C_SYN_SENT) S.tcp_refused++; got_rst(k); }
            break;
        case NET_LAN_TCP_DATA:
            if (!k || k->state != C_EST || k->rx_rst){
                if (!k || !k->rst_sent) rst_back(sl, cid);
                break;
            }
            if (k->rx_fin) break;
            if (!rb_put(k, p, (uint32_t)pn, REMOTE_CAP)){
                S.tcp_overflow++;
                ws_log("virtual LAN: tcp from %s:%u: %u bytes unread, the connection is reset", ipn(src_n, b1), sport, k->rlen);
                send_rst(k); got_rst(k);
                break;
            }
            S.tcp_bytes_in += (uint64_t)pn; k->bytes_in += (uint32_t)pn;
            break;
        case NET_LAN_TCP_FIN:
            if (k) k->rx_fin = 1;
            break;
        default: S.bad++; break;
        }
        break; }
    default: S.bad++; break;
    }
}
static uint32_t be32(const uint8_t *p){ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void vs_rx_frame(const uint8_t *f, int len){
    if (!g_inited || len < (int)FRAME_HDR) return;
    RxFrame r = { f[0], be32(f + 4), be32(f + 8), (uint32_t)(len - (int)FRAME_HDR),
                  (uint16_t)(f[12] << 8 | f[13]), (uint16_t)(f[14] << 8 | f[15]), f + FRAME_HDR };
    if (r.kind < NET_KIND_LAN_UDP || r.kind > NET_KIND_LAN_ICMP) return;
    EnterCriticalSection(&g_mu);
    if (g_hello && online()) rx_frame(&r);              /* frames to an earlier address of ours: not ours */
    LeaveCriticalSection(&g_mu);
    bump();
}
void vs_peer_gone(uint32_t vip){
    if (!g_inited) return;
    EnterCriticalSection(&g_mu);
    if (g_hello && vip != g_vip) reset_remote(H(vip), "the other player left the switch");
    LeaveCriticalSection(&g_mu);
    bump();
}

static void stats_line(const char *when){
    ws_log("virtual LAN %s: frames out %llu in %llu; udp out %llu (broadcast %llu, local %llu) in %llu, no port %llu, "
           "outside the LAN %llu, queue full %llu; tcp out %llu in %llu local %llu, refused %llu, unreachable %llu, "
           "timed out %llu, resets in %llu out %llu, link resets %llu, bytes out %llu in %llu; icmp out %llu in %llu, "
           "echo answered %llu, answers over the limit %llu; port unreachable to the sender %llu (connected %llu, first binder %llu, nobody %llu); "
           "TX ring full %llu (control frames waited %llu, lost %llu), offline %llu, for an old link %llu",
           when, (unsigned long long)S.tx_frames, (unsigned long long)S.rx_frames, (unsigned long long)S.udp_out,
           (unsigned long long)S.udp_bcast_out, (unsigned long long)S.udp_local, (unsigned long long)S.udp_in,
           (unsigned long long)S.udp_noport, (unsigned long long)S.udp_outside, (unsigned long long)S.udp_qfull,
           (unsigned long long)S.tcp_out, (unsigned long long)S.tcp_in, (unsigned long long)S.tcp_local,
           (unsigned long long)S.tcp_refused, (unsigned long long)S.tcp_unreach, (unsigned long long)S.tcp_timeout,
           (unsigned long long)S.tcp_rst_in, (unsigned long long)S.tcp_rst_out, (unsigned long long)S.tcp_reset_link,
           (unsigned long long)S.tcp_bytes_out, (unsigned long long)S.tcp_bytes_in, (unsigned long long)S.icmp_out,
           (unsigned long long)S.icmp_in, (unsigned long long)S.echo_answered, (unsigned long long)S.auto_held, (unsigned long long)S.unreach_sender,
           (unsigned long long)S.unreach_conn, (unsigned long long)S.unreach_first, (unsigned long long)S.unreach_none,
           (unsigned long long)S.tx_full, (unsigned long long)S.ctl_waited, (unsigned long long)S.ctl_lost,
           (unsigned long long)S.tx_offline, (unsigned long long)S.tx_old_link);
}
/* every remote TCP connection: how long, bytes each way, unread, and when the game last polled, called recv, read
 * (g_mu held) */
static const char *ago(uint64_t now, uint64_t t, char *b){
    if (!t){ snprintf(b, 24, "never"); return b; }
    snprintf(b, 24, "%.1f s ago", (double)(now - t) / 1e6); return b;
}
static void conn_report(void){
    uint64_t now = now_us();
    for (VConn *k = g_conns; k; k = k->next){
        if (k->local || k->state != C_EST) continue;
        char ip[16], a1[24], a2[24], a3[24];
        ws_log("virtual LAN: tcp to %s:%u for %.0f s: in %u B, out %u B, unread %u B%s; polled for reading %s, recv %s, "
               "data read %s", ipn(k->rip, ip), k->rport, (double)(now - k->t0) / 1e6, k->bytes_in, k->bytes_out, k->rlen,
               k->rx_fin ? " (closed by the other end)" : k->rx_rst ? " (reset)" : "",
               ago(now, k->polled_rd, a1), ago(now, k->recv_call, a2), ago(now, k->read_at, a3));
    }
}
/* data that waits unread for 20 s: said once, when it happens (the writer thread; g_mu held) */
#define STALL_US 20000000ull
static void stall_check(void){
    uint64_t now = now_us();
    for (VConn *k = g_conns; k; k = k->next){
        if (k->local || k->state != C_EST || !k->rlen || k->stall_said) continue;
        uint64_t since = k->read_at > k->t0 ? k->read_at : k->t0;
        if (now - since < STALL_US) continue;
        k->stall_said = 1;
        char ip[16], a1[24], a2[24];
        ws_log("virtual LAN: tcp to %s:%u: %u B unread for %.0f s (polled for reading %s, recv %s): the game is not "
               "reading this connection", ipn(k->rip, ip), k->rport, k->rlen, (double)(now - since) / 1e6,
               ago(now, k->polled_rd, a1), ago(now, k->recv_call, a2));
    }
}

void vs_report(void){
    if (!g_inited) return;
    EnterCriticalSection(&g_mu);
    if (g_hello) stats_line("so far");
    LeaveCriticalSection(&g_mu);
}

/* ---- the link (the page's link state in the browser) ----------------------------------- */
void vs_link_up(uint32_t vip, uint32_t mask){
    if (!g_inited) return;
    char b[16];
    EnterCriticalSection(&g_mu);
    if (g_hello && g_up){                                /* a new link without word that the old one went */
        g_ctlq_n = 0;
        reset_remote(0, "the switch connection was renewed (frames were lost meanwhile)");
    }
    int was = g_up;
    g_up = 1; g_link_vip = vip; g_link_mask = mask; g_gen++;
    if (g_hello && !was) ws_log("virtual LAN up again%s", vip == g_vip ? "" : " with another address: no network until the next session");
    if (g_hello && vip == g_vip) lan_note();             /* the new connection's hello said it; say it again, in order */
    LeaveCriticalSection(&g_mu);
    (void)b;
    bump();
}
void vs_link_down(void){
    if (!g_inited) return;
    EnterCriticalSection(&g_mu);
    if (g_up){
        g_up = 0; g_gen++;
        if (g_hello){
            g_ctlq_n = 0;
            reset_remote(0, "lost the switch (frames were lost meanwhile)");
            ws_log("virtual LAN down: reconnecting to the server's switch");
        }
    }
    LeaveCriticalSection(&g_mu);
    bump();
}

/* ---- the writer thread (the page's TX loop) ----------------------------------------------- */
static DWORD WINAPI writer_main(LPVOID u){
    (void)u;
    uint8_t *f = vs_alloc(FRAME_HDR + NET_MAX_PAYLOAD);
    uint32_t lan_sent = 0; uint64_t next_stats = now_us() + 60000000ull;
    if (!f) return 1;
    for (;;){
        WaitForSingleObject(g_tx_ev, 50);
        /* the LAN state first: what a bind changed is known to the switch before the frames after it */
        EnterCriticalSection(&g_mu);
        uint32_t seq = g_lan_seq, want = g_want, udp = g_udp_ports;
        LeaveCriticalSection(&g_mu);
        if (seq != lan_sent){ lan_sent = seq; if (g_vs_tr->lan) g_vs_tr->lan(g_vs_tr->user, (int)want, udp); }
        uint32_t tail = (uint32_t)g_tx_tail, head = (uint32_t)g_tx_head; int sent = 0;
        while (tail != head){
            const TxSlot *s = &g_tx[tail % NET_TX_SLOTS];
            if (s->gen != g_gen) S.tx_old_link++;        /* queued for a link that is gone (its connections are reset) */
            else {
                uint32_t src = g_vip, dst = s->dst_h;
                f[0] = (uint8_t)s->kind; f[1] = f[2] = f[3] = 0;
                f[4] = (uint8_t)(src >> 24); f[5] = (uint8_t)(src >> 16); f[6] = (uint8_t)(src >> 8); f[7] = (uint8_t)src;
                f[8] = (uint8_t)(dst >> 24); f[9] = (uint8_t)(dst >> 16); f[10] = (uint8_t)(dst >> 8); f[11] = (uint8_t)dst;
                f[12] = (uint8_t)(s->sport >> 8); f[13] = (uint8_t)s->sport; f[14] = (uint8_t)(s->dport >> 8); f[15] = (uint8_t)s->dport;
                memcpy(f + FRAME_HDR, s->data, s->len);
                if (!g_vs_tr->send(g_vs_tr->user, f, (int)(FRAME_HDR + s->len))) S.tx_offline++;
            }
            tail++; sent++;
            if (sent % 16 == 0){                          /* room for senders while a long burst goes out */
                EnterCriticalSection(&g_mu); InterlockedExchange(&g_tx_tail, (LONG)tail); LeaveCriticalSection(&g_mu); bump();
            }
            head = (uint32_t)g_tx_head;
        }
        EnterCriticalSection(&g_mu);
        InterlockedExchange(&g_tx_tail, (LONG)tail);
        if (g_ctlq_n) ctl_flush();                        /* control frames that waited for room */
        uint64_t t = now_us(); int stats = 0;
        if (t >= next_stats){ next_stats = t + 60000000ull; stats = g_hello && winsock_session_active(); }
        if (stats){ stats_line("stats"); conn_report(); }
        stall_check();
        LeaveCriticalSection(&g_mu);
        if (sent) bump();
        vs_calls_tick();                                  /* the game's calls (vsock_ws2.c), when a report is due */
    }
    return 0;
}

int vs_init(const vs_transport *t){
    if (InterlockedCompareExchange(&g_inited, 1, 0) != 0) return 0;
    g_vs_tr = t;
    InitializeCriticalSection(&g_mu);
    InitializeConditionVariable(&g_cv);
    g_tx = vs_alloc(sizeof(TxSlot) * NET_TX_SLOTS);
    g_tx_ev = CreateEventA(0, FALSE, FALSE, 0);
    if (!g_tx || !g_tx_ev){ ws_log("virtual LAN: no memory for the TX ring"); return -1; }
    HANDLE h = CreateThread(0, 64 * 1024, writer_main, 0, 0, 0);
    if (!h){ ws_log("virtual LAN: CreateThread failed (%lu)", GetLastError()); return -1; }
    CloseHandle(h);
    return 0;
}

/* ==================================================================================== */
/* vsock_ws2.c's backend (net_lan.c's vlan_*)                                          */
/* ==================================================================================== */
static void ident_from_link(NetIdent *id){
    g_vip = g_link_vip; g_mask = g_link_mask;
    memset(id, 0, sizeof *id);
    id->ip = H(g_vip); id->mask = H(g_mask);
    id->mac[0] = 0x02; id->mac[1] = 0x4E; id->mac[2] = 0x56; id->mac[3] = 0x4C;   /* vlanhub's 02:4e:56:4c:c:d */
    id->mac[4] = (uint8_t)(g_vip >> 8); id->mac[5] = (uint8_t)g_vip;
    snprintf(id->name, sizeof id->name, "VLAN-%u", g_vip & ~g_mask);   /* the host number: unique on the switch */
}
static void set_want(uint32_t on){                        /* g_mu held */
    if (g_want == on) return;
    g_want = on; lan_note();
}
/* the link up, at most 5 s; g_api released meanwhile */
static int wait_online(void){
    uint64_t t0 = now_us();
    EnterCriticalSection(&g_mu); int up = g_up; LeaveCriticalSection(&g_mu);
    if (up) return 1;
    if (g_vs_tr->want) g_vs_tr->want(g_vs_tr->user);
    guest_wait_begin();
    EnterCriticalSection(&g_mu);
    while (!g_up){
        uint64_t el = now_us() - t0;
        if (el >= 5000000ull) break;
        SleepConditionVariableCS(&g_cv, &g_mu, (DWORD)((5000000ull - el) / 1000ull) + 1);
    }
    up = g_up;
    LeaveCriticalSection(&g_mu);
    guest_wait_end();
    return up;
}
int vlan_hello(NetIdent *id){
    if (!g_inited) return VE_NETDOWN;
    uint64_t t0 = now_us();
    EnterCriticalSection(&g_mu); set_want(1); LeaveCriticalSection(&g_mu);
    if (!wait_online()){
        EnterCriticalSection(&g_mu); set_want(0); LeaveCriticalSection(&g_mu);
        ws_log("virtual LAN: not connected to the server's switch after 5 s: no network");
        return VE_NETDOWN;
    }
    EnterCriticalSection(&g_mu);
    ident_from_link(id);
    g_hello = 1;
    LeaveCriticalSection(&g_mu);
    char b[16];
    ws_log("virtual LAN: this game is %s/%d on the server's switch (%.1f ms to connect)",
           ipn(id->ip, b), __builtin_popcount(g_mask), (double)(now_us() - t0) / 1000.0);
    return 0;
}
int vlan_alive(void){
    if (!g_hello) return 0;
    if (!winsock_session_active()){
        EnterCriticalSection(&g_mu); set_want(1); LeaveCriticalSection(&g_mu);
        if (!wait_online()){
            ws_log("virtual LAN: not connected again after 5 s: no network");
            EnterCriticalSection(&g_mu); set_want(0); LeaveCriticalSection(&g_mu);
            return 0;
        }
    }
    EnterCriticalSection(&g_mu);
    int r = 1;
    if (!g_up || g_link_vip == g_vip) r = 1;
    else if (winsock_session_active()) r = 0;            /* sockets bound to the old address: no network for them */
    else {
        uint32_t old = g_vip;
        ident_from_link(&g_ws_id);
        char a[16], b[16];
        ws_log("virtual LAN: the switch gave this game a new address: %s (was %s)", ipn(H(g_vip), a), ipn(H(old), b));
    }
    LeaveCriticalSection(&g_mu);
    return r;
}
void vlan_session_end(void){ EnterCriticalSection(&g_mu); set_want(0); LeaveCriticalSection(&g_mu); }

int vlan_open(WsSock *s){
    EnterCriticalSection(&g_mu);
    int i = 1; while (i < NVS && g_vs[i].used) i++;
    if (i == NVS){ LeaveCriticalSection(&g_mu); return VE_MFILE; }
    VSock *v = &g_vs[i];
    memset(v, 0, sizeof *v);
    v->used = 1; v->kind = s->type; v->vfd = -1;
    if (s->type != 1){
        v->vfd = vfd_new(VF_DGRAM, v);
        if (v->vfd < 0){ v->used = 0; LeaveCriticalSection(&g_mu); return VE_MFILE; }
    }
    s->vid = (uint32_t)i; s->fd = v->vfd;
    LeaveCriticalSection(&g_mu);
    return 0;
}

int vlan_bind(WsSock *s, uint32_t ip, uint16_t port){
    EnterCriticalSection(&g_mu);
    VSock *v = vs_of(s->vid);
    int e = 0; char b[16];
    if (!v) e = VE_NOTSOCK;
    else if (ip && ip != own_n() && !is_loop_n(ip)) e = VE_ADDRNOTAVAIL;
    else if (v->bound) e = VE_INVAL;
    else {
        int tcp = v->kind == 1, reuse_all;
        if (!port){ port = ephemeral(tcp); if (!port) e = VE_NOBUFS; }
        else if (port_used(tcp, port, ip, v, &reuse_all) && !s->so_reuse){
            ws_log("virtual LAN: bind %s %s:%u -> WSAEADDRINUSE", tcp ? "tcp" : "udp", ipn(ip, b), port);
            e = VE_ADDRINUSE;
        }
        if (!e){
            v->bip = ip; v->bport = port; v->bound = 1; v->reuse = s->so_reuse;
            s->laddr = ip; s->lport = port;
            if (!tcp) ports_note();
            ws_log("virtual LAN: bind %s %s:%u", tcp ? "tcp" : v->kind == 3 ? "icmp" : "udp", ipn(ip, b), port);
        }
    }
    LeaveCriticalSection(&g_mu);
    return e;
}

int vlan_listen(WsSock *s, int backlog){
    EnterCriticalSection(&g_mu);
    VSock *v = vs_of(s->vid);
    int e = 0;
    if (!v || v->kind != 1) e = VE_INVAL;
    else {
        if (v->vfd < 0) v->vfd = vfd_new(VF_LISTEN, v);
        if (v->vfd < 0) e = VE_MFILE;
        else {
            v->listening = 1; v->backlog = backlog > 0 ? backlog : 1;
            s->fd = v->vfd;
            char b[16];
            ws_log("virtual LAN: listening on tcp %s:%u (backlog %d)", ipn(v->bip, b), v->bport, v->backlog);
        }
    }
    LeaveCriticalSection(&g_mu);
    return e;
}

int vlan_accept(WsSock *ls, WsSock *ns){
    EnterCriticalSection(&g_mu);
    VSock *l = vs_of(ls->vid);
    int e = 0;
    if (!l || !l->listening) e = VE_INVAL;
    else if (!l->acc_h) e = VE_WOULDBLOCK;
    else {
        VConn *k = l->acc_h;
        l->acc_h = k->acc_next; if (!l->acc_h) l->acc_t = 0;
        l->pending--; k->listener = 0; k->acc_next = 0;
        ns->fd = k->vfd; ns->raddr = k->rip; ns->rport = k->rport; ns->laddr = k->lip; ns->lport = k->lport;
        ns->conn_id = k->cid; ns->vid = 0;
    }
    LeaveCriticalSection(&g_mu);
    return e;
}

int vlan_connect(WsSock *s, uint32_t ip, uint16_t port){
    EnterCriticalSection(&g_mu);
    VSock *v = vs_of(s->vid);
    char b[16], b2[16];
    if (!v){ LeaveCriticalSection(&g_mu); return VE_NOTSOCK; }
    if (s->fd >= 0){ VConn *old = stream_of(s->fd); if (old) conn_free(old); s->fd = -1; }   /* an earlier attempt */
    if (!v->bound){ v->bip = 0; v->bport = ephemeral(1); v->bound = 1; }
    int loop = is_loop_n(ip), self = ip == own_n();
    uint32_t lip = loop ? H(0x7F000001u) : own_n();
    s->laddr = lip; s->lport = v->bport;
    uint64_t now = now_us();
    uint32_t cid; do cid = vs_random(); while (!cid);
    int e = 0;
    if (loop || self){
        VSock *l = find_listener(ip, port);
        s->conn_ready_us = now;
        if (!l || l->pending >= l->backlog + 4){ S.tcp_refused++; e = VE_CONNREFUSED; }
        else {
            VConn *a = conn_new(lip, v->bport, ip, port, cid, C_EST), *z = a ? conn_new(ip, port, lip, v->bport, cid, C_EST) : 0;
            if (!z){ if (a) conn_free(a); e = VE_NOBUFS; }
            else {
                a->local = z->local = 1; a->peer = z; z->peer = a;
                acc_push(l, z);
                s->fd = a->vfd; s->conn_id = cid;
                S.tcp_local++;
            }
        }
    } else if (!peer_present_h(H(ip)) || !online()){
        S.tcp_unreach++;
        ws_log("virtual LAN: tcp connect %s:%u -> no such host on the switch (WSAEHOSTUNREACH in 3 s)", ipn(ip, b), port);
        s->conn_ready_us = now + CONNECT_TMO; e = VE_HOSTUNREACH;
    } else {
        VConn *a = conn_new(lip, v->bport, ip, port, cid, C_SYN_SENT);
        if (!a) e = VE_NOBUFS;
        else if (!tx(NET_KIND_LAN_TCP, H(ip), v->bport, port, NET_LAN_TCP_SYN, 64, cid, 0, 0)){
            conn_free(a); S.tcp_timeout++; s->conn_ready_us = now + CONNECT_TMO; e = VE_TIMEDOUT;
        } else {
            a->deadline = now + CONNECT_TMO;
            s->fd = a->vfd; s->conn_id = cid;
            s->conn_ready_us = a->deadline;
            S.tcp_out++;
            ws_log("virtual LAN: tcp connect %s:%u -> %s:%u", ipn(lip, b), v->bport, ipn(ip, b2), port);
        }
    }
    LeaveCriticalSection(&g_mu);
    return e;
}

void vlan_conn_update(WsSock *s){
    if (s->fd < 0) return;
    EnterCriticalSection(&g_mu);
    VConn *k = stream_of(s->fd);
    if (k){
        uint64_t now = now_us();
        if (k->state == C_SYN_SENT && now >= k->deadline){
            k->state = C_FAILED; S.tcp_timeout++;
            char b[16]; ws_log("virtual LAN: tcp connect to %s:%u: no answer in 3 s (WSAETIMEDOUT)", ipn(k->rip, b), k->rport);
        }
        if (k->state == C_EST){ s->conn_result = 0; s->conn_ready_us = 0; }
        else if (k->state == C_REFUSED){ s->conn_result = VE_CONNREFUSED; s->conn_ready_us = 0; }
        else if (k->state == C_FAILED){ s->conn_result = VE_TIMEDOUT; s->conn_ready_us = 0; }
    }
    LeaveCriticalSection(&g_mu);
}

int vlan_sendto(WsSock *s, const uint8_t *buf, int len, uint32_t ip, uint16_t port, int *sent){
    EnterCriticalSection(&g_mu);
    VSock *v = vs_of(s->vid);
    *sent = len;
    if (!v){ LeaveCriticalSection(&g_mu); return VE_NOTSOCK; }
    uint32_t ip_h = H(ip);
    int loop = is_loop_n(ip), self = ip == own_n(), bc = bcast_h(ip_h);
    uint32_t src = loop ? H(0x7F000001u) : own_n();
    char b[16];
    if (v->kind == 3){                                            /* ICMP (ProtoPing) */
        S.icmp_out++;
        if (loop || self){
            if (len >= 8 && buf[0] == 8){
                uint8_t rep[2048]; int n = len < (int)sizeof rep ? len : (int)sizeof rep;
                memcpy(rep, buf, (size_t)n); rep[0] = 0; rep[1] = 0; icmp_cksum(rep, n);
                icmp_deliver(ip, rep, n);
            }
        } else if (len <= (int)UDP_MAX && in_subnet_h(ip_h) && !bc)
            tx(NET_KIND_LAN_ICMP, ip_h, 0, 0, 0, (uint8_t)s->ip_ttl, 0, buf, (uint32_t)len);
        LeaveCriticalSection(&g_mu); bump();
        return 0;
    }
    if (len > (int)UDP_MAX){
        S.udp_oversize++;
        if (S.udp_oversize <= 5) ws_log("virtual LAN: a %d-byte datagram to %s:%u is larger than %u bytes: dropped",
                                        len, ipn(ip, b), port, UDP_MAX);
        LeaveCriticalSection(&g_mu);
        return 0;
    }
    S.udp_out++;
    v->has_peer = s->has_peer; v->raddr = s->raddr; v->rport = s->rport;
    if (loop || self){
        S.udp_local++;
        if (!udp_deliver(src, v->bport, ip, port, buf, len, 0) && self) dg_push(v, ip, port, 1, 0, 0);   /* port unreachable */
    } else if (bc){
        S.udp_bcast_out++;
        udp_deliver(own_n(), v->bport, 0xFFFFFFFFu, port, buf, len, 1);   /* our own copy, as on Windows */
        tx(NET_KIND_LAN_UDP, NET_BROADCAST, v->bport, port, NET_LAN_UDP_DATA, (uint8_t)s->ip_ttl, 0, buf, (uint32_t)len);
    } else if (in_subnet_h(ip_h)){
        if (tx(NET_KIND_LAN_UDP, ip_h, v->bport, port, NET_LAN_UDP_DATA, (uint8_t)s->ip_ttl, 0, buf, (uint32_t)len)) sent_note(v, ip, port);
    } else {
        S.udp_outside++;
        static uint32_t said[8]; static int ns;
        int k; for (k = 0; k < ns && said[k] != ip; k++) {}
        if (k == ns && ns < 8){ said[ns++] = ip; ws_log("virtual LAN: datagram to %s:%u dropped: outside the virtual LAN", ipn(ip, b), port); }
    }
    LeaveCriticalSection(&g_mu);
    bump();
    return 0;
}

int vlan_recvfrom(WsSock *s, uint8_t *buf, int len, int peek, uint32_t *ip, uint16_t *port, int *got){
    EnterCriticalSection(&g_mu);
    VSock *v = vs_of(s->vid);
    int e = VE_WOULDBLOCK;
    *got = 0;
    if (v){ v->has_peer = s->has_peer; v->raddr = s->raddr; v->rport = s->rport; }
    while (v && v->qh){
        Dgram *d = v->qh;
        if (d->flags & 1){                                       /* port unreachable for this socket */
            int ours = s->has_peer && d->ip == s->raddr && d->port == s->rport;
            v->qh = d->next; if (!v->qh) v->qt = 0; v->qn--; vs_free(d);
            if (ours){ e = VE_CONNRESET; break; }
            continue;
        }
        *ip = d->ip; *port = d->port;
        int n = d->len;
        if (v->kind == 3){
            /* Windows raw sockets deliver the IPv4 header in front of the ICMP message */
            uint8_t ih[20]; memset(ih, 0, sizeof ih);
            int tot = n + 20;
            ih[0] = 0x45; ih[2] = (uint8_t)(tot >> 8); ih[3] = (uint8_t)tot; ih[8] = 64; ih[9] = 1;
            memcpy(ih + 12, &d->ip, 4); uint32_t me = own_n(); memcpy(ih + 16, &me, 4);
            uint32_t sum = 0; for (int i = 0; i < 20; i += 2) sum += (uint32_t)(ih[i] << 8 | ih[i + 1]);
            while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
            sum = ~sum & 0xFFFF; ih[10] = (uint8_t)(sum >> 8); ih[11] = (uint8_t)sum;
            int c1 = len < 20 ? len : 20; memcpy(buf, ih, (size_t)c1);
            int c2 = len - c1 < n ? len - c1 : n; if (c2 > 0) memcpy(buf + c1, d->data, (size_t)c2);
            if (tot > len){ *got = len; e = VE_MSGSIZE; } else { *got = tot; e = 0; }
        } else if (n > len){ memcpy(buf, d->data, (size_t)len); *got = len; e = VE_MSGSIZE; }
        else { memcpy(buf, d->data, (size_t)n); *got = n; e = 0; }
        if (!peek){ v->qh = d->next; if (!v->qh) v->qt = 0; v->qn--; v->qbytes -= (uint32_t)d->len; vs_free(d); }
        break;
    }
    LeaveCriticalSection(&g_mu);
    return e;
}

void vlan_close(WsSock *s, int abortive){
    EnterCriticalSection(&g_mu);
    VConn *k = s->fd >= 0 ? stream_of(s->fd) : 0;
    if (k && abortive) send_rst(k);
    VSock *v = vs_of(s->vid);
    if (v){
        if (v->listening){
            while (v->acc_h){
                VConn *q = v->acc_h; v->acc_h = q->acc_next;
                send_rst(q); conn_free(q);
            }
            v->acc_t = 0; v->pending = 0;
        }
        dg_free_all(v);
        int i = vfd_ix(v->vfd);
        if (i >= 0 && g_vf[i].p == v){ g_vf[i].type = VF_DEAD; g_vf[i].p = 0; }
        v->used = 0;
        ports_note();
    }
    LeaveCriticalSection(&g_mu);
    bump();
}

int vlan_conn_was_reset(WsSock *s){
    EnterCriticalSection(&g_mu);
    VConn *k = s->fd >= 0 ? stream_of(s->fd) : 0;
    int r = k && k->rx_rst;
    LeaveCriticalSection(&g_mu);
    return r;
}

void vlan_fionread(WsSock *s, uint32_t *out){
    EnterCriticalSection(&g_mu);
    *out = 0;
    VConn *k = stream_of(s->fd);
    if (k) *out = k->rlen;
    else { VSock *v = vs_of(s->vid);
           if (v && v->qh) *out = (uint32_t)v->qh->len + (v->kind == 3 ? 20u : 0u); }
    LeaveCriticalSection(&g_mu);
}

/* ==================================================================================== */
/* virtual fds                                                                          */
/* ==================================================================================== */
static short revents_of(int fd, short ev){
    int i = vfd_ix(fd);
    if (i < 0) return VP_NVAL;
    short r = 0;
    switch (g_vf[i].type){
    case VF_DGRAM: { VSock *v = g_vf[i].p; if (v->qh) r |= VP_IN; r |= VP_OUT; break; }
    case VF_LISTEN: { VSock *v = g_vf[i].p; if (v->acc_h) r |= VP_IN; break; }
    case VF_STREAM: {
        VConn *k = g_vf[i].p;
        if (k->state == C_SYN_SENT) return 0;
        if (k->state != C_EST) return VP_ERR;                    /* refused / no answer */
        /* events 0: the caller waits for a connect's outcome: the connection is complete */
        if (!ev) return VP_OUT;
        if (ev & VP_IN) k->polled_rd = now_us();
        if (k->rlen || k->rx_fin || k->rx_rst) r |= VP_IN;
        if (k->rx_rst) r |= VP_HUP;
        else if (k->local ? k->peer && k->peer->rlen < LOCAL_CAP : online() && tx_free() > TX_RESERVE) r |= VP_OUT;
        else if (k->local && !k->peer) r |= VP_HUP;
        break; }
    case VF_DEAD: return VP_HUP;
    default: return VP_NVAL;
    }
    return (short)(r & (ev | VP_HUP | VP_ERR | VP_NVAL));
}

int vfd_poll(vpollfd *p, int n, int ms){
    uint64_t dl = now_us() + (uint64_t)(ms > 0 ? ms : 0) * 1000ull;
    EnterCriticalSection(&g_mu);
    for (;;){
        int ready = 0;
        for (int i = 0; i < n; i++){
            p[i].revents = p[i].fd >= 0 ? revents_of(p[i].fd, p[i].events) : 0;
            if (p[i].revents) ready++;
        }
        if (ready || ms == 0){ LeaveCriticalSection(&g_mu); return ready; }
        uint64_t now = now_us();
        if (ms > 0 && now >= dl){ LeaveCriticalSection(&g_mu); return 0; }
        uint64_t w = ms < 0 ? 50000ull : dl - now;
        if (w > 50000ull) w = 50000ull;                          /* the TX ring frees slots without a word */
        SleepConditionVariableCS(&g_cv, &g_mu, (DWORD)((w + 999) / 1000));
    }
}

int vfd_send(int fd, const void *buf, int len){
    EnterCriticalSection(&g_mu);
    VConn *k = stream_of(fd);
    int r = -1, err = 0;
    if (!k) err = VE_NOTSOCK;
    else if (k->state != C_EST) err = VE_NOTCONN;
    else if (k->rx_rst || k->rst_sent || k->tx_fin) err = VE_PIPE;
    else if (k->local){
        VConn *pk = k->peer;
        if (!pk || pk->rx_rst) err = VE_PIPE;
        else {
            uint32_t room = pk->rlen < LOCAL_CAP ? LOCAL_CAP - pk->rlen : 0;
            uint32_t m = (uint32_t)len < room ? (uint32_t)len : room;
            if (!m && len) err = VE_WOULDBLOCK;
            else if (!rb_put(pk, buf, m, LOCAL_CAP)) err = VE_NOBUFS;
            else { r = (int)m; k->bytes_out += m; }
        }
    } else {
        uint32_t fr = online() ? tx_free() : 0, freeslots = fr > TX_RESERVE ? fr - TX_RESERVE : 0;
        int off = 0;
        while (off < len && freeslots){
            int m = len - off < (int)TCP_CHUNK ? len - off : (int)TCP_CHUNK;
            if (!tx(NET_KIND_LAN_TCP, H(k->rip), k->lport, k->rport, NET_LAN_TCP_DATA, 64, k->cid, (const uint8_t *)buf + off, (uint32_t)m)) break;
            off += m; freeslots--;
        }
        if (!off && len) err = VE_WOULDBLOCK;
        else { r = off; S.tcp_bytes_out += (uint64_t)off; k->bytes_out += (uint32_t)off; }
    }
    LeaveCriticalSection(&g_mu);
    if (r > 0) bump();
    return r < 0 ? -err : r;
}

int vfd_recv(int fd, void *buf, int len, int peek){
    EnterCriticalSection(&g_mu);
    VConn *k = stream_of(fd);
    int r = -1, err = 0, freed = 0;
    if (k) k->recv_call = now_us();
    if (!k) err = VE_NOTSOCK;
    else if (k->rlen){
        k->read_at = k->recv_call; k->stall_said = 0;
        uint32_t m = (uint32_t)len < k->rlen ? (uint32_t)len : k->rlen;
        memcpy(buf, k->rb + k->roff, m);
        if (!peek){ k->roff += m; k->rlen -= m; if (!k->rlen) k->roff = 0; freed = k->local; }
        r = (int)m;
    } else if (k->rx_fin || k->rx_rst || (k->local && !k->peer)) r = 0;
    else if (k->state != C_EST) err = VE_NOTCONN;
    else err = VE_WOULDBLOCK;
    LeaveCriticalSection(&g_mu);
    if (freed) bump();
    return r < 0 ? -err : r;
}

int vfd_shutdown(int fd, int how){
    EnterCriticalSection(&g_mu);
    VConn *k = stream_of(fd);
    if (k && (how == 1 || how == 2)) send_fin(k);
    LeaveCriticalSection(&g_mu);
    bump();                                                      /* closesocket under a waiter: wake it */
    return 0;
}

int vfd_close(int fd){
    EnterCriticalSection(&g_mu);
    int i = vfd_ix(fd);
    if (i >= 0){
        if (g_vf[i].type == VF_STREAM){
            VConn *k = g_vf[i].p;
            if (k->listener){
                VSock *l = k->listener; VConn **pp = &l->acc_h;
                while (*pp && *pp != k) pp = &(*pp)->acc_next;
                if (*pp){ *pp = k->acc_next; l->pending--; l->acc_t = 0; for (VConn *q = l->acc_h; q; q = q->acc_next) l->acc_t = q; }
            }
            if (k->state == C_SYN_SENT) send_rst(k);
            else send_fin(k);
            conn_free(k);
        } else if (g_vf[i].type == VF_DGRAM || g_vf[i].type == VF_LISTEN){
            VSock *v = g_vf[i].p;
            if (v){ dg_free_all(v); v->used = 0; ports_note(); }
            g_vf[i].type = VF_FREE; g_vf[i].p = 0;
        } else { g_vf[i].type = VF_FREE; g_vf[i].p = 0; }
    }
    LeaveCriticalSection(&g_mu);
    bump();
    return i >= 0 ? 0 : -1;
}
