/* vmm.c - page table management.
 *
 * The kernel inherits the page table the loader built and keeps using it as the
 * canonical kernel address space.  Every upper-half PML4 slot is populated with
 * an (initially empty) page-directory-pointer table during init, so a new
 * address space can share kernel mappings simply by copying those 256 entries
 * once - later kernel mappings appear in every address space automatically
 * because they only ever modify tables below an already-shared entry.
 */
#include "kernel.h"
#include "mm.h"
#include "klog.h"
#include "spinlock.h"
#include "smp.h"
#include "cpu.h"

static u64 kernel_pml4;
static spinlock_t page_table_lock;

/* Serialize walkers against mutation/free as well as concurrent table creation.
 * The sole nested allocator is PMM (which never acquires this lock). A CPU
 * waiting here with IF clear must service an outstanding remote invalidation,
 * otherwise the lock owner could wait forever for that same CPU's TLB IPI. */
static bool vmm_lock(void) {
    bool irq = irq_save();
    while (__atomic_exchange_n(&page_table_lock.held, 1u, __ATOMIC_ACQUIRE)) {
        smp_tlb_poll();
        __asm__ volatile("pause");
    }
    return irq;
}

static void vmm_unlock(bool irq) { spin_unlock_irqrestore(&page_table_lock, irq); }

/* Uncached window for device registers, well clear of the direct map. */
#define MMIO_WINDOW_BASE 0xFFFFA00000000000ULL
#define MMIO_WINDOW_SIZE (64ULL << 30)
static u64 mmio_next = MMIO_WINDOW_BASE;

u64 vmm_kernel_pml4(void) { return kernel_pml4; }

static inline u64 *table_at(u64 phys) { return (u64 *)phys_to_virt(phys & PTE_ADDR); }

/* Walk to the next level, optionally creating it.  Intermediate entries are
 * permissive (user + writable); the leaf entry is what actually restricts
 * access, which is how x86-64 combines permissions along the walk. */
static u64 *walk(u64 *table, int index, bool create) {
    u64 e = table[index];
    if (!(e & PTE_P)) {
        if (!create) return NULL;
        u64 p = pmm_alloc_zeroed();
        if (!p) return NULL;
        table[index] = p | PTE_P | PTE_W | PTE_U;
        return table_at(p);
    }
    if (e & PTE_PS) return NULL;    /* a large page is already mapped here */
    return table_at(e);
}

static bool map_locked(u64 pml4_phys, u64 va, u64 pa, u64 flags, bool *replaced) {
    u64 *pml4 = table_at(pml4_phys);
    u64 *pdpt = walk(pml4, (int)((va >> 39) & 0x1FF), true);
    if (!pdpt) return false;
    u64 *pd = walk(pdpt, (int)((va >> 30) & 0x1FF), true);
    if (!pd) return false;
    u64 *pt = walk(pd, (int)((va >> 21) & 0x1FF), true);
    if (!pt) return false;

    int i = (int)((va >> 12) & 0x1FF);
    u64 old = pt[i], next = (pa & PTE_ADDR) | flags | PTE_P;
    if ((old & ~(PTE_A | PTE_D)) == (next & ~(PTE_A | PTE_D))) return true;
    pt[i] = next;
    /* Hardware does not cache a translation derived from a non-present leaf.
     * Fresh mappings need no remote invalidation; replacement/restriction of a
     * present mapping does. Preserve the local invalidation for every change. */
    if (old & PTE_P) *replaced = true;
    invlpg(va);
    return true;
}

bool vmm_map(u64 pml4_phys, u64 va, u64 pa, u64 flags) {
    bool irq = vmm_lock(), replaced = false;
    bool ok = map_locked(pml4_phys, va, pa, flags, &replaced);
    if (replaced) smp_tlb_invalidate(pml4_phys, va, false);
    vmm_unlock(irq);
    return ok;
}

bool vmm_map_range(u64 pml4, u64 va, u64 pa, size_t bytes, u64 flags) {
    if (!bytes) return true;
    if (bytes > ~va || va + bytes > ~0ULL - PAGE_MASK) return false;
    u64 end = PAGE_ALIGN_UP(va + bytes);
    bool irq = vmm_lock(), replaced = false, ok = true;
    for (u64 v = PAGE_ALIGN_DOWN(va), p = PAGE_ALIGN_DOWN(pa); v < end; v += PAGE_SIZE, p += PAGE_SIZE)
        if (!map_locked(pml4, v, p, flags, &replaced)) { ok = false; break; }
    /* A partial failure can already have replaced an earlier mapping. Complete
     * its invalidation before returning failure, not just on the success path. */
    if (replaced) smp_tlb_invalidate(pml4, va, true);
    vmm_unlock(irq);
    return ok;
}

