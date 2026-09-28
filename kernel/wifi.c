/* wifi.c - the Wi-Fi device layer, and knowing what card is fitted.
 *
 * Identification matters more here than anywhere else in this system.  Most
 * Wi-Fi chips will not do anything at all until the vendor's firmware has been
 * pushed into them, and a machine whose card is unsupported looks exactly like
 * a machine whose firmware file is simply absent.  Those are completely
 * different situations - one is a limit, the other is a missing file the user
 * can supply in a minute - and the only way to tell them apart is to know what
 * the card is and name the file it wants.
 *
 * So the table below is long on purpose.  It covers Intel, MediaTek, Realtek,
 * Qualcomm Atheros, Broadcom, Marvell and Ralink across the generations
 * actually found in machines, and every entry says which firmware file that
 * part needs and where it comes from.
 */
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "klog.h"
#include "firmware.h"
#include "wifi.h"

static wifi_device_t *devices;
static int device_count;

/* ------------------------------------------------------------------- names */

const char *wifi_vendor_name(wifi_vendor_t v) {
    switch (v) {
    case WIFI_VENDOR_INTEL:    return "Intel";
    case WIFI_VENDOR_MEDIATEK: return "MediaTek";
    case WIFI_VENDOR_REALTEK:  return "Realtek";
    case WIFI_VENDOR_ATHEROS:  return "Qualcomm Atheros";
    case WIFI_VENDOR_BROADCOM: return "Broadcom";
    case WIFI_VENDOR_MARVELL:  return "Marvell";
    case WIFI_VENDOR_RALINK:   return "Ralink";
    default:                   return "unknown";
    }
}

const char *wifi_security_name(wifi_security_t s) {
    switch (s) {
    case WIFI_SECURITY_OPEN: return "open";
    case WIFI_SECURITY_WEP:  return "WEP";
    case WIFI_SECURITY_WPA:  return "WPA";
    case WIFI_SECURITY_WPA2: return "WPA2";
    case WIFI_SECURITY_WPA3: return "WPA3";
    default:                 return "unknown";
    }
}

const char *wifi_state_name(wifi_state_t s) {
    switch (s) {
    case WIFI_DOWN:            return "down";
    case WIFI_SCANNING:        return "scanning";
    case WIFI_AUTHENTICATING:  return "authenticating";
    case WIFI_ASSOCIATING:     return "associating";
    case WIFI_HANDSHAKING:     return "exchanging keys";
    case WIFI_CONNECTED:       return "connected";
    default:                   return "unknown";
    }
}

/* -------------------------------------------------------------- the table */

typedef struct {
    u16 vendor;
    u16 first, last;           /* an inclusive range of device ids */
    wifi_vendor_t which;
    const char *model;
    const char *firmware;      /* empty when the part needs none */
} wifi_id_t;

/* Where each vendor's firmware comes from, said once. */
#define SRC_LINUX  "the linux-firmware package"
#define SRC_NONE   ""

