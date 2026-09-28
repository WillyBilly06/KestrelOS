/* refresh_modes_host_test.c - check refresh_options() off-target.
 *
 * The rule it enforces is small but easy to get subtly wrong: offer the
 * standard rates up to the panel's ceiling, always include the current rate,
 * never duplicate it, sort ascending, and - the trap - treat 59.94 and 60.00
 * as the same rate so a 60 Hz panel does not show two near-identical chips.
 *
 * Build + run (from the repo root):
 *   clang -std=c11 -Wall -Wextra tools/refresh_modes_host_test.c \
 *         user/libgui/refresh_modes.c -Iuser/libgui -o refresh_test && ./refresh_test
 */
#include "refresh_modes.h"
#include <stdio.h>

static int fails = 0;
#define OK(cond, msg) do { if (cond) printf("  PASS %s\n", msg); \
    else { printf("  FAIL %s\n", msg); fails++; } } while (0)

static int has(const uint32_t *a, int n, uint32_t v) {
    for (int i = 0; i < n; i++) if (a[i] == v) return 1;
    return 0;
}
static int sorted_unique(const uint32_t *a, int n) {
    for (int i = 1; i < n; i++) if (a[i] <= a[i - 1]) return 0;
    return 1;
}

int main(void) {
    uint32_t o[16];

    /* 1. A 240 Hz panel currently at 59.94 offers the whole standard ladder. */
    int n = refresh_options(240000, 59940, o, 16);
    printf("test 1: 240Hz panel @59.94 -> %d options\n", n);
    OK(sorted_unique(o, n), "sorted ascending, no duplicates");
    OK(has(o, n, 59940), "includes 59.94 (current)");
    OK(has(o, n, 120000) && has(o, n, 144000) && has(o, n, 240000),
       "includes 120/144/240");
    OK(!has(o, n, 60000), "no bare 60000 beside the 59.94 (not double-counted)");

    /* 2. A 165 Hz panel excludes everything above 165. */
    n = refresh_options(165000, 59940, o, 16);
    printf("test 2: 165Hz panel -> %d options\n", n);
    OK(has(o, n, 165000), "includes 165");
    OK(!has(o, n, 200000) && !has(o, n, 240000), "excludes 200 and 240");

    /* 3. The current rate is offered even if it is odd and above the ladder. */
    n = refresh_options(75000, 74998, o, 16);
    printf("test 3: current 74.998 on a 75Hz panel -> %d options\n", n);
    OK(has(o, n, 74998), "the exact current rate is a choice");
    OK(has(o, n, 59940), "and 59.94 below it");
    OK(sorted_unique(o, n), "still sorted, unique");

    /* 4. No ceiling known: only the current rate, nothing guessed. */
    n = refresh_options(0, 59940, o, 16);
    printf("test 4: unknown ceiling -> %d options\n", n);
    OK(n == 1 && o[0] == 59940, "exactly the current rate, nothing invented");

    /* 5. Nothing known at all: empty, not a crash. */
    n = refresh_options(0, 0, o, 16);
    printf("test 5: nothing known -> %d options\n", n);
    OK(n == 0, "empty list");

    /* 6. A 60 Hz office panel offers only 59.94. */
    n = refresh_options(60000, 59940, o, 16);
    printf("test 6: 60Hz panel -> %d options\n", n);
    OK(n == 1 && o[0] == 59940, "just 59.94, no higher rates");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
