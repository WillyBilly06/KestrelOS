/* nv_vmm.c - the page tables the card reads memory through.
 *
 * A graphics card does not see physical addresses.  Every address in a push
 * buffer, every surface a window points at, every ring the engines read is a
 * graphics address, and the card walks a tree of page tables to turn it into
 * a real one.  Those tables are built by the driver - so until this exists,
 * nothing the driver hands the card is reachable by it.
 *
 * That is why this is not an optimisation or a convenience.  It is the thing
 * that makes an address the driver knows into an address the card can use.
 *
 * The layout below is Pascal's, and it is still Blackwell's.  Five levels for
 * four-kilobyte pages:
 *
 *     bits 48:47   the top directory        4 entries used of 512
 *     bits 46:38   a directory              512 entries, 8 bytes each
 *     bits 37:29   a directory              512 entries, 8 bytes each
 *     bits 28:21   the dual directory       256 entries, *sixteen* bytes each
 *     bits 20:12   the page table           512 entries, 8 bytes each
 *     bits 11:0    the offset into the page
 *
 * Three things about it are easy to get wrong and each produces a card that
 * faults on every access rather than one that is subtly slow:
 *
 *   The fourth level's entries are sixteen bytes, not eight, and they are two
 *   pointers rather than one: the first half points at a table of large pages
 *   and the second at a table of small ones.  A driver that writes a small
 *   page table's address into the first half has built a tree the card walks
 *   into the wrong table.
 *
 *   A directory entry has no valid bit.  What makes it valid is its aperture
 *   field being something other than zero - so an entry that has been given an
 *   address and no aperture is not a half-configured entry, it is an absent
 *   one, and the card will fault on it while the driver can see the address
 *   sitting right there.
 *
 *   Addresses are stored shifted right by four, not by twelve.  A page-aligned
 *   address shifted right by four lands with its low eight bits clear, which
 *   is where the flags live - so the encoding works out, but only if it is
 *   done in exactly that order.
 *
 * ---------------------------------------------------------------------------
 * The check at the end of this file is a walker written from the format rather
 * than from the code above: it reads raw entries and decodes their fields the
 * way the card's own walker does.  So what it establishes is that the tables
 * the driver builds are the tables the format describes - which is the whole
 * question - rather than that two pieces of code agree.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "nv.h"

/* Where each level's index sits in a graphics address. */
#define PT_SHIFT   12
#define PD0_SHIFT  21
#define PD1_SHIFT  29
#define PD2_SHIFT  38
#define PD3_SHIFT  47
#define PD4_SHIFT  56   /* the top level: 1 bit, 2 entries - Blackwell's page
                         * directory is SIX levels, and RM only accepts a root
                         * whose numEntries is 1<<1=2 (this level) */

#define PT_ENTRIES   512
#define PD0_ENTRIES  256
#define PD_ENTRIES   512

/* GMMU version 3 (Hopper / Blackwell, NV_MMU_VER3_*).  This is NOT the Pascal
 * "v2" format: the physical address sits IN PLACE at bits 51:12 (not shifted
 * down under the flags), and the old per-bit valid/volatile/priv/ro flags are
 * replaced by a Permission Control Field (PCF).  A Blackwell GMMU cannot walk a
 * v2 table at all - it reads the flag bits as address and faults - so getting
 * this exactly right is what makes the card execute anything.  Fields, from
 * dev_mmu.h gh100: VALID 0:0, APERTURE 2:1, PTE_PCF 7:3, KIND 11:8, PDE_PCF 5:3,
 * ADDRESS 51:12. */
#define ADDR_MASK  0x000FFFFFFFFFF000ull   /* the ADDRESS field, in place      */

#define APERTURE_INVALID    0u
/* A PTE numbers video memory 0; a PDE numbers it 1.  They are different tables
 * in the manual, so keep them apart here. */
#define PTE_APER_VRAM       0u
#define PTE_APER_SYSCOH     2u
#define PDE_APER_VRAM       1u
#define PDE_APER_SYSCOH     2u

/* PTE PCF (bits 7:3): regular (non-privileged), atomic-capable; cached for
 * video memory, uncached for the host's; the read-only variants add 4. */
