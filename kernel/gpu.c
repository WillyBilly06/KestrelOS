/* gpu.c - find the graphics hardware and work out what can be done with it.
 *
 * The display works without any of this: UEFI leaves a linear framebuffer and
 * the card is already scanning out of it.  What this adds is knowing what the
 * hardware is, how much memory it has, and which of the cards in the machine
 * the firmware chose - so the System app can say something true rather than
 * "graphics device", and so a note about what is and is not driven appears
 * where the user will actually read it.
 */
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "klog.h"
#include "gpu.h"
#include "svga.h"
#include "nv.h"
#include "intel_display.h"
#include "edid.h"
#include "vfs.h"
#include "time.h"
#include "proc.h"
#include "firmware.h"
#include "nvkms_kapi_client.h"
#include "nvrm_dp_link.h"
#include "../include/kestrel/display_layout.h"

#define MAX_GPUS 4

static gpu_info_t gpus[MAX_GPUS];
static int        gpu_n;
static bool       gpu_test_gui_ready;

/* ----------------------------------------------------------------- vendors */

static gpu_vendor_t vendor_of(u16 id) {
    switch (id) {
    case 0x10DE: return GPU_NVIDIA;
    case 0x1002:
    case 0x1022: return GPU_AMD;
    case 0x8086: return GPU_INTEL;
    case 0x15AD: return GPU_VMWARE;
    case 0x1234:
    case 0x1AF4: return GPU_QEMU;
    case 0x102B: return GPU_MATROX;
    case 0x1A03: return GPU_ASPEED;
    default:     return GPU_UNKNOWN;
    }
}

/* Intel's integrated graphics have used the same device-id blocks per
 * generation since Gen4; naming the generation is more useful than naming a
 * hundred individual parts. */
static const char *intel_generation(u16 device) {
    /* A few exact parts worth naming, verified against the Linux kernel's
     * drm/intel/pciids.h: the Xe-LPG block splits into Meteor Lake and the
     * three Arrow Lake dies, and the machine this was written for has the
     * desktop one (0x7D67, ARL-S) beside its RTX 5070 Ti. */
    switch (device) {
    case 0x7D67: case 0x7D65:            return "Xe-LPG (Arrow Lake-S, desktop)";
    case 0x7D51: case 0x7DD1:            return "Xe-LPG (Arrow Lake-H)";
    case 0x7D41:                         return "Xe-LPG (Arrow Lake-U)";
    case 0x7D40: case 0x7D45: case 0x7D55: case 0x7DD5:
                                         return "Xe-LPG (Meteor Lake)";
    default: break;
    }

    u16 d = device & 0xFF00;
    switch (d) {
    case 0x2900: case 0x2A00: return "Gen4 (GMA X3100/4500)";
    case 0x0100: return "Gen6 (Sandy Bridge HD Graphics)";
    case 0x0400: case 0x0A00: case 0x0D00: return "Gen7 (Ivy Bridge / Haswell)";
    case 0x1600: return "Gen8 (Broadwell)";
    case 0x1900: return "Gen9 (Skylake)";
    case 0x3E00: case 0x5900: case 0x8A00: return "Gen9.5 (Coffee Lake / Ice Lake)";
    case 0x9A00: return "Gen12 (Tiger Lake Xe)";
    case 0x4600: case 0x4C00: return "Gen12 (Alder Lake Xe)";
    case 0xA700: return "Gen12.7 (Raptor Lake Xe)";
    case 0x5600: case 0x4F00: return "Xe-HPG (Arc Alchemist)";
    case 0x7D00: return "Xe-LPG (Meteor Lake / Arrow Lake)";
    case 0xE200: return "Xe2 (Battlemage / Lunar Lake)";
    default:     return NULL;
    }
}

/* AMD's blocks are less regular, but the big families are recognisable. */
static const char *amd_generation(u16 device) {
    if (device >= 0x7550 && device <= 0x75FF) return "RDNA 4 (Navi 48)";
    if (device >= 0x7440 && device <= 0x74FF) return "RDNA 3 (Navi 31/32)";
    if (device >= 0x7300 && device <= 0x73FF) return "RDNA 2 (Navi 21/22/23)";
    if (device >= 0x7310 && device <= 0x731F) return "RDNA 2 (Navi 21)";
    if (device >= 0x7100 && device <= 0x71FF) return "RDNA (Navi 10)";
    if (device >= 0x67C0 && device <= 0x67FF) return "GCN 4 (Polaris)";
    if (device >= 0x6860 && device <= 0x687F) return "GCN 5 (Vega)";
    if (device >= 0x6900 && device <= 0x69FF) return "GCN 3 (Tonga/Fiji)";
    if (device >= 0x6600 && device <= 0x66FF) return "GCN (Oland/Vega 20)";
    if (device >= 0x1500 && device <= 0x15FF) return "integrated (Raven/Renoir/Phoenix)";
    if (device >= 0x9800 && device <= 0x98FF) return "integrated (Kabini/Mullins)";
    return NULL;
}

/* ------------------------------------------------------------------- probe */

static void describe_generic(gpu_info_t *g, pci_dev_t *d) {
    const char *gen = NULL;
    const char *vendor = "Graphics";

    switch (g->vendor) {
    case GPU_INTEL:
        vendor = "Intel";
        gen = intel_generation(g->pci_device);
        strlcpy(g->note, "display via firmware framebuffer; the render engine "
                         "needs a driver per generation", sizeof g->note);
        break;
    case GPU_AMD:
        vendor = "AMD";
        gen = amd_generation(g->pci_device);
        strlcpy(g->note, "display via firmware framebuffer; the command "
                         "processor needs signed microcode", sizeof g->note);
        break;
    case GPU_VMWARE:
        vendor = "VMware";
        gen = "SVGA II";
        if (svga_present()) {
            /* Not the firmware's framebuffer any more: the adapter is being
             * driven, which is what makes a frame cost what it does. */
            u32 vram = 0;
            svga_limits(NULL, NULL, &vram);
            g->vram_bytes = vram;
            strlcpy(g->note,
                    "driven: changed regions reported, the adapter draws the "
                    "pointer, modes can be set while running",
                    sizeof g->note);
        } else {
            strlcpy(g->note, "display via firmware framebuffer", sizeof g->note);
        }
        break;
    case GPU_QEMU:
        vendor = "QEMU";
        gen = "virtual display";
        strlcpy(g->note, "display via firmware framebuffer", sizeof g->note);
        break;
    case GPU_MATROX: vendor = "Matrox"; break;
    case GPU_ASPEED: vendor = "ASPEED"; break;
    default:
        strlcpy(g->note, "display via firmware framebuffer", sizeof g->note);
        break;
    }

    if (gen) {
        snprintf(g->name, sizeof g->name, "%s %s", vendor, gen);
        strlcpy(g->arch, gen, sizeof g->arch);
    } else {
        snprintf(g->name, sizeof g->name, "%s graphics [%04x:%04x]",
                 vendor, g->pci_vendor, g->pci_device);
        strlcpy(g->arch, "unrecognised", sizeof g->arch);
    }

    /* Where nothing better is known, the memory aperture is at least a bound
     * on how much video memory there is. */
    if (!g->vram_bytes && g->bar_vram_size) {
        g->vram_bytes = g->bar_vram_size;
        g->vram_exact = false;
    }
    (void)d;
}

/* The firmware's framebuffer lives in one of the card's memory apertures.
 * Whichever BAR contains it is the card the display is on. */
static bool owns_framebuffer(const pci_dev_t *d, u64 fb) {
    if (!fb) return false;
    for (int i = 0; i < 6; i++) {
        if (!d->bar[i] || d->bar_is_io[i]) continue;
        u64 size = d->bar_size[i] ? d->bar_size[i] : 0x1000;
        if (fb >= d->bar[i] && fb < d->bar[i] + size) return true;
    }
    return false;
}

static size_t nvidia_probe_window_size(u64 aperture_bytes) {
    /* Identification must not enlarge a BAR to reach a desired register.
     * Unknown aperture sizes are not permission to map an assumed 16 MiB.
     * nvidia_identify can still report PCI identity without MMIO. */
    if (aperture_bytes > 0x1000000) return 0x1000000;
    return (size_t)aperture_bytes;
}

