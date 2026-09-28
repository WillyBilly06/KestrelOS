/* heap.c - kernel allocator.
 *
 * A first-fit free list over arenas taken from the physical allocator and
 * addressed through the direct map, so no separate virtual bookkeeping is
 * needed.  Blocks carry a header with size and a magic word; adjacent free
 * blocks inside an arena are coalesced on free.  Allocations larger than a
 * quarter of the default arena get their own dedicated page run.
 */
#include "kernel.h"
#include "mm.h"
#include "klog.h"
#include "spinlock.h"

#define HEAP_MAGIC_USED 0x4B48454150555345ULL  /* "KHEAPUSE" */
#define HEAP_MAGIC_FREE 0x4B48454150465245ULL  /* "KHEAPFRE" */
#define ARENA_PAGES     256                    /* 1 MiB per arena           */
#define ALIGN_TO        16

typedef struct __attribute__((aligned(16))) block {
    u64           magic;
    size_t        size;     /* payload bytes, not counting this header */
    struct block *next;     /* next block in the arena, by address     */
    struct block *prev;
    struct arena *owner;
} block_t;
_Static_assert(sizeof(block_t) % ALIGN_TO == 0,
               "heap block payload and split headers retain 16-byte alignment");

typedef struct arena {
    struct arena *next;
    u64           phys;
    size_t        pages;
    block_t      *first;
    bool          dedicated;   /* one huge allocation, freed whole */
} arena_t;

static arena_t *arenas;
static size_t   total_bytes;
static size_t   used_bytes;
static bool     heap_ready;
/* Lock order: heap -> physical allocator. Neither allocator enters the
 * scheduler; callers must own their allocation while reallocating/freeing it. */
static spinlock_t heap_lock;

static size_t align_up(size_t n) { return (n + (ALIGN_TO - 1)) & ~(size_t)(ALIGN_TO - 1); }

static arena_t *arena_new(size_t pages, bool dedicated) {
    u64 phys = pmm_alloc_pages(pages);
    if (!phys) return NULL;

    /* The arena descriptor lives at the front of its own memory. */
    arena_t *a = phys_to_virt(phys);
    memset(a, 0, sizeof *a);
    a->phys      = phys;
    a->pages     = pages;
    a->dedicated = dedicated;

    size_t hdr = align_up(sizeof(arena_t));
    block_t *b = (block_t *)((u8 *)a + hdr);
    b->magic = HEAP_MAGIC_FREE;
    b->size  = pages * PAGE_SIZE - hdr - sizeof(block_t);
    b->next  = NULL;
    b->prev  = NULL;
    b->owner = a;
    a->first = b;

    a->next = arenas;
    arenas = a;
    total_bytes += pages * PAGE_SIZE;
    return a;
}

void heap_init(void) {
    if (!arena_new(ARENA_PAGES, false)) panic("heap: cannot allocate the first arena");
    heap_ready = true;
    kinfo("heap", "ready, %zu KiB in the initial arena", (size_t)(ARENA_PAGES * PAGE_SIZE / 1024));
}

