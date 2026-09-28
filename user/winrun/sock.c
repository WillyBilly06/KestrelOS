/* sock.c - winsock, over this system's own networking.
 *
 * Winsock is the Berkeley socket interface with Windows spellings and one
 * genuine difference: a socket is a handle, not a descriptor, and errors come
 * from WSAGetLastError rather than errno.  Underneath, the calls map onto the
 * client-side TCP this system already has - connect, send, receive, close.
 *
 * What is here is the client half.  Listening for connections is not, because
 * nothing below this can accept one yet, and a program told that bind
 * succeeded would then wait forever for a connection that cannot arrive.
 */
#include "win.h"

typedef UINT_PTR SOCKET;
#define INVALID_SOCKET ((SOCKET)~0)
#define SOCKET_ERROR   (-1)

#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define IPPROTO_TCP  6

#define WSAEWOULDBLOCK    10035
#define WSAENOTSOCK       10038
#define WSAEAFNOSUPPORT   10047
#define WSAECONNREFUSED   10061
#define WSAHOST_NOT_FOUND 11001
#define WSAENOTCONN       10057
#define WSAEOPNOTSUPP     10045

typedef struct {
    WORD  sin_family;
    WORD  sin_port;             /* network order */
    DWORD sin_addr;             /* network order */
    BYTE  sin_zero[8];
} sockaddr_in;

typedef struct {
    WORD  sa_family;
    char  sa_data[14];
} sockaddr;

typedef struct {
    WORD  wVersion, wHighVersion;
    char  szDescription[257];
    char  szSystemStatus[129];
    WORD  iMaxSockets, iMaxUdpDg;
    char *lpVendorInfo;
} WSADATA;

typedef struct {
    char  *h_name;
    char **h_aliases;
    WORD   h_addrtype, h_length;
    char **h_addr_list;
} hostent;

static DWORD wsa_error;
static bool  started;

#define MAX_SOCKETS 16
typedef struct { bool used; int handle; bool connected; int timeout_ms; } sockrec;
static sockrec sockets[MAX_SOCKETS];
#define SOCK_BASE 0x6000

static sockrec *sock_from(SOCKET s) {
    if (s < SOCK_BASE || s >= SOCK_BASE + MAX_SOCKETS) return NULL;
    sockrec *r = &sockets[s - SOCK_BASE];
    return r->used ? r : NULL;
}

static uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static uint32_t swap32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

/* ------------------------------------------------------------------ start-up */

static int WINAPI w_WSAStartup(WORD version, WSADATA *data) {
    started = true;
    if (data) {
        memset(data, 0, sizeof *data);
        data->wVersion = version;
        data->wHighVersion = 0x0202;
        strlcpy(data->szDescription, "KestrelOS sockets", sizeof data->szDescription);
        strlcpy(data->szSystemStatus, "running", sizeof data->szSystemStatus);
        data->iMaxSockets = MAX_SOCKETS;
    }
    return 0;
}
static int  WINAPI w_WSACleanup(void) { started = false; return 0; }
static int  WINAPI w_WSAGetLastError(void) { return (int)wsa_error; }
static void WINAPI w_WSASetLastError(int e) { wsa_error = (DWORD)e; }

/* --------------------------------------------------------------- the sockets */

static SOCKET WINAPI w_socket(int family, int type, int protocol) {
    (void)protocol;
    if (family != AF_INET) { wsa_error = WSAEAFNOSUPPORT; return INVALID_SOCKET; }
    if (type != SOCK_STREAM) {
        /* Datagrams have no path through this system's networking yet, and a
         * program told it had one would send into nothing. */
        wsa_error = WSAEOPNOTSUPP;
        win_trace("a datagram socket was asked for, which is not supported");
        return INVALID_SOCKET;
    }
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].used) continue;
        memset(&sockets[i], 0, sizeof sockets[i]);
        sockets[i].used = true;
        sockets[i].handle = -1;
        sockets[i].timeout_ms = 10000;
        return (SOCKET)(SOCK_BASE + i);
    }
    wsa_error = WSAENOTSOCK;
    return INVALID_SOCKET;
}