static void probe(pci_dev_t *d) {
    if (gpu_n >= MAX_GPUS) return;

    gpu_info_t *g = &gpus[gpu_n];
    memset(g, 0, sizeof *g);
    g->pci_vendor = d->vendor;
    g->pci_device = d->device;
    g->subsys_vendor = d->subsys_vendor;
    g->subsys_device = d->subsys_device;
    g->bus = d->bus; g->slot = d->slot; g->func = d->func;
    g->vendor = vendor_of(d->vendor);
    g->revision = d->revision;

    /* Pick out the register window and the largest memory aperture.  On every
     * modern card BAR 0 is registers and BAR 1 is the memory window; on older
     * ones the order varies, so the sizes decide. */
    int mmio_bar = -1;
    for (int i = 0; i < 6; i++) {
        if (!d->bar[i] || d->bar_is_io[i]) continue;
        u64 size = d->bar_size[i];
        if (!g->bar_mmio) { g->bar_mmio = d->bar[i]; mmio_bar = i; continue; }
        if (size > g->bar_vram_size) { g->bar_vram = d->bar[i]; g->bar_vram_size = size; }
    }

    g->is_boot_display = owns_framebuffer(d, g_boot.fb.base);

    if (g->vendor == GPU_NVIDIA) {
        /* As much of the register window as the card says it has, up to the
         * sixteen megabytes these cards actually publish.
         *
         * This used to map eight kilobytes, with a comment saying four was all
         * that was needed.  It was not: identifying the card reads the chip
         * number at offset zero and then the memory configuration at 0x100CE0,
         * which is a megabyte in.  On the first machine with a real NVIDIA
         * card this ran on, that second read was a page fault into unmapped
         * space and the kernel stopped there.
         *
         * The size is passed on rather than assumed, so the reads on the other
         * side are checked against what was really mapped instead of against
         * what somebody believed. */
        /* The size of the region bar_mmio actually came from, which is the
         * first non-IO BAR and only usually BAR 0. */
        u64 have = (mmio_bar >= 0) ? d->bar_size[mmio_bar] : 0;
        size_t want = nvidia_probe_window_size(have);

        volatile u8 *regs = (g->bar_mmio && want) ?
            vmm_map_mmio(g->bar_mmio, want) : NULL;
        nvidia_identify(g, regs, regs ? want : 0);
    } else if (g->vendor == GPU_AMD && d->vendor == 0x1002) {
        /* The AMD driver finds its own register window: on these cards it is
         * not BAR 0, and the loop above picked BAR 0. */
        amd_identify_gpu(g, d);
    } else {
        describe_generic(g, d);
    }

    /* Claimed only when this system actually puts pixels through it.  A card
     * that is merely identified - read for its name, memory size and engine
     * list - is not being driven, and reporting it as driven would hide the
     * single most important gap this system has. */
    if (g->is_boot_display) pci_claim(d, "gpu-fb");

    g->accel = g->is_boot_display ? GPU_ACCEL_FRAMEBUFFER : GPU_ACCEL_NONE;
    gpu_n++;
}

/* ------------------------------------------------------------------ public */

void gpu_init(void) {
    /* Class 3 is display.  Subclass 0 is VGA-compatible, 2 is 3D, 128 is
     * "other display controller", which is what a card behind a firmware that
     * disabled VGA legacy decoding reports. */
    for (u8 sub = 0; sub <= 0x80; sub++) {
        pci_dev_t *d = NULL;
        while ((d = pci_find(0x03, sub, 0xFF, d)) != NULL) probe(d);
        if (sub == 0x02) sub = 0x7F;          /* skip to 0x80 */
    }

    if (!gpu_n) {
        kinfo("gpu", "no display controller on the PCI bus");
        return;
    }

    for (int i = 0; i < gpu_n; i++) {
        gpu_info_t *g = &gpus[i];
        char mem[32];
        if (g->vram_bytes)
            snprintf(mem, sizeof mem, "%s%lu MiB",
                     g->vram_exact ? "" : "<=", g->vram_bytes >> 20);
        else
            strlcpy(mem, "unknown memory", sizeof mem);

        kinfo("gpu", "%s: %s, %s%s", g->name, g->arch, mem,
              g->is_boot_display ? ", driving the display" : "");
        if (g->note[0]) kinfo("gpu", "  %s", g->note);
    }
}

/* What the display is, and what it could be.  Worth saying out loud at boot:
 * on a machine whose panel wants a mode the firmware did not offer, this line
 * is the only place the difference shows up. */
void display_report(void) {
    const kboot_display *d = &g_boot.display;

    if (d->present) {
        char size[32] = "";
        if (d->phys_width_mm && d->phys_height_mm)
            snprintf(size, sizeof size, ", %u x %u mm",
                     d->phys_width_mm, d->phys_height_mm);

        kinfo("display", "%s %s: native %ux%u at %u.%02u Hz%s",
              d->manufacturer[0] ? d->manufacturer : "monitor",
              d->model[0] ? d->model : "",
              d->native_width, d->native_height,
              d->refresh_mhz / 1000, (d->refresh_mhz % 1000) / 10, size);
    }

    if (!g_boot.mode_count) {
        kinfo("display", "running at %ux%u; the firmware offered no mode list",
              g_boot.fb.width, g_boot.fb.height);
        return;
    }

    u32 max_w = 0, max_h = 0;
    for (u32 i = 0; i < g_boot.mode_count; i++) {
        if ((u32)g_boot.modes[i].width * g_boot.modes[i].height > max_w * max_h) {
            max_w = g_boot.modes[i].width;
            max_h = g_boot.modes[i].height;
        }
    }

    kinfo("display", "running at %ux%u; %u mode(s) available, largest %ux%u",
          g_boot.fb.width, g_boot.fb.height, g_boot.mode_count, max_w, max_h);

    /* Work out how the outputs share the framebuffer.  How many outputs there
     * are is what the adapter says (svga_num_displays): the firmware framebuffer
     * is one scan-out, but the VMware adapter exposes as many as the VM was told
     * to give (svga.numDisplays) and real hardware as many as the card has.  The
     * same call spans, mirrors or hands over to the second output; the geometry
     * is unit-tested in display_layout_host_test.c. */
    int n_out = (int)svga_num_displays();
    if (n_out < 1) n_out = 1;
    if (n_out > DISPLAY_MAX_OUTPUTS) n_out = DISPLAY_MAX_OUTPUTS;
    dl_output_size_t outs[DISPLAY_MAX_OUTPUTS];
    for (int i = 0; i < n_out; i++) {
        outs[i].width = (int)g_boot.fb.width;
        outs[i].height = (int)g_boot.fb.height;
    }
    dl_layout_t layout;
    dl_mode_t mode = (n_out > 1) ? DL_EXTEND : DL_ONLY_PRIMARY;
    display_layout_compute(outs, n_out, mode, &layout);
    if (n_out > 1)
        kinfo("display", "%d displays available; the card drives the second, and "
                         "extend / mirror / only-the-other all work (extend spans "
                         "%dx%d). The arrangement is BOOT.CFG's displaymode=.",
              n_out, layout.fb_width, layout.fb_height);
    else
        kinfo("display", "%d output(s), arrangement: %s (%dx%d); a second output "
                         "enables extend / mirror / only-the-other",
              layout.n, display_layout_mode_name(DL_ONLY_PRIMARY),
              layout.fb_width, layout.fb_height);

    /* Re-parse the boot display's raw EDID for what the loader's quick pass
     * skips - above all the refresh RANGE, which is the difference between
     * "this panel runs at 60" (its base timing) and "this panel accepts up to
     * 240" (what a refresh-rate choice needs to be real). */
    if (g_boot.display.present) {
        edid_info_t ed;
        u32 elen = g_boot.display.edid_len ? g_boot.display.edid_len
                                           : (u32)sizeof g_boot.display.edid;
        if (edid_parse(g_boot.display.edid, elen, &ed)) {
            kinfo("display", "monitor %s %s: native %ux%u, up to %u Hz there; "
                            "%d mode(s) offered, accepts %u-%u Hz",
                  ed.manufacturer, ed.model,
                  ed.native.hactive, ed.native.vactive,
                  (ed.max_refresh_mhz_at_native + 500) / 1000,
                  ed.mode_count, ed.refresh_min_hz, ed.refresh_max_hz);

            /* The loader's quick pass only sees the base timings, so its
             * max_refresh is the native ~60 Hz.  The full parse finds the CTA
             * high-refresh modes and the 0xFD range descriptor - the real
             * ceiling a refresh-rate choice (#5) needs.  Lift the reported max
             * to whichever source says highest, so userland offers 120/144/240
             * where the panel truly accepts them rather than only 60. */
            u32 ceiling = g_boot.display.max_refresh_mhz;
            if (ed.max_refresh_mhz_at_native > ceiling)
                ceiling = ed.max_refresh_mhz_at_native;
            if ((u32)ed.refresh_max_hz * 1000u > ceiling)
                ceiling = (u32)ed.refresh_max_hz * 1000u;
            g_boot.display.max_refresh_mhz = ceiling;
            if (!g_boot.display.refresh_mhz && ed.native.refresh_mhz)
                g_boot.display.refresh_mhz = ed.native.refresh_mhz;
        }
    }

    /* If there is an Intel iGPU, read what its display engine is doing.  This
     * is where the firmware framebuffer above is a single scan-out and the
     * iGPU is the part that could actually drive several outputs and change
     * their refresh - read-only for now, and silent when there is no iGPU. */
    intel_display_probe();
}

int gpu_count(void) { return gpu_n; }

