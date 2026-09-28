/* tcp.c - client-side TCP.
 *
 * Enough TCP to be a client: connect, send, receive, close, with the parts that
 * actually decide whether a connection works rather than the parts that are
 * easy to write.
 *
 * What is here: the three-way handshake, sequence and acknowledgement tracking,
 * a receive buffer and an advertised window, retransmission of unacknowledged
 * data on a timer, duplicate and out-of-order segment handling by dropping what
 * does not fit the expected sequence, an orderly close in both directions, and
 * a reset on anything unexpected.
 *
 * What is not: congestion control, selective acknowledgement, window scaling,
 * timestamps, and listening for incoming connections.  A client fetching a page
 * over a local network never enters a congestion event, and a stack that
 * pretends to implement slow start without ever testing it is worse than one
 * that says it does not.
 *
 * Everything runs under the network lock, on the calling thread, and polls
 * while it waits - the same arrangement the rest of the stack uses.
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "proc.h"
#include "klog.h"
#include "net.h"

/* Provided by net.c: the shared plumbing this sits on. */
void  net_lock_public(void);
void  net_unlock_public(void);
void  net_poll_public(void);
int   ip_send_payload(netdev_t *dev, ipv4_t dest, u8 protocol,
                      const void *payload, int len);
u32   ip_pseudo_sum(ipv4_t src, ipv4_t dest, u8 protocol, u16 length);
u16   net_checksum(const void *data, int len, u32 initial);
netdev_t *net_default_device(void);

#define IP_PROTO_TCP 6

/* ------------------------------------------------------------------ header */

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

static inline u16 get16(const void *p) {
    const u8 *b = p;
    return (u16)((b[0] << 8) | b[1]);
}
static inline u32 get32(const void *p) {
    const u8 *b = p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}
static inline void put16(void *p, u16 v) {
    u8 *b = p;
    b[0] = (u8)(v >> 8); b[1] = (u8)v;
}
static inline void put32(void *p, u32 v) {
    u8 *b = p;
    b[0] = (u8)(v >> 24); b[1] = (u8)(v >> 16);
    b[2] = (u8)(v >> 8);  b[3] = (u8)v;
}

/* ---------------------------------------------------------------- sockets */

#define TCP_SOCKETS      4
#define RX_BUFFER        16384
#define TX_BUFFER        8192
#define TCP_MSS          1400        /* under the 1460 an Ethernet link allows,
                                      * leaving room for anything in the way */
#define TCP_HELD_SEGMENTS 8
#define RETRANSMIT_MS    600
#define MAX_RETRIES      8

typedef enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT,        /* our FIN sent, waiting for theirs        */
    TCP_CLOSE_WAIT,      /* their FIN seen, ours not sent yet       */
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
} tcp_state_t;

typedef struct {
    bool        used;
    tcp_state_t state;

    netdev_t *dev;
    ipv4_t    remote_ip;
    u16       remote_port, local_port;

    u32 send_next;          /* the next sequence number to use        */
    u32 send_unacked;       /* the oldest one not yet acknowledged    */
    u32 recv_next;          /* the next sequence number expected      */
    u16 remote_window;

    /* Data written but not yet acknowledged, kept so it can be sent again. */
    u8  tx[TX_BUFFER];
    int tx_len;
    u64 tx_sent_ms;
    int retries;

    u8  rx[RX_BUFFER];
    int rx_head, rx_tail;

    /* Segments that arrived before the one they follow.
     *
     * Without this, a single lost or reordered segment means every segment
     * after it is thrown away and has to be sent again, and the connection
     * moves forward one retransmission timeout at a time.  Over a link where
     * that happens even occasionally, a page that should take a moment takes
     * half a minute - which is long enough that the other end gives up and
     * closes first.  Holding them until the gap fills turns a lost segment
     * into one retransmission rather than a stall. */
    struct {
        u32 seq;
        int len;
        u8  data[TCP_MSS];
    } held[TCP_HELD_SEGMENTS];
    int held_count;
    int reordered;          /* how many arrived early, for the record */

    bool peer_closed;       /* their FIN has been seen and acknowledged */
    bool reset;             /* the connection was reset                 */
} tcp_socket_t;

static tcp_socket_t sockets[TCP_SOCKETS];

/* Sequence numbers wrap, so they are compared by the sign of the difference
 * rather than by magnitude. */