#define PTE_PCF_RW_CACHED    0x10u   /* REGULAR_RW_ATOMIC_CACHED_ACD   (vram) */
#define PTE_PCF_RW_UNCACHED  0x11u   /* REGULAR_RW_ATOMIC_UNCACHED_ACD (sys)  */
#define PTE_PCF_RO_CACHED    0x14u
#define PTE_PCF_RO_UNCACHED  0x15u
/* PRIVILEGE variants (dev_mmu.h gh100: privilege = regular + 2).  GR/engine
 * context-switch state (the buffers PROMOTE_CTX maps) MUST live behind these:
 * the FECS/GPCCS context microcode enforces privileged access to its own save/
 * restore area and faults GR with ROBUST_CHANNEL_GR_EXCEPTION - no MMU fault,
 * since the translation is still valid - if it finds that state on a REGULAR
 * page.  nouveau r535_gr_promote_ctx maps every GR ctxbuf with .priv=1. */
#define PTE_PCF_RW_CACHED_PRIV    0x12u   /* PRIVILEGE_RW_ATOMIC_CACHED_ACD   */
#define PTE_PCF_RW_UNCACHED_PRIV  0x13u   /* PRIVILEGE_RW_ATOMIC_UNCACHED_ACD */
#define PTE_PCF_RO_CACHED_PRIV    0x16u   /* PRIVILEGE_RO_ATOMIC_CACHED_ACD   */
#define PTE_PCF_RO_UNCACHED_PRIV  0x17u   /* PRIVILEGE_RO_ATOMIC_UNCACHED_ACD */

/* PDE PCF (bits 5:3): a valid directory pointer. */
#define PDE_PCF_VRAM     2u   /* VALID_CACHED_ATS_NOT_ALLOWED    */
#define PDE_PCF_SYSCOH   1u   /* VALID_UNCACHED_ATS_ALLOWED      */

#define ENT_VALID        (1ull << 0)   /* PTE valid bit; a PDE is "valid" when
                                        * its aperture field is non-zero       */
#define ADDRESS_OF(e)    ((u64)(e) & ADDR_MASK)
#define APERTURE_OF(e)   ((u32)((((u64)(e)) >> 1) & 3))

/* ------------------------------------------------------------ the tables */

/* Somewhere to put a table.  Page tables have to be memory the card can read,
 * so they come out of a region the driver set aside for exactly that rather
 * than from the ordinary heap. */
static u64 take_page(nv_vmm_t *vmm, u64 *physical) {
    if (vmm->used >= vmm->pages) {
        kwarn("nv-vmm", "no room left for another page table");
        return 0;
    }

    u32 at = vmm->used++;
    u8 *host = vmm->pool + (size_t)at * PAGE_SIZE;
    memset(host, 0, PAGE_SIZE);

    if (physical) *physical = vmm->pool_gpu + (u64)at * PAGE_SIZE;
    return (u64)(uintptr_t)host;
}

static u64 *host_of(nv_vmm_t *vmm, u64 gpu) {
    if (gpu < vmm->pool_gpu) return NULL;
    u64 into = gpu - vmm->pool_gpu;
    if (into + PAGE_SIZE > (u64)vmm->pages * PAGE_SIZE) return NULL;
    return (u64 *)(vmm->pool + into);
}

static u64 descend(nv_vmm_t *vmm, u64 table_gpu, u32 index, bool dual,
                   bool second_half, bool create);

/* Materialise and describe the directory path containing one 512-MiB (2^29)
 * VA slot.  GSP-RM owns 4 GiB..4.5 GiB in every split client VASpace.  A
 * normal NVIDIA client RM reserves this path and sends the four backing level
 * instances to server RM, which fills its private mappings into the lowest
 * page-directory page.  Direct clients such as Kestrel must perform that step
 * themselves before a channel uses the VASpace.
 *
 * Blackwell's gh100_vmm_desc_16 path from a 512-MiB entry to the root is:
 * PD1(29), PD2(38), PD3(47), root(56).  The control lists it root-first, as
 * Nouveau r535_mmu_vaspace_new() does. */
