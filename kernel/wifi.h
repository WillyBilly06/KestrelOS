/* wifi.h - the Wi-Fi subsystem.
 *
 * Wi-Fi splits cleanly into three parts, and they are worth separating because
 * only one of them is vendor-specific:
 *
 *   The 802.11 protocol - frames, scanning, association, the four-way
 *   handshake, CCMP encryption.  This is published, identical across every
 *   vendor, and written from the specification in ieee80211.c and wpa.c.
 *
 *   The driver - how one particular chip is told to tune to a channel and send
 *   a frame.  This differs per vendor and per generation.
 *
 *   The firmware - a blob of the vendor's own code that most chips will not run
 *   without.  Vendors distribute it and forbid redistribution, which is why no
 *   operating system ships it and why this one loads it from wherever the user
 *   put it.
 *
 * Atheros AR9xxx is the exception worth knowing about: it needs no firmware at
 * all, which makes it the one family that can work with nothing supplied.
 */
#ifndef KESTREL_WIFI_H
#define KESTREL_WIFI_H

#include "kernel.h"
#include "net.h"

#define WIFI_SSID_MAX 32
#define WIFI_MAX_SCAN 32

typedef enum {
    WIFI_VENDOR_UNKNOWN = 0,
    WIFI_VENDOR_INTEL,
    WIFI_VENDOR_MEDIATEK,
    WIFI_VENDOR_REALTEK,
    WIFI_VENDOR_ATHEROS,      /* Qualcomm Atheros */
    WIFI_VENDOR_BROADCOM,
    WIFI_VENDOR_MARVELL,
    WIFI_VENDOR_RALINK,
} wifi_vendor_t;

/* What security a network uses, which decides what has to happen to join it. */
typedef enum {
    WIFI_SECURITY_OPEN = 0,
    WIFI_SECURITY_WEP,        /* recognised so it can be refused             */
    WIFI_SECURITY_WPA,
    WIFI_SECURITY_WPA2,
    WIFI_SECURITY_WPA3,
} wifi_security_t;

/* One network a scan found. */
typedef struct {
    char            ssid[WIFI_SSID_MAX + 1];
    mac_t           bssid;
    u8              channel;
    s8              signal_dbm;
    wifi_security_t security;
    bool            hidden;
} wifi_network_t;

typedef enum {
    WIFI_DOWN = 0,
    WIFI_SCANNING,
    WIFI_AUTHENTICATING,
    WIFI_ASSOCIATING,
    WIFI_HANDSHAKING,        /* the four-way exchange */
    WIFI_CONNECTED,
} wifi_state_t;

typedef struct wifi_device wifi_device_t;

/* What a driver has to provide.  Everything above this is shared. */
typedef struct {
    const char *name;

    /* Bring the radio up.  Returns false when it could not - which for most
     * chips means the firmware was missing. */
    bool (*start)(wifi_device_t *dev);
    void (*stop)(wifi_device_t *dev);

    /* Tune to a channel and send one 802.11 frame. */
    bool (*set_channel)(wifi_device_t *dev, u8 channel);
    int  (*transmit)(wifi_device_t *dev, const void *frame, int len);

    /* Collect whatever has arrived, calling ieee80211_receive for each frame. */
    void (*poll)(wifi_device_t *dev);

    /* Install a key once the handshake has produced one.  A chip that decrypts
     * in hardware needs telling; one that does not can leave this null and the
     * software path is used. */
    bool (*set_key)(wifi_device_t *dev, const u8 *key, int len, bool pairwise);
} wifi_driver_t;

struct wifi_device {
    char  name[16];               /* wlan0, and so on                    */
    char  model[64];
    wifi_vendor_t vendor;
    u16   pci_vendor, pci_device;

    mac_t mac;
    bool  firmware_needed;
    char  firmware_name[64];
    bool  firmware_present;
    bool  radio_up;

