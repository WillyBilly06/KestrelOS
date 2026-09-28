/* bootinfo.h - the hand-off contract between the KestrelOS UEFI loader and the
 * kernel.  Both sides compile this header, so any change must be made in one
 * place and the version bumped.
 *
 * The loader leaves the machine in this state before jumping to the kernel:
 *
 *   - long mode, paging on, CR3 = a page table built by the loader containing
 *       * an identity map of the low 4 GiB (2 MiB pages)
 *       * a direct map of every usable physical region at KB_HHDM_BASE
 *       * the kernel image at its link address (KB_KERNEL_BASE)
 *   - interrupts disabled, boot services exited
 *   - RDI = physical/HHDM address of a kboot_info (SysV ABI first argument)
 *   - RSP = top of a 64 KiB loader-allocated stack, 16-byte aligned
 *   - the framebuffer is linear and already in the mode described below
 */
#ifndef KESTREL_BOOTINFO_H
#define KESTREL_BOOTINFO_H

#include <stdint.h>

#define KB_MAGIC        0x314C45525453454BULL /* "KESTREL1", little-endian */
#define KB_VERSION      2

/* Fixed virtual layout.  The kernel is linked at KB_KERNEL_BASE and assumes the
 * loader has direct-mapped physical memory at KB_HHDM_BASE. */
#define KB_HHDM_BASE    0xFFFF800000000000ULL
#define KB_KERNEL_BASE  0xFFFFFFFF80000000ULL

/* Memory map entry types. */
enum {
    KB_MEM_USABLE        = 1,  /* free for the kernel's physical allocator   */
    KB_MEM_RESERVED      = 2,  /* firmware/hardware, never touch             */
    KB_MEM_ACPI_RECLAIM  = 3,  /* ACPI tables; usable once parsed            */
    KB_MEM_ACPI_NVS      = 4,  /* must be preserved across sleep             */
    KB_MEM_LOADER        = 5,  /* loader structures; usable after boot       */
    KB_MEM_KERNEL        = 6,  /* the kernel image and its initrd            */
    KB_MEM_FRAMEBUFFER   = 7,  /* linear framebuffer                         */
    KB_MEM_BAD           = 8,  /* firmware reported the RAM as defective     */
};

typedef struct {
    uint64_t base;    /* physical base address, page aligned */
    uint64_t pages;   /* length in 4 KiB pages               */
    uint32_t type;    /* KB_MEM_*                            */
    uint32_t pad;
} kboot_mmap_entry;

/* Pixel layout of the linear framebuffer.  The loader only ever selects a mode
 * whose pixels are 32 bits so the kernel can treat the surface as uint32_t[]. */
typedef struct {
    uint64_t base;          /* physical address of the framebuffer      */
    uint64_t size;          /* bytes                                    */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;         /* bytes per scanline                       */
    uint32_t bpp;           /* always 32                                */
    uint8_t  red_shift,  red_bits;
    uint8_t  green_shift, green_bits;
    uint8_t  blue_shift, blue_bits;
    uint16_t pad;
} kboot_framebuffer;

/* One mode the firmware offers.  The list is passed through so the system can
 * show what the display can do and let the user pick, rather than being stuck
 * with whatever the loader chose. */
typedef struct {
    uint16_t width, height;
    uint16_t pitch_pixels;
    uint8_t  format;        /* 0 BGRX, 1 RGBX, 2 bit mask */
    uint8_t  flags;         /* KB_MODE_*                  */
} kboot_video_mode;

#define KB_MODE_CURRENT  0x01   /* the mode in use right now          */
#define KB_MODE_NATIVE   0x02   /* matches the panel's native timing  */

/* Multi-display arrangements for kboot_display.display_mode (#7). */
#define KB_DISPLAY_EXTEND     0   /* one desktop across both (default)  */
#define KB_DISPLAY_MIRROR     1   /* both show the same picture         */
#define KB_DISPLAY_ONLY_OTHER 2   /* main dark; only the second shows   */
#define KB_DISPLAY_ONLY_BASE  16  /* + zero-based connected-output index */

#define KB_MAX_VIDEO_MODES 64

/* What the monitor says about itself, read from its EDID.  All zero when no
 * EDID was available - which is normal on a virtual machine. */