static int WINAPI w_connect(SOCKET s, const sockaddr *addr, int len) {
    sockrec *r = sock_from(s);
    if (!r || !addr || len < (int)sizeof(sockaddr_in)) { wsa_error = WSAENOTSOCK; return SOCKET_ERROR; }
    const sockaddr_in *in = (const sockaddr_in *)addr;

    /* Everything on the wire is big-endian and everything here is not. */
    uint32_t ip = swap32(in->sin_addr);
    uint16_t port = swap16(in->sin_port);

    int h = tcp_connect(ip, port, r->timeout_ms);
    if (h < 0) { wsa_error = WSAECONNREFUSED; return SOCKET_ERROR; }
    r->handle = h;
    r->connected = true;
    return 0;
}

static int WINAPI w_send(SOCKET s, const char *buf, int len, int flags) {
    (void)flags;
    sockrec *r = sock_from(s);
    if (!r || !r->connected) { wsa_error = WSAENOTCONN; return SOCKET_ERROR; }
    int n = tcp_send(r->handle, buf, len, r->timeout_ms);
    if (n < 0) { wsa_error = WSAENOTCONN; return SOCKET_ERROR; }
    return n;
}

static int WINAPI w_recv(SOCKET s, char *buf, int len, int flags) {
    (void)flags;
    sockrec *r = sock_from(s);
    if (!r || !r->connected) { wsa_error = WSAENOTCONN; return SOCKET_ERROR; }
    int n = tcp_recv(r->handle, buf, len, r->timeout_ms);
    if (n < 0) { wsa_error = WSAEWOULDBLOCK; return SOCKET_ERROR; }
    return n;                      /* zero means the other end closed */
}

static int WINAPI w_closesocket(SOCKET s) {
    sockrec *r = sock_from(s);
    if (!r) { wsa_error = WSAENOTSOCK; return SOCKET_ERROR; }
    if (r->connected) tcp_close(r->handle);
    r->used = false;
    return 0;
}

static int WINAPI w_shutdown(SOCKET s, int how) { (void)how; return sock_from(s) ? 0 : SOCKET_ERROR; }

static int WINAPI w_setsockopt(SOCKET s, int level, int name, const char *value, int len) {
    sockrec *r = sock_from(s);
    if (!r) { wsa_error = WSAENOTSOCK; return SOCKET_ERROR; }
    /* The only option that changes anything here is the timeout. */
    if (level == 0xFFFF && (name == 0x1005 || name == 0x1006) && value && len >= 4)
        r->timeout_ms = *(const int *)value;
    return 0;
}
static int WINAPI w_getsockopt(SOCKET s, int level, int name, char *value, int *len) {
    (void)level; (void)name;
    if (!sock_from(s)) { wsa_error = WSAENOTSOCK; return SOCKET_ERROR; }
    if (value && len && *len >= 4) { *(int *)value = 0; *len = 4; }
    return 0;
}

/* Listening is not implemented, and saying so is better than accepting a bind
 * that can never produce a connection. */
static int WINAPI w_bind(SOCKET s, const sockaddr *addr, int len) {
    (void)s; (void)addr; (void)len;
    wsa_error = WSAEOPNOTSUPP;
    win_trace("bind was called; this system has no listening sockets");
    return SOCKET_ERROR;
}
static int WINAPI w_listen(SOCKET s, int backlog) {
    (void)s; (void)backlog;
    wsa_error = WSAEOPNOTSUPP;
    return SOCKET_ERROR;
}
static SOCKET WINAPI w_accept(SOCKET s, sockaddr *addr, int *len) {
    (void)s; (void)addr; (void)len;
    wsa_error = WSAEOPNOTSUPP;
    return INVALID_SOCKET;
}

/* ------------------------------------------------------------------ names */

static DWORD WINAPI w_inet_addr(const char *text) {
    uint32_t ip = 0;
    if (!text || !parse_ipv4(text, &ip)) return 0xFFFFFFFF;   /* INADDR_NONE */
    return swap32(ip);
}

static char *WINAPI w_inet_ntoa(DWORD addr) {
    static char text[16];
    format_ipv4(swap32(addr), text, sizeof text);
    return text;
}

