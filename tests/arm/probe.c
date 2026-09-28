/* probe.c - built for ARM, run by the interpreter.
 *
 * Compiled by the ordinary toolchain into real AArch64 machine code - the same
 * bytes an ARM processor would be given - and then executed one instruction at
 * a time by user/libarm.  Nothing here is arranged to be easy: it is ordinary
 * C, compiled with optimisation, so what comes out is whatever the compiler
 * felt like emitting.  That is the point.  A test written in hand-picked
 * instructions proves the interpreter handles hand-picked instructions.
 */
typedef unsigned long long u64;
typedef unsigned int u32;

/* Arithmetic, and a multiply-add, which the compiler folds into one
 * instruction. */
int arithmetic(int a, int b) { return a * b + 7; }

/* A loop with a comparison and a branch, which is where the flags matter. */
int sum_to(int n) {
    int total = 0;
    for (int i = 1; i <= n; i++) total += i;
    return total;
}

/* Division, both ways, and the remainder. */
int divide(int a, int b) { return (a / b) * 100 + (a % b); }

/* Memory: a walk over an array, which is loads with a scaled index. */
int walk(const int *values, int count) {
    int best = values[0];
    for (int i = 1; i < count; i++) if (values[i] > best) best = values[i];
    return best;
}

/* Bit work, which is where the bitfield instructions and the mask immediates
 * come out. */
u64 bits(u64 value) {
    u64 out = 0;
    out |= (value & 0xFF) << 24;
    out |= (value >> 8) & 0xFF00;
    out ^= 0x5555555555555555ull;
    out &= ~0xF0F0ull;
    return out;
}

/* Calls, which means the link register and the stack. */
static int helper(int x) { return x * 3 - 1; }
int nested(int x) { return helper(helper(x)) + helper(x); }

/* Sixty-four bit shifts by a register, and a rotate the compiler will spell
 * with two shifts or one. */
u64 shifts(u64 value, int by) {
    return (value << by) | (value >> (64 - by));
}

/* Signed and unsigned widening, which is the bitfield-move family. */
int widen(int packed) {
    signed char low = (signed char)(packed & 0xFF);
    unsigned short high = (unsigned short)((packed >> 8) & 0xFFFF);
    return (int)low + (int)high;
}

/* And a switch, which becomes a table of branches or a chain of compares. */
int choose(int which, int a, int b) {
    switch (which) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return a < b ? a : b;
    default: return -1;
    }
}

/* ------------------------------------------------------ asking for something
 *
 * Everything above computes.  This does not: it asks the system underneath for
 * something, which on this architecture means putting the request number in x8
 * and the arguments in x0 onwards and executing one instruction.  What answers
 * is not a processor at all.
 *
 * The numbers are the ones Linux uses on this architecture, because a program
 * built for ARM by an ordinary toolchain is built expecting those.
 */
static inline long ask(long number, long a, long b, long c) {
    register long x8 asm("x8") = number;
    register long x0 asm("x0") = a;
    register long x1 asm("x1") = b;
    register long x2 asm("x2") = c;
    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x8), "r"(x1), "r"(x2)
                 : "memory", "cc");
    return x0;
}

/* Write some text out, and report how much of it went. */
int say(const char *text, int length) {
    return (int)ask(64, 1, (long)text, length);
}

/* Write, then finish with a status - the shape of every program that has ever
 * run: do something, say how it went. */
int say_and_finish(const char *text, int length, int status) {
    ask(64, 1, (long)text, length);
    ask(93, status, 0, 0);
    return -1;                  /* not reached: the program has ended */
}

/* ------------------------------------------------------- floating point
 *
 * Numbers with a fractional part are a separate set of registers and a
 * separate set of instructions, and a compiler reaches for them the moment a
 * program says double or float.  Everything above could be done without them;
 * none of this can.
 */
double fp_arithmetic(double a, double b) {
    return a * b + a / b - (a - b);
}

/* Mixing the two widths, which means converting between them. */
double fp_widths(double a, float b) {
    float narrow = (float)a * b;
    return (double)narrow + a;
}

/* Comparing, which sets the ordinary flags and is branched on the ordinary
 * way - so this is really a test of the comparison reaching the branch. */
int fp_order(double a, double b) {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

/* Crossing between whole numbers and fractional ones, in both directions. */
double fp_from_whole(int n) { return (double)n / 8.0; }
int fp_to_whole(double a) { return (int)(a * 1000.0); }

/* --------------------------------------------------------------- vectors
 *
 * Given a loop like this and permission to optimise, a compiler does not emit
 * a loop that adds one number at a time.  It emits instructions that add four
 * or eight at once, on the wide registers - which is a third instruction set
 * again, on top of the whole numbers and the fractional ones.
 */
int vector_sum(const int *values, int count) {
    int total = 0;
    for (int i = 0; i < count; i++) total += values[i];
    return total;
}

void vector_scale(int *out, const int *in, int count, int by) {
    for (int i = 0; i < count; i++) out[i] = in[i] * by;
}

float vector_dot(const float *a, const float *b, int count) {
    float total = 0;
    for (int i = 0; i < count; i++) total += a[i] * b[i];
    return total;
}