static inline bool seq_after(u32 a, u32 b)  { return (s32)(a - b) > 0; }
static inline bool seq_before(u32 a, u32 b) { return (s32)(a - b) < 0; }

static int rx_used(const tcp_socket_t *s) {
    return (s->rx_head - s->rx_tail + RX_BUFFER) % RX_BUFFER;
}
static int rx_free(const tcp_socket_t *s) { return RX_BUFFER - 1 - rx_used(s); }

/* ------------------------------------------------------------------ output */

static void tcp_send(tcp_socket_t *s, u8 flags, const void *data, int len) {
    static u8 segment[TCP_MSS + 20];
    if (len < 0 || len > TCP_MSS) return;

    memset(segment, 0, 20);
    put16(segment + 0, s->local_port);
    put16(segment + 2, s->remote_port);
    put32(segment + 4, s->send_next);
    put32(segment + 8, (flags & TCP_ACK) ? s->recv_next : 0);
    segment[12] = 5 << 4;                       /* a 20-byte header */
    segment[13] = flags;

    int window = rx_free(s);
    if (window > 65535) window = 65535;
    put16(segment + 14, (u16)window);
    put16(segment + 16, 0);                     /* checksum, filled in below */
    put16(segment + 18, 0);                     /* no urgent pointer */

    if (len && data) memcpy(segment + 20, data, (size_t)len);

    u32 sum = ip_pseudo_sum(s->dev->ip, s->remote_ip, IP_PROTO_TCP,
                            (u16)(20 + len));
    put16(segment + 16, net_checksum(segment, 20 + len, sum));

    ip_send_payload(s->dev, s->remote_ip, IP_PROTO_TCP, segment, 20 + len);
}

/* Send whatever is queued that has not been acknowledged. */
static void tcp_flush(tcp_socket_t *s) {
    if (!s->tx_len) return;

    int len = s->tx_len;
    if (len > TCP_MSS) len = TCP_MSS;
    if (s->remote_window && len > s->remote_window) len = s->remote_window;
    if (len <= 0) return;

    /* send_next is rewound to the unacknowledged point so a retransmission
     * carries the sequence number the other end is still waiting for. */
    s->send_next = s->send_unacked;
    tcp_send(s, TCP_ACK | TCP_PSH, s->tx, len);
    s->send_next = s->send_unacked + (u32)len;
    s->tx_sent_ms = g_uptime_ms;
}

/* Move bytes into the ring, as many as there is room for.  Taking part of a
 * segment is allowed: the rest will be sent again, because only what was
 * taken is acknowledged. */
static void accept_data(tcp_socket_t *s, const u8 *payload, int payload_len) {
    int room = rx_free(s);
    int take = payload_len < room ? payload_len : room;
    for (int i = 0; i < take; i++) {
        s->rx[s->rx_head] = payload[i];
        s->rx_head = (s->rx_head + 1) % RX_BUFFER;
    }
    s->recv_next += (u32)take;
}

/* Keep a segment that arrived before its turn. */
static void hold_segment(tcp_socket_t *s, u32 seq, const u8 *payload, int len) {
    if (len > TCP_MSS) len = TCP_MSS;

    /* One already held covering this is enough. */
    for (int i = 0; i < s->held_count; i++)
        if (s->held[i].seq == seq) return;

    if (s->held_count >= TCP_HELD_SEGMENTS) {
        /* Full.  Drop the one furthest ahead, since it is the least likely to
         * be needed next and keeping it would block a nearer one. */
        int furthest = 0;
        for (int i = 1; i < s->held_count; i++)
            if (seq_after(s->held[i].seq, s->held[furthest].seq)) furthest = i;
        if (!seq_after(s->held[furthest].seq, seq)) return;   /* this one is worse */
        s->held[furthest] = s->held[s->held_count - 1];
        s->held_count--;
    }

    s->held[s->held_count].seq = seq;
    s->held[s->held_count].len = len;
    memcpy(s->held[s->held_count].data, payload, (size_t)len);
    s->held_count++;
    s->reordered++;
}

