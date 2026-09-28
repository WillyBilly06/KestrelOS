/* wpa.c - the WPA2 four-way handshake and CCMP.
 *
 * Joining a protected network means proving to the access point that you know
 * the passphrase, without either side sending it, and ending up with a fresh
 * key that is different every time.  That is what the four-way handshake does,
 * and it is worth following because every step exists for a reason:
 *
 *   The passphrase and the network's name become a 256-bit key by running
 *   PBKDF2 four thousand times.  Both sides can compute it; it is the same
 *   every time for a given network, which is exactly why it is never used
 *   directly.
 *
 *   Message one: the access point sends a random number.  We send one back in
 *   message two.  Both sides now mix the shared key, both random numbers and
 *   both addresses into a session key that neither side chose alone and that
 *   is different on every connection.
 *
 *   Message two also carries an integrity code computed with that session key.
 *   The access point can only check it if it derived the same key, which it
 *   only could if it knew the passphrase - so message two is the proof, and a
 *   wrong passphrase fails here.
 *
 *   Messages three and four confirm in the other direction and install the key.
 *
 * After that every data frame is encrypted with AES-CCM: the header is
 * authenticated, the payload is encrypted and authenticated, and a packet
 * number that never repeats stops an old frame being replayed.
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "cpu.h"
#include "wifi.h"
#include "crypto.h"

/* ------------------------------------------------------------------ EAPOL */

#define EAPOL_VERSION 2
#define EAPOL_TYPE_KEY 3

/* Key information bits, in the field that says what a message is for. */
#define KEY_INFO_PAIRWISE  0x0008
#define KEY_INFO_INSTALL   0x0040
#define KEY_INFO_ACK       0x0080
#define KEY_INFO_MIC       0x0100
#define KEY_INFO_SECURE    0x0200
#define KEY_INFO_ENCRYPTED 0x1000

#define KEY_DESC_VERSION_MASK 0x0007

#define NONCE_LEN 32
#define PTK_LEN   48        /* 16 confirmation, 16 encryption, 16 temporal */
#define MIC_LEN   16

typedef struct {
    bool  active;
    bool  ready;              /* the key is installed and frames can flow */

    u8    pmk[32];            /* from the passphrase and the network name */
    u8    ptk[PTK_LEN];       /* this connection only                     */
    u8    anonce[NONCE_LEN];  /* the access point's random number         */
    u8    snonce[NONCE_LEN];  /* ours                                     */

    /* CCMP state.  The packet number goes up for every frame and is never
     * allowed to repeat, because a repeat would let an old frame be replayed. */
    u64   tx_packet_number;
    u64   rx_packet_number;
    aes_t aes;
    bool  have_key;
} wpa_t;

/* One supplicant per device; there are never many devices. */
#define MAX_SUPPLICANTS 4
static struct { wifi_device_t *dev; wpa_t state; } supplicants[MAX_SUPPLICANTS];

static wpa_t *state_for(wifi_device_t *dev, bool create) {
    for (int i = 0; i < MAX_SUPPLICANTS; i++)
        if (supplicants[i].dev == dev) return &supplicants[i].state;
    if (!create) return NULL;
    for (int i = 0; i < MAX_SUPPLICANTS; i++)
        if (!supplicants[i].dev) {
            supplicants[i].dev = dev;
            memset(&supplicants[i].state, 0, sizeof supplicants[i].state);
            return &supplicants[i].state;
        }
    return NULL;
}

static inline u16 get16be(const void *p) {
    const u8 *b = p;
    return (u16)((b[0] << 8) | b[1]);
}
static inline void put16be(void *p, u16 v) {
    u8 *b = p;
    b[0] = (u8)(v >> 8); b[1] = (u8)v;
}
static inline u16 get16le(const void *p) {
    const u8 *b = p;
    return (u16)(b[0] | (b[1] << 8));
}
static inline void put16le(void *p, u16 v) {
    u8 *b = p;
    b[0] = (u8)v; b[1] = (u8)(v >> 8);
}

