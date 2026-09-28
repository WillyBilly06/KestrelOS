/* arm64_test.c - checking the interpreter against a real compiler.
 *
 * The code being run here was not written by hand. tests/arm/probe.c is
 * ordinary C, compiled with optimisation by the ordinary toolchain for a
 * processor this machine does not have, and the bytes that came out are what
 * gets executed - whatever the compiler felt like emitting, including the
 * instructions nobody would have thought to test.
 *
 * Each function is called the way the architecture says: arguments in x0
 * onwards, the answer in x0, and a return address in x30 pointing at somewhere
 * recognisable so that the interpreter can tell when the function has
 * finished. The answers are then compared against what the same C computes
 * here, natively. Two different processors agreeing on the answer is the whole
 * test.
 */
#include "arm64.h"
#include "arm_probe.h"

/* Where the program's memory begins, and how it is laid out: the code first,
 * then room for a stack, then a little space for data the tests pass in. */
#define BASE      0x100000ull
#define STACK_TOP (BASE + 0x8000)
#define DATA_AT   (BASE + 0x9000)
#define TOTAL     0xA000

/* A return address that is deliberately not code.  When the program returns to
 * it, the interpreter stops, which is how a call is known to have finished. */
#define DONE      (BASE + 0xF000)

static uint8_t memory[TOTAL];

/* What the program asked this system for, kept so the test can check it got
 * there rather than trusting that the request was made. */
static struct {
    char   text[256];
    int    length;
    int    status;
    bool   finished;
    int    refused;
} heard;

/* The system underneath the program.  Two requests are answered - writing
 * something out and finishing - because those are the two a program cannot do
 * without.  Anything else is counted and refused, which is what a program
 * should be told rather than being left to run into nothing. */
static bool answer(arm64_cpu_t *cpu, uint32_t what, void *ctx) {
    (void)what; (void)ctx;
    uint64_t number = cpu->x[8];

    if (number == 64) {                     /* write */
        uint64_t at = cpu->x[1];
        int64_t  len = (int64_t)cpu->x[2];
        if (len < 0 || at < cpu->memory_base ||
            at + (uint64_t)len > cpu->memory_base + cpu->memory_size) {
            cpu->x[0] = (uint64_t)-1;
            return true;
        }
        const char *from = (const char *)cpu->memory + (at - cpu->memory_base);
        for (int64_t i = 0; i < len && heard.length < (int)sizeof heard.text; i++)
            heard.text[heard.length++] = from[i];
        cpu->x[0] = (uint64_t)len;
        return true;
    }

    if (number == 93 || number == 94) {     /* finish, one thread or all */
        heard.status = (int)(cpu->x[0] & 0xFF);
        heard.finished = true;
        return false;                       /* the program has ended */
    }

    heard.refused++;
    cpu->x[0] = (uint64_t)-38;              /* no such request */
    return true;
}

static uint64_t call(arm64_cpu_t *cpu, uint64_t at, const uint64_t *args,
                     int count, const char **why) {
    arm64_init(cpu, memory, BASE, TOTAL);
    cpu->pc = BASE + at;
    cpu->sp = STACK_TOP;
    cpu->x[30] = DONE;
    for (int i = 0; i < count && i < 8; i++) cpu->x[i] = args[i];

    while (!cpu->stopped) {
        if (cpu->pc == DONE) break;            /* the function returned */
        if (!arm64_step(cpu)) break;
        if (cpu->executed > 2000000) {
            *why = "it never finished";
            return 0;
        }
    }
    if (cpu->fault) { *why = cpu->fault; return 0; }
    *why = NULL;
    return cpu->x[0];
}

/* Calling something that works in numbers with a fractional part.  Those go
 * in their own registers rather than the ordinary ones, and the answer comes
 * back in the first of them - so nothing about this call looks like the ones
 * above, even though it is the same processor. */
static double call_double(arm64_cpu_t *cpu, uint64_t at, const double *args,
                          int count, const char **why) {
    arm64_init(cpu, memory, BASE, TOTAL);
    cpu->pc = BASE + at;
    cpu->sp = STACK_TOP;
    cpu->x[30] = DONE;
    for (int i = 0; i < count && i < 8; i++)
        memcpy(&cpu->vreg[i][0], &args[i], sizeof(double));

    while (!cpu->stopped) {
        if (cpu->pc == DONE) break;
        if (!arm64_step(cpu)) break;
        if (cpu->executed > 2000000) { *why = "it never finished"; return 0; }
    }
    if (cpu->fault) { *why = cpu->fault; return 0; }
    *why = NULL;
    double out;
    memcpy(&out, &cpu->vreg[0][0], sizeof out);
    return out;
}