    /* True when this card is a generation its driver was not written for.
     *
     * Distinct from a radio that failed to start, and the difference is the
     * whole message: one is a fault somebody might chase, and this is a card
     * that is deliberately left alone because driving it would mean writing
     * another part's start-up sequence into it.  Reporting the second as the
     * first sends people looking for a problem with their hardware. */
    bool  unsupported_generation;

    /* The MAC has been brought out of reset, but the card is not yet usable.
     * Kept apart from radio_up on purpose: a card that is powered and one that
     * can carry traffic are different states, and showing the first as the
     * second is how a driver comes to be described as working when it is a
     * long way from it. */
    bool  powered_on;
    /* Whether the user wants the radio on at all.  Separate from radio_up,
     * which says whether it actually came up: an interface can be switched on
     * and still be down because its firmware is missing, and the two need
     * different words in front of a person. */
    bool  enabled;

    wifi_state_t state;
    char  ssid[WIFI_SSID_MAX + 1];
    mac_t bssid;
    u8    channel;
    s8    signal_dbm;
    wifi_security_t security;

    /* The networks the last scan found. */
    wifi_network_t scan[WIFI_MAX_SCAN];
    int   scan_count;
    u64   scan_finished_ms;

    const wifi_driver_t *driver;
    void *ctx;

    /* The Ethernet interface the IP stack sees.  802.11 frames are converted
     * to and from Ethernet ones so nothing above has to know the difference. */
    netdev_t net;

    struct wifi_device *next;
};

/* ------------------------------------------------------------------ public */

void wifi_init(void);
wifi_device_t *wifi_first(void);
wifi_device_t *wifi_by_name(const char *name);
int wifi_count(void);

void wifi_register(wifi_device_t *dev);

/* Scan, then join.  Both block the calling thread until they finish or time
 * out, as the rest of the network stack does. */
int wifi_scan(wifi_device_t *dev, int timeout_ms);
int wifi_connect(wifi_device_t *dev, const char *ssid, const char *passphrase,
                 int timeout_ms);
void wifi_disconnect(wifi_device_t *dev);

/* Switch a radio on or off.  Switching it off disconnects first and then stops
 * the hardware; switching it on starts it again.  Returns true if the radio
 * ended up in the state that was asked for. */
bool wifi_set_enabled(wifi_device_t *dev, bool on);

const char *wifi_vendor_name(wifi_vendor_t v);
const char *wifi_security_name(wifi_security_t s);
const char *wifi_state_name(wifi_state_t s);

/* Identify a card from its PCI ids, filling in the model, the vendor and which
 * firmware file it would need.  Used by the drivers and by the reporting path
 * for cards no driver claimed. */
bool wifi_identify(u16 pci_vendor, u16 pci_device, wifi_device_t *out);

/* ---------------------------------------------------------------- drivers */

void ath9k_init(void);

/* Stand a hardware model in for a card, so the driver code runs where no
 * Atheros hardware exists.  Used only by the self-test. */
bool ath9k_attach_model(void);
void iwlwifi_init(void);

/* Check the Intel firmware container is read correctly.  Needs no hardware:
 * the format is a file, and reading it is where a firmware load usually goes
 * wrong. */
int  iwl_selftest(void);

/* Drive the Intel driver against a model of an Intel device: the queues, the
 * handshake, the command path and a scan.  Needs no card. */
int  iwl_drive_test(void);
void mt76_init(void);
void rtw_init(void);

/* Bring up the test adapter and drive a whole join through it: scan, find the
 * network, authenticate, associate, exchange keys.  Returns the number of
 * checks that failed.  This is how the protocol and the cryptography are
 * verified where there is no radio - which is everywhere a virtual machine
 * runs, because none of them emulate a Wi-Fi card. */
int wifi_selftest(void);

/* The session key derivation, exposed so the test adapter can derive the same
 * key from the access point's side.  Both ends run this over the same inputs
 * in the same order and must arrive at the same answer - which is precisely
 * what the handshake is checking, so testing it means doing it twice. */
void wpa_derive_ptk(const u8 pmk[32], const mac_t *a, const mac_t *b,
                    const u8 nonce_a[32], const u8 nonce_b[32], u8 ptk[48]);

#endif
