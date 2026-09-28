/* net - look at and configure the network.
 *
 *   net                    what interfaces there are and how they are set up
 *   net up [iface]         ask a DHCP server for an address
 *   net set <ip> <mask> [gateway] [dns]
 *   net arp                who has been seen on the local network
 *   net resolve <name>     look a name up
 *   net stats              packet counters, which is what a fault shows up in
 */
#include "kestrel.h"

static void print_addr(const char *label, uint32_t ip) {
    char text[20];
    if (!ip) { printf("  %-10s -\n", label); return; }
    format_ipv4(ip, text, sizeof text);
    printf("  %-10s %s\n", label, text);
}

static void show_one(const knetinfo_t *n, bool detail) {
    char mac[24];
    format_mac(n->mac, mac, sizeof mac);

    printf("%s: %s\n", n->name, n->model);
    printf("  %-10s %s\n", "hardware", mac);

    if (n->link_up)
        printf("  %-10s up, %u Mb/s\n", "link", n->link_speed_mbps);
    else
        printf("  %-10s down\n", "link");

    if (n->configured) {
        print_addr("address", n->ip);
        print_addr("netmask", n->netmask);
        print_addr("gateway", n->gateway);
        print_addr("dns", n->dns);
    } else {
        printf("  %-10s not configured - try `net up`\n", "address");
    }

    if (detail) {
        printf("  %-10s %llu packets, %llu bytes, %llu dropped, %llu errors\n",
               "received", (unsigned long long)n->rx_packets,
               (unsigned long long)n->rx_bytes,
               (unsigned long long)n->rx_dropped,
               (unsigned long long)n->rx_errors);
        printf("  %-10s %llu packets, %llu bytes, %llu dropped, %llu errors\n",
               "sent", (unsigned long long)n->tx_packets,
               (unsigned long long)n->tx_bytes,
               (unsigned long long)n->tx_dropped,
               (unsigned long long)n->tx_errors);
    }
    printf("\n");
}

static int show_all(bool detail) {
    knetinfo_t n;
    int count = 0;

    for (uint32_t i = 0; ; i++) {
        if (enum_net(i, &n) < 0) break;
        show_one(&n, detail);
        count++;
    }

    if (!count) {
        printf("No network interface.\n\n");
        printf("There are drivers here for Intel gigabit cards and the Realtek\n");
        printf("RTL8139.  Wi-Fi is not supported at all: a wireless card needs a\n");
        printf("vendor driver, signed firmware and an 802.11 stack, none of which\n");
        printf("are in this system.\n");
    }
    return count ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) return show_all(false);

    if (!strcmp(argv[1], "stats")) return show_all(true);

    if (!strcmp(argv[1], "up")) {
        const char *iface = (argc > 2) ? argv[2] : "";
        printf("net: asking for an address...\n");

        int r = net_dhcp(iface, 8000);
        if (r < 0) {
            printf("net: no DHCP server answered.\n");
            printf("     `net set <ip> <mask> [gateway] [dns]` configures it by hand.\n");
            return 1;
        }
        return show_all(false);
    }

    if (!strcmp(argv[1], "set")) {
        if (argc < 4) {
            printf("usage: net set <ip> <mask> [gateway] [dns]\n");
            return 1;
        }
        uint32_t ip, mask, gw = 0, dns = 0;
        if (!parse_ipv4(argv[2], &ip) || !parse_ipv4(argv[3], &mask)) {
            printf("net: that is not an address\n");
            return 1;
        }
        if (argc > 4 && !parse_ipv4(argv[4], &gw)) {
            printf("net: that is not a gateway address\n");
            return 1;
        }
        if (argc > 5 && !parse_ipv4(argv[5], &dns)) {
            printf("net: that is not a name server address\n");
            return 1;
        }
        if (net_set_address("", ip, mask, gw, dns) < 0) {
            printf("net: no interface to configure\n");
            return 1;
        }
        return show_all(false);
    }

    if (!strcmp(argv[1], "arp")) {
        karpentry_t e;
        int count = 0;
        printf("%-16s %s\n", "ADDRESS", "HARDWARE");
        for (uint32_t i = 0; ; i++) {
            if (enum_arp(i, &e) < 0) break;
            char ip[20], mac[24];
            format_ipv4(e.ip, ip, sizeof ip);
            format_mac(e.mac, mac, sizeof mac);
            printf("%-16s %s\n", ip, mac);
            count++;
        }
        if (!count) printf("(nothing seen yet)\n");
        return 0;
    }

    if (!strcmp(argv[1], "resolve")) {
        if (argc < 3) { printf("usage: net resolve <name>\n"); return 1; }
        uint32_t ip = net_resolve(argv[2], 5000);
        if (!ip) {
            printf("net: could not resolve %s\n", argv[2]);
            return 1;
        }
        char text[20];
        format_ipv4(ip, text, sizeof text);
        printf("%s is %s\n", argv[2], text);
        return 0;
    }

    printf("usage: net [up [iface] | set <ip> <mask> [gw] [dns] | arp"
           " | resolve <name> | stats]\n");
    return 1;
}