/* Having just filled a gap, take everything that was waiting behind it. */
static void drain_held(tcp_socket_t *s) {
    bool moved = true;
    while (moved && s->held_count) {
        moved = false;
        for (int i = 0; i < s->held_count; i++) {
            if (s->held[i].seq != s->recv_next) {
                /* One that overlaps what has already been taken: the useful
                 * part is what starts at the expected point. */
                if (seq_before(s->held[i].seq, s->recv_next)) {
                    u32 skip = s->recv_next - s->held[i].seq;
                    if (skip < (u32)s->held[i].len) {
                        accept_data(s, s->held[i].data + skip,
                                    s->held[i].len - (int)skip);
                        moved = true;
                    }
                    s->held[i] = s->held[--s->held_count];
                    i--;
                }
                continue;
            }
            accept_data(s, s->held[i].data, s->held[i].len);
            s->held[i] = s->held[--s->held_count];
            moved = true;
            break;
        }
    }
}


/* ------------------------------------------------------------------- input */

static tcp_socket_t *find_socket(ipv4_t from, u16 from_port, u16 to_port) {
    for (int i = 0; i < TCP_SOCKETS; i++) {
        tcp_socket_t *s = &sockets[i];
        if (!s->used || s->state == TCP_CLOSED) continue;
        if (s->local_port != to_port) continue;
        if (s->remote_port != from_port || s->remote_ip != from) continue;
        return s;
    }
    return NULL;
}

/* Called from net.c's IPv4 handler.  Runs inside the receive path, so nothing
 * here may wait for anything. */
void tcp_receive(netdev_t *dev, ipv4_t from, const u8 *p, int len) {
    (void)dev;
    if (len < 20) return;

    u16 from_port = get16(p + 0);
    u16 to_port = get16(p + 2);
    u32 seq = get32(p + 4);
    u32 ack = get32(p + 8);
    int header_len = (p[12] >> 4) * 4;
    u8  flags = p[13];
    u16 window = get16(p + 14);

    if (header_len < 20 || header_len > len) return;

    const u8 *payload = p + header_len;
    int payload_len = len - header_len;

    tcp_socket_t *s = find_socket(from, from_port, to_port);
    if (!s) return;                     /* nothing here wants it */

    s->remote_window = window;

    if (flags & TCP_RST) {
        s->reset = true;
        s->state = TCP_CLOSED;
        return;
    }

    switch (s->state) {
    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
            ack == s->send_next) {
            /* Their sequence number starts one past their SYN. */
            s->recv_next = seq + 1;
            s->send_unacked = ack;
            s->state = TCP_ESTABLISHED;
            tcp_send(s, TCP_ACK, NULL, 0);
        }
        return;

    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT:
    case TCP_CLOSE_WAIT:
    case TCP_LAST_ACK:
        break;

    default:
        return;
    }

    /* Acknowledgement: retire what has been confirmed. */
    if ((flags & TCP_ACK) && seq_after(ack, s->send_unacked) &&
        !seq_after(ack, s->send_next)) {
        int confirmed = (int)(ack - s->send_unacked);
        if (confirmed > s->tx_len) confirmed = s->tx_len;
        if (confirmed > 0) {
            memmove(s->tx, s->tx + confirmed, (size_t)(s->tx_len - confirmed));
            s->tx_len -= confirmed;
        }
        s->send_unacked = ack;
        s->retries = 0;

        if (s->state == TCP_LAST_ACK && ack == s->send_next) {
            s->state = TCP_CLOSED;
            return;
        }
        if (s->state == TCP_FIN_WAIT && ack == s->send_next && s->peer_closed) {
            s->state = TCP_CLOSED;
            return;
        }
    }

    /* Data. */
    if (payload_len > 0) {
        if (seq == s->recv_next) {
            accept_data(s, payload, payload_len);

            /* The gap this filled may have been holding others behind it. */
            drain_held(s);
            tcp_send(s, TCP_ACK, NULL, 0);
        } else if (seq_after(seq, s->recv_next)) {
            /* Early.  Keep it, and tell the other end where the gap starts so
             * it sends the missing piece rather than everything after it. */
            hold_segment(s, seq, payload, payload_len);
            tcp_send(s, TCP_ACK, NULL, 0);
            return;
        } else {
            /* Already had it: the acknowledgement was lost, not the data. */
            tcp_send(s, TCP_ACK, NULL, 0);
            return;
        }
    }

    if ((flags & TCP_FIN) && seq_before(seq, s->recv_next + 1) &&
        !s->peer_closed) {
        s->peer_closed = true;
        s->recv_next++;                          /* a FIN takes a sequence */
        tcp_send(s, TCP_ACK, NULL, 0);

        if (s->state == TCP_ESTABLISHED) s->state = TCP_CLOSE_WAIT;
        else if (s->state == TCP_FIN_WAIT && s->send_unacked == s->send_next)
            s->state = TCP_CLOSED;
    }
}

