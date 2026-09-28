/* linux_abi.c - running a program built for Linux.
 *
 * A Linux x86-64 program and a KestrelOS one are the same kind of file.  Both
 * are static ELF for the same processor; both enter the kernel with the same
 * instruction, the call number in RAX and the arguments in RDI, RSI, RDX, R10,
 * R8 and R9.  Nothing about the machine has to be pretended at: the program's
 * instructions are the processor's own and run at its own speed.
 *
 * What the two disagree about is what the number in RAX means.  KestrelOS
 * numbered its calls in the order it grew them; Linux numbered its own two
 * decades earlier.  Both call 1 "write" by coincidence and nothing else lines
 * up - Linux's 0 is read, KestrelOS's 0 is exit - so a Linux program run
 * without translation stops at its first system call in a way that looks like
 * a hang rather than a mistake.
 *
 * So this is the translation.  Most of it is renumbering.  Some of it is not:
 * a few calls mean genuinely different things, a few have no equivalent, and
 * the ones that hand back a structure have to hand back the structure Linux
 * programs are compiled to read.  Those are the interesting ones and they are
 * commented where they occur.
 *
 * Two pieces of luck are worth naming because they are not luck: the error
 * numbers already agree, because KestrelOS took Linux's, and the argument
 * registers already agree, because both follow the same ABI document.  What
 * remains is the numbering and the structures.
 */
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "vfs.h"
#include "klog.h"
#include "time.h"
#include "crypto.h"
#include "cpu.h"
#include "../include/kestrel/syscall.h"

/* ------------------------------------------------------- Linux call numbers */
#define L_read              0
#define L_write             1
#define L_open              2
#define L_close             3
#define L_stat              4
#define L_fstat             5
#define L_lstat             6
#define L_lseek             8
#define L_mmap              9
#define L_mprotect         10
#define L_munmap           11
#define L_brk              12
#define L_rt_sigaction     13
#define L_rt_sigprocmask   14
#define L_ioctl            16
#define L_readv            19
#define L_writev           20
#define L_access           21
#define L_pipe             22
#define L_sched_yield      24
#define L_dup              32
#define L_dup2             33
#define L_nanosleep        35
#define L_getpid           39
#define L_clone            56
#define L_execve           59
#define L_exit             60
#define L_wait4            61
#define L_kill             62
#define L_uname            63
#define L_fcntl            72
#define L_getcwd           79
#define L_chdir            80
#define L_rename           82
#define L_mkdir            83
#define L_rmdir            84
#define L_unlink           87
#define L_readlink         89
#define L_gettimeofday     96
#define L_getuid          102
#define L_getgid          104
#define L_geteuid         107
#define L_getegid         108
#define L_arch_prctl      158
#define L_gettid          186
#define L_futex           202
#define L_set_tid_address 218
#define L_clock_gettime   228
#define L_clock_getres    229
#define L_exit_group      231
#define L_openat          257
#define L_newfstatat      262
#define L_prlimit64       302
#define L_getrandom       318
#define L_rseq            334

#define L_set_robust_list 273

#define L_AT_FDCWD  (-100)

/* What futex is being asked to do, and the two flags that do not change it. */
#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_WAIT_BITSET    9
#define FUTEX_WAKE_BITSET   10
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_CLOCK_REALTIME 256

/* Which parts of itself a clone shares, from Linux's sched.h. */
#define CLONE_VM            0x00000100
#define CLONE_FS            0x00000200
#define CLONE_FILES         0x00000400
#define CLONE_SIGHAND       0x00000800
#define CLONE_THREAD        0x00010000
#define CLONE_SYSVSEM       0x00040000
#define CLONE_SETTLS        0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_DETACHED      0x00400000 /* obsolete, ignored by Linux */
#define CLONE_CHILD_SETTID  0x01000000

/* What arch_prctl is asked to do, from Linux's own header. */
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