/* ------------------------------------------------------- key derivation */

/* The pseudo-random function 802.11 specifies: HMAC over a label, the data and
 * a counter, repeated until enough bytes have come out. */
static void prf(const u8 *key, int key_len, const char *label,
                const u8 *data, int data_len, u8 *out, int out_len) {
    u8 counter = 0;
    int produced = 0;

    while (produced < out_len) {
        const u8 *parts[4];
        size_t lens[4];

        parts[0] = (const u8 *)label;  lens[0] = strlen(label) + 1;  /* with its zero */
        parts[1] = data;               lens[1] = (size_t)data_len;
        parts[2] = &counter;           lens[2] = 1;

        u8 digest[SHA1_SIZE];
        hmac_sha1_vector(key, (size_t)key_len, 3, parts, lens, digest);

        int take = out_len - produced;
        if (take > SHA1_SIZE) take = SHA1_SIZE;
        memcpy(out + produced, digest, (size_t)take);
        produced += take;
        counter++;
    }
}

/* The session key, from the shared key, both random numbers and both
 * addresses.  The addresses and nonces go in sorted order so that both ends
 * derive the same thing without having to agree who is who. */
void wpa_derive_ptk(const u8 pmk[32], const mac_t *a, const mac_t *b,
                    const u8 nonce_a[32], const u8 nonce_b[32], u8 ptk[48]) {
    u8 data[2 * ETH_ALEN + 2 * NONCE_LEN];
    u8 *p = data;

    if (memcmp(a->addr, b->addr, ETH_ALEN) < 0) {
        memcpy(p, a->addr, ETH_ALEN); p += ETH_ALEN;
        memcpy(p, b->addr, ETH_ALEN); p += ETH_ALEN;
    } else {
        memcpy(p, b->addr, ETH_ALEN); p += ETH_ALEN;
        memcpy(p, a->addr, ETH_ALEN); p += ETH_ALEN;
    }

    if (memcmp(nonce_a, nonce_b, NONCE_LEN) < 0) {
        memcpy(p, nonce_a, NONCE_LEN); p += NONCE_LEN;
        memcpy(p, nonce_b, NONCE_LEN); p += NONCE_LEN;
    } else {
        memcpy(p, nonce_b, NONCE_LEN); p += NONCE_LEN;
        memcpy(p, nonce_a, NONCE_LEN); p += NONCE_LEN;
    }

    prf(pmk, 32, "Pairwise key expansion", data, (int)sizeof data, ptk, PTK_LEN);
}

static void derive_ptk(wpa_t *wpa, const mac_t *own, const mac_t *ap) {
    wpa_derive_ptk(wpa->pmk, own, ap, wpa->snonce, wpa->anonce, wpa->ptk);
}

/* ----------------------------------------------------------- randomness */

/* A nonce has to be unpredictable: if it were guessable, the session key would
 * be too.  The generator in random.c is the machine's one source - the
 * processor's own where it has one, and a pool stirred from interrupt timing
 * where it does not - so this uses that rather than keeping a second, weaker
 * copy of the same idea. */

/* ----------------------------------------------------------- the exchange */

void wpa_begin(wifi_device_t *dev, const char *passphrase) {
    wpa_t *wpa = state_for(dev, true);
    if (!wpa) return;

    memset(wpa, 0, sizeof *wpa);
    wpa->active = true;

    /* The shared key: the passphrase and the network's name, four thousand
     * rounds deep.  This is the expensive step, and it is expensive on purpose. */
    pbkdf2_sha1(passphrase, (const u8 *)dev->ssid, strlen(dev->ssid),
                4096, wpa->pmk, sizeof wpa->pmk);

    random_bytes(wpa->snonce, sizeof wpa->snonce);
}

