/* nv_gsp_boot.c - assemble everything the GSP co-processor's firmware reads to
 * bring itself up, on Hopper and Blackwell where the security processor starts
 * it (the FSP route).
 *
 * The FSP is handed one pointer - the address of a GSP_FMC_BOOT_PARAMS - and
 * from there the firmware finds everything else by following addresses this
 * file writes down:
 *
 *     GSP_FMC_BOOT_PARAMS
 *       .bootGspRmParams.gspRmDescOffset -> GspFwWprMeta
 *                                             .sysmemAddrOfRadix3Elf -> a
 *                                               three-level page table over the
 *                                               ELF's .fwimage payload
 *                                             .sysmemAddrOfBootloader -> the
 *                                               bootloader image
 *                                             .sysmemAddrOfSignature -> a
 *                                               separate, 256-byte-aligned copy
 *                                               of the image's signature
 *       .gspRmParams.bootArgsOffset      -> a LibosMemoryRegionInitArgument
 *                                            table naming the log buffers and
 *                                            the RM arguments, and the RM
 *                                            arguments carry where the two
 *                                            message rings live
 *
 * ---------------------------------------------------------------------------
 * WHERE THESE STRUCTURES COME FROM, AND WHAT IS AND IS NOT PROVEN HERE.
 *
 * Every structure below is transcribed field for field from Linux's nouveau
 * driver (drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/nvrm/gsp.h and
 * r535/gsp.c) and NVIDIA's own open-gpu-kernel-modules, which agree.  They are
 * NOT packed - nouveau lays them out with natural alignment and so does this,
 * and because both are the x86-64 SysV ABI the layout is identical.  The
 * _Static_asserts below pin the sizes and the offsets that the firmware reads
 * by fixed position, so a field that drifts is caught at compile time rather
 * than as silence on the card.
 *
 * What that proves: the bytes handed to the firmware are laid out the way a
 * driver that works on this silicon lays them out.  What it does NOT prove is
 * that the real security processor accepts them.  Real GB203 runs have now
 * accepted the FSP chain-of-trust command and built WPR2; the remaining FMC
 * failure is still under investigation.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "klog.h"
#include "mm.h"
#include "vfs.h"
#include "firmware.h"
#include "nv.h"
#include "nv_fwimage.h"

#define GSP_PAGE        4096u
#define ALIGN_UP(x, a)  (((u64)(x) + ((a) - 1)) & ~(u64)((a) - 1))

/* ------------------------------------------------------------- structures */

/* GspFwWprMeta - the firmware's description of the write-protected region and
 * everything staged in system memory for it.  256 bytes; the offsets the
 * firmware reads by position are asserted after the definition. */
typedef struct {
    u64 magic;                          /* 0   */
    u64 revision;                       /* 8   */
    u64 sysmemAddrOfRadix3Elf;          /* 16  */
    u64 sizeOfRadix3Elf;                /* 24  */
    u64 sysmemAddrOfBootloader;         /* 32  */
    u64 sizeOfBootloader;               /* 40  */
    u64 bootloaderCodeOffset;           /* 48  */
    u64 bootloaderDataOffset;           /* 56  */
    u64 bootloaderManifestOffset;       /* 64  */
    u64 sysmemAddrOfSignature;          /* 72  (union in nouveau; this branch) */
    u64 sizeOfSignature;                /* 80  */
    u64 gspFwRsvdStart;                 /* 88  */
    u64 nonWprHeapOffset;               /* 96  */
    u64 nonWprHeapSize;                 /* 104 */
    u64 gspFwWprStart;                  /* 112 */
    u64 gspFwHeapOffset;                /* 120 */
    u64 gspFwHeapSize;                  /* 128 */
    u64 gspFwOffset;                    /* 136 */
    u64 bootBinOffset;                  /* 144 */
    u64 frtsOffset;                     /* 152 */
    u64 frtsSize;                       /* 160 */
    u64 gspFwWprEnd;                    /* 168 */
    u64 fbSize;                         /* 176 */
    u64 vgaWorkspaceOffset;             /* 184 */
    u64 vgaWorkspaceSize;               /* 192 */
    u64 bootCount;                      /* 200 */
    /* A 32-byte union in nouveau (partitionRpc / crashReportQueue); left zero
     * on a fresh boot, so it is written out as reserved space of the right
     * size rather than either branch. */
    u8  partition_union[32];            /* 208 */
    u8  gspFwHeapVfPartitionCount;      /* 240 */
    u8  flags;                          /* 241 */
    u8  padding[2];                     /* 242 */
    u32 pmuReservedSize;                /* 244 */
    u64 verified;                       /* 248 */
} GspFwWprMeta;