bool nv_vmm_reserve_512m_levels(nv_vmm_t *vmm, u64 va,
                                nv_vmm_level_t levels[4]) {
    if (!vmm || !vmm->ready || !levels || (va & ((1ull << 29) - 1)))
        return false;

    memset(levels, 0, 4 * sizeof *levels);
    levels[0].phys_address = vmm->root_gpu;
    levels[0].size = 2u * sizeof(u64);       /* root has one index bit */
    levels[0].aperture = vmm->tables_in_vram ? 1u : 2u;
    levels[0].page_shift = 56;

    u64 pd3 = descend(vmm, vmm->root_gpu, (u32)((va >> PD4_SHIFT) & 1u),
                      false, false, true);
    if (!pd3) return false;
    levels[1].phys_address = pd3;
    levels[1].size = PAGE_SIZE;
    levels[1].aperture = levels[0].aperture;
    levels[1].page_shift = 47;

    u64 pd2 = descend(vmm, pd3, (u32)((va >> PD3_SHIFT) & (PD_ENTRIES - 1)),
                      false, false, true);
    if (!pd2) return false;
    levels[2].phys_address = pd2;
    levels[2].size = PAGE_SIZE;
    levels[2].aperture = levels[0].aperture;
    levels[2].page_shift = 38;

    u64 pd1 = descend(vmm, pd2, (u32)((va >> PD2_SHIFT) & (PD_ENTRIES - 1)),
                      false, false, true);
    if (!pd1) return false;
    levels[3].phys_address = pd1;
    levels[3].size = PAGE_SIZE;
    levels[3].aperture = levels[0].aperture;
    levels[3].page_shift = 29;
    return true;
}

bool nv_vmm_init(nv_vmm_t *vmm, u8 *pool, u64 pool_gpu, u32 pages,
                 bool tables_in_vram) {
    memset(vmm, 0, sizeof *vmm);
    if (!pool || pages < 2) return false;

    vmm->pool = pool;
    vmm->pool_gpu = pool_gpu;
    vmm->pages = pages;
    vmm->tables_in_vram = tables_in_vram;
    memset(pool, 0, (size_t)pages * PAGE_SIZE);

    /* The top of the tree, which is what the card is told about. */
    u64 physical = 0;
    if (!take_page(vmm, &physical)) return false;
    vmm->root_gpu = physical;

    vmm->ready = true;
    return true;
}

/* One level down, making the table if it is not there yet.  Returns where the
 * next level's table is, as a graphics address. */
static u64 descend(nv_vmm_t *vmm, u64 table_gpu, u32 index, bool dual,
                   bool second_half, bool create) {
    u64 *table = host_of(vmm, table_gpu);
    if (!table) return 0;

    u32 at = dual ? index * 2 + (second_half ? 1 : 0) : index;
    u64 entry = table[at];

    /* No valid bit: an aperture of zero is what absent means. */
    if (APERTURE_OF(entry) != APERTURE_INVALID) return ADDRESS_OF(entry);
    if (!create) return 0;

    u64 physical = 0;
    if (!take_page(vmm, &physical)) return 0;

    /* Address first, then the aperture that makes it count - so an entry is
     * never briefly valid and pointing at nothing.
     *
     * The aperture says where the table itself lives, not where the memory it
     * describes lives.  A card usually keeps its tables in its own memory; a
     * driver that puts them in the host's has to say so, or the card looks for
     * them in the wrong place entirely. */
    u64 built = (physical & ADDR_MASK);
    if (vmm->tables_in_vram) {
        built |= (PDE_PCF_VRAM << 3) | (PDE_APER_VRAM << 1);
    } else {
        built |= (PDE_PCF_SYSCOH << 3) | (PDE_APER_SYSCOH << 1);
    }
    /* IS_PTE (bit 0) stays 0: this is a page-directory entry, not a page.  For
     * the dual PD0 entry the small-page half is the second 64-bit word (`at` is
     * already index*2+1), and this same encoding lands in its APERTURE_SMALL /
     * PCF_SMALL / ADDRESS fields; the big half is left zero (no big pages). */
    __asm__ volatile("" ::: "memory");
    table[at] = built;

    return physical;
}