typedef struct {
    uint16_t native_width, native_height;
    uint32_t refresh_mhz;       /* millihertz, so 59.94 Hz is 59940; u32 because
                                 * 120/144/240 Hz overflow 16 bits (65.5 Hz max) */
    uint32_t max_refresh_mhz;   /* the highest the panel reports    */
    uint16_t phys_width_mm, phys_height_mm;
    uint8_t  present;
    /* The multi-display arrangement chosen for two-or-more displays (#7),
     * from BOOT.CFG's displaymode=: 0 extend (the default), 1 mirror,
     * 2 only-the-other.  Ignored when there is one display. */
    uint8_t  display_mode;
    char     manufacturer[4];   /* the three-letter PNP id          */
    char     model[16];

    /* The raw EDID (up to the base block plus one CTA extension), so the kernel
     * can extract what the loader's quick parse leaves out: the refresh RANGE
     * (0xFD descriptor) and - in the extension block - the high-refresh modes
     * (120/144/240 Hz) that a panel keeps out of its base timings.  edid_len is
     * how many bytes are real; 0 when no EDID was read. */
    uint16_t edid_len;
    uint8_t  edid[256];
} kboot_display;

#define KB_CMDLINE_MAX  256
#define KB_BOOTDEV_MAX  80

typedef struct {
    uint64_t magic;             /* KB_MAGIC                                  */
    uint32_t version;           /* KB_VERSION                                */
    uint32_t size;              /* sizeof(kboot_info)                        */

    kboot_framebuffer fb;

    uint64_t mmap;              /* physical address of kboot_mmap_entry[]    */
    uint32_t mmap_count;
    uint32_t pad0;

    uint64_t initrd_base;       /* physical, page aligned; 0 if none         */
    uint64_t initrd_size;

    uint64_t kernel_phys;       /* where the loader placed the kernel image  */
    uint64_t kernel_virt;       /* == KB_KERNEL_BASE                         */
    uint64_t kernel_size;

    /* Everything the firmware offers, and what the panel says it wants. */
    kboot_display    display;
    uint32_t         mode_count;
    uint32_t         mode_current;      /* index into modes[]                */
    kboot_video_mode modes[KB_MAX_VIDEO_MODES];

    uint64_t hhdm_base;         /* == KB_HHDM_BASE                           */
    uint64_t pml4_phys;         /* the page table the loader installed       */

    uint64_t rsdp;              /* ACPI RSDP, physical; 0 if not found       */
    uint64_t efi_system_table;  /* physical; 0 if unavailable                */
    uint64_t efi_runtime;       /* EFI_RUNTIME_SERVICES, physical; 0 if none */

    uint64_t loader_stack_top;  /* so the kernel can reclaim it later        */

    /* Where the loader itself was booted from, so the kernel can find the
     * partition holding the rest of the system without guessing. */
    uint8_t  boot_disk_guid[16];    /* GPT disk GUID of the boot device      */
    uint8_t  boot_part_guid[16];    /* GPT unique partition GUID of the ESP  */
    uint32_t boot_part_index;       /* 1-based; 0 when unknown               */
    uint32_t pad1;

    char cmdline[KB_CMDLINE_MAX];   /* from \KESTREL\BOOT.CFG                */
    char bootdev[KB_BOOTDEV_MAX];   /* "PARTUUID=..." of the data partition  */

    /* A region of memory the loader set aside for the kernel's log.
     *
     * The kernel cannot write files: on a machine that boots from a USB stick
     * its only route to that stick is its own storage driver, and when that
     * driver is the thing being debugged the log explaining why it failed has
     * nowhere to go.  So the kernel writes its log here instead and the loader
     * writes it out on the next boot, while the firmware's own storage stack
     * is still running.  See <kestrel/ramlog.h>.
     *
     * At the end of the structure on purpose: a loader older than this field
     * simply does not write it, and `size` says whether it is there. */
    uint64_t ramlog_base;           /* physical; 0 when none was reserved    */
    uint32_t ramlog_size;           /* bytes, header included                */
    uint32_t ramlog_prev;           /* bytes recovered from the last boot    */
} kboot_info;

#endif /* KESTREL_BOOTINFO_H */
