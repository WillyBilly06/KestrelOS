/* pmm.c - physical page allocator.
 *
 * A flat bitmap over every physical page the firmware reported, one bit per
 * 4 KiB frame, 1 = in use.  The bitmap itself is carved out of the largest
 * usable region before anything else is handed out.  A rotating cursor keeps
 * the common single-page allocation close to O(1) without needing a free list.
 */
#include "kernel.h"
#include "mm.h"
#include "klog.h"
#include "spinlock.h"

static u64   *bitmap;
static u64    bitmap_words;
static u64    total_pages;      /* highest frame index + 1                */
static u64    usable_pages;     /* frames that started out free           */
static u64    used_pages;
static u64    cursor;           /* next frame to examine                  */
static spinlock_t pmm_lock;     /* bitmap, cursor and allocation counters */

static inline bool bit_test(u64 i)  { return (bitmap[i >> 6] >> (i & 63)) & 1; }
static inline void bit_set(u64 i)   { bitmap[i >> 6] |=  (1ULL << (i & 63)); }
static inline void bit_clear(u64 i) { bitmap[i >> 6] &= ~(1ULL << (i & 63)); }

static void mark_used(u64 first, u64 count) {
    for (u64 i = first; i < first + count && i < total_pages; i++)
        if (!bit_test(i)) { bit_set(i); used_pages++; }
}

static void mark_free(u64 first, u64 count) {
    for (u64 i = first; i < first + count && i < total_pages; i++)
        if (bit_test(i)) { bit_clear(i); used_pages--; }
}

static bool region_is_usable(u32 type) {
    /* Loader memory is reclaimed: everything the kernel still needs from it was
     * copied into kernel .bss before pmm_init ran. */
    return type == KB_MEM_USABLE || type == KB_MEM_LOADER;
}

void pmm_init(void) {
    u64 max_addr = 0;
    for (u32 i = 0; i < g_mmap_count; i++) {
        u64 end = g_mmap[i].base + g_mmap[i].pages * PAGE_SIZE;
        if (end > max_addr) max_addr = end;
    }
    total_pages = max_addr / PAGE_SIZE;
    bitmap_words = (total_pages + 63) / 64;
    u64 bitmap_bytes = bitmap_words * 8;

    /* Find somewhere to put the bitmap: the largest usable region that can hold
     * it, preferring one above 1 MiB so we leave low memory for DMA. */
    u64 best_base = 0, best_pages = 0;
    for (u32 i = 0; i < g_mmap_count; i++) {
        if (!region_is_usable(g_mmap[i].type)) continue;
        u64 base = g_mmap[i].base, pages = g_mmap[i].pages;
        if (base < 0x100000) {                    /* skip the first megabyte */
            u64 skip = (0x100000 - base) / PAGE_SIZE;
            if (pages <= skip) continue;
            base += skip * PAGE_SIZE;
            pages -= skip;
        }
        if (pages * PAGE_SIZE >= bitmap_bytes && pages > best_pages) {
            best_base = base;
            best_pages = pages;
        }
    }
    if (!best_base) panic("pmm: no region large enough for a %lu KiB page bitmap", bitmap_bytes / 1024);

    bitmap = phys_to_virt(best_base);

    /* Everything is used until proven otherwise, so holes in the firmware map
     * are never handed out. */
    memset(bitmap, 0xFF, bitmap_bytes);
    used_pages = total_pages;

    for (u32 i = 0; i < g_mmap_count; i++) {
        if (!region_is_usable(g_mmap[i].type)) continue;
        mark_free(g_mmap[i].base / PAGE_SIZE, g_mmap[i].pages);
    }
    usable_pages = total_pages - used_pages;

    /* Never hand out the first page (null pointer traps) or the bitmap. */
    mark_used(0, 1);
    mark_used(best_base / PAGE_SIZE, (bitmap_bytes + PAGE_MASK) / PAGE_SIZE);

    cursor = 1;

    kinfo("pmm", "%lu MiB usable, %lu MiB total, bitmap at %p (%lu KiB)",
          usable_pages * PAGE_SIZE / (1024 * 1024),
          total_pages * PAGE_SIZE / (1024 * 1024),
          (void *)best_base, bitmap_bytes / 1024);
}

