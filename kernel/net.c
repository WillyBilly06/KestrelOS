/* net.c - Ethernet, ARP, IPv4, ICMP, UDP, DHCP and DNS.
 *
 * Everything above the frame boundary.  A driver calls net_receive with each
 * frame that arrives and provides a transmit function; nothing here knows which
 * card it is talking to.
 *
 * Addresses are kept in host byte order everywhere inside this file and
 * converted at the wire, because the alternative - carrying network order
 * through the whole stack - is where byte-order mistakes come from.  There is
 * exactly one place each way.
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "proc.h"
#include "klog.h"
#include "net.h"
#include "r8169.h"

/* --------------------------------------------------------- byte order */

/* Read and write wire fields a byte at a time.  Doing it this way is both the
 * byte-order conversion and the answer to alignment: a field in the middle of a
 * frame is rarely aligned, and a 16-bit load from an odd address is a fault on
 * some machines and merely slow on this one. */
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
    b[0] = (u8)(v >> 8);
    b[1] = (u8)v;
}
static inline void put32(void *p, u32 v) {
    u8 *b = p;
    b[0] = (u8)(v >> 24); b[1] = (u8)(v >> 16);
    b[2] = (u8)(v >> 8);  b[3] = (u8)v;
}

/* ------------------------------------------------------------- formatting */