/* The shape Linux programs are compiled to read a stat into.  This is not a
 * struct anyone would design; it is the one glibc and musl agree on for
 * x86-64, and a program reads st_mode at 24 and st_size at 48 because that is
 * where its own headers said they would be.  Getting a field's offset wrong
 * here does not fail - it hands back a plausible number from the wrong place,
 * which is worse. */
typedef struct {
    u64 st_dev;
    u64 st_ino;
    u64 st_nlink;
    u32 st_mode;
    u32 st_uid;
    u32 st_gid;
    u32 pad0;
    u64 st_rdev;
    s64 st_size;
    s64 st_blksize;
    s64 st_blocks;
    u64 atime_sec, atime_nsec;
    u64 mtime_sec, mtime_nsec;
    u64 ctime_sec, ctime_nsec;
    s64 reserved[3];
} linux_stat_t;

#define L_S_IFREG 0100000
#define L_S_IFDIR 0040000
#define L_S_IFCHR 0020000
#define L_S_IFBLK 0060000

typedef vfs_iovec_t linux_iovec_t;
typedef struct { s64 sec; s64 nsec; } linux_timespec_t;
typedef struct { s64 sec; s64 usec; } linux_timeval_t;

/* What uname hands back: six fixed-size strings, one after another. */
typedef struct { char s[6][65]; } linux_utsname_t;

s64  syscall_native(proc_t *p, u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4);
bool syscall_user_ok(u64 addr, size_t len, bool need_write);
bool syscall_user_copy(void *buffer, u64 address, size_t bytes, bool to_user);
int syscall_stat_snapshot(proc_t *p, u64 path, kstat_t *out);

#include "linux_wait.h"

static void fill_stat(linux_stat_t *out, u32 type, u64 size, u64 mtime) {
    memset(out, 0, sizeof *out);
    switch (type) {
    case VN_DIR: out->st_mode = L_S_IFDIR | 0755; break;
    case VN_CHR: out->st_mode = L_S_IFCHR | 0666; break;
    case VN_BLK: out->st_mode = L_S_IFBLK | 0660; break;
    default:     out->st_mode = L_S_IFREG | 0644; break;
    }
    out->st_nlink   = 1;
    out->st_size    = (s64)size;
    out->st_blksize = 4096;
    out->st_blocks  = (s64)((size + 511) / 512);
    out->mtime_sec  = mtime;
    out->atime_sec  = mtime;
    out->ctime_sec  = mtime;
    out->st_ino     = 1;
}

