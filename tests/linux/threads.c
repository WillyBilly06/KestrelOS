/* threads.c - a Linux program that starts threads and locks against them.
 *
 * Built the same way as hello.c: clang, targeting Linux, with optimisation,
 * and left exactly as it came out.  This one exercises the two calls that
 * every threaded Linux program is built on and neither of which has anything
 * to do with arithmetic:
 *
 *   clone - which does not mean what a thread means on most systems.  The
 *           child does not start at a function; it returns from the same call
 *           the parent is returning from, with zero in RAX, on a stack the
 *           program allocated itself.  A system that starts it at an entry
 *           point instead produces a thread that runs the wrong code.
 *
 *   futex - waiting on a word in memory.  Every lock a Linux program takes is
 *           this underneath: spin briefly, and if the word is still held, ask
 *           the kernel to sleep until somebody changes it.
 *
 * The test is deliberately one that a broken lock fails rather than one a
 * broken lock merely slows down: four threads each add to the same counter a
 * few thousand times, and the total is only right if no two of them were ever
 * inside at once.  With no lock at all this loses updates on the first run.
 *
 * Reported by exit status: 0 for all checks, otherwise the first that failed.
 */

typedef unsigned long u64;
typedef long          s64;
typedef unsigned int  u32;

#define SYS_write       1
#define SYS_mmap        9
#define SYS_clone      56
#define SYS_exit       60
#define SYS_futex     202
#define SYS_gettid    186
#define SYS_exit_group 231
#define SYS_sched_yield 24
#define SYS_nanosleep  35

#define FUTEX_WAIT 128          /* with the private flag, as a runtime uses */
#define FUTEX_WAKE 129

#define CLONE_VM            0x00000100
#define CLONE_FS            0x00000200
#define CLONE_FILES         0x00000400
#define CLONE_SIGHAND       0x00000800
#define CLONE_THREAD        0x00010000

#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

static s64 sc(s64 n, s64 a, s64 b, s64 c) {
    s64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

static s64 sc6(s64 n, s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {
    s64 r;
    register s64 r10 __asm__("r10") = d;
    register s64 r8  __asm__("r8")  = e;
    register s64 r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
                       "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { sc(SYS_write, 1, (s64)s, slen(s)); }

/* ------------------------------------------------------------------ a lock
 *
 * The three-state lock every futex tutorial arrives at: 0 free, 1 held with
 * nobody waiting, 2 held with someone waiting.  The third state is what keeps
 * the unlock path from calling into the kernel when nobody is asleep.
 */
static int lock_word;

static int cas(int *p, int expect, int want) {
    int old = expect;
    __asm__ volatile("lock cmpxchgl %2, %1"
                     : "+a"(old), "+m"(*p) : "r"(want) : "memory");
    return old;
}

static int swap(int *p, int want) {
    __asm__ volatile("xchgl %0, %1" : "+r"(want), "+m"(*p) :: "memory");
    return want;
}

static void lock(void) {
    int c = cas(&lock_word, 0, 1);
    if (c == 0) return;
    if (c != 2) c = swap(&lock_word, 2);
    while (c != 0) {
        sc6(SYS_futex, (s64)&lock_word, FUTEX_WAIT, 2, 0, 0, 0);
        c = swap(&lock_word, 2);
    }
}

static void unlock(void) {
    if (__atomic_fetch_sub(&lock_word, 1, __ATOMIC_SEQ_CST) != 1) {
        lock_word = 0;
        sc6(SYS_futex, (s64)&lock_word, FUTEX_WAKE, 1, 0, 0, 0);
    }
}

/* --------------------------------------------------------------- the work */

#define THREADS 4
#define ROUNDS  4000

static volatile long counter;
static volatile int  finished;      /* how many threads have said they are done */
static volatile int  started_tid[THREADS];

/* Each thread's stack, from the program's own memory - which is where a
 * runtime would put it, and the thing that makes clone's arrangement matter. */
#define STACK_BYTES (64 * 1024)

static void thread_body(int index) {
    started_tid[index] = (int)sc(SYS_gettid, 0, 0, 0);
    for (int i = 0; i < ROUNDS; i++) {
        lock();
        counter = counter + 1;
        unlock();
    }
    __atomic_fetch_add((int *)&finished, 1, __ATOMIC_SEQ_CST);
    sc6(SYS_futex, (s64)&finished, FUTEX_WAKE, THREADS + 1, 0, 0, 0);
    sc(SYS_exit, 0, 0, 0);
    for (;;) { }
}

/* clone returns twice: zero in the child, the new thread's number in the
 * parent.  Everything below the branch runs on the new stack. */
static long spawn(void *stack_top, int index) {
    long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND
                 | CLONE_THREAD;
    long rc = sc6(SYS_clone, flags, (s64)stack_top, 0, 0, 0, 0);
    if (rc == 0) {
        thread_body(index);
        for (;;) { }
    }
    return rc;
}

int main(void) {
    say("a Linux program starting threads\n");

    /* Memory for the stacks, asked for the way a runtime asks. */
    long mem = sc6(SYS_mmap, 0, THREADS * STACK_BYTES,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
    if (mem <= 0) return 2;

    for (int i = 0; i < THREADS; i++) {
        /* Sixteen-byte aligned, and the child gets the top because a stack
         * grows down. */
        unsigned char *top = (unsigned char *)mem + (i + 1) * STACK_BYTES;
        top = (unsigned char *)((u64)top & ~15ULL);
        long tid = spawn(top, i);
        if (tid <= 0) return 3;
    }

    /* Wait for all of them, by sleeping on the count rather than spinning -
     * which is the other half of what futex is for. */
    for (;;) {
        int done = __atomic_load_n((int *)&finished, __ATOMIC_SEQ_CST);
        if (done >= THREADS) break;
        sc6(SYS_futex, (s64)&finished, FUTEX_WAIT, done, 0, 0, 0);
    }

    /* The whole point: every increment survived, so no two threads were ever
     * inside the lock at the same time. */
    if (counter != (long)THREADS * ROUNDS) return 4;

    /* And each thread really was its own thread, with its own number. */
    for (int i = 0; i < THREADS; i++)
        if (started_tid[i] <= 0) return 5;
    for (int i = 0; i < THREADS; i++)
        for (int j = i + 1; j < THREADS; j++)
            if (started_tid[i] == started_tid[j]) return 6;

    say("four threads shared a counter and none of it was lost\n");
    return 0;
}

void _start(void) {
    sc(SYS_exit_group, main(), 0, 0);
    __builtin_unreachable();
}