/* ------------------------------------------------------------------ timers */

/* Resend anything outstanding that has been waiting too long.  Called from the
 * wait loops, which is the only place it needs to run: a socket nobody is
 * waiting on has nothing in flight. */
static void tcp_tick(tcp_socket_t *s) {
    if (!s->tx_len || s->state == TCP_CLOSED) return;
    if (g_uptime_ms - s->tx_sent_ms < RETRANSMIT_MS) return;

    if (++s->retries > MAX_RETRIES) {
        kwarn("tcp", "no acknowledgement after %d attempts; giving up",
              MAX_RETRIES);
        s->reset = true;
        s->state = TCP_CLOSED;
        return;
    }
    tcp_flush(s);
}

/* ------------------------------------------------------------------ public */

/* A port nothing else is using.  The ephemeral range exists for exactly this. */
static u16 pick_port(void) {
    static u16 next;
    if (!next) next = (u16)(49152 + (g_uptime_ms & 0x3FFF));

    for (int attempt = 0; attempt < 1000; attempt++) {
        if (++next < 49152) next = 49152;
        bool taken = false;
        for (int i = 0; i < TCP_SOCKETS; i++)
            if (sockets[i].used && sockets[i].local_port == next) taken = true;
        if (!taken) return next;
    }
    return 0;
}

int tcp_connect(ipv4_t ip, u16 port, int timeout_ms) {
    netdev_t *dev = net_default_device();
    if (!dev || !dev->configured) return -1;

    net_lock_public();

    tcp_socket_t *s = NULL;
    int handle = -1;
    for (int i = 0; i < TCP_SOCKETS; i++)
        if (!sockets[i].used) { s = &sockets[i]; handle = i; break; }
    if (!s) { net_unlock_public(); return -1; }

    memset(s, 0, sizeof *s);
    s->used = true;
    s->dev = dev;
    s->remote_ip = ip;
    s->remote_port = port;
    s->local_port = pick_port();
    /* The initial sequence number should not be guessable and should not repeat
     * between connections to the same port pair. */
    s->send_next = (u32)(g_uptime_ms * 2654435761u) ^ ((u32)s->local_port << 16);
    s->send_unacked = s->send_next;
    s->state = TCP_SYN_SENT;
    s->remote_window = TCP_MSS;

    u32 syn_seq = s->send_next;
    tcp_send(s, TCP_SYN, NULL, 0);
    s->send_next = syn_seq + 1;                  /* a SYN takes a sequence */

    u64 deadline = g_uptime_ms + (u64)timeout_ms;
    u64 resend = g_uptime_ms + RETRANSMIT_MS;

    while (!proc_stop_requested() && g_uptime_ms < deadline && s->state == TCP_SYN_SENT && !s->reset) {
        if (g_uptime_ms >= resend) {
            u32 saved = s->send_next;
            s->send_next = syn_seq;
            tcp_send(s, TCP_SYN, NULL, 0);
            s->send_next = saved;
            resend = g_uptime_ms + RETRANSMIT_MS;
        }
        net_poll_public();
        sched_sleep_ms(1);
    }

    bool connected = !proc_stop_requested() && (s->state == TCP_ESTABLISHED);
    if (!connected) {
        s->used = false;
        handle = -1;
    }
    net_unlock_public();
    return handle;
}