bool nv_vmm_map_kind(nv_vmm_t *vmm, u64 va, u64 pa, u64 bytes, bool vram,
                     bool read_only, bool priv, u32 kind) {
    if (!vmm->ready) return false;

    if ((va | pa | bytes) & (PAGE_SIZE - 1)) {
        kwarn("nv-vmm", "a mapping has to be whole pages: %llx to %llx, %llu "
                        "bytes", (unsigned long long)va, (unsigned long long)pa,
              (unsigned long long)bytes);
        return false;
    }
    if (va >> 57) {
        kwarn("nv-vmm", "%llx is outside the address space the tables cover",
              (unsigned long long)va);
        return false;
    }
    if (kind > 0xfu) {
        kwarn("nv-vmm", "PTE kind %#x does not fit the Blackwell VER3 field", kind);
        return false;
    }
    if (!bytes) return true;

    u64 aperture = vram ? PTE_APER_VRAM : PTE_APER_SYSCOH;
    u64 pcf;
    if (priv)
        pcf = vram ? (read_only ? PTE_PCF_RO_CACHED_PRIV   : PTE_PCF_RW_CACHED_PRIV)
                   : (read_only ? PTE_PCF_RO_UNCACHED_PRIV : PTE_PCF_RW_UNCACHED_PRIV);
    else
        pcf = vram ? (read_only ? PTE_PCF_RO_CACHED   : PTE_PCF_RW_CACHED)
                   : (read_only ? PTE_PCF_RO_UNCACHED : PTE_PCF_RW_UNCACHED);
    u64 flags = ENT_VALID | (aperture << 1) | (pcf << 3) |
                ((u64)kind << 8);

    for (u64 done = 0; done < bytes; done += PAGE_SIZE) {
        u64 address = va + done;

        /* The top level is a single bit (two entries); the card's page-directory
         * base points here and it walks six levels down, so this level must
         * exist or the walk lands one table short and faults. */
        u64 pd3 = descend(vmm, vmm->root_gpu,
                          (u32)((address >> PD4_SHIFT) & 1u),
                          false, false, true);
        if (!pd3) return false;

        u64 pd2 = descend(vmm, pd3,
                          (u32)((address >> PD3_SHIFT) & (PD_ENTRIES - 1)),
                          false, false, true);
        if (!pd2) return false;

        u64 pd1 = descend(vmm, pd2,
                          (u32)((address >> PD2_SHIFT) & (PD_ENTRIES - 1)),
                          false, false, true);
        if (!pd1) return false;

        u64 pd0 = descend(vmm, pd1,
                          (u32)((address >> PD1_SHIFT) & (PD_ENTRIES - 1)),
                          false, false, true);
        if (!pd0) return false;

        /* The dual entry.  Small pages hang off the second half of it, and
         * putting them in the first half builds a tree the card walks into a
         * table of large pages that is not there. */
        u64 pt = descend(vmm, pd0,
                         (u32)((address >> PD0_SHIFT) & (PD0_ENTRIES - 1)),
                         true, true, true);
        if (!pt) return false;

        u64 *entries = host_of(vmm, pt);
        if (!entries) return false;

        u32 index = (u32)((address >> PT_SHIFT) & (PT_ENTRIES - 1));
        if (!(entries[index] & 1ull)) vmm->mapped++;
        entries[index] = ((pa + done) & ADDR_MASK) | flags;
    }

    return true;
}

bool nv_vmm_map(nv_vmm_t *vmm, u64 va, u64 pa, u64 bytes, bool vram,
                bool read_only, bool priv) {
    /* Generic rings, pushbuffers, semaphores and pitch-linear scanouts use
     * kind zero.  RM-owned engine contexts use nv_vmm_map_kind() instead. */
    return nv_vmm_map_kind(vmm, va, pa, bytes, vram, read_only, priv, 0);
}

bool nv_vmm_unmap(nv_vmm_t *vmm, u64 va, u64 bytes) {
    if (!vmm->ready) return false;
    if ((va | bytes) & (PAGE_SIZE - 1)) return false;

    for (u64 done = 0; done < bytes; done += PAGE_SIZE) {
        u64 address = va + done;

        u64 pd3 = descend(vmm, vmm->root_gpu,
                          (u32)((address >> PD4_SHIFT) & 1u),
                          false, false, false);
        if (!pd3) continue;
        u64 pd2 = descend(vmm, pd3,
                          (u32)((address >> PD3_SHIFT) & (PD_ENTRIES - 1)),
                          false, false, false);
        if (!pd2) continue;
        u64 pd1 = descend(vmm, pd2,
                          (u32)((address >> PD2_SHIFT) & (PD_ENTRIES - 1)),
                          false, false, false);
        if (!pd1) continue;
        u64 pd0 = descend(vmm, pd1,
                          (u32)((address >> PD1_SHIFT) & (PD_ENTRIES - 1)),
                          false, false, false);
        if (!pd0) continue;
        u64 pt = descend(vmm, pd0,
                         (u32)((address >> PD0_SHIFT) & (PD0_ENTRIES - 1)),
                         true, true, false);
        if (!pt) continue;

        u64 *entries = host_of(vmm, pt);
        if (!entries) continue;

        u32 index = (u32)((address >> PT_SHIFT) & (PT_ENTRIES - 1));
        if (entries[index] & 1ull) {
            entries[index] = 0;
            vmm->mapped--;
        }
    }
    return true;
}