bool gpu_get(int index, gpu_info_t *out) {
    if (index < 0 || index >= gpu_n || !out) return false;
    *out = gpus[index];
    if (out->vendor == GPU_NVIDIA) {
        nv_card_t *c = nv_card_for_pci(out->bus, out->slot, out->func);
        if (c && c->vram_exact && c->vram_bytes) {
            out->vram_bytes = c->vram_bytes;
            out->vram_exact = true;
        }
    }
    return true;
}

/* ---------------------------------------------------------- acceleration
 *
 * Which adapter does the work when the shell asks for a rectangle filled or
 * moved.
 *
 * This used to be settled at the call site: the system call asked the VMware
 * adapter directly, by name.  On a machine with a VMware adapter that is
 * correct and on any other machine it is a dead end - the question is put to a
 * driver that is not there, the answer is no, and every pixel goes through the
 * processor no matter what card is fitted or how much of it is driven.  That
 * is not a missing feature in the other drivers; it is the question being
 * asked of the wrong one.
 *
 * Registration is per PCI device; only an explicit framebuffer handoff selects
 * a screen target. A different adapter becoming ready must not redirect drawing
 * to its memory. Each dispatch captures one immutable record and its context.
 */
typedef struct {
    u8 bus, slot, func;
    const gpu_accel_ops_t *ops;
    void *context;
    const char *owner;
} gpu_accel_binding_t;

static gpu_accel_binding_t accel_bindings[MAX_GPUS];
static unsigned accel_binding_count;
static gpu_accel_binding_t *accel_selected;

bool gpu_accel_register(u8 bus, u8 slot, u8 func, const gpu_accel_ops_t *ops,
                        void *context, const char *owner) {
    if (!ops || slot >= 32 || func >= 8) return false;
    for (unsigned i = 0; i < accel_binding_count; i++) {
        const gpu_accel_binding_t *b = &accel_bindings[i];
        if (b->bus != bus || b->slot != slot || b->func != func) continue;
        /* An identical repeated registration is harmless. Replacing live
         * context behind a caller is not; teardown needs its own lifecycle. */
        return b->ops == ops && b->context == context;
    }
    if (accel_binding_count == ARRAY_LEN(accel_bindings)) {
        kwarn("gpu", "no accelerator registry slot for %02x:%02x.%u", bus, slot, func);
        return false;
    }
    gpu_accel_binding_t *b = &accel_bindings[accel_binding_count++];
    *b = (gpu_accel_binding_t){bus, slot, func, ops, context,
                              owner ? owner : "a driver"};
    kinfo("gpu", "%s registered for %02x:%02x.%u; screen ownership unchanged",
          b->owner, bus, slot, func);
    return true;
}

void gpu_accel_clear(void) {
    __atomic_store_n(&accel_selected, NULL, __ATOMIC_RELEASE);
}

bool gpu_accel_select(u8 bus, u8 slot, u8 func) {
    for (unsigned i = 0; i < accel_binding_count; i++) {
        gpu_accel_binding_t *b = &accel_bindings[i];
        if (b->bus != bus || b->slot != slot || b->func != func) continue;
        __atomic_store_n(&accel_selected, b, __ATOMIC_RELEASE);
        kinfo("gpu", "screen accelerator target %02x:%02x.%u (%s)",
              bus, slot, func, b->owner);
        return true;
    }
    gpu_accel_clear();
    return false;
}

u32 gpu_accel_capabilities(void) {
    u32 can = 0;
    gpu_accel_binding_t *b = __atomic_load_n(&accel_selected, __ATOMIC_ACQUIRE);
    if (!b) return 0;
    const gpu_accel_ops_t *ops = b->ops;
    if (ops->fill && ops->can_fill && ops->can_fill(b->context)) can |= GPU_ACCEL_CAN_FILL;
    if (ops->copy && ops->can_copy && ops->can_copy(b->context)) can |= GPU_ACCEL_CAN_COPY;
    if (ops->cursor_move && ops->can_cursor && ops->can_cursor(b->context)) can |= GPU_ACCEL_CAN_CURSOR;
    return can;
}

bool gpu_accel_fill(int x, int y, int w, int h, u32 colour) {
    gpu_accel_binding_t *b = __atomic_load_n(&accel_selected, __ATOMIC_ACQUIRE);
    if (!b || !b->ops->fill || !b->ops->can_fill ||
        !b->ops->can_fill(b->context)) return false;
    return b->ops->fill(b->context, x, y, w, h, colour);
}

bool gpu_accel_copy(int from_x, int from_y, int to_x, int to_y, int w, int h) {
    gpu_accel_binding_t *b = __atomic_load_n(&accel_selected, __ATOMIC_ACQUIRE);
    if (!b || !b->ops->copy || !b->ops->can_copy ||
        !b->ops->can_copy(b->context)) return false;
    return b->ops->copy(b->context, from_x, from_y, to_x, to_y, w, h);
}

bool gpu_accel_cursor_move(int x, int y) {
    gpu_accel_binding_t *b = __atomic_load_n(&accel_selected, __ATOMIC_ACQUIRE);
    if (!b || !b->ops->cursor_move || !b->ops->can_cursor ||
        !b->ops->can_cursor(b->context)) return false;
    b->ops->cursor_move(b->context, x, y);
    return true;
}

const char *gpu_accel_owner(void) {
    gpu_accel_binding_t *b = __atomic_load_n(&accel_selected, __ATOMIC_ACQUIRE);
    return b ? b->owner : "";
}

bool gpu_accel_is_selected(u8 bus, u8 slot, u8 func) {
    gpu_accel_binding_t *b = __atomic_load_n(&accel_selected, __ATOMIC_ACQUIRE);
    return b && b->bus == bus && b->slot == slot && b->func == func;
}

u32 gpu_accel_draw_mode(void) {
    gpu_accel_binding_t *b = __atomic_load_n(&accel_selected, __ATOMIC_ACQUIRE);
    if (!b || !b->ops->draw_mode || !b->ops->draw_triangles) return 0;
    return b->ops->draw_mode(b->context);
}

int gpu_accel_draw_triangles(const float *vertices, u32 triangles) {
    gpu_accel_binding_t *b = __atomic_load_n(&accel_selected, __ATOMIC_ACQUIRE);
    if (!b || !vertices || !triangles || !b->ops->draw_mode ||
        !b->ops->draw_triangles || !b->ops->draw_mode(b->context)) return -1;
    return b->ops->draw_triangles(b->context, vertices, triangles);
}

int gpu_accel_draw_image(const u32 *pixels, u32 width, u32 height, u32 stride,
                         int x, int y, u32 draw_width, u32 draw_height) {
    gpu_accel_binding_t *b = __atomic_load_n(&accel_selected, __ATOMIC_ACQUIRE);
    if (!b || !b->ops->draw_image || !pixels || !width || !height || stride < width)
        return -1;
    return b->ops->draw_image(b->context, pixels, width, height, stride,
                             x, y, draw_width, draw_height);
}

bool gpu_accel_read_pixel(int x, int y, u32 *pixel) {
    gpu_accel_binding_t *b = __atomic_load_n(&accel_selected, __ATOMIC_ACQUIRE);
    return b && b->ops->read_pixel && pixel && b->ops->read_pixel(b->context, x, y, pixel);
}

int gpu_accel_set_shaders(u32 format, const gpu_shader_program_t *vertex,
                         const gpu_shader_program_t *pixel) {
    gpu_accel_binding_t *b = __atomic_load_n(&accel_selected, __ATOMIC_ACQUIRE);
    if (!b || !b->ops->set_shaders || !vertex || !pixel ||
        !vertex->code || !pixel->code || !vertex->words || !pixel->words ||
        (vertex->input_count && !vertex->inputs) ||
        (vertex->output_count && !vertex->outputs) ||
        (pixel->input_count && !pixel->inputs) ||
        (pixel->output_count && !pixel->outputs)) return -1;
    return b->ops->set_shaders(b->context, format, vertex, pixel);
}

int gpu_accel_set_layout(u32 format, const u32 *elements, u32 count, u32 stride) {
    gpu_accel_binding_t *b = __atomic_load_n(&accel_selected, __ATOMIC_ACQUIRE);
    if (!b || !b->ops->set_layout || !elements || !count || !stride) return -1;
    return b->ops->set_layout(b->context, format, elements, count, stride);
}

/* The GPU half of the shutdown verdict.  See storage_report_verdict() in
 * block.c for why these are derived rather than read back out of the log. */
void gpu_report_verdict(void) {
    void nv_gsp_report(void);
    void nvidia_discovery_report(void);
    nvidia_discovery_report();

    nv_card_t *c = nv_card(0);
    if (!c) return;
    kinfo("verdict", "GPU: %s %s%s, %llu MiB of memory",
          c->architecture ? c->architecture : "?",
          c->codename ? c->codename : "?",
          c->modelled ? " (MODELLED, not real silicon)" : "",
          (unsigned long long)(c->vram_bytes / (1024 * 1024)));
    nv_gsp_report();
}

