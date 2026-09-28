/* nvidia.c - identifying an NVIDIA graphics card, from the oldest to the newest.
 *
 * Every NVIDIA GPU since the Riva TNT answers the same question the same way.
 * The first register in its memory window, PMC_BOOT_0 at offset zero, holds the
 * architecture and revision of the silicon itself.  That register has not moved
 * or changed meaning in twenty-five years, which makes it the one identification
 * that also works for a card released after this code was written: the chip
 * says what it is rather than being looked up in a table that has to be kept
 * current.
 *
 * The PCI device id is read as well, because it distinguishes cards that share
 * silicon - a 5070 Ti and a 5080 are both GB203 - and because it is available
 * even when the register window cannot be mapped.
 *
 * What this driver does NOT do is drive the card, and the reason differs by
 * generation.
 *
 * From Turing onward the card is not driven by registers at all.  A
 * co-processor called the GSP runs NVIDIA's own firmware, and a driver talks to
 * that rather than to the hardware.  NVIDIA publishes the kernel side of this -
 * their open kernel modules are that interface - and ships the firmware in
 * their driver package, but does not permit anyone else to redistribute it.  So
 * the requirement is named here and checked against what the user has supplied,
 * the same way Linux does it: a machine one file short of having a chance is
 * not the same as one that never could.  What remains unwritten is the boot
 * sequence for that co-processor and the command interface above it.
 *
 * Earlier generations are driven by registers and are documented well enough to
 * write for, but each is a separate large project.
 *
 * Either way the display works, because the firmware left a framebuffer behind
 * and this card is scanning out of its own memory to produce it.
 */
#include "kernel.h"
#include "klog.h"
#include "gpu.h"
#include "firmware.h"
#include "mm.h"

/* The per-chip GSP release, from the same table (nv_blackwell.c) the GSP loader
 * uses - so the "needs firmware" report names the version that would actually
 * drive this chip, not a hardcoded one. */
const char *nv_gsp_release(u32 chipset);

/* PMC_BOOT_0.  Bits 28:20 are the architecture, 19:16 the implementation
 * within it, and 7:0 the revision.  On the very oldest parts the whole upper
 * field reads zero, which is itself the answer. */
#define NV_PMC_BOOT_0   0x000000

/* Fermi and later report how much memory is fitted here, as a mantissa and a
 * power-of-two scale. */
#define NV_PFB_LOCAL_MEMORY_RANGE  0x100CE0

/* Everything from the Riva through GeForce 7 keeps it in the old place. */
#define NV_PFB_CSTATUS  0x10020C

typedef struct {
    u32         family;      /* the architecture nibble, 0x170 for Ampere    */
    const char *name;
} nv_family_t;

/* The architecture field of PMC_BOOT_0, masked to 0x1f0.  These are the values
 * NVIDIA has actually shipped; the codenames are the ones the hardware is known
 * by rather than the marketing series. */