/* ==========================================================================
 * The walk, as the card does it.
 *
 * Written from the format rather than from the builder above: it reads raw
 * sixty-four bit entries and pulls the fields out of them by hand.  If the
 * builder puts something in the wrong place, this finds nothing there - which
 * is the entire point of writing it separately.
 * ==========================================================================
 */
typedef struct {
    bool present;
    u64  physical;
    u32  aperture;
    bool read_only;
    bool volatile_;
    int  levels_walked;
    const char *fault;
} nv_vmm_walk_t;

void nv_vmm_walk(const nv_vmm_t *vmm, u64 va, nv_vmm_walk_t *out) {
    memset(out, 0, sizeof *out);

    const u8 *pool = vmm->pool;
    u64 base = vmm->pool_gpu;
    u64 span = (u64)vmm->pages * PAGE_SIZE;

    u64 table = vmm->root_gpu;

    struct { int shift; int bits; bool dual; } level[5] = {
        { PD4_SHIFT, 1, false },
        { PD3_SHIFT, 9, false },
        { PD2_SHIFT, 9, false },
        { PD1_SHIFT, 9, false },
        { PD0_SHIFT, 8, true  },
    };

    for (int i = 0; i < 5; i++) {
        if (table < base || table - base + PAGE_SIZE > span) {
            out->fault = "a directory outside the memory the tables live in";
            return;
        }
        const u64 *entries = (const u64 *)(pool + (table - base));

        u32 index = (u32)((va >> level[i].shift) & ((1u << level[i].bits) - 1));
        /* The dual entry is sixteen bytes and the small-page half is the
         * second of the two. */
        u64 entry = level[i].dual ? entries[index * 2 + 1] : entries[index];

        u32 aperture = (u32)((entry >> 1) & 3);
        if (aperture == 0) {
            out->fault = "a directory entry with no aperture, which is what "
                         "absent means";
            return;
        }

        table = ADDRESS_OF(entry);
        out->levels_walked++;
    }

    if (table < base || table - base + PAGE_SIZE > span) {
        out->fault = "a page table outside the memory the tables live in";
        return;
    }

    const u64 *entries = (const u64 *)(pool + (table - base));
    u64 entry = entries[(va >> PT_SHIFT) & (PT_ENTRIES - 1)];

    if (!(entry & 1)) {
        out->fault = "nothing mapped here";
        return;
    }

    out->present = true;
    out->physical = ADDRESS_OF(entry) | (va & (PAGE_SIZE - 1));
    out->aperture = (u32)((entry >> 1) & 3);
    /* Decode the PCF (bits 7:3): value 0x1x, +4 for read-only, +1 for uncached. */
    u32 pcf = (u32)((entry >> 3) & 0x1f);
    out->read_only = (pcf & 0x4) != 0;
    out->volatile_ = (pcf & 0x1) != 0;
    out->levels_walked++;
}

/* ------------------------------------------------------------------- test */

#define POOL_PAGES 48