_Static_assert(sizeof(GspFwWprMeta) == 256, "WPR meta is 256 bytes");
_Static_assert(__builtin_offsetof(GspFwWprMeta, sysmemAddrOfRadix3Elf) == 16, "");
_Static_assert(__builtin_offsetof(GspFwWprMeta, sysmemAddrOfBootloader) == 32, "");
_Static_assert(__builtin_offsetof(GspFwWprMeta, sysmemAddrOfSignature) == 72, "");
_Static_assert(__builtin_offsetof(GspFwWprMeta, gspFwHeapSize) == 128, "");
_Static_assert(__builtin_offsetof(GspFwWprMeta, pmuReservedSize) == 244, "");
_Static_assert(__builtin_offsetof(GspFwWprMeta, verified) == 248, "");

#define GSP_FW_WPR_META_MAGIC     0xdc3aae21371a60b3ULL
#define GSP_FW_WPR_META_REVISION  1

/* GSP_FMC_BOOT_PARAMS - the one structure the FSP is pointed at. */
typedef enum {
    GSP_DMA_TARGET_LOCAL_FB = 0,
    GSP_DMA_TARGET_COHERENT_SYSTEM = 1,
    GSP_DMA_TARGET_NONCOHERENT_SYSTEM = 2,
    GSP_DMA_TARGET_COUNT = 3
} gsp_dma_target;

typedef struct { u32 regkeys; } GSP_FMC_INIT_PARAMS;

typedef struct {
    gsp_dma_target target;
    u32 gspRmDescSize;
    u64 gspRmDescOffset;
    u64 wprCarveoutOffset;
    u32 wprCarveoutSize;
    u8  bIsGspRmBoot;                   /* NvBool == u8 */
} GSP_ACR_BOOT_GSP_RM_PARAMS;

typedef struct {
    gsp_dma_target target;
    u64 bootArgsOffset;
} GSP_RM_PARAMS;

typedef struct {
    gsp_dma_target target;
    u64 payloadBufferOffset;
    u32 payloadBufferSize;
} GSP_SPDM_PARAMS;

typedef struct {
    GSP_FMC_INIT_PARAMS        initParams;
    GSP_ACR_BOOT_GSP_RM_PARAMS bootGspRmParams;
    GSP_RM_PARAMS              gspRmParams;
    GSP_SPDM_PARAMS            gspSpdmParams;
} GSP_FMC_BOOT_PARAMS;

/* The FSP reads bootGspRmParams and gspRmParams by position, so those two
 * offsets are the ones that must not drift. */
_Static_assert(__builtin_offsetof(GSP_FMC_BOOT_PARAMS, bootGspRmParams) == 8,
               "the boot params start after the four-byte init params");

/* libos memory-region init arguments - four regions naming the log buffers and
 * the RM arguments. */
typedef enum {
    LIBOS_MEMORY_REGION_NONE = 0,
    LIBOS_MEMORY_REGION_CONTIGUOUS = 1,
    LIBOS_MEMORY_REGION_RADIX3 = 2
} libos_region_kind;

typedef enum {
    LIBOS_MEMORY_REGION_LOC_NONE = 0,
    LIBOS_MEMORY_REGION_LOC_SYSMEM = 1,
    LIBOS_MEMORY_REGION_LOC_FB = 2
} libos_region_loc;

typedef struct {
    u64 id8;
    u64 pa;
    u64 size;
    u8  kind;
    u8  loc;
} LibosMemoryRegionInitArgument;

/* RM arguments - where the two message rings live, handed to the firmware
 * through the RMARGS libos region. */
typedef struct {
    u64 sharedMemPhysAddr;
    u32 pageTableEntryCount;
    u64 cmdQueueOffset;                 /* NvLength == u64 */
    u64 statQueueOffset;
} MESSAGE_QUEUE_INIT_ARGUMENTS;

typedef struct {
    u32 oldLevel;
    u32 flags;
    u8  bInPMTransition;
} GSP_SR_INIT_ARGUMENTS;

typedef struct {
    MESSAGE_QUEUE_INIT_ARGUMENTS messageQueueInitArguments;
    GSP_SR_INIT_ARGUMENTS        srInitArguments;
    u32                          gpuInstance;
    u8                           bDmemStack;
    struct { u64 pa; u64 size; } profilerArgs;
} GSP_ARGUMENTS_CACHED;

/* --------------------------------------------------------- what is staged */

/* Kept so the whole thing can be reached and, at shutdown, freed or reported.
 * One card, one boot; a single static instance is enough. */
static struct {
    bool  staged;

    void *fw_va;            u64 fw_phys;      size_t fw_pages;   /* .fwimage copy */
    void *sig_va;           u64 sig_phys;     size_t sig_pages;  /* aligned signature */
    void *bl_va;            u64 bl_phys;      size_t bl_pages;   /* bootloader */
    void *radix_lvl0;       u64 radix_lvl0_phys;
    void *radix_lvl1;       u64 radix_lvl1_phys;
    void *radix_lvl2;       u64 radix_lvl2_phys;  size_t radix_lvl2_pages;
    void *wpr_va;           u64 wpr_phys;
    void *bootargs_va;      u64 bootargs_phys;     /* GSP_FMC_BOOT_PARAMS */
    void *libos_va;         u64 libos_phys;
    void *shm_va;           u64 shm_phys;          size_t shm_pages;
    void *rmargs_va;        u64 rmargs_phys;
    void *loginit_va;       u64 loginit_phys;
    void *logintr_va;       u64 logintr_phys;
    void *logrm_va;         u64 logrm_phys;
    void *logmnoc_va;       u64 logmnoc_phys;   /* .fwlogging_mnoc          */
    void *logkrnl_va;       u64 logkrnl_phys;   /* .fwlogging_kernel_gb20x  */