static const nv_family_t families[] = {
    { 0x000, "Fahrenheit (NV0x)"   },   /* Riva TNT, TNT2                    */
    { 0x010, "Celsius (NV1x)"      },   /* GeForce 256, GeForce 2            */
    { 0x020, "Kelvin (NV2x)"       },   /* GeForce 3, GeForce 4 Ti           */
    { 0x030, "Rankine (NV3x)"      },   /* GeForce FX                        */
    { 0x040, "Curie (NV4x)"        },   /* GeForce 6, GeForce 7              */
    { 0x060, "Curie (NV6x)"        },
    { 0x050, "Tesla (G8x)"         },   /* GeForce 8, first unified shaders  */
    { 0x080, "Tesla (G9x)"         },   /* GeForce 9                         */
    { 0x090, "Tesla (GT2xx)"       },   /* GeForce GTX 200                   */
    { 0x0a0, "Tesla (GT21x)"       },   /* GeForce GT 210 to GTS 250         */
    { 0x0c0, "Fermi (GF10x)"       },   /* GeForce GTX 400                   */
    { 0x0d0, "Fermi (GF11x)"       },   /* GeForce GTX 500                   */
    { 0x0e0, "Kepler (GK10x)"      },   /* GeForce GTX 600                   */
    { 0x0f0, "Kepler (GK11x)"      },   /* GeForce GTX 700                   */
    { 0x100, "Kepler (GK20x)"      },
    { 0x110, "Maxwell (GM10x)"     },   /* GeForce GTX 750                   */
    { 0x120, "Maxwell (GM20x)"     },   /* GeForce GTX 900                   */
    { 0x130, "Pascal (GP10x)"      },   /* GeForce GTX 1000                  */
    { 0x140, "Volta (GV10x)"       },   /* Titan V, Quadro GV100             */
    { 0x160, "Turing (TU10x)"      },   /* GeForce RTX 2000, GTX 1600        */
    { 0x170, "Ampere (GA10x)"      },   /* GeForce RTX 3000                  */
    { 0x180, "Hopper (GH10x)"      },   /* datacentre only                   */
    { 0x190, "Ada Lovelace (AD10x)"},   /* GeForce RTX 4000                  */
    { 0x1a0, "Blackwell (GB10x)"   },   /* datacentre B100, B200             */
    { 0x1b0, "Blackwell (GB20x)"   },   /* GeForce RTX 5000                  */
};

/* PCI device ids are handed out in blocks, one or two blocks per architecture.
 * This is the fallback when the register window cannot be mapped, and the
 * cross-check when it can.  A range that has not been allocated yet falls
 * through to the register, which is why an unreleased card still identifies. */
typedef struct {
    u16 first, last;
    u32 family;
    const char *series;
} nv_range_t;