void ipv4_format(ipv4_t ip, char *buf, size_t cap) {
    snprintf(buf, cap, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
}

bool ipv4_parse(const char *text, ipv4_t *out) {
    u32 value = 0;
    int parts = 0;

    while (parts < 4) {
        if (*text < '0' || *text > '9') return false;
        u32 octet = 0;
        int digits = 0;
        while (*text >= '0' && *text <= '9' && digits < 3) {
            octet = octet * 10 + (u32)(*text++ - '0');
            digits++;
        }
        if (octet > 255) return false;
        value = (value << 8) | octet;
        parts++;
        if (parts < 4) {
            if (*text != '.') return false;
            text++;
        }
    }
    if (*text) return false;
    if (out) *out = value;
    return true;
}

void mac_format(const mac_t *mac, char *buf, size_t cap) {
    snprintf(buf, cap, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac->addr[0], mac->addr[1], mac->addr[2],
             mac->addr[3], mac->addr[4], mac->addr[5]);
}

/* ------------------------------------------------------------- checksums */

/* The one's-complement sum the IP family uses.  Folding the carries back in is
 * what makes it insensitive to byte order on the way in, but the result still
 * has to be written as a 16-bit big-endian field. */
static u16 checksum(const void *data, int len, u32 initial) {
    u32 sum = initial;
    const u8 *p = data;

    while (len > 1) { sum += get16(p); p += 2; len -= 2; }
    if (len) sum += (u32)p[0] << 8;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

/* ------------------------------------------------------------------ devices */

static netdev_t *devices;
static int device_count;

void netdev_register(netdev_t *dev) {
    dev->next = NULL;
    if (!devices) devices = dev;
    else {
        netdev_t *last = devices;
        while (last->next) last = last->next;
        last->next = dev;
    }
    device_count++;

    char mac[24];
    mac_format(&dev->mac, mac, sizeof mac);
    kinfo("net", "%s: %s, %s%s", dev->name, dev->model, mac,
          dev->link_up ? ", link up" : ", no link");
}

netdev_t *netdev_first(void) { return devices; }
int netdev_count(void) { return device_count; }

netdev_t *netdev_by_name(const char *name) {
    for (netdev_t *d = devices; d; d = d->next)
        if (!strcmp(d->name, name)) return d;
    return NULL;
}

/* The stack has one shared frame buffer and one shared cache, and two threads
 * reach it: the network thread collecting whatever arrives, and whichever
 * thread is blocked in a syscall waiting for a reply.  Both must not be inside
 * a driver at once - descriptor rings do not survive it, and neither does the
 * frame being built.
 *
 * The lock is recursive because the paths genuinely nest: a syscall takes it
 * and then polls, and a received packet handled inside that poll can itself
 * send a reply.  Interrupts guard the flag rather than the whole section,
 * because a section that waits for a DHCP server has to be able to sleep. */
static proc_t *net_owner;
static int     net_depth;

static void net_lock(void) {
    proc_t *me = proc_current();
    for (;;) {
        bool irq = irq_save();
        if (!net_owner || net_owner == me) {
            net_owner = me;
            net_depth++;
            irq_restore(irq);
            return;
        }
        irq_restore(irq);
        sched_sleep_ms(1);
    }
}

static void net_unlock(void) {
    bool irq = irq_save();
    if (net_depth > 0 && --net_depth == 0) net_owner = NULL;
    irq_restore(irq);
}

/* Whether this thread is inside the receive path.
 *
 * Handling a packet can mean sending one - an echo request wants a reply, and a
 * reply needs the destination's hardware address.  If that send were allowed to
 * wait for an address, it would poll while it waited, the poll would deliver
 * another packet, and handling that packet would wait again: the receive path
 * would call itself until the kernel stack ran off its end into whatever was
 * allocated below it.
 *
 * So nothing reached from a receive handler is allowed to wait.  A send that
 * needs an address it does not have gives up on that one packet instead; the
 * request it leaves behind is answered in the ordinary way, and the packet
 * after it goes out. */
static bool in_receive;
static bool polling;

/* Poll every driver for received frames.  Called from the network thread and
 * from anything waiting on a reply, so a wait makes progress.  The caller
 * always holds the lock. */
static void poll_all(void) {
    /* A driver must not be re-entered while it is walking its own ring. */
    if (polling) return;
    polling = true;

    for (netdev_t *d = devices; d; d = d->next)
        if (d->poll) d->poll(d);

    polling = false;
}

/* The bodies of the operations that block.  Each public entry point takes the
 * lock and calls its counterpart here, so the internal paths - which nest -
 * never have to think about it. */
static bool arp_lookup_locked(netdev_t *dev, ipv4_t ip, mac_t *out, int timeout_ms);

/* ---------------------------------------------------------------- Ethernet */

static const mac_t broadcast_mac = { { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };

/* One frame under construction.  Everything is sent from this rather than from
 * the stack, so a frame can be larger than a kernel stack is comfortable with. */
static u8  tx_frame[ETH_FRAME_MAX];

static u8 *eth_start(const mac_t *dest, const mac_t *src, u16 type) {
    memcpy(tx_frame + 0, dest->addr, ETH_ALEN);
    memcpy(tx_frame + 6, src->addr, ETH_ALEN);
    put16(tx_frame + 12, type);
    return tx_frame + ETH_HLEN;
}

static int eth_send(netdev_t *dev, int payload_len) {
    int len = ETH_HLEN + payload_len;
    /* Anything below the minimum frame size is padded; the hardware will not
     * send a runt and a switch would drop it. */
    if (len < 60) {
        memset(tx_frame + len, 0, (size_t)(60 - len));
        len = 60;
    }
    if (!dev->transmit) return -1;

    int r = dev->transmit(dev, tx_frame, len);
    if (r < 0) { dev->tx_errors++; return r; }
    dev->tx_packets++;
    dev->tx_bytes += (u64)len;
    return r;
}

/* --------------------------------------------------------------------- ARP */

#define ARP_CACHE 32
#define ARP_TTL_MS 120000

static arp_entry_t arp_cache[ARP_CACHE];

static void arp_insert(ipv4_t ip, const mac_t *mac) {
    int free_slot = -1, oldest = 0;

    for (int i = 0; i < ARP_CACHE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            arp_cache[i].mac = *mac;
            arp_cache[i].seen_ms = g_uptime_ms;
            return;
        }
        if (!arp_cache[i].valid && free_slot < 0) free_slot = i;
        if (arp_cache[i].seen_ms < arp_cache[oldest].seen_ms) oldest = i;
    }

    int slot = free_slot >= 0 ? free_slot : oldest;
    arp_cache[slot].ip = ip;
    arp_cache[slot].mac = *mac;
    arp_cache[slot].seen_ms = g_uptime_ms;
    arp_cache[slot].valid = true;
}

static bool arp_find(ipv4_t ip, mac_t *out) {
    for (int i = 0; i < ARP_CACHE; i++) {
        if (!arp_cache[i].valid || arp_cache[i].ip != ip) continue;
        if (g_uptime_ms - arp_cache[i].seen_ms > ARP_TTL_MS) {
            arp_cache[i].valid = false;
            return false;
        }
        if (out) *out = arp_cache[i].mac;
        return true;
    }
    return false;
}

int net_arp_snapshot(arp_entry_t *out, int max) {
    /* A snapshot only reads, and the table is small enough that a torn entry
     * would show a stale address rather than anything unsafe. */
    int n = 0;
    for (int i = 0; i < ARP_CACHE && n < max; i++)
        if (arp_cache[i].valid) out[n++] = arp_cache[i];
    return n;
}

static void arp_send(netdev_t *dev, u16 op, ipv4_t target_ip,
                     const mac_t *target_mac) {
    u8 *p = eth_start(op == 1 ? &broadcast_mac : target_mac, &dev->mac,
                      ETHERTYPE_ARP);

    put16(p + 0, 1);                       /* hardware type: Ethernet    */
    put16(p + 2, ETHERTYPE_IPV4);
    p[4] = ETH_ALEN;
    p[5] = 4;
    put16(p + 6, op);                      /* 1 request, 2 reply         */
    memcpy(p + 8, dev->mac.addr, ETH_ALEN);
    put32(p + 14, dev->ip);
    memcpy(p + 18, op == 1 ? (const u8 *)"\0\0\0\0\0\0" : target_mac->addr, ETH_ALEN);
    put32(p + 24, target_ip);

    eth_send(dev, 28);
}

static void arp_receive(netdev_t *dev, const u8 *p, int len) {
    if (len < 28) return;
    if (get16(p + 2) != ETHERTYPE_IPV4 || p[4] != ETH_ALEN || p[5] != 4) return;

    u16 op = get16(p + 6);
    mac_t sender_mac;
    memcpy(sender_mac.addr, p + 8, ETH_ALEN);
    ipv4_t sender_ip = get32(p + 14);
    ipv4_t target_ip = get32(p + 24);

    /* Anything that talks to us is worth remembering, request or reply. */
    if (sender_ip) arp_insert(sender_ip, &sender_mac);

    if (op == 1 && dev->configured && target_ip == dev->ip)
        arp_send(dev, 2, sender_ip, &sender_mac);
}

bool net_arp_lookup(netdev_t *dev, ipv4_t ip, mac_t *out, int timeout_ms) {
    net_lock();
    bool found = arp_lookup_locked(dev, ip, out, timeout_ms);
    net_unlock();
    return found;
}

static bool arp_lookup_locked(netdev_t *dev, ipv4_t ip, mac_t *out, int timeout_ms) {
    if (arp_find(ip, out)) return true;
    if (!dev->configured) return false;

    /* Called while handling a received packet: send the request but do not wait
     * for it, because waiting means polling and polling means recursion. */
    if (in_receive) {
        ipv4_t ask = ip;
        if (dev->netmask && ((ip ^ dev->ip) & dev->netmask)) {
            if (!dev->gateway) return false;
            ask = dev->gateway;
            if (arp_find(ask, out)) return true;
        }
        arp_send(dev, 1, ask, NULL);
        return false;
    }

    /* Anything off this subnet goes to the gateway, so that is what gets
     * resolved rather than the destination. */
    ipv4_t target = ip;
    if (dev->netmask && ((ip ^ dev->ip) & dev->netmask)) {
        if (!dev->gateway) return false;
        target = dev->gateway;
        if (arp_find(target, out)) return true;
    }

    u64 deadline = g_uptime_ms + (u64)timeout_ms;
    u64 next_send = 0;

    while (g_uptime_ms < deadline) {
        if (g_uptime_ms >= next_send) {
            arp_send(dev, 1, target, NULL);
            next_send = g_uptime_ms + 500;
        }
        poll_all();
        if (arp_find(target, out)) return true;
        sched_sleep_ms(2);
    }
    return false;
}

/* -------------------------------------------------------------------- IPv4 */

#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define IP_PROTO_TCP  6

static u16 ip_ident;

/* Build an IPv4 header and send it.  Returns the payload pointer to fill in
 * before calling, so the caller writes its payload straight into the frame. */
static u8 *ip_start(netdev_t *dev, ipv4_t dest, u8 protocol, mac_t *dest_mac) {
    if (!arp_lookup_locked(dev, dest, dest_mac, 2000)) return NULL;
    u8 *ip = eth_start(dest_mac, &dev->mac, ETHERTYPE_IPV4);
    (void)protocol;
    return ip + 20;                        /* where the payload goes */
}

static int ip_send(netdev_t *dev, const mac_t *dest_mac, ipv4_t dest,
                   u8 protocol, int payload_len) {
    u8 *ip = tx_frame + ETH_HLEN;
    (void)dest_mac;

    ip[0] = 0x45;                          /* version 4, 20-byte header */
    ip[1] = 0;                             /* no differentiated services */
    put16(ip + 2, (u16)(20 + payload_len));
    put16(ip + 4, ++ip_ident);
    put16(ip + 6, 0x4000);                 /* do not fragment           */
    ip[8] = 64;                            /* time to live              */
    ip[9] = protocol;
    put16(ip + 10, 0);                     /* checksum, filled in below */
    put32(ip + 12, dev->ip);
    put32(ip + 16, dest);

    put16(ip + 10, checksum(ip, 20, 0));
    return eth_send(dev, 20 + payload_len);
}

/* ------------------------------------------------------------------- ICMP */

static int ping_locked(ipv4_t dest, int count, int timeout_ms, u32 *times_ms);

/* The most recent echo reply, which is what ping waits on. */
static struct {
    bool   waiting;
    u16    ident, sequence;
    ipv4_t from;
    u64    sent_ms;
    bool   got_reply;
    u32    rtt_ms;
} echo;

static void icmp_receive(netdev_t *dev, ipv4_t from, const u8 *p, int len) {
    if (len < 8) return;

    u8 type = p[0];
    u16 ident = get16(p + 4);
    u16 seq = get16(p + 6);

    if (type == 8) {
        /* An echo request: reply with the same payload, which is what the
         * sender is checking. */
        if (len > ETH_MTU - 20) return;
        mac_t dest_mac;
        u8 *out = ip_start(dev, from, IP_PROTO_ICMP, &dest_mac);
        if (!out) return;

        memcpy(out, p, (size_t)len);
        out[0] = 0;                        /* echo reply */
        put16(out + 2, 0);
        put16(out + 2, checksum(out, len, 0));
        ip_send(dev, &dest_mac, from, IP_PROTO_ICMP, len);
        return;
    }

    if (type == 0 && echo.waiting && ident == echo.ident && seq == echo.sequence) {
        echo.got_reply = true;
        echo.from = from;
        echo.rtt_ms = (u32)(g_uptime_ms - echo.sent_ms);
    }
}

int net_ping(ipv4_t dest, int count, int timeout_ms, u32 *times_ms) {
    net_lock();
    int r = ping_locked(dest, count, timeout_ms, times_ms);
    net_unlock();
    return r;
}

static int ping_locked(ipv4_t dest, int count, int timeout_ms, u32 *times_ms) {
    netdev_t *dev = devices;
    while (dev && !dev->configured) dev = dev->next;
    if (!dev) return -1;

    static u16 next_ident = 1;
    u16 ident = next_ident++;
    int replies = 0;

    for (int i = 0; i < count; i++) {
        mac_t dest_mac;
        u8 *icmp = ip_start(dev, dest, IP_PROTO_ICMP, &dest_mac);
        if (!icmp) return replies;         /* nothing answered the ARP */

        /* 56 bytes of payload, which is what every other ping sends. */
        const int payload = 56;
        icmp[0] = 8;                       /* echo request */
        icmp[1] = 0;
        put16(icmp + 2, 0);
        put16(icmp + 4, ident);
        put16(icmp + 6, (u16)(i + 1));
        for (int k = 0; k < payload; k++) icmp[8 + k] = (u8)(k & 0xFF);
        put16(icmp + 2, checksum(icmp, 8 + payload, 0));

        echo.waiting = true;
        echo.got_reply = false;
        echo.ident = ident;
        echo.sequence = (u16)(i + 1);
        echo.sent_ms = g_uptime_ms;

        ip_send(dev, &dest_mac, dest, IP_PROTO_ICMP, 8 + payload);

        u64 deadline = g_uptime_ms + (u64)timeout_ms;
        while (g_uptime_ms < deadline && !echo.got_reply) {
            poll_all();
            sched_sleep_ms(1);
        }
        echo.waiting = false;

        if (echo.got_reply) {
            if (times_ms) times_ms[replies] = echo.rtt_ms;
            replies++;
        }
        if (i + 1 < count) sched_sleep_ms(200);
    }
    return replies;
}

/* --------------------------------------------------------------------- UDP */

#define UDP_BINDINGS 8

static struct {
    bool          used;
    u16           port;
    udp_handler_t handler;
    void         *ctx;
} bindings[UDP_BINDINGS];

int udp_bind(u16 port, udp_handler_t handler, void *ctx) {
    for (int i = 0; i < UDP_BINDINGS; i++) {
        if (bindings[i].used) continue;
        bindings[i].used = true;
        bindings[i].port = port;
        bindings[i].handler = handler;
        bindings[i].ctx = ctx;
        return 0;
    }
    return -1;
}

void udp_unbind(u16 port) {
    for (int i = 0; i < UDP_BINDINGS; i++)
        if (bindings[i].used && bindings[i].port == port) bindings[i].used = false;
}

/* The pseudo-header UDP and TCP checksums cover: the addresses, the protocol
 * and the length, none of which are in the transport header itself. */
static u32 pseudo_header_sum(ipv4_t src, ipv4_t dest, u8 protocol, u16 length) {
    u32 sum = 0;
    sum += (src >> 16) & 0xFFFF;
    sum += src & 0xFFFF;
    sum += (dest >> 16) & 0xFFFF;
    sum += dest & 0xFFFF;
    sum += protocol;
    sum += length;
    return sum;
}

int udp_send(netdev_t *dev, ipv4_t dest, u16 dest_port, u16 src_port,
             const void *data, int len) {
    if (!dev || len < 0 || len > ETH_MTU - 28) return -1;

    mac_t dest_mac;

    /* A broadcast goes out without asking anyone, which is what makes DHCP
     * possible before there is an address to ask from. */
    u8 *udp;
    if (dest == 0xFFFFFFFFu) {
        dest_mac = broadcast_mac;
        udp = eth_start(&dest_mac, &dev->mac, ETHERTYPE_IPV4) + 20;
    } else {
        udp = ip_start(dev, dest, IP_PROTO_UDP, &dest_mac);
        if (!udp) return -1;
    }

    put16(udp + 0, src_port);
    put16(udp + 2, dest_port);
    put16(udp + 4, (u16)(8 + len));
    put16(udp + 6, 0);
    if (len && data) memcpy(udp + 8, data, (size_t)len);

    u32 sum = pseudo_header_sum(dev->ip, dest, IP_PROTO_UDP, (u16)(8 + len));
    u16 c = checksum(udp, 8 + len, sum);
    /* Zero means "no checksum" on the wire, so it is sent as all ones. */
    put16(udp + 6, c ? c : 0xFFFF);

    return ip_send(dev, &dest_mac, dest, IP_PROTO_UDP, 8 + len);
}

static void udp_receive(netdev_t *dev, ipv4_t from, const u8 *p, int len) {
    (void)dev;
    if (len < 8) return;

    u16 src_port = get16(p + 0);
    u16 dest_port = get16(p + 2);
    u16 length = get16(p + 4);
    if (length < 8 || length > len) return;

    for (int i = 0; i < UDP_BINDINGS; i++) {
        if (!bindings[i].used || bindings[i].port != dest_port) continue;
        if (bindings[i].handler)
            bindings[i].handler(bindings[i].ctx, from, src_port,
                                p + 8, length - 8);
        return;
    }
}

/* The hardware address the frame being handled came from, so the IPv4 layer can
 * record it without threading the Ethernet header through every call. */
static mac_t frame_source;

/* ------------------------------------------------------------------ IPv4 in */

static void ip_receive(netdev_t *dev, const u8 *p, int len) {
    if (len < 20) return;
    if ((p[0] >> 4) != 4) return;

    int header_len = (p[0] & 0x0F) * 4;
    if (header_len < 20 || header_len > len) return;

    u16 total = get16(p + 2);
    if (total < (u16)header_len || total > len) return;

    /* A fragmented datagram needs reassembly, which nothing here sends and
     * nothing here needs; dropping it is better than acting on half of one. */
    u16 flags_offset = get16(p + 6);
    if ((flags_offset & 0x2000) || (flags_offset & 0x1FFF)) {
        dev->rx_dropped++;
        return;
    }

    if (checksum(p, header_len, 0) != 0) {
        dev->rx_errors++;
        return;
    }

    ipv4_t src = get32(p + 12);
    ipv4_t dest = get32(p + 16);

    /* Learn where this came from.  A host on the same subnet that has just sent
     * us a packet is one whose hardware address we now know, and remembering it
     * means a reply never has to wait for an address resolution - which the
     * receive path is not allowed to do. */
    if (dev->configured && src && dev->netmask &&
        !((src ^ dev->ip) & dev->netmask))
        arp_insert(src, &frame_source);

    /* Accept what is addressed to us, plus broadcasts - which is how a DHCP
     * offer arrives before there is an address at all. */
    bool for_us = !dev->configured || dest == dev->ip || dest == 0xFFFFFFFFu ||
                  (dev->netmask && dest == (dev->ip | ~dev->netmask));
    if (!for_us) return;

    const u8 *payload = p + header_len;
    int payload_len = total - header_len;

    switch (p[9]) {
    case IP_PROTO_ICMP: icmp_receive(dev, src, payload, payload_len); break;
    case IP_PROTO_UDP:  udp_receive(dev, src, payload, payload_len); break;
    case IP_PROTO_TCP:  tcp_receive(dev, src, payload, payload_len); break;
    default: break;
    }
}

/* Where a test can watch what a driver hands up.
 *
 * A driver test that stops at "the descriptor was returned" has not checked
 * the thing that matters, which is whether the bytes that reach the stack are
 * the bytes that arrived.  This is how a test sees them without the driver
 * knowing it is being watched. */
static u8   *capture_buf;
static size_t capture_cap;
static int  *capture_len;

void net_test_capture(u8 *buf, size_t cap, int *len_out) {
    capture_buf = buf;
    capture_cap = cap;
    capture_len = len_out;
    if (len_out) *len_out = 0;
}

void net_receive(netdev_t *dev, const void *frame, int len) {
    if (capture_buf && len > 0) {
        size_t take = (size_t)len < capture_cap ? (size_t)len : capture_cap;
        memcpy(capture_buf, frame, take);
        if (capture_len) *capture_len = len;
    }

    if (len < ETH_HLEN) { dev->rx_errors++; return; }

    dev->rx_packets++;
    dev->rx_bytes += (u64)len;

    const u8 *p = frame;

    /* Accept our own address and broadcasts; a card in promiscuous mode sees
     * everything else too, and forwarding is not this system's job. */
    if (memcmp(p, dev->mac.addr, ETH_ALEN) != 0 &&
        memcmp(p, broadcast_mac.addr, ETH_ALEN) != 0 &&
        !(p[0] & 1))                                  /* multicast */
        return;

    memcpy(frame_source.addr, p + 6, ETH_ALEN);

    /* Everything below here may send, but may not wait. */
    bool outer = !in_receive;
    in_receive = true;

    u16 type = get16(p + 12);
    switch (type) {
    case ETHERTYPE_ARP:  arp_receive(dev, p + ETH_HLEN, len - ETH_HLEN); break;
    case ETHERTYPE_IPV4: ip_receive(dev, p + ETH_HLEN, len - ETH_HLEN); break;
    default: break;
    }

    if (outer) in_receive = false;
}

/* -------------------------------------------------------------------- DHCP */

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC       0x63825363

static struct {
    netdev_t *dev;
    u32    xid;
    int    state;              /* 0 idle, 1 discovering, 2 requesting, 3 done */
    ipv4_t offered_ip, server, mask, gateway, dns;
} dhcp;

/* Walk the option field looking for one tag.  Options are a byte of tag, a byte
 * of length and that many bytes of value, ending at tag 255. */
static const u8 *dhcp_option(const u8 *options, int len, u8 want, int *out_len) {
    int i = 0;
    while (i < len) {
        u8 tag = options[i];
        if (tag == 255) break;
        if (tag == 0) { i++; continue; }
        if (i + 1 >= len) break;
        int size = options[i + 1];
        if (i + 2 + size > len) break;
        if (tag == want) {
            if (out_len) *out_len = size;
            return options + i + 2;
        }
        i += 2 + size;
    }
    return NULL;
}

static void dhcp_send(netdev_t *dev, u8 message_type, ipv4_t requested,
                      ipv4_t server) {
    static u8 packet[300];
    memset(packet, 0, sizeof packet);

    packet[0] = 1;                          /* a request from a client   */
    packet[1] = 1;                          /* Ethernet                  */
    packet[2] = ETH_ALEN;
    put32(packet + 4, dhcp.xid);
    put16(packet + 10, 0x8000);             /* ask for a broadcast reply */
    memcpy(packet + 28, dev->mac.addr, ETH_ALEN);
    put32(packet + 236, DHCP_MAGIC);

    u8 *opt = packet + 240;
    *opt++ = 53; *opt++ = 1; *opt++ = message_type;

    if (requested) { *opt++ = 50; *opt++ = 4; put32(opt, requested); opt += 4; }
    if (server)    { *opt++ = 54; *opt++ = 4; put32(opt, server); opt += 4; }

    /* Ask for the three things that make an interface usable. */
    *opt++ = 55; *opt++ = 3;
    *opt++ = 1;                             /* subnet mask   */
    *opt++ = 3;                             /* router        */
    *opt++ = 6;                             /* name servers  */

    *opt++ = 255;

    int len = (int)(opt - packet);
    if (len < 300) len = 300;               /* some servers expect the full
                                             * fixed-size message */
    udp_send(dev, 0xFFFFFFFFu, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, packet, len);
}

static void dhcp_receive(void *ctx, ipv4_t from, u16 from_port,
                         const void *data, int len) {
    (void)ctx; (void)from_port;
    const u8 *p = data;

    if (len < 240 || p[0] != 2) return;     /* not a reply */
    if (get32(p + 4) != dhcp.xid) return;   /* someone else's exchange */
    if (get32(p + 236) != DHCP_MAGIC) return;

    const u8 *options = p + 240;
    int options_len = len - 240;

    int size = 0;
    const u8 *type = dhcp_option(options, options_len, 53, &size);
    if (!type || size < 1) return;

    ipv4_t yours = get32(p + 16);

    if (*type == 2 && dhcp.state == 1) {              /* an offer */
        dhcp.offered_ip = yours;
        dhcp.server = from;

        const u8 *v = dhcp_option(options, options_len, 54, &size);
        if (v && size == 4) dhcp.server = get32(v);

        dhcp.state = 2;
        dhcp_send(dhcp.dev, 3, dhcp.offered_ip, dhcp.server);   /* request */
        return;
    }

    if (*type == 5 && dhcp.state == 2) {              /* acknowledged */
        const u8 *v = dhcp_option(options, options_len, 1, &size);
        dhcp.mask = (v && size == 4) ? get32(v) : IPV4(255, 255, 255, 0);

        v = dhcp_option(options, options_len, 3, &size);
        dhcp.gateway = (v && size >= 4) ? get32(v) : 0;

        v = dhcp_option(options, options_len, 6, &size);
        dhcp.dns = (v && size >= 4) ? get32(v) : 0;

        dhcp.offered_ip = yours;
        dhcp.state = 3;
        return;
    }

    if (*type == 6) dhcp.state = -1;                  /* refused */
}

static int dhcp_locked(netdev_t *dev, int timeout_ms);

int net_dhcp(netdev_t *dev, int timeout_ms) {
    if (!dev) return -1;
    net_lock();
    int r = dhcp_locked(dev, timeout_ms);
    net_unlock();
    return r;
}

static int dhcp_locked(netdev_t *dev, int timeout_ms) {
    memset(&dhcp, 0, sizeof dhcp);
    dhcp.dev = dev;
    /* The transaction id ties a reply to this exchange; the clock and the
     * card's address are enough to make it unlike anyone else's. */
    dhcp.xid = (u32)(g_uptime_ms * 2654435761u) ^
               ((u32)dev->mac.addr[3] << 16) ^ ((u32)dev->mac.addr[4] << 8) ^
               dev->mac.addr[5];
    dhcp.state = 1;

    dev->dhcp_in_progress = true;
    dev->ip = 0;                            /* discover goes out from 0.0.0.0 */

    if (udp_bind(DHCP_CLIENT_PORT, dhcp_receive, NULL) < 0) {
        dev->dhcp_in_progress = false;
        return -1;
    }

    u64 deadline = g_uptime_ms + (u64)timeout_ms;
    u64 next_send = 0;
    int last_state = 0;

    while (g_uptime_ms < deadline && dhcp.state > 0 && dhcp.state != 3) {
        /* Resend on a timer, and immediately whenever the state advances. */
        if (g_uptime_ms >= next_send || last_state != dhcp.state) {
            if (dhcp.state == 1) dhcp_send(dev, 1, 0, 0);       /* discover */
            next_send = g_uptime_ms + 2000;
            last_state = dhcp.state;
        }
        poll_all();
        sched_sleep_ms(2);
    }

    udp_unbind(DHCP_CLIENT_PORT);
    dev->dhcp_in_progress = false;

    if (dhcp.state != 3) {
        kwarn("net", "%s: no answer from a DHCP server", dev->name);
        return -1;
    }

    net_configure(dev, dhcp.offered_ip, dhcp.mask, dhcp.gateway, dhcp.dns);

    char ip[20], gw[20];
    ipv4_format(dev->ip, ip, sizeof ip);
    ipv4_format(dev->gateway, gw, sizeof gw);
    kinfo("net", "%s: %s from DHCP, gateway %s", dev->name, ip, gw);
    return 0;
}

int net_configure(netdev_t *dev, ipv4_t ip, ipv4_t mask, ipv4_t gw, ipv4_t dns) {
    if (!dev) return -1;
    net_lock();
    dev->ip = ip;
    dev->netmask = mask ? mask : IPV4(255, 255, 255, 0);
    dev->gateway = gw;
    dev->dns = dns;
    dev->configured = ip != 0;

    /* Announce the address so switches and neighbours learn where it is, and
     * so a duplicate shows up straight away. */
    if (dev->configured) arp_send(dev, 1, dev->ip, NULL);
    net_unlock();
    return 0;
}

/* --------------------------------------------------------------------- DNS */

static struct {
    bool   waiting;
    u16    id;
    ipv4_t result;
} dns;

static void dns_receive(void *ctx, ipv4_t from, u16 from_port,
                        const void *data, int len) {
    (void)ctx; (void)from; (void)from_port;
    const u8 *p = data;
    if (len < 12 || !dns.waiting) return;
    if (get16(p) != dns.id) return;

    u16 flags = get16(p + 2);
    if (!(flags & 0x8000)) return;                 /* not a response */
    if (flags & 0x000F) { dns.waiting = false; return; }   /* an error code */

    u16 questions = get16(p + 4);
    u16 answers = get16(p + 6);
    if (!answers) { dns.waiting = false; return; }

    int i = 12;
    /* Step over the questions, each a name then four bytes. */
    for (int q = 0; q < questions && i < len; q++) {
        while (i < len && p[i]) {
            if ((p[i] & 0xC0) == 0xC0) { i += 2; break; }   /* a pointer */
            i += p[i] + 1;
        }
        if (i < len && !p[i]) i++;
        i += 4;
    }

    for (int a = 0; a < answers && i + 12 <= len; a++) {
        /* The name, which is nearly always a pointer back into the question. */
        if ((p[i] & 0xC0) == 0xC0) i += 2;
        else {
            while (i < len && p[i]) {
                if ((p[i] & 0xC0) == 0xC0) { i += 2; break; }
                i += p[i] + 1;
            }
            if (i < len && !p[i]) i++;
        }
        if (i + 10 > len) break;

        u16 type = get16(p + i);
        u16 rdlength = get16(p + i + 8);
        i += 10;
        if (i + rdlength > len) break;

        if (type == 1 && rdlength == 4) {           /* an address record */
            dns.result = get32(p + i);
            dns.waiting = false;
            return;
        }
        i += rdlength;
    }
    dns.waiting = false;
}

static ipv4_t resolve_locked(const char *host, int timeout_ms);

ipv4_t net_resolve(const char *host, int timeout_ms) {
    /* A literal address needs no lookup, and no lock either. */
    ipv4_t literal;
    if (ipv4_parse(host, &literal)) return literal;

    net_lock();
    ipv4_t r = resolve_locked(host, timeout_ms);
    net_unlock();
    return r;
}

static ipv4_t resolve_locked(const char *host, int timeout_ms) {

    netdev_t *dev = devices;
    while (dev && (!dev->configured || !dev->dns)) dev = dev->next;
    if (!dev) return 0;

    static u8 query[300];
    memset(query, 0, sizeof query);

    static u16 next_id = 1;
    dns.id = next_id++;
    dns.result = 0;
    dns.waiting = true;

    put16(query + 0, dns.id);
    put16(query + 2, 0x0100);                       /* recursion, please */
    put16(query + 4, 1);                            /* one question      */

    /* A name on the wire is a run of length-prefixed labels ending in zero. */
    int i = 12;
    const char *label = host;
    while (*label && i < 260) {
        const char *dot = strchr(label, '.');
        int n = dot ? (int)(dot - label) : (int)strlen(label);
        if (n <= 0 || n > 63) { dns.waiting = false; return 0; }
        query[i++] = (u8)n;
        memcpy(query + i, label, (size_t)n);
        i += n;
        if (!dot) break;
        label = dot + 1;
    }
    query[i++] = 0;
    put16(query + i, 1); i += 2;                    /* an address record */
    put16(query + i, 1); i += 2;                    /* on the internet   */

    u16 port = (u16)(40000 + (g_uptime_ms & 0x3FFF));
    if (udp_bind(port, dns_receive, NULL) < 0) { dns.waiting = false; return 0; }

    udp_send(dev, dev->dns, 53, port, query, i);

    u64 deadline = g_uptime_ms + (u64)timeout_ms;
    u64 resend = g_uptime_ms + 1000;
    while (!proc_stop_requested() && g_uptime_ms < deadline && dns.waiting) {
        if (g_uptime_ms >= resend) {
            udp_send(dev, dev->dns, 53, port, query, i);
            resend = g_uptime_ms + 1000;
        }
        poll_all();
        sched_sleep_ms(2);
    }

    udp_unbind(port);
    dns.waiting = false;
    return dns.result;
}

/* ------------------------------------------------- what TCP builds on top of */

/* TCP lives in its own file because it is a protocol rather than plumbing, but
 * it needs the same lock, the same poll and the same way out to the wire.
 * Exposing those four is smaller than moving the plumbing. */

void net_lock_public(void)   { net_lock(); }
void net_unlock_public(void) { net_unlock(); }
void net_poll_public(void)   { poll_all(); }

netdev_t *net_default_device(void) {
    for (netdev_t *d = devices; d; d = d->next)
        if (d->configured) return d;
    return devices;
}

u16 net_checksum(const void *data, int len, u32 initial) {
    return checksum(data, len, initial);
}

u32 ip_pseudo_sum(ipv4_t src, ipv4_t dest, u8 protocol, u16 length) {
    return pseudo_header_sum(src, dest, protocol, length);
}

/* Send one already-built transport segment.  The caller has filled in its own
 * header and checksum; this adds the IP header and puts it on the wire. */
int ip_send_payload(netdev_t *dev, ipv4_t dest, u8 protocol,
                    const void *payload, int len) {
    if (!dev || len < 0 || len > ETH_MTU - 20) return -1;

    mac_t dest_mac;
    u8 *out = ip_start(dev, dest, protocol, &dest_mac);
    if (!out) return -1;

    if (len && payload) memcpy(out, payload, (size_t)len);
    return ip_send(dev, &dest_mac, dest, protocol, len);
}

/* -------------------------------------------------------------- the thread */

/* Frames arrive whether or not anything is waiting for them, so something has
 * to collect them.  Polling rather than taking an interrupt keeps the driver
 * simple and costs almost nothing at these rates. */
static void net_thread(void *arg) {
    (void)arg;
    for (;;) {
        /* Skip the poll rather than queue behind a syscall that is already
         * inside the stack: that syscall polls as it waits, so nothing is
         * missed, and the thread never piles up behind a slow DHCP exchange. */
        bool irq = irq_save();
        bool free_now = (net_owner == NULL);
        irq_restore(irq);

        if (free_now) {
            net_lock();
            poll_all();
            net_unlock();
        }
        sched_sleep_ms(1);
    }
}

/* Bring links online without being asked.
 *
 * A machine is expected to be on the network the moment it has a live cable -
 * the way every desktop system is - and not only when someone runs a command.
 * This watches each interface and, when it has a link but no address, asks
 * DHCP for one.  It keeps watching afterwards, so a cable plugged in later, or
 * a lease that is lost, is picked up the same way.  Running it on its own
 * thread keeps the up-to-six-second DHCP wait off the boot path: net_init
 * returns immediately and the address arrives a moment later. */
static void net_autoconf_thread(void *arg) {
    (void)arg;
    for (;;) {
        for (netdev_t *d = devices; d; d = d->next) {
            if (!d->link_up || d->configured || d->dhcp_in_progress) continue;
            kinfo("net", "%s: link is up, asking DHCP for an address", d->name);
            net_dhcp(d, 6000);
        }
        sched_sleep_ms(2000);
    }
}

void net_init(void) {
    e1000_init();

    /* The Realtek gigabit and 2.5-gigabit family, which is the wired socket on
     * most desktop boards.  Before this the only Realtek card the system knew
     * was the RTL8139, which is the part before this family and has not been
     * fitted to a board in twenty years. */
    int realtek_before = netdev_count();
    r8169_init();

    /* With no such card in the machine, drive the driver against a model of
     * one instead.  It is the only way this driver gets exercised at all on a
     * machine without Realtek Ethernet, and it costs a few milliseconds once
     * at start-up; where a real card was found, the real card is the test and
     * this is skipped. */
    if (netdev_count() == realtek_before) r8169_selftest();

    rtl8139_init();

    if (!devices) {
        kdebug("net", "no supported network card found");
        return;
    }

    if (kthread_create("net", net_thread, NULL) < 0)
        kerr("net", "cannot start the network thread");

    /* Now that frames can be received, watch for a live link and configure it. */
    if (kthread_create("netcfg", net_autoconf_thread, NULL) < 0)
        kwarn("net", "cannot start the auto-configuration thread");
}