static const wifi_id_t known[] = {
    /* ---- Intel.  Every one of these needs firmware; the file name encodes
     * the part and the API version the driver asks for. ---- */
    { 0x8086, 0x0082, 0x0085, WIFI_VENDOR_INTEL, "Intel Centrino Advanced-N 6205", "iwlwifi-6000g2a-6.ucode" },
    { 0x8086, 0x0087, 0x008B, WIFI_VENDOR_INTEL, "Intel Centrino Advanced-N 6235", "iwlwifi-6000g2b-6.ucode" },
    { 0x8086, 0x008E, 0x0091, WIFI_VENDOR_INTEL, "Intel Centrino Wireless-N 7260", "iwlwifi-7260-17.ucode" },
    { 0x8086, 0x095A, 0x095B, WIFI_VENDOR_INTEL, "Intel Wireless 7265",            "iwlwifi-7265D-29.ucode" },
    { 0x8086, 0x3165, 0x3166, WIFI_VENDOR_INTEL, "Intel Wireless 3165",            "iwlwifi-7265D-29.ucode" },
    { 0x8086, 0x24F3, 0x24FD, WIFI_VENDOR_INTEL, "Intel Wireless 8260/8265",       "iwlwifi-8265-36.ucode" },
    { 0x8086, 0x2526, 0x2526, WIFI_VENDOR_INTEL, "Intel Wireless-AC 9260",         "iwlwifi-9260-th-b0-jf-b0-46.ucode" },
    { 0x8086, 0x2723, 0x2723, WIFI_VENDOR_INTEL, "Intel Wi-Fi 6 AX200",            "iwlwifi-cc-a0-77.ucode" },
    { 0x8086, 0x2725, 0x2726, WIFI_VENDOR_INTEL, "Intel Wi-Fi 6 AX210",            "iwlwifi-ty-a0-gf-a0-89.ucode" },
    { 0x8086, 0x272B, 0x272B, WIFI_VENDOR_INTEL, "Intel Wi-Fi 7 BE200",            "iwlwifi-gl-c0-fm-c0-89.ucode" },
    { 0x8086, 0x7A70, 0x7AF0, WIFI_VENDOR_INTEL, "Intel Wi-Fi 6E AX211/AX411",     "iwlwifi-so-a0-gf-a0-89.ucode" },
    { 0x8086, 0x51F0, 0x51F1, WIFI_VENDOR_INTEL, "Intel Wi-Fi 6E AX211",           "iwlwifi-so-a0-gf-a0-89.ucode" },
    { 0x8086, 0x54F0, 0x54F0, WIFI_VENDOR_INTEL, "Intel Wi-Fi 6E AX211",           "iwlwifi-so-a0-gf-a0-89.ucode" },
    { 0x8086, 0x4DF0, 0x4DF0, WIFI_VENDOR_INTEL, "Intel Wi-Fi 6E AX211",           "iwlwifi-so-a0-gf-a0-89.ucode" },
    { 0x8086, 0x0000, 0xFFFF, WIFI_VENDOR_INTEL, "Intel wireless",                 "iwlwifi firmware for this part" },

    /* ---- MediaTek.  The MT76 family; all need firmware. ---- */
    { 0x14C3, 0x7915, 0x7915, WIFI_VENDOR_MEDIATEK, "MediaTek MT7915 Wi-Fi 6",  "mediatek/mt7915_wm.bin" },
    { 0x14C3, 0x7921, 0x7922, WIFI_VENDOR_MEDIATEK, "MediaTek MT7921 Wi-Fi 6",  "mediatek/WIFI_RAM_CODE_MT7961_1.bin" },
    { 0x14C3, 0x7925, 0x7925, WIFI_VENDOR_MEDIATEK, "MediaTek MT7925 Wi-Fi 7",  "mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.bin" },
    { 0x14C3, 0x7961, 0x7961, WIFI_VENDOR_MEDIATEK, "MediaTek MT7961 Wi-Fi 6",  "mediatek/WIFI_RAM_CODE_MT7961_1.bin" },
    { 0x14C3, 0x0000, 0xFFFF, WIFI_VENDOR_MEDIATEK, "MediaTek wireless",        "MediaTek firmware for this part" },

    /* ---- Realtek. ---- */
    { 0x10EC, 0x8179, 0x8179, WIFI_VENDOR_REALTEK, "Realtek RTL8188EE",   "rtlwifi/rtl8188efw.bin" },
    { 0x10EC, 0x8176, 0x8178, WIFI_VENDOR_REALTEK, "Realtek RTL8192CE",   "rtlwifi/rtl8192cfw.bin" },
    { 0x10EC, 0x8812, 0x8812, WIFI_VENDOR_REALTEK, "Realtek RTL8812AE",   "rtlwifi/rtl8812aefw.bin" },
    { 0x10EC, 0x8821, 0x8821, WIFI_VENDOR_REALTEK, "Realtek RTL8821AE",   "rtlwifi/rtl8821aefw.bin" },
    { 0x10EC, 0xB822, 0xB822, WIFI_VENDOR_REALTEK, "Realtek RTL8822BE",   "rtw88/rtw8822b_fw.bin" },
    { 0x10EC, 0xC822, 0xC822, WIFI_VENDOR_REALTEK, "Realtek RTL8822CE",   "rtw88/rtw8822c_fw.bin" },
    { 0x10EC, 0x8852, 0x8852, WIFI_VENDOR_REALTEK, "Realtek RTL8852AE Wi-Fi 6", "rtw89/rtw8852a_fw.bin" },
    { 0x10EC, 0xA85A, 0xA85A, WIFI_VENDOR_REALTEK, "Realtek RTL8852AE Wi-Fi 6", "rtw89/rtw8852a_fw.bin" },
    { 0x10EC, 0xB852, 0xB852, WIFI_VENDOR_REALTEK, "Realtek RTL8852BE Wi-Fi 6", "rtw89/rtw8852b_fw.bin" },
    { 0x10EC, 0xB85B, 0xB85B, WIFI_VENDOR_REALTEK, "Realtek RTL8852BE Wi-Fi 6", "rtw89/rtw8852b_fw.bin" },
    { 0x10EC, 0xC852, 0xC852, WIFI_VENDOR_REALTEK, "Realtek RTL8852CE Wi-Fi 6E", "rtw89/rtw8852c_fw.bin" },
    { 0x10EC, 0x8851, 0x8851, WIFI_VENDOR_REALTEK, "Realtek RTL8851BE Wi-Fi 6",  "rtw89/rtw8851b_fw.bin" },
    /* Wi-Fi 7.  The firmware for this one is split across several files, and
     * the first is what the chip is started from - the rest are loaded by the
     * part running on it, so naming the first is what a driver has to get
     * right.  The names come from the firmware collection itself rather than
     * from a pattern: guessing at one is how a card ends up asking for a file
     * nobody could have supplied because it was never published. */
    { 0x10EC, 0x8922, 0x8922, WIFI_VENDOR_REALTEK, "Realtek RTL8922AE Wi-Fi 7", "rtw89/rtw8922a_fw.bin" },

    /* ---- Qualcomm Atheros.  The AR9xxx line is the one family that needs no
     * firmware: everything it does is in silicon and driven by registers.  The
     * later QCA parts went the other way. ---- */
    { 0x168C, 0x0023, 0x0024, WIFI_VENDOR_ATHEROS, "Atheros AR5416",  SRC_NONE },
    { 0x168C, 0x0027, 0x0027, WIFI_VENDOR_ATHEROS, "Atheros AR9160",  SRC_NONE },
    { 0x168C, 0x0029, 0x002A, WIFI_VENDOR_ATHEROS, "Atheros AR9220",  SRC_NONE },
    { 0x168C, 0x002B, 0x002B, WIFI_VENDOR_ATHEROS, "Atheros AR9285",  SRC_NONE },
    { 0x168C, 0x002C, 0x002C, WIFI_VENDOR_ATHEROS, "Atheros AR2427",  SRC_NONE },
    { 0x168C, 0x002D, 0x002D, WIFI_VENDOR_ATHEROS, "Atheros AR9227",  SRC_NONE },
    { 0x168C, 0x002E, 0x002E, WIFI_VENDOR_ATHEROS, "Atheros AR9287",  SRC_NONE },
    { 0x168C, 0x0030, 0x0030, WIFI_VENDOR_ATHEROS, "Atheros AR9300",  SRC_NONE },
    { 0x168C, 0x0032, 0x0034, WIFI_VENDOR_ATHEROS, "Atheros AR9462/AR9485", SRC_NONE },
    { 0x168C, 0x0036, 0x0037, WIFI_VENDOR_ATHEROS, "Atheros AR9565",  SRC_NONE },
    { 0x168C, 0x003C, 0x003E, WIFI_VENDOR_ATHEROS, "Qualcomm QCA6174", "ath10k/QCA6174/hw3.0/firmware-6.bin" },
    { 0x168C, 0x0040, 0x0042, WIFI_VENDOR_ATHEROS, "Qualcomm QCA9377", "ath10k/QCA9377/hw1.0/firmware-6.bin" },
    { 0x168C, 0x0000, 0xFFFF, WIFI_VENDOR_ATHEROS, "Atheros wireless", SRC_NONE },

    /* ---- Broadcom.  Every one needs firmware, and Broadcom has historically
     * been the least forthcoming about supplying it. ---- */
    { 0x14E4, 0x4311, 0x4331, WIFI_VENDOR_BROADCOM, "Broadcom BCM43xx",  "brcm/brcmfmac43xx.bin" },
    { 0x14E4, 0x4350, 0x4365, WIFI_VENDOR_BROADCOM, "Broadcom BCM4350",  "brcm/brcmfmac4350-pcie.bin" },
    { 0x14E4, 0x43A0, 0x43BA, WIFI_VENDOR_BROADCOM, "Broadcom BCM4360",  "brcm/brcmfmac4360-pcie.bin" },
    { 0x14E4, 0x0000, 0xFFFF, WIFI_VENDOR_BROADCOM, "Broadcom wireless", "Broadcom firmware for this part" },

    /* ---- Marvell and Ralink. ---- */
    { 0x11AB, 0x2B38, 0x2B40, WIFI_VENDOR_MARVELL, "Marvell 88W8897", "mrvl/pcieuart8897_combo_v4.bin" },
    { 0x1814, 0x0000, 0xFFFF, WIFI_VENDOR_RALINK,  "Ralink wireless", "rt2870.bin" },
};