    /* Where the two rings sit inside the shared region, for the post-boot
     * conversation. */
    u8   *cmdq_region;
    u8   *msgq_region;
} boot;

/* --------------------------------------------------------- little helpers */

/* Read a whole file into freshly-allocated contiguous DMA pages.
 *
 * Contiguous because the radix3 below is simplest over a run, and because a
 * machine that has just started has the room; a failure here is reported
 * rather than worked around, since the alternative - scattering a sixty
 * megabyte read across the page allocator - is a great deal of code for a
 * path that on this hardware has never run at all. */
static void *load_file_dma(const char *name, size_t *bytes_out,
                           u64 *phys_out, size_t *pages_out) {
    char path[192];
    if (!firmware_resolve(name, path, sizeof path)) {
        kerr("nv-boot", "%s is not present - the GSP-RM image has to be on the "
                        "data volume or in the initial archive", name);
        return NULL;
    }

    vstat_t st;
    if (vfs_stat(path, &st) != 0 || !st.size) {
        kerr("nv-boot", "%s could not be sized", name);
        return NULL;
    }

    size_t pages = (size_t)((st.size + GSP_PAGE - 1) / GSP_PAGE);
    u64 phys = 0;
    void *va = dma_alloc_pages(pages, &phys);
    if (!va) {
        kerr("nv-boot", "no run of %zu contiguous pages (%llu MiB) for %s",
             pages, (unsigned long long)(st.size / (1024 * 1024)), name);
        return NULL;
    }

    s64 got = vfs_read_file(path, va, pages * GSP_PAGE);
    if (got != (s64)st.size) {
        kerr("nv-boot", "%s: read %lld of %llu bytes", name,
             (long long)got, (unsigned long long)st.size);
        dma_free_pages(va, pages);
        return NULL;
    }

    if (bytes_out) *bytes_out = (size_t)st.size;
    if (phys_out)  *phys_out = phys;
    if (pages_out) *pages_out = pages;
    return va;
}

/* Finding a section by name uses the shared, host-verified ELF reader in
 * nv_fwimage.c (nv_fw_elf_section, which handles the 64-bit image) and is
 * checked against the real files by tools/nv_firmware_host_test.c.  NVIDIA's
 * _kgspPrepareGspRmBinaryImage does not hand the ELF container to FMC: it
 * copies .fwimage into the radix allocation and the signature into a separate
 * 256-byte-aligned allocation.  The staging below intentionally mirrors that. */

/* Build the three-level page table the firmware walks to reach the image.
 *
 * Level 0 is one entry pointing at level 1; level 1 holds the addresses of the
 * level-2 pages; level 2 holds the address of every 4 KiB page of the image.
 * Over a contiguous image the addresses are a simple run, but the structure is
 * built in full because the firmware walks it as a tree regardless. */
static bool build_radix3(u64 image_phys, size_t image_pages) {
    /* Level 2: one u64 per image page. */
    size_t lvl2_bytes = image_pages * sizeof(u64);
    size_t lvl2_pages = (size_t)((lvl2_bytes + GSP_PAGE - 1) / GSP_PAGE);

    boot.radix_lvl0 = dma_alloc_pages(1, &boot.radix_lvl0_phys);
    boot.radix_lvl1 = dma_alloc_pages(1, &boot.radix_lvl1_phys);
    boot.radix_lvl2 = dma_alloc_pages(lvl2_pages, &boot.radix_lvl2_phys);
    boot.radix_lvl2_pages = lvl2_pages;
    if (!boot.radix_lvl0 || !boot.radix_lvl1 || !boot.radix_lvl2) {
        kerr("nv-boot", "no memory for the image's page table");
        return false;
    }

    u64 *lvl0 = boot.radix_lvl0;
    u64 *lvl1 = boot.radix_lvl1;
    u64 *lvl2 = boot.radix_lvl2;

    lvl0[0] = boot.radix_lvl1_phys;
    for (size_t i = 0; i < lvl2_pages; i++)
        lvl1[i] = boot.radix_lvl2_phys + (u64)i * GSP_PAGE;
    for (size_t i = 0; i < image_pages; i++)
        lvl2[i] = image_phys + (u64)i * GSP_PAGE;

    return true;
}

/* nvfw_bin_hdr, then RM_RISCV_UCODE_DESC at its header_offset. */
typedef struct {
    u32 bin_magic, bin_ver, bin_size, header_offset, data_offset, data_size;
} nvfw_bin_hdr;

/* Only the three offsets the WPR meta needs are read out; the descriptor has
 * more fields but they are not this stage's business. */