/* Run the GPU self-tests and, on a REAL NVIDIA card, attempt the co-processor
 * (GSP) boot automatically, writing the whole verdict to /boot/KESTREL/gpu-boot.log on
 * the stick.  The user asked for this so they need not type `gpu start` each
 * boot and can read the result off the USB.  Two safety points are built in:
 *   - It only writes to the card when nv_card(0) is real (not NULL, not
 *     modelled), so the VM and any model boot just log the self-tests.
 *   - The log is flushed to the stick BEFORE the write that hands the card its
 *     firmware, so a boot that blanks the display still leaves the trail.
 * `nogpustart` on the boot line turns the write attempt off (the self-tests
 * still run and log). */
void gpu_boot_and_log(void) {
    int intel_display_selftest(void);
    int nv_compute_selftest(void);
    int nv_gsp_boot_selftest(void);
    int nv_gsp_init_rpc_selftest(void);
    const char *nv_gsp_last_stage(void);

    /* Native multi-head + per-engine verdicts are intentionally detailed.
     * Keep enough room that snprintf's would-have-written return cannot carry
     * the persisted length beyond the backing buffer. */
    static char rep[65536];
    int n = 0;
    #define GLINE(...) do { if (n < (int)sizeof rep - 1) { \
        int room = (int)sizeof rep - n; \
        int written = snprintf(rep + n, (size_t)room, __VA_ARGS__); \
        if (written > 0) n += written < room ? written : room - 1; \
    } } while (0)

    char stamp[32];
    time_format(stamp, sizeof stamp, time_unix_seconds());
    GLINE("KestrelOS GPU boot log  (%s)\n", stamp);
    GLINE("================================================\n\n");
    int t_intel = intel_display_selftest();
    int t_comp  = nv_compute_selftest();
    int t_gspb  = nv_gsp_boot_selftest();
    int t_gspr  = nv_gsp_init_rpc_selftest();
    int t_svga  = svga3d_selftest();
    GLINE("self-tests (PASS = 0 failures):\n");
    GLINE("  Intel modeset arithmetic ... %s\n", t_intel == 0 ? "PASS" : "FAIL");
    GLINE("  NVIDIA compute dispatch  ... %s\n", t_comp  == 0 ? "PASS" : "FAIL");
    GLINE("  NVIDIA GSP boot staging  ... %s\n", t_gspb  == 0 ? "PASS" : "FAIL");
    GLINE("  NVIDIA GSP init RPCs     ... %s\n", t_gspr  == 0 ? "PASS" : "FAIL");
    GLINE("  SVGA3D (virtual GPU)     ... %s\n", t_svga  == 0 ? "PASS/absent" : "FAIL");
    GLINE("\n");

    /* Prove the fix that unblocks /data on this machine is live.  The USB stick
     * and Wi-Fi could not work when their DMA buffers landed above 4 GiB, which
     * only happens once a machine has enough RAM to reach there - so on the
     * user's box (unlike a small VM) it showed.  Report the RAM span and show a
     * freshly allocated DMA buffer comes back below 4 GiB. */
    u64 rammib = pmm_total_bytes() / (1024 * 1024);
    u64 dphys = 0;
    void *dp = dma_alloc_pages(1, &dphys);
    GLINE("system RAM: %llu MiB usable%s\n", (unsigned long long)rammib,
          rammib > 4096 ? " (extends past 4 GiB - the case that broke real hardware)" : "");
    GLINE("DMA buffer test: allocated at %#llx - %s\n",
          (unsigned long long)dphys,
          (dphys && dphys < 0x100000000ull) ? "below 4 GiB (32-bit DMA OK)"
                                             : "ABOVE 4 GiB (32-bit DMA would fail)");
    if (dp) dma_free_pages(dp, 1);
    kinfo("gpu", "DMA below-4GiB check: %llu MiB RAM, buffer at %#llx (%s)",
          (unsigned long long)rammib, (unsigned long long)dphys,
          (dphys && dphys < 0x100000000ull) ? "OK" : "ABOVE 4 GiB");
    GLINE("\n");

    /* The Intel iGPU (#8) - read-only, so safe on any machine and run always.
     * On the user's box it reports whether the iGPU is present and what it is
     * driving: the no-GSP display route if the NVIDIA boot does not take. */
    intel_display_state_t ids;
    intel_display_read(&ids);
    GLINE("Intel iGPU (#8): %s\n", ids.present ? "present" : "not on the PCI bus");
    if (ids.present) {
        GLINE("  %d pipe(s) lit of %d\n", ids.active_pipes, INTEL_DISPLAY_PIPES);
        for (int i = 0; i < INTEL_DISPLAY_PIPES; i++)
            if (ids.pipe[i].active)
                GLINE("  pipe %c: %ux%u, primary plane %s\n", 'A' + i,
                      ids.pipe[i].width, ids.pipe[i].height,
                      ids.pipe[i].plane_on ? "on" : "off");
    }
    GLINE("\n");

    nv_card_t *c = nv_card(0);
    if (!c) {
        GLINE("No NVIDIA card on the PCI bus - GSP boot not attempted (this is a VM).\n");
        int w = vfs_write_file("/boot/KESTREL/gpu-boot.log", rep, (size_t)n);
        kwarn("gpu", "wrote /boot/KESTREL/gpu-boot.log (%d bytes): %s", n, w == 0 ? "ok" : "FAILED");
        return;
    }
    GLINE("NVIDIA card: %s %s%s, %llu MiB\n",
          c->architecture ? c->architecture : "?", c->codename ? c->codename : "?",
          c->modelled ? " (MODELLED)" : "",
          (unsigned long long)(c->vram_bytes / (1024 * 1024)));
    if (c->modelled) {
        GLINE("Modelled card, not real silicon - GSP boot not attempted.\n");
        vfs_write_file("/boot/KESTREL/gpu-boot.log", rep, (size_t)n);
        return;
    }
    GLINE("GSP-RM firmware on the stick: 595.99.02 %s, 570.144 %s\n",
          firmware_present("nvidia/gb202/gsp/gsp-595.99.02.bin", 0) ? "present" : "MISSING",
          firmware_present("nvidia/gb202/gsp/gsp-570.144.bin", 0) ? "present" : "MISSING");

    /* Blackwell is already owned by the complete 595.99.02 host RM at this
     * point.  The former test below called nv_gsp_start() again, creating a
     * second RM/GSP owner, then programmed only one guessed display route.
     * Never enter that path when host RM is alive.  Drive every connected
     * output through NVIDIA's native KAPI instead: this is the interface used
     * by nvidia-drm for mode validation, DP/HDMI link training and DSC. */
    extern bool nvrm_is_ready(void);
    if (cmdline_has("gpustart") && nvrm_is_ready()) {
        /* Includes retained EDIDs; keep it off the boot task's kernel stack. */
        static nvkms_kapi_test_result_t kr;
        nvkms_kapi_accel_result_t visible_pre, visible_post;
        static nv_rm_t host_rm;
        int copy = -1, engines = 0, compute = -1, raster3d = -1;
        int nvdec = -1, nvdec_app = -1, nvenc = -1, codec_roundtrip = -1;
        bool host_tree = false, render_channels = false;
        bool pre_ok = false, post_ok = false;
        bool runtime_ok = false, runtime_accel_ok = false;
        memset(&visible_pre, 0, sizeof(visible_pre));
        memset(&visible_post, 0, sizeof(visible_post));
        GLINE("\nNative NVIDIA 595.99.02 NVKMS/KAPI multi-display test:\n");
        GLINE("  policy: highest validated pixel area, then highest refresh at that resolution\n");
        vfs_write_file("/boot/KESTREL/gpu-boot.log", rep, (size_t)n);
        klog_persist_flush();

        /* Allocate Blackwell's client-managed GR/Falcon contexts before NVKMS
         * allocates two full-resolution scanout surfaces for every head.  The
         * official 595 RM reserves a dedicated (and ordering-sensitive) context
         * region; real-card logs already proved GR + two sm_120 grids with this
         * early ordering, while six 2560x1440 scanouts ahead of GR made the
         * golden CE97/context allocations fail with NV_ERR_NO_MEMORY. */
        nv_allow_writes(true);
        host_tree = nv_rm_host_bring_up(c, &host_rm);
        if (host_tree && nv_chan_open(c, &host_rm) == 0) {
            copy = nv_chan_selftest();
            engines = nv_chan_open_engines(c, &host_rm);
            render_channels = true;
            gpu_test_watchdog_progress("render channels reserved before display surfaces");
            /* Native display probing is extremely verbose and can evict these
             * one-time allocation failures from the in-memory log ring. */
            klog_persist_flush();
        }

        bool kapi_ok = nvkms_kapi_run_display_test(&kr);
        gpu_test_watchdog_progress("multi-display pattern suite complete");
        GLINE("  functions table ........ %s\n", kr.table_ok ? "PASS" : "FAIL");
        GLINE("  device / ownership ..... %s / %s\n",
              kr.device_ok ? "PASS" : "FAIL",
              kr.ownership_ok ? "PASS" : "FAIL");
        GLINE("  connected / active ..... %u / %u\n",
              kr.connected_displays, kr.active_displays);
        GLINE("  validate / atomic commit  %s / %s (heads %#x)\n",
              kr.validated ? "PASS" : "FAIL",
              kr.committed ? "PASS" : "FAIL", kr.active_heads_mask);
        GLINE("  kernel-ring records dropped before report: %llu\n",
              (unsigned long long)klog_dropped());
        for (u32 i = 0; i < kr.evidence_count; i++) {
            const nvkms_display_evidence_t *e = &kr.evidence[i];
            GLINE("  identity %#x connector %#x %s: %s %s; EDID %s, %u bytes, wait %u ms\n",
                  e->handle, e->connector, e->is_dp ? "DP" : "non-DP",
                  e->manufacturer[0] ? e->manufacturer : "unknown",
                  e->model[0] ? e->model : "unknown",
                  e->source == 2 ? "bound override" : e->source == 1 ? "live" : "unvalidated",
                  e->size, e->waited_ms);
            for (u32 j = 0; j < e->size; j += 16) {
                GLINE("    EDID %#x +%03x:", e->handle, j);
                for (u32 b = j; b < e->size && b < j + 16; b++)
                    GLINE(" %02x", e->bytes[b]);
                GLINE("\n");
            }
        }
        for (u32 i = 0; i < kr.active_displays; i++) {
            GLINE("  display %#x -> head%u: %ux%u @ %u.%03u Hz, pixel clock %u Hz\n",
                  kr.display[i].handle, kr.display[i].head,
                  kr.display[i].width, kr.display[i].height,
                  kr.display[i].refresh_millihz / 1000u,
                  kr.display[i].refresh_millihz % 1000u,
                  kr.display[i].pixel_clock_hz);
        }
        if (!kapi_ok)
            GLINE("  stopped at: %s\n", kr.failed_at ? kr.failed_at : "unknown");
        GLINE("  patterns / output CRC .. %s / %s\n",
              kapi_ok ? "PASS" : "FAIL",
              kr.scanout_confirmed ? "all-head GPU output CRC changed" : "unconfirmed");
        GLINE("  physical sink signal/pixels: NOT established by GPU CRC or modeset acceptance\n");
        /* Keep discovery even if a later RM query fails to return. */
        vfs_write_file("/boot/KESTREL/gpu-boot.log", rep, (size_t)n);
        /* Native host-RM readback only, after KAPI returns. Do not call the
         * legacy direct-GSP relight/AUX ownership path alongside host NVKMS. */
        if (host_tree) {
            u32 dp_paths = 0;
            for (u32 i = 0; i < kr.evidence_count; i++)
                if (kr.evidence[i].is_dp) dp_paths |= kr.evidence[i].handle;
            u32 query_object = 0;
            if (dp_paths) {
                u32 query_status = nv_rm_host_display_query_object(c, &host_rm, &query_object);
                GLINE("  host DP query object status %#x (not a modeset result)\n", query_status);
            }
            if (query_object) for (u32 i = 0; i < kr.evidence_count; i++) {
                const nvkms_display_evidence_t *e = &kr.evidence[i];
                if (!e->is_dp) continue;
                nvrm_dp_link_t link;
                nvrm_host_read_dp_link(host_rm.client, query_object, e->handle, &link);
                GLINE("  DP %#x TX status %#x/%#x valid %u stable %u: lanes %u rate-code %#x UHBR-10Mbps %u FEC %u\n",
                      e->handle, link.tx_status, link.tx_after_status, link.tx_valid,
                      link.tx_config_stable, link.lanes, link.rate_code, link.rate_10mbps, link.fec);
                GLINE("    AUX status %#x reply %#x count %u valid %u; DPCD 200..205: %02x %02x %02x %02x %02x %02x; legacy lane lock %s\n",
                      link.aux_status, link.aux_reply, link.aux_bytes, link.rx_valid,
                      link.receiver[0], link.receiver[1], link.receiver[2], link.receiver[3],
                      link.receiver[4], link.receiver[5],
                      !link.legacy_lock_known ? "unknown" : link.legacy_locked ? "locked" : "NOT LOCKED");
                GLINE("    MSA status %#x: active %ux%u totals %ux%u start %u/%u sync %u/%u polarity %u/%u Mvid/Nvid %u/%u misc %#x/%#x\n",
                      link.msa_status, link.width, link.height, link.h_total, link.v_total,
                      link.h_start, link.v_start, link.h_sync, link.v_sync,
                      link.h_positive, link.v_positive, link.mvid, link.nvid, link.misc0, link.misc1);
            }
        }
        /* Persist the bounded discovery snapshot before later engine/codec
         * work. No filesystem call is made from a KAPI callback or while a
         * display transaction is in progress. */
        vfs_write_file("/boot/KESTREL/gpu-boot.log", rep, (size_t)n);
        klog_persist_flush();

        /* NVKMS owns the live displays on the same official host RM.  With the
         * scarce engine contexts already reserved, exercise them against the
         * newly committed physical scanout surfaces. */
        if (kapi_ok && render_channels) {
                compute = nv_compute_selftest_hw();
                (void)nv_3d_selftest_hw();
                raster3d = nv_3d_raster_selftest_hw();
                gpu_test_watchdog_progress("render engines and offscreen proofs complete");

                /* These are physical-display tests, not hidden buffers: each
                 * active head receives its own hardware-rendered 2D frame and
                 * SM-rasterized 3D scene for eight seconds. */
                pre_ok = nvkms_kapi_run_visible_accel_test(&visible_pre, false);
                gpu_test_watchdog_progress("pre-codec visible all-head proof complete");

                /* Keep codecs independent in the verdict.  The second visible
                 * cycle proves a failed Falcon experiment did not wedge the
                 * render/display path that the desktop is about to use. */
                nvdec = nv_nvdec_selftest_hw();
                /* Validate actual compressed nonuniform pixels and the dynamic
                 * application buffers independently of native encoder success. */
                if (nvdec == 0) nvdec_app = nv_nvdec_application_selftest_hw();
                nvenc = nv_nvenc_selftest_hw();
                if (nvdec == 0 && nvenc == 0)
                    codec_roundtrip = nv_nvenc_roundtrip_hw();
                gpu_test_watchdog_progress("codec proofs complete");
                post_ok = nvkms_kapi_run_visible_accel_test(&visible_post, true);
                gpu_test_watchdog_progress("post-codec visible all-head proof complete");
                if (post_ok) {
                    runtime_ok = nvkms_kapi_publish_runtime_framebuffer();
                    if (runtime_ok)
                        runtime_accel_ok = nvkms_kapi_runtime_accel_selftest();
                    gpu_test_watchdog_progress("production desktop acceleration sequence complete");
                }
        }

        #define HVS(v) ((v) == 0 ? "PASS" : ((v) > 0 ? "FAIL" : "not run"))
        GLINE("\nNative host-RM hardware engine results:\n");
        GLINE("  render client / copy ... %s / %s\n",
              host_tree ? "PASS" : "FAIL", HVS(copy));
        GLINE("  engine objects ......... %d/4 opened\n", engines);
        GLINE("  compute / offscreen 3D . %s / %s\n", HVS(compute), HVS(raster3d));
        GLINE("  visible 2D all heads ... %s (draw %#x pixel %#x CRC %#x)\n",
              visible_pre.two_d_pass ? "PASS" : "FAIL",
              visible_pre.two_d_drawn_mask, visible_pre.two_d_pixel_mask,
              visible_pre.two_d_crc_mask);
        GLINE("  visible 3D all heads ... %s (draw %#x pixel %#x CRC %#x)\n",
              visible_pre.three_d_pass ? "PASS" : "FAIL",
              visible_pre.three_d_drawn_mask, visible_pre.three_d_pixel_mask,
              visible_pre.three_d_crc_mask);
        if (!pre_ok)
            GLINE("  visible pre-codec stop . %s\n",
                  visible_pre.failed_at ? visible_pre.failed_at : "not reached");
        GLINE("  NVDEC / NVENC H.264 .... %s / %s\n", HVS(nvdec), HVS(nvenc));
        GLINE("  NVDEC application IDR .. %s (independent nonuniform reference)\n", HVS(nvdec_app));
        GLINE("  NVENC -> NVDEC pixels .. %s\n", HVS(codec_roundtrip));
        GLINE("  post-codec 2D / 3D ..... %s / %s\n",
              visible_post.two_d_pass ? "PASS" : "FAIL",
              visible_post.three_d_pass ? "PASS" : "FAIL");
        if (!post_ok)
            GLINE("  visible post-codec stop  %s\n",
                  visible_post.failed_at ? visible_post.failed_at : "not reached");
        GLINE("  desktop scanout bridge . %s\n", runtime_ok ? "PASS" : "FAIL");
        GLINE("  desktop accel sequence . %s\n", runtime_accel_ok ? "PASS" : "FAIL");

        /* Desktop admission is tied to what remains usable after codecs.  A
         * codec can fail its own test without falsely relabelling 2D/3D, but
         * any render/display regression withholds the desktop. */
        gpu_test_gui_ready = kapi_ok && kr.scanout_confirmed && host_tree &&
                             copy == 0 && compute == 0 && raster3d == 0 &&
                             pre_ok && post_ok && runtime_ok && runtime_accel_ok;
        GLINE("  hardware desktop gate .. %s\n",
              gpu_test_gui_ready ? "PASS" : "FAIL");
        GLINE("=================================================\n");
        vfs_write_file("/boot/KESTREL/gpu-boot.log", rep, (size_t)n);
        kprintf("\nNATIVE NVIDIA TEST: display %s, copy %s, compute %s, visible 2D/3D %s, NVDEC %s, NVENC %s\n",
                kapi_ok ? "PASS" : "FAIL", HVS(copy), HVS(compute),
                (pre_ok && post_ok) ? "PASS" : "FAIL", HVS(nvdec), HVS(nvenc));
        #undef HVS
        return;
    }

    /* Booting GSP resets the co-processor that is CURRENTLY driving the display
     * (the firmware started it), which freezes the panel on whatever was last
     * drawn - the kernel-log console - and never returns until a display re-light
     * (NVKMS modeset) we have not built.  So the desktop, though it runs, is
     * never seen.  Default now is to LEAVE the card alone so the firmware
     * framebuffer stays live and the desktop is actually visible; the GSP boot is
     * opt-in with `gpustart` on the boot line (for the accel bring-up work). */
    if (!cmdline_has("gpustart")) {
        GLINE("Left the co-processor alone (no `gpustart`) so the display the\n"
              "firmware is driving keeps working and the desktop is visible.\n"
              "Add `gpustart` to the boot line to attempt the GSP/accel bring-up\n"
              "(that freezes the panel until display re-light exists).\n");
        /* `redfill`: prove the OS drives real pixels on the NVIDIA-connected
         * monitor by solid-filling the firmware framebuffer RED - no GSP modeset,
         * no channel takeover, just the OS writing the live scanout the firmware
         * left running (the same surface the boot log renders on).  Hold so it
         * stays on screen for the user to see. */
        if (cmdline_has("redfill")) {
            GLINE("redfill: filling the firmware framebuffer solid RED "
                  "(OS pixels on the live NVIDIA scanout, no GSP).\n");
            vfs_write_file("/boot/KESTREL/gpu-boot.log", rep, (size_t)n);
            klog_persist_flush();
            console_fill_rgb(0xFF, 0x00, 0x00);   /* solid red */
            for (;;) { __asm__ __volatile__("hlt"); }   /* hold the red screen */
        }
        vfs_write_file("/boot/KESTREL/gpu-boot.log", rep, (size_t)n);
        return;
    }

    /* `rgbtest`: BEFORE we touch the card (the GSP boot below resets the
     * co-processor and kills this firmware framebuffer), prove the display
     * pipeline end-to-end by cycling the LIVE firmware scanout through solid
     * RED, GREEN, BLUE, WHITE.  These are OS pixels on the monitor the firmware
     * lit - guaranteed visible - so the user sees real colour on the panel
     * first; the driver's own GSP-channel modeset re-light is then attempted
     * below (the real NVIDIA-driver test).  One boot does both. */
    if (cmdline_has("rgbtest")) {
        GLINE("\nrgbtest: cycling the firmware framebuffer RED/GREEN/BLUE/WHITE "
              "(OS pixels on the live scanout) before the GSP boot.\n");
        klog_persist_flush();
        console_fill_rgb(0xFF, 0x00, 0x00); timer_mdelay(2500);  /* red   */
        console_fill_rgb(0x00, 0xFF, 0x00); timer_mdelay(2500);  /* green */
        console_fill_rgb(0x00, 0x00, 0xFF); timer_mdelay(2500);  /* blue  */
        console_fill_rgb(0xFF, 0xFF, 0xFF); timer_mdelay(1500);  /* white */
        console_clear();
    }

    /* Real card, writes permitted.  Flush FIRST - the next step writes to the
     * card that is drawing this display; if it will not accept the firmware it
     * stays halted and the screen does not return until a power cycle. */
    GLINE("\nAttempting the co-processor (GSP) boot - this WRITES to the card.\n");
    GLINE("If the display does not come back, power off and on, then read this file.\n\n");
    vfs_write_file("/boot/KESTREL/gpu-boot.log", rep, (size_t)n);

    /* Flush the persistent event log (KERNEL.LOG on the FAT boot volume) to the
     * stick NOW, before the write below - the GSP boot can halt the card that
     * is drawing this display, and a halted machine never flushes the buffered
     * lines that describe how far the attempt got.  Everything up to this point
     * - including the whole FSP handoff about to run - must already be on the
     * stick so it can be read after a power cycle. */
    klog_persist_flush();

    nv_allow_writes(true);
    bool ok = nv_gsp_start(c);
    GLINE("  result: %s\n", ok ? "the co-processor STARTED" : "did NOT start");
    GLINE("  got as far as: %s\n", nv_gsp_last_stage());

    /* The card's own registers right after the attempt - what actually
     * happened, so one boot pinpoints the step, not just names it. */
    void nv_gsp_registers(u32 *, u32 *, u32 *, u32 *, u32 *, u32 *, u32 *);
    u32 ctl = 0, boot = 0, dma = 0, mb0 = 0, mb1 = 0, wlo = 0, whi = 0;
    nv_gsp_registers(&ctl, &boot, &dma, &mb0, &mb1, &wlo, &whi);
    GLINE("\ncard registers after the attempt:\n");
    GLINE("  RISCV CPUCTL   %#010x   Falcon CPUCTL %#010x   DMACFG %#010x\n", ctl, boot, dma);
    GLINE("  mailboxes      %#010x %#010x\n", mb0, mb1);
    GLINE("  WPR2 window    %#010x .. %#010x  -> %s\n", wlo, whi,
          (wlo && whi && whi >= wlo)
              ? "something was accepted into protected memory (good sign)"
              : "nothing placed in it (the card did not take the image)");

    nv_fsp_diag_t fd;
    nv_fsp_get_diag(&fd);
    if (fd.attempted) {
        GLINE("\nFSP handoff diagnostics:\n");
        GLINE("  PCIe FLR       %s; framebuffer released %s, supported %s, issued %s, returned %s\n",
              fd.flr_requested ? "requested" : "not requested",
              fd.framebuffer_released ? "yes" : "no",
              fd.flr_supported ? "yes" : "no",
              fd.flr_performed ? "yes" : "no",
              fd.flr_returned ? "yes" : "no");
        GLINE("  secure boot    %#010x (%s)\n", fd.secure_boot_value,
              fd.secure_boot_ready ? "ready" : "not ready");
        GLINE("  before send    cmd %#x/%#x, msg %#x/%#x; EMEM read-back %s\n",
              fd.cmd_head_before, fd.cmd_tail_before,
              fd.msg_head_before, fd.msg_tail_before,
              fd.emem_verified ? "exact" : "FAILED/not reached");
        GLINE("  after send     cmd %#x/%#x (%s), msg %#x/%#x (%s)\n",
              fd.cmd_head_after, fd.cmd_tail_after,
              fd.command_consumed ? "consumed" : "not consumed",
              fd.msg_head_after, fd.msg_tail_after,
              fd.response_seen ? "response seen" : "no response");
        if (fd.response_seen)
            GLINE("  response       task %#x, command %#x, error %#x (%s)\n",
                  fd.response_task, fd.response_command, fd.response_error,
                  fd.response_valid ? "valid success" : "INVALID/rejected");
        GLINE("  FSP scratch    %#x %#x %#x %#x\n",
              fd.scratch[0], fd.scratch[1], fd.scratch[2], fd.scratch[3]);
    }
    GLINE("\nRead this back to the assistant to say what to fix next.\n");
    vfs_write_file("/boot/KESTREL/gpu-boot.log", rep, (size_t)n);
    kinfo("gpu", "GSP boot attempt written to /boot/KESTREL/gpu-boot.log: %s", nv_gsp_last_stage());

    /* Do not synchronously append KERNEL.LOG in the GPU transaction.  A FAT
     * append stalled the 06:22 boot immediately after the proven triangle and
     * prevented display, codecs, summary, desktop, and reboot from running.
     * The main idle/log worker drains the in-memory ring after this returns. */

    /* ---- Final pass/fail summary -------------------------------------------
     * Reboot is deliberately NOT done here.  A dedicated gpustart boot now runs
     * this function synchronously after kernel setup, then either starts the
     * hardware-backed desktop or withholds it.  A separate sleeping thread owns
     * the observation countdown so a launched desktop remains schedulable. */
    int hw_comp = -1, hw_runtime = -1, hw_nvdec = -1, hw_nvenc = -1, hw_3d = -1;
    nv_gsp_hw_verdicts(&hw_comp, &hw_runtime, &hw_nvdec, &hw_nvenc, &hw_3d);
    #define VS(v) ((v) == 0 ? "PASS" : ((v) > 0 ? "FAIL" : "not run"))

    GLINE("\n================  RESULT SUMMARY  ================\n");
    GLINE("  GSP co-processor boot ....... %s\n", ok ? "PASS" : "FAIL");
    GLINE("  Compute dispatch (HW) ....... %s\n", VS(hw_comp));
    GLINE("  GUI/2D live scanout (HW) ... %s\n", VS(hw_runtime));
    GLINE("  NVENC H.264 encode (HW) ..... %s\n", VS(hw_nvenc));
    GLINE("  NVDEC H.264 decode (HW) ..... %s\n", VS(hw_nvdec));
    GLINE("  3D graphics (HW) ............ %s\n", VS(hw_3d));
    GLINE("  model self-tests: intel %s / compute %s / gsp-boot %s / gsp-rpc %s\n",
          VS(t_intel), VS(t_comp), VS(t_gspb), VS(t_gspr));
    GLINE("  (full detail in KERNEL.LOG on this stick)\n");

    /* Durable DisplayPort re-light trace: the klog ring evicts the nv-disp lines
     * before this flush, so the re-light records each RM step in a struct that
     * survives.  This block is the ONLY reliable record of how far the re-light
     * got and what the display queries returned (the KERNEL.LOG nv-disp lines are
     * gone by now). */
    void nv_disp_relight_get_diag(nv_disp_relight_diag_t *);
    nv_disp_relight_diag_t rl;
    nv_disp_relight_get_diag(&rl);
    GLINE("\n----------------  DISPLAY RE-LIGHT  --------------\n");
    if (!rl.ran) {
        GLINE("  re-light did NOT run (nodisplay set, or channel bring-up failed)\n");
    } else {
        GLINE("  stopped at ...... %s\n", rl.stopped_at ? rl.stopped_at : "?");
        GLINE("  EDID mode ....... %s %ux%u @ %u kHz\n",
              rl.edid_ok ? "ok" : "MISSING", rl.mode_w, rl.mode_h, rl.mode_pclk_khz);
        GLINE("  RAMIN inst mem .. %s (WRITE_INST_MEM)\n", rl.inst_mem_ok ? "registered" : "REFUSED");
        GLINE("  static info ..... %s (windows %#x, heads %u)\n",
              rl.static_info_ok ? "ok" : "refused", rl.windows_present, rl.num_heads);
        GLINE("  disp root 0xca70  %s%s", rl.disp_root_ok ? "allocated" : "REFUSED",
              rl.disp_root_ok ? "\n" : "");
        if (!rl.disp_root_ok) GLINE(" (status %#x)\n", rl.disp_root_status);
        GLINE("  GET_SUPPORTED ... mask %#x (post disp-root)\n", rl.supported_mask);
        GLINE("  CONNECT_STATE ... connected %#x (of supported)\n", rl.connected_mask);
        GLINE("  runtime sinks .... %u, valid EDID mask %#x, lit mask %#x, RGBW mask %#x\n",
              rl.sink_count, rl.edid_mask, rl.lit_mask, rl.rgbw_mask);
        GLINE("  chosen displayId  %#x (%s%s)\n", rl.display_id,
              rl.display_from_active ? "GET_ACTIVE boot head" : "fallback lowest-bit",
              rl.display_from_active ? "" : " - NO active boot head found");
        if (rl.display_from_active)
            GLINE("  boot head ....... head%u owns this display (GET_ACTIVE routing)\n",
                  rl.active_head);
        GLINE("  OR_GET_INFO ..... %s (SOR idx %u type %u protocol %u)\n",
              rl.or_info_ok ? "ok" : "REFUSED", rl.or_index, rl.or_type, rl.or_protocol);
        GLINE("  DP_GET_CAPS ..... %s (maxLinkRate %u)\n",
              rl.caps_ok ? "ok" : "refused", rl.max_link_rate);
        GLINE("  DFP_ASSIGN_SOR .. %s (slot %d)\n",
              rl.assign_sor_ok ? "ok" : "refused", rl.sor_slot);
        GLINE("  DP link training  %s after %d attempt(s) (err %#x, retryMs %u)\n",
              rl.trained ? "TRAINED" : "did NOT train", rl.dp_attempts,
              rl.dp_last_err, rl.dp_last_retry_ms);
        GLINE("  sink DPCD ....... %s rate %#x lanes %u\n",
              rl.dpcd_valid ? "read" : "FAILED", rl.dpcd_rate, rl.dpcd_lanes);
        GLINE("  DPCD AUX ........ ctrl_status %#x replyType %u (0=ACK 1=NACK 2=DEFER 3=TIMEOUT) bytes %u\n",
              rl.dpcd_aux_status, rl.dpcd_reply_type, rl.dpcd_got_bytes);
        GLINE("  DP_CTRL sent .... cmd %#x data %#x\n", rl.dp_sent_cmd, rl.dp_sent_data);
        GLINE("  DP_CTRL reason .. %#x  (E0000003=no-answer E0000004=short-reply else=RM status)\n",
              rl.dp_ctrl_status);
        GLINE("  SOR route restore  %s\n",
              rl.restore_sor_ok ? "OK" : "failed");
        GLINE("  RGB test frame .. %s (alloc status %#x)\n",
              rl.rgb_test_shown ? "solid RED VRAM surface committed" : "not shown",
              rl.rgb_alloc_status);
        GLINE("  core/window chan  %s / %s\n",
              rl.core_chan_ok ? "open" : "-", rl.window_chan_ok ? "open" : "-");
        if (rl.chan_fail_stage)
            GLINE("  chan fail ....... stage %u (%s) pb_status %#x alloc_status %#x\n",
                  rl.chan_fail_stage, rl.chan_fail_stage == 1 ? "PUSHBUFFER" : "object-alloc",
                  rl.chan_pb_status, rl.chan_alloc_status);
        GLINE("  modeset UPDATE .. %s\n", rl.committed ? "KICKED (panel should light)" : "not sent");
        GLINE("  core chan GET/PUT %#x / %#x -> %s\n", rl.core_get, rl.core_put,
              (rl.core_get == rl.core_put && rl.core_put) ? "CONSUMED (channel ran)"
                                                          : "STUCK (channel did not process)");
        GLINE("  win0 chan GET/PUT %#x / %#x -> %s\n", rl.window_get, rl.window_put,
              (rl.window_get == rl.window_put && rl.window_put) ? "CONSUMED (interlock partner ran)"
                                                                : "STUCK (attach cannot complete)");
        GLINE("  SST watermark ... %s (wm %u)\n", rl.wm_override ? "override=1 computed" : "override=0 (RM)", rl.wm_value);
        GLINE("  OLUT ............ %s\n", rl.olut_ok ? "identity LUT set" : "MISSING");
        GLINE("  UPDATE notifier . %s (val %#x)\n",
              rl.update_notified ? "LATCHED (modeset completed)" : "NOT written (did not latch)",
              rl.notifier_val);
        if (rl.exc_other & 1u)
            GLINE("  FE core ACTIVE .. stat/data/code %#x/%#x/%#x (reason %u method %#x)\n",
                  rl.core_exc_stat, rl.core_exc_data, rl.core_exc_code,
                  (rl.core_exc_stat >> 12) & 7u, (rl.core_exc_stat & 0xfffu) << 2);
        else
            GLINE("  FE core ......... summary clear; slot %#x/%#x/%#x stale/inactive\n",
                  rl.core_exc_stat, rl.core_exc_data, rl.core_exc_code);
        if (rl.exc_window & 1u)
            GLINE("  FE win0 ACTIVE .. stat/data/code %#x/%#x/%#x (reason %u method %#x)\n",
                  rl.win_exc_stat, rl.win_exc_data, rl.win_exc_code,
                  (rl.win_exc_stat >> 12) & 7u, (rl.win_exc_stat & 0xfffu) << 2);
        else
            GLINE("  FE win0 ......... summary clear; slot %#x/%#x/%#x stale/inactive\n",
                  rl.win_exc_stat, rl.win_exc_data, rl.win_exc_code);
        GLINE("  FE raw status ... summary other/win %#x/%#x detail %#x awaken win/other %#x/%#x sem %#x ctrl %#x\n",
              rl.exc_other, rl.exc_window, rl.ctrl_detail, rl.awaken_win,
              rl.awaken_other, rl.sem_win, rl.ctrl_intr);
        GLINE("  raster RG LOADV . %#x -> %#x (%s)\n", rl.rg_loadv_before, rl.rg_loadv_after,
              rl.rg_loadv_before != rl.rg_loadv_after ? "ADVANCED" : "unchanged");
        GLINE("  raster POSTCOMP . %#x -> %#x (%s)\n",
              rl.postcomp_loadv_before, rl.postcomp_loadv_after,
              rl.postcomp_loadv_before != rl.postcomp_loadv_after ? "ADVANCED" : "unchanged");
        GLINE("  raster CRASHLOCK  %#x -> %#x (V %#x -> %#x)\n",
              rl.crashlock_before, rl.crashlock_after,
              rl.crashlock_before >> 16, rl.crashlock_after >> 16);
        GLINE("  SOR DP_LINKCTL .. %#x  DP_PADCTL %#x (%s)\n",
              rl.sor_dp_linkctl, rl.sor_dp_padctl,
              (rl.sor_dp_linkctl || rl.sor_dp_padctl) ? "PAD DRIVEN" : "pad OFF - SOR not transmitting");
        /* ARM-vs-ASSY promotion probe - the decisive per-stage triage. */
        GLINE("  head0 raster .... ASSY %#x ARM %#x (%s)  pclkARM %#x\n",
              rl.head_raster_assy, rl.head_raster_arm,
              (rl.head_raster_arm == rl.head_raster_assy && rl.head_raster_arm != 0u)
                  ? "PROMOTED" : "NOT promoted - UPDATE never latched",
              rl.head_pclk_arm);
        GLINE("  RG scan counter . %#x -> %#x (%s)\n", rl.rg_dpca_1, rl.rg_dpca_2,
              rl.rg_dpca_1 != rl.rg_dpca_2 ? "SCANNING" : "frozen - no pixel clock");
        GLINE("  SF active symbols %#x (%s, wm %u)\n", rl.sf_dp_ctl,
              (rl.sf_dp_ctl & (1u << 27)) ? "ON - stream formatted" : "OFF - no active video",
              rl.sf_dp_ctl & 0x3Fu);
        GLINE("  SOR detach (PhB). kicked %d  owner ARM %#x -> %#x (%s)  dp-release err %#x\n",
              (int)rl.detach_kicked, rl.sor_owner_arm_before_detach,
              rl.sor_owner_arm_after_detach,
              rl.sor_released ? "RELEASED before attach" : "still owned",
              rl.detach_dp_release_err);
        GLINE("  SOR attach req .. value %#x (owner %#x proto %#x)\n",
              rl.sor_attach_value, rl.sor_attach_value & 0xffu,
              (rl.sor_attach_value >> 8) & 0xfu);
        GLINE("  connector XBAR .. out %u expected-low5 %#x; before A/B %#x/%#x, assigned %#x/%#x, after %#x/%#x\n",
              rl.xbar_output, rl.xbar_expected, rl.xbar_a_before, rl.xbar_b_before,
              rl.xbar_a_assigned, rl.xbar_b_assigned, rl.xbar_a_after, rl.xbar_b_after);
        GLINE("  RM head0 active . %s displayId %#x\n",
              rl.rm_active_ok ? "reported" : "query failed", rl.rm_active_display);
        GLINE("  SOR post-attach . ASSY %#x ARM %#x (%s)\n",
              rl.sor_owner_assy_p1, rl.sor_owner_arm_p1,
              (rl.sor_owner_arm_p1 & 0xFFu) ? "attached" : "still detached");
        GLINE("  SOR owner ....... ASSY %#x ARM %#x (%s)\n",
              rl.sor_owner_assy, rl.sor_owner_arm,
              (rl.sor_owner_arm & 0xFFu) ? "bound to head" : "detached");
        GLINE("  SOR power/seq ... PWR %#x (%s)  SEQ %#x (%s)\n",
              rl.sor_pwr, ((rl.sor_pwr & 1u) && !(rl.sor_pwr & 0x80000000u)) ? "PU-normal" : "not-up",
              rl.sor_seq_ctl, (rl.sor_seq_ctl & (1u << 28)) ? "busy" : "settled");
        GLINE("  core-chan/SV .... state %#x (%s)  supervisor pending %#x\n",
              rl.core_chan_state,
              (((rl.core_chan_state >> 16) & 0x1Fu) == 0xBu) ? "quiescent/good" : "NOT idle",
              rl.sv_pending & 0x7u);
    }

    bool committed = rl.ran && rl.committed;
    gpu_test_gui_ready = ok && committed && hw_runtime == 0;
    GLINE("=================================================\n");
    GLINE("Hardware desktop gate: %s.\n",
          gpu_test_gui_ready
              ? "PASS - start userland on the validated GPU scanout"
              : "FAIL - do not launch a software/frozen desktop");
    vfs_write_file("/boot/KESTREL/gpu-boot.log", rep, (size_t)n);
    /* Persistence is asynchronous from here; never hold completion/reboot
     * hostage to removable-media I/O. */

    kprintf("\n================  GPU TEST RESULT  ================\n");
    kprintf("  GSP: %s compute: %s GUI/2D: %s 3D: %s NVENC: %s NVDEC: %s\n",
            ok ? "PASS" : "FAIL", VS(hw_comp), VS(hw_runtime), VS(hw_3d),
            VS(hw_nvenc), VS(hw_nvdec));
    if (committed)
        kprintf("  DISPLAY: modeset committed on displayId %#x - LOOK AT THE MONITORS NOW.\n",
                rl.display_id);
    kprintf("  Full results saved to the USB stick (gpu-boot.log / KERNEL.LOG).\n");
    #undef VS
    #undef GLINE
}

