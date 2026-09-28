/* bt - what is nearby, and connecting to it.
 *
 * The Bluetooth stack in this system goes down a long way: an adapter is
 * found, reset and asked who it is; both of its radios are told to look; a
 * link is opened; two devices agree on a key; and things travel over that link
 * inside L2CAP.  None of that is reachable by anybody without this.
 *
 * That gap is worth naming rather than quietly filling.  A driver nothing can
 * call is not a working driver, however carefully it is written and however
 * well it is tested - it is code that has never been asked to do the thing it
 * exists for.  This is the asking.
 *
 *   bt              what the adapter is, and what is currently connected
 *   bt scan [secs]  listen on both radios and list what answers
 *   bt connect <address>
 *   bt disconnect
 */
#include "kestrel.h"

static void print_address(const unsigned char *a) {
    printf("%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
}

/* "aa:bb:cc:dd:ee:ff", and nothing else - a mistyped address should be
 * refused rather than turned into a different one. */
static int parse_address(const char *text, unsigned char *out) {
    int n = 0;
    for (; n < 6; n++) {
        int hi = -1, lo = -1;
        for (int k = 0; k < 2; k++) {
            char c = *text++;
            int v = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (v < 0) return -1;
            if (k == 0) hi = v; else lo = v;
        }
        out[n] = (unsigned char)((hi << 4) | lo);

        if (n == 5) break;
        if (*text++ != ':') return -1;
    }
    return *text ? -1 : 0;
}

static int show_adapter(void) {
    kbtadapter_t a;
    if (bt_adapter(&a) < 0 || !a.present) {
        printf("bt: no Bluetooth adapter\n");
        printf(A_GREY "  A USB adapter appears here once it is plugged in.\n" A_RESET);
        return 1;
    }

    printf("adapter: %s, address ", a.maker[0] ? a.maker : "Bluetooth");
    print_address(a.address);
    printf("\n");

    kbtlink_t link;
    if (bt_link(&link) == 0 && link.open) {
        printf("connected to ");
        print_address(link.address);
        printf(", link %u", link.handle);
        if (link.interval_us)
            printf(", one exchange every %u.%02u ms",
                   link.interval_us / 1000, (link.interval_us % 1000) / 10);
        printf("\n");
    } else {
        printf("nothing is connected\n");
    }
    return 0;
}

static int do_scan(int seconds) {
    kbtadapter_t a;
    if (bt_adapter(&a) < 0 || !a.present) {
        printf("bt: no Bluetooth adapter\n");
        return 1;
    }

    printf("listening for %d second(s) on both radios...\n", seconds);
    flush_output();

    int found = bt_scan(seconds);
    if (found < 0) {
        printf("bt: the adapter would not scan: %s\n", strerror(errno));
        return 1;
    }
    if (!found) {
        printf("nothing answered.\n");
        printf(A_GREY "  A device has to be advertising to be found - most only\n"
                      "  do that for a minute or two after being put into\n"
                      "  pairing mode.\n" A_RESET);
        return 0;
    }

    printf("\n%-18s  %-4s  %-18s  %s\n", "address", "kind", "name", "signal");
    for (int i = 0; i < found; i++) {
        kbtdevice_t d;
        if (bt_found(i, &d) < 0) break;

        print_address(d.address);
        printf("  %-4s  %-18s  ",
               d.low_energy ? "LE" : "BR",
               d.name[0] ? d.name : "-");

        if (d.rssi != 127) printf("%d dBm", d.rssi);
        else printf("-");

        if (d.kind[0]) printf("   (%s)", d.kind);
        printf("\n");
    }

    printf("\n" A_GREY "  bt connect <address> to open a link.\n" A_RESET);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return show_adapter();

    if (!strcmp(argv[1], "scan")) {
        int seconds = argc > 2 ? atoi(argv[2]) : 6;
        if (seconds < 1) seconds = 1;
        if (seconds > 30) seconds = 30;
        return do_scan(seconds);
    }

    if (!strcmp(argv[1], "connect")) {
        if (argc < 3) {
            printf("usage: bt connect aa:bb:cc:dd:ee:ff\n");
            return 1;
        }
        unsigned char addr[6];
        if (parse_address(argv[2], addr) < 0) {
            printf("bt: %s is not an address\n", argv[2]);
            return 1;
        }

        printf("opening a link to %s...\n", argv[2]);
        flush_output();

        if (bt_connect(addr) < 0) {
            printf(A_RED "bt: the link did not open" A_RESET "  See "
                   A_BOLD "events" A_RESET " for what the adapter said.\n");
            return 1;
        }
        printf(A_GREEN "connected." A_RESET "\n");
        return show_adapter();
    }

    if (!strcmp(argv[1], "disconnect")) {
        if (bt_disconnect() < 0) {
            printf("bt: %s\n", strerror(errno));
            return 1;
        }
        printf("disconnected.\n");
        return 0;
    }

    printf("usage: bt [scan [seconds] | connect <address> | disconnect]\n");
    return 1;
}
