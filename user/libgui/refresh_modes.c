/* refresh_modes.c - see refresh_modes.h. */
#include "refresh_modes.h"

/* The refresh rates a PC display commonly runs at, in millihertz.  A panel is
 * offered the ones at or below the rate it says it can reach; nothing here is
 * invented for a panel that never claimed it.  60 Hz is written as its exact
 * broadcast value, 59.94, because that is what panels report and what the
 * "current" rate will equal - so it must compare equal rather than showing up
 * as a second, near-duplicate chip beside a clean 60000. */
static const uint32_t STANDARD_MHZ[] = {
    59940, 75000, 100000, 120000, 144000, 165000, 200000, 240000,
};

/* A panel that says it tops out at 60 Hz reports 59.94 or 60.00 depending on
 * where the number came from; a rate within this much of the ceiling is "at"
 * it, not above it, so a 59.94 rate is not filtered out by a 60000 ceiling. */
#define NEAR_MHZ 1500

static int already(const uint32_t *out, int n, uint32_t v) {
    for (int i = 0; i < n; i++)
        if (out[i] == v) return 1;
    return 0;
}

static void insert_sorted(uint32_t *out, int *n, int cap, uint32_t v) {
    if (v == 0 || *n >= cap || already(out, *n, v)) return;
    int i = *n;
    while (i > 0 && out[i - 1] > v) { out[i] = out[i - 1]; i--; }
    out[i] = v;
    (*n)++;
}

int refresh_options(uint32_t max_mhz, uint32_t cur_mhz, uint32_t *out, int cap) {
    int n = 0;
    if (cap <= 0) return 0;

    /* The current rate is always a choice, whatever the ceiling says. */
    insert_sorted(out, &n, cap, cur_mhz);

    /* Every standard rate the panel says it can reach.  With no ceiling known,
     * offer only what is certainly safe - the current rate alone - rather than
     * guess a panel into rates it never claimed. */
    if (max_mhz) {
        for (unsigned i = 0; i < sizeof STANDARD_MHZ / sizeof STANDARD_MHZ[0]; i++)
            if (STANDARD_MHZ[i] <= max_mhz + NEAR_MHZ)
                insert_sorted(out, &n, cap, STANDARD_MHZ[i]);
    }

    return n;
}
