/* vsock_int.h -- between the virtual IP stack (vsock.c, the port of runtime/web/net_lan.c) and the Winsock surface
 * (vsock_ws2.c, the port of runtime/winsock.c's vlan paths).  The split and the names are the browser's, so each half
 * can be read next to its original: a socket is a WsSock (the Winsock view) and a virtual fd (VFD_BASE + n) or
 * socket id in the stack; the stack's vfd_* calls are what winsock.c calls poll/send/recv/shutdown/close there.
 *
 * Differences from the browser, all from running natively instead of in the emulator:
 *   - the game's threads call in concurrently: vsock_ws2.c holds its own lock (g_api, the emulator's "guest lock")
 *     through every call and lets it go while it waits (guest_wait_begin/end), the stack's lock (g_mu) is below it;
 *   - waiting is a condition variable that every change wakes, not memory.atomic.wait32 on an event word;
 *   - the stack's errors are WSA codes (vfd_send/recv return -code), not errno. */
#ifndef NFSU2_VSOCK_INT_H
#define NFSU2_VSOCK_INT_H
#include <stdint.h>
#include <stddef.h>
#include "vsock.h"

/* Winsock error codes (winerror.h) */
#define VE_INTR          10004
#define VE_BADF          10009
#define VE_ACCES         10013
#define VE_FAULT         10014
#define VE_INVAL         10022
#define VE_MFILE         10024
#define VE_WOULDBLOCK    10035
#define VE_ALREADY       10037
#define VE_NOTSOCK       10038
#define VE_DESTADDRREQ   10039
#define VE_MSGSIZE       10040
#define VE_PROTOTYPE     10041
#define VE_NOPROTOOPT    10042
#define VE_PROTONOSUPPORT 10043
#define VE_SOCKTNOSUPPORT 10044
#define VE_OPNOTSUPP     10045
#define VE_AFNOSUPPORT   10047
#define VE_ADDRINUSE     10048
#define VE_ADDRNOTAVAIL  10049
#define VE_NETDOWN       10050
#define VE_NETUNREACH    10051
#define VE_CONNABORTED   10053
#define VE_CONNRESET     10054
#define VE_NOBUFS        10055
#define VE_ISCONN        10056
#define VE_NOTCONN       10057
#define VE_SHUTDOWN      10058
#define VE_TIMEDOUT      10060
#define VE_CONNREFUSED   10061
#define VE_HOSTUNREACH   10065
#define VE_SYSNOTREADY   10091
#define VE_VERNOTSUPPORTED 10092
#define VE_NOTINITIALISED 10093
#define VE_HOST_NOT_FOUND 11001
/* vfd_send / vfd_recv: a stream whose other end reset it (the browser's EPIPE) */
#define VE_PIPE          (-32)

/* poll events of the virtual fds (only these files see them) */
#define VP_IN    0x01
#define VP_OUT   0x04
#define VP_ERR   0x08
#define VP_HUP   0x10
#define VP_NVAL  0x20
typedef struct { int fd; short events, revents; } vpollfd;
#define VFD_BASE 0x40000000

enum { ST_NEW = 0, ST_BOUND, ST_LISTEN, ST_CONNECTING, ST_CONNECTED, ST_FAILED };

typedef struct WsSock {
    int used;
    int fd;                 /* the stack's virtual fd (-1: none yet, e.g. a TCP socket before connect) */
    int type;               /* 1 SOCK_STREAM, 2 SOCK_DGRAM, 3 SOCK_RAW (ICMP only) */
    int proto;
    int nb;                 /* FIONBIO */
    int state;
    int bound;              /* explicit or implicit bind done */
    int has_peer;           /* UDP: connect() set a default destination */
    int shut_rd, shut_wr;
    int so_broadcast, so_reuse, so_keepalive, so_oobinline, so_dontroute;
    int linger_on, linger_s;
    uint32_t rcvtimeo, sndtimeo;   /* ms, 0 = none */
    int sndbuf, rcvbuf;            /* reported values */
    int ip_ttl;
    int pending_err;        /* SO_ERROR (WSA code), cleared when read */
    int conn_result;        /* connect outcome once known: 0 ok, else WSA code */
    uint64_t conn_ready_us; /* connect completes at this time */
    uint32_t laddr, raddr;  /* network byte order */
    uint16_t lport, rport;  /* host byte order */
    int udp_reset;          /* connected UDP: next recv reports WSAECONNRESET */
    uint32_t vid;           /* the stack's socket id */
    uint32_t conn_id;       /* TCP connection id */
    int waiters;            /* blocking calls in progress (g_api released while they wait) */
    volatile long closing;
} WsSock;

/* Identity on the virtual LAN */
typedef struct {
    uint32_t ip;            /* network order; 0 = none */
    uint32_t mask;          /* network order */
    uint8_t  mac[6];
    char     name[64];      /* gethostname */
} NetIdent;

/* ---- vsock_ws2.c services for the stack ---- */
uint64_t ws_now_us(void);
void ws_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void guest_wait_begin(void);   /* g_api released: other game threads may call in while this one waits */
void guest_wait_end(void);
extern NetIdent g_ws_id;
extern const vs_transport *g_vs_tr;

/* ---- vsock.c: the stack (net_lan.c's vlan_* and vfd_*) ---- */
int  vlan_hello(NetIdent *id);                 /* the first WSAStartup: waits up to 5 s for the link; 0 or WSA error */
int  vlan_alive(void);                         /* every later WSAStartup: 1 usable */
void vlan_session_end(void);                   /* the last WSACleanup */
int  vlan_open(WsSock *s);
int  vlan_bind(WsSock *s, uint32_t ip, uint16_t port);
int  vlan_connect(WsSock *s, uint32_t ip, uint16_t port);
void vlan_conn_update(WsSock *s);
int  vlan_listen(WsSock *s, int backlog);
int  vlan_accept(WsSock *ls, WsSock *ns);
int  vlan_sendto(WsSock *s, const uint8_t *buf, int len, uint32_t ip, uint16_t port, int *sent);
int  vlan_recvfrom(WsSock *s, uint8_t *buf, int len, int peek, uint32_t *ip, uint16_t *port, int *got);
void vlan_close(WsSock *s, int abortive);
int  vlan_conn_was_reset(WsSock *s);
void vlan_fionread(WsSock *s, uint32_t *v);
int  vfd_poll(vpollfd *p, int n, int ms);      /* ms 0: no wait, < 0: forever; call with g_api released if ms != 0 */
int  vfd_send(int fd, const void *buf, int len);   /* bytes, or -WSA code (VE_WOULDBLOCK, VE_PIPE, VE_NOTCONN) */
int  vfd_recv(int fd, void *buf, int len, int peek);
int  vfd_shutdown(int fd, int how);            /* 1 SD_SEND, 2 SD_BOTH: FIN */
int  vfd_close(int fd);
int  winsock_session_active(void);             /* vsock_ws2.c */
void vs_calls_tick(void);                      /* vsock_ws2.c: the call counters' report when due (the writer thread) */
void vs_calls_report(const char *why);         /* ... now */

/* ---- freestanding helpers (vsock.c) ---- */
void *vs_alloc(size_t n);                      /* zeroed */
void *vs_realloc(void *p, size_t n);
void  vs_free(void *p);
uint32_t vs_random(void);
#endif
