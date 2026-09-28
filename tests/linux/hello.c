/* hello.c - an ordinary Linux program, compiled by an ordinary Linux compiler.
 *
 * Nothing about this file knows what it is going to run on.  It is built with
 * clang targeting x86_64-unknown-linux-gnu, with optimisation, and what comes
 * out is a static ELF that would run unchanged on any Linux machine.  The
 * instructions in it are the processor's own, so nothing interprets them:
 * KestrelOS loads the file and the processor runs it at full speed.  The only
 * thing standing between the two is the system call numbering, which is what
 * this exercises.
 *
 * It is written without a C library because there is no Linux sysroot on the
 * machine that builds it - so the calls are made directly, exactly as a
 * library would make them.  That is the same arrangement the ARM probe uses
 * and it proves the same thing: the compiler decided the instructions, and
 * those instructions are what runs.
 *
 * What it checks is deliberately the awkward half of the translation:
 *
 *   - the calls that are only a different number  (write, open, read, close)
 *   - the one that means something different      (brk, which is not sbrk)
 *   - the one that hands back a struct            (fstat, whose layout must
 *                                                  match byte for byte)
 *   - the one that must happen before anything    (arch_prctl, which is where
 *     else works                                   thread-local storage lives)
 *   - the list-of-writes one                      (writev)
 *   - one that has no answer here                 (readlink, which must say
 *                                                  so rather than pretend)
 *
 * It reports by exit code: 0 for all of them, otherwise the number of the
 * first check that failed, so a failure names itself without needing output.
 */

typedef unsigned long u64;
typedef long          s64;

/* The Linux call numbers, as a program's headers would give them. */
#define SYS_read        0
#define SYS_write       1
#define SYS_open        2
#define SYS_close       3
#define SYS_fstat       5
#define SYS_brk        12
#define SYS_writev     20
#define SYS_getpid     39
#define SYS_exit       60
#define SYS_uname      63
#define SYS_readlink   89
#define SYS_arch_prctl 158
#define SYS_exit_group 231

#define ARCH_SET_FS 0x1002

static s64 sc(s64 n, s64 a, s64 b, s64 c) {
    s64 r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

static unsigned slen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static void say(const char *s) { sc(SYS_write, 1, (s64)s, slen(s)); }

/* Where a Linux program keeps a per-thread variable: at an offset from FS.
 * Reading it back through FS is the only way to know arch_prctl worked - the
 * value is not anywhere else. */
static u64 read_fs(unsigned off) {
    u64 v;
    __asm__ volatile("mov %%fs:(%1), %0" : "=r"(v) : "r"((u64)off));
    return v;
}

struct linux_stat {
    u64 st_dev, st_ino, st_nlink;
    unsigned st_mode, st_uid, st_gid, pad0;
    u64 st_rdev;
    s64 st_size, st_blksize, st_blocks;
    u64 at_s, at_ns, mt_s, mt_ns, ct_s, ct_ns;
    s64 reserved[3];
};

struct iovec { const void *base; u64 len; };

static u64 tls_area[8];

int main(void) {
    char buf[512];

    /* 1 - write, which is the one number both systems happen to agree on,
     * and so proves nothing on its own except that the program is running. */
    say("hello from a Linux program\n");

    /* 2 - thread-local storage.  Everything a real Linux runtime does after
     * this point depends on FS pointing somewhere, so this comes first. */
    tls_area[0] = 0;
    tls_area[1] = 0xFEEDFACE12345678ULL;
    if (sc(SYS_arch_prctl, ARCH_SET_FS, (s64)tls_area, 0) != 0) return 2;
    if (read_fs(8) != 0xFEEDFACE12345678ULL) return 3;

    /* 3 - brk.  Linux's brk is not KestrelOS's sbrk: it takes where the heap
     * should end, not how much further, and answers with where it ends now.
     * A translation that forwarded the argument unchanged would ask for a
     * heap several terabytes long and be refused - or, worse, succeed. */
    s64 start = sc(SYS_brk, 0, 0, 0);
    if (start <= 0) return 4;
    s64 got = sc(SYS_brk, start + 8192, 0, 0);
    if (got < start + 8192) return 5;

    /* And the memory it handed back has to be real, so it gets written to and
     * read back - a break that moves without memory behind it is the failure
     * this catches. */
    volatile unsigned char *heap = (volatile unsigned char *)start;
    for (int i = 0; i < 8192; i += 512) heap[i] = (unsigned char)(i / 512 + 1);
    for (int i = 0; i < 8192; i += 512)
        if (heap[i] != (unsigned char)(i / 512 + 1)) return 6;

    /* 4 - a file, opened and read by number the Linux way. */
    s64 fd = sc(SYS_open, (s64)"/etc/motd", 0, 0);
    if (fd < 0) return 7;

    /* 5 - fstat, whose answer is a structure this program was compiled to
     * read at fixed offsets.  If st_size is at the wrong offset the number
     * that comes back is some other field, which is plausible and wrong. */
    struct linux_stat st;
    if (sc(SYS_fstat, fd, (s64)&st, 0) != 0) return 8;
    if (st.st_size <= 0) return 9;
    if ((st.st_mode & 0170000) != 0100000) return 10;   /* a regular file */

    s64 n = sc(SYS_read, fd, (s64)buf, sizeof buf);
    if (n <= 0) return 11;
    if (n != st.st_size) return 12;      /* what fstat said and what read got */
    sc(SYS_close, fd, 0, 0);

    int lines = 0;
    for (s64 i = 0; i < n; i++) if (buf[i] == '\n') lines++;
    if (lines < 1) return 13;

    /* 6 - writev, which is several writes described by a list.  A translation
     * that stops after the first one produces output that looks almost right,
     * so the check is on the returned total as well as the effect. */
    struct iovec v[3];
    v[0].base = "a "; v[0].len = 2;
    v[1].base = "Linux program "; v[1].len = 14;
    v[2].base = "reading a file\n"; v[2].len = 15;
    if (sc(SYS_writev, 1, (s64)v, 3) != 31) return 14;

    /* 7 - uname.  A program that asks deserves the truth, so the check is
     * that it does not claim to be Linux. */
    char uts[6 * 65];
    if (sc(SYS_uname, (s64)uts, 0, 0) != 0) return 15;
    if (uts[0] != 'K') return 16;

    /* 8 - and one with no answer here, which has to say so.  A call that
     * quietly returned success would leave the program using a buffer that
     * was never filled in. */
    if (sc(SYS_readlink, (s64)"/etc/motd", (s64)buf, sizeof buf) >= 0)
        return 17;

    if (sc(SYS_getpid, 0, 0, 0) <= 0) return 18;

    say("all of it worked\n");
    return 0;
}

void _start(void) {
    sc(SYS_exit_group, main(), 0, 0);
    __builtin_unreachable();
}