int tcp_send_data(int handle, const void *data, int len, int timeout_ms) {
    if (handle < 0 || handle >= TCP_SOCKETS || len < 0) return -1;

    net_lock_public();
    tcp_socket_t *s = &sockets[handle];
    /* Sending is still allowed after the other end has said it has finished
     * sending: their FIN closes their half of the connection, not ours.  A
     * server that finishes its part of a conversation early - which is what a
     * handshake looks like from the other side - must not make this side
     * unable to reply. */
    if (!s->used ||
        (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT)) {
        net_unlock_public();
        return -1;
    }

    const u8 *src = data;
    int written = 0;
    u64 deadline = g_uptime_ms + (u64)timeout_ms;

    while (!proc_stop_requested() && written < len && g_uptime_ms < deadline && !s->reset &&
           s->state != TCP_CLOSED) {
        int room = TX_BUFFER - s->tx_len;
        if (room > 0) {
            int take = len - written;
            if (take > room) take = room;
            memcpy(s->tx + s->tx_len, src + written, (size_t)take);
            s->tx_len += take;
            written += take;
            tcp_flush(s);
        }

        /* Wait for room, which means waiting for an acknowledgement. */
        while (!proc_stop_requested() && s->tx_len >= TX_BUFFER && g_uptime_ms < deadline && !s->reset) {
            tcp_tick(s);
            net_poll_public();
            sched_sleep_ms(1);
        }
    }

    /* Do not return until it has actually gone out and been acknowledged;
     * a caller that then waits for a reply would otherwise wait forever. */
    while (!proc_stop_requested() && s->tx_len > 0 && g_uptime_ms < deadline && !s->reset) {
        tcp_tick(s);
        net_poll_public();
        sched_sleep_ms(1);
    }

    net_unlock_public();
    return s->reset ? -1 : written;
}

const char *tcp_state_name(int handle) {
    if (handle < 0 || handle >= TCP_SOCKETS) return "not a connection";
    tcp_socket_t *s = &sockets[handle];
    if (!s->used) return "closed";
    if (s->reset) return "reset by the other end";
    switch (s->state) {
    case TCP_CLOSED:      return "closed";
    case TCP_SYN_SENT:    return "still connecting";
    case TCP_ESTABLISHED: return "open";
    case TCP_FIN_WAIT:    return "closing";
    case TCP_CLOSE_WAIT:  return "the other end has finished sending";
    default:              return "in an unexpected state";
    }
}

int tcp_receive_data(int handle, void *buf, int len, int timeout_ms) {
    if (handle < 0 || handle >= TCP_SOCKETS || len <= 0) return -1;

    net_lock_public();
    tcp_socket_t *s = &sockets[handle];
    if (!s->used) { net_unlock_public(); return -1; }

    u64 deadline = g_uptime_ms + (u64)timeout_ms;

    while (!proc_stop_requested() && rx_used(s) == 0 && g_uptime_ms < deadline && !s->reset) {
        /* Nothing more will arrive on a connection the other end has closed. */
        if (s->peer_closed) break;
        if (s->state == TCP_CLOSED) break;
        tcp_tick(s);
        net_poll_public();
        sched_sleep_ms(1);
    }

    int available = rx_used(s);
    int take = available < len ? available : len;
    u8 *out = buf;
    for (int i = 0; i < take; i++) {
        out[i] = s->rx[s->rx_tail];
        s->rx_tail = (s->rx_tail + 1) % RX_BUFFER;
    }

    /* Taking data from the buffer opens the window again. */
    if (take > 0 && (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT))
        tcp_send(s, TCP_ACK, NULL, 0);

    int result = take;
    if (!take && (s->reset || s->state == TCP_CLOSED)) result = -1;

    net_unlock_public();
    return result;
}

void tcp_close(int handle) {
    if (handle < 0 || handle >= TCP_SOCKETS) return;

    net_lock_public();
    tcp_socket_t *s = &sockets[handle];
    if (!s->used) { net_unlock_public(); return; }

    if (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT) {
        bool they_closed_first = (s->state == TCP_CLOSE_WAIT);
        tcp_send(s, TCP_ACK | TCP_FIN, NULL, 0);
        s->send_next++;
        s->state = they_closed_first ? TCP_LAST_ACK : TCP_FIN_WAIT;

        /* Wait briefly for the close to be acknowledged, then let go: a peer
         * that never answers must not hold a socket forever. */
        u64 deadline = g_uptime_ms + 1500;
        while (!proc_stop_requested() && g_uptime_ms < deadline && s->state != TCP_CLOSED && !s->reset) {
            net_poll_public();
            sched_sleep_ms(1);
        }
    }

    s->used = false;
    s->state = TCP_CLOSED;
    net_unlock_public();
}

bool tcp_is_open(int handle) {
    if (handle < 0 || handle >= TCP_SOCKETS) return false;
    tcp_socket_t *s = &sockets[handle];
    return s->used && !s->reset &&
           (s->state == TCP_ESTABLISHED || rx_used(s) > 0);
}
