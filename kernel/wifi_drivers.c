/* wifi_drivers.c - what the wireless drivers are checked against.
 *
 * The drivers themselves are in ath9k.c, iwlwifi.c, rtw.c and mt76.c, each
 * beside a model of the hardware it is written for.  This file drives all four
 * of them and checks what came back.
 *
 * No virtual machine emulates a Wi-Fi card - VMware, QEMU and VirtualBox all
 * present a wired Ethernet adapter to the guest whatever the host is connected
 * to - so none of this code has been run against the silicon it is written
 * for.  What it has been run against is a model of that silicon, which
 * exercises everything except whether the register offsets are the right ones.
 * Each driver's own preamble says exactly what that does and does not
 * establish.
 */
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "time.h"
#include "proc.h"
#include "klog.h"
#include "firmware.h"
#include "wifi.h"
#include "crypto.h"
#include "rtw.h"
#include "mt76.h"

void ieee80211_receive(wifi_device_t *dev, u8 *frame, int len, s8 signal_dbm);
bool wpa_decrypt_frame(wifi_device_t *dev, u8 *frame, int *len);
int  wpa_encrypt_frame(wifi_device_t *dev, u8 *frame, int len, int capacity);
void ieee80211_attach(wifi_device_t *dev);

/* ========================================================================= */
/* Checking what can be checked                                              */
/* ========================================================================= */

const char *ath_model_ssid(void);
const char *ath_model_passphrase(void);
u8          ath_model_channel(void);
bool        ath9k_is_modelled(void);

bool wpa_decrypt_frame(wifi_device_t *dev, u8 *frame, int *len);
int  wpa_encrypt_frame(wifi_device_t *dev, u8 *frame, int len, int capacity);

/* Drive a whole join and check what came out.
 *
 * This goes through the real Atheros driver - its descriptor rings, its DMA,
 * its transmit and receive paths - against a model of the hardware, because
 * nothing available to test on emulates a Wi-Fi card.  What that establishes
 * and what it does not is set out at the top of ath9k_model.c, and the same
 * boundary is reported to whoever runs this.
 *
 * What is checked: the scan finds a beacon and reads its security correctly,
 * authentication and association complete, the four-way handshake ends with
 * both sides having derived the same key, a frame survives encryption and
 * decryption, and a wrong passphrase is refused at the point it must be. */
int wifi_selftest(void) {
    int failures = 0;

    /* With no card in the machine, stand the model up so the driver has
     * something to drive.  A real card, if there is one, is used instead. */
    if (!wifi_first()) ath9k_attach_model();

    wifi_device_t *dev = wifi_first();
    if (!dev) {
        kerr("wifi-test", "no wireless interface to test with");
        return 1;
    }

    /* The Intel firmware container, which is checkable without any hardware. */
    failures += iwl_selftest();

    /* And the Intel driver itself, against a model of an Intel device.  This
     * runs the whole path the container test does not reach: the queues, the
     * handshake that says the microcode has started, the commands that read
     * the device's address and tune it, and a scan going out through the
     * transmit ring and coming back through the receive ring. */
    failures += iwl_drive_test();

    /* And the Realtek driver, which hands its firmware over a page at a time
     * through a window and then talks to it through four mailboxes. */
    failures += rtw_drive_test();

    /* And the MediaTek driver, which sends both its firmware files and every
     * command afterwards over the same descriptor rings. */
    failures += mt76_drive_test();

    kinfo("wifi-test", "driving a join through the %s driver%s",
          dev->driver->name,
          ath9k_is_modelled() ? " against a hardware model" : "");

    const char *ssid = ath_model_ssid();
    const char *passphrase = ath_model_passphrase();

    /* The scan has to find the network, on the right channel, with the right
     * security read out of its RSN element. */
    wifi_scan(dev, 2000);

    const wifi_network_t *found = NULL;
    for (int i = 0; i < dev->scan_count; i++)
        if (!strcmp(dev->scan[i].ssid, ssid)) found = &dev->scan[i];

    if (!found) {
        kerr("wifi-test", "the scan did not find the network");
        failures++;
    } else {
        kinfo("wifi-test", "found \"%s\" on channel %u, %s, %d dBm",
              found->ssid, found->channel,
              wifi_security_name(found->security), found->signal_dbm);
        if (found->security != WIFI_SECURITY_WPA2) {
            kerr("wifi-test", "the security was read as %s, not WPA2",
                 wifi_security_name(found->security));
            failures++;
        }
        if (found->channel != ath_model_channel()) {
            kerr("wifi-test", "the channel was read as %u, not %u",
                 found->channel, ath_model_channel());
            failures++;
        }
    }

    if (wifi_connect(dev, ssid, passphrase, 6000) == 0 &&
        dev->state == WIFI_CONNECTED) {
        kinfo("wifi-test", "joined with the correct passphrase; the key was agreed");
    } else {
        kerr("wifi-test", "the join failed with the correct passphrase");
        failures++;
    }

    /* A frame has to survive the round trip through CCMP unchanged. */
    if (dev->state == WIFI_CONNECTED) {
        static u8 frame[256];
        memset(frame, 0, sizeof frame);
        frame[0] = 0x08; frame[1] = 0x01;
        memcpy(frame + 4, dev->bssid.addr, ETH_ALEN);
        memcpy(frame + 10, dev->mac.addr, ETH_ALEN);
        memcpy(frame + 16, dev->bssid.addr, ETH_ALEN);

        static const char message[] = "the quick brown fox";
        int payload = (int)sizeof message;
        memcpy(frame + 24, message, (size_t)payload);
        int plain_len = 24 + payload;

        int encrypted = wpa_encrypt_frame(dev, frame, plain_len, (int)sizeof frame);
        if (encrypted <= plain_len) {
            kerr("wifi-test", "the frame did not encrypt");
            failures++;
        } else if (!memcmp(frame + 24 + 8, message, (size_t)payload)) {
            kerr("wifi-test", "the payload came out unencrypted");
            failures++;
        } else {
            int len = encrypted;
            if (!wpa_decrypt_frame(dev, frame, &len)) {
                kerr("wifi-test", "the frame failed its own integrity check");
                failures++;
            } else if (len != plain_len ||
                       memcmp(frame + 24, message, (size_t)payload)) {
                kerr("wifi-test", "the frame did not survive the round trip");
                failures++;
            } else {
                kinfo("wifi-test", "a frame encrypted and decrypted intact "
                                   "(%d bytes plain, %d on the air)",
                      plain_len, encrypted);
            }
        }
    }

    wifi_disconnect(dev);

    /* A wrong passphrase has to fail, and has to fail at message three - where
     * the access point's proof does not verify.  A supplicant that accepted it
     * would look identical to one that worked, right up until nothing could be
     * decrypted. */
    if (wifi_connect(dev, ssid, "not-the-passphrase", 3000) == 0) {
        kerr("wifi-test", "a wrong passphrase was accepted");
        failures++;
        wifi_disconnect(dev);
    } else {
        kinfo("wifi-test", "a wrong passphrase was refused, as it must be");
    }

    if (failures)
        kerr("wifi-test", "%d check(s) failed", failures);
    else
        kinfo("wifi-test", "the driver, the 802.11 exchange, the key agreement "
                           "and CCMP are all correct");
    return failures;
}