int nv_vmm_selftest(void) {
    int failures = 0;

    u64 pool_phys = 0;
    u8 *pool = dma_alloc_pages(POOL_PAGES, &pool_phys);
    if (!pool) {
        kerr("nv-vmm", "no memory for page tables");
        return 1;
    }

    static nv_vmm_t vmm;
    /* The tables are in the host's memory here, which is where a
     * driver puts them before it has anywhere else to put them. */
    if (!nv_vmm_init(&vmm, pool, pool_phys, POOL_PAGES, false)) {
        kerr("nv-vmm", "the address space would not come up");
        return 1;
    }

    /* An ordinary run of pages. */
    const u64 va = 0x0000200000ull;         /* 2 MiB in                      */
    const u64 pa = 0x0000123456000ull;
    if (!nv_vmm_map(&vmm, va, pa, PAGE_SIZE * 16, true, false, false)) {
        kerr("nv-vmm", "a sixteen page mapping was refused");
        failures++;
    } else {
        for (int i = 0; i < 16; i++) {
            nv_vmm_walk_t walk;
            nv_vmm_walk(&vmm, va + (u64)i * PAGE_SIZE + 0x40, &walk);

            if (!walk.present) {
                kerr("nv-vmm", "page %d does not resolve: %s", i,
                     walk.fault ? walk.fault : "no reason");
                failures++;
                break;
            }
            if (walk.physical != pa + (u64)i * PAGE_SIZE + 0x40) {
                kerr("nv-vmm", "page %d resolved to %llx, expected %llx", i,
                     (unsigned long long)walk.physical,
                     (unsigned long long)(pa + (u64)i * PAGE_SIZE + 0x40));
                failures++;
                break;
            }
            if (walk.aperture != PTE_APER_VRAM) {
                kerr("nv-vmm", "page %d came out in aperture %u", i,
                     walk.aperture);
                failures++;
                break;
            }
            if (walk.levels_walked != 6) {
                kerr("nv-vmm", "the walk took %d levels, expected 6",
                     walk.levels_walked);
                failures++;
                break;
            }
        }
    }

    /* One that crosses the boundary where a new page table has to be made -
     * two megabytes, which is where the fourth level's index changes. */
    {
        u64 edge = 0x00003FF000ull;          /* the last page below 4 MiB    */
        if (!nv_vmm_map(&vmm, edge, 0x900000000ull, PAGE_SIZE * 2, true, false, false)) {
            kerr("nv-vmm", "a mapping across a table boundary was refused");
            failures++;
        } else {
            nv_vmm_walk_t a, b;
            nv_vmm_walk(&vmm, edge, &a);
            nv_vmm_walk(&vmm, edge + PAGE_SIZE, &b);
            if (!a.present || !b.present ||
                a.physical != 0x900000000ull ||
                b.physical != 0x900001000ull) {
                kerr("nv-vmm", "the two sides of a table boundary did not both "
                               "resolve");
                failures++;
            }
        }
    }

    /* And one high enough to need a different branch of the whole tree, which
     * is what catches an index taken from the wrong bits. */
    {
        u64 high = 0x0000400000000ull;       /* 16 GiB in                    */
        if (!nv_vmm_map(&vmm, high, 0xABCDE000ull, PAGE_SIZE, false, true, false)) {
            kerr("nv-vmm", "a mapping high in the address space was refused");
            failures++;
        } else {
            nv_vmm_walk_t walk;
            nv_vmm_walk(&vmm, high, &walk);
            if (!walk.present || walk.physical != 0xABCDE000ull) {
                kerr("nv-vmm", "the high mapping did not resolve: %s",
                     walk.fault ? walk.fault : "wrong address");
                failures++;
            } else if (!walk.read_only) {
                kerr("nv-vmm", "a read-only mapping came back writable");
                failures++;
            } else if (walk.aperture != PTE_APER_SYSCOH) {
                kerr("nv-vmm", "host memory came out in aperture %u",
                     walk.aperture);
                failures++;
            }
        }
    }

    /* Nothing is mapped where nothing was mapped, and the walk says which
     * level it stopped at rather than returning a plausible address. */
    {
        nv_vmm_walk_t walk;
        nv_vmm_walk(&vmm, 0x7000000000ull, &walk);
        if (walk.present) {
            kerr("nv-vmm", "an address nobody mapped resolved anyway");
            failures++;
        }
    }

    /* Unmapping takes it away. */
    if (!nv_vmm_unmap(&vmm, va, PAGE_SIZE * 16)) {
        kerr("nv-vmm", "the mapping would not come down");
        failures++;
    } else {
        nv_vmm_walk_t walk;
        nv_vmm_walk(&vmm, va, &walk);
        if (walk.present) {
            kerr("nv-vmm", "an unmapped page still resolves");
            failures++;
        }
    }

    /* A mapping that is not whole pages has to be refused, because half a page
     * cannot be described by an entry that covers a whole one. */
    if (nv_vmm_map(&vmm, va + 0x800, pa, PAGE_SIZE, true, false, false)) {
        kerr("nv-vmm", "a mapping that is not page aligned was accepted");
        failures++;
    }

    /* And one outside what the tables can reach. */
    if (nv_vmm_map(&vmm, 1ull << 50, pa, PAGE_SIZE, true, false, false)) {
        kerr("nv-vmm", "an address outside the address space was accepted");
        failures++;
    }

    if (!failures)
        kinfo("nv-vmm", "the card's page tables work: %u pages mapped across "
                        "five levels, %u tables built, addresses high and low "
                        "resolving through a walk written from the format "
                        "rather than from the builder",
              vmm.mapped, vmm.used);
    return failures;
}