/* Resolve a virtual address, honouring 1 GiB and 2 MiB leaves so that
 * translations inside the loader's direct map work too. */
static u64 translate_locked(u64 pml4_phys, u64 va) {
    u64 *pml4 = table_at(pml4_phys);
    u64 e = pml4[(va >> 39) & 0x1FF];
    if (!(e & PTE_P)) return 0;

    u64 *pdpt = table_at(e);
    e = pdpt[(va >> 30) & 0x1FF];
    if (!(e & PTE_P)) return 0;
    if (e & PTE_PS) return (e & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFF);

    u64 *pd = table_at(e);
    e = pd[(va >> 21) & 0x1FF];
    if (!(e & PTE_P)) return 0;
    if (e & PTE_PS) return (e & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFF);

    u64 *pt = table_at(e);
    e = pt[(va >> 12) & 0x1FF];
    if (!(e & PTE_P)) return 0;
    return (e & PTE_ADDR) | (va & PAGE_MASK);
}

u64 vmm_translate(u64 pml4_phys, u64 va) {
    bool irq = vmm_lock();
    u64 result = translate_locked(pml4_phys, va);
    vmm_unlock(irq);
    return result;
}

static bool mapped_locked(u64 pml4, u64 va) {
    bool present = false;
    u64 *table = table_at(pml4);
    for (int shift = 39; shift >= 12; shift -= 9) {
        u64 e = table[(va >> shift) & 0x1ff];
        if (!(e & PTE_P)) break;
        if (shift == 12 || ((shift == 30 || shift == 21) && (e & PTE_PS))) {
            present = true; /* physical address zero is a valid present mapping */
            break;
        }
        table = table_at(e);
    }
    return present;
}

bool vmm_is_mapped(u64 pml4, u64 va) {
    bool irq = vmm_lock();
    bool present = mapped_locked(pml4, va);
    vmm_unlock(irq);
    return present;
}

static bool user_range_locked(u64 pml4, u64 va, size_t bytes, bool writable) {
    if (!bytes) return true;
    const u64 user_end = 1ULL << 47;
    if (!pml4 || va < PAGE_SIZE || va >= user_end || bytes > user_end - va)
        return false;
    u64 end = va + bytes;
    u64 required = PTE_P | PTE_U | (writable ? PTE_W : 0);
    bool ok = true;
    for (u64 page = PAGE_ALIGN_DOWN(va); page < end; page += PAGE_SIZE) {
        u64 *table = table_at(pml4);
        for (int shift = 39; shift >= 12; shift -= 9) {
            u64 entry = table[(page >> shift) & 0x1ff];
            /* x86 combines user/write permission at every level, not only
             * the leaf. A writable user PTE under a supervisor or read-only
             * ancestor must not pass a writable user-pointer check. */
            if ((entry & required) != required || (shift == 39 && (entry & PTE_PS))) {
                ok = false;
                break;
            }
            if (shift == 12 || (entry & PTE_PS)) break;
            table = table_at(entry);
        }
        if (!ok) break;
    }
    return ok;
}

bool vmm_user_range_ok(u64 pml4, u64 va, size_t bytes, bool writable) {
    bool irq = vmm_lock();
    bool ok = user_range_locked(pml4, va, bytes, writable);
    vmm_unlock(irq);
    return ok;
}

bool vmm_user_copy(u64 pml4, void *kernel_buffer, u64 va, size_t bytes, bool to_user) {
    if (!bytes) return true;
    if (!kernel_buffer || bytes > VMM_USER_COPY_MAX) return false;
    bool irq = vmm_lock();
    bool ok = (read_cr3() & PTE_ADDR) == (pml4 & PTE_ADDR) &&
              user_range_locked(pml4, va, bytes, to_user);
    /* IRQ exclusion prevents migration/CR3 replacement on this CPU; the shared
     * VM lock prevents another CPU removing or restricting the validated leaf.
     * Copy through its actual mapping, not a WB direct-map alias of possible
     * borrowed/device memory with different cache attributes. */
    if (ok) {
        if (to_user) memcpy((void *)va, kernel_buffer, bytes);
        else memcpy(kernel_buffer, (const void *)va, bytes);
    }
    vmm_unlock(irq);
    return ok;
}