bool wifi_identify(u16 pci_vendor, u16 pci_device, wifi_device_t *out) {
    for (size_t i = 0; i < ARRAY_LEN(known); i++) {
        const wifi_id_t *k = &known[i];
        if (k->vendor != pci_vendor) continue;
        if (pci_device < k->first || pci_device > k->last) continue;

        out->vendor = k->which;
        out->pci_vendor = pci_vendor;
        out->pci_device = pci_device;
        strlcpy(out->model, k->model, sizeof out->model);

        if (k->firmware[0]) {
            out->firmware_needed = true;
            strlcpy(out->firmware_name, k->firmware, sizeof out->firmware_name);
            out->firmware_present = firmware_present(k->firmware, NULL);
        } else {
            out->firmware_needed = false;
            out->firmware_name[0] = 0;
            out->firmware_present = true;      /* nothing to be missing */
        }
        return true;
    }
    return false;
}

/* ------------------------------------------------------------- registration */

void wifi_register(wifi_device_t *dev) {
    /* A radio is on unless somebody switches it off; whether it actually comes
     * up is a separate question the driver answers. */
    dev->enabled = true;

    dev->next = NULL;
    if (!devices) devices = dev;
    else {
        wifi_device_t *last = devices;
        while (last->next) last = last->next;
        last->next = dev;
    }
    device_count++;

    char mac[24];
    mac_format(&dev->mac, mac, sizeof mac);

    if (dev->firmware_needed && !dev->firmware_present) {
        kwarn("wifi", "%s: %s, %s - needs %s, which is not present",
              dev->name, dev->model, mac, dev->firmware_name);
        kwarn("wifi", "%s: put that file in %s and it will start",
              dev->name, FIRMWARE_PATH_DATA);
    } else {
        kinfo("wifi", "%s: %s, %s%s", dev->name, dev->model, mac,
              dev->firmware_needed ? ", firmware present" : ", no firmware needed");
    }
}

