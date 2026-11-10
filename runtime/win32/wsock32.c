// WSOCK32 (Winsock 1.1) on BSD sockets. CoDMP imports these by ordinal.
//
// SOCKET handles are the host fds. Windows sockaddr has a 16-bit family where BSD has sa_len + sa_family, so
// addresses are converted at every boundary; only AF_INET is supported (IPX -> WSAEAFNOSUPPORT, the engine skips it).
// Winsock error codes are WSABASEERR + the BSD errno for the socket errors, which is what macOS errno values are.
//
// COD_HOSTS="name=a.b.c.d,name2=..." overrides DNS for gethostbyname (e.g. to point codmaster.activision.com at a
// community master server).
#include "../rt.h"
#include "../platform.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define W_INVALID_SOCKET 0xffffffffu
#define W_SOCKET_ERROR   0xffffffffu
#define W_AF_INET        2
#define WSAEWOULDBLOCK   10035
#define WSAEAFNOSUPPORT  10047
#define WSAEOPNOTSUPP    10045
#define WSAEINVAL        10022
#define WSAHOST_NOT_FOUND 11001

static uint32_t wsa_err(int e) {
    if (e == EAGAIN || e == EINPROGRESS) return WSAEWOULDBLOCK;
    if (e == EOPNOTSUPP || e == ENOTSUP) return WSAEOPNOTSUPP;
    return 10000u + (uint32_t)e;
}
static uint32_t fail(void) { rt_set_last_error(wsa_err(errno)); return W_SOCKET_ERROR; }

// guest sockaddr_in {u16 family, u16 port(be), u32 addr(be), u8 zero[8]} <-> host
static int to_host(uint32_t gp, uint32_t glen, struct sockaddr_in *h) {
    if (!gp || glen < 8) { rt_set_last_error(WSAEINVAL); return -1; }
    const uint8_t *g = gptr(gp);
    uint16_t fam; memcpy(&fam, g, 2);
    if (fam != W_AF_INET) { rt_set_last_error(WSAEAFNOSUPPORT); return -1; }
    memset(h, 0, sizeof *h);
    h->sin_len = sizeof *h;
    h->sin_family = AF_INET;
    memcpy(&h->sin_port, g + 2, 2);
    memcpy(&h->sin_addr, g + 4, 4);
    return 0;
}
static void to_guest(const struct sockaddr_in *h, uint32_t gp, uint32_t glenp) {
    if (!gp) return;
    uint8_t *g = gptr(gp);
    uint16_t fam = W_AF_INET;
    memset(g, 0, 16);
    memcpy(g, &fam, 2);
    memcpy(g + 2, &h->sin_port, 2);
    memcpy(g + 4, &h->sin_addr, 4);
    if (glenp) g_wr32(glenp, 16);
}

static int g_trace_net = -1;
#define NTRACE(...) do { if (g_trace_net < 0) g_trace_net = getenv("COD_NETTRACE") != NULL; \
                         if (g_trace_net) LOG("net: " __VA_ARGS__); } while (0)