/* Two numbers agreeing to within what the last few bits can hold apart. */
static bool near_enough(double got, double want) {
    double gap = got - want;
    if (gap < 0) gap = -gap;
    double scale = want < 0 ? -want : want;
    if (scale < 1.0) scale = 1.0;
    return gap <= scale * 1e-12;
}

/* The same, but with a system underneath for the program to ask things of. */
static uint64_t call_with_system(arm64_cpu_t *cpu, uint64_t at,
                                 const uint64_t *args, int count,
                                 const char **why) {
    arm64_init(cpu, memory, BASE, TOTAL);
    cpu->pc = BASE + at;
    cpu->sp = STACK_TOP;
    cpu->x[30] = DONE;
    cpu->on_call = answer;
    for (int i = 0; i < count && i < 8; i++) cpu->x[i] = args[i];

    while (!cpu->stopped) {
        if (cpu->pc == DONE) break;
        if (!arm64_step(cpu)) break;
        if (cpu->executed > 2000000) { *why = "it never finished"; return 0; }
    }
    if (cpu->fault) { *why = cpu->fault; return 0; }
    *why = NULL;
    return cpu->x[0];
}

/* The same computations, natively, so there is something to compare against. */
static double native_fp_arithmetic(double a, double b) {
    return a * b + a / b - (a - b);
}
static double native_fp_widths(double a, float b) {
    float narrow = (float)a * b;
    return (double)narrow + a;
}

static int native_arithmetic(int a, int b) { return a * b + 7; }
static int native_sum_to(int n) { int t = 0; for (int i = 1; i <= n; i++) t += i; return t; }
static int native_divide(int a, int b) { return (a / b) * 100 + (a % b); }
static int native_helper(int x) { return x * 3 - 1; }
static int native_nested(int x) { return native_helper(native_helper(x)) + native_helper(x); }
static int native_widen(int packed) {
    signed char low = (signed char)(packed & 0xFF);
    unsigned short high = (unsigned short)((packed >> 8) & 0xFFFF);
    return (int)low + (int)high;
}
static uint64_t native_bits(uint64_t value) {
    uint64_t out = 0;
    out |= (value & 0xFF) << 24;
    out |= (value >> 8) & 0xFF00;
    out ^= 0x5555555555555555ull;
    out &= ~0xF0F0ull;
    return out;
}
static uint64_t native_shifts(uint64_t value, int by) {
    return (value << by) | (value >> (64 - by));
}
static int native_choose(int which, int a, int b) {
    switch (which) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return a < b ? a : b;
    default: return -1;
    }
}

static int failures;
static int checks;

static void expect(const char *what, uint64_t got, uint64_t want, const char *why) {
    checks++;
    if (why) {
        printf("  FAIL %s: %s\n", what, why);
        failures++;
        return;
    }
    if (got != want) {
        printf("  FAIL %s: got %llu, expected %llu\n", what,
               (unsigned long long)got, (unsigned long long)want);
        failures++;
        return;
    }
    printf("  ok   %s\n", what);
}