wifi_device_t *wifi_first(void) { return devices; }
int wifi_count(void) { return device_count; }

wifi_device_t *wifi_by_name(const char *name) {
    for (wifi_device_t *d = devices; d; d = d->next)
        if (!strcmp(d->name, name)) return d;
    return NULL;
}

/* --------------------------------------------------- cards nothing claimed */

/* Walk the PCI bus for wireless controllers no driver took, and record what
 * each would need.  A card that appears here with its firmware missing is one
 * the user can make work; a card that appears with no driver named is one this
 * system genuinely does not support yet, and the difference is worth stating.
 */
static void report_unclaimed(void) {
    pci_dev_t *d = NULL;

    /* Class 2 subclass 128 is "other network controller", which is where
     * wireless parts land; some report subclass 0x80 and a few use 0x00. */
    for (u8 sub = 0x80; sub != 0x81; sub++) {
        d = NULL;
        while ((d = pci_find(0x02, sub, 0xFF, d)) != NULL) {
            bool claimed = false;
            for (wifi_device_t *w = devices; w; w = w->next)
                if (w->pci_vendor == d->vendor && w->pci_device == d->device)
                    claimed = true;
            if (claimed) continue;

            wifi_device_t info;
            memset(&info, 0, sizeof info);
            if (!wifi_identify(d->vendor, d->device, &info)) {
                kwarn("wifi", "unrecognised wireless card %04x:%04x",
                      d->vendor, d->device);
                continue;
            }

            if (info.firmware_needed) {
                firmware_declare(info.firmware_name, SRC_LINUX);
                kwarn("wifi", "%s found, but there is no driver for it here; "
                              "it would also need %s",
                      info.model, info.firmware_name);
            } else {
                kwarn("wifi", "%s found, but there is no driver for it here",
                      info.model);
            }
        }
    }
}

/* -------------------------------------------------------------------- init */

void wifi_init(void) {
    /* Atheros first: it is the one that can work with nothing supplied. */
    ath9k_init();
    iwlwifi_init();
    mt76_init();
    rtw_init();

    report_unclaimed();

    if (!device_count)
        kinfo("wifi", "no wireless interface");
}
