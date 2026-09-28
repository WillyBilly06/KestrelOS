/* arm64.h - running code written for a different processor.
 *
 * This machine is x86-64. A program built for ARM is not a different version
 * of the same thing - it is a different instruction set, and the only way to
 * run one is to read its instructions and do what they say.
 *
 * That is what this is: an interpreter for AArch64. Every register the
 * architecture defines is a field in a structure here, every instruction is
 * decoded and carried out, and the flags come out of each operation the way
 * the architecture says they must. A program cannot tell the difference except
 * in speed.
 *
 * See arm64.c.
 */
#ifndef KESTREL_ARM64_H
#define KESTREL_ARM64_H

#include "kestrel.h"

#define ARM64_REGS 31          /* x0 to x30; x31 reads as zero or is the stack */

typedef struct arm64_cpu arm64_cpu_t;

/* What a program does when it wants something from the system underneath it.
 * On real hardware that is an exception into a kernel; here it is a call back
 * into whatever is hosting the program. Returning false stops the program. */
typedef bool (*arm64_call_t)(arm64_cpu_t *cpu, uint32_t imm, void *ctx);

struct arm64_cpu {
    uint64_t x[ARM64_REGS];
    uint64_t sp;
    uint64_t pc;

    /* The four condition flags, held one per bit so that reading them is a
     * shift rather than a branch: negative, zero, carry, overflow. */
    bool n, z, c, v;

    /* The two hundred and fifty-six bit vector registers.  Held as pairs of
     * sixty-four bit halves, which is how every instruction that touches them
     * addresses them anyway. */
    uint64_t vreg[32][2];

    /* Where the program's memory is, as seen from here.  An address the
     * program uses is an offset into this. */
    uint8_t *memory;
    uint64_t memory_base;      /* what the program calls memory[0] */
    uint64_t memory_size;

    /* The exclusive monitor, which is how ARM builds a lock: a load marks an
     * address, and the matching store succeeds only if nothing disturbed it. */
    bool     exclusive;
    uint64_t exclusive_addr;

    arm64_call_t on_call;      /* SVC and friends */
    void        *call_ctx;

    bool     stopped;
    const char *fault;         /* why it stopped, when it stopped badly */
    uint64_t fault_addr;
    uint64_t executed;         /* how many instructions have been run */
};

void arm64_init(arm64_cpu_t *cpu, uint8_t *memory, uint64_t base, uint64_t size);

/* Run until it stops, or until `budget` instructions have gone by - which is
 * what stops a program that has run away from taking the machine with it. */
void arm64_run(arm64_cpu_t *cpu, uint64_t budget);

/* One instruction.  Returns false when the processor stopped. */
bool arm64_step(arm64_cpu_t *cpu);

/* What an instruction is, in words - for a fault message that can be acted on
 * rather than a number. */
void arm64_describe(uint32_t instruction, char *out, size_t cap);

int arm64_selftest(void);

#endif