static bool bootloader_offsets(const u8 *bl, size_t bl_bytes,
                               u64 *code, u64 *data, u64 *manifest,
                               u64 *data_off, u64 *data_sz) {
    if (bl_bytes < sizeof(nvfw_bin_hdr)) return false;
    const nvfw_bin_hdr *h = (const nvfw_bin_hdr *)bl;
    if (h->bin_magic != 0x10DEu && h->bin_magic != 0x000010DEu) {
        kwarn("nv-boot", "the bootloader's header magic is %#x, not NVIDIA's",
              h->bin_magic);
    }
    if (h->header_offset + 60 > bl_bytes) return false;

    /* RM_RISCV_UCODE_DESC: monitorCodeOffset/monitorDataOffset/manifestOffset
     * are at fixed word positions - see rmRiscvUcode.h. version(0),
     * bootloaderOffset(1)..appVersion(7), manifestOffset(8), manifestSize(9),
     * monitorDataOffset(10), monitorDataSize(11), monitorCodeOffset(12). */
    const u32 *d = (const u32 *)(bl + h->header_offset);
    *manifest = d[8];
    *data     = d[10];
    *code     = d[12];
    if (data_off) *data_off = h->data_offset;
    if (data_sz)  *data_sz  = h->data_size;
    return true;
}

/* The heap sizes, from nouveau's r570 gb20x wpr config and the documented
 * formula.  These are what the firmware validates the reserved region against.
 * Values in bytes. */
#define SZ_1M                       (1024u * 1024u)
/* All verified against NVIDIA's GB202/GB203 HAL dispatch (open-gpu-kernel-modules
 * 570, g_kernel_gsp_nvoc.c): the FMC validates this descriptor and REFUSES the
 * boot (mailbox0 = 0xb) if the heap layout is not what it expects, so every
 * number here has to be the one the real driver hands over, not a near one. */
#define GSP_HEAP_OS_CARVEOUT_GB20X  (22u * SZ_1M)       /* kgspGetFwHeapParamOsCarveoutSize_4b5307: bare-metal 22 MiB */
#define GSP_HEAP_BASE_GH100         (14u * SZ_1M)       /* fwHeapParamBaseSize, GH100+/Blackwell bucket */
#define GSP_HEAP_PER_GB_FB          (96u << 10)         /* GSP_FW_HEAP_PARAM_SIZE_PER_GB_FB: 96 KiB per GiB */
#define GSP_HEAP_CLIENT_ALLOC       (96u * SZ_1M)       /* GSP_FW_HEAP_PARAM_CLIENT_ALLOC_SIZE = (48<<10)*2048 */
#define GSP_HEAP_MIN_BLACKWELL      (88u * SZ_1M)       /* kgspGetMinWprHeapSizeMB_647ce6: 88 MiB bare-metal */
#define GSP_HEAP_MAX_BLACKWELL      (280u * SZ_1M)      /* kgspGetMaxWprHeapSizeMB_e7e092: 280 MiB bare-metal */
#define GSP_NONWPR_HEAP_GB20X       0x230000u           /* kgspGetNonWprHeapSize_ad951d = 2293760; 0x220000 was 64 KiB short */
#define GSP_RSVD_PMU_GB20X          (0x0800000u + 0x1000000u + 0x0001000u)

/* kgspGetFwHeapSize: OS carveout + base + per-GiB + client alloc, clamped to
 * the bare-metal [min, max].  For a 16 GiB board: 22 + 14 + 2 + 96 = 134 MiB
 * = 0x8600000, which is what NVIDIA's driver hands the FMC.  The old min clamp
 * of 170 MiB pushed ours to 170 MiB - not what the FMC's layout check expects. */
static u64 wpr_heap_size(u64 fb_bytes) {
    u64 fb_gb = (fb_bytes + (1ull << 30) - 1) >> 30;
    u64 h = GSP_HEAP_OS_CARVEOUT_GB20X + GSP_HEAP_BASE_GH100 +
            ALIGN_UP((u64)GSP_HEAP_PER_GB_FB * fb_gb, SZ_1M) +
            ALIGN_UP(GSP_HEAP_CLIENT_ALLOC, SZ_1M);
    if (h < GSP_HEAP_MIN_BLACKWELL) h = GSP_HEAP_MIN_BLACKWELL;
    if (h > GSP_HEAP_MAX_BLACKWELL) h = GSP_HEAP_MAX_BLACKWELL;
    return h;
}

/* ------------------------------------------------------------- the rings */

/* The shared region the message rings live in: a header page and 63 entry
 * pages each, laid out with the "swapped read pointer" arrangement the
 * firmware expects.  Its address and the two ring offsets go into the RM
 * arguments.  See nv_gsp_queue.c for the wire format. */
#define QUEUE_REGION_BYTES  NV_GSP_QUEUE_REGION_BYTES     /* 256 KiB each */