bool vmm_user_word_key(u64 pml4, u64 va, u64 *key, u32 *value) {
    if (!key || (va & 3u)) return false;
    bool irq = vmm_lock();
    bool ok = (read_cr3() & PTE_ADDR) == pml4 &&
              user_range_locked(pml4, va, sizeof(u32), false);
    if (ok) {
        /* Unlike separate translate/check/load operations this cannot observe
         * a key from one mapping and a word from its concurrently replaced
         * successor. An aligned word cannot cross a base-page boundary. */
        *key = translate_locked(pml4, va);
        if (value) *value = __atomic_load_n((const u32 *)va, __ATOMIC_ACQUIRE);
    }
    vmm_unlock(irq);
    return ok;
}

bool vmm_user_store_word_key(u64 pml4, u64 va, u32 value, u64 *key) {
    if (!key || (va & 3u)) return false;
    bool irq = vmm_lock();
    bool ok = (read_cr3() & PTE_ADDR) == pml4 &&
              user_range_locked(pml4, va, sizeof(u32), true);
    if (ok) {
        *key = translate_locked(pml4, va);
        __atomic_store_n((u32 *)va, value, __ATOMIC_RELEASE);
    }
    vmm_unlock(irq);
    return ok;
}

static bool unmap_locked(u64 pml4_phys, u64 va) {
    u64 *pml4 = table_at(pml4_phys);
    u64 *pdpt = walk(pml4, (int)((va >> 39) & 0x1FF), false);
    if (!pdpt) return false;
    u64 *pd = walk(pdpt, (int)((va >> 30) & 0x1FF), false);
    if (!pd) return false;
    u64 *pt = walk(pd, (int)((va >> 21) & 0x1FF), false);
    if (!pt) return false;
    if (!(pt[(va >> 12) & 0x1FF] & PTE_P)) return false;
    pt[(va >> 12) & 0x1FF] = 0;
    return true;
}

void vmm_unmap(u64 pml4_phys, u64 va) {
    bool irq = vmm_lock();
    if (unmap_locked(pml4_phys, va)) smp_tlb_invalidate(pml4_phys, va, false);
    vmm_unlock(irq);
}

void vmm_unmap_range(u64 pml4, u64 va, size_t bytes) {
    if (!bytes || bytes > ~va || va + bytes > ~0ULL - PAGE_MASK) return;
    u64 end = PAGE_ALIGN_UP(va + bytes);
    bool irq = vmm_lock(), removed = false;
    for (u64 v = PAGE_ALIGN_DOWN(va); v < end; v += PAGE_SIZE)
        removed |= unmap_locked(pml4, v);
    if (removed) smp_tlb_invalidate(pml4, va, true);
    vmm_unlock(irq);
}

/* Unlike vmm_unmap (which only removes an alias), these helpers inspect the
 * actual 4 KiB leaf before changing ownership. A translation through a huge
 * page is not permission to free a 4 KiB frame inside that page. */
static u64 *user_leaf(u64 pml4_phys, u64 va) {
    if (va >= (1ULL << 47)) return NULL;
    u64 *table = table_at(pml4_phys);
    for (int shift = 39; shift >= 21; shift -= 9) {
        u64 e = table[(va >> shift) & 0x1ff];
        if ((e & (PTE_P | PTE_U)) != (PTE_P | PTE_U) || (e & PTE_PS))
            return NULL;
        table = table_at(e);
    }
    u64 *leaf = &table[(va >> 12) & 0x1ff];
    return (*leaf & (PTE_P | PTE_U)) == (PTE_P | PTE_U) ? leaf : NULL;
}

bool vmm_unmap_user_page(u64 pml4_phys, u64 va) {
    bool irq = vmm_lock();
    u64 *leaf = user_leaf(pml4_phys, va);
    if (!leaf) { vmm_unlock(irq); return false; }
    u64 e = *leaf;
    *leaf = 0;
    smp_tlb_invalidate(pml4_phys, va, false);
    if (!(e & PTE_BORROWED)) pmm_free_page(e & PTE_ADDR);
    vmm_unlock(irq);
    return true;
}

/* A bounded fresh allocation transaction for kernel-assigned thread stacks.
 * No existing leaf (including PA zero or a huge mapping) may be overwritten.
 * On failure, unlink exactly the frames installed by this transaction, wait
 * for invalidation, then free them. Empty page tables remain address-space
 * owned and reusable; normal address-space destruction eventually frees them. */
