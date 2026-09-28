/* arm64_host_test.c - run the AArch64 interpreter's conformance suite off-target.
 *
 * user/libarm/arm64.c is a from-scratch AArch64 interpreter - the ARM half of
 * capability #8 (run other x86/ARM software). user/libarm/arm64_test.c feeds it
 * real, optimiser-emitted AArch64 machine code (compiled from tests/arm/probe.c
 * and embedded in arm_probe.h) and checks each function's result against the
 * same C computed natively by THIS compiler. Two different instruction sets
 * agreeing on the answer is the test.
 *
 * This wrapper supplies the one thing the suite lacks for a standalone run - a
 * main() - and pulls in both translation units directly, with a host shim for
 * kestrel.h (tools/armhost). It proves the interpreter core is correct on a
 * build machine, exactly as the Intel/HDA/EDID host tests prove theirs; what it
 * does NOT prove is a full program loading and running live on the OS.
 *
 * Build + run (from repo root):
 *   clang -std=c11 -Wno-unused-function -I tools/armhost -I user/libarm \
 *         tools/arm64_host_test.c -o out/hosttests/arm64
 *   ./out/hosttests/arm64
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "arm64.c"        /* the interpreter under test, verbatim */
#include "arm64_test.c"   /* the conformance suite: arm64_selftest() */

int main(void) {
    int failures = arm64_selftest();
    if (failures)
        printf("\n%d failures  <<< REGRESSION\n", failures);
    else
        printf("\nALL GOOD\n");
    return failures ? 1 : 0;
}