static hostent *WINAPI w_gethostbyname(const char *name) {
    static hostent host;
    static uint32_t address;
    static char *addr_list[2];
    static char namebuf[128];

    if (!name) { wsa_error = WSAHOST_NOT_FOUND; return NULL; }

    uint32_t ip = 0;
    if (!parse_ipv4(name, &ip)) {
        ip = net_resolve(name, 5000);
        if (!ip) { wsa_error = WSAHOST_NOT_FOUND; return NULL; }
    }

    address = swap32(ip);
    addr_list[0] = (char *)&address;
    addr_list[1] = NULL;
    strlcpy(namebuf, name, sizeof namebuf);

    host.h_name = namebuf;
    host.h_aliases = NULL;
    host.h_addrtype = AF_INET;
    host.h_length = 4;
    host.h_addr_list = addr_list;
    return &host;
}

typedef struct addrinfo_s {
    int flags, family, socktype, protocol;
    size_t addrlen;
    char *canonname;
    sockaddr *addr;
    struct addrinfo_s *next;
} addrinfo;

static int WINAPI w_getaddrinfo(const char *node, const char *service,
                                const addrinfo *hints, addrinfo **out) {
    (void)hints;
    static addrinfo result;
    static sockaddr_in address;
    if (!node || !out) return WSAHOST_NOT_FOUND;

    uint32_t ip = 0;
    if (!parse_ipv4(node, &ip)) {
        ip = net_resolve(node, 5000);
        if (!ip) return WSAHOST_NOT_FOUND;
    }

    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr = swap32(ip);
    address.sin_port = swap16((uint16_t)(service ? atoi(service) : 0));

    memset(&result, 0, sizeof result);
    result.family = AF_INET;
    result.socktype = SOCK_STREAM;
    result.protocol = IPPROTO_TCP;
    result.addrlen = sizeof address;
    result.addr = (sockaddr *)&address;
    *out = &result;
    return 0;
}
static void WINAPI w_freeaddrinfo(addrinfo *a) { (void)a; }

static WORD WINAPI w_htons(WORD v) { return swap16(v); }
static WORD WINAPI w_ntohs(WORD v) { return swap16(v); }
static DWORD WINAPI w_htonl(DWORD v) { return swap32(v); }
static DWORD WINAPI w_ntohl(DWORD v) { return swap32(v); }

static int WINAPI w_gethostname(char *out, int len) {
    return out && len > 8 ? (strlcpy(out, "kestrel", (size_t)len), 0) : SOCKET_ERROR;
}

static const win_export_t ws2_32[] = {
    { "WSAStartup",     (void *)w_WSAStartup },
    { "WSACleanup",     (void *)w_WSACleanup },
    { "WSAGetLastError",(void *)w_WSAGetLastError },
    { "WSASetLastError",(void *)w_WSASetLastError },
    { "socket",         (void *)w_socket },
    { "connect",        (void *)w_connect },
    { "send",           (void *)w_send },
    { "recv",           (void *)w_recv },
    { "closesocket",    (void *)w_closesocket },
    { "shutdown",       (void *)w_shutdown },
    { "setsockopt",     (void *)w_setsockopt },
    { "getsockopt",     (void *)w_getsockopt },
    { "bind",           (void *)w_bind },
    { "listen",         (void *)w_listen },
    { "accept",         (void *)w_accept },
    { "inet_addr",      (void *)w_inet_addr },
    { "inet_ntoa",      (void *)w_inet_ntoa },
    { "gethostbyname",  (void *)w_gethostbyname },
    { "gethostname",    (void *)w_gethostname },
    { "getaddrinfo",    (void *)w_getaddrinfo },
    { "freeaddrinfo",   (void *)w_freeaddrinfo },
    { "htons",          (void *)w_htons },
    { "ntohs",          (void *)w_ntohs },
    { "htonl",          (void *)w_htonl },
    { "ntohl",          (void *)w_ntohl },
    { NULL, NULL }
};

void sock_init(void) {
    win_register("ws2_32.dll", ws2_32);
    win_register("wsock32.dll", ws2_32);
}