bool vmm_alloc_user_stack(u64 pml4, u64 va, size_t bytes) {
    u64 frames[64];
    const u64 user_end = 1ULL << 47;
    if (!pml4 || va < PAGE_SIZE || va >= user_end ||
        (va & PAGE_MASK) || (bytes & PAGE_MASK) || !bytes ||
        bytes > sizeof frames / sizeof frames[0] * PAGE_SIZE ||
        bytes > user_end - va) return false;
    bool irq = vmm_lock();
    unsigned pages = (unsigned)(bytes / PAGE_SIZE), installed = 0;
    for (unsigned i = 0; i < pages; i++) {
        if (mapped_locked(pml4, va + (u64)i * PAGE_SIZE)) {
            vmm_unlock(irq);
            return false;
        }
    }
    for (; installed < pages; installed++) {
        u64 phys = pmm_alloc_zeroed();
        if (!phys) break;
        bool replaced = false;
        if (!map_locked(pml4, va + (u64)installed * PAGE_SIZE, phys,
                        PTE_U | PTE_W | (g_cpu.has_nx ? PTE_NX : 0), &replaced)) {
            pmm_free_page(phys);
            break;
        }
        if (replaced) panic("vmm: fresh stack replaced a mapping");
        frames[installed] = phys;
    }
    /* Existing intermediate tables may be supervisor-only/read-only; adding
     * a user leaf cannot loosen their permissions. Never publish an unusable
     * stack, and use the raw unlink path for rollback under such ancestors. */
    bool ok = installed == pages && user_range_locked(pml4, va, bytes, true);
    if (!ok) {
        for (unsigned i = 0; i < installed; i++)
            unmap_locked(pml4, va + (u64)i * PAGE_SIZE);
        if (installed) smp_tlb_invalidate(pml4, va, true);
        for (unsigned i = 0; i < installed; i++) pmm_free_page(frames[i]);
    }
    vmm_unlock(irq);
    return ok;
}

void vmm_unmap_user_range(u64 pml4_phys, u64 va, size_t bytes) {
    const u64 user_end = 1ULL << 47;
    if (!bytes || va >= user_end || bytes > user_end - va) return;
    u64 end = PAGE_ALIGN_UP(va + bytes);
    u64 retired[256];
    for (u64 page = PAGE_ALIGN_DOWN(va); page < end;) {
        unsigned owned = 0, count = 0;
        bool irq = vmm_lock(), removed = false;
        while (page < end && count++ < ARRAY_LEN(retired)) {
            u64 *leaf = user_leaf(pml4_phys, page);
            if (leaf) {
                u64 entry = *leaf;
                *leaf = 0;
                removed = true;
                if (!(entry & PTE_BORROWED)) retired[owned++] = entry & PTE_ADDR;
            }
            page += PAGE_SIZE;
        }
        /* Do not issue an all-CPU IPI round trip for every 4 KiB of a large
         * heap shrink. The bounded staging array retains ownership until the
         * batch's single full barrier completes. No allocation under VM lock. */
        if (removed) smp_tlb_invalidate(pml4_phys, 0, true);
        for (unsigned i = 0; i < owned; i++) pmm_free_page(retired[i]);
        vmm_unlock(irq);
    }
}

bool vmm_unmap_borrowed_page(u64 pml4_phys, u64 va, u64 expected_phys) {
    bool irq = vmm_lock();
    u64 *leaf = user_leaf(pml4_phys, va);
    if (!leaf || !(*leaf & PTE_BORROWED) ||
        (*leaf & PTE_ADDR) != expected_phys) { vmm_unlock(irq); return false; }
    *leaf = 0;
    smp_tlb_invalidate(pml4_phys, va, false);
    vmm_unlock(irq);
    return true;
}

void vmm_init(void) {
    kernel_pml4 = read_cr3() & PTE_ADDR;
    u64 *pml4 = table_at(kernel_pml4);

    /* Populate every kernel-half slot now.  After this the top-level entries
     * never change, so address spaces created later stay in sync with any
     * kernel mapping added afterwards. */
    int created = 0;
    for (int i = 256; i < 512; i++) {
        if (pml4[i] & PTE_P) continue;
        u64 p = pmm_alloc_zeroed();
        if (!p) panic("vmm: out of memory building kernel page tables");
        pml4[i] = p | PTE_P | PTE_W;    /* no PTE_U: kernel half stays private */
        created++;
    }

    /* The identity map the loader built is left in place, and this is load
     * bearing rather than an oversight.
     *
     * This comment used to say the opposite - that the map was dropped now
     * that the kernel runs out of the direct map - and no code has ever
     * dropped it.  Entry 0 still covers the first 512 GiB, and something now
     * depends on that: a processor woken after boot starts in real mode at a
     * low physical address, turns paging on part way through, and executes the
     * very next instruction out of that page.  Clearing entry 0 would fault
     * every waking core before it reached any handler, which on this hardware
     * restarts the machine - a boot loop with nothing logged.
     *
     * So it stays, and anybody tempted to tidy it away should read smp.c
     * first. */
    kinfo("vmm", "kernel pml4 at %p, %d kernel slots created", (void *)kernel_pml4, created);
}