static const nv_range_t ranges[] = {
    { 0x0020, 0x002f, 0x000, "RIVA TNT"              },
    { 0x00a0, 0x00af, 0x000, "Aladdin TNT2"          },
    { 0x0100, 0x0113, 0x010, "GeForce 256"           },
    { 0x0150, 0x0153, 0x010, "GeForce 2"             },
    { 0x0170, 0x018f, 0x010, "GeForce 4 MX"          },
    { 0x01f0, 0x01ff, 0x010, "GeForce 4 MX"          },
    { 0x0200, 0x020f, 0x020, "GeForce 3"             },
    { 0x0250, 0x025f, 0x020, "GeForce 4 Ti"          },
    { 0x0280, 0x028f, 0x020, "GeForce 4 Ti"          },
    { 0x0300, 0x033f, 0x030, "GeForce FX"            },
    { 0x0330, 0x034f, 0x030, "GeForce FX 5900"       },
    { 0x0040, 0x004f, 0x040, "GeForce 6800"          },
    { 0x00c0, 0x00cf, 0x040, "GeForce 6800 GT"       },
    { 0x0140, 0x014f, 0x040, "GeForce 6600"          },
    { 0x0160, 0x016f, 0x040, "GeForce 6200"          },
    { 0x0090, 0x009f, 0x040, "GeForce 7800"          },
    { 0x01d0, 0x01df, 0x040, "GeForce 7300"          },
    { 0x0290, 0x029f, 0x040, "GeForce 7900"          },
    { 0x0390, 0x039f, 0x040, "GeForce 7600"          },
    { 0x0190, 0x019f, 0x050, "GeForce 8800"          },
    { 0x0400, 0x042f, 0x050, "GeForce 8600"          },
    { 0x05e0, 0x05ff, 0x090, "GeForce GTX 200"       },
    { 0x0600, 0x063f, 0x080, "GeForce 9800"          },
    { 0x0640, 0x066f, 0x080, "GeForce 9500"          },
    { 0x06e0, 0x06ff, 0x080, "GeForce 8400"          },
    { 0x0a00, 0x0aff, 0x0a0, "GeForce GT 200"        },
    { 0x0ca0, 0x0cff, 0x0a0, "GeForce GT 300"        },
    { 0x06c0, 0x06df, 0x0c0, "GeForce GTX 400"       },
    { 0x0e20, 0x0e3f, 0x0c0, "GeForce GTX 400"       },
    { 0x0dc0, 0x0dff, 0x0c0, "GeForce GTS 400"       },
    { 0x1080, 0x109f, 0x0d0, "GeForce GTX 500"       },
    { 0x1200, 0x123f, 0x0d0, "GeForce GTX 500"       },
    { 0x1040, 0x107f, 0x0d0, "GeForce GT 600"        },
    { 0x1180, 0x11ff, 0x0e0, "GeForce GTX 600"       },
    { 0x0fc0, 0x0fff, 0x0e0, "GeForce GTX 600"       },
    { 0x1280, 0x12ff, 0x100, "GeForce GT 700"        },
    { 0x1000, 0x103f, 0x0f0, "GeForce GTX 700"       },
    { 0x1380, 0x139f, 0x110, "GeForce GTX 750"       },
    { 0x1340, 0x137f, 0x110, "GeForce 800M"          },
    { 0x13c0, 0x13ff, 0x120, "GeForce GTX 900"       },
    { 0x1400, 0x143f, 0x120, "GeForce GTX 900"       },
    { 0x17c0, 0x17ff, 0x120, "GeForce GTX 980 Ti"    },
    { 0x1b00, 0x1b7f, 0x130, "GeForce GTX 1080"      },
    { 0x1b80, 0x1bff, 0x130, "GeForce GTX 1070"      },
    { 0x1c00, 0x1c7f, 0x130, "GeForce GTX 1060"      },
    { 0x1c80, 0x1cff, 0x130, "GeForce GTX 1050"      },
    { 0x1d00, 0x1d7f, 0x130, "GeForce GT 1030"       },
    { 0x1d80, 0x1dff, 0x140, "Titan V"               },
    { 0x1e00, 0x1e7f, 0x160, "GeForce RTX 2080 Ti"   },
    { 0x1e80, 0x1eff, 0x160, "GeForce RTX 2080"      },
    { 0x1f00, 0x1f7f, 0x160, "GeForce RTX 2070"      },
    { 0x1f80, 0x1fff, 0x160, "GeForce RTX 2060"      },
    { 0x2180, 0x21ff, 0x160, "GeForce GTX 1660"      },
    { 0x2200, 0x227f, 0x170, "GeForce RTX 3090"      },
    { 0x2280, 0x22ff, 0x170, "GeForce RTX 3080"      },
    { 0x2480, 0x24ff, 0x170, "GeForce RTX 3070"      },
    { 0x2500, 0x257f, 0x170, "GeForce RTX 3060"      },
    { 0x2580, 0x25ff, 0x170, "GeForce RTX 3050"      },
    { 0x2600, 0x267f, 0x190, "GeForce RTX 4090"      },
    { 0x2680, 0x26ff, 0x190, "GeForce RTX 4080"      },
    { 0x2700, 0x277f, 0x190, "GeForce RTX 4070"      },
    { 0x2780, 0x27ff, 0x190, "GeForce RTX 4070"      },
    { 0x2800, 0x287f, 0x190, "GeForce RTX 4060"      },
    { 0x2880, 0x28ff, 0x190, "GeForce RTX 4060"      },
    { 0x2b00, 0x2b7f, 0x1b0, "GeForce RTX 5090"      },
    { 0x2b80, 0x2bff, 0x1b0, "GeForce RTX 5080"      },
    { 0x2c00, 0x2c7f, 0x1b0, "GeForce RTX 5070 Ti"   },
    { 0x2c80, 0x2cff, 0x1b0, "GeForce RTX 5070"      },
    { 0x2d00, 0x2d7f, 0x1b0, "GeForce RTX 5060"      },
    { 0x2f00, 0x2fff, 0x1b0, "GeForce RTX 5000"      },
};

static const char *family_name(u32 family) {
    for (size_t i = 0; i < ARRAY_LEN(families); i++)
        if (families[i].family == (family & 0x1f0)) return families[i].name;
    return NULL;
}