s64 linux_dispatch(proc_t *p, regs_t *r) {
    /* The whole frame rather than six arguments, because clone starts its
     * child at the same instruction the parent is returning to - so it needs
     * every register the caller had, not just the ones it passed. */
    u64 nr = r->rax;
    u64 a0 = r->rdi, a1 = r->rsi, a2 = r->rdx;
    u64 a3 = r->r10, a4 = r->r8,  a5 = r->r9;
    (void)a5;

    switch (nr) {

    /* ---- the ones that are only a different number --------------------- */
    case L_read:         return syscall_native(p, SYS_READ,  a0, a1, a2, 0, 0);
    case L_write:        return syscall_native(p, SYS_WRITE, a0, a1, a2, 0, 0);
    case L_close:        return syscall_native(p, SYS_CLOSE, a0, 0, 0, 0, 0);
    case L_lseek:        return syscall_native(p, SYS_SEEK,  a0, a1, a2, 0, 0);
    case L_munmap:       return syscall_native(p, SYS_MUNMAP, a0, a1, 0, 0, 0);
    case L_ioctl:        return syscall_native(p, SYS_IOCTL, a0, a1, a2, 0, 0);
    case L_pipe:         return syscall_native(p, SYS_PIPE,  a0, 0, 0, 0, 0);
    case L_sched_yield:  return syscall_native(p, SYS_YIELD, 0, 0, 0, 0, 0);
    case L_dup:          return syscall_native(p, SYS_DUP,   a0, 0, 0, 0, 0);
    case L_dup2:         return syscall_native(p, SYS_DUP2,  a0, a1, 0, 0, 0);
    case L_getpid:       return proc_shared(p)->pid;
    case L_gettid:       return p->pid;
    case L_wait4:        return syscall_native(p, SYS_WAIT,  a0, a1, 0, 0, 0);
    case L_kill:         return syscall_native(p, SYS_KILL,  a0, a1, 0, 0, 0);
    case L_getcwd:       return syscall_native(p, SYS_GETCWD, a0, a1, 0, 0, 0);
    case L_chdir:        return syscall_native(p, SYS_CHDIR, a0, 0, 0, 0, 0);
    case L_mkdir:        return syscall_native(p, SYS_MKDIR, a0, a1, 0, 0, 0);
    case L_rmdir:
    case L_unlink:       return syscall_native(p, SYS_UNLINK, a0, 0, 0, 0, 0);
    case L_rename:       return syscall_native(p, SYS_RENAME, a0, a1, 0, 0, 0);
    case L_execve:       return syscall_native(p, SYS_EXEC, a0, a1, 0, 0, 0);

    /* A clone child may exit without terminating every sibling. */
    case L_exit:         proc_thread_exit((int)a0);
    case L_exit_group:   return syscall_native(p, SYS_EXIT, a0, 0, 0, 0, 0);

    /* ---- the ones that mean something different ------------------------ */

    /* Linux's open takes flags in the second argument and a mode in the
     * third; openat puts a directory in front of all three.  Only "relative
     * to the working directory" is answerable here, because there is nothing
     * that holds a directory open to be relative to. */
    case L_open:
        return syscall_native(p, SYS_OPEN, a0, a1, 0, 0, 0);
    case L_openat:
        if ((s64)(s32)a0 != L_AT_FDCWD) return -E_NOSYS;
        return syscall_native(p, SYS_OPEN, a1, a2, 0, 0, 0);

    /* brk sets where the heap ends; sbrk moves it by a distance.  The
     * translation is a subtraction, but the return values differ too: brk
     * answers with where the heap ends now, and on failure with where it
     * still ends - never with an error - which is why a program that checks
     * brk for a negative number never sees one. */
    case L_brk:
        return syscall_brk(p, a0);

    /* Linux's mmap takes six arguments; this answers the one every runtime
     * actually uses at startup - anonymous memory, no file behind it.  A
     * file-backed mapping is refused rather than quietly handed back zeroes,
     * because zeroes that should have been a file are a wrong answer the
     * program cannot detect. */
    case L_mmap:
        if ((s64)(s32)a4 != -1) return -E_NOSYS;   /* a file was asked for */
        return syscall_native(p, SYS_MMAP, a0, a1, a2, 0, 0);

    /* Nothing here makes a page read-only after the fact, and a program that
     * asks is not harmed by being told it worked: the pages it is asking
     * about are already writable and it will not write to them. */
    case L_mprotect: return 0;

    /* Where a program keeps its own thread's variables.  A Linux program does
     * this before main and touches nothing beforehand, so getting it wrong
     * shows up as a fault on the program's first real instruction. */
    case L_arch_prctl:
        switch (a0) {
        case ARCH_SET_FS:
            return proc_thread_set_base(true, a1);
        case ARCH_SET_GS:
            return proc_thread_set_base(false, a1);
        case ARCH_GET_FS:
        case ARCH_GET_GS: {
            u64 base = a0 == ARCH_GET_FS ? p->fsbase : p->gsbase;
            return syscall_user_copy(&base, a1, sizeof base, true) ? 0 : -E_FAULT;
        }
        default: return -E_INVAL;
        }

    /* Snapshot vector metadata once and retain one description/offset
     * transaction across the entire vector. Never reread user vectors after a
     * blocking driver operation; a sibling could mutate or unmap that array. */
    case L_writev:
    case L_readv: {
        if (a2 > VFS_IOV_MAX) return -E_INVAL;
        file_t *f = proc_fd_acquire(p, (int)a0);
        if (!f) return -E_BADF;
        size_t bytes = (size_t)a2 * sizeof(linux_iovec_t);
        linux_iovec_t *v = bytes ? kmalloc(bytes) : NULL;
        s64 result = -E_NOMEM;
        if (!bytes || v) {
            if (bytes && !syscall_user_copy(v, a1, bytes, false)) result = -E_INVAL;
            else result = vfs_user_iov(f, p->pml4, v, (size_t)a2, nr == L_writev);
        }
        kfree(v);
        vfs_close(f);
        return result;
    }

    /* The three that hand back a stat.  The work is the same each time and
     * only where the name comes from differs. */
    case L_stat:
    case L_lstat:
    case L_newfstatat: {
        u64 path = (nr == L_newfstatat) ? a1 : a0;
        u64 out  = (nr == L_newfstatat) ? a2 : a1;
        if (nr == L_newfstatat && (s64)(s32)a0 != L_AT_FDCWD) return -E_NOSYS;
        if (!syscall_user_ok(out, sizeof(linux_stat_t), true)) return -E_FAULT;

        kstat_t ks;
        s64 r = syscall_stat_snapshot(p, path, &ks);
        if (r < 0) return r;
        linux_stat_t snapshot;
        fill_stat(&snapshot, ks.type, ks.size, ks.mtime);
        return syscall_user_copy(&snapshot, out, sizeof snapshot, true) ? 0 : -E_FAULT;
    }

    /* The same, for something already open.  There is no call to forward to,
     * so it reads the open file's own node - which is what a forwarded call
     * would have done anyway. */
    case L_fstat: {
        if (!syscall_user_ok(a1, sizeof(linux_stat_t), true)) return -E_FAULT;
        file_t *f = proc_fd_acquire(p, (int)a0);
        if (!f) return -E_BADF;
        linux_stat_t snapshot;
        fill_stat(&snapshot, f->vn->type, f->vn->size, 0);
        vfs_close(f);
        return syscall_user_copy(&snapshot, a1, sizeof snapshot, true) ? 0 : -E_FAULT;
    }

    /* Sleeping, described by a structure rather than a number. */
    case L_nanosleep: {
        linux_timespec_t snapshot;
        if (!syscall_user_copy(&snapshot, a0, sizeof snapshot, false)) return -E_FAULT;
        u64 ms;
        if (!linux_timespec_ms(&snapshot, &ms)) return -E_INVAL;
        if (!ms) return 0;
        return syscall_native(p, SYS_SLEEP, ms, 0, 0, 0, 0);
    }

    case L_clock_gettime: {
        linux_timespec_t snapshot;
        int result = linux_clock_read(a0, &snapshot);
        if (result < 0) return result;
        return syscall_user_copy(&snapshot, a1, sizeof snapshot, true) ? 0 : -E_FAULT;
    }

    case L_clock_getres: {
        if (a0 != 0 && a0 != 1) return -E_INVAL;
        linux_timespec_t resolution = {0, 1000000};
        return !a1 || syscall_user_copy(&resolution, a1, sizeof resolution, true) ? 0 : -E_FAULT;
    }

    case L_gettimeofday: {
        if (a0) {
            linux_timespec_t clock;
            (void)linux_clock_read(0, &clock);
            linux_timeval_t snapshot = {clock.sec, clock.nsec / 1000};
            if (!syscall_user_copy(&snapshot, a0, sizeof snapshot, true)) return -E_FAULT;
        }
        if (a1) {
            s32 utc[2] = {0, 0};
            if (!syscall_user_copy(utc, a1, sizeof utc, true)) return -E_FAULT;
        }
        return 0;
    }

    /* What system this is.  A program that asks is usually deciding which of
     * several paths to take, and the honest answer is the one that says what
     * this is rather than the one that says "Linux" to get past the check. */
    case L_uname: {
        if (!syscall_user_ok(a0, sizeof(linux_utsname_t), true))
            return -E_INVAL;
        linux_utsname_t *u = (linux_utsname_t *)a0;
        memset(u, 0, sizeof *u);
        strlcpy(u->s[0], "KestrelOS", 65);       /* sysname  */
        strlcpy(u->s[1], "kestrel", 65);         /* nodename */
        strlcpy(u->s[2], "1.0", 65);             /* release  */
        strlcpy(u->s[3], "KestrelOS running a Linux program", 65);
        strlcpy(u->s[4], "x86_64", 65);          /* machine  */
        u->s[5][0] = 0;
        return 0;
    }

    case L_getrandom:
        if (!syscall_user_ok(a0, a1, true)) return -E_INVAL;
        random_bytes((void *)a0, (size_t)a1);
        return (s64)a1;

    /* ---- the ones with an honest fixed answer -------------------------- */

    /* There is one user here and it owns everything, so these are not
     * pretending: they are the true answer for this system. */
    case L_getuid: case L_geteuid: case L_getgid: case L_getegid: return 0;

    /* Register the exit-time clear-and-futex-wake address for this thread. */
    case L_set_tid_address:
        return proc_set_tid_address(a0);

    /* Asked at startup by every glibc program to find out how much it is
     * allowed; there are no limits here, and saying so is accurate. */
    case L_prlimit64: return 0;

    /* Signals.  Nothing here delivers them, so registering a handler that
     * will never fire is accepted quietly - a program that cannot register
     * one usually stops, and a handler that never fires costs nothing. */
    case L_rt_sigaction:
    case L_rt_sigprocmask: return 0;

    /* Answerable by asking whether it can be stat'd. */
    case L_access: {
        kstat_t ks;
        return syscall_stat_snapshot(p, a0, &ks);
    }

    /* F_GETFL and F_SETFL are the only two a startup path uses. */
    case L_fcntl:
        if (a1 == 3 || a1 == 4) return 0;
        return -E_NOSYS;

    /* Scoped keys, bitsets and absolute deadlines are translated explicitly. */
    case L_futex:
        return linux_futex_call(a0, (u32)a1, (u32)a2, a3, (u32)a5);
    /* A new thread.  Linux uses one call for threads and for whole new
     * processes and tells them apart by flags; only the thread half is
     * answerable here, and the other half says so rather than producing
     * something that is neither. */
    case L_clone: {
        if (!(a0 & CLONE_THREAD) || !(a0 & CLONE_VM))
            return -E_NOSYS;              /* a fork, not a thread */
        if (!(a0 & CLONE_SIGHAND)) return -E_INVAL;
        const u64 supported = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
            CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID |
            CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID | CLONE_DETACHED;
        /* This thread implementation shares descriptors and cwd. Do not
         * silently share resources a caller explicitly requested to separate. */
        if ((a0 & ~supported) || !(a0 & CLONE_FS) || !(a0 & CLONE_FILES)) return -E_NOSYS;
        const proc_clone_options_t options = {
            .stack = a1, .tls = a4, .parent_tid = a2, .child_tid = a3,
            .set_tls = !!(a0 & CLONE_SETTLS),
            .set_parent_tid = !!(a0 & CLONE_PARENT_SETTID),
            .set_child_tid = !!(a0 & CLONE_CHILD_SETTID),
            .clear_child_tid = !!(a0 & CLONE_CHILD_CLEARTID),
        };

        int tid = 0;
        s64 rc = proc_thread_clone(r, &options, &tid);
        if (rc < 0) return rc;
        return tid;
    }

    /* ---- the ones with no answer here ---------------------------------- */

    /* Said plainly rather than faked.  A program told a call does not exist
     * can choose another way; a program given a wrong answer cannot. */
    case L_rseq:
    case L_readlink:
        return -E_NOSYS;

    /* A list of locks to release if this thread dies holding them.  Nothing
     * here kills a thread that way, so remembering the list would change
     * nothing; accepting it lets a runtime get past its own startup. */
    case L_set_robust_list:
        return 0;

    default:
        kwarn("linux", "%s asked for call %llu, which is not translated",
              p->name, (unsigned long long)nr);
        return -E_NOSYS;
    }
}