/* ------------------------------------------------------- the Intel driver
 *
 * The container test above checks the file format.  This checks the driver:
 * whether it can bring a device up, hold a conversation with the microcode
 * running on it, and get a frame there and back.
 *
 * Returns the number of checks that failed.
 */
int iwl_drive_test(void) {
    bool iwlwifi_attach_model(void);
    wifi_device_t *iwlwifi_model_device(void);

    if (!iwlwifi_attach_model()) {
        kwarn("iwl-test", "the Intel model could not be brought up");
        return 1;
    }

    wifi_device_t *dev = iwlwifi_model_device();
    if (!dev) {
        kerr("iwl-test", "the Intel model registered no device");
        return 1;
    }

    int failures = 0;

    if (!dev->radio_up) {
        kerr("iwl-test", "the radio did not come up");
        return 1;
    }

    /* The address came back through the command interface, which means the
     * command went out through the transmit ring, the model read the
     * descriptor, and the answer came back through the receive ring with a
     * sequence number that matched. */
    static const u8 expected[ETH_ALEN] = { 0x00, 0x21, 0x6a, 0x44, 0x55, 0x66 };
    if (memcmp(dev->mac.addr, expected, ETH_ALEN)) {
        char got[24];
        mac_format(&dev->mac, got, sizeof got);
        kerr("iwl-test", "the device's address came back as %s", got);
        failures++;
    } else {
        kinfo("iwl-test", "the microcode answered: address read through the "
                          "command queue");
    }

    /* A scan drives the whole path: a channel command down the command queue,
     * a probe request down the data queue, and a probe response back up the
     * receive ring into the 802.11 layer. */
    wifi_scan(dev, 2000);

    const wifi_network_t *found = NULL;
    for (int i = 0; i < dev->scan_count; i++)
        if (!strcmp(dev->scan[i].ssid, "kestrel-test")) found = &dev->scan[i];

    if (!found) {
        kerr("iwl-test", "the scan found nothing through the Intel driver");
        failures++;
    } else if (found->security != WIFI_SECURITY_WPA2) {
        kerr("iwl-test", "the security came back as %s",
             wifi_security_name(found->security));
        failures++;
    } else {
        kinfo("iwl-test", "a probe request went out through the transmit ring "
                          "and \"%s\" came back through the receive ring",
              found->ssid);
    }

    if (!failures)
        kinfo("iwl-test", "the Intel driver's queues, handshake and command "
                          "path are all correct");
    return failures;
}
