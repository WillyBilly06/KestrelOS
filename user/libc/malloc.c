/* malloc.c - user-space allocator.
 *
 * A first-fit free list over memory obtained with sbrk.  Freed blocks are
 * coalesced with their neighbours, and the top of the heap is returned to the
 * kernel when a large trailing block becomes free, so a program that allocates
 * a big buffer and frees it does not hold the memory forever.
 */
#include "kestrel.h"

#define ALIGN 16
#define MAGIC_USED 0xA110C8EDu
#define MAGIC_FREE 0xF2EEB10Cu
#define GROW_MIN   (64 * 1024)

typedef struct block {
    uint32_t      magic;
    uint32_t      pad;
    size_t        size;        /* payload bytes */
    struct block *next;
    struct block *prev;
} block_t;

static block_t *head;
static block_t *tail;
static char    *heap_end;

/* 0 unlocked, 1 owned without known waiters, 2 owned/contended. Sleep rather
 * than spin on a preempted owner. Futex compare-and-wait closes the unlock /
 * sleep race; returning spuriously always rechecks ownership. Not signal-safe. */
static uint32_t heap_mutex;
static void heap_lock(void) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&heap_mutex, &expected, 1u, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    while (__atomic_exchange_n(&heap_mutex, 2u, __ATOMIC_ACQUIRE) != 0)
        (void)futex_wait(&heap_mutex, 2u, -1);
}
static void heap_unlock(void) {
    if (__atomic_fetch_sub(&heap_mutex, 1u, __ATOMIC_RELEASE) != 1u) {
        __atomic_store_n(&heap_mutex, 0u, __ATOMIC_RELEASE);
        (void)futex_wake(&heap_mutex, 1);
    }
}

static bool allocation_size_ok(size_t n) {
    /* sbrk takes intptr_t; leave room for header and alignment before adding. */
    return n <= ((size_t)-1 >> 1) - sizeof(block_t) - (ALIGN - 1);
}

static size_t align_up(size_t n) { return (n + (ALIGN - 1)) & ~(size_t)(ALIGN - 1); }

static block_t *grow(size_t need) {
    size_t want = align_up(need + sizeof(block_t));
    if (want < GROW_MIN) want = GROW_MIN;

    void *base = sbrk((intptr_t)want);
    if (base == (void *)-1) return NULL;

    block_t *b = base;
    b->magic = MAGIC_FREE;
    b->size = want - sizeof(block_t);
    b->next = NULL;
    b->prev = tail;

    if (tail) tail->next = b;
    else head = b;
    tail = b;
    heap_end = (char *)base + want;

    /* If the new block is contiguous with the previous one and that one is
     * free, merge so a series of small grows does not fragment. */
    if (b->prev && b->prev->magic == MAGIC_FREE &&
        (char *)(b->prev + 1) + b->prev->size == (char *)b) {
        block_t *p = b->prev;
        p->size += sizeof(block_t) + b->size;
        p->next = b->next;
        if (b->next) b->next->prev = p;
        else tail = p;
        return p;
    }
    return b;
}

static void split(block_t *b, size_t want) {
    if (b->size < want + sizeof(block_t) + ALIGN) return;

    block_t *n = (block_t *)((char *)(b + 1) + want);
    n->magic = MAGIC_FREE;
    n->size = b->size - want - sizeof(block_t);
    n->next = b->next;
    n->prev = b;
    if (b->next) b->next->prev = n;
    else tail = n;
    b->next = n;
    b->size = want;
    /* Realloc shrinking a used block can put this new free suffix directly
     * before an already-free block. Keep the free-list invariant here too;
     * otherwise repeated shrinking leaves chains that free() cannot collapse. */
    while (n->next && n->next->magic == MAGIC_FREE &&
           (char *)(n + 1) + n->size == (char *)n->next) {
        block_t *next = n->next;
        n->size += sizeof(block_t) + next->size;
        n->next = next->next;
        if (n->next) n->next->prev = n;
        else tail = n;
    }
}

static void *malloc_locked(size_t n) {
    if (!n) return NULL;
    if (!allocation_size_ok(n)) { errno = ENOMEM; return NULL; }
    n = align_up(n);

    for (block_t *b = head; b; b = b->next) {
        if (b->magic != MAGIC_FREE || b->size < n) continue;
        split(b, n);
        b->magic = MAGIC_USED;
        return b + 1;
    }

    block_t *b = grow(n);
    if (!b) { errno = ENOMEM; return NULL; }
    split(b, n);
    b->magic = MAGIC_USED;
    return b + 1;
}

void *calloc(size_t count, size_t size) {
    if (count && size > (size_t)-1 / count) { errno = ENOMEM; return NULL; }
    size_t total = count * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

static void free_locked(void *p) {
    if (!p) return;
    block_t *b = (block_t *)p - 1;
    if (b->magic != MAGIC_USED) return;    /* double free or a foreign pointer */

    b->magic = MAGIC_FREE;

    /* Coalesce forwards, then backwards, but only across blocks that really
     * are adjacent in memory. */
    while (b->next && b->next->magic == MAGIC_FREE &&
        (char *)(b + 1) + b->size == (char *)b->next) {
        block_t *n = b->next;
        b->size += sizeof(block_t) + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
        else tail = b;
    }
    if (b->prev && b->prev->magic == MAGIC_FREE &&
        (char *)(b->prev + 1) + b->prev->size == (char *)b) {
        block_t *pv = b->prev;
        pv->size += sizeof(block_t) + b->size;
        pv->next = b->next;
        if (b->next) b->next->prev = pv;
        else tail = pv;
        b = pv;
    }

    /* Hand a large free tail back to the kernel. */
    if (b == tail && (char *)(b + 1) + b->size == heap_end && b->size >= GROW_MIN * 2) {
        size_t give = b->size - GROW_MIN;
        give &= ~(size_t)(4096 - 1);
        if (give) {
            if (sbrk(-(intptr_t)give) != (void *)-1) {
                b->size -= give;
                heap_end -= give;
            }
        }
    }
}

static void *realloc_locked(void *p, size_t n) {
    if (!p) return malloc_locked(n);
    if (!n) { free_locked(p); return NULL; }
    if (!allocation_size_ok(n)) { errno = ENOMEM; return NULL; }

    block_t *b = (block_t *)p - 1;
    if (b->magic != MAGIC_USED) return NULL;

    n = align_up(n);
    if (b->size >= n) { split(b, n); return p; }

    /* Absorb the next block when it is free and adjacent. */
    if (b->next && b->next->magic == MAGIC_FREE &&
        (char *)(b + 1) + b->size == (char *)b->next &&
        b->size + sizeof(block_t) + b->next->size >= n) {
        block_t *nx = b->next;
        b->size += sizeof(block_t) + nx->size;
        b->next = nx->next;
        if (nx->next) nx->next->prev = b;
        else tail = b;
        split(b, n);
        return p;
    }

    void *fresh = malloc_locked(n);
    if (!fresh) return NULL;
    memcpy(fresh, p, b->size);
    free_locked(p);
    return fresh;
}

void *malloc(size_t n) {
    if (!n) return NULL;
    heap_lock();
    void *result = malloc_locked(n);
    heap_unlock();
    return result;
}

void free(void *p) {
    if (!p) return;
    heap_lock();
    free_locked(p);
    heap_unlock();
}

void *realloc(void *p, size_t n) {
    heap_lock();
    void *result = realloc_locked(p, n);
    heap_unlock();
    return result;
}
