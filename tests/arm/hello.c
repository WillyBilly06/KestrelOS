/* hello.c - a whole program built for ARM.
 *
 * Not a function called by a test: a program, with an entry point, that asks
 * the system underneath it for things and finishes with a status.  It is
 * compiled and linked by the ordinary toolchain for AArch64 into an ordinary
 * executable file, and what runs it on this machine is an interpreter reading
 * its instructions one at a time.
 *
 * Nothing here is arranged to be easy for that interpreter.  This is simply
 * what a small program looks like when it has no library underneath it: the
 * request number goes in x8, the arguments in x0 onwards, and one instruction
 * leaves the program entirely.
 */

static long ask(long number, long a, long b, long c) {
    register long x8 asm("x8") = number;
    register long x0 asm("x0") = a;
    register long x1 asm("x1") = b;
    register long x2 asm("x2") = c;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2)
                 : "memory", "cc");
    return x0;
}

/* Four arguments, for the requests that need them. */
static long ask4(long number, long a, long b, long c, long d) {
    register long x8 asm("x8") = number;
    register long x0 asm("x0") = a;
    register long x1 asm("x1") = b;
    register long x2 asm("x2") = c;
    register long x3 asm("x3") = d;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                 : "memory", "cc");
    return x0;
}

static unsigned long length_of(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void say(const char *text) {
    ask(64, 1, (long)text, (long)length_of(text));
}

/* Something with a loop and some arithmetic in it, so that what is printed
 * depends on the interpreter having actually run the program rather than on
 * the text having been found in the file. */
static int total_to(int n) {
    int total = 0;
    for (int i = 1; i <= n; i++) total += i;
    return total;
}

void _start(void) {
    say("a program built for ARM is running on a machine that is not one\n");

    int sum = total_to(100);
    char digits[8];
    int at = 0;
    int value = sum;
    while (value > 0) { digits[at++] = (char)('0' + value % 10); value /= 10; }

    char line[64];
    int n = 0;
    const char *lead = "it added the numbers to a hundred and got ";
    for (int i = 0; lead[i]; i++) line[n++] = lead[i];
    while (at > 0) line[n++] = digits[--at];
    line[n++] = '\n';
    ask(64, 1, (long)line, n);

    /* Now something a program does rather than computes: open a file, read it,
     * and say what was in it.  Nothing about this is arithmetic - every step
     * leaves the program and comes back with something it could not have
     * worked out for itself. */
    {
        static const char path[] = "/etc/motd";
        /* Relative to nothing in particular, which is what -100 means. */
        long fd = ask4(56, -100, (long)path, 0, 0);
        if (fd < 0) {
            say("could not open the file\n");
        } else {
            char buffer[128];
            long got = ask(63, fd, (long)buffer, sizeof buffer);
            ask(57, fd, 0, 0);

            if (got <= 0) {
                say("the file was empty\n");
            } else {
                /* Count the lines, so that what is printed depends on what was
                 * read rather than on the file having been opened. */
                int lines = 0;
                for (long i = 0; i < got; i++) if (buffer[i] == 10) lines++;

                char out[64];
                int n = 0;
                const char *lead = "it read a file of ";
                for (int i = 0; lead[i]; i++) out[n++] = lead[i];
                char digits[8];
                int at = 0;
                long value = got;
                while (value > 0) { digits[at++] = (char)(48 + value % 10);
                                    value /= 10; }
                while (at > 0) out[n++] = digits[--at];
                const char *tail = " bytes in ";
                for (int i = 0; tail[i]; i++) out[n++] = tail[i];
                out[n++] = (char)(48 + lines);
                const char *end = " lines\n";
                for (int i = 0; end[i]; i++) out[n++] = end[i];
                ask(64, 1, (long)out, n);
            }
        }
    }

    /* And somewhere to put things, asked for rather than declared. */
    {
        long start = ask(214, 0, 0, 0);
        long moved = ask(214, start + 4096, 0, 0);
        if (moved >= start + 4096) {
            /* Use it, so that having been given it means something. */
            char *room = (char *)start;
            for (int i = 0; i < 4096; i++) room[i] = (char)(i & 0xFF);
            int ok = 1;
            for (int i = 0; i < 4096; i++)
                if (room[i] != (char)(i & 0xFF)) ok = 0;
            say(ok ? "it asked for more memory and used it\n"
                   : "the memory it was given did not hold what was put in\n");
        } else {
            say("it was not given any more memory\n");
        }
    }

    /* And a status, which is the only thing the system underneath keeps once
     * the program is gone. */
    ask(93, sum == 5050 ? 0 : 1, 0, 0);

    for (;;) { }        /* not reached */
}