u64 pmm_alloc_page(void) {
    bool irq = spin_lock_irqsave(&pmm_lock);
    for (u64 tries = 0; tries < total_pages; tries++) {
        u64 i = cursor;
        cursor = cursor + 1 >= total_pages ? 1 : cursor + 1;

        /* Skip a whole word when it is fully allocated. */
        if (bitmap[i >> 6] == ~0ULL) {
            u64 next = ((i >> 6) + 1) << 6;
            cursor = next >= total_pages ? 1 : next;
            tries += 63;
            continue;
        }
        if (!bit_test(i)) {
            bit_set(i);
            used_pages++;
            spin_unlock_irqrestore(&pmm_lock, irq);
            return i * PAGE_SIZE;
        }
    }
    u64 used = used_pages;
    spin_unlock_irqrestore(&pmm_lock, irq);
    kerr("pmm", "out of physical memory (%lu MiB in use)", used * PAGE_SIZE / (1024 * 1024));
    return 0;
}

static u64 alloc_run(size_t count, u64 limit_page) {
    if (count == 0) return 0;
    u64 last = total_pages;
    if (limit_page && limit_page < last) last = limit_page;

    bool irq = spin_lock_irqsave(&pmm_lock);
    u64 run = 0, start = 0;
    for (u64 i = 1; i < last; i++) {
        if (bit_test(i)) { run = 0; continue; }
        if (run == 0) start = i;
        if (++run == count) {
            mark_used(start, count);
            spin_unlock_irqrestore(&pmm_lock, irq);
            return start * PAGE_SIZE;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, irq);
    return 0;
}

u64 pmm_alloc_pages(size_t count) {
    if (count == 1) return pmm_alloc_page();
    u64 p = alloc_run(count, 0);
    if (!p) kerr("pmm", "no run of %zu contiguous pages available", count);
    return p;
}

u64 pmm_alloc_pages_below(size_t count, u64 limit) {
    u64 p = alloc_run(count, limit / PAGE_SIZE);
    if (!p) kerr("pmm", "no run of %zu pages below %p", count, (void *)limit);
    return p;
}

void pmm_free_page(u64 phys) { pmm_free_pages(phys, 1); }

void pmm_free_pages(u64 phys, size_t count) {
    if (!phys) return;
    bool irq = spin_lock_irqsave(&pmm_lock);
    u64 first = phys / PAGE_SIZE;
    if (first < total_pages) mark_free(first, count);
    if (first < cursor) cursor = first;      /* reuse promptly */
    spin_unlock_irqrestore(&pmm_lock, irq);
}

void pmm_reserve(u64 phys, size_t count) {
    bool irq = spin_lock_irqsave(&pmm_lock);
    mark_used(phys / PAGE_SIZE, count);
    spin_unlock_irqrestore(&pmm_lock, irq);
}

u64 pmm_total_bytes(void) { return usable_pages * PAGE_SIZE; }
u64 pmm_used_bytes(void) {
    bool irq = spin_lock_irqsave(&pmm_lock);
    u64 value = (used_pages - (total_pages - usable_pages)) * PAGE_SIZE;
    spin_unlock_irqrestore(&pmm_lock, irq);
    return value;
}
u64 pmm_free_bytes(void) {
    bool irq = spin_lock_irqsave(&pmm_lock);
    u64 value = (total_pages - used_pages) * PAGE_SIZE;
    spin_unlock_irqrestore(&pmm_lock, irq);
    return value;
}


/* ---------------------------------------------------------------- DMA memory */

void *dma_alloc_pages(size_t pages, u64 *phys_out) {
    if (!pages) pages = 1;

    /* DMA buffers must be 32-bit addressable.  Many devices program only the
     * low 32 bits of a buffer address (USB storage over xHCI, the Wi-Fi part,
     * older NICs), so a buffer above 4 GiB - which is what the allocator hands
     * out once a machine has enough RAM to reach there - is fetched from the
     * wrong place or refused outright.  On a real machine with lots of memory
     * this is why the USB stick would not mount and the Wi-Fi firmware would
     * not load; in a small VM every buffer was already below 4 GiB, so it never
     * showed.  Allocate below 4 GiB; fall back to anywhere only if the low
     * region is exhausted (large allocations on a memory-starved box), where a
     * high buffer is at least better than no buffer. */
    u64 p = alloc_run(pages, 0x100000000ull / PAGE_SIZE);
    if (!p) p = pmm_alloc_pages(pages);
    if (!p) return NULL;

    void *v = phys_to_virt(p);
    memset(v, 0, pages * PAGE_SIZE);
    if (phys_out) *phys_out = p;
    return v;
}

void dma_free_pages(void *virt, size_t pages) {
    if (!virt) return;
    if (!pages) pages = 1;
    pmm_free_pages(virt_to_phys(virt), pages);
}