bool wpa_is_ready(wifi_device_t *dev) {
    wpa_t *wpa = state_for(dev, false);
    return wpa && wpa->ready;
}

/* Send one EAPOL key message inside a data frame. */
static void send_eapol(wifi_device_t *dev, wpa_t *wpa, u16 key_info,
                       const u8 *key_data, int key_data_len, u16 replay_high,
                       const u8 *replay) {
    static u8 frame[512];

    /* The 802.11 header, going out through the access point. */
    put16le(frame + 0, (u16)((2 << 2) | (0 << 4) | 0x0100));   /* data, to DS */
    put16le(frame + 2, 0);
    memcpy(frame + 4, dev->bssid.addr, ETH_ALEN);
    memcpy(frame + 10, dev->mac.addr, ETH_ALEN);
    memcpy(frame + 16, dev->bssid.addr, ETH_ALEN);
    put16le(frame + 22, 0);

    u8 *p = frame + 24;
    p[0] = 0xAA; p[1] = 0xAA; p[2] = 0x03;
    p[3] = 0x00; p[4] = 0x00; p[5] = 0x00;
    p[6] = 0x88; p[7] = 0x8E;                   /* EAPOL */
    p += 8;

    u8 *eapol = p;
    eapol[0] = EAPOL_VERSION;
    eapol[1] = EAPOL_TYPE_KEY;
    put16be(eapol + 2, (u16)(95 + key_data_len));

    u8 *body = eapol + 4;
    memset(body, 0, 95);
    body[0] = 2;                                 /* RSN key descriptor */
    put16be(body + 1, key_info);
    put16be(body + 3, 16);                       /* key length: CCMP */

    /* The replay counter is echoed back exactly as it arrived, which is how
     * the access point matches a reply to its request. */
    if (replay) memcpy(body + 5, replay, 8);
    else put16be(body + 11, replay_high);

    memcpy(body + 13, wpa->snonce, NONCE_LEN);

    put16be(body + 93, (u16)key_data_len);
    if (key_data_len) memcpy(body + 95, key_data, (size_t)key_data_len);

    /* The integrity code covers the whole EAPOL message with the code field
     * zeroed, using the first sixteen bytes of the session key. */
    if (key_info & KEY_INFO_MIC) {
        int total = 4 + 95 + key_data_len;
        u8 mic[SHA1_SIZE];
        hmac_sha1(wpa->ptk, 16, eapol, (size_t)total, mic);
        memcpy(body + 77, mic, MIC_LEN);
    }

    int frame_len = (int)(body + 95 + key_data_len - frame);
    if (dev->driver->transmit) dev->driver->transmit(dev, frame, frame_len);
}