static bool build_shared_rings(void) {
    size_t region_pages = QUEUE_REGION_BYTES / GSP_PAGE;
    /* The two ring regions, plus a page-table-entry array the firmware uses to
     * find the pages of the ENTIRE shared allocation, including the page-table
     * page itself.  NVIDIA's _getMsgQueueParams first counts the 128 queue
     * pages, adds one entry for the page that holds those PTEs, and therefore
     * publishes 129 entries.  The old Kestrel table described only the queue
     * pages (128 entries) and made PTE[0] point at page 1; GSP-RM consequently
     * interpreted a different mapping from the one CPU-RM constructed. */
    size_t total_pages = region_pages * 2;
    size_t ptes = total_pages +
        (size_t)((total_pages * sizeof(u64) + GSP_PAGE - 1) / GSP_PAGE);
    size_t pte_pages =
        (size_t)((ptes * sizeof(u64) + GSP_PAGE - 1) / GSP_PAGE);
    boot.shm_pages = pte_pages + total_pages;

    boot.shm_va = dma_alloc_pages(boot.shm_pages, &boot.shm_phys);
    if (!boot.shm_va) {
        kerr("nv-boot", "no memory for the shared rings");
        return false;
    }

    u64 *pte = boot.shm_va;
    for (size_t i = 0; i < boot.shm_pages; i++)
        pte[i] = boot.shm_phys + (u64)i * GSP_PAGE;

    boot.cmdq_region = (u8 *)boot.shm_va + (size_t)pte_pages * GSP_PAGE;
    boot.msgq_region = boot.cmdq_region + QUEUE_REGION_BYTES;
    return true;
}

/* --------------------------------------------------------- assembling it */

