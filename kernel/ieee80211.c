/* ieee80211.c - the 802.11 protocol.
 *
 * This is the half of Wi-Fi that is the same on every card from every vendor,
 * so it is written once here and every driver shares it: frame formats, the
 * management exchange that joins a network, and the conversion between 802.11
 * data frames and the Ethernet frames the IP stack already understands.
 *
 * Joining a network is four steps, and each has to finish before the next
 * begins: scan for what is there, authenticate (which for every modern network
 * is a formality that still has to happen), associate, and then exchange keys.
 * Only after the fourth does a data frame mean anything.
 *
 * The conversion at the bottom is what keeps the rest of the system unaware of
 * any of this.  An 802.11 data frame carries three or four addresses and an
 * LLC/SNAP header where Ethernet carries two addresses and a type; translating
 * between them means net.c, the IP stack and every program above it work over
 * Wi-Fi without knowing they are.
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "proc.h"
#include "klog.h"
#include "wifi.h"
#include "crypto.h"

/* ------------------------------------------------------------------ frames */

/* The frame control field: two bits of type, four of subtype, and the two
 * distribution-system bits that say which of the addresses means what. */
#define FC_TYPE_MGMT   0
#define FC_TYPE_CTRL   1
#define FC_TYPE_DATA   2

#define MGMT_ASSOC_REQ    0
#define MGMT_ASSOC_RESP   1
#define MGMT_PROBE_REQ    4
#define MGMT_PROBE_RESP   5
#define MGMT_BEACON       8
#define MGMT_DISASSOC    10
#define MGMT_AUTH        11
#define MGMT_DEAUTH      12

#define DATA_SUBTYPE_DATA     0
#define DATA_SUBTYPE_QOS_DATA 8

#define FC_TO_DS      0x0100
#define FC_FROM_DS    0x0200
#define FC_PROTECTED  0x4000

/* Information element numbers, which is how everything variable is carried. */
#define IE_SSID            0
#define IE_SUPPORTED_RATES 1
#define IE_DS_PARAMS       3
#define IE_RSN            48
#define IE_VENDOR        221

#define IEEE80211_HDR_LEN 24
#define LLC_SNAP_LEN      8

