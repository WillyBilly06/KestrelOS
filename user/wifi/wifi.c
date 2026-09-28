/* wifi - look at and use the wireless hardware.
 *
 *   wifi                        what card is fitted and what state it is in
 *   wifi scan                   the networks in range
 *   wifi connect <ssid> [pass]  join one
 *   wifi disconnect
 *   wifi selftest               drive every driver against a model of its
 *                               hardware, and check the cryptography
 *
 * When there is no card, or the card needs a firmware file that is not there,
 * this says exactly that - because "no wireless" and "one file short of
 * working wireless" are very different situations to be in.
 */
#include "kestrel.h"

static const char *state_name(uint8_t s) {
    switch (s) {
    case 0: return "down";
    case 1: return "scanning";
    case 2: return "authenticating";
    case 3: return "associating";
    case 4: return "exchanging keys";
    case 5: return "connected";
    default: return "unknown";
    }
}

static const char *security_name(uint8_t s) {
    switch (s) {
    case 0: return "open";
    case 1: return "WEP";
    case 2: return "WPA";
    case 3: return "WPA2";
    case 4: return "WPA3";
    default: return "unknown";
    }
}

/* A rough bar from the signal strength, which is easier to read at a glance
 * than a negative number in decibels. */
static void print_signal(int8_t dbm) {
    int bars = 0;
    if (dbm >= -55)      bars = 4;
    else if (dbm >= -67) bars = 3;
    else if (dbm >= -78) bars = 2;
    else if (dbm >= -90) bars = 1;

    char meter[6];
    for (int i = 0; i < 4; i++) meter[i] = (i < bars) ? '#' : '.';
    meter[4] = 0;
    printf("%s %4d dBm", meter, dbm);
}

static void show_firmware_needs(void) {
    kfirmware_t f;
    int count = 0;

    for (uint32_t i = 0; ; i++) {
        if (enum_firmware(i, &f) < 0) break;
        if (!count) {
            printf("\nFirmware this machine's hardware asked for:\n");
            printf("  %-46s %s\n", "FILE", "STATUS");
        }
        char status[64];
        if (f.present)
            snprintf(status, sizeof status, "present, %llu bytes",
                     (unsigned long long)f.size);
        else
            snprintf(status, sizeof status, "missing - %s", f.source);
        printf("  %-46s %s\n", f.name, status);
        count++;
    }

    if (count)
        printf("\n  Put missing files in /data/firmware and reboot.\n"
               "  `firmware import <path>` copies one in from a mounted volume.\n");
}

static int show_all(void) {
    kwifiinfo_t w;
    int count = 0;

    for (uint32_t i = 0; ; i++) {
        if (enum_wifi(i, &w) < 0) break;

        char mac[24];
        format_mac(w.mac, mac, sizeof mac);

        printf("%s: %s\n", w.name, w.model);
        printf("  %-10s %s\n", "vendor", w.vendor);
        printf("  %-10s %s\n", "hardware", mac);
        printf("  %-10s %s\n", "state", state_name(w.state));

        if (w.firmware_needed) {
            printf("  %-10s %s (%s)\n", "firmware", w.firmware_name,
                   w.firmware_present ? "present" : "MISSING");
        } else {
            printf("  %-10s none needed\n", "firmware");
        }

        if (w.state == 5) {
            printf("  %-10s \"%s\" on channel %u, %s\n", "network",
                   w.ssid, w.channel, security_name(w.security));
        }
        printf("\n");
        count++;
    }

    if (!count) {
        printf("No wireless interface.\n\n");
        printf("Either there is no Wi-Fi card in this machine, or the card that\n");
        printf("is here has no driver in this system.  `events -a` shows what the\n");
        printf("kernel found on the PCI bus at start-up, naming any wireless card\n");
        printf("it recognised but could not drive.\n");
    }

    show_firmware_needs();
    return count ? 0 : 1;
}

static int do_scan(void) {
    printf("wifi: scanning...\n");
    int found = wifi_scan("", 6000);
    if (found < 0) {
        printf("wifi: no interface to scan with\n");
        return 1;
    }
    if (!found) {
        printf("wifi: no networks in range\n");
        return 0;
    }

    printf("\n%-32s %-8s %-6s %s\n", "NETWORK", "SECURITY", "CH", "SIGNAL");
    for (uint32_t i = 0; ; i++) {
        kwifinet_t n;
        if (enum_scan(i, &n) < 0) break;

        const char *name = n.hidden ? "(hidden)" : n.ssid;
        printf("%-32s %-8s %-6u ", name, security_name(n.security), n.channel);
        print_signal(n.signal_dbm);
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return show_all();

    if (!strcmp(argv[1], "scan")) return do_scan();

    if (!strcmp(argv[1], "selftest")) {
        printf("wifi: driving a join through the driver, the 802.11 exchange\n");
        printf("      and the key agreement.\n\n");
        printf("      With no Wi-Fi card present this runs the real driver\n");
        printf("      against a model of the hardware - which exercises the\n");
        printf("      descriptor rings, the DMA and the protocol, but cannot\n");
        printf("      confirm the register offsets against real silicon.\n\n");
        int failures = wifi_selftest();
        if (failures < 0) {
            printf("wifi: the self-test could not run\n");
            return 1;
        }
        if (failures) {
            printf("\nwifi: %d check(s) failed\n", failures);
            return 1;
        }
        printf("\nwifi: the driver path, the protocol and the cryptography\n");
        printf("      are all correct\n");
        return 0;
    }

    if (!strcmp(argv[1], "connect")) {
        if (argc < 3) {
            printf("usage: wifi connect <ssid> [passphrase]\n");
            return 1;
        }
        const char *pass = (argc > 3) ? argv[3] : NULL;

        printf("wifi: joining \"%s\"...\n", argv[2]);
        if (wifi_connect("", argv[2], pass, 12000) < 0) {
            printf("wifi: could not join. `events -a` says at which step it stopped.\n");
            return 1;
        }
        printf("wifi: joined. Run `net up` to get an address.\n");
        return 0;
    }

    if (!strcmp(argv[1], "disconnect")) {
        if (wifi_disconnect("") < 0) {
            printf("wifi: no interface\n");
            return 1;
        }
        printf("wifi: disconnected\n");
        return 0;
    }

    printf("usage: wifi [scan | connect <ssid> [pass] | disconnect | selftest]\n");
    return 1;
}