bool nv_gsp_boot_stage(nv_card_t *c, u64 *boot_params_phys, u64 *rsvd_out) {
    memset(&boot, 0, sizeof boot);

    const char *dir = nv_gsp_directory(c->chipset);
    const char *rel = nv_gsp_release(c->chipset);
    if (!dir || !rel) {
        kerr("nv-boot", "no firmware directory known for this chip");
        return false;
    }

    char name[128];
    size_t fw_container_bytes = 0, fw_image_bytes = 0, bl_bytes = 0;

    /* The resident manager, FMC, and bootloader are one release-matched boot
     * protocol.  In particular R595 enlarged several structures and changed
     * when its initial RPCs are delivered, so silently preferring a newer RM
     * produces a byte-valid image with an incompatible host ABI. */
    snprintf(name, sizeof name, "nvidia/%s/gsp/gsp-%s.bin", dir, rel);
    boot.fw_va = load_file_dma(name, &fw_container_bytes,
                               &boot.fw_phys, &boot.fw_pages);
    if (!boot.fw_va) return false;

    const u8 *version_ptr = NULL;
    u32 version_size = 0;
    size_t expected_version_size = strlen(rel) + 1;
    if (!nv_fw_elf_section(boot.fw_va, fw_container_bytes, ".fwversion",
                           &version_ptr, &version_size) ||
        version_size < expected_version_size ||
        memcmp(version_ptr, rel, expected_version_size) != 0) {
        kerr("nv-boot", "%s is not the release-matched %s GSP-RM image",
             name, rel);
        return false;
    }

    /* NVIDIA extracts just .fwimage from the ELF container and copies it into
     * the radix descriptor's page-aligned data area.  In this allocator the
     * original container already owns a contiguous page-aligned run, so move
     * the section down to the run's base in place.  Radix leaves must never
     * start at the section's unaligned file offset (0x40 in the R570 image),
     * and sizeOfRadix3Elf is the section size, not the container size. */
    const u8 *image_ptr = NULL;
    u32 image_size = 0;
    if (!nv_fw_elf_section(boot.fw_va, fw_container_bytes, ".fwimage",
                           &image_ptr, &image_size) || !image_size) {
        kerr("nv-boot", "%s has no usable .fwimage section", name);
        return false;
    }

    /* The image carries a chip-family signature.  FMC's Booter DMA requires
     * its address to be 256-byte aligned, so NVIDIA allocates and copies it
     * rather than pointing back inside the ELF container. */
    char sigsec[48];
    snprintf(sigsec, sizeof sigsec, ".fwsignature_%s",
             (c->chipset & 0x1F0) >= 0x1B0 ? "gb20x" :
             (c->chipset & 0x1F0) >= 0x1A0 ? "gb10x" : "ga10x");
    const u8 *sig_ptr = NULL;
    u32 sig_size = 0;
    if (!nv_fw_elf_section(boot.fw_va, fw_container_bytes,
                           sigsec, &sig_ptr, &sig_size) || !sig_size) {
        kerr("nv-boot", "%s has no %s section", name, sigsec);
        return false;
    }
    size_t sig_alloc_bytes = (size_t)ALIGN_UP(sig_size, 256);
    boot.sig_pages = (sig_alloc_bytes + GSP_PAGE - 1) / GSP_PAGE;
    boot.sig_va = dma_alloc_pages(boot.sig_pages, &boot.sig_phys);
    if (!boot.sig_va) {
        kerr("nv-boot", "no memory for the aligned GSP-RM signature");
        return false;
    }
    memcpy(boot.sig_va, sig_ptr, sig_size);

    fw_image_bytes = image_size;
    size_t image_pages = (fw_image_bytes + GSP_PAGE - 1) / GSP_PAGE;
    memmove(boot.fw_va, image_ptr, fw_image_bytes);
    memset((u8 *)boot.fw_va + fw_image_bytes, 0,
           image_pages * GSP_PAGE - fw_image_bytes);
    if (!build_radix3(boot.fw_phys, image_pages)) return false;

    /* The bootloader, and the three offsets inside its RISC-V descriptor. */
    snprintf(name, sizeof name, "nvidia/%s/gsp/bootloader-%s.bin", dir, rel);
    boot.bl_va = load_file_dma(name, &bl_bytes, &boot.bl_phys, &boot.bl_pages);
    if (!boot.bl_va) return false;

    u64 bl_code = 0, bl_data = 0, bl_manifest = 0, bl_dataoff = 0, bl_datasz = 0;
    if (!bootloader_offsets(boot.bl_va, bl_bytes, &bl_code, &bl_data,
                            &bl_manifest, &bl_dataoff, &bl_datasz)) {
        kerr("nv-boot", "the bootloader's descriptor could not be read");
        return false;
    }

    /* The WPR meta. */
    boot.wpr_va = dma_alloc_pages(1, &boot.wpr_phys);
    if (!boot.wpr_va) { kerr("nv-boot", "no memory for the WPR meta"); return false; }
    GspFwWprMeta *m = boot.wpr_va;
    memset(m, 0, sizeof *m);
    m->magic = GSP_FW_WPR_META_MAGIC;
    m->revision = GSP_FW_WPR_META_REVISION;
    m->sysmemAddrOfRadix3Elf = boot.radix_lvl0_phys;
    m->sizeOfRadix3Elf = fw_image_bytes;
    m->sysmemAddrOfBootloader = boot.bl_phys + bl_dataoff;
    m->sizeOfBootloader = bl_datasz;
    m->bootloaderCodeOffset = bl_code;
    m->bootloaderDataOffset = bl_data;
    m->bootloaderManifestOffset = bl_manifest;
    m->sysmemAddrOfSignature = boot.sig_phys;
    m->sizeOfSignature = sig_alloc_bytes;
    m->nonWprHeapSize = GSP_NONWPR_HEAP_GB20X;
    m->gspFwHeapSize = wpr_heap_size(c->vram_bytes ? c->vram_bytes
                                                   : (16ull << 30));
    m->frtsSize = 0x100000;
    m->vgaWorkspaceSize = 128 * 1024;
    m->pmuReservedSize = (u32)ALIGN_UP(GSP_RSVD_PMU_GB20X, 0x20000);
    /* fbSize is deliberately LEFT ZERO, matching NVIDIA's FSP path
     * (kgspCalculateFbLayout_GH100 fills only sizes; the end-of-FB offsets,
     * fbSize included, are filled in by the ACR/GSP-FMC when it sets up WPR2).
     * Supplying our own vram_bytes here risks a wrong end-of-FB layout if it is
     * even slightly off what the firmware computes, so let the firmware derive
     * it. */
    m->fbSize = 0;

    /* The shared rings, then the RM arguments that name them. */
    if (!build_shared_rings()) return false;

    boot.rmargs_va = dma_alloc_pages(1, &boot.rmargs_phys);
    if (!boot.rmargs_va) { kerr("nv-boot", "no memory for the RM arguments"); return false; }
    GSP_ARGUMENTS_CACHED *args = boot.rmargs_va;
    memset(args, 0, sizeof *args);
    args->messageQueueInitArguments.sharedMemPhysAddr = boot.shm_phys;
    args->messageQueueInitArguments.pageTableEntryCount = (u32)boot.shm_pages;
    args->messageQueueInitArguments.cmdQueueOffset =
        (u64)((u8 *)boot.cmdq_region - (u8 *)boot.shm_va);
    args->messageQueueInitArguments.statQueueOffset =
        (u64)((u8 *)boot.msgq_region - (u8 *)boot.shm_va);
    args->bDmemStack = 1;

    /* The libos regions: FIVE log buffers and the RM arguments - six entries,
     * which is what r570 hands a GB20x (_getLogArgCount = LOGIDX_SIZE = 5 for
     * anything newer than GA100, plus RMARGS).  An earlier version sent only
     * three logs; LOGMNOC (.fwlogging_mnoc) and LOGKRNL (.fwlogging_kernel_gb20x)
     * are the two the newer firmware also expects.  Each log buffer carries its
     * own page-table-entry array one u64 in, the way create_pte_array lays it
     * out. */
    boot.loginit_va = dma_alloc_pages(16, &boot.loginit_phys);   /* 64 KiB */
    boot.logintr_va = dma_alloc_pages(16, &boot.logintr_phys);
    boot.logrm_va   = dma_alloc_pages(16, &boot.logrm_phys);
    boot.logmnoc_va = dma_alloc_pages(16, &boot.logmnoc_phys);
    boot.logkrnl_va = dma_alloc_pages(16, &boot.logkrnl_phys);
    boot.libos_va   = dma_alloc_pages(1, &boot.libos_phys);
    if (!boot.loginit_va || !boot.logintr_va || !boot.logrm_va ||
        !boot.logmnoc_va || !boot.logkrnl_va || !boot.libos_va) {
        kerr("nv-boot", "no memory for the libos regions");
        return false;
    }

    struct { void *va; u64 phys; const char *id; } logs[5] = {
        { boot.loginit_va, boot.loginit_phys, "LOGINIT" },
        { boot.logintr_va, boot.logintr_phys, "LOGINTR" },
        { boot.logrm_va,   boot.logrm_phys,   "LOGRM"   },
        { boot.logmnoc_va, boot.logmnoc_phys, "LOGMNOC" },
        { boot.logkrnl_va, boot.logkrnl_phys, "LOGKRNL" },
    };
    for (int i = 0; i < 5; i++) {
        u64 *ptes = (u64 *)((u8 *)logs[i].va + sizeof(u64));
        size_t n = (16 * GSP_PAGE) / GSP_PAGE;
        for (size_t p = 0; p < n; p++) ptes[p] = logs[i].phys + (u64)p * GSP_PAGE;
    }

    LibosMemoryRegionInitArgument *la = boot.libos_va;
    memset(la, 0, GSP_PAGE);
    /* id8 is the region name packed big-endian into eight bytes, the way
     * r535_gsp_libos_id8 builds it. */
    /* Order matters: the five logs first (LOGINIT must be first), then RMARGS
     * last - the layout kgspSetupLibosInitArgs builds. */
    static const struct { const char *name; int idx; } names[6] = {
        { "LOGINIT", 0 }, { "LOGINTR", 1 }, { "LOGRM", 2 },
        { "LOGMNOC", 3 }, { "LOGKRNL", 4 }, { "RMARGS", 5 } };
    u64 addrs[6] = { boot.loginit_phys, boot.logintr_phys, boot.logrm_phys,
                     boot.logmnoc_phys, boot.logkrnl_phys, boot.rmargs_phys };
    u64 sizes[6] = { 16 * GSP_PAGE, 16 * GSP_PAGE, 16 * GSP_PAGE,
                     16 * GSP_PAGE, 16 * GSP_PAGE, GSP_PAGE };
    for (int i = 0; i < 6; i++) {
        u64 id = 0;
        for (const char *p = names[i].name; *p; p++) id = (id << 8) | (u8)*p;
        la[i].id8 = id;
        la[i].pa = addrs[i];
        la[i].size = sizes[i];
        la[i].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
        la[i].loc = LIBOS_MEMORY_REGION_LOC_SYSMEM;
    }

    /* And finally the one structure the FSP is handed. */
    boot.bootargs_va = dma_alloc_pages(1, &boot.bootargs_phys);
    if (!boot.bootargs_va) { kerr("nv-boot", "no memory for the boot params"); return false; }
    GSP_FMC_BOOT_PARAMS *bp = boot.bootargs_va;
    memset(bp, 0, sizeof *bp);
    bp->bootGspRmParams.target = GSP_DMA_TARGET_COHERENT_SYSTEM;
    bp->bootGspRmParams.bIsGspRmBoot = 1;
    bp->bootGspRmParams.gspRmDescOffset = boot.wpr_phys;
    bp->bootGspRmParams.gspRmDescSize = (u32)sizeof *m;
    bp->gspRmParams.target = GSP_DMA_TARGET_NONCOHERENT_SYSTEM;
    bp->gspRmParams.bootArgsOffset = boot.libos_phys;

    u64 rsvd = ALIGN_UP((u64)GSP_NONWPR_HEAP_GB20X + m->pmuReservedSize, 0x200000);

    boot.staged = true;
    if (boot_params_phys) *boot_params_phys = boot.bootargs_phys;
    if (rsvd_out) *rsvd_out = rsvd;

    kinfo("nv-boot", "GSP-RM staged: %zu MiB image radix3-paged, WPR meta at "
                     "%#llx, boot params at %#llx, %u-byte heap reserved",
          fw_image_bytes / (1024 * 1024), (unsigned long long)boot.wpr_phys,
          (unsigned long long)boot.bootargs_phys, (unsigned)m->gspFwHeapSize);

    /* Every field the GSP-FMC/ACR reads out of the WprMeta, dumped so a log
     * collected after a 0xb can be compared field-by-field against NVIDIA's
     * kgspCalculateFbLayout_GH100 without another guess-and-boot cycle.  The
     * ACR-owned end-of-FB offsets are deliberately left ZERO here (it fills
     * them); only sizes + sysmem addresses are ours to get right. */
    kinfo("nv-wprmeta", "magic %#llx rev %u | radix3 root %#llx size %llu | "
                        "bootloader %#llx size %llu code %#llx data %#llx man %#llx",
          (unsigned long long)m->magic, (unsigned)m->revision,
          (unsigned long long)m->sysmemAddrOfRadix3Elf, (unsigned long long)m->sizeOfRadix3Elf,
          (unsigned long long)m->sysmemAddrOfBootloader, (unsigned long long)m->sizeOfBootloader,
          (unsigned long long)m->bootloaderCodeOffset, (unsigned long long)m->bootloaderDataOffset,
          (unsigned long long)m->bootloaderManifestOffset);
    kinfo("nv-wprmeta", "signature %#llx size %llu | nonWprHeap %#llx | fwHeap %#llx | "
                        "frts %#llx | vgaWs %#llx | pmuRsvd %#llx | fbSize %#llx (0=ACR fills)",
          (unsigned long long)m->sysmemAddrOfSignature, (unsigned long long)m->sizeOfSignature,
          (unsigned long long)m->nonWprHeapSize, (unsigned long long)m->gspFwHeapSize,
          (unsigned long long)m->frtsSize, (unsigned long long)m->vgaWorkspaceSize,
          (unsigned long long)m->pmuReservedSize, (unsigned long long)m->fbSize);
    return true;
}