bool wpa_handle_eapol(wifi_device_t *dev, const u8 *data, int len) {
    wpa_t *wpa = state_for(dev, false);
    if (!wpa || !wpa->active || len < 99) return false;

    if (data[1] != EAPOL_TYPE_KEY) return false;

    const u8 *body = data + 4;
    u16 key_info = get16be(body + 1);
    const u8 *replay = body + 5;
    const u8 *nonce = body + 13;

    /* Message one: a random number and a request for ours.  It carries no
     * integrity code, because there is no key yet to compute one with. */
    if ((key_info & KEY_INFO_ACK) && !(key_info & KEY_INFO_MIC)) {
        memcpy(wpa->anonce, nonce, NONCE_LEN);
        derive_ptk(wpa, &dev->mac, &dev->bssid);

        /* Message two: our random number, and the proof.  The access point
         * can only check this code if it derived the same key, which it only
         * could if it knows the passphrase. */
        u16 reply = (u16)((key_info & KEY_DESC_VERSION_MASK) |
                          KEY_INFO_PAIRWISE | KEY_INFO_MIC);

        /* The RSN element we sent when associating, repeated here so the
         * access point can check nothing was altered in between. */
        static const u8 rsn[] = {
            0x30, 0x14, 0x01, 0x00,
            0x00, 0x0F, 0xAC, 0x04,              /* group cipher: CCMP     */
            0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,  /* pairwise: CCMP         */
            0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,  /* authentication: PSK    */
            0x00, 0x00,
        };
        send_eapol(dev, wpa, reply, rsn, (int)sizeof rsn, 0, replay);
        return true;
    }

    /* Message three: the access point's proof, and the instruction to install
     * the key.  Its integrity code is checked before anything is believed. */
    if ((key_info & KEY_INFO_MIC) && (key_info & KEY_INFO_ACK)) {
        u16 key_data_len = get16be(body + 93);
        int total = 4 + 95 + key_data_len;
        if (total > len) return false;

        /* Verify by recomputing over a copy with the code field zeroed. */
        static u8 copy[512];
        if (total > (int)sizeof copy) return false;
        memcpy(copy, data, (size_t)total);
        memset(copy + 4 + 77, 0, MIC_LEN);

        u8 expected[SHA1_SIZE];
        hmac_sha1(wpa->ptk, 16, copy, (size_t)total, expected);

        u8 difference = 0;
        for (int i = 0; i < MIC_LEN; i++)
            difference |= (u8)(expected[i] ^ body[77 + i]);

        if (difference) {
            kwarn("wifi", "%s: the access point's key check failed - the "
                          "passphrase is wrong, or something is in the way",
                  dev->name);
            dev->state = WIFI_DOWN;
            wpa->active = false;
            return false;
        }

        /* Message four: acknowledge, and start using the key.  The last
         * sixteen bytes of the session key are the ones frames are encrypted
         * with. */
        u16 reply = (u16)((key_info & KEY_DESC_VERSION_MASK) |
                          KEY_INFO_PAIRWISE | KEY_INFO_MIC | KEY_INFO_SECURE);
        send_eapol(dev, wpa, reply, NULL, 0, 0, replay);

        aes_setkey(&wpa->aes, wpa->ptk + 32, 128);
        wpa->have_key = true;
        wpa->ready = true;
        wpa->tx_packet_number = 1;
        wpa->rx_packet_number = 0;

        if (dev->driver->set_key)
            dev->driver->set_key(dev, wpa->ptk + 32, 16, true);

        dev->state = WIFI_CONNECTED;
        dev->net.link_up = true;
        kinfo("wifi", "%s: keys agreed, the link is protected with CCMP",
              dev->name);
        return true;
    }

    return false;
}

/* --------------------------------------------------------------- CCMP */

/* The eight-byte header CCMP puts in front of the payload: the packet number,
 * split around a key index byte for reasons of backwards compatibility. */
#define CCMP_HDR_LEN 8
#define CCMP_MIC_LEN 8

/* The nonce is the priority, the sender's address and the packet number - so
 * no two frames from this station ever use the same one. */
static void ccmp_nonce(const u8 *addr, u64 packet_number, u8 nonce[13]) {
    nonce[0] = 0;                                 /* priority zero */
    memcpy(nonce + 1, addr, ETH_ALEN);
    for (int i = 0; i < 6; i++)
        nonce[7 + i] = (u8)(packet_number >> ((5 - i) * 8));
}

/* The additional authenticated data: the parts of the header that must not be
 * altered.  Fields that legitimately change in flight are masked out, because
 * authenticating them would make every frame fail. */
static int ccmp_aad(const u8 *frame, u8 *aad) {
    u16 fc = get16le(frame);

    /* Retry, power management, more data and the protected bit all change
     * without the frame having been tampered with. */
    u16 masked = (u16)(fc & ~0x4780);
    put16le(aad, masked);

    memcpy(aad + 2, frame + 4, ETH_ALEN * 3);      /* the three addresses */

    /* The sequence number varies; only its fragment part is covered. */
    u16 seq = (u16)(get16le(frame + 22) & 0x000F);
    put16le(aad + 20, seq);

    return 22;
}