void *vmm_map_mmio(u64 phys, size_t bytes) {
    bool irq = vmm_lock();
    u64 off   = phys & PAGE_MASK;
    u64 start = PAGE_ALIGN_DOWN(phys);
    u64 span  = PAGE_ALIGN_UP(bytes + off);

    if (mmio_next + span > MMIO_WINDOW_BASE + MMIO_WINDOW_SIZE) {
        vmm_unlock(irq);
        kerr("vmm", "MMIO window exhausted mapping %p (%zu bytes)", (void *)phys, bytes);
        return NULL;
    }
    u64 va = mmio_next;
    mmio_next += span + PAGE_SIZE;   /* a guard page between mappings */
    vmm_unlock(irq);

    /* Strong uncacheable: device registers must not be cached or reordered. */
    if (!vmm_map_range(kernel_pml4, va, start, span, PTE_W | PTE_PCD | PTE_PWT | PTE_NX)) {
        kerr("vmm", "failed to map MMIO at %p", (void *)phys);
        return NULL;
    }
    return (void *)(va + off);
}

/* The same window, but write-combining rather than uncached.  Video memory is
 * written far more than it is read and never has side effects, so gathering
 * stores into whole cache lines is both safe and much faster. */
void *vmm_map_wc(u64 phys, size_t bytes) {
    bool irq = vmm_lock();
    u64 off   = phys & PAGE_MASK;
    u64 start = PAGE_ALIGN_DOWN(phys);
    u64 span  = PAGE_ALIGN_UP(bytes + off);

    if (mmio_next + span > MMIO_WINDOW_BASE + MMIO_WINDOW_SIZE) {
        vmm_unlock(irq);
        kerr("vmm", "MMIO window exhausted mapping %p (%zu bytes)", (void *)phys, bytes);
        return NULL;
    }
    u64 va = mmio_next;
    mmio_next += span + PAGE_SIZE;
    vmm_unlock(irq);

    /* PWT alone selects page attribute table entry 1, which pat_init turns
     * into write-combining.  Where the table was left alone that entry is
     * write-through, which is still correct - only slower. */
    if (!vmm_map_range(kernel_pml4, va, start, span, PTE_W | PTE_PWT | PTE_NX)) {
        kerr("vmm", "failed to map video memory at %p", (void *)phys);
        return NULL;
    }
    return (void *)(va + off);
}

u64 vmm_new_address_space(void) {
    u64 p = pmm_alloc_zeroed();
    if (!p) return 0;
    bool irq = vmm_lock();
    u64 *dst = table_at(p);
    u64 *src = table_at(kernel_pml4);
    for (int i = 256; i < 512; i++) dst[i] = src[i];
    vmm_unlock(irq);
    return p;
}

/* Free the user half: owned leaf frames plus the tables that described them.
 * Borrowed display/device frames and the shared kernel half stay owned by
 * their original allocator. */
static void free_level(u64 table_phys, int level) {
    u64 *t = table_at(table_phys);
    for (int i = 0; i < 512; i++) {
        u64 e = t[i];
        if (!(e & PTE_P)) continue;
        if (level > 1 && !(e & PTE_PS)) free_level(e & PTE_ADDR, level - 1);
        else if ((e & PTE_U) && !(e & PTE_BORROWED))
            pmm_free_page(e & PTE_ADDR);    /* an owned user data page */
    }
    pmm_free_page(table_phys);
}

void vmm_destroy_address_space(u64 pml4_phys) {
    if (!pml4_phys || pml4_phys == kernel_pml4) return;
    /* Caller has stopped every user and prevented new CR3 entries. A TLB
     * barrier retires cached translations, not an address-space reference. */
    if ((read_cr3() & PTE_ADDR) == (pml4_phys & PTE_ADDR))
        panic("vmm: cannot destroy the current address space");
    bool irq = vmm_lock();
    u64 *pml4 = table_at(pml4_phys);
    u64 detached[256];
    for (int i = 0; i < 256; i++) {
        detached[i] = pml4[i];
        pml4[i] = 0;
    }
    /* Unlink first, then wait for every online CPU, then recycle tables/frames.
     * free_level must never run before the remote acknowledgments. */
    smp_tlb_invalidate(pml4_phys, 0, true);
    for (int i = 0; i < 256; i++)
        if (detached[i] & PTE_P) free_level(detached[i] & PTE_ADDR, 3);
    pmm_free_page(pml4_phys);
    vmm_unlock(irq);
}
