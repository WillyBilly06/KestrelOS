/* proc.c - the ELF loader, per-process file descriptors, path resolution and
 * the user-mode page fault path. */
#include "kernel.h"
#include "proc.h"
#include "../include/kestrel/syscall.h"
#include "cpu.h"
#include "mm.h"
#include "vfs.h"
#include "klog.h"
#include "block.h"
#include "time.h"
#include "spinlock.h"
#include "smp.h"

/* ------------------------------------------------------------------------- */
/* ELF64                                                                     */
#define ELF_OSABI_KESTREL 0x4B   /* what build.py stamps into byte 7 */
/* ------------------------------------------------------------------------- */

typedef struct {
    u8  ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf64_ehdr;

typedef struct {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
} elf64_phdr;

#define PT_LOAD    1
#define PF_X       1
#define PF_W       2
#define PF_R       4

int elf_load(proc_t *p, const void *image, size_t len, u64 *entry_out) {
    if (len < sizeof(elf64_ehdr)) return -E_INVAL;
    const elf64_ehdr *eh = image;

    if (memcmp(eh->ident, "\x7F" "ELF", 4)) { kerr("elf", "%s is not an ELF image", p->name); return -E_INVAL; }
    if (eh->ident[4] != 2)  { kerr("elf", "%s is not 64-bit", p->name); return -E_INVAL; }
    if (eh->ident[5] != 1)  { kerr("elf", "%s is not little-endian", p->name); return -E_INVAL; }
    if (eh->machine != 0x3E){ kerr("elf", "%s is not x86-64", p->name); return -E_INVAL; }
    if (eh->type != 2)      { kerr("elf", "%s is not a static executable", p->name); return -E_INVAL; }

    /* Whose system calls this file expects, out of the byte ELF keeps for
     * saying so.  A KestrelOS program says KestrelOS; anything else - a file
     * built by an ordinary Linux toolchain, which leaves the byte zero - is
     * taken at its word as a Linux program and gets the translated path.
     * Guessing the other way round would run a Linux program's exit as a
     * read, which is a hang rather than an error. */
    p->linux_abi = (eh->ident[7] != ELF_OSABI_KESTREL);
    if (p->linux_abi)
        kinfo("elf", "%s expects Linux system calls, so it gets them "
                     "translated", p->name);
    if (eh->phoff + (u64)eh->phnum * eh->phentsize > len) { kerr("elf", "%s has a bad program header table", p->name); return -E_INVAL; }
    if (eh->phentsize < sizeof(elf64_phdr)) return -E_INVAL;

    u64 lowest = ~0ULL, highest = 0;
    int loaded = 0;

    for (int i = 0; i < eh->phnum; i++) {
        const elf64_phdr *ph = (const elf64_phdr *)((const u8 *)image + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != PT_LOAD) continue;
        if (ph->memsz == 0) continue;

        if (ph->offset + ph->filesz > len) { kerr("elf", "%s segment %d runs past the file", p->name, i); return -E_INVAL; }
        if (ph->filesz > ph->memsz) { kerr("elf", "%s segment %d is malformed", p->name, i); return -E_INVAL; }

        /* Refuse anything that would land in kernel space or below the fixed
         * image base, which is what keeps a hostile ELF from mapping over the
         * kernel's own address range. */
        if (ph->vaddr < USER_IMAGE_BASE || ph->vaddr + ph->memsz > USER_STACK_TOP - USER_STACK_SIZE) {
            kerr("elf", "%s segment %d wants %p, outside the user range", p->name, i, (void *)ph->vaddr);
            return -E_INVAL;
        }

        u64 start = PAGE_ALIGN_DOWN(ph->vaddr);
        u64 end   = PAGE_ALIGN_UP(ph->vaddr + ph->memsz);

        u64 flags = PTE_U;
        if (ph->flags & PF_W) flags |= PTE_W;
        if (!(ph->flags & PF_X) && g_cpu.has_nx) flags |= PTE_NX;

        for (u64 va = start; va < end; va += PAGE_SIZE) {
            u64 phys = vmm_translate(p->pml4, va);
            if (!phys) {
                phys = pmm_alloc_zeroed();
                if (!phys) return -E_NOMEM;
                if (!vmm_map(p->pml4, va, phys, flags)) return -E_NOMEM;
            } else {
                /* Two segments sharing a page: keep the more permissive flags
                 * so neither is broken by the other's mapping. */
                vmm_map(p->pml4, va, phys, flags);
            }
        }

        /* Copy the file-backed part through the direct map. */
        u64 copied = 0;
        while (copied < ph->filesz) {
            u64 va = ph->vaddr + copied;
            u64 phys = vmm_translate(p->pml4, va);
            if (!phys) return -E_NOMEM;
            size_t chunk = PAGE_SIZE - (va & PAGE_MASK);
            if (chunk > ph->filesz - copied) chunk = (size_t)(ph->filesz - copied);
            memcpy(phys_to_virt(phys), (const u8 *)image + ph->offset + copied, chunk);
            copied += chunk;
        }
        /* memsz beyond filesz is .bss; the pages were allocated zeroed. */

        if (start < lowest) lowest = start;
        if (end > highest) highest = end;
        loaded++;
    }

    if (!loaded) { kerr("elf", "%s has no loadable segments", p->name); return -E_INVAL; }
    if (eh->entry < lowest || eh->entry >= highest) {
        kerr("elf", "%s entry point %p is outside its own image", p->name, (void *)eh->entry);
        return -E_INVAL;
    }

    p->image_base = lowest;
    p->image_end = highest;

    *entry_out = eh->entry;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* file descriptors                                                          */
/* ------------------------------------------------------------------------- */

#include "process_fds.h"

/* ------------------------------------------------------------------------- */
/* paths                                                                     */
/* ------------------------------------------------------------------------- */

void proc_resolve_path(proc_t *p, const char *in, char *out, size_t cap) {
    p = proc_shared(p);
    if (!in || !in[0]) { strlcpy(out, p ? p->cwd : "/", cap); return; }
    if (in[0] == '/') { strlcpy(out, in, cap); return; }

    const char *base = p ? p->cwd : "/";
    if (!strcmp(base, "/")) snprintf(out, cap, "/%s", in);
    else snprintf(out, cap, "%s/%s", base, in);
}

/* ------------------------------------------------------------------------- */
/* faults                                                                    */
/* ------------------------------------------------------------------------- */

/* Called from the exception path for every page fault.  Returns true when the
 * fault has been dealt with and execution can resume - either because the page
 * was created on demand, or because the offending process was killed and
 * another thread is now current. */
bool user_fault(regs_t *r, u64 cr2) {
    proc_t *p = proc_current();
    bool from_user = (r->cs & 3) == 3;

    if (!p || !from_user || p->is_kernel || !p->pml4) return false;

    /* Grow the stack downwards on demand, within a bounded window. */
    u64 stack_limit = USER_STACK_TOP - (4 * 1024 * 1024);
    if (cr2 < p->stack_low && cr2 >= stack_limit && cr2 < USER_STACK_TOP) {
        u64 want = PAGE_ALIGN_DOWN(cr2);
        for (u64 va = want; va < p->stack_low; va += PAGE_SIZE) {
            u64 phys = pmm_alloc_zeroed();
            if (!phys) break;
            if (!vmm_map(p->pml4, va, phys, PTE_W | PTE_U | PTE_NX)) { pmm_free_page(phys); break; }
        }
        if (vmm_translate(p->pml4, cr2)) {
            p->stack_low = want;
            kdebug("proc", "grew %s's stack to %p", p->name, (void *)want);
            return true;
        }
    }

    /* A process can ask to hear about its own faults - a Windows program has
     * to, because handling them is part of the language it is written in. */
    if (user_exception(r, 14, cr2)) return true;

    /* Anything else in a user process is fatal to that process only. */
    kerr("proc", "pid %d (%s) faulted at %p accessing %p (%s%s); terminating it",
         p->pid, p->name, (void *)r->rip, (void *)cr2,
         (r->error & 2) ? "write" : ((r->error & 16) ? "execute" : "read"),
         (r->error & 1) ? ", protection" : ", not mapped");

    proc_exit(139);
    return true;    /* not reached: proc_exit switches away */
}

/* ------------------------------------------------------------------------- */
/* first user process                                                        */
/* ------------------------------------------------------------------------- */

/* Push the log out to disk regularly, so that pulling the power - which is all
 * anyone can do to a machine that has stopped responding - loses at most half a
 * second of what it had to say.
 *
 * Half a second rather than a second because the thing this is for is a machine
 * that has just gone wrong, and the interesting lines are always the last ones.
 * The cost is a small write to a USB stick twice a second, and only when there
 * is something new to write: a flush with nothing pending returns immediately. */
bool block_start_boot_log(void);
bool efi_boot_snapshot(void);   /* efivar.c */

static void log_flusher(void *arg) {
    (void)arg;
    int passes = 0;
    bool snapshot_taken = false;

    /* Two jobs, and the second is the reason this runs rather than a timer.
     *
     * The volume to write to is looked for as soon as the disks are known,
     * which is before USB has been brought up - and a machine booted from a
     * USB stick has its boot volume on a device that does not exist yet at
     * that point.  Worse, the stick appears some seconds later still, because
     * enumerating a device behind a hub takes real time.
     *
     * So the search is repeated until it succeeds rather than done once.  It
     * costs one scan of the disk list per half second until it finds
     * something, and it is what lets a system booted from a stick keep a log
     * on that stick - which on a machine with no serial cable is the whole
     * difference between diagnosing a problem and photographing a screen. */
    for (;;) {
        sched_sleep_ms(500);

        if (!klog_persist_active()) {
            static int tries;
            if (block_start_boot_log()) {
                kinfo("log", "the volume this was booted from is writable "
                             "after all; the log is going onto it");
            } else if (++tries == 20) {
                /* Ten seconds in.  Said once, because a machine installed to a
                 * disk legitimately never finds one and should not complain
                 * about it twice a second forever. */
                kwarn("log", "nothing writable to keep a log on: no installed "
                             "system, and the volume this was booted from did "
                             "not come back as a disk.  If that was a USB "
                             "stick, its storage driver did not claim it");
            }
        }

        /* Ten seconds in, keep a copy of the boot where the power cannot reach
         * it.  This is the moment the question is answerable: every driver has
         * had its chance at its hardware, the search above has either found a
         * volume or given up, and all of that is still in the log ring.  A few
         * minutes later it will not be - the ring holds 512 entries and normal
         * use overwrites them - so a snapshot taken at shutdown can say what
         * the machine was doing at the end but not what it decided at the
         * start.  Written once; this is firmware storage, not a disk. */
        if (!snapshot_taken && ++passes >= 20) {
            snapshot_taken = true;
            efi_boot_snapshot();
        }

        klog_persist_flush();

        /* Push what was just appended all the way to the physical disk, not
         * only into the write-back cache.  Without this the log (and every
         * other write) sat in RAM until the cache happened to evict it or the
         * machine shut down cleanly - so a reboot, a hard power-off, or a stick
         * pulled out lost everything since the last eviction.  That is why the
         * on-disk log stopped a fraction of a second into the boot even though
         * the system ran for far longer: the rest was cached and never written.
         * Flushing here, every half second, makes writes durable within half a
         * second of being made - the difference between "wrote the disk" and
         * "wrote a cache that is about to vanish". */
        block_cache_flush_all();
    }
}

void proc_start_init(void) {
    /* The console has to be open before the first process is created, because
     * a child inherits its parent's standard descriptors. */
    file_t *con = NULL;
    int r = vfs_open("/dev/console", O_RDWR, &con);
    if (r < 0) {
        kerr("proc", "cannot open /dev/console: %s", vfs_strerror(r));
        panic("no console device; there is nothing to run on");
    }

    proc_t *boot = proc_current();
    if (proc_fd_alloc(boot, con) != 0) panic("cannot install stdin");
    for (int fd = 1; fd < 3; fd++) {
        file_t *ref = vfs_file_ref(con);
        if (!ref || proc_fd_alloc(boot, ref) != fd) panic("cannot install console stdio");
    }

    kthread_create("logflush", log_flusher, NULL);

    const char *argv[1] = { "init" };
    int pid = 0;
    r = proc_spawn("/bin/init", argv, 1, &pid);
    if (r < 0) {
        kerr("proc", "cannot start /bin/init: %s", vfs_strerror(r));
        panic("init failed to start");
    }
}

/* ------------------------------------------------------------------------- */
/* delivering a fault to the process it happened in                          */
/* ------------------------------------------------------------------------- */

/* True when [addr, addr+len) is writable by this process.  A fault report is
 * written onto the faulting thread's own stack, and if that stack is what
 * overflowed, writing there would fault again - so it is checked first. */
static bool writable(proc_t *p, u64 addr, size_t len) {
    for (u64 va = PAGE_ALIGN_DOWN(addr); va < addr + len; va += PAGE_SIZE) {
        u64 phys = vmm_translate(p->pml4, va);
        if (!phys) return false;
    }
    return true;
}

bool user_exception(regs_t *r, int vector, u64 address) {
    proc_t *p = proc_current();
    if (!p || p->is_kernel || !p->pml4) return false;
    if ((r->cs & 3) != 3) return false;

    proc_t *lead = proc_shared(p);
    if (!lead->fault_handler) return false;

    /* A handler that faults, whose handler faults, and so on: allowed a few
     * levels because a real one can legitimately nest, then stopped. */
    if (p->fault_depth >= 4) {
        kerr("proc", "pid %d (%s) faulted inside its own fault handler", p->pid, p->name);
        return false;
    }

    /* The report goes just below the faulting stack pointer, clear of the
     * red zone that a leaf function may still be using. */
    u64 sp = (r->rsp - 128 - sizeof(kfault_t)) & ~15ULL;
    if (!writable(p, sp, sizeof(kfault_t))) return false;

    kfault_t *f = (kfault_t *)sp;
    f->vector = (u32)vector;
    f->error = (u32)r->error;
    f->address = address;
    f->rip = r->rip;   f->rsp = r->rsp; f->rflags = r->rflags;
    f->rax = r->rax;   f->rbx = r->rbx; f->rcx = r->rcx; f->rdx = r->rdx;
    f->rsi = r->rsi;   f->rdi = r->rdi; f->rbp = r->rbp;
    f->r8  = r->r8;    f->r9  = r->r9;  f->r10 = r->r10; f->r11 = r->r11;
    f->r12 = r->r12;   f->r13 = r->r13; f->r14 = r->r14; f->r15 = r->r15;

    /* Enter the handler as an ordinary call would: the report in the first
     * argument register, and the stack eight bytes off alignment because a
     * call would have pushed a return address. */
    u64 handler_sp = (sp & ~15ULL) - 8;
    if (!writable(p, handler_sp - 4096, 4096 + 8)) return false;

    p->fault_depth++;
    r->rdi = sp;
    r->rip = lead->fault_handler;
    r->rsp = handler_sp;
    r->rflags &= ~0x100ULL;               /* never resume single-stepping */

    kdebug("proc", "pid %d took %s at %p; delivered to its own handler",
           p->pid, exception_name(vector), (void *)f->rip);
    return true;
}