const char *nvidia_arch_from_device_id(u16 device, u32 *family_out) {
    for (size_t i = 0; i < ARRAY_LEN(ranges); i++) {
        if (device < ranges[i].first || device > ranges[i].last) continue;
        if (family_out) *family_out = ranges[i].family;
        return ranges[i].series;
    }
    if (family_out) *family_out = 0;
    return NULL;
}

/* ---------------------------------------------------------------- firmware
 *
 * From Turing onward the card is run by a co-processor rather than by
 * registers, and that co-processor runs signed firmware that only NVIDIA can
 * produce.  The files are per-architecture and live in a directory named after
 * the first chip of that architecture - which is not the same as the chip in
 * the machine, and is the thing people get wrong when they go looking. */
static const char *gsp_directory(u32 family) {
    switch (family & 0x1f0) {
    case 0x160: return "tu102";               /* Turing        */
    case 0x170: return "ga102";               /* Ampere        */
    case 0x190: return "ad102";               /* Ada Lovelace  */
    case 0x1a0: return "gb100";               /* Blackwell, datacentre */
    case 0x1b0: return "gb202";               /* Blackwell, GeForce    */
    default:    return "ga102";
    }
}

/* ------------------------------------------------------------------ memory */

/* Read a register, having first checked it is inside the window that was
 * actually mapped.
 *
 * The reason this exists rather than a plain dereference is a page fault on
 * the first machine with a real NVIDIA card this ever ran on.  The caller
 * mapped eight kilobytes of the register window - with a comment saying four
 * was all that was needed - and the memory configuration this file reads sits
 * a megabyte in.  Under a virtual machine there is no NVIDIA card, so the code
 * never ran and the mistake sat there being wrong in a comment.
 *
 * Mapping more would have fixed that particular fault and left the shape of
 * the bug in place: an unchecked dereference of a device window is a fault
 * waiting for whichever register somebody adds next.  A card that is smaller
 * than expected, or a BAR the firmware did not assign, is a thing to report,
 * not a thing to crash on - so a read past the end returns all ones, which is
 * what a real bus returns for an address nobody answers. */
static u32 rd32(volatile u8 *regs, size_t size, u32 offset) {
    if (!regs || (u64)offset + 4 > size) return 0xFFFFFFFFu;
    return *(volatile u32 *)(regs + offset);
}

