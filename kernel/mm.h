#ifndef KESTREL_MM_H
#define KESTREL_MM_H

#include "kernel.h"

/* ------------------------------------------------------------ page flags */
#define PTE_P    (1ULL << 0)
#define PTE_W    (1ULL << 1)
#define PTE_U    (1ULL << 2)
#define PTE_PWT  (1ULL << 3)
#define PTE_PCD  (1ULL << 4)
#define PTE_A    (1ULL << 5)
#define PTE_D    (1ULL << 6)
#define PTE_PS   (1ULL << 7)
#define PTE_G    (1ULL << 8)
/* Software-owned leaf bit: removing a user alias must not release a frame
 * whose lifetime belongs to a device or another kernel subsystem. */
#define PTE_BORROWED (1ULL << 9)
#define PTE_NX   (1ULL << 63)
#define PTE_ADDR 0x000FFFFFFFFFF000ULL

/* ------------------------------------------------------ physical memory */
void  pmm_init(void);
u64   pmm_alloc_page(void);                 /* returns 0 on failure          */
u64   pmm_alloc_pages(size_t count);        /* physically contiguous run     */
u64   pmm_alloc_pages_below(size_t count, u64 limit);  /* for legacy DMA     */
void  pmm_free_page(u64 phys);
void  pmm_free_pages(u64 phys, size_t count);
void  pmm_reserve(u64 phys, size_t count);  /* take a range out of service   */
u64   pmm_total_bytes(void);
u64   pmm_free_bytes(void);
u64   pmm_used_bytes(void);

/* Physically contiguous, page-aligned and zeroed, with its physical address
 * handed back: what a device that reads memory on its own needs.  On this
 * processor the direct map is cache-coherent with device access, so ordinary
 * memory is what a DMA buffer is made of.
 *
 * The caller must keep the physical address; the virtual one is inside the
 * direct map and can always be recovered with virt_to_phys. */
void *dma_alloc_pages(size_t pages, u64 *phys_out);
void  dma_free_pages(void *virt, size_t pages);

/* A zeroed page, ready for use as a page table or a fresh user page. */
static inline u64 pmm_alloc_zeroed(void) {
    u64 p = pmm_alloc_page();
    if (p) memset(phys_to_virt(p), 0, PAGE_SIZE);
    return p;
}

/* ------------------------------------------------------- virtual memory */
/* Table walks/mutations are serialized across CPUs. Removal waits for remote
 * TLB acknowledgments before recycling frames. This is not a mapping pin:
 * callers retaining translated physical pointers must separately keep the
 * address space/mapping alive. Destruction requires all CR3 users stopped. */
void  vmm_init(void);
u64   vmm_kernel_pml4(void);

bool  vmm_map(u64 pml4, u64 va, u64 pa, u64 flags);
bool  vmm_map_range(u64 pml4, u64 va, u64 pa, size_t bytes, u64 flags);
void  vmm_unmap(u64 pml4, u64 va);
void  vmm_unmap_range(u64 pml4, u64 va, size_t bytes);
bool  vmm_unmap_user_page(u64 pml4, u64 va); /* free only an owned user leaf */
/* Fresh zeroed user stack, page aligned, at most 64 pages. All-or-nothing leaf
 * ownership; refuses existing mappings. Caller retains address-space lifetime. */
bool  vmm_alloc_user_stack(u64 pml4, u64 va, size_t bytes);
/* Bounded 256-page retirement batches: each batch unlinks, acknowledges remote
 * invalidation, then frees owned frames. Borrowed frames are never freed. */
void  vmm_unmap_user_range(u64 pml4, u64 va, size_t bytes);
bool  vmm_unmap_borrowed_page(u64 pml4, u64 va, u64 expected_phys);
u64   vmm_translate(u64 pml4, u64 va);      /* 0 if not present              */
bool  vmm_is_mapped(u64 pml4, u64 va);
/* Permission validation under the VM lock, including every ancestor entry.
 * Validation ends when this call returns; it is not a retained mapping pin. */
bool  vmm_user_range_ok(u64 pml4, u64 va, size_t bytes, bool writable);
/* Bounded synchronous copy while permission validation and access share the
 * same VMM lock. kernel_buffer must be permanent kernel memory; pml4 must be
 * this CPU's current user address space. No retained pin after return. */
#define VMM_USER_COPY_MAX 16384u
bool  vmm_user_copy(u64 pml4, void *kernel_buffer, u64 va, size_t bytes, bool to_user);
/* Resolve an aligned readable user word and optionally sample it under one VM
 * lock. Caller retains the current address space; key zero may be valid.
 * No mapping lease survives return. value==NULL avoids reading the word. */
bool  vmm_user_word_key(u64 pml4, u64 va, u64 *key, u32 *value);
/* Retained address space must be current CR3. Validate writable user permissions,
 * atomically store the word and capture its physical futex key under one VM
 * lock. Caller serializes wake/registration separately. */
bool  vmm_user_store_word_key(u64 pml4, u64 va, u32 value, u64 *key);

/* Map device registers uncached into the kernel's MMIO window. */
void *vmm_map_mmio(u64 phys, size_t bytes);

/* Map video memory write-combining into the same window. */
void *vmm_map_wc(u64 phys, size_t bytes);

/* A fresh address space that shares every kernel (upper-half) mapping. */
u64   vmm_new_address_space(void);
void  vmm_destroy_address_space(u64 pml4);

static inline void vmm_switch(u64 pml4) { write_cr3(pml4); }

/* ------------------------------------------------------------------ heap */
void  heap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *krealloc(void *p, size_t size);
void  kfree(void *p);
size_t heap_used(void);
size_t heap_total(void);

#endif
