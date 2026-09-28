/* Shared heap/mmap metadata operations. Included by syscall.c.
 * Caller retains proc_vm_begin ownership; never call these while holding a
 * render/driver lock. Page-table mutation retains its independent VMM lock. */
static s64 process_memory_locked(proc_t *p, u64 nr, u64 a0, u64 a1, u64 a2) {
    switch (nr) {
    case SYS_SBRK: {
        s64 delta = (s64)a0;
        proc_t *h = proc_shared(p);
        u64 old = h->heap_end;
        if (delta == 0) return (s64)old;

        if (delta > 0) {
            u64 want = old + (u64)delta;
            if (want > USER_HEAP_MAX || want < old) return -E_NOMEM;
            for (u64 va = PAGE_ALIGN_UP(old); va < PAGE_ALIGN_UP(want); va += PAGE_SIZE) {
                if (vmm_translate(p->pml4, va)) continue;
                u64 phys = pmm_alloc_zeroed();
                if (!phys) return -E_NOMEM;
                if (!vmm_map(p->pml4, va, phys, PTE_W | PTE_U | PTE_NX)) { pmm_free_page(phys); return -E_NOMEM; }
            }
            h->heap_end = want;
        } else {
            /* Unsigned magnitude also handles INT64_MIN without signed UB
             * and clamps before subtracting, rather than wrapping the heap. */
            u64 magnitude = 0ull - (u64)delta;
            u64 want = magnitude > old - h->heap_base ? h->heap_base : old - magnitude;
            vmm_unmap_user_range(p->pml4, PAGE_ALIGN_UP(want),
                                 PAGE_ALIGN_UP(old) - PAGE_ALIGN_UP(want));
            h->heap_end = want;
        }
        return (s64)old;
    }

    case SYS_MMAP: {
        if (!a1 || a1 > (256u << 20)) return -E_INVAL;
        size_t len = PAGE_ALIGN_UP(a1);
        if (!len || len > (256u << 20)) return -E_INVAL;

        /* Mappings stay clear of the thread stacks, which are laid out from
         * USER_TSTACK_BASE upward in the same address space. */
        proc_t *m = proc_shared(p);
        u64 base = a0 ? PAGE_ALIGN_DOWN(a0) : m->mmap_next;
        if (a0 && (base < USER_IMAGE_BASE || base >= USER_TSTACK_BASE ||
                   len > USER_TSTACK_BASE - base))
            return -E_INVAL;
        if (!a0 && (base < USER_MMAP_BASE || (base & PAGE_MASK) ||
                    base > USER_TSTACK_BASE - PAGE_SIZE ||
                    len > USER_TSTACK_BASE - base - PAGE_SIZE)) return -E_NOMEM;

        u64 flags = PTE_U | PTE_W;
        if (!(a2 & 4) && g_cpu.has_nx) flags |= PTE_NX;      /* a2 bit 2 = executable */

        for (u64 va = base; va < base + len; va += PAGE_SIZE) {
            if (vmm_translate(p->pml4, va)) continue;
            u64 phys = pmm_alloc_zeroed();
            if (!phys) return -E_NOMEM;
            if (!vmm_map(p->pml4, va, phys, flags)) { pmm_free_page(phys); return -E_NOMEM; }
        }
        if (!a0) m->mmap_next = base + len + PAGE_SIZE;
        return (s64)base;
    }

    case SYS_MUNMAP: {
        if (!a1 || a1 > USER_STACK_TOP - PAGE_MASK) return -E_INVAL;
        size_t len = PAGE_ALIGN_UP(a1);
        u64 base = PAGE_ALIGN_DOWN(a0);
        if (base < USER_IMAGE_BASE || base >= USER_STACK_TOP ||
            len > USER_STACK_TOP - base) return -E_INVAL;
        vmm_unmap_user_range(p->pml4, base, len);
        return 0;
    }
    default: return -E_INVAL;
    }
}

static s64 process_memory_op(proc_t *p, u64 nr, u64 a0, u64 a1, u64 a2) {
    proc_t *token;
    if (p != proc_current() || !proc_vm_begin(&token)) return -E_BUSY;
    s64 result = process_memory_locked(p, nr, a0, a1, a2);
    proc_vm_end(token);
    return result;
}

/* Linux brk is absolute. Query and update must use ONE metadata transaction;
 * translating it into two independent sbrk calls races with sibling threads. */
s64 syscall_brk(proc_t *p, u64 requested) {
    proc_t *token;
    if (p != proc_current() || !proc_vm_begin(&token)) return -E_BUSY;
    proc_t *lead = proc_shared(p);
    u64 old = lead->heap_end;
    if (requested >= lead->heap_base && requested <= USER_HEAP_MAX) {
        u64 delta = requested >= old ? requested - old : 0ull - (old - requested);
        (void)process_memory_locked(p, SYS_SBRK, delta, 0, 0);
    }
    u64 result = lead->heap_end;
    proc_vm_end(token);
    return (s64)result;
}