static u64 read_vram(volatile u8 *regs, size_t size, u32 family, bool *exact) {
    *exact = false;
    if (!regs) return 0;

    if ((family & 0x1f0) >= 0x0c0) {
        /* Fermi and later: a mantissa and a power-of-two scale, in megabytes.
         * A card that has not been initialised reads zero, so the answer is
         * range-checked rather than trusted. */
        u32 v = rd32(regs, size, NV_PFB_LOCAL_MEMORY_RANGE);
        if (v == 0xFFFFFFFFu) return 0;      /* nothing answered there */
        u32 mantissa = v & 0xF;
        u32 scale = (v >> 4) & 0xF;
        if (mantissa) {
            u64 size = (u64)mantissa << (scale + 20);
            if (size >= 16ULL << 20 && size <= 256ULL << 30) {
                *exact = true;
                return size;
            }
        }
        return 0;
    }

    if ((family & 0x1f0) >= 0x010) {
        /* Riva through GeForce 7: the size sits in the top of one register. */
        u32 v = rd32(regs, size, NV_PFB_CSTATUS);
        if (v == 0xFFFFFFFFu) return 0;
        u64 size = v & 0xFFF00000u;
        if (size >= 4ULL << 20 && size <= 4ULL << 30) {
            *exact = true;
            return size;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- identify */

void nvidia_identify(gpu_info_t *g, volatile u8 *regs, size_t regs_size) {
    u32 pci_family = 0;
    const char *series = nvidia_arch_from_device_id(g->pci_device, &pci_family);

    u32 family = pci_family;
    u32 boot0 = 0;

    if (regs) {
        boot0 = rd32(regs, regs_size, NV_PMC_BOOT_0);
        /* All ones means the window is not really there. */
        if (boot0 != 0xFFFFFFFFu && boot0 != 0) {
            if (boot0 & 0x1f000000u) {
                g->chipset = (boot0 & 0x1ff00000u) >> 20;
                g->revision = (u8)(boot0 & 0xFF);
                family = g->chipset;
            } else {
                /* The Riva TNT generation leaves the architecture field clear. */
                g->chipset = 0x04;
                family = 0x000;
            }
        }
    }

    const char *arch = family_name(family);

    /* The chip's own answer wins over the device-id table, which is what lets a
     * card newer than this driver still name its architecture correctly. */
    if (arch && g->chipset)
        snprintf(g->arch, sizeof g->arch, "%s rev %u", arch, g->revision);
    else if (arch)
        strlcpy(g->arch, arch, sizeof g->arch);
    else if (g->chipset)
        snprintf(g->arch, sizeof g->arch, "NV%03x rev %u", g->chipset, g->revision);
    else
        strlcpy(g->arch, "unrecognised NVIDIA architecture", sizeof g->arch);

    if (series)
        snprintf(g->name, sizeof g->name, "NVIDIA %s [%04x]", series, g->pci_device);
    else
        snprintf(g->name, sizeof g->name, "NVIDIA graphics [10de:%04x]", g->pci_device);

    g->vram_bytes = read_vram(regs, regs_size, family, &g->vram_exact);
    if (!g->vram_bytes && g->bar_vram_size) {
        /* Fall back to the memory aperture.  Without resizable BAR that is
         * 256 MiB whatever the card has, so it is reported as an aperture
         * rather than passed off as the memory size. */
        g->vram_bytes = g->bar_vram_size;
        g->vram_exact = false;
    }

    /* What can actually be driven.
     *
     * From Turing onward the card is not driven by registers at all: a
     * co-processor called the GSP runs NVIDIA's own firmware, and the driver
     * talks to that rather than to the hardware.  NVIDIA publishes the kernel
     * side of this - their open kernel modules are the same interface - and
     * ships the firmware in their driver package.  What they do not do is let
     * anyone else redistribute the firmware, which is why it has to come from
     * the user, exactly as it does on Linux.
     *
     * So the requirement is named and checked rather than left as "not
     * supported": a machine one file away from having a chance is a different
     * situation from one that never could. */
    if ((family & 0x1f0) >= 0x160) {
        /* Which files, exactly.  A GSP card needs three of them and they are
         * per-architecture: the microcode itself, and two small images that
         * load and unload it into the protected region it runs from.  Naming
         * them precisely is the difference between a user who can go and get
         * them and one who cannot. */
        const char *directory = gsp_directory(family);
        /* The version is per-chip - NOT a hardcoded 535.113.01, which named the
         * Turing/Ampere/Ada release for every card and told a Blackwell owner to
         * fetch a file that would never drive their GPU (and flagged "needs
         * firmware" even when the RIGHT version was present).  nv_gsp_release()
         * is the same table the actual GSP loader uses. */
        const char *rel = nv_gsp_release(g->chipset);
        char name[80];

        snprintf(name, sizeof name, "nvidia/%s/gsp/gsp-%s.bin", directory, rel);
        firmware_declare(name, "linux-firmware, or NVIDIA's driver package");
        bool have_gsp = firmware_present(name, NULL);

        if (have_gsp)
            kinfo("nvidia", "the GSP firmware for %s (%s) is present", directory, rel);

        snprintf(name, sizeof name, "nvidia/%s/gsp/gsp-%s.bin", directory, rel);
        if (have_gsp) {
            snprintf(g->note, sizeof g->note,
                     "the GSP firmware for %s is present; the sequence that "
                     "boots it is not written", directory);
        } else {
            snprintf(g->note, sizeof g->note,
                     "driven through the GSP, which needs nvidia/%s/gsp/",
                     directory);
        }
        strlcpy(g->firmware_name, name, sizeof g->firmware_name);
        g->firmware_needed = true;
        g->firmware_present = have_gsp;
    } else if (family) {
        strlcpy(g->note,
                "display via firmware framebuffer; this generation's 2D engine "
                "is documented but not implemented here",
                sizeof g->note);
    } else {
        strlcpy(g->note, "display via firmware framebuffer", sizeof g->note);
    }
}

/* ------------------------------------------------------------------- tests
 *
 * This file identifies NVIDIA cards, and until recently none of it had ever
 * run: the machines it is developed on have no NVIDIA card in them, so every
 * line here was dead code that looked alive.  The first time it executed, on a
 * real card, it read a register a megabyte into a window somebody had mapped
 * eight kilobytes of, and the kernel stopped with a page fault.
 *
 * Mapping more would fix that one register.  What is checked below is the
 * shape of the mistake instead: that a read outside the window that was mapped
 * is answered rather than taken, whatever the window's size happens to be and
 * whichever register somebody adds next.
 *
 * There is no NVIDIA card here either, so the window is a piece of ordinary
 * memory.  That is enough - the question is what this code does with a size it
 * has been given, and a real card would not make that question different.
 */
int nvidia_identify_selftest(void) {
    int failures = 0;

    /* Big enough to hold the chip identity and nothing else, which is the
     * window the caller used to map. */
    enum { SHORT_WINDOW = 0x2000 };
    u8 *window = kzalloc(SHORT_WINDOW);
    if (!window) {
        kerr("nv-id", "no memory for the test");
        return 1;
    }

    /* A GB203, in the layout PMC_BOOT_0 uses. */
    *(volatile u32 *)(window + NV_PMC_BOOT_0) = (0x1b3u << 20) | 0xA1;

    gpu_info_t g;
    memset(&g, 0, sizeof g);
    g.pci_vendor = 0x10DE;
    g.pci_device = 0x2C05;

    /* The call that used to fault.  It asks for the memory configuration at
     * 0x100CE0, which is far outside what was mapped. */
    nvidia_identify(&g, window, SHORT_WINDOW);

    if (g.chipset != 0x1b3) {
        kerr("nv-id", "the chip came out as %03x through a short window",
             g.chipset);
        failures++;
    } else if (g.vram_exact) {
        kerr("nv-id", "memory was reported as exact from a register that was "
                      "never mapped");
        failures++;
    } else if (g.vram_bytes) {
        kerr("nv-id", "memory came out as %llu bytes from outside the window",
             (unsigned long long)g.vram_bytes);
        failures++;
    } else {
        kinfo("nv-id", "a register outside the mapped window is answered, not "
                       "taken: the chip is still identified as %03x and the "
                       "memory size is reported as unknown", g.chipset);
    }

    /* And that a window long enough to reach it gives the real answer, so the
     * check above is a bounds test rather than a way of never reading. */
    enum { FULL_WINDOW = 0x102000 };
    u8 *big = kzalloc(FULL_WINDOW);
    if (big) {
        *(volatile u32 *)(big + NV_PMC_BOOT_0) = (0x1b3u << 20) | 0xA1;
        /* Mantissa 4, scale 2: four megabytes shifted up by two, which is
         * sixteen gigabytes - what a 5070 Ti has. */
        *(volatile u32 *)(big + NV_PFB_LOCAL_MEMORY_RANGE) = (12u << 4) | 4u;

        gpu_info_t h;
        memset(&h, 0, sizeof h);
        h.pci_vendor = 0x10DE;
        h.pci_device = 0x2C05;
        nvidia_identify(&h, big, FULL_WINDOW);

        if (!h.vram_exact || h.vram_bytes != (16ULL << 30)) {
            kerr("nv-id", "through a full window memory came out as %llu MiB "
                          "(exact %d), expected 16384",
                 (unsigned long long)(h.vram_bytes >> 20), (int)h.vram_exact);
            failures++;
        } else {
            kinfo("nv-id", "and through a window that does reach it, the same "
                           "register reads 16 GiB");
        }
        kfree(big);
    }

    kfree(window);
    return failures;
}