bool gpu_boot_test_sync(void) {
    gpu_test_gui_ready = false;
    gpu_boot_and_log();
    return gpu_test_gui_ready;
}

/* The worker sleeps instead of busy-waiting, which is important when the
 * desktop was admitted: compositor/input threads retain the CPU throughout the
 * complete one-minute observation window. */
void gpu_test_reboot_kthread(void *desktop_started) {
    bool desktop = desktop_started != NULL;
    void proc_thread_exit(int code);
    void acpi_reboot(void);
    if (cmdline_has("noreboot")) {
        kinfo("gpu", "noreboot set - leaving the %s screen running indefinitely",
              desktop ? "hardware desktop" : "GPU result");
        proc_thread_exit(0);
    }
    kinfo("gpu", "%s observation window started; reboot in 60 seconds",
          desktop ? "hardware desktop" : "GPU failure screen");
    for (int s = 60; s > 0; s--) {
        if (s == 60 || s == 30 || s == 10 || s <= 5)
            kprintf("  GPU test: rebooting in %d second%s ...\n", s, s == 1 ? "" : "s");
        sched_sleep_ms(1000);
    }
    kprintf("  GPU test: rebooting now.\n");
    acpi_reboot();
    proc_thread_exit(0);
}

/* A display/codec method or removable-media write must never leave a test box
 * requiring a manual power cycle.  The ordinary worker above still performs
 * the requested reboot exactly 60 seconds after a completed test.  This
 * independent deadline is only the escape hatch when the synchronous driver
 * never returns; it deliberately calls ACPI directly without a filesystem
 * flush, because a stuck flush is one of the failures it exists to escape. */