// ------------------------------------------------------------------ WSAAsyncSelect
// libwww (the cl_wwwDownload HTTP client) registers every socket with WSAAsyncSelect and is driven entirely by the
// resulting window messages, dispatched to its hidden "AsyncWindowClass" window by the engine's message loop.
// Emulated with poll() whenever the guest polls its message queue (user32 PeekMessage/GetMessage). Winsock's
// re-enabling rules are kept so each condition is posted once until the guest consumes it: FD_READ re-arms on
// recv, FD_WRITE on a send that would block, FD_ACCEPT on accept; FD_CONNECT and FD_CLOSE are posted once.
#define W_FD_READ    0x01
#define W_FD_WRITE   0x02
#define W_FD_OOB     0x04
#define W_FD_ACCEPT  0x08
#define W_FD_CONNECT 0x10
#define W_FD_CLOSE   0x20
typedef struct { int fd; uint32_t hwnd, msg, mask; uint8_t rd, wr, acc, connecting, closed; } Async;
static Async g_async[32];
static int g_nasync;
static Async *async_get(int fd) {
    for (int i = 0; i < g_nasync; i++) if (g_async[i].fd == fd) return &g_async[i];
    return NULL;
}
static void async_drop(int fd) {
    for (int i = 0; i < g_nasync; i++) if (g_async[i].fd == fd) { g_async[i] = g_async[--g_nasync]; return; }
}
static void async_post(Async *a, uint32_t ev, int err) {
    NTRACE("async %d: event %02x err %d", a->fd, ev, err);
    plat_post_msg(a->hwnd, a->msg, (uint32_t)a->fd, ev | (err ? wsa_err(err) << 16 : 0));
}
void wsa_async_poll(void) {
    for (int i = 0; i < g_nasync; i++) {
        Async *a = &g_async[i];
        struct pollfd pf = {.fd = a->fd, .events = POLLIN | POLLOUT};
        if (poll(&pf, 1, 0) <= 0) continue;
        int err = 0; socklen_t el = sizeof err;
        if (a->connecting) {
            if (!(pf.revents & (POLLOUT | POLLERR | POLLHUP))) continue;
            getsockopt(a->fd, SOL_SOCKET, SO_ERROR, &err, &el);
            a->connecting = 0;
            async_post(a, W_FD_CONNECT, err);
            if (err) { a->closed = 1; continue; }
        }
        if (pf.revents & (POLLIN | POLLHUP | POLLERR)) {
            if (a->mask & W_FD_ACCEPT) {
                if (a->acc && (pf.revents & POLLIN)) { a->acc = 0; async_post(a, W_FD_ACCEPT, 0); }
            } else if (a->rd && !a->closed) {
                char b;
                ssize_t n = recv(a->fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
                if (n > 0) {
                    if (a->mask & W_FD_READ) { a->rd = 0; async_post(a, W_FD_READ, 0); }
                } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {   // EOF / reset: all data consumed
                    int e = n == 0 ? 0 : errno;
                    a->closed = 1;
                    if (a->mask & W_FD_CLOSE) async_post(a, W_FD_CLOSE, e);
                    else if (a->mask & W_FD_READ) { a->rd = 0; async_post(a, W_FD_READ, e); }
                }
            }
        }
        if ((pf.revents & POLLOUT) && a->wr && (a->mask & W_FD_WRITE) && !a->closed) {
            a->wr = 0;
            async_post(a, W_FD_WRITE, 0);
        }
    }
}
static void async_rearm(int fd, int rd, int wr, int acc) {
    if (!g_nasync) return;
    Async *a = async_get(fd);
    if (!a) return;
    if (rd) a->rd = 1;
    if (wr) a->wr = 1;
    if (acc) a->acc = 1;
}

// ------------------------------------------------------------------ setup
SHIM(WSOCK32, ord115) {   // int WSAStartup(WORD ver, LPWSADATA)
    uint8_t *d = ARG(1) ? gptr(ARG(1)) : NULL;
    if (d) {
        memset(d, 0, 400);
        uint16_t v = 0x0101, hv = 0x0202;
        memcpy(d, &v, 2); memcpy(d + 2, &hv, 2);
        strcpy((char *)d + 4, "BSD sockets (cod1native)");
        strcpy((char *)d + 4 + 257, "Running");
        uint16_t maxs = 1024, maxdg = 65467;
        memcpy(d + 4 + 257 + 129 + 0, &maxs, 2);   // iMaxSockets @390 (WSADATA is packed to 2)
        memcpy(d + 4 + 257 + 129 + 2, &maxdg, 2);  // iMaxUdpDg @392
    }
    NTRACE("WSAStartup(%04x)", ARG(0) & 0xffff);
    RET(0, 2);
}
SHIM(WSOCK32, ord116) { RET(0, 0); }                              // WSACleanup
SHIM(WSOCK32, ord111) { RET(rt_last_error(), 0); }                // WSAGetLastError

// ------------------------------------------------------------------ sockets
SHIM(WSOCK32, ord23) {    // SOCKET socket(af, type, protocol)
    int af = (int)ARG(0), type = (int)ARG(1), proto = (int)ARG(2);
    if (af != W_AF_INET) { NTRACE("socket(af=%d) unsupported", af); rt_set_last_error(WSAEAFNOSUPPORT); RET(W_INVALID_SOCKET, 3); }
    int s = socket(AF_INET, type, proto);
    if (s < 0) RET(fail(), 3);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    NTRACE("socket(type=%d, proto=%d) -> %d", type, proto, s);
    RET((uint32_t)s, 3);
}
SHIM(WSOCK32, ord3) {     // closesocket
    NTRACE("closesocket(%d)", (int)ARG(0));
    async_drop((int)ARG(0));
    RET(close((int)ARG(0)) < 0 ? fail() : 0, 1);
}
SHIM(WSOCK32, ord2) {     // bind(s, addr, len)
    struct sockaddr_in h;
    if (to_host(ARG(1), ARG(2), &h) < 0) RET(W_SOCKET_ERROR, 3);
    int r = bind((int)ARG(0), (struct sockaddr *)&h, sizeof h);
    NTRACE("bind(%d, %s:%d) -> %d%s", (int)ARG(0), inet_ntoa(h.sin_addr), ntohs(h.sin_port), r, r < 0 ? strerror(errno) : "");
    RET(r < 0 ? fail() : 0, 3);
}
SHIM(WSOCK32, ord4) {     // connect(s, addr, len)
    struct sockaddr_in h;
    if (to_host(ARG(1), ARG(2), &h) < 0) RET(W_SOCKET_ERROR, 3);
    int r = connect((int)ARG(0), (struct sockaddr *)&h, sizeof h);
    NTRACE("connect(%d, %s:%d) -> %d", (int)ARG(0), inet_ntoa(h.sin_addr), ntohs(h.sin_port), r < 0 ? errno : 0);
    RET(r < 0 ? fail() : 0, 3);
}
SHIM(WSOCK32, ord13) { RET(listen((int)ARG(0), (int)ARG(1)) < 0 ? fail() : 0, 2); }   // listen
SHIM(WSOCK32, ord1) {     // accept(s, addr*, len*)
    struct sockaddr_in h; socklen_t hl = sizeof h;
    int r = accept((int)ARG(0), (struct sockaddr *)&h, &hl);
    async_rearm((int)ARG(0), 0, 0, 1);
    if (r < 0) RET(fail(), 3);
    int one = 1;
    setsockopt(r, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    to_guest(&h, ARG(1), ARG(2));
    RET((uint32_t)r, 3);
}
static void name_of(Cpu *cpu, int peer) {
    struct sockaddr_in h; socklen_t hl = sizeof h;
    int r = peer ? getpeername((int)ARG(0), (struct sockaddr *)&h, &hl) : getsockname((int)ARG(0), (struct sockaddr *)&h, &hl);
    if (r < 0) RET(fail(), 3);
    to_guest(&h, ARG(1), ARG(2));
    RET(0, 3);
}
SHIM(WSOCK32, ord5) { name_of(cpu, 1); }   // getpeername
SHIM(WSOCK32, ord6) { name_of(cpu, 0); }   // getsockname

// SOL_SOCKET (0xffff) and the SO_*/TCP_NODELAY values match between Winsock and BSD. Anything else is accepted
// and ignored (IP_* multicast/TOS numbering differs, and the engine does not depend on them).
static int host_level(uint32_t level) { return level == 0xffff ? SOL_SOCKET : level == 6 ? IPPROTO_TCP : -1; }
SHIM(WSOCK32, ord21) {    // setsockopt(s, level, name, val, len)
    int lv = host_level(ARG(1)), name = (int)ARG(2);
    NTRACE("setsockopt(%d, %x, %x)", (int)ARG(0), ARG(1), name);
    if (lv < 0 || name == 0x80 /*SO_LINGER*/ || name == (int)0xff7f /*SO_DONTLINGER*/) RET(0, 5);
    int v = ARG(4) >= 4 ? (int)g_rd32(ARG(3)) : (ARG(4) ? *(uint8_t *)gptr(ARG(3)) : 0);
    RET(setsockopt((int)ARG(0), lv, name, &v, sizeof v) < 0 ? fail() : 0, 5);
}
SHIM(WSOCK32, ord7) {     // getsockopt(s, level, name, val, len*)
    int lv = host_level(ARG(1)), v = 0;
    socklen_t l = sizeof v;
    if (lv >= 0 && getsockopt((int)ARG(0), lv, (int)ARG(2), &v, &l) < 0) RET(fail(), 5);
    if (ARG(3)) g_wr32(ARG(3), (uint32_t)v);
    if (ARG(4)) g_wr32(ARG(4), 4);
    RET(0, 5);
}
SHIM(WSOCK32, ord12) {    // ioctlsocket(s, cmd, u_long *arg)
    int s = (int)ARG(0);
    uint32_t cmd = ARG(1);
    if (cmd == 0x8004667eu) {          // FIONBIO
        int fl = fcntl(s, F_GETFL, 0);
        fl = g_rd32(ARG(2)) ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
        RET(fcntl(s, F_SETFL, fl) < 0 ? fail() : 0, 3);
    }
    if (cmd == 0x4004667fu) {          // FIONREAD
        int n = 0;
        if (ioctl(s, FIONREAD, &n) < 0) RET(fail(), 3);
        g_wr32(ARG(2), (uint32_t)n);
        RET(0, 3);
    }
    LOG("ioctlsocket(%d, %08x) unsupported", s, cmd);
    rt_set_last_error(WSAEINVAL);
    RET(W_SOCKET_ERROR, 3);
}

// ------------------------------------------------------------------ data
SHIM(WSOCK32, ord16) {    // recv(s, buf, len, flags)
    ssize_t r = recv((int)ARG(0), gptr(ARG(1)), ARG(2), (int)ARG(3));
    async_rearm((int)ARG(0), 1, 0, 0);
    RET(r < 0 ? fail() : (uint32_t)r, 4);
}
SHIM(WSOCK32, ord19) {    // send(s, buf, len, flags)
    ssize_t r = send((int)ARG(0), gptr(ARG(1)), ARG(2), (int)ARG(3));
    if (r < 0 && (errno == EAGAIN || errno == ENOBUFS)) async_rearm((int)ARG(0), 0, 1, 0);
    RET(r < 0 ? fail() : (uint32_t)r, 4);
}
SHIM(WSOCK32, ord17) {    // recvfrom(s, buf, len, flags, from*, fromlen*)
    struct sockaddr_in h; socklen_t hl = sizeof h;
    ssize_t r = recvfrom((int)ARG(0), gptr(ARG(1)), ARG(2), (int)ARG(3), (struct sockaddr *)&h, &hl);
    if (r < 0) RET(fail(), 6);
    if (h.sin_family == AF_INET) to_guest(&h, ARG(4), ARG(5));
    RET((uint32_t)r, 6);
}
SHIM(WSOCK32, ord20) {    // sendto(s, buf, len, flags, to, tolen)
    struct sockaddr_in h;
    if (to_host(ARG(4), ARG(5), &h) < 0) RET(W_SOCKET_ERROR, 6);
    ssize_t r = sendto((int)ARG(0), gptr(ARG(1)), ARG(2), (int)ARG(3), (struct sockaddr *)&h, sizeof h);
    if (r < 0) NTRACE("sendto(%s:%d) failed: %s", inet_ntoa(h.sin_addr), ntohs(h.sin_port), strerror(errno));
    RET(r < 0 ? fail() : (uint32_t)r, 6);
}

// ------------------------------------------------------------------ byte order / names
SHIM(WSOCK32, ord9) { RET(htons((uint16_t)ARG(0)), 1); }    // htons
SHIM(WSOCK32, ord14) { RET(ntohl(ARG(0)), 1); }             // ntohl
SHIM(WSOCK32, ord10) {    // inet_addr(const char *)
    struct in_addr a;
    RET(ARG(0) && inet_aton(ARGP(0), &a) ? a.s_addr : 0xffffffffu, 1);
}
SHIM(WSOCK32, ord57) {    // gethostname(buf, len)
    char n[256];
    if (gethostname(n, sizeof n) < 0) strcpy(n, "localhost");
    char *dot = strchr(n, '.');
    if (dot && strstr(n, ".local")) *dot = 0;
    if (strlen(n) + 1 > ARG(1)) { rt_set_last_error(10014); RET(W_SOCKET_ERROR, 2); }
    strcpy(ARGP(0), n);
    RET(0, 2);
}

// One guest hostent per thread is what Winsock guarantees; the engine copies the result immediately.
// layout: hostent{name,aliases,u16 type,u16 len,addr_list} @0, aliases[1] @16, addr_list[9] @20, addrs @56, name @92
static uint32_t g_hostent;
static uint32_t make_hostent(const char *name, const uint32_t *addrs, int n) {
    if (!g_hostent) g_hostent = galloc(512);
    uint32_t b = g_hostent;
    uint8_t *p = gptr(b);
    memset(p, 0, 512);
    if (n > 8) n = 8;
    snprintf((char *)p + 92, 512 - 92, "%s", name);
    g_wr32(b + 0, b + 92);
    g_wr32(b + 4, b + 16);
    uint16_t t = W_AF_INET, l = 4;
    memcpy(p + 8, &t, 2); memcpy(p + 10, &l, 2);
    g_wr32(b + 12, b + 20);
    for (int i = 0; i < n; i++) { g_wr32(b + 20 + 4 * i, b + 56 + 4 * i); g_wr32(b + 56 + 4 * i, addrs[i]); }
    return b;
}
static int host_override(const char *name, uint32_t *addr) {
    const char *e = getenv("COD_HOSTS");
    if (!e) return 0;
    size_t nl = strlen(name);
    for (const char *p = e; *p;) {
        const char *eq = strchr(p, '='), *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        if (eq && eq < end && (size_t)(eq - p) == nl && !strncasecmp(p, name, nl)) {
            char ip[64];
            snprintf(ip, sizeof ip, "%.*s", (int)(end - eq - 1), eq + 1);
            struct addrinfo hints = {.ai_family = AF_INET}, *ai;
            if (getaddrinfo(ip, NULL, &hints, &ai) == 0) {
                *addr = ((struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr;
                freeaddrinfo(ai);
                return 1;
            }
        }
        p = *end ? end + 1 : end;
    }
    return 0;
}
SHIM(WSOCK32, ord52) {    // gethostbyname(name)
    const char *name = ARG(0) ? ARGP(0) : "";
    uint32_t addrs[8];
    int n = 0;
    if (host_override(name, &addrs[0])) n = 1;
    else {
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *ai, *it;
        if (getaddrinfo(name, NULL, &hints, &ai) == 0) {
            for (it = ai; it && n < 8; it = it->ai_next)
                addrs[n++] = ((struct sockaddr_in *)it->ai_addr)->sin_addr.s_addr;
            freeaddrinfo(ai);
        }
    }
    NTRACE("gethostbyname(%s) -> %d addrs", name, n);
    if (!n) { rt_set_last_error(WSAHOST_NOT_FOUND); RET(0, 1); }
    RET(make_hostent(name, addrs, n), 1);
}
SHIM(WSOCK32, ord51) {    // gethostbyaddr(addr, len, type)
    if (ARG(1) < 4 || ARG(2) != W_AF_INET) { rt_set_last_error(WSAHOST_NOT_FOUND); RET(0, 3); }
    uint32_t a = g_rd32(ARG(0));
    struct sockaddr_in h = {.sin_len = sizeof h, .sin_family = AF_INET};
    h.sin_addr.s_addr = a;
    char name[256];
    if (getnameinfo((struct sockaddr *)&h, sizeof h, name, sizeof name, NULL, 0, NI_NAMEREQD) != 0) {
        rt_set_last_error(WSAHOST_NOT_FOUND);
        RET(0, 3);
    }
    RET(make_hostent(name, &a, 1), 3);
}

SHIM(WSOCK32, ord101) {   // int WSAAsyncSelect(s, hwnd, msg, events): see wsa_async_poll
    int fd = (int)ARG(0);
    uint32_t ev = ARG(3);
    NTRACE("WSAAsyncSelect(%d, hwnd=%x, msg=%x, ev=%x)", fd, ARG(1), ARG(2), ev);
    if (!ev) { async_drop(fd); RET(0, 4); }
    Async *a = async_get(fd);
    if (!a) {
        if (g_nasync == (int)(sizeof g_async / sizeof *g_async)) { rt_set_last_error(10055); RET(W_SOCKET_ERROR, 4); }
        a = &g_async[g_nasync++];
        memset(a, 0, sizeof *a);
        a->fd = fd;
    }
    int fl = fcntl(fd, F_GETFL, 0);   // WSAAsyncSelect implies non-blocking
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    // FD_CONNECT newly requested on a socket that is not connected yet: report completion of the pending connect()
    if ((ev & W_FD_CONNECT) && !(a->mask & W_FD_CONNECT)) {
        struct sockaddr_in h; socklen_t hl = sizeof h;
        a->connecting = getpeername(fd, (struct sockaddr *)&h, &hl) < 0;
    }
    a->hwnd = ARG(1); a->msg = ARG(2); a->mask = ev;
    a->rd = a->wr = a->acc = 1;   // re-registering re-posts any condition that is currently true
    RET(0, 4);
}
