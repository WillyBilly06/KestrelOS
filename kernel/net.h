/* net.h - the network stack.
 *
 * A driver hands whole Ethernet frames up and takes whole frames back down;
 * everything above that is protocol.  Keeping the boundary at the frame means
 * adding a second card is a driver and nothing else.
 *
 * Wi-Fi is not here and cannot be: an Intel, MediaTek or Realtek wireless card
 * needs a vendor-specific driver, signed firmware the vendor distributes, an
 * 802.11 state machine and a WPA supplicant, and each vendor family is a large
 * independent project on its own.  Wired Ethernet is a different matter - the
 * controllers are documented, and the ones here are the ones actually found in
 * machines and in virtual machines.
 */
#ifndef KESTREL_NET_H
#define KESTREL_NET_H

#include "kernel.h"

#define ETH_ALEN      6
#define ETH_HLEN      14
#define ETH_MTU       1500
#define ETH_FRAME_MAX 1518

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

typedef struct { u8 addr[ETH_ALEN]; } mac_t;
typedef u32 ipv4_t;                        /* host byte order throughout */

#define IPV4(a, b, c, d) \
    (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

/* ------------------------------------------------------------------ device */

typedef struct netdev {
    char   name[16];
    char   model[48];
    mac_t  mac;
    bool   link_up;
    u32    link_speed_mbps;

    /* Configuration, whether from DHCP or set by hand. */
    ipv4_t ip, netmask, gateway, dns;
    bool   configured;
    bool   dhcp_in_progress;

    /* Counters, which are what a network problem is diagnosed from. */
    u64 rx_packets, tx_packets;
    u64 rx_bytes, tx_bytes;
    u64 rx_dropped, tx_dropped, rx_errors, tx_errors;

    /* Filled in by the driver. */
    void *ctx;
    int (*transmit)(struct netdev *dev, const void *frame, int len);
    void (*poll)(struct netdev *dev);      /* called to collect received frames */

    struct netdev *next;
} netdev_t;

void      netdev_register(netdev_t *dev);
netdev_t *netdev_first(void);
netdev_t *netdev_by_name(const char *name);
int       netdev_count(void);

/* A driver calls this with each frame it receives. */
void net_receive(netdev_t *dev, const void *frame, int len);

/* ------------------------------------------------------------------ stack */

void net_init(void);

/* Bring an interface up with a fixed address, or ask DHCP for one. */
int  net_configure(netdev_t *dev, ipv4_t ip, ipv4_t mask, ipv4_t gw, ipv4_t dns);
int  net_dhcp(netdev_t *dev, int timeout_ms);

/* Resolve a name.  Returns 0 when it could not be resolved. */
ipv4_t net_resolve(const char *host, int timeout_ms);

/* Send echo requests and report what came back.  `times_ms` receives one
 * round-trip time per reply, in milliseconds. */
int net_ping(ipv4_t dest, int count, int timeout_ms, u32 *times_ms);

/* ARP, exposed so `net arp` can show the table. */
typedef struct {
    ipv4_t ip;
    mac_t  mac;
    u64    seen_ms;
    bool   valid;
} arp_entry_t;

int  net_arp_snapshot(arp_entry_t *out, int max);
bool net_arp_lookup(netdev_t *dev, ipv4_t ip, mac_t *out, int timeout_ms);

/* ------------------------------------------------------------------- UDP */

/* A minimal socket layer: enough for DHCP, DNS and a program that wants to
 * send a datagram.  One handler per bound port. */
typedef void (*udp_handler_t)(void *ctx, ipv4_t from, u16 from_port,
                              const void *data, int len);

int  udp_bind(u16 port, udp_handler_t handler, void *ctx);
void udp_unbind(u16 port);
int  udp_send(netdev_t *dev, ipv4_t dest, u16 dest_port, u16 src_port,
              const void *data, int len);

/* ------------------------------------------------------------------- TCP */

/* Client-side only: connect out, exchange data, close.  Each call blocks the
 * calling thread until it finishes or its timeout runs out.
 *
 * tcp_connect returns a handle or a negative error; the data calls return how
 * many bytes moved, or negative when the connection is gone. */
int  tcp_connect(ipv4_t ip, u16 port, int timeout_ms);
int  tcp_send_data(int handle, const void *data, int len, int timeout_ms);
int  tcp_receive_data(int handle, void *buf, int len, int timeout_ms);
void tcp_close(int handle);
bool tcp_is_open(int handle);

/* Why a connection stopped working, in words - for a caller reporting a
 * failure to someone who has to act on it. */
const char *tcp_state_name(int handle);

/* Called by the IPv4 layer for each segment that arrives. */
void tcp_receive(netdev_t *dev, ipv4_t from, const u8 *p, int len);

/* -------------------------------------------------------------- formatting */

void ipv4_format(ipv4_t ip, char *buf, size_t cap);
bool ipv4_parse(const char *text, ipv4_t *out);
void mac_format(const mac_t *mac, char *buf, size_t cap);

/* ---------------------------------------------------------------- drivers */

void e1000_init(void);
void rtl8139_init(void);
void virtio_net_init(void);

#endif
