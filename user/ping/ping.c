/* ping - send echo requests and report what comes back.
 *
 *   ping <host> [count]
 *
 * The host may be an address or a name; a name is resolved first, which also
 * makes this the quickest way to find out whether name resolution works.
 */
#include "kestrel.h"

#define MAX_COUNT 32

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: ping <host> [count]\n");
        return 1;
    }

    int count = (argc > 2) ? atoi(argv[2]) : 4;
    if (count < 1) count = 1;
    if (count > MAX_COUNT) count = MAX_COUNT;

    uint32_t ip = net_resolve(argv[1], 5000);
    if (!ip) {
        printf("ping: cannot resolve %s\n", argv[1]);
        printf("      `net` shows whether an interface is configured.\n");
        return 1;
    }

    char text[20];
    format_ipv4(ip, text, sizeof text);
    if (strcmp(argv[1], text))
        printf("PING %s (%s), 56 bytes of data\n", argv[1], text);
    else
        printf("PING %s, 56 bytes of data\n", text);

    uint32_t times[MAX_COUNT];
    int replies = net_ping(ip, count, 2000, times);

    if (replies < 0) {
        printf("ping: no interface is configured - try `net up`\n");
        return 1;
    }

    uint32_t total = 0, best = 0xFFFFFFFF, worst = 0;
    for (int i = 0; i < replies; i++) {
        printf("  reply from %s: seq=%d time=%u ms\n", text, i + 1, times[i]);
        total += times[i];
        if (times[i] < best) best = times[i];
        if (times[i] > worst) worst = times[i];
    }

    int lost = count - replies;
    printf("\n%d sent, %d received, %d%% lost\n",
           count, replies, count ? lost * 100 / count : 0);

    if (replies)
        printf("round trip: least %u ms, average %u ms, most %u ms\n",
               best, total / (uint32_t)replies, worst);

    return replies ? 0 : 1;
}