/* 802.11 puts everything on the wire little-endian, unlike the IP family. */
static inline u16 get16le(const void *p) {
    const u8 *b = p;
    return (u16)(b[0] | (b[1] << 8));
}
static inline void put16le(void *p, u16 v) {
    u8 *b = p;
    b[0] = (u8)v; b[1] = (u8)(v >> 8);
}
static inline u32 get32be(const void *p) {
    const u8 *b = p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

/* A frame under construction.  One buffer, because only one frame is ever
 * being built at a time and the sequence is driven from one thread. */
static u8 tx[2048];
static u16 sequence;

/* ------------------------------------------------- information elements */

/* Walk the variable part of a management frame looking for one element. */
static const u8 *find_ie(const u8 *ies, int len, u8 want, int *out_len) {
    int i = 0;
    while (i + 2 <= len) {
        u8 id = ies[i];
        u8 size = ies[i + 1];
        if (i + 2 + size > len) break;
        if (id == want) {
            if (out_len) *out_len = size;
            return ies + i + 2;
        }
        i += 2 + size;
    }
    return NULL;
}

/* What security a network uses, from the elements it advertises.
 *
 * An RSN element means WPA2 or WPA3, and which one is decided by the
 * authentication suite inside it: SAE is WPA3, and a network requiring
 * management frame protection with SAE is WPA3-only.  A vendor element with
 * Microsoft's identifier and a subtype of 1 is the original WPA.  Neither
 * present, and the privacy bit set, means WEP. */
static wifi_security_t security_from_ies(const u8 *ies, int len, u16 capability) {
    int rsn_len = 0;
    const u8 *rsn = find_ie(ies, len, IE_RSN, &rsn_len);

    if (rsn && rsn_len >= 8) {
        /* version(2) group cipher(4) pairwise count(2) pairwise suites(4n)
         * then the authentication count and suites. */
        int offset = 2 + 4;
        if (offset + 2 > rsn_len) return WIFI_SECURITY_WPA2;
        u16 pairwise_count = get16le(rsn + offset);
        offset += 2 + 4 * pairwise_count;
        if (offset + 2 > rsn_len) return WIFI_SECURITY_WPA2;

        u16 auth_count = get16le(rsn + offset);
        offset += 2;

        for (int i = 0; i < auth_count && offset + 4 <= rsn_len; i++, offset += 4) {
            u32 suite = get32be(rsn + offset);
            /* 00-0F-AC:8 is SAE, which is WPA3. */
            if (suite == 0x000FAC08 || suite == 0x000FAC09)
                return WIFI_SECURITY_WPA3;
        }
        return WIFI_SECURITY_WPA2;
    }

    int i = 0;
    while (i + 2 <= len) {
        u8 id = ies[i], size = ies[i + 1];
        if (i + 2 + size > len) break;
        if (id == IE_VENDOR && size >= 4) {
            const u8 *v = ies + i + 2;
            if (v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 0x01)
                return WIFI_SECURITY_WPA;
        }
        i += 2 + size;
    }

    return (capability & 0x0010) ? WIFI_SECURITY_WEP : WIFI_SECURITY_OPEN;
}

/* ------------------------------------------------------------- management */

static u8 *frame_start(wifi_device_t *dev, u8 type, u8 subtype,
                       const mac_t *addr1, const mac_t *addr2,
                       const mac_t *addr3) {
    u16 fc = (u16)((type << 2) | (subtype << 4));
    put16le(tx + 0, fc);
    put16le(tx + 2, 0);                       /* duration, left to the card */
    memcpy(tx + 4, addr1->addr, ETH_ALEN);
    memcpy(tx + 10, addr2->addr, ETH_ALEN);
    memcpy(tx + 16, addr3->addr, ETH_ALEN);
    put16le(tx + 22, (u16)(sequence++ << 4));
    (void)dev;
    return tx + IEEE80211_HDR_LEN;
}

static const mac_t broadcast = { { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };

static void send_probe_request(wifi_device_t *dev, const char *ssid) {
    u8 *p = frame_start(dev, FC_TYPE_MGMT, MGMT_PROBE_REQ,
                        &broadcast, &dev->mac, &broadcast);

    /* An empty SSID element asks every network to answer; a filled one asks a
     * particular network, which is the only way to find a hidden one. */
    size_t ssid_len = ssid ? strlen(ssid) : 0;
    if (ssid_len > WIFI_SSID_MAX) ssid_len = WIFI_SSID_MAX;
    *p++ = IE_SSID;
    *p++ = (u8)ssid_len;
    if (ssid_len) { memcpy(p, ssid, ssid_len); p += ssid_len; }

    /* The rates every 802.11g radio supports, which is enough to be answered. */
    static const u8 rates[] = { 0x02, 0x04, 0x0B, 0x16, 0x0C, 0x12, 0x18, 0x24 };
    *p++ = IE_SUPPORTED_RATES;
    *p++ = sizeof rates;
    memcpy(p, rates, sizeof rates);
    p += sizeof rates;

    if (dev->driver->transmit)
        dev->driver->transmit(dev, tx, (int)(p - tx));
}

static void send_auth(wifi_device_t *dev) {
    u8 *p = frame_start(dev, FC_TYPE_MGMT, MGMT_AUTH,
                        &dev->bssid, &dev->mac, &dev->bssid);
    put16le(p, 0); p += 2;        /* open system: the only algorithm WPA uses */
    put16le(p, 1); p += 2;        /* sequence number one                      */
    put16le(p, 0); p += 2;        /* status: success, on a request            */

    if (dev->driver->transmit)
        dev->driver->transmit(dev, tx, (int)(p - tx));
}

static void send_assoc_request(wifi_device_t *dev) {
    u8 *p = frame_start(dev, FC_TYPE_MGMT, MGMT_ASSOC_REQ,
                        &dev->bssid, &dev->mac, &dev->bssid);

    u16 capability = 0x0001;                          /* an infrastructure station */
    if (dev->security != WIFI_SECURITY_OPEN) capability |= 0x0010;
    put16le(p, capability); p += 2;
    put16le(p, 10); p += 2;                           /* listen interval */

    size_t ssid_len = strlen(dev->ssid);
    *p++ = IE_SSID;
    *p++ = (u8)ssid_len;
    memcpy(p, dev->ssid, ssid_len);
    p += ssid_len;

    static const u8 rates[] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
    *p++ = IE_SUPPORTED_RATES;
    *p++ = sizeof rates;
    memcpy(p, rates, sizeof rates);
    p += sizeof rates;

    /* For WPA2 the request has to say which ciphers we intend to use, and the
     * access point checks it against what it advertised. */
    if (dev->security == WIFI_SECURITY_WPA2 || dev->security == WIFI_SECURITY_WPA3) {
        *p++ = IE_RSN;
        u8 *len_field = p++;
        u8 *start = p;

        put16le(p, 1); p += 2;                        /* version one          */
        p[0]=0x00; p[1]=0x0F; p[2]=0xAC; p[3]=0x04; p += 4;   /* group: CCMP  */
        put16le(p, 1); p += 2;                        /* one pairwise cipher  */
        p[0]=0x00; p[1]=0x0F; p[2]=0xAC; p[3]=0x04; p += 4;   /* CCMP         */
        put16le(p, 1); p += 2;                        /* one auth suite       */
        if (dev->security == WIFI_SECURITY_WPA3) {
            p[0]=0x00; p[1]=0x0F; p[2]=0xAC; p[3]=0x08; p += 4;   /* SAE      */
        } else {
            p[0]=0x00; p[1]=0x0F; p[2]=0xAC; p[3]=0x02; p += 4;   /* PSK      */
        }
        put16le(p, 0); p += 2;                        /* no capabilities      */

        *len_field = (u8)(p - start);
    }

    if (dev->driver->transmit)
        dev->driver->transmit(dev, tx, (int)(p - tx));
}

/* ---------------------------------------------------------- what arrives */

/* Record a network a beacon or probe response described. */
static void note_network(wifi_device_t *dev, const u8 *frame, int len,
                         s8 signal_dbm) {
    if (len < IEEE80211_HDR_LEN + 12) return;

    mac_t bssid;
    memcpy(bssid.addr, frame + 16, ETH_ALEN);

    /* timestamp(8) beacon interval(2) capability(2), then the elements. */
    const u8 *body = frame + IEEE80211_HDR_LEN;
    u16 capability = get16le(body + 10);
    const u8 *ies = body + 12;
    int ies_len = len - IEEE80211_HDR_LEN - 12;

    int ssid_len = 0;
    const u8 *ssid = find_ie(ies, ies_len, IE_SSID, &ssid_len);
    if (ssid_len > WIFI_SSID_MAX) ssid_len = WIFI_SSID_MAX;

    int ds_len = 0;
    const u8 *ds = find_ie(ies, ies_len, IE_DS_PARAMS, &ds_len);
    u8 channel = (ds && ds_len >= 1) ? ds[0] : dev->channel;

    /* Merge with what is already known rather than adding a duplicate: an
     * access point beacons every hundred milliseconds. */
    for (int i = 0; i < dev->scan_count; i++) {
        if (memcmp(dev->scan[i].bssid.addr, bssid.addr, ETH_ALEN)) continue;
        if (signal_dbm > dev->scan[i].signal_dbm)
            dev->scan[i].signal_dbm = signal_dbm;
        /* A probe response can fill in an SSID a beacon hid. */
        if (!dev->scan[i].ssid[0] && ssid && ssid_len) {
            memcpy(dev->scan[i].ssid, ssid, (size_t)ssid_len);
            dev->scan[i].ssid[ssid_len] = 0;
            dev->scan[i].hidden = false;
        }
        return;
    }

    if (dev->scan_count >= WIFI_MAX_SCAN) return;

    wifi_network_t *n = &dev->scan[dev->scan_count++];
    memset(n, 0, sizeof *n);
    n->bssid = bssid;
    n->channel = channel;
    n->signal_dbm = signal_dbm;
    n->security = security_from_ies(ies, ies_len, capability);

    if (ssid && ssid_len && ssid[0]) {
        memcpy(n->ssid, ssid, (size_t)ssid_len);
        n->ssid[ssid_len] = 0;
    } else {
        n->hidden = true;
    }
}

/* Implemented in wpa.c: the four-way handshake and the frame cipher. */
bool wpa_handle_eapol(wifi_device_t *dev, const u8 *data, int len);
bool wpa_decrypt_frame(wifi_device_t *dev, u8 *frame, int *len);
int  wpa_encrypt_frame(wifi_device_t *dev, u8 *frame, int len, int capacity);
bool wpa_is_ready(wifi_device_t *dev);

/* An 802.11 data frame becomes an Ethernet one and goes up the stack. */
static void deliver_data(wifi_device_t *dev, u8 *frame, int len) {
    u16 fc = get16le(frame);
    int header_len = IEEE80211_HDR_LEN;

    /* A QoS frame carries two more bytes before the payload. */
    if (((fc >> 4) & 0x0F) == DATA_SUBTYPE_QOS_DATA) header_len += 2;

    if (fc & FC_PROTECTED) {
        if (!wpa_decrypt_frame(dev, frame, &len)) {
            dev->net.rx_dropped++;
            return;
        }
    }

    if (len < header_len + LLC_SNAP_LEN) return;

    /* Which address is which depends on the direction bits.  From an access
     * point to us: address one is us, two is the access point, three is who
     * actually sent it. */
    const u8 *dest, *src;
    if (fc & FC_FROM_DS) { dest = frame + 4;  src = frame + 16; }
    else                 { dest = frame + 4;  src = frame + 10; }

    const u8 *snap = frame + header_len;
    /* AA AA 03 00 00 00 followed by the ethertype. */
    if (snap[0] != 0xAA || snap[1] != 0xAA || snap[2] != 0x03) return;

    const u8 *payload = snap + LLC_SNAP_LEN;
    int payload_len = len - header_len - LLC_SNAP_LEN;
    if (payload_len <= 0 || payload_len > ETH_MTU) return;

    static u8 ethernet[ETH_FRAME_MAX];
    memcpy(ethernet + 0, dest, ETH_ALEN);
    memcpy(ethernet + 6, src, ETH_ALEN);
    ethernet[12] = snap[6];
    ethernet[13] = snap[7];
    memcpy(ethernet + ETH_HLEN, payload, (size_t)payload_len);

    net_receive(&dev->net, ethernet, ETH_HLEN + payload_len);
}

/* Every frame a driver receives arrives here. */
void ieee80211_receive(wifi_device_t *dev, u8 *frame, int len, s8 signal_dbm) {
    if (len < IEEE80211_HDR_LEN) return;

    u16 fc = get16le(frame);
    u8 type = (u8)((fc >> 2) & 0x03);
    u8 subtype = (u8)((fc >> 4) & 0x0F);

    if (type == FC_TYPE_MGMT) {
        switch (subtype) {
        case MGMT_BEACON:
        case MGMT_PROBE_RESP:
            note_network(dev, frame, len, signal_dbm);
            break;

        case MGMT_AUTH:
            if (dev->state == WIFI_AUTHENTICATING && len >= IEEE80211_HDR_LEN + 6) {
                u16 status = get16le(frame + IEEE80211_HDR_LEN + 4);
                if (status == 0) {
                    dev->state = WIFI_ASSOCIATING;
                    send_assoc_request(dev);
                } else {
                    kwarn("wifi", "%s: the access point refused authentication "
                                  "(status %u)", dev->name, status);
                    dev->state = WIFI_DOWN;
                }
            }
            break;

        case MGMT_ASSOC_RESP:
            if (dev->state == WIFI_ASSOCIATING && len >= IEEE80211_HDR_LEN + 6) {
                u16 status = get16le(frame + IEEE80211_HDR_LEN + 2);
                if (status == 0) {
                    /* An open network is usable now; anything else has to
                     * exchange keys before a data frame means anything. */
                    if (dev->security == WIFI_SECURITY_OPEN) {
                        dev->state = WIFI_CONNECTED;
                        dev->net.link_up = true;
                    } else {
                        dev->state = WIFI_HANDSHAKING;
                    }
                } else {
                    kwarn("wifi", "%s: association refused (status %u)",
                          dev->name, status);
                    dev->state = WIFI_DOWN;
                }
            }
            break;

        case MGMT_DEAUTH:
        case MGMT_DISASSOC:
            if (dev->state != WIFI_DOWN) {
                kwarn("wifi", "%s: the access point disconnected us", dev->name);
                dev->state = WIFI_DOWN;
                dev->net.link_up = false;
            }
            break;

        default:
            break;
        }
        return;
    }

    if (type != FC_TYPE_DATA) return;
    if (subtype != DATA_SUBTYPE_DATA && subtype != DATA_SUBTYPE_QOS_DATA) return;

    /* The key exchange travels in data frames of its own, before there is a
     * key, so it has to be picked out before anything is decrypted. */
    int header_len = IEEE80211_HDR_LEN;
    if (subtype == DATA_SUBTYPE_QOS_DATA) header_len += 2;

    if (!(fc & FC_PROTECTED) && len >= header_len + LLC_SNAP_LEN) {
        const u8 *snap = frame + header_len;
        if (snap[0] == 0xAA && snap[1] == 0xAA && snap[2] == 0x03 &&
            snap[6] == 0x88 && snap[7] == 0x8E) {          /* EAPOL */
            wpa_handle_eapol(dev, snap + LLC_SNAP_LEN,
                             len - header_len - LLC_SNAP_LEN);
            return;
        }
    }

    deliver_data(dev, frame, len);
}

/* --------------------------------------------------------------- transmit */

/* An Ethernet frame from the IP stack becomes an 802.11 one and goes out. */
static int wifi_transmit(netdev_t *net, const void *frame, int len) {
    wifi_device_t *dev = net->ctx;
    if (dev->state != WIFI_CONNECTED || len < ETH_HLEN) return -1;

    const u8 *eth = frame;
    int payload_len = len - ETH_HLEN;
    if (payload_len < 0 || payload_len > ETH_MTU) return -1;

    mac_t dest;
    memcpy(dest.addr, eth, ETH_ALEN);

    /* Going out through an access point: address one is the access point,
     * two is us, three is where it is really going. */
    u16 fc = (u16)((FC_TYPE_DATA << 2) | (DATA_SUBTYPE_DATA << 4) | FC_TO_DS);
    put16le(tx + 0, fc);
    put16le(tx + 2, 0);
    memcpy(tx + 4, dev->bssid.addr, ETH_ALEN);
    memcpy(tx + 10, dev->mac.addr, ETH_ALEN);
    memcpy(tx + 16, dest.addr, ETH_ALEN);
    put16le(tx + 22, (u16)(sequence++ << 4));

    u8 *p = tx + IEEE80211_HDR_LEN;
    p[0] = 0xAA; p[1] = 0xAA; p[2] = 0x03;
    p[3] = 0x00; p[4] = 0x00; p[5] = 0x00;
    p[6] = eth[12]; p[7] = eth[13];
    memcpy(p + LLC_SNAP_LEN, eth + ETH_HLEN, (size_t)payload_len);

    int total = IEEE80211_HDR_LEN + LLC_SNAP_LEN + payload_len;

    if (dev->security != WIFI_SECURITY_OPEN) {
        if (!wpa_is_ready(dev)) return -1;
        total = wpa_encrypt_frame(dev, tx, total, (int)sizeof tx);
        if (total <= 0) return -1;
    }

    if (!dev->driver->transmit) return -1;
    return dev->driver->transmit(dev, tx, total);
}

static void wifi_net_poll(netdev_t *net) {
    wifi_device_t *dev = net->ctx;
    if (dev->driver->poll) dev->driver->poll(dev);
}

/* Give the device its Ethernet face, so the IP stack can use it. */
void ieee80211_attach(wifi_device_t *dev) {
    memset(&dev->net, 0, sizeof dev->net);
    strlcpy(dev->net.name, dev->name, sizeof dev->net.name);
    strlcpy(dev->net.model, dev->model, sizeof dev->net.model);
    dev->net.mac = dev->mac;
    dev->net.ctx = dev;
    dev->net.transmit = wifi_transmit;
    dev->net.poll = wifi_net_poll;
    dev->net.link_up = false;
    netdev_register(&dev->net);
}

/* ------------------------------------------------------------------- scan */

/* The channels worth looking at.  One to eleven are legal everywhere; twelve
 * and thirteen are not legal in every country, and a station that only listens
 * on them is doing nothing a regulator objects to. */
static const u8 channels_2g[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };

int wifi_scan(wifi_device_t *dev, int timeout_ms) {
    if (!dev || !dev->radio_up) return -1;

    dev->scan_count = 0;
    dev->state = WIFI_SCANNING;

    /* Sit on each channel long enough to hear a beacon, which access points
     * send about ten times a second. */
    int per_channel = timeout_ms / (int)ARRAY_LEN(channels_2g);
    if (per_channel < 120) per_channel = 120;

    for (size_t i = 0; i < ARRAY_LEN(channels_2g); i++) {
        if (dev->driver->set_channel &&
            !dev->driver->set_channel(dev, channels_2g[i]))
            continue;
        dev->channel = channels_2g[i];

        send_probe_request(dev, NULL);

        u64 until = g_uptime_ms + (u64)per_channel;
        while (g_uptime_ms < until) {
            if (dev->driver->poll) dev->driver->poll(dev);
            sched_sleep_ms(2);
        }
    }

    dev->state = WIFI_DOWN;
    dev->scan_finished_ms = g_uptime_ms;
    kinfo("wifi", "%s: %d network(s) found", dev->name, dev->scan_count);
    return dev->scan_count;
}

/* ------------------------------------------------------------------- join */

void wpa_begin(wifi_device_t *dev, const char *passphrase);

int wifi_connect(wifi_device_t *dev, const char *ssid, const char *passphrase,
                 int timeout_ms) {
    if (!dev || !dev->radio_up || !ssid) return -1;

    /* Find it in the last scan, or scan again if there is nothing to look in. */
    const wifi_network_t *target = NULL;
    for (int attempt = 0; attempt < 2 && !target; attempt++) {
        for (int i = 0; i < dev->scan_count; i++)
            if (!strcmp(dev->scan[i].ssid, ssid)) { target = &dev->scan[i]; break; }
        if (!target && attempt == 0) wifi_scan(dev, 3000);
    }

    if (!target) {
        kwarn("wifi", "%s: no network called \"%s\" is in range", dev->name, ssid);
        return -1;
    }

    if (target->security == WIFI_SECURITY_WEP) {
        kwarn("wifi", "%s: \"%s\" uses WEP, which is broken and not implemented",
              dev->name, ssid);
        return -1;
    }
    if (target->security != WIFI_SECURITY_OPEN && (!passphrase || !*passphrase)) {
        kwarn("wifi", "%s: \"%s\" is protected and needs a passphrase",
              dev->name, ssid);
        return -1;
    }

    strlcpy(dev->ssid, ssid, sizeof dev->ssid);
    dev->bssid = target->bssid;
    dev->channel = target->channel;
    dev->security = target->security;
    dev->signal_dbm = target->signal_dbm;

    if (dev->driver->set_channel) dev->driver->set_channel(dev, dev->channel);
    if (dev->security != WIFI_SECURITY_OPEN) wpa_begin(dev, passphrase);

    dev->state = WIFI_AUTHENTICATING;
    send_auth(dev);

    u64 deadline = g_uptime_ms + (u64)timeout_ms;
    u64 retry = g_uptime_ms + 800;
    wifi_state_t last = dev->state;

    while (g_uptime_ms < deadline && dev->state != WIFI_CONNECTED &&
           dev->state != WIFI_DOWN) {
        if (dev->driver->poll) dev->driver->poll(dev);

        /* Management frames go unacknowledged more often than not; resend
         * whichever step has stopped making progress. */
        if (g_uptime_ms >= retry) {
            if (dev->state == WIFI_AUTHENTICATING) send_auth(dev);
            else if (dev->state == WIFI_ASSOCIATING) send_assoc_request(dev);
            retry = g_uptime_ms + 800;
        }
        if (dev->state != last) { last = dev->state; retry = g_uptime_ms + 800; }

        sched_sleep_ms(2);
    }

    if (dev->state != WIFI_CONNECTED) {
        kwarn("wifi", "%s: could not join \"%s\" (%s)", dev->name, ssid,
              wifi_state_name(dev->state));
        dev->state = WIFI_DOWN;
        dev->net.link_up = false;
        return -1;
    }

    dev->net.link_up = true;
    dev->net.link_speed_mbps = 54;        /* what the rates offered promise */
    kinfo("wifi", "%s: joined \"%s\" on channel %u, %s", dev->name, ssid,
          dev->channel, wifi_security_name(dev->security));
    return 0;
}

void wifi_disconnect(wifi_device_t *dev) {
    if (!dev || dev->state == WIFI_DOWN) return;

    if (dev->driver->transmit) {
        u8 *p = frame_start(dev, FC_TYPE_MGMT, MGMT_DEAUTH,
                            &dev->bssid, &dev->mac, &dev->bssid);
        put16le(p, 3); p += 2;            /* leaving, and meaning to */
        dev->driver->transmit(dev, tx, (int)(p - tx));
    }

    dev->state = WIFI_DOWN;
    dev->net.link_up = false;
    dev->ssid[0] = 0;
    kinfo("wifi", "%s: disconnected", dev->name);
}

/* --------------------------------------------------------- switching it off
 *
 * Turning a radio off is not just refusing to use it: the hardware keeps
 * transmitting beacons and probe responses on its own until it is stopped, so
 * off means stopped.  Turning it back on means going through the whole
 * start-up again, including the firmware load, because stopping it undid that.
 */
bool wifi_set_enabled(wifi_device_t *dev, bool on) {
    if (!dev) return false;
    if (dev->enabled == on && dev->radio_up == on) return true;

    dev->enabled = on;

    if (!on) {
        if (dev->state != WIFI_DOWN) wifi_disconnect(dev);
        if (dev->driver && dev->driver->stop) dev->driver->stop(dev);
        dev->radio_up = false;
        dev->scan_count = 0;
        kinfo("wifi", "%s: switched off", dev->name);
        return true;
    }

    if (!dev->driver || !dev->driver->start) return false;
    bool up = dev->driver->start(dev);
    kinfo("wifi", "%s: switched on%s", dev->name,
          up ? "" : ", but the radio did not come up");
    return up;
}