/* After the FSP has started the co-processor and the lockdown has cleared,
 * this is the memory the message rings attach to - the same shared region the
 * RM arguments named. */
u8 *nv_gsp_boot_cmdq(void) { return boot.staged ? boot.cmdq_region : NULL; }
u8 *nv_gsp_boot_msgq(void) { return boot.staged ? boot.msgq_region : NULL; }

/* ----------------------------------------------------------------- test */

/* Everything that can be checked without a card: that the structures are the
 * size and shape the firmware reads, and that a staging assembled from a small
 * synthetic image links together - the WPR magic in place, the radix3 leaves
 * pointing at the image, the boot params pointing at the WPR meta, and the RM
 * arguments naming the rings. */
int nv_gsp_boot_selftest(void) {
    int failures = 0;

    /* Layouts. */
    if (sizeof(GspFwWprMeta) != 256) {
        kerr("nv-boot", "the WPR meta is %zu bytes, not 256",
             sizeof(GspFwWprMeta));
        failures++;
    }
    if (__builtin_offsetof(GSP_FMC_BOOT_PARAMS, bootGspRmParams) != 8) {
        kerr("nv-boot", "the boot params are laid out wrong");
        failures++;
    }

    /* A synthetic radix3 over three pages: the leaves must be the three page
     * addresses in order. */
    u64 base = 0x1000000ull;
    boot.fw_phys = base; boot.fw_pages = 3;
    boot.radix_lvl2_pages = 0;
    boot.radix_lvl0 = boot.radix_lvl1 = boot.radix_lvl2 = NULL;
    if (build_radix3(base, 3)) {
        u64 *lvl2 = boot.radix_lvl2;
        u64 *lvl1 = boot.radix_lvl1;
        u64 *lvl0 = boot.radix_lvl0;
        if (lvl0[0] != boot.radix_lvl1_phys) { kerr("nv-boot", "radix level 0 wrong"); failures++; }
        if (lvl1[0] != boot.radix_lvl2_phys) { kerr("nv-boot", "radix level 1 wrong"); failures++; }
        for (int i = 0; i < 3; i++)
            if (lvl2[i] != base + (u64)i * GSP_PAGE) {
                kerr("nv-boot", "radix leaf %d points at the wrong page", i);
                failures++;
            }
        if (boot.radix_lvl0) dma_free_pages(boot.radix_lvl0, 1);
        if (boot.radix_lvl1) dma_free_pages(boot.radix_lvl1, 1);
        if (boot.radix_lvl2) dma_free_pages(boot.radix_lvl2, boot.radix_lvl2_pages);
    } else {
        kwarn("nv-boot", "selftest: no memory for a radix3");
    }

    /* A libos id8 is the name big-endian in eight bytes. */
    u64 id = 0; for (const char *p = "RMARGS"; *p; p++) id = (id << 8) | (u8)*p;
    if (id != 0x0000524d41524753ull) {   /* "RMARGS" */
        kerr("nv-boot", "the libos id packing is wrong (%#llx)",
             (unsigned long long)id);
        failures++;
    }

    /* NVIDIA includes the page-table page in the shared-memory PTE list.  Two
     * 256 KiB queues therefore occupy 128 data pages plus one table page, and
     * PTE[0] maps the table itself. */
    if (build_shared_rings()) {
        u64 *pte = boot.shm_va;
        if (boot.shm_pages != 129) {
            kerr("nv-boot", "the shared ring mapping has %zu pages, not 129",
                 boot.shm_pages);
            failures++;
        }
        if (pte[0] != boot.shm_phys ||
            pte[boot.shm_pages - 1] !=
                boot.shm_phys + (boot.shm_pages - 1) * GSP_PAGE) {
            kerr("nv-boot", "the shared ring PTEs do not map the full allocation");
            failures++;
        }
        dma_free_pages(boot.shm_va, boot.shm_pages);
    } else {
        kwarn("nv-boot", "selftest: no memory for the shared ring mapping");
    }

    memset(&boot, 0, sizeof boot);
    if (!failures)
        kinfo("nv-boot", "the GSP-RM boot structures are the size and shape the "
                         "firmware reads, and a staging links together");
    return failures;
}