static volatile u64 gpu_test_watchdog_deadline_ms;

void gpu_test_watchdog_arm(void) {
    gpu_test_watchdog_deadline_ms = g_uptime_ms + 120000u;
}

void gpu_test_watchdog_progress(const char *stage) {
    /* Deliberate monitor dwell time is not a hang.  Give each independently
     * bounded stage its own escape window while preserving automatic recovery
     * if any one RM/engine transaction stops returning. */
    gpu_test_watchdog_deadline_ms = g_uptime_ms + 120000u;
    kinfo("gpu", "GPU watchdog progress: %s; deadline renewed for 120 seconds",
          stage ? stage : "unnamed stage");
}

void gpu_test_watchdog_kthread(void *unused) {
    (void)unused;
    void proc_thread_exit(int code);
    void acpi_reboot(void);
    /* Use the deadline established before creation, not 120 seconds after this
     * worker first happens to be scheduled.  The 13:35 boot did not first run
     * this thread until the oversized pattern loop was already late. */
    for (;;) {
        u64 now = g_uptime_ms;
        if (now >= gpu_test_watchdog_deadline_ms) break;
        u64 left = gpu_test_watchdog_deadline_ms - now;
        sched_sleep_ms(left > 1000u ? 1000u : left);
    }
    kerr("gpu", "GPU test exceeded the 120-second hard deadline; forcing ACPI reboot");
    acpi_reboot();
    proc_thread_exit(0);
}

/* Run the GSP-RM bring-up on its OWN thread so it never blocks boot.  The
 * bring-up waits seconds for the co-processor's INIT_DONE and for each RPC
 * reply; running it inline stalled the whole startup (and starved the USB and
 * log-flush paths) behind the GPU.  As a kernel thread it starts once the
 * scheduler is up, brings the card up in the background while the desktop is
 * already interactive on the firmware framebuffer, and its RPC polls yield
 * (sched_yield) so other threads keep running.  This is why a slow or absent
 * card no longer delays the machine reaching the desktop. */
void gpu_boot_kthread(void *arg) {
    (void)arg;
    gpu_boot_and_log();
    /* A kernel thread must NOT fall off the end of its function - returning
     * jumps to a garbage address and faults (invalid opcode).  One-shot work
     * exits cleanly through proc_thread_exit. */
    void proc_thread_exit(int code);
    proc_thread_exit(0);
}