int arm64_selftest(void) {
    arm64_cpu_t cpu;
    const char *why;
    uint64_t args[4];
    char note[96];

    failures = 0;
    checks = 0;

    printf("arm: running %d bytes of real AArch64 code\n", ARM_PROBE_SIZE);

    memset(memory, 0, sizeof memory);
    memcpy(memory, arm_probe_code, ARM_PROBE_SIZE);

    /* Arithmetic, which the compiler folded into a multiply-add. */
    args[0] = (uint64_t)(int64_t)6;
    args[1] = (uint64_t)(int64_t)7;
    uint64_t got = call(&cpu, ARM_AT_arithmetic, args, 2, &why);
    expect("multiply and add", (uint32_t)got, (uint32_t)native_arithmetic(6, 7), why);

    /* A loop, which is where the flags and the conditional branch matter. */
    args[0] = 100;
    got = call(&cpu, ARM_AT_sum_to, args, 1, &why);
    expect("a counted loop", (uint32_t)got, (uint32_t)native_sum_to(100), why);

    args[0] = (uint64_t)(int64_t)-5;
    got = call(&cpu, ARM_AT_sum_to, args, 1, &why);
    expect("a loop that never runs", (uint32_t)got, (uint32_t)native_sum_to(-5), why);

    /* Division, both signs. */
    args[0] = (uint64_t)(int64_t)47; args[1] = (uint64_t)(int64_t)5;
    got = call(&cpu, ARM_AT_divide, args, 2, &why);
    expect("divide and remainder", (uint32_t)got, (uint32_t)native_divide(47, 5), why);

    args[0] = (uint64_t)(int64_t)-47; args[1] = (uint64_t)(int64_t)5;
    got = call(&cpu, ARM_AT_divide, args, 2, &why);
    expect("a negative divide", (uint32_t)got, (uint32_t)native_divide(-47, 5), why);

    /* Memory: an array walked with a scaled index. */
    {
        static const int values[8] = { 3, 19, -4, 7, 42, 11, 0, 8 };
        int *in_program = (int *)(memory + (DATA_AT - BASE));
        for (int i = 0; i < 8; i++) in_program[i] = values[i];

        args[0] = DATA_AT;
        args[1] = 8;
        got = call(&cpu, ARM_AT_walk, args, 2, &why);
        expect("walking an array in memory", (uint32_t)got, 42u, why);
    }

    /* Bit work, which exercises the mask immediates and the bitfield moves. */
    args[0] = 0x0123456789ABCDEFull;
    got = call(&cpu, ARM_AT_bits, args, 1, &why);
    expect("masks and shifts", got, native_bits(0x0123456789ABCDEFull), why);

    /* Calls, which means the link register and the stack really work. */
    args[0] = 5;
    got = call(&cpu, ARM_AT_nested, args, 1, &why);
    expect("nested calls", (uint32_t)got, (uint32_t)native_nested(5), why);

    /* Shifts by a register, sixty-four bits wide. */
    args[0] = 0xDEADBEEFCAFEF00Dull;
    args[1] = 13;
    got = call(&cpu, ARM_AT_shifts, args, 2, &why);
    expect("a rotate by a register", got,
           native_shifts(0xDEADBEEFCAFEF00Dull, 13), why);

    /* Widening, signed and unsigned. */
    args[0] = (uint64_t)(int64_t)0x1234F0;
    got = call(&cpu, ARM_AT_widen, args, 1, &why);
    expect("signed and unsigned widening", (uint32_t)got,
           (uint32_t)native_widen(0x1234F0), why);

    /* A switch, which the compiler turns into whatever it likes. */
    for (int which = 0; which < 5; which++) {
        args[0] = (uint64_t)(int64_t)which;
        args[1] = 20;
        args[2] = 6;
        got = call(&cpu, ARM_AT_choose, args, 3, &why);
        snprintf(note, sizeof note, "a switch, branch %d", which);
        expect(note, (uint32_t)got, (uint32_t)native_choose(which, 20, 6), why);
    }

    /* ------------------------------------------- asking the system for things
     *
     * Everything above is the interpreter computing.  This is a program built
     * for ARM doing what a program is for: writing something out and finishing
     * with a status.  Neither is arithmetic - both leave the program entirely,
     * and what answers is this system.
     *
     * The request numbers are the ones Linux uses on this architecture,
     * because that is what an ordinary toolchain builds a program to expect.
     * Nothing here pretends to be Linux; it answers the two requests a program
     * cannot do without, and says plainly when it is asked for anything else.
     */
    {
        static const char message[] = "hello from a processor this machine "
                                      "does not have";
        const int length = (int)sizeof message - 1;

        char *in_program = (char *)(memory + (DATA_AT - BASE));
        for (int i = 0; i < length; i++) in_program[i] = message[i];

        heard.length = 0;
        heard.status = -1;
        heard.finished = false;
        heard.refused = 0;

        args[0] = DATA_AT;
        args[1] = (uint64_t)(int64_t)length;
        got = call_with_system(&cpu, ARM_AT_say, args, 2, &why);

        bool same = heard.length == length &&
                    !memcmp(heard.text, message, (size_t)length);
        expect("ARM: a program writes something out",
               same ? 1u : 0u, 1u, why);
        expect("ARM: and is told how much of it went",
               (uint32_t)got, (uint32_t)length, why);

        /* And finishing: the program says how it went and stops.  What comes
         * back is not a return value - the program never returns. */
        heard.length = 0;
        heard.finished = false;
        args[0] = DATA_AT;
        args[1] = (uint64_t)(int64_t)length;
        args[2] = 3;
        call_with_system(&cpu, ARM_AT_say_and_finish, args, 3, &why);

        expect("ARM: a program finishes with a status",
               heard.finished && heard.status == 3 ? 1u : 0u, 1u, why);
        expect("ARM: and wrote its message before it did",
               heard.length == length ? 1u : 0u, 1u, why);
    }

    /* ------------------------------------------------- floating point
     *
     * Numbers with a fractional part: their own registers, their own
     * instructions, and their own way of being passed to a function.  Both
     * processors follow the same standard for what a number is, so the answers
     * should agree to the last few bits rather than approximately.
     */
    {
        double fargs[2];
        double fgot;

        fargs[0] = 7.25; fargs[1] = 3.5;
        fgot = call_double(&cpu, ARM_AT_fp_arithmetic, fargs, 2, &why);
        expect("ARM: arithmetic on fractional numbers",
               near_enough(fgot, native_fp_arithmetic(7.25, 3.5)) ? 1u : 0u,
               1u, why);

        fargs[0] = -0.125; fargs[1] = 1024.5;
        fgot = call_double(&cpu, ARM_AT_fp_arithmetic, fargs, 2, &why);
        expect("ARM: and on negative ones",
               near_enough(fgot, native_fp_arithmetic(-0.125, 1024.5)) ? 1u : 0u,
               1u, why);

        /* Both widths at once, which means converting between them.  The
         * second argument is the narrow kind and sits in its own register. */
        {
            arm64_cpu_t c2;
            arm64_init(&c2, memory, BASE, TOTAL);
            c2.pc = BASE + ARM_AT_fp_widths;
            c2.sp = STACK_TOP;
            c2.x[30] = DONE;
            double a = 2.5;
            float b = 0.75f;
            memcpy(&c2.vreg[0][0], &a, sizeof a);
            uint32_t bbits;
            memcpy(&bbits, &b, sizeof b);
            c2.vreg[1][0] = bbits;
            const char *w2 = NULL;
            while (!c2.stopped) {
                if (c2.pc == DONE) break;
                if (!arm64_step(&c2)) break;
                if (c2.executed > 2000000) { w2 = "it never finished"; break; }
            }
            if (c2.fault) w2 = c2.fault;
            double out = 0;
            memcpy(&out, &c2.vreg[0][0], sizeof out);
            expect("ARM: the narrow kind and the wide kind together",
                   near_enough(out, native_fp_widths(2.5, 0.75f)) ? 1u : 0u,
                   1u, w2);
        }

        /* Comparing, and the branch that follows from it. */
        static const struct { double a, b; int want; } order[3] = {
            { 1.5, 2.5, -1 }, { 2.5, 1.5, 1 }, { 4.0, 4.0, 0 },
        };
        for (int i = 0; i < 3; i++) {
            fargs[0] = order[i].a; fargs[1] = order[i].b;
            arm64_init(&cpu, memory, BASE, TOTAL);
            cpu.pc = BASE + ARM_AT_fp_order;
            cpu.sp = STACK_TOP;
            cpu.x[30] = DONE;
            memcpy(&cpu.vreg[0][0], &fargs[0], sizeof(double));
            memcpy(&cpu.vreg[1][0], &fargs[1], sizeof(double));
            why = NULL;
            while (!cpu.stopped) {
                if (cpu.pc == DONE) break;
                if (!arm64_step(&cpu)) break;
                if (cpu.executed > 2000000) { why = "it never finished"; break; }
            }
            if (cpu.fault) why = cpu.fault;
            snprintf(note, sizeof note, "ARM: comparing %d", i);
            expect(note, (uint32_t)(int32_t)cpu.x[0],
                   (uint32_t)(int32_t)order[i].want, why);
        }

        /* And crossing between whole and fractional, both ways. */
        args[0] = 100;
        got = call(&cpu, ARM_AT_fp_from_whole, args, 1, &why);
        {
            double out;
            memcpy(&out, &cpu.vreg[0][0], sizeof out);
            expect("ARM: a whole number becomes a fractional one",
                   near_enough(out, 12.5) ? 1u : 0u, 1u, why);
        }

        fargs[0] = 3.14159;
        arm64_init(&cpu, memory, BASE, TOTAL);
        cpu.pc = BASE + ARM_AT_fp_to_whole;
        cpu.sp = STACK_TOP;
        cpu.x[30] = DONE;
        memcpy(&cpu.vreg[0][0], &fargs[0], sizeof(double));
        why = NULL;
        while (!cpu.stopped) {
            if (cpu.pc == DONE) break;
            if (!arm64_step(&cpu)) break;
            if (cpu.executed > 2000000) { why = "it never finished"; break; }
        }
        if (cpu.fault) why = cpu.fault;
        expect("ARM: and a fractional one becomes whole",
               (uint32_t)cpu.x[0], 3141u, why);
    }

    /* ----------------------------------------------------------- vectors
     *
     * Nothing in these functions mentions a vector.  They are ordinary loops
     * over ordinary arrays, and the compiler turned them into instructions
     * that work on four or eight numbers at a time because it was allowed to.
     * That is why this matters more than it looks: a program does not have to
     * ask for any of this to depend on it.
     */
    {
        static const int values[16] = { 3, 19, -4, 7, 42, 11, 0, 8,
                                        1, 2, 3, 4, 5, 6, 7, 100 };
        int *in_program = (int *)(memory + (DATA_AT - BASE));
        for (int i = 0; i < 16; i++) in_program[i] = values[i];

        int want = 0;
        for (int i = 0; i < 16; i++) want += values[i];

        args[0] = DATA_AT;
        args[1] = 16;
        got = call(&cpu, ARM_AT_vector_sum, args, 2, &why);
        if (cpu.fault)
            printf("     (the instruction it stopped on was %08x)\n",
                   (unsigned)cpu.fault_addr);
        expect("ARM: a loop the compiler turned into vector work",
               (uint32_t)got, (uint32_t)want, why);

        /* And one that writes an array rather than reducing it. */
        int *out_program = (int *)(memory + (DATA_AT - BASE) + 256);
        for (int i = 0; i < 16; i++) out_program[i] = 0;
        args[0] = DATA_AT + 256;
        args[1] = DATA_AT;
        args[2] = 16;
        args[3] = 3;
        call(&cpu, ARM_AT_vector_scale, args, 4, &why);
        if (cpu.fault)
            printf("     (the instruction it stopped on was %08x)\n",
                   (unsigned)cpu.fault_addr);
        bool all = (why == NULL);
        for (int i = 0; i < 16 && all; i++)
            if (out_program[i] != values[i] * 3) all = false;
        expect("ARM: and one that writes a whole array at a time",
               all ? 1u : 0u, 1u, why);

        /* The same again, in numbers with a fractional part. */
        float *fa = (float *)(memory + (DATA_AT - BASE) + 512);
        float *fb = (float *)(memory + (DATA_AT - BASE) + 640);
        float want_dot = 0;
        for (int i = 0; i < 16; i++) {
            fa[i] = (float)(i + 1) * 0.5f;
            fb[i] = (float)(16 - i) * 0.25f;
            want_dot += fa[i] * fb[i];
        }
        arm64_init(&cpu, memory, BASE, TOTAL);
        cpu.pc = BASE + ARM_AT_vector_dot;
        cpu.sp = STACK_TOP;
        cpu.x[30] = DONE;
        cpu.x[0] = DATA_AT + 512;
        cpu.x[1] = DATA_AT + 640;
        cpu.x[2] = 16;
        why = NULL;
        while (!cpu.stopped) {
            if (cpu.pc == DONE) break;
            if (!arm64_step(&cpu)) break;
            if (cpu.executed > 2000000) { why = "it never finished"; break; }
        }
        if (cpu.fault) {
            why = cpu.fault;
            printf("     (the instruction it stopped on was %08x)\n",
                   (unsigned)cpu.fault_addr);
        }
        float dot_got = 0;
        {
            uint32_t bits = (uint32_t)cpu.vreg[0][0];
            memcpy(&dot_got, &bits, sizeof dot_got);
        }
        double gap = (double)dot_got - (double)want_dot;
        if (gap < 0) gap = -gap;
        expect("ARM: vector work on fractional numbers",
               gap <= 0.01 ? 1u : 0u, 1u, why);
    }

    printf("arm: %d checks, %d failed\n", checks, failures);
    return failures;
}