int wpa_encrypt_frame(wifi_device_t *dev, u8 *frame, int len, int capacity) {
    wpa_t *wpa = state_for(dev, false);
    if (!wpa || !wpa->have_key) return -1;
    if (len + CCMP_HDR_LEN + CCMP_MIC_LEN > capacity) return -1;

    int header_len = 24;
    int payload_len = len - header_len;
    if (payload_len <= 0) return -1;

    /* Make room for the CCMP header between the frame header and the payload. */
    memmove(frame + header_len + CCMP_HDR_LEN, frame + header_len,
            (size_t)payload_len);

    u64 pn = wpa->tx_packet_number++;
    u8 *ccmp = frame + header_len;
    ccmp[0] = (u8)(pn >> 0);
    ccmp[1] = (u8)(pn >> 8);
    ccmp[2] = 0;                                   /* reserved */
    ccmp[3] = 0x20;                                /* key index zero, extended */
    ccmp[4] = (u8)(pn >> 16);
    ccmp[5] = (u8)(pn >> 24);
    ccmp[6] = (u8)(pn >> 32);
    ccmp[7] = (u8)(pn >> 40);

    /* Say the frame is protected before the header is authenticated. */
    put16le(frame, (u16)(get16le(frame) | 0x4000));

    u8 aad[32];
    int aad_len = ccmp_aad(frame, aad);

    u8 nonce[13];
    ccmp_nonce(frame + 10, pn, nonce);             /* address two: us */

    u8 *payload = frame + header_len + CCMP_HDR_LEN;
    u8 mic[CCMP_MIC_LEN];

    if (!aes_ccm_encrypt(&wpa->aes, nonce, sizeof nonce, aad, aad_len,
                         payload, payload_len, mic, CCMP_MIC_LEN))
        return -1;

    memcpy(payload + payload_len, mic, CCMP_MIC_LEN);
    return header_len + CCMP_HDR_LEN + payload_len + CCMP_MIC_LEN;
}

bool wpa_decrypt_frame(wifi_device_t *dev, u8 *frame, int *len) {
    wpa_t *wpa = state_for(dev, false);
    if (!wpa || !wpa->have_key) return false;

    int header_len = 24;
    if (*len < header_len + CCMP_HDR_LEN + CCMP_MIC_LEN) return false;

    const u8 *ccmp = frame + header_len;
    u64 pn = (u64)ccmp[0] | ((u64)ccmp[1] << 8) | ((u64)ccmp[4] << 16) |
             ((u64)ccmp[5] << 24) | ((u64)ccmp[6] << 32) | ((u64)ccmp[7] << 40);

    /* A packet number that has been seen means an old frame is being played
     * back; refusing it is the whole point of counting them. */
    if (pn <= wpa->rx_packet_number) {
        kdebug("wifi", "%s: dropped a replayed frame", dev->name);
        return false;
    }

    u8 aad[32];
    int aad_len = ccmp_aad(frame, aad);

    u8 nonce[13];
    ccmp_nonce(frame + 10, pn, nonce);             /* address two: the sender */

    u8 *payload = frame + header_len + CCMP_HDR_LEN;
    int payload_len = *len - header_len - CCMP_HDR_LEN - CCMP_MIC_LEN;
    const u8 *mic = payload + payload_len;

    if (!aes_ccm_decrypt(&wpa->aes, nonce, sizeof nonce, aad, aad_len,
                         payload, payload_len, mic, CCMP_MIC_LEN)) {
        kdebug("wifi", "%s: a frame failed its integrity check", dev->name);
        return false;
    }

    wpa->rx_packet_number = pn;

    /* Take the CCMP header back out so what is left is an ordinary frame. */
    memmove(frame + header_len, payload, (size_t)payload_len);
    put16le(frame, (u16)(get16le(frame) & ~0x4000));
    *len = header_len + payload_len;
    return true;
}