/* Split a block if the remainder can hold a header plus a useful payload. */
static void split(block_t *b, size_t want) {
    if (b->size < want + sizeof(block_t) + ALIGN_TO) return;
    block_t *n = (block_t *)((u8 *)(b + 1) + want);
    n->magic = HEAP_MAGIC_FREE;
    n->size  = b->size - want - sizeof(block_t);
    n->owner = b->owner;
    n->next  = b->next;
    n->prev  = b;
    if (b->next) b->next->prev = n;
    b->next = n;
    b->size = want;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    if (!heap_ready) panic("kmalloc(%zu) before heap_init", size);

    if (size > (size_t)-1 - (ALIGN_TO - 1)) return NULL;
    size = align_up(size);
    const size_t overhead = align_up(sizeof(arena_t)) + sizeof(block_t) + PAGE_MASK;
    if (size > (size_t)-1 - overhead) return NULL;
    bool irq = spin_lock_irqsave(&heap_lock);

    /* Large requests get their own arena so they never fragment the pool. */
    if (size > (ARENA_PAGES * PAGE_SIZE) / 4) {
        size_t pages = (size + align_up(sizeof(arena_t)) + sizeof(block_t) + PAGE_MASK) / PAGE_SIZE;
        arena_t *a = arena_new(pages, true);
        if (!a) { spin_unlock_irqrestore(&heap_lock, irq); kerr("heap", "out of memory for %zu bytes", size); return NULL; }
        a->first->magic = HEAP_MAGIC_USED;
        used_bytes += a->first->size;
        spin_unlock_irqrestore(&heap_lock, irq);
        return a->first + 1;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        for (arena_t *a = arenas; a; a = a->next) {
            if (a->dedicated) continue;
            for (block_t *b = a->first; b; b = b->next) {
                if (b->magic != HEAP_MAGIC_FREE || b->size < size) continue;
                split(b, size);
                b->magic = HEAP_MAGIC_USED;
                used_bytes += b->size;
                spin_unlock_irqrestore(&heap_lock, irq);
                return b + 1;
            }
        }
        if (attempt == 0 && !arena_new(ARENA_PAGES, false)) break;
    }

    size_t used = used_bytes, total = total_bytes;
    spin_unlock_irqrestore(&heap_lock, irq);
    kerr("heap", "out of memory for %zu bytes (%zu KiB used of %zu KiB)",
         size, used / 1024, total / 1024);
    return NULL;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void kfree(void *p) {
    if (!p) return;
    block_t *b = (block_t *)p - 1;

    bool irq = spin_lock_irqsave(&heap_lock);
    if (b->magic == HEAP_MAGIC_FREE) {
        spin_unlock_irqrestore(&heap_lock, irq);
        kerr("heap", "double free of %p", p);
        return;
    }
    if (b->magic != HEAP_MAGIC_USED) {
        spin_unlock_irqrestore(&heap_lock, irq);
        kerr("heap", "kfree(%p) on a corrupted or foreign block", p);
        return;
    }

    used_bytes -= b->size;
    b->magic = HEAP_MAGIC_FREE;

    arena_t *a = b->owner;
    if (a->dedicated) {
        /* Unlink and give the whole run back. */
        arena_t **link = &arenas;
        while (*link && *link != a) link = &(*link)->next;
        if (*link) *link = a->next;
        total_bytes -= a->pages * PAGE_SIZE;
        u64 phys = a->phys;
        size_t pages = a->pages;
        spin_unlock_irqrestore(&heap_lock, irq);
        pmm_free_pages(phys, pages);
        return;
    }

    /* Coalesce with neighbours. */
    if (b->next && b->next->magic == HEAP_MAGIC_FREE) {
        block_t *n = b->next;
        b->size += sizeof(block_t) + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
    }
    if (b->prev && b->prev->magic == HEAP_MAGIC_FREE) {
        block_t *pv = b->prev;
        pv->size += sizeof(block_t) + b->size;
        pv->next = b->next;
        if (b->next) b->next->prev = pv;
    }
    spin_unlock_irqrestore(&heap_lock, irq);
}

void *krealloc(void *p, size_t size) {
    if (!p) return kmalloc(size);
    if (size == 0) { kfree(p); return NULL; }

    block_t *b = (block_t *)p - 1;
    if (b->magic != HEAP_MAGIC_USED) { kerr("heap", "krealloc(%p) on a bad block", p); return NULL; }
    if (b->size >= size) return p;

    void *n = kmalloc(size);
    if (!n) return NULL;
    memcpy(n, p, b->size);
    kfree(p);
    return n;
}

size_t heap_used(void) {
    bool irq = spin_lock_irqsave(&heap_lock);
    size_t value = used_bytes;
    spin_unlock_irqrestore(&heap_lock, irq);
    return value;
}
size_t heap_total(void) {
    bool irq = spin_lock_irqsave(&heap_lock);
    size_t value = total_bytes;
    spin_unlock_irqrestore(&heap_lock, irq);
    return value;
}
