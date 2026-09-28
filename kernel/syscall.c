/* syscall.c - the system call dispatcher.
 *
 * Every pointer that arrives from user mode is validated against the calling
 * process's address space before it is touched: a bad pointer must return
 * -E_INVAL, never fault inside the kernel.
 */
#include "kernel.h"
#include "cpu.h"
#include "proc.h"
#include "mm.h"
#include "vfs.h"
#include "block.h"
#include "pci.h"
#include "gpu.h"
#include "nv.h"
#include "nvkms_kapi_client.h"
#include "nv_surface.h"
#include "nv_video.h"
#include "usb.h"
#include "hda.h"
#include "net.h"
#include "svga.h"
#include "tls.h"
#include "wifi.h"
#include "firmware.h"
#include "klog.h"

bool efi_log_publish(void);
bool efi_variables_available(void);
#include "smp.h"
#include "time.h"
#include "acpi.h"
#include "smbios.h"
#include "input.h"
#include "../include/kestrel/syscall.h"
#include "../include/kestrel/input.h"

/* ------------------------------------------------- presenting, in parallel
 *
 * One band of rows, copied from the program's buffer to the display.  Every
 * band is independent - it reads its own rows and writes its own rows, touches
 * nothing shared, and calls nothing - which is exactly the rule a job handed to
 * the other processors has to keep.
 */
typedef struct {
    const u32 *back;
    u32       *front;
    u32        back_stride, front_stride;
    int        x, y, w, h;
} present_job_t;

/* Check that the bands between them cover every row exactly once.
 *
 * The split is four lines of arithmetic and it is the kind that is wrong
 * quietly: a row covered twice is drawn twice and looks identical, and a row
 * covered by nobody is a scanline of last frame's picture left standing in
 * this one - which reads as tearing or as a driver problem rather than as a
 * division being off by one.
 *
 * Heights that do not divide evenly are the whole risk, so those are what is
 * checked: the remainder has to go somewhere, and it has to go to a different
 * band each time.
 */
int present_band_selftest(void) {
    int failures = 0;
    static u8 covered[512];

    static const int heights[] = { 1, 2, 3, 7, 8, 17, 64, 100, 511 };
    static const int splits[]  = { 1, 2, 3, 4, 8, 20 };

    for (size_t hi = 0; hi < ARRAY_LEN(heights); hi++) {
        for (size_t si = 0; si < ARRAY_LEN(splits); si++) {
            int h = heights[hi];
            int pieces = splits[si];
            if (pieces > h) continue;      /* never more bands than rows */

            memset(covered, 0, sizeof covered);

            for (int piece = 0; piece < pieces; piece++) {
                int rows = h / pieces;
                int extra = h % pieces;
                int first = piece * rows + (piece < extra ? piece : extra);
                int count = rows + (piece < extra ? 1 : 0);

                for (int r = 0; r < count; r++) {
                    int row = first + r;
                    if (row < 0 || row >= h) {
                        kwarn("fb", "selftest: %d rows in %d bands - band %d "
                                    "reaches row %d, which is outside the "
                                    "rectangle", h, pieces, piece, row);
                        failures++;
                        continue;
                    }
                    covered[row]++;
                }
            }

            for (int r = 0; r < h; r++) {
                if (covered[r] == 1) continue;
                kwarn("fb", "selftest: %d rows in %d bands - row %d is covered "
                            "%u time(s)", h, pieces, r, covered[r]);
                failures++;
                break;                     /* one report per case is enough */
            }
        }
    }

    if (!failures)
        kinfo("fb", "the bands a frame is split into cover every row exactly "
                    "once, at every size and every number of processors");
    return failures;
}

static void present_band(void *arg, int piece, int pieces) {
    const present_job_t *j = arg;

    int rows = j->h / pieces;
    int extra = j->h % pieces;
    int first = piece * rows + (piece < extra ? piece : extra);
    int count = rows + (piece < extra ? 1 : 0);

    for (int r = 0; r < count; r++) {
        int row = j->y + first + r;
        const u32 *src = j->back + (u64)row * j->back_stride + j->x;
        u32 *dst = j->front + (u64)row * j->front_stride + j->x;
        memcpy(dst, src, (size_t)j->w * 4);
    }
}

void mouse_position(int *x, int *y);
bool mouse_present(void);

/* ------------------------------------------------------------------------- */
/* user pointer validation                                                   */
/* ------------------------------------------------------------------------- */

/* True when [addr, addr+len) is entirely inside the calling process's own
 * mappings.  Kernel threads are trusted with their own addresses. */
static bool user_range_ok(u64 addr, size_t len, bool need_write) {
    proc_t *p = proc_current();
    if (!p) return false;
    if (p->is_kernel) return true;
    if (len == 0) return true;

    /* Reject anything that wraps or reaches into the non-canonical hole. */
    if (addr + len < addr) return false;
    if (addr + len > USER_STACK_TOP) return false;
    if (addr < PAGE_SIZE) return false;

    /* The walk is serialized against table mutation/destruction. Callers that
     * retain a pointer still need mapping lifetime protection across its use;
     * this permission check alone is not a pin. */
    return vmm_user_range_ok(p->pml4, addr, len, need_write);
}

/* Unlike a permission check followed by memcpy, this keeps page tables stable
 * through the bounded access even if another CPU attempts munmap concurrently.
 * The caller supplies a kernel buffer, never a second unchecked user pointer. */
static bool user_copy(void *buffer, u64 address, size_t bytes, bool to_user) {
    proc_t *p = proc_current();
    if (!p || !buffer || bytes > VMM_USER_COPY_MAX) return false;
    if (!bytes) return true;
    if (p->is_kernel) {
        if (to_user) memcpy((void *)address, buffer, bytes);
        else memcpy(buffer, (const void *)address, bytes);
        return true;
    }
    if (address < PAGE_SIZE || address >= USER_STACK_TOP || bytes > USER_STACK_TOP - address)
        return false;
    return vmm_user_copy(p->pml4, buffer, address, bytes, to_user);
}

/* Copy a NUL-terminated string in from user space, bounded. */
static int copy_user_string(u64 addr, char *out, size_t cap) {
    if (!addr) return -E_INVAL;
    for (size_t i = 0; i < cap; i++) {
        if (addr > ~0ULL - i || !user_copy(&out[i], addr + i, 1, false)) return -E_INVAL;
        if (!out[i]) return 0;
    }
    return -E_NAMETOOLONG;
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static s64 do_open(proc_t *p, u64 path_ptr, u32 flags) {
    char raw[VFS_PATH_MAX], full[VFS_PATH_MAX];
    int r = copy_user_string(path_ptr, raw, sizeof raw);
    if (r < 0) return r;
    proc_resolve_path(p, raw, full, sizeof full);

    file_t *f = NULL;
    r = vfs_open(full, flags, &f);
    if (r < 0) return r;

    int fd = proc_fd_alloc(proc_shared(p), f);
    if (fd < 0) { vfs_close(f); return fd; }
    return fd;
}

/* ------------------------------------------------------------------------- */
/* enumeration                                                               */
/* ------------------------------------------------------------------------- */

static s64 enum_block(u32 index, u64 out_ptr) {
    if (!user_range_ok(out_ptr, sizeof(kblockinfo_t), true)) return -E_INVAL;

    u32 seen = 0;
    for (blockdev_t *d = block_first(); d; d = d->next) {
        if (seen++ != index) continue;

        kblockinfo_t info;
        memset(&info, 0, sizeof info);
        strlcpy(info.name, d->name, sizeof info.name);
        strlcpy(info.model, d->model, sizeof info.model);
        strlcpy(info.label, d->part_label, sizeof info.label);
        info.size = d->sector_count * d->sector_size;
        info.sector_size = d->sector_size;
        info.part_index = d->part_index;
        info.is_partition = d->parent != NULL;
        info.readonly = d->readonly;
        if (d->parent) {
            guid_format(d->part_type_guid, info.type_guid, sizeof info.type_guid);
            guid_format(d->part_guid, info.part_guid, sizeof info.part_guid);
        }
        memcpy((void *)out_ptr, &info, sizeof info);
        return 0;
    }
    return -E_NOENT;
}

static s64 enum_pci(u32 index, u64 out_ptr) {
    if (!user_range_ok(out_ptr, sizeof(kpciinfo_t), true)) return -E_INVAL;

    u32 seen = 0;
    for (pci_dev_t *d = pci_first(); d; d = d->next) {
        if (seen++ != index) continue;

        kpciinfo_t info;
        memset(&info, 0, sizeof info);
        info.segment = d->segment;
        info.bus = d->bus; info.slot = d->slot; info.func = d->func;
        info.vendor = d->vendor; info.device = d->device;
        info.class_code = d->class_code; info.subclass = d->subclass;
        info.prog_if = d->prog_if; info.revision = d->revision;
        if (d->driver) strlcpy(info.driver, d->driver, sizeof info.driver);
        pci_describe(d, info.description, sizeof info.description);
        memcpy((void *)out_ptr, &info, sizeof info);
        return 0;
    }
    return -E_NOENT;
}

static s64 enum_gpu(u32 index, u64 out_ptr) {
    if (!user_range_ok(out_ptr, sizeof(kgpuinfo_t), true)) return -E_INVAL;

    gpu_info_t g;
    if (!gpu_get((int)index, &g)) return -E_NOENT;

    kgpuinfo_t info;
    memset(&info, 0, sizeof info);
    info.pci_vendor = g.pci_vendor;
    info.pci_device = g.pci_device;
    info.bus = g.bus; info.slot = g.slot; info.func = g.func;
    info.boot_display = g.is_boot_display ? 1 : 0;
    info.vram_exact = g.vram_exact ? 1 : 0;
    info.accel = (u8)g.accel;
    info.chipset = g.chipset;
    info.vram_bytes = g.vram_bytes;
    strlcpy(info.name, g.name, sizeof info.name);
    strlcpy(info.arch, g.arch, sizeof info.arch);
    strlcpy(info.note, g.note, sizeof info.note);
    strlcpy(info.firmware_name, g.firmware_name, sizeof info.firmware_name);
    info.firmware_needed  = g.firmware_needed ? 1 : 0;
    info.firmware_present = g.firmware_present ? 1 : 0;

    /* What the card is doing, when it is one this driver watches.  A card it
     * only identified reports every engine as absent rather than as idle. */
    for (int e = 0; e < 4; e++) info.engine_percent[e] = -1;
    info.temperature_c = -1000;

    if (g.vendor == GPU_NVIDIA) {
        nv_card_t *c = nv_card_for_pci(g.bus, g.slot, g.func);
        if (c) {
            nv_telemetry_t t;
            nv_telemetry_read(c, &t);
            for (int e = 0; e < 4 && e < NV_ENGINE_COUNT; e++)
                info.engine_percent[e] = t.engine_percent[e];
            info.engines_sampled = t.sampled ? 1 : 0;
            info.temperature_c   = t.temperature_c;
            info.fan_percent     = t.fan_percent;
            info.vram_used       = t.vram_used;
            strlcpy(info.driver_version, nv_driver_version(),
                    sizeof info.driver_version);
            strlcpy(info.driver_date, nv_driver_date(), sizeof info.driver_date);
        }
    }

    /* The drawing interfaces this system offers.  They are the same whichever
     * card is fitted, because they are implemented here rather than by the
     * card - which is exactly what the renderer line says. */
    /* All four, because all four are there.
     *
     * This said "OpenGL 1.2, Direct3D 9" and understated the system: there is
     * a Direct3D 11 layer and a Vulkan one beside them, and gfxtest draws the
     * same scene through every one and compares the pixels.  Naming two of
     * four is the same kind of error as naming a DirectX version that does not
     * exist - it just happens to err in the other direction. */
    strlcpy(info.graphics_apis,
            "Kestrel GL / Vulkan / D3D subsets; not conformance certified",
            sizeof info.graphics_apis);
    bool screen_target = gpu_accel_is_selected(g.bus, g.slot, g.func);
    if (screen_target && (gpu_accel_capabilities() &
                           (GPU_ACCEL_CAN_FILL | GPU_ACCEL_CAN_COPY)))
        info.accel = GPU_ACCEL_2D;
    strlcpy(info.renderer,
            g.vendor==GPU_NVIDIA && screen_target && gpu_accel_draw_mode()==2 ? "GPU compute raster + VRAM composition"
                : screen_target && gpu_accel_draw_mode()==1 ? "GPU 3D (selected adapter)"
                : screen_target && info.accel==GPU_ACCEL_2D ? "GPU fills and copies"
                : g.accel==GPU_ACCEL_FRAMEBUFFER && !gpu_accel_owner()[0] &&
                  g_boot.fb.base && g_boot.fb.size ? "CPU framebuffer (this adapter)"
                : "Not selected / renderer unavailable",
            sizeof info.renderer);

    memcpy((void *)out_ptr, &info, sizeof info);
    return 0;
}

static s64 enum_videomode(u32 index, u64 out_ptr) {
    if (!user_range_ok(out_ptr, sizeof(kvideomode_t), true)) return -E_INVAL;
    if (index >= g_boot.mode_count) return -E_NOENT;

    const kboot_video_mode *m = &g_boot.modes[index];
    kvideomode_t out;
    memset(&out, 0, sizeof out);
    out.width = m->width;
    out.height = m->height;
    out.current = (m->flags & KB_MODE_CURRENT) ? 1 : 0;
    out.native = (m->flags & KB_MODE_NATIVE) ? 1 : 0;
    memcpy((void *)out_ptr, &out, sizeof out);
    return 0;
}

static s64 enum_display_output(u32 index, u64 out_ptr) {
    if (!user_range_ok(out_ptr, sizeof(kdisplay_output_t), true)) return -E_INVAL;
    kdisplay_output_t out;
    if (!nvkms_kapi_display_output(index, &out)) return -E_NOENT;
    memcpy((void *)out_ptr, &out, sizeof out);
    return 0;
}

static s64 enum_display_output_mode(u32 index, u64 out_ptr) {
    if (!user_range_ok(out_ptr, sizeof(kdisplay_mode_t), true)) return -E_INVAL;
    kdisplay_mode_t out;
    if (!nvkms_kapi_display_mode(index >> 16, index & 0xffffu, &out)) return -E_NOENT;
    memcpy((void *)out_ptr, &out, sizeof out);
    return 0;
}

static s64 enum_display(u32 index, u64 out_ptr) {
    if (index != 0) return -E_NOENT;
    if (!user_range_ok(out_ptr, sizeof(kdisplayinfo_t), true)) return -E_INVAL;

    kdisplayinfo_t out;
    memset(&out, 0, sizeof out);
    out.native_width    = g_boot.display.native_width;
    out.native_height   = g_boot.display.native_height;
    out.refresh_mhz     = g_boot.display.refresh_mhz;
    out.max_refresh_mhz = g_boot.display.max_refresh_mhz;
    out.phys_width_mm   = g_boot.display.phys_width_mm;
    out.phys_height_mm  = g_boot.display.phys_height_mm;
    out.mode_count      = (u16)g_boot.mode_count;
    out.present         = g_boot.display.present;
    memcpy(out.manufacturer, g_boot.display.manufacturer, sizeof out.manufacturer);
    memcpy(out.model, g_boot.display.model, sizeof out.model);

    /* And what the adapter itself will take, which is a different and usually
     * much larger question than what the firmware offered. */
    if (svga_present()) {
        u32 max_w = 0, max_h = 0, vram = 0;
        svga_limits(&max_w, &max_h, &vram);
        out.driver_max_width = max_w;
        out.driver_max_height = max_h;
        out.driver_vram_bytes = vram;
        out.driver_present = 1;
        out.display_count = (uint8_t)svga_num_displays();   /* for #7 UI */
    }
    nv_card_t *display_card = nv_chan_display_card();
    if (nvkms_kapi_runtime_selected() && display_card) {
        nv_disp_relight_diag_t d;
        nv_disp_relight_get_diag(&d);
        out.driver_present = 1;
        out.driver_max_width = 10240;
        out.driver_max_height = 4320;
        out.driver_vram_bytes = display_card->vram_bytes;
        if (d.sink_count) out.display_count = (uint8_t)d.sink_count;
    }
    if (out.display_count < 1) out.display_count = 1;

    memcpy((void *)out_ptr, &out, sizeof out);
    return 0;
}

static s64 enum_net(u32 index, u64 out_ptr) {
    if (!user_range_ok(out_ptr, sizeof(knetinfo_t), true)) return -E_INVAL;

    u32 seen = 0;
    for (netdev_t *d = netdev_first(); d; d = d->next) {
        if (seen++ != index) continue;

        knetinfo_t out;
        memset(&out, 0, sizeof out);
        strlcpy(out.name, d->name, sizeof out.name);
        strlcpy(out.model, d->model, sizeof out.model);
        memcpy(out.mac, d->mac.addr, ETH_ALEN);
        out.link_up = d->link_up ? 1 : 0;
        out.configured = d->configured ? 1 : 0;
        out.link_speed_mbps = d->link_speed_mbps;
        out.ip = d->ip; out.netmask = d->netmask;
        out.gateway = d->gateway; out.dns = d->dns;
        out.rx_packets = d->rx_packets; out.tx_packets = d->tx_packets;
        out.rx_bytes = d->rx_bytes;     out.tx_bytes = d->tx_bytes;
        out.rx_dropped = d->rx_dropped; out.tx_dropped = d->tx_dropped;
        out.rx_errors = d->rx_errors;   out.tx_errors = d->tx_errors;
        memcpy((void *)out_ptr, &out, sizeof out);
        return 0;
    }
    return -E_NOENT;
}

static s64 enum_arp(u32 index, u64 out_ptr) {
    if (!user_range_ok(out_ptr, sizeof(karpentry_t), true)) return -E_INVAL;

    arp_entry_t table[32];
    int n = net_arp_snapshot(table, (int)ARRAY_LEN(table));
    if ((int)index >= n) return -E_NOENT;

    karpentry_t out;
    memset(&out, 0, sizeof out);
    out.ip = table[index].ip;
    memcpy(out.mac, table[index].mac.addr, ETH_ALEN);
    out.seen_ms = table[index].seen_ms;
    memcpy((void *)out_ptr, &out, sizeof out);
    return 0;
}

static s64 enum_wifi(u32 index, u64 out_ptr) {
    if (!user_range_ok(out_ptr, sizeof(kwifiinfo_t), true)) return -E_INVAL;

    u32 seen = 0;
    for (wifi_device_t *d = wifi_first(); d; d = d->next) {
        if (seen++ != index) continue;

        kwifiinfo_t out;
        memset(&out, 0, sizeof out);
        strlcpy(out.name, d->name, sizeof out.name);
        strlcpy(out.model, d->model, sizeof out.model);
        memcpy(out.mac, d->mac.addr, ETH_ALEN);
        out.radio_up = d->radio_up ? 1 : 0;
    out.unsupported_generation = d->unsupported_generation ? 1 : 0;
        out.enabled = d->enabled ? 1 : 0;
        out.firmware_needed = d->firmware_needed ? 1 : 0;
        out.firmware_present = d->firmware_present ? 1 : 0;
        out.state = (u8)d->state;
        out.security = (u8)d->security;
        out.channel = d->channel;
        out.signal_dbm = d->signal_dbm;
        strlcpy(out.ssid, d->ssid, sizeof out.ssid);
        strlcpy(out.firmware_name, d->firmware_name, sizeof out.firmware_name);
        strlcpy(out.vendor, wifi_vendor_name(d->vendor), sizeof out.vendor);
        memcpy((void *)out_ptr, &out, sizeof out);
        return 0;
    }
    return -E_NOENT;
}

static s64 enum_scan(u32 index, u64 out_ptr) {
    if (!user_range_ok(out_ptr, sizeof(kwifinet_t), true)) return -E_INVAL;

    wifi_device_t *d = wifi_first();
    if (!d) return -E_NODEV;
    if ((int)index >= d->scan_count) return -E_NOENT;

    kwifinet_t out;
    memset(&out, 0, sizeof out);
    strlcpy(out.ssid, d->scan[index].ssid, sizeof out.ssid);
    memcpy(out.bssid, d->scan[index].bssid.addr, ETH_ALEN);
    out.channel = d->scan[index].channel;
    out.signal_dbm = d->scan[index].signal_dbm;
    out.security = (u8)d->scan[index].security;
    out.hidden = d->scan[index].hidden ? 1 : 0;
    memcpy((void *)out_ptr, &out, sizeof out);
    return 0;
}

static s64 enum_firmware(u32 index, u64 out_ptr) {
    if (!user_range_ok(out_ptr, sizeof(kfirmware_t), true)) return -E_INVAL;

    firmware_need_t table[16];
    int n = firmware_snapshot(table, (int)ARRAY_LEN(table));
    if ((int)index >= n) return -E_NOENT;

    kfirmware_t out;
    memset(&out, 0, sizeof out);
    strlcpy(out.name, table[index].name, sizeof out.name);
    strlcpy(out.source, table[index].source, sizeof out.source);
    out.present = table[index].present ? 1 : 0;
    out.size = table[index].size;
    memcpy((void *)out_ptr, &out, sizeof out);
    return 0;
}

static s64 enum_audio(u32 index, u64 out_ptr) {
    if (!user_range_ok(out_ptr, sizeof(kaudioinfo_t), true)) return -E_INVAL;

    /* Index zero is the machine's own audio; anything after it is a USB device.
     *
     * These used to be invisible.  The driver reads a headset's alternate
     * settings, picks the best of them, opens the endpoint and feeds it - and
     * nothing could ask what it found, because this returned only the built-in
     * codec and refused every other index.  A headset that plays at
     * twenty-four bits and records at thirty-two was being driven correctly
     * and reported nowhere.
     */
    if (index > 0) {
        char name[48];
        u32 out_ch = 0, out_bits = 0, in_ch = 0, in_bits = 0;

        if (!usbaudio_get((int)index - 1, name, sizeof name,
                          &out_ch, &out_bits, &in_ch, &in_bits))
            return -E_NOENT;

        kaudioinfo_t u;
        memset(&u, 0, sizeof u);
        u.present = 1;

        u.output_ready = out_bits ? 1 : 0;
        u.channels = (u8)out_ch;
        u.bits = (u8)out_bits;

        u.input_ready    = in_bits ? 1 : 0;
        u.input_bits     = (u8)in_bits;
        u.input_channels = (u8)in_ch;

        strlcpy(u.controller, "USB", sizeof u.controller);
        strlcpy(u.codec, name, sizeof u.codec);
        strlcpy(u.note, "over USB; the rate follows what the device asks for "
                        "rather than being set here",
                sizeof u.note);

        memcpy((void *)out_ptr, &u, sizeof u);
        return 0;
    }

    hda_info_t h;
    hda_get_info(&h);

    kaudioinfo_t out;
    memset(&out, 0, sizeof out);
    out.pci_vendor = h.pci_vendor;
    out.pci_device = h.pci_device;
    out.codec_id = h.codec_vendor_id;
    out.sample_rate = h.sample_rate;
    out.present = h.present ? 1 : 0;
    out.output_ready = h.output_ready ? 1 : 0;
    out.channels = h.channels;
    out.bits = h.bits;
    out.dac_node = h.dac_node;
    out.pin_node = h.pin_node;

    out.input_ready    = h.input_ready ? 1 : 0;
    out.input_rate     = h.input_rate;
    out.input_bits     = h.input_bits;
    out.input_channels = h.input_channels;
    out.adc_node       = h.adc_node;
    out.in_pin_node    = h.in_pin_node;
    strlcpy(out.controller, h.controller, sizeof out.controller);
    strlcpy(out.codec, h.codec, sizeof out.codec);
    strlcpy(out.note, h.note, sizeof out.note);
    memcpy((void *)out_ptr, &out, sizeof out);
    return 0;
}

static s64 enum_usb(u32 index, u64 out_ptr) {
    if (!user_range_ok(out_ptr, sizeof(kusbinfo_t), true)) return -E_INVAL;

    usb_devinfo_t devs[16];
    int n = usb_snapshot(devs, (int)ARRAY_LEN(devs));
    if ((int)index >= n) return -E_NOENT;

    kusbinfo_t info;
    memset(&info, 0, sizeof info);
    info.port = devs[index].port;
    info.slot = devs[index].slot;
    info.speed = devs[index].speed;
    info.vendor = devs[index].vendor;
    info.product = devs[index].product;
    strlcpy(info.name, devs[index].name, sizeof info.name);
    strlcpy(info.driver, devs[index].driver, sizeof info.driver);
    memcpy((void *)out_ptr, &info, sizeof info);
    return 0;
}

/* Which process is drawing on the screen, so the console knows when to take it
 * back.  Zero means the console still owns it. */
int fb_owner_pid;

/* Snapshot user metadata once: another user thread must not change geometry
 * after validation but before a native copy or parallel CPU band consumes it. */
static s64 sys_fb_present(u32 caller, u64 pointer) {
    if ((u32)fb_owner_pid != caller) return -E_PERM;
    kpresent_t q;
    if (!user_copy(&q, pointer, sizeof q, false)) return -E_INVAL;
    if (q.w < 0 || q.h < 0 || q.x < 0 || q.y < 0) return -E_INVAL;
    if (!q.w || !q.h) return 0;

    u32 fw=0,fh=0,fpitch=0;
    u64 fbase=console_framebuffer(&fw,&fh,&fpitch);
    if (!fbase) return -E_NODEV;
    u64 right=(u64)(u32)q.x+(u32)q.w, bottom=(u64)(u32)q.y+(u32)q.h;
    /* Keep the signed row indices used by present_band representable too. */
    if (right>fw || bottom>fh || right>0x7fffffffull || bottom>0x7fffffffull ||
        (u64)fw*4>fpitch || (fpitch&3u) || q.stride<right) return -E_INVAL;
    /* Exact last accessed pixel, not a full unused final stride. Divide before
     * multiplying so hostile dimensions/strides cannot wrap the byte length. */
    if (bottom-1>(~0ull/4-right)/q.stride) return -E_INVAL;
    u64 end=((bottom-1)*q.stride+right)*4;
    u64 start=((u64)(u32)q.y*q.stride+(u32)q.x)*4;
    if(q.back>~0ull-start)return -E_INVAL;
    u64 first=q.back+start,need=end-start;
    /* Validate the damaged span, not every page above it in a full desktop.
     * A tiny bottom-row repaint must not walk megabytes of unrelated pages. */
    if (!user_range_ok(first,need,false)) return -E_INVAL;

    if (nvkms_kapi_runtime_selected())
        return nvkms_kapi_runtime_present(
            (const u32 *)(uintptr_t)first,
            (u32)q.w,(u32)q.h,q.stride,q.x,q.y) ? 0 : -E_IO;
    present_job_t job={
        .back=(const u32 *)(uintptr_t)q.back,.front=(u32 *)(uintptr_t)fbase,
        .back_stride=q.stride,.front_stride=fpitch/4,
        .x=q.x,.y=q.y,.w=q.w,.h=q.h,
    };
    int pieces=smp_worker_count()+1;
    if(pieces>q.h)pieces=q.h;
    if(pieces<1)pieces=1;
    smp_run_in(present_band,&job,pieces,read_cr3());
    return 0;
}

static s64 sys_fb_displaymode(u32 caller,u64 mode) {
    if (fb_owner_pid && (u32)fb_owner_pid!=caller) return -E_PERM;
    if (mode>3 && (mode<16 || mode>=16+KDISPLAY_MAX_OUTPUTS)) return -E_INVAL;
    if (nvkms_kapi_runtime_selected()) {
        if(mode>=16 || mode==3){
            kdisplay_output_t output;
            if(!nvkms_kapi_display_output(mode==3?1:(u32)mode-16,&output))return -E_NOENT;
            u32 generation=output.reserved;
            for(u32 i=0;i<KDISPLAY_MAX_OUTPUTS;i++){
                if(!nvkms_kapi_display_output(i,&output))break;
                if(output.reserved!=generation || !(output.flags&KDISPLAY_CONNECTED) ||
                   (output.flags&(KDISPLAY_STALE|KDISPLAY_DETECTED_ONLY)))return -E_BUSY;
            }
        }
        /* Not an Apply or persistence operation yet. The caller must save its
         * startup preference explicitly until native transactions are wired. */
        return 1;
    }
    if(!svga3d_second_attached())return -E_NOSYS;
    if(mode>=16){
        u32 output=(u32)mode-16;
        if(output>1)return -E_NOENT;
        mode=output?3:0;
    }
    if(mode==1 || svga3d_extend_active())return 1;
    svga3d_set_second_mode((int)mode);
    return 0;
}

/* Read-only preflight; snapshot caller data under the same transaction guard
 * used by presentation and eventual configuration changes. */
static s64 sys_fb_check_configuration(u32 caller,u64 address,u64 output) {
    if(fb_owner_pid!=caller)return -E_PERM;
    if(!nvkms_kapi_runtime_selected())return -E_NOSYS;
    if(!nv_render_try_begin())return -E_BUSY;
    s64 result=-E_INVAL;
    kdisplay_configuration_t request;
    kdisplay_configuration_check_t answer;
    if(!user_range_ok(address,sizeof request,false))goto done;
    memcpy(&request,(const void*)address,sizeof request);
    if(!user_range_ok(output,sizeof answer,true))goto done;
    result=nvkms_kapi_check_configuration(&request,&answer);
    if(!result)memcpy((void*)output,&answer,sizeof answer);
done:
    nv_render_end();return result;
}

/* Prepare/cancel does not publish or map replacement display storage. Tokens
 * use the nonrecycled address-space identity, never the reusable PID alone. */
static s64 sys_fb_prepare_configuration(proc_t *p,u64 operation,u64 address,u64 output) {
    proc_t *m=proc_shared(p);
    if(fb_owner_pid!=m->pid || !m->gpu_owner_id || m->resource_closing)return -E_PERM;
    if(!nvkms_kapi_runtime_selected())return -E_NOSYS;
    if(!nv_render_try_begin())return -E_BUSY;
    s64 result=-E_INVAL;
    if(operation==FB_CANCEL_CONFIGURATION){
        result=nvkms_kapi_cancel_configuration(m->gpu_owner_id,address);
    }else if(operation==FB_PREPARE_CONFIGURATION){
        kdisplay_configuration_t request;
        kdisplay_prepared_configuration_t answer;
        /* Mapping validation and the copy itself share the VMM lock. The
         * longer KAPI work uses only this immutable kernel snapshot. */
        bool valid=user_copy(&request,address,sizeof request,false);
        if(valid)valid=user_range_ok(output,sizeof answer,true);
        if(!valid)goto done;
        result=nvkms_kapi_prepare_configuration(m->gpu_owner_id,&request,&answer);
        if(!result){
            /* KAPI allocation/validation may sleep. The sibling may have
             * unmapped the result in that interval: recheck, and cancel a
             * candidate whose token cannot be returned instead of leaking it. */
            valid=user_copy(&answer,output,sizeof answer,true);
            if(!valid){
                nvkms_kapi_cancel_configuration(m->gpu_owner_id,answer.token);
                result=-E_INVAL;
            }
        }
    }
done:
    nv_render_end();return result;
}

#include "process_memory.h"

static s64 sys_fb_output_mode(proc_t *p,u64 operation,u64 address) {
    proc_t *m=proc_shared(p);
    if(fb_owner_pid!=m->pid || !m->gpu_owner_id || m->resource_closing)return -E_PERM;
    if(!nvkms_kapi_runtime_selected())return -E_NOSYS;
    if(!nv_render_try_begin())return -E_BUSY;
    s64 result=-E_INVAL;
    if(operation==FB_APPLY_OUTPUT_MODE){
        kdisplay_output_mode_request_t request;
        if(user_copy(&request,address,sizeof request,false))
            result=nvkms_kapi_apply_output_mode(m->gpu_owner_id,&request);
    }else if(operation==FB_CONFIRM_OUTPUT_MODE || operation==FB_REVERT_OUTPUT_MODE)
        result=nvkms_kapi_finish_output_mode(m->gpu_owner_id,address,operation==FB_CONFIRM_OUTPUT_MODE);
    nv_render_end();return result;
}

/* Build the complete replacement before retiring this address space's old
 * aliases. The render guard serializes framebuffer acquisitions and pins the
 * caller; the address-space metadata mutex also excludes sibling mmap/unmap.
 * Display storage remains kernel-owned even while mapped writable by a user. */
static s64 framebuffer_map_locked(proc_t *p, u64 address) {
    proc_t *m = proc_shared(p);
    if (!user_range_ok(address, sizeof(kframebuffer_t), true)) return -E_INVAL;
    kboot_framebuffer fb = g_boot.fb;
    if (!fb.base || !fb.size) return -E_NODEV;
    if (fb_owner_pid && fb_owner_pid != m->pid) {
        if (proc_pid_alive(fb_owner_pid))
            return -E_BUSY;
    }

    u64 offset = fb.base & PAGE_MASK;
    if (!fb.width || !fb.height || fb.bpp != 32 ||
        (u64)fb.width * 4 > fb.pitch || (u64)fb.pitch * fb.height > fb.size ||
        fb.size > 0xffffffffULL - PAGE_MASK - offset ||
        fb.base > PTE_ADDR + PAGE_SIZE - fb.size) return -E_INVAL;
    u64 phys = PAGE_ALIGN_DOWN(fb.base);
    u64 span = PAGE_ALIGN_UP(fb.size + offset);
    u64 va = m->mmap_next;
    if (va < USER_MMAP_BASE || (va & PAGE_MASK) || va >= USER_TSTACK_BASE ||
        span > USER_TSTACK_BASE - va - PAGE_SIZE) return -E_NOMEM;

    /* The result cannot live in a mapping that this call is about to retire. */
    if (m->fb_mapped_va && address < m->fb_mapped_va + m->fb_mapped_span &&
        address + sizeof(kframebuffer_t) > m->fb_mapped_va) return -E_INVAL;
    for (u64 off = 0; off < span; off += PAGE_SIZE)
        if (vmm_is_mapped(p->pml4, va + off)) return -E_BUSY;

    /* Native shadow RAM must use WB, matching the kernel alias. Legacy VRAM
     * retains the existing PWT attribute. Never mix WB/PWT aliases of RAM. */
    u64 flags = PTE_W | PTE_U | PTE_NX | PTE_BORROWED;
    if (!nvkms_kapi_runtime_selected()) flags |= PTE_PWT;
    for (u64 off = 0; off < span; off += PAGE_SIZE) {
        if (!vmm_map(p->pml4, va + off, phys + off, flags)) {
            for (u64 undo = 0; undo < off; undo += PAGE_SIZE)
                vmm_unmap_borrowed_page(p->pml4, va + undo, phys + undo);
            return -E_NOMEM;
        }
    }

    kframebuffer_t info;
    memset(&info, 0, sizeof info);
    info.address = va + offset;
    info.width = fb.width; info.height = fb.height; info.pitch = fb.pitch;
    info.bpp = fb.bpp;
    info.red_shift = fb.red_shift; info.green_shift = fb.green_shift;
    info.blue_shift = fb.blue_shift;
    info.size = (u32)fb.size;
    if (!user_copy(&info, address, sizeof info, true)) {
        for (u64 undo = 0; undo < span; undo += PAGE_SIZE)
            vmm_unmap_borrowed_page(p->pml4, va + undo, phys + undo);
        return -E_INVAL;
    }
    for (u64 off = 0; off < m->fb_mapped_span; off += PAGE_SIZE)
        vmm_unmap_borrowed_page(p->pml4, m->fb_mapped_va + off,
                               m->fb_mapped_phys + off);
    m->fb_mapped_va = va; m->fb_mapped_span = span; m->fb_mapped_phys = phys;
    m->mmap_next = va + span + PAGE_SIZE;
    console_release_framebuffer();
    fb_owner_pid = m->pid;
    kinfo("fb", "framebuffer mapped into pid %d at %p (%ux%u)",
          p->pid, (void *)info.address, info.width, info.height);
    return 0;
}

static s64 framebuffer_map(proc_t *p, u64 address) {
    proc_t *token;
    if (p != proc_current() || !proc_vm_begin(&token)) return -E_BUSY;
    /* Metadata -> render is the only ordering. A failed try never sleeps
     * holding render state, and no GPU wait occurs in this mapping operation. */
    if (!nv_render_try_begin()) { proc_vm_end(token); return -E_BUSY; }
    s64 result = framebuffer_map_locked(p, address);
    nv_render_end();
    proc_vm_end(token);
    return result;
}

static void gpu_surface_reaper(void *unused) {
    (void)unused;
    for (;;) {
        sched_sleep_ms(100);
        if (nv_render_try_begin()) {
            nv_surface_reap_exited();
            nv_render_end();
        }
    }
}

/* The transaction pins the calling thread before touching user memory and
 * keeps handles, page tables, command storage and scanout binding serialized
 * until the GPU fence retires. Never submit a userspace pointer to the GPU. */
/* A process pin prevents exit, not sibling unmap/protection changes. Validate
 * and copy under the cross-CPU VM lock per chunk, including AFTER device waits.
 * NULL buffer means range preflight only. The GPU receives kernel snapshots,
 * never these user mappings. Failed multi-chunk output may have copied a prefix;
 * callers return an error and must not advertise complete output in that case.
 * This protects each copy, not mappings across a device wait or mutable user
 * contents across multiple chunks. GPU inputs become private kernel snapshots. */
static bool gpu_user_access(void *buffer, u64 address, size_t bytes, bool write) {
    if (bytes > (~(u64)0) - address) return false;
    u8 *data = buffer;
    while (bytes) {
        size_t chunk = bytes > VMM_USER_COPY_MAX ? VMM_USER_COPY_MAX : bytes;
        bool valid = data ? user_copy(data, address, chunk, write) :
                            user_range_ok(address, chunk, write);
        if (!valid) return false;
        address += chunk;
        if (data) data += chunk;
        bytes -= chunk;
    }
    return true;
}

/* The legacy direct-draw ABI is small enough for one VM-locked copy. Never
 * retain the user's mapping across adapter dispatch: a native driver can sleep
 * while acquiring its render transaction or waiting for earlier GPU work. */
static s64 gpu_triangle_request(u64 address, u64 count) {
    if (!count || count > 128u) return -E_INVAL;
    _Static_assert(128u * 3u * sizeof(kvertex_t) <= VMM_USER_COPY_MAX,
                   "direct triangles must fit in one bounded user copy");
    size_t bytes = (size_t)count * 3u * sizeof(kvertex_t);
    if (!user_range_ok(address, bytes, false)) return -E_INVAL;
    if (!gpu_accel_draw_mode()) return -E_NOSYS;
    float *snapshot = kmalloc(bytes);
    if (!snapshot) return -E_NOMEM;
    if (!user_copy(snapshot, address, bytes, false)) {
        kfree(snapshot);
        return -E_INVAL;
    }
    int drew = gpu_accel_draw_triangles(snapshot, (u32)count);
    kfree(snapshot);
    return drew < 0 ? -E_NOSYS : drew;
}

/* Legacy SVGA-DX programs are not native instructions for other adapters.
 * Copy both bytecode streams before entering a backend that may block. Counts
 * and signature arrays come from the same bounded metadata snapshot. */
static s64 gpu_shaders_request(u64 address) {
    kshaders_t req;
    _Static_assert(sizeof(req) <= VMM_USER_COPY_MAX && 512u*4u <= VMM_USER_COPY_MAX,
                   "shader metadata/code must fit bounded user copies");
    if (!user_copy(&req, address, sizeof req, false)) return -E_INVAL;
    if (!req.vertex_words || !req.pixel_words ||
        req.vertex_words > 512u || req.pixel_words > 512u ||
        req.vertex_takes_count > KSIG_MAX || req.vertex_gives_count > KSIG_MAX ||
        req.pixel_takes_count > KSIG_MAX || req.pixel_gives_count > KSIG_MAX)
        return -E_INVAL;
    size_t vs_bytes = (size_t)req.vertex_words*sizeof(u32);
    size_t ps_bytes = (size_t)req.pixel_words*sizeof(u32);
    u32 *code = kmalloc(vs_bytes+ps_bytes);
    if (!code) return -E_NOMEM;
    u32 *pixel = code+req.vertex_words;
    if (!user_copy(code, (u64)(uintptr_t)req.vertex, vs_bytes, false) ||
        !user_copy(pixel, (u64)(uintptr_t)req.pixel, ps_bytes, false)) {
        kfree(code);
        return -E_INVAL;
    }
    const gpu_shader_program_t vs = {
        .code=code, .words=req.vertex_words,
        .inputs=(const u32 *)req.vertex_takes, .input_count=req.vertex_takes_count,
        .outputs=(const u32 *)req.vertex_gives, .output_count=req.vertex_gives_count};
    const gpu_shader_program_t ps = {
        .code=pixel, .words=req.pixel_words,
        .inputs=(const u32 *)req.pixel_takes, .input_count=req.pixel_takes_count,
        .outputs=(const u32 *)req.pixel_gives, .output_count=req.pixel_gives_count};
    int result = gpu_accel_set_shaders(GPU_PROGRAM_SVGA_DX, &vs, &ps);
    kfree(code);
    return result < 0 ? -E_NOSYS : 0;
}

static s64 gpu_layout_request(u64 address) {
    klayout_t req;
    _Static_assert(sizeof(req) <= VMM_USER_COPY_MAX, "layout must fit bounded user copy");
    if (!user_copy(&req, address, sizeof req, false)) return -E_INVAL;
    if (!req.count || req.count > KLAYOUT_MAX || !req.stride) return -E_INVAL;
    int result = gpu_accel_set_layout(GPU_PROGRAM_SVGA_DX, (const u32 *)req.elements,
                                      req.count, req.stride);
    return result < 0 ? -E_NOSYS : 0;
}

static s64 gpu_surface_request(proc_t *p, u64 address, u64 size) {
    if (size != sizeof(kg2d_request_t)) return -E_INVAL;
    if (!nvkms_kapi_runtime_selected()) return -E_NOSYS;
    if (!nv_render_try_begin()) return -E_BUSY;
    s64 result = -E_INVAL;
    void *snapshot = NULL;
    kg2d_request_t r;
    if (!gpu_user_access(&r, address, sizeof r, false)) goto done;
    if (r.version != KG2D_ABI || r.operation > KG2D_SHADER_GEOMETRY) goto done;
    u64 owner = proc_shared(p)->gpu_owner_id;
    if (!owner || proc_shared(p)->resource_closing) goto done;
    switch (r.operation) {
    case KG2D_CREATE: {
        if (!gpu_user_access(NULL, address, sizeof r, true)) break;
        static bool reaper_started;
        if (!reaper_started) {
            if (kthread_create("gpu-surface-reaper", gpu_surface_reaper, NULL) < 0) {
                result = -E_NOMEM; break;
            }
            reaper_started = true;
        }
        nv_surface_reap_exited();
        bool pool_exhausted = false;
        r.handle = nv_surface_create_with_pressure(owner, r.width, r.height,
                                                  &pool_exhausted);
        if (!r.handle) { result = pool_exhausted ? -E_NOMEM : -E_IO; break; }
        nv_surface_info_t info;
        if (!nv_surface_info(owner, r.handle, &info)) {
            nv_surface_destroy(owner, r.handle); result = -E_IO; break;
        }
        r.pitch = info.pitch; r.bytes = info.bytes;
        if (!gpu_user_access(&r, address, sizeof r, true)) {
            nv_surface_destroy(owner, r.handle);
            break;
        }
        result = 0;
        break;
    }
    case KG2D_DESTROY:
        result = nv_surface_destroy(owner, r.handle) ? 0 : -E_IO;
        break;
    case KG2D_UPLOAD:
    case KG2D_DOWNLOAD: {
        bool readback = r.operation == KG2D_DOWNLOAD;
        if (!r.bytes || r.bytes > KG2D_TRANSFER_MAX ||
            ((r.offset | r.bytes) & 3u) ||
            !gpu_user_access(NULL, r.data, r.bytes, readback)) break;
        snapshot = kmalloc(r.bytes);
        if (!snapshot) { result = -E_NOMEM; break; }
        if (!readback && !gpu_user_access(snapshot, r.data, r.bytes, false)) break;
        result = nv_surface_transfer(owner, r.handle, r.offset, snapshot,
                                     (u32)r.bytes, readback) ? 0 : -E_IO;
        if (!result && readback && !gpu_user_access(snapshot, r.data, r.bytes, true))
            result = -E_INVAL;
        break;
    }
    case KG2D_DRAW: {
        if (!r.count || r.count > KG2D_MAX_COMMANDS) break;
        size_t bytes = r.count * sizeof(kg2d_command_t);
        if (!gpu_user_access(NULL, r.data, bytes, false)) break;
        snapshot = kmalloc(bytes);
        if (!snapshot) { result = -E_NOMEM; break; }
        if (!gpu_user_access(snapshot, r.data, bytes, false)) break;
        result = nv_surface_draw(owner, r.handle, r.source, snapshot, r.count,
                                  r.x, r.y, r.width, r.height) ? 0 : -E_IO;
        break;
    }
    case KG2D_DRAW3D: {
        if (!r.count || r.count > KG3D_MAX_COMMANDS) break;
        size_t bytes = r.count * sizeof(kg3d_command_t);
        if (!gpu_user_access(NULL, r.data, bytes, false)) break;
        snapshot = kmalloc(bytes);
        if (!snapshot) { result = -E_NOMEM; break; }
        if (!gpu_user_access(snapshot, r.data, bytes, false)) break;
        result = nv_surface_draw3d(owner, r.handle, r.offset, r.source,
                                    snapshot, r.count, r.x, r.y,
                                    r.width, r.height) ? 0 : -E_IO;
        break;
    }
    case KG2D_SHADER_VM: {
        if (r.bytes != sizeof(ksh_dispatch_t) ||
            !gpu_user_access(NULL, r.data, sizeof(ksh_dispatch_t), false)) break;
        snapshot = kmalloc(sizeof(ksh_dispatch_t));
        if (!snapshot) { result = -E_NOMEM; break; }
        if (!gpu_user_access(snapshot, r.data, sizeof(ksh_dispatch_t), false)) break;
        result = nv_surface_shader(owner, r.handle, r.offset, r.source, snapshot) ? 0 : -E_IO;
        break;
    }
    case KG2D_SHADER_RASTER: {
        if (r.bytes != sizeof(kshr_submission_t) ||
            !gpu_user_access(NULL, r.data, sizeof(kshr_submission_t), false)) break;
        snapshot = kmalloc(sizeof(kshr_submission_t));
        if (!snapshot) { result = -E_NOMEM; break; }
        if (!gpu_user_access(snapshot, r.data, sizeof(kshr_submission_t), false)) break;
        result = nv_surface_shader_raster(owner, r.handle, r.offset, r.source, snapshot) ? 0 : -E_IO;
        break;
    }
    case KG2D_SHADER_GEOMETRY: {
        if (r.bytes != sizeof(kshs_submission_t) ||
            !gpu_user_access(NULL, address, sizeof r, true) ||
            !gpu_user_access(NULL, r.data, sizeof(kshs_submission_t), false)) break;
        snapshot = kmalloc(sizeof(kshs_submission_t));
        if (!snapshot) { result = -E_NOMEM; break; }
        if (!gpu_user_access(snapshot, r.data, sizeof(kshs_submission_t), false)) break;
        u32 emitted = 0;
        u64 executed = 0;
        result = nv_surface_shader_geometry(owner, r.handle, r.offset, r.source,
                                            snapshot, &emitted, &executed) ? 0 : -E_IO;
        if (!result) {
            r.count = emitted;
            r.bytes = executed;
            /* The mapping can change while the GPU waits. Revalidate copyout;
             * a copyout error must not cause userland to replay this draw. */
            if (!gpu_user_access(&r, address, sizeof r, true)) result = -E_INVAL;
        }
        break;
    }
    case KG2D_PRESENT:
        if (fb_owner_pid != proc_shared(p)->pid) { result = -E_PERM; break; }
        result = nvkms_kapi_runtime_present_surface(owner, r.handle,
                       r.x, r.y, r.width, r.height) ? 0 : -E_IO;
        break;
    }
done:
    if (snapshot) kfree(snapshot);
    nv_render_end();
    return result;
}

/* A video call owns immutable kernel input and private output until the codec
 * fence and error checks retire. Only complete output is copied to userland.
 * The same transaction as graphics serializes RM/VMM and pins the address space. */
static s64 gpu_video_request(proc_t *p, u64 address, u64 size) {
    if (size != sizeof(kvideo_request_t)) return -E_INVAL;
    if (!nv_render_try_begin()) return -E_BUSY;
    s64 result = -E_INVAL;
    u8 *input = NULL, *output = NULL;
    kvideo_request_t r;
    if (!gpu_user_access(NULL, address, sizeof r, true) ||
        !gpu_user_access(&r, address, sizeof r, false)) goto done;
    if (r.version != KVIDEO_ABI || r.operation > KVIDEO_H264_ENCODE_IDR || r.reserved ||
        !proc_shared(p)->gpu_owner_id || proc_shared(p)->resource_closing) goto done;
    u32 operation = r.operation;
    bool encode=operation==KVIDEO_H264_ENCODE_PLAN || operation==KVIDEO_H264_ENCODE_IDR;
    r = (kvideo_request_t){.version=KVIDEO_ABI, .operation=operation,
        .input=r.input, .input_bytes=r.input_bytes, .output=r.output,
        .output_capacity=r.output_capacity, .phase=KVIDEO_PHASE_HEADERS,
        .coded_width=encode?r.coded_width:0, .coded_height=encode?r.coded_height:0};
    if (encode) {
        r.operation=KVIDEO_H264_ENCODE_PLAN;
        result=nv_nvenc_encode_idr(NULL,0,NULL,0,&r);
        r.operation=operation;
        if (result || operation==KVIDEO_H264_ENCODE_PLAN) goto publish;
        result=-E_INVAL;
        if (r.input_bytes!=KVIDEO_ENCODE_INPUT_BYTES) goto publish;
    }
    if (!r.input_bytes || r.input_bytes > KVIDEO_INPUT_MAX ||
        !gpu_user_access(NULL, r.input, r.input_bytes, false)) goto publish;
    input = kmalloc((size_t)r.input_bytes);
    if (!input) { r.phase=KVIDEO_PHASE_ALLOCATE; result=-E_NOMEM; goto publish; }
    if (!gpu_user_access(input, r.input, (size_t)r.input_bytes, false)) goto publish;
    if (!encode) {
        r.operation = KVIDEO_H264_INSPECT;
        result = nv_nvdec_decode_idr(input, (u32)r.input_bytes, NULL, 0, &r);
        r.operation = operation;
        if (result || operation == KVIDEO_H264_INSPECT) goto publish;
    }
    r.phase = KVIDEO_PHASE_LAYOUT;
    result = -E_INVAL;
    if (!r.required_bytes || r.required_bytes > KVIDEO_OUTPUT_MAX ||
        r.output_capacity < r.required_bytes ||
        !gpu_user_access(NULL, r.output, r.required_bytes, true)) goto publish;
    /* Pixel/bitstream output must not overwrite its own returned diagnostics.
     * Both additions are safe after validated user ranges. Input overlap is
     * harmless: the complete input was already snapshotted. */
    if (r.output < address + sizeof r && address < r.output + r.required_bytes) goto publish;
    u32 output_bytes = r.required_bytes;
    output = kmalloc(output_bytes);
    if (!output) { r.phase=KVIDEO_PHASE_ALLOCATE; result=-E_NOMEM; goto publish; }
    result = encode ? nv_nvenc_encode_idr(input,(u32)r.input_bytes,output,output_bytes,&r) :
                      nv_nvdec_decode_idr(input,(u32)r.input_bytes,output,output_bytes,&r);
    if (!result) {
        if (r.phase != KVIDEO_PHASE_COMPLETE || !r.written_bytes ||
            r.written_bytes > output_bytes || (!encode && r.written_bytes != output_bytes) ||
            r.required_bytes != output_bytes) {
            r.written_bytes = 0; result = -E_IO;
        } else if (!gpu_user_access(output, r.output, r.written_bytes, true)) {
            r.written_bytes = 0; result = -E_INVAL;
        }
    } else r.written_bytes = 0;
publish:
    if (!gpu_user_access(&r, address, sizeof r, true)) result = -E_INVAL;
done:
    if (output) kfree(output);
    if (input) kfree(input);
    nv_render_end();
    return result;
}

/* ------------------------------------------------------------------------- */
/* EFI runtime services                                                      */
/* ------------------------------------------------------------------------- */

/* The firmware's runtime services would still be reachable - the loader never
 * called SetVirtualAddressMap, so they sit at their original addresses inside
 * the identity map - but writing a Boot#### entry means constructing a device
 * path the firmware will accept and getting the NVRAM update exactly right.
 * The installer instead writes the removable-media path \EFI\BOOT\BOOTX64.EFI,
 * which every UEFI implementation boots without an NVRAM entry, and the Windows
 * side uses bcdedit where a firmware entry is genuinely wanted. */
static s64 efivar_op(u32 op, u64 arg) {
    (void)arg;
    if (op == EFIVAR_AVAILABLE) return 0;
    return -E_NOSYS;
}

/* ------------------------------------------------------------------------- */
/* dispatch                                                                  */
/* ------------------------------------------------------------------------- */

/* Reading the event log, and connecting securely, both need buffers far larger
 * than anything else a system call uses.  They live in functions of their own
 * rather than in the dispatcher, because the dispatcher is one switch over
 * every call there is and the compiler gives its frame room for the largest
 * local in any branch - all of them at once, on every call, however small the
 * call actually is.  Sixty-four log records is thirteen kilobytes; on a
 * thirty-two kilobyte kernel stack that leaves too little for the filesystem
 * and disk code underneath a write, and the overflow lands on the saved
 * registers of the process that made the call, which then resumes at address
 * zero.  Keeping the dispatcher's own frame small is what stops that. */
static s64 log_read_op(u64 from_seq, u64 out_addr, u64 max_in) {
    int max = (int)max_in;
    if (max <= 0 || max > 64) return -E_INVAL;
    if (!user_range_ok(out_addr, (size_t)max * sizeof(klog_record_t), true))
        return -E_INVAL;

    klog_record_t *out = (klog_record_t *)out_addr;
    int done = 0;

    /* Sixteen at a time.  Reading all sixty-four in one go would need thirteen
     * kilobytes of stack, and this runs on the same stack the filesystem and
     * disk code below it use. */
    while (done < max) {
        klog_entry batch[16];
        int want = max - done;
        if (want > 16) want = 16;

        int n = klog_read(from_seq + (u64)done, batch, want);
        if (n <= 0) break;

        for (int i = 0; i < n; i++) {
            out[done + i].seq = batch[i].seq;
            out[done + i].time_ms = batch[i].time_ms;
            out[done + i].level = (u32)batch[i].level;
            out[done + i].pad = 0;
            strlcpy(out[done + i].subsys, batch[i].subsys,
                    sizeof out[done + i].subsys);
            strlcpy(out[done + i].msg, batch[i].msg, sizeof out[done + i].msg);
        }
        done += n;
        if (n < want) break;
    }
    return done;
}

static s64 tls_connect_op(u64 args, u64 host_addr) {
    if (!user_range_ok(args, sizeof(u32) * 3, false)) return -E_INVAL;
    const u32 *in = (const u32 *)args;

    char host[256];
    host[0] = 0;
    if (host_addr && copy_user_string(host_addr, host, sizeof host) < 0)
        return -E_INVAL;

    const char *why = NULL;
    int handle = tls_connect(in[0], (u16)in[1], host[0] ? host : NULL,
                             (int)in[2], &why);
    if (handle < 0) {
        /* The reason a secure connection was refused is the most useful thing
         * this call produces when it fails, and it would be lost if only a
         * number came back. */
        if (why) kwarn("tls", "%s: %s", host[0] ? host : "connection", why);
        return -E_IO;
    }
    return handle | NET_TLS_HANDLE;
}

/* The two things the Linux translation needs from in here: the check that a
 * pointer a program handed over is really its own, and the file behind a
 * descriptor.  Both are the same checks a native call goes through - the
 * translation gets no shortcut around them, which is the point. */
static s64 dispatch(proc_t *p, u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4);
s64 linux_dispatch(proc_t *p, regs_t *r);

/* What the translation forwards a renumbered call into. */
s64 syscall_native(proc_t *p, u64 nr, u64 a0, u64 a1, u64 a2, u64 a3,
                   u64 a4) {
    return dispatch(p, nr, a0, a1, a2, a3, a4);
}

bool syscall_user_ok(u64 addr, size_t len, bool need_write) {
    return user_range_ok(addr, len, need_write);
}

bool syscall_user_copy(void *buffer, u64 address, size_t bytes, bool to_user) {
    return user_copy(buffer, address, bytes, to_user);
}

/* Internal metadata operation: pathname is user memory, result is a kernel
 * snapshot. Linux translation must not pass this result back through SYS_STAT,
 * whose output argument is user memory. Never weaken that syscall boundary. */
int syscall_stat_snapshot(proc_t *p, u64 path, kstat_t *out) {
    char raw[VFS_PATH_MAX], full[VFS_PATH_MAX];
    int r = copy_user_string(path, raw, sizeof raw);
    if (r < 0) return r;
    proc_resolve_path(p, raw, full, sizeof full);
    vstat_t st;
    r = vfs_stat(full, &st);
    if (r < 0) return r;
    *out = (kstat_t){ st.type, st.mode, st.size, st.mtime };
    return 0;
}

static s64 dispatch(proc_t *p, u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4) {
    switch (nr) {

    case SYS_EXIT:
        proc_exit((int)a0);
        return 0;

    case SYS_WRITE: {
        if (!user_range_ok(a1, (size_t)a2, false)) return -E_INVAL;
        file_t *f = proc_fd_acquire(p, (int)a0);
        if (!f) return -E_BADF;
        s64 result = p->is_kernel ? vfs_write(f, (const void *)a1, (size_t)a2)
                                 : vfs_user_io(f, p->pml4, a1, (size_t)a2, true);
        vfs_close(f);
        return result;
    }

    case SYS_READ: {
        if (!user_range_ok(a1, (size_t)a2, true)) return -E_INVAL;
        file_t *f = proc_fd_acquire(p, (int)a0);
        if (!f) return -E_BADF;
        s64 result = p->is_kernel ? vfs_read(f, (void *)a1, (size_t)a2)
                                 : vfs_user_io(f, p->pml4, a1, (size_t)a2, false);
        vfs_close(f);
        return result;
    }

    case SYS_OPEN:
        return do_open(p, a0, (u32)a1);

    case SYS_CLOSE:
        return proc_fd_close(p, (int)a0);

    case SYS_SEEK: {
        file_t *f = proc_fd_acquire(p, (int)a0);
        if (!f) return -E_BADF;
        s64 result = vfs_seek(f, (s64)a1, (int)a2);
        vfs_close(f);
        return result;
    }

    case SYS_STAT: {
        if (!user_range_ok(a1, sizeof(kstat_t), true)) return -E_INVAL;
        kstat_t out;
        int r = syscall_stat_snapshot(p, a0, &out);
        if (r < 0) return r;
        return user_copy(&out, a1, sizeof out, true) ? 0 : -E_INVAL;
    }

    case SYS_READDIR: {
        if (!user_range_ok(a2, sizeof(kdirent_t), true)) return -E_INVAL;
        file_t *f = proc_fd_acquire(p, (int)a0);
        if (!f) return -E_BADF;

        dirent_k d;
        int r = vfs_readdir(f, (u32)a1, &d);
        vfs_close(f);
        if (r < 0) return r;

        kdirent_t out;
        memset(&out, 0, sizeof out);
        strlcpy(out.name, d.name, sizeof out.name);
        out.type = d.type;
        out.size = d.size;
        return user_copy(&out, a2, sizeof out, true) ? 0 : -E_INVAL;
    }

    case SYS_MKDIR: {
        char raw[VFS_PATH_MAX], full[VFS_PATH_MAX];
        int r = copy_user_string(a0, raw, sizeof raw);
        if (r < 0) return r;
        proc_resolve_path(p, raw, full, sizeof full);
        return vfs_mkdir(full);
    }

    case SYS_UNLINK: {
        char raw[VFS_PATH_MAX], full[VFS_PATH_MAX];
        int r = copy_user_string(a0, raw, sizeof raw);
        if (r < 0) return r;
        proc_resolve_path(p, raw, full, sizeof full);
        return vfs_unlink(full);
    }

    case SYS_RENAME: {
        char raw_a[VFS_PATH_MAX], raw_b[VFS_PATH_MAX];
        char full_a[VFS_PATH_MAX], full_b[VFS_PATH_MAX];
        int r = copy_user_string(a0, raw_a, sizeof raw_a);
        if (r < 0) return r;
        r = copy_user_string(a1, raw_b, sizeof raw_b);
        if (r < 0) return r;
        proc_resolve_path(p, raw_a, full_a, sizeof full_a);
        proc_resolve_path(p, raw_b, full_b, sizeof full_b);
        return vfs_rename(full_a, full_b);
    }

    case SYS_TRUNCATE: {
        file_t *f = proc_fd_acquire(p, (int)a0);
        if (!f) return -E_BADF;
        int result = vfs_truncate(f, a1);
        vfs_close(f);
        return result;
    }

    case SYS_IOCTL: {
        file_t *f = proc_fd_acquire(p, (int)a0);
        if (!f) return -E_BADF;
        int result = p->is_kernel ? vfs_ioctl(f, (u32)a1, (void *)a2)
                                 : vfs_user_ioctl(f, p->pml4, (u32)a1, a2);
        vfs_close(f);
        return result;
    }

    case SYS_SPAWN: {
        char raw[VFS_PATH_MAX], full[VFS_PATH_MAX];
        int r = copy_user_string(a0, raw, sizeof raw);
        if (r < 0) return r;
        proc_resolve_path(p, raw, full, sizeof full);

        int argc = (int)a2;
        if (argc < 0 || argc > PROC_ARGS_MAX) return -E_INVAL;
        if (argc && !user_range_ok(a1, (size_t)argc * 8, false)) return -E_INVAL;

        char *argv_copy[PROC_ARGS_MAX];
        const char *argv[PROC_ARGS_MAX];
        int built = 0;
        for (int i = 0; i < argc; i++) {
            u64 ptr = ((const u64 *)a1)[i];
            argv_copy[i] = kmalloc(256);
            if (!argv_copy[i]) { r = -E_NOMEM; goto spawn_done; }
            built++;
            r = copy_user_string(ptr, argv_copy[i], 256);
            if (r < 0) goto spawn_done;
            argv[i] = argv_copy[i];
        }

        int pid = 0;
        r = proc_spawn(full, argv, argc, &pid);
        if (r == 0) r = pid;

    spawn_done:
        for (int i = 0; i < built; i++) kfree(argv_copy[i]);
        return r;
    }

    case SYS_EXEC:
        /* Replacing the running image would mean tearing down the address
         * space we are executing on; spawn plus wait covers what the shell
         * needs, so this is deliberately absent. */
        return -E_NOSYS;

    case SYS_WAIT: {
        int status = 0;
        int r = proc_wait((int)a0, &status);
        if (r >= 0 && a1) {
            if (!user_range_ok(a1, sizeof(int), true)) return -E_INVAL;
            *(int *)a1 = status;
        }
        return r;
    }

    case SYS_KILL:
        return proc_kill((int)a0, (int)a1);

    case SYS_SLEEP:
        sched_sleep_ms(a0);
        return 0;

    case SYS_YIELD:
        sched_yield();
        return 0;

    case SYS_THREAD:
        switch (a0) {
        case THREAD_CREATE: {
            /* The entry point has to be somewhere the process could already
             * jump to, and so does the argument if it is a pointer; nothing
             * else about them is the kernel's business. */
            if (!user_range_ok(a1, 1, false)) return -E_INVAL;
            int tid = 0;
            int r = proc_thread_create(a1, a2, &tid);
            return r < 0 ? r : tid;
        }
        case THREAD_EXIT:
            proc_thread_exit((int)a1);
            return 0;                       /* not reached */
        case THREAD_SETGS:
            return proc_thread_set_base(false, a1);
        case THREAD_GETTID:
            return p->pid;
        case THREAD_ONFAULT:
            if (a1 && !user_range_ok(a1, 1, false)) return -E_INVAL;
            proc_shared(p)->fault_handler = a1;
            return 0;
        case THREAD_HANDLED:
            /* The handler is about to jump back into the program, so this
             * fault is over.  Without this the nesting count only ever rises
             * and a program that survives four faults is killed on the
             * fifth. */
            if (p->fault_depth > 0) p->fault_depth--;
            return 0;
        default:
            return -E_INVAL;
        }

    case SYS_FUTEX:
        switch (a0) {
        case FUTEX_WAIT_OP:
            if (!user_range_ok(a1, 4, false)) return -E_INVAL;
            return futex_wait(a1, (u32)a2, (int)(s64)a3);
        case FUTEX_WAKE_OP:
            if (!user_range_ok(a1, 4, false)) return -E_INVAL;
            return futex_wake(a1, (int)a2);
        default:
            return -E_INVAL;
        }

    case SYS_GETPID:
        return p->pid;

    case SYS_SBRK:
    case SYS_MMAP:
    case SYS_MUNMAP:
        return process_memory_op(p, nr, a0, a1, a2);

    case SYS_UPTIME:
        /* Milliseconds by default; microseconds when asked, which is the only
         * resolution fine enough to measure a frame. */
        /* Milliseconds by default; microseconds when asked, which is the only
         * resolution fine enough to measure a frame; and the cycle counter's
         * rate, so user space can read the counter itself and skip the call. */
        if (a0 == 2) return (s64)timer_cycles_per_us();
        return a0 ? (s64)timer_now_us() : (s64)g_uptime_ms;

    case SYS_TIME:
        return (s64)time_unix_seconds();

    case SYS_LOG_WRITE: {
        char subsys[16], msg[192];
        int r = copy_user_string(a1, subsys, sizeof subsys);
        if (r < 0) return r;
        r = copy_user_string(a2, msg, sizeof msg);
        if (r < 0) return r;
        klog((int)a0, subsys, "%s", msg);
        return 0;
    }

    case SYS_LOG_READ:
        return log_read_op(a0, a1, a2);

    case SYS_LOG_CTL:
        switch (a0) {
        case LOGCTL_COUNTS:
            if (!user_range_ok(a1, sizeof(u32) * 5, true)) return -E_INVAL;
            klog_counts((u32 *)a1);
            return 0;
        case LOGCTL_CLEAR:   klog_clear(); return 0;
        case LOGCTL_NEXTSEQ: return (s64)klog_next_seq();
        case LOGCTL_FLUSH:   klog_persist_flush(); return klog_persist_active() ? 1 : 0;
        case LOGCTL_DROPPED: return (s64)klog_dropped();
        case LOGCTL_CONSOLE_LEVEL: klog_set_console_level((int)a1); return 0;
        default: return -E_INVAL;
        }

    case SYS_SYSINFO: {
        _Static_assert(sizeof(ksysinfo_t) <= VMM_USER_COPY_MAX,
                       "sysinfo snapshot must fit one protected user copy");
        if (!user_range_ok(a0, sizeof(ksysinfo_t), true)) return -E_INVAL;
        ksysinfo_t info;
        memset(&info, 0, sizeof info);
        strlcpy(info.cpu, g_cpu.brand[0] ? g_cpu.brand : g_cpu.vendor, sizeof info.cpu);
        strlcpy(info.kernel, "KestrelOS " __DATE__, sizeof info.kernel);
        info.uptime_ms = g_uptime_ms;
        info.mem_total = pmm_total_bytes();
        info.mem_free = pmm_free_bytes();
        info.heap_total = heap_total();
        info.heap_used = heap_used();
        proc_count_snapshot(&info.proc_count, &info.thread_count);
        info.cpu_count = g_topo.threads; /* enabled processors, not MADT slots */

        info.cpu_sockets          = g_topo.sockets;
        info.cpu_cores            = g_topo.cores;
        info.cpu_threads          = g_topo.threads;
        info.cpu_threads_per_core = g_topo.threads_per_core;
        info.cpu_perf_cores       = g_topo.perf_cores;
        info.cpu_eff_cores        = g_topo.eff_cores;
        /* The boot processor, and however many others answered. */
        info.cpu_running          = (u32)smp_worker_count() + sched_application_cpu_count();
        info.cpu_application_threads = (u8)sched_application_cpu_count();
        info.cpu_base_mhz         = g_topo.base_mhz;
        info.cpu_max_mhz          = g_topo.max_mhz;
        info.cpu_bus_mhz          = g_topo.bus_mhz;
        info.cpu_l1               = cpu_cache_total(1);
        info.cpu_l2               = cpu_cache_total(2);
        info.cpu_l3               = cpu_cache_total(3);
        info.cpu_virtualization   = g_topo.virtualization ? 1 : 0;
        info.cpu_hybrid           = g_topo.hybrid ? 1 : 0;
        strlcpy(info.cpu_virt_name, g_topo.virt_name, sizeof info.cpu_virt_name);
        strlcpy(info.cpu_boot_core_kind, g_topo.boot_core_kind,
                sizeof info.cpu_boot_core_kind);

        strlcpy(info.board_maker,    g_smbios.board_maker,    sizeof info.board_maker);
        strlcpy(info.board_product,  g_smbios.board_product,  sizeof info.board_product);
        strlcpy(info.system_maker,   g_smbios.system_maker,   sizeof info.system_maker);
        strlcpy(info.system_product, g_smbios.system_product, sizeof info.system_product);
        strlcpy(info.bios_version,   g_smbios.bios_version,   sizeof info.bios_version);
        strlcpy(info.bios_date,      g_smbios.bios_date,      sizeof info.bios_date);
        strlcpy(info.mem_kind,       g_smbios.memory_kind,    sizeof info.mem_kind);
        strlcpy(info.mem_form,       g_smbios.memory_form,    sizeof info.mem_form);
        info.mem_slots_total = g_smbios.memory_slots_total
                             ? g_smbios.memory_slots_total : g_smbios.memory_count;
        info.mem_slots_used  = g_smbios.memory_slots_used;
        info.mem_speed_mts   = g_smbios.memory_speed_mts;
        info.fb_width = g_boot.fb.width;
        info.fb_height = g_boot.fb.height;
        info.pci_count = (u32)pci_device_count();
        info.block_count = (u32)block_disk_count();

        /* Raw totals since boot; the caller divides.  See the note beside
         * these fields in the header for why the kernel does not. */
        extern u64 g_disk_read_bytes, g_disk_write_bytes;
        info.cpu_time_valid = sched_cpu_time_snapshot(&info.cpu_busy_ms, &info.cpu_idle_ms);
        info.disk_read_bytes = g_disk_read_bytes;
        info.disk_write_bytes = g_disk_write_bytes;

        /* USB only, because that is what this counts and there is no
         * equivalent question to ask the PS/2 controller - it reports a port
         * as present whether or not anything is on the end of it. */
        info.keyboards = usb_keyboard_present() ? 1 : 0;
        info.pointers  = usb_mouse_present() ? 1 : 0;

        extern u32 g_hid_reports;
        extern u8  g_hid_last[8], g_hid_last_len;
        /* Whether the log has anywhere to go.  On a machine booted from a USB
         * stick this is the question "did the storage driver claim it", which
         * cannot otherwise be asked without a keyboard. */
        info.logging = klog_persist_active() ? 1 : 0;
        strlcpy(info.log_path, klog_persist_path(), sizeof info.log_path);
        usb_event_counts(&info.usb_events, &info.usb_transfers,
                         &info.usb_last_code, &info.usb_ep_state);

        const char *usbmsc_last_stage(void);
        int usbmsc_disk_count(void);
        info.usb_disks = (u8)usbmsc_disk_count();
        strlcpy(info.storage_stage, usbmsc_last_stage(),
                sizeof info.storage_stage);
        info.hid_reports = g_hid_reports;
        info.hid_last_len = g_hid_last_len;
        for (int i = 0; i < 8; i++) info.hid_last[i] = g_hid_last[i];
        if (!user_copy(&info, a0, sizeof info, true)) return -E_INVAL;
        return 0;
    }

    case SYS_CONSOLE: {
        /* A shortcut for programs that do not want to open /dev/console. */
        file_t *f = proc_fd_acquire(p, STDOUT_FD);
        if (!f) return -E_BADF;
        int result = p->is_kernel ? vfs_ioctl(f, (u32)a0, (void *)a1)
                                 : vfs_user_ioctl(f, p->pml4, (u32)a0, a1);
        vfs_close(f);
        return result;
    }

    case SYS_POWEROFF:
    case SYS_REBOOT: {
        bool restarting = (nr == SYS_REBOOT);
        kinfo("system", "%s requested by pid %d",
              restarting ? "reboot" : "power off", p->pid);

        /* The log goes out FIRST, before anything that touches a disk.
         *
         * It used to be written after the sync and the cache flush, which is
         * the tidier-looking order and was the wrong one: those two are the
         * riskiest calls in the whole sequence, because they are where the
         * kernel asks a device to accept a write and then waits.  When that
         * wait did not return - and on the target hardware it did not - the
         * machine stopped one line above the only statement that would have
         * said why.  The account of a boot must not be the thing that a
         * failure during shutdown destroys.
         *
         * So: publish, then do the work, then publish again.  The second write
         * supersedes the first and adds how the flush went; if it never
         * happens, the first is still there and its last line names the step
         * that hung.
         *
         * The verdicts go first, because they are what somebody reading this
         * afterwards actually wants and the tail is only eight kilobytes.
         * They are re-derived from what the drivers still hold rather than
         * read back out of the log, which has long since wrapped past them. */
        void efi_snapshot_report(void);
        void gpu_report_verdict(void);
        storage_report_verdict();
        gpu_report_verdict();
        efi_snapshot_report();

        efi_log_publish();

        vfs_sync();
        kinfo("system", "filesystems synced; flushing the block cache");
        block_cache_flush_all();
        kinfo("system", "block cache flushed; %s now",
              restarting ? "restarting" : "powering off");

        /* Now with the flush accounted for. */
        efi_log_publish();

        if (restarting) acpi_reboot();
        else            acpi_poweroff();

        /* Neither returns on a machine that obeys.  On one that does not, say
         * so rather than falling through in silence - a shutdown that quietly
         * does nothing looks exactly like a freeze from the front. */
        kwarn("system", "the firmware did not %s when asked",
              restarting ? "restart" : "switch the power off");
        efi_log_publish();
        return 0;
    }

    case SYS_MOUNT: {
        char dev[64], raw[VFS_PATH_MAX], full[VFS_PATH_MAX];
        int r = copy_user_string(a0, dev, sizeof dev);
        if (r < 0) return r;
        r = copy_user_string(a1, raw, sizeof raw);
        if (r < 0) return r;
        proc_resolve_path(p, raw, full, sizeof full);

        const char *name = dev;
        if (!strncmp(name, "/dev/", 5)) name += 5;
        blockdev_t *d = block_find(name);
        if (!d) return -E_NODEV;

        filesystem_t *fs = vfs_probe(d);
        if (!fs) return -E_INVAL;
        r = vfs_mount(full, fs);
        if (r < 0 && fs->unmount) fs->unmount(fs);
        return r;
    }

    case SYS_UNMOUNT: {
        char raw[VFS_PATH_MAX], full[VFS_PATH_MAX];
        int r = copy_user_string(a0, raw, sizeof raw);
        if (r < 0) return r;
        proc_resolve_path(p, raw, full, sizeof full);
        return vfs_unmount(full);
    }

    case SYS_MOUNTLIST: {
        if (!user_range_ok(a0, (size_t)a1, true)) return -E_INVAL;
        char buf[1024];
        int n = vfs_list_mounts(buf, sizeof buf);
        size_t len = strlen(buf) + 1;
        if (len > a1) len = (size_t)a1;
        memcpy((void *)a0, buf, len);
        ((char *)a0)[len ? len - 1 : 0] = 0;
        return n;
    }

    case SYS_SYNC:
        vfs_sync();
        block_cache_flush_all();
        klog_persist_flush();
        return 0;

    case SYS_CHDIR: {
        char raw[VFS_PATH_MAX], full[VFS_PATH_MAX];
        int r = copy_user_string(a0, raw, sizeof raw);
        if (r < 0) return r;
        proc_resolve_path(p, raw, full, sizeof full);

        vstat_t st;
        r = vfs_stat(full, &st);
        if (r < 0) return r;
        if (st.type != VN_DIR) return -E_NOTDIR;

        /* Store the normalised form so ".." never accumulates. */
        vnode_t *vn = NULL;
        if (vfs_resolve(full, &vn) == 0) vnode_unref(vn);
        strlcpy(proc_shared(p)->cwd, full, sizeof p->cwd);

        /* Trim any trailing slash except on the root. */
        char *cwd = proc_shared(p)->cwd;
        size_t n = strlen(cwd);
        while (n > 1 && cwd[n - 1] == '/') cwd[--n] = 0;
        return 0;
    }

    case SYS_GETCWD: {
        if (!user_range_ok(a0, (size_t)a1, true)) return -E_INVAL;
        size_t len = strlen(proc_shared(p)->cwd) + 1;
        if (len > a1) return -E_INVAL;
        memcpy((void *)a0, proc_shared(p)->cwd, len);
        return (s64)(len - 1);
    }

    case SYS_ENUM:
        if (a0 == ENUM_CPU) {
            if (!user_range_ok(a2, sizeof(kcpuinfo_t), true)) return -E_INVAL;
            kcpuinfo_t info;
            if (!cpu_logical_info((u32)a1, &info)) return -E_NOENT;
            memcpy((void *)a2, &info, sizeof info);
            return 0;
        }
        if (a0 == ENUM_BLOCK) return enum_block((u32)a1, a2);
        if (a0 == ENUM_PCI) return enum_pci((u32)a1, a2);
        if (a0 == ENUM_GPU) return enum_gpu((u32)a1, a2);
        if (a0 == ENUM_USB) return enum_usb((u32)a1, a2);
        if (a0 == ENUM_VIDEOMODE) return enum_videomode((u32)a1, a2);
        if (a0 == ENUM_DISPLAY) return enum_display((u32)a1, a2);
        if (a0 == ENUM_DISPLAY_OUTPUT) return enum_display_output((u32)a1, a2);
        if (a0 == ENUM_DISPLAY_OUTPUT_MODE) return enum_display_output_mode((u32)a1, a2);
        if (a0 == ENUM_AUDIO) return enum_audio((u32)a1, a2);
        if (a0 == ENUM_NET) return enum_net((u32)a1, a2);
        if (a0 == ENUM_ARP) return enum_arp((u32)a1, a2);
        if (a0 == ENUM_WIFI) return enum_wifi((u32)a1, a2);
        if (a0 == ENUM_SCAN) return enum_scan((u32)a1, a2);
        if (a0 == ENUM_FIRMWARE) return enum_firmware((u32)a1, a2);
        return -E_INVAL;

    case SYS_NET: {
        /* Every one of these can take seconds - a DHCP exchange, a name
         * lookup, a round of echo requests - so they run on the caller's
         * thread and block it, rather than pretending to be immediate. */
        switch ((u32)a0) {
        case NETOP_DHCP: {
            char name[16];
            if (a1 && copy_user_string(a1, name, sizeof name) < 0) return -E_INVAL;
            netdev_t *d = (a1 && name[0]) ? netdev_by_name(name) : netdev_first();
            if (!d) return -E_NODEV;
            return net_dhcp(d, (int)a2);
        }
        case NETOP_SET: {
            char name[16];
            if (a1 && copy_user_string(a1, name, sizeof name) < 0) return -E_INVAL;
            if (!user_range_ok(a2, sizeof(u32) * 4, false)) return -E_INVAL;
            netdev_t *d = (a1 && name[0]) ? netdev_by_name(name) : netdev_first();
            if (!d) return -E_NODEV;
            const u32 *v = (const u32 *)a2;
            return net_configure(d, v[0], v[1], v[2], v[3]);
        }
        case NETOP_RESOLVE: {
            char host[128];
            if (copy_user_string(a1, host, sizeof host) < 0) return -E_INVAL;
            return (s64)net_resolve(host, (int)a2);
        }
        case NETOP_PING: {
            if (!user_range_ok(a1, sizeof(u32) * 3, false)) return -E_INVAL;
            const u32 *in = (const u32 *)a1;
            u32 count = in[1];
            if (count > 32) count = 32;
            if (a2 && !user_range_ok(a2, sizeof(u32) * count, true)) return -E_INVAL;

            u32 times[32];
            int replies = net_ping(in[0], (int)count, (int)in[2], times);
            if (replies > 0 && a2)
                memcpy((void *)a2, times, sizeof(u32) * (size_t)replies);
            return replies;
        }
        case NETOP_CONNECT: {
            if (!user_range_ok(a1, sizeof(u32) * 3, false)) return -E_INVAL;
            const u32 *in = (const u32 *)a1;
            return tcp_connect(in[0], (u16)in[1], (int)in[2]);
        }
        case NETOP_TLS:
            /* The handshake takes a second or two against a distant server:
             * several round trips, a signature to check, and a chain to walk.
             * It runs on the caller's thread and blocks it, as connecting
             * does. */
            return tls_connect_op(a1, a2);

        case NETOP_TLSPEER: {
            if (!((u32)a1 & NET_TLS_HANDLE)) return -E_INVAL;
            if (!user_range_ok(a2, sizeof(tls_peer_t), true)) return -E_INVAL;
            if (!tls_peer_info((int)a1 & ~NET_TLS_HANDLE, (tls_peer_t *)a2))
                return -E_INVAL;
            return 0;
        }
        case NETOP_SEND: {
            if (!user_range_ok(a1, sizeof(u32) * 3, false)) return -E_INVAL;
            const u32 *in = (const u32 *)a1;
            if (in[1] > (1u << 20)) return -E_INVAL;
            if (!user_range_ok(a2, in[1], false)) return -E_INVAL;
            if (in[0] & NET_TLS_HANDLE)
                return tls_send_data((int)(in[0] & ~NET_TLS_HANDLE),
                                     (const void *)a2, (int)in[1], (int)in[2]);
            return tcp_send_data((int)in[0], (const void *)a2, (int)in[1], (int)in[2]);
        }
        case NETOP_RECV: {
            if (!user_range_ok(a1, sizeof(u32) * 3, false)) return -E_INVAL;
            const u32 *in = (const u32 *)a1;
            if (in[1] > (1u << 20)) return -E_INVAL;
            if (!user_range_ok(a2, in[1], true)) {
                kwarn("net", "a %u byte receive buffer at %p is not writable "
                             "memory in this process", in[1], (void *)a2);
                return -E_INVAL;
            }
            if (in[0] & NET_TLS_HANDLE)
                return tls_receive_data((int)(in[0] & ~NET_TLS_HANDLE),
                                        (void *)a2, (int)in[1], (int)in[2]);
            return tcp_receive_data((int)in[0], (void *)a2, (int)in[1], (int)in[2]);
        }
        case NETOP_CLOSE:
            if ((u32)a1 & NET_TLS_HANDLE) tls_close((int)a1 & ~NET_TLS_HANDLE);
            else tcp_close((int)a1);
            return 0;

        default:
            return -E_INVAL;
        }
    }

    case SYS_BLUETOOTH: {
        /* One adapter, which is what a machine has.  Everything below works
         * through it, so it is found once and the operations say what to do
         * with it rather than which one. */
        void *bt = btusb_adapter(0);

        switch ((u32)a0) {
        case BTOP_ADAPTER: {
            if (!user_range_ok(a1, sizeof(kbtadapter_t), true)) return -E_INVAL;
            kbtadapter_t out;
            memset(&out, 0, sizeof out);

            char maker[40];
            u8 addr[6];
            if (btusb_get(0, maker, sizeof maker, addr, NULL)) {
                out.present = 1;
                memcpy(out.address, addr, 6);
                strlcpy(out.maker, maker, sizeof out.maker);
            }
            memcpy((void *)a1, &out, sizeof out);
            return 0;
        }

        case BTOP_SCAN: {
            if (!bt) return -E_NODEV;
            int seconds = (int)a1;
            if (!btusb_scan(bt, seconds)) return -E_IO;

            /* Listening takes as long as it was asked to, on this thread -
             * the same way scanning for wireless networks does. */
            sched_sleep_ms((u32)(seconds > 0 ? seconds : 1) * 1000);
            btusb_scan_stop(bt);
            return btusb_found_count();
        }

        case BTOP_FOUND: {
            if (!user_range_ok(a2, sizeof(kbtdevice_t), true)) return -E_INVAL;
            kbtdevice_t out;
            memset(&out, 0, sizeof out);

            u8 addr[6];
            char name[32];
            int rssi = 127;
            u32 cls = 0;
            bool le = false;

            if (!btusb_found_get((int)a1, addr, name, sizeof name, &rssi,
                                 &cls, &le))
                return -E_NOENT;

            memcpy(out.address, addr, 6);
            out.low_energy = le ? 1 : 0;
            out.rssi = rssi;
            out.device_class = cls;
            strlcpy(out.name, name, sizeof out.name);
            strlcpy(out.kind, btusb_kind_name(cls), sizeof out.kind);
            memcpy((void *)a2, &out, sizeof out);
            return 0;
        }

        case BTOP_CONNECT: {
            if (!bt) return -E_NODEV;
            if (!user_range_ok(a1, 6, false)) return -E_INVAL;
            u8 addr[6];
            memcpy(addr, (const void *)a1, 6);
            return btusb_connect(bt, addr) ? 0 : -E_IO;
        }

        case BTOP_DISCONNECT:
            if (!bt) return -E_NODEV;
            btusb_disconnect(bt);
            return 0;

        case BTOP_LINK: {
            if (!user_range_ok(a1, sizeof(kbtlink_t), true)) return -E_INVAL;
            kbtlink_t out;
            memset(&out, 0, sizeof out);

            u8 addr[6];
            u16 handle = 0, interval = 0;
            if (btusb_link(addr, &handle, &interval)) {
                out.open = 1;
                memcpy(out.address, addr, 6);
                out.handle = handle;
                out.interval_us = interval;
            }
            memcpy((void *)a1, &out, sizeof out);
            return 0;
        }

        default:
            return -E_INVAL;
        }
    }

    case SYS_WIFI: {
        /* Scanning and joining take seconds; they run on the caller's thread
         * and block it, as the rest of the network stack does. */
        switch ((u32)a0) {
        case WIFIOP_SCAN: {
            char name[16];
            if (a1 && copy_user_string(a1, name, sizeof name) < 0) return -E_INVAL;
            wifi_device_t *d = (a1 && name[0]) ? wifi_by_name(name) : wifi_first();
            if (!d) return -E_NODEV;
            return wifi_scan(d, (int)a2);
        }
        case WIFIOP_CONNECT: {
            if (!user_range_ok(a1, sizeof(u64) * 3, false)) return -E_INVAL;
            const u64 *in = (const u64 *)a1;

            char iface[16], ssid[40], pass[80];
            iface[0] = ssid[0] = pass[0] = 0;
            if (in[0] && copy_user_string(in[0], iface, sizeof iface) < 0) return -E_INVAL;
            if (copy_user_string(in[1], ssid, sizeof ssid) < 0) return -E_INVAL;
            if (in[2] && copy_user_string(in[2], pass, sizeof pass) < 0) return -E_INVAL;

            wifi_device_t *d = iface[0] ? wifi_by_name(iface) : wifi_first();
            if (!d) return -E_NODEV;
            return wifi_connect(d, ssid, pass[0] ? pass : NULL, (int)a2);
        }
        case WIFIOP_DISCONNECT: {
            char name[16];
            if (a1 && copy_user_string(a1, name, sizeof name) < 0) return -E_INVAL;
            wifi_device_t *d = (a1 && name[0]) ? wifi_by_name(name) : wifi_first();
            if (!d) return -E_NODEV;
            wifi_disconnect(d);
            return 0;
        }
        case WIFIOP_SELFTEST:
            return wifi_selftest();
        case WIFIOP_ENABLE: {
            char name[16];
            if (a1 && copy_user_string(a1, name, sizeof name) < 0) return -E_INVAL;
            wifi_device_t *d = (a1 && name[0]) ? wifi_by_name(name) : wifi_first();
            if (!d) return -E_NODEV;
            return wifi_set_enabled(d, a2 != 0) ? 1 : 0;
        }
        case WIFIOP_ENABLED: {
            char name[16];
            if (a1 && copy_user_string(a1, name, sizeof name) < 0) return -E_INVAL;
            wifi_device_t *d = (a1 && name[0]) ? wifi_by_name(name) : wifi_first();
            if (!d) return -E_NODEV;
            return d->enabled ? 1 : 0;
        }
        default:
            return -E_INVAL;
        }
    }

    case SYS_GPU:
        switch (a0) {
        case GPUOP_VIDEO:
            return gpu_video_request(p, a1, a2);
        case GPUOP_SURFACE:
            return gpu_surface_request(p, a1, a2);
        case GPUOP_SELFTEST:
            /* Both vendors, because a graphics driver that only handles one
             * of them is not a graphics driver. */
            return nvidia_drive_test() + amd_drive_test();
        case GPUOP_CANDRAW:
            /* 2 means NVIDIA owns a new VRAM scanout, so userland must not
             * benchmark itself back onto the now-stale firmware framebuffer. */
            return gpu_accel_draw_mode();
        case GPUOP_READPIXEL: {
            if (!user_range_ok(a3, sizeof(u32), true)) return -E_INVAL;
            u32 pixel = 0;
            if (!gpu_accel_read_pixel((s32)a1, (s32)a2, &pixel))
                return -E_NOSYS;
            return user_copy(&pixel,a3,sizeof pixel,true) ? 0 : -E_INVAL;
        }
        case GPUOP_DRAW: {
            return gpu_triangle_request(a1, a2);
        }
        case GPUOP_IMAGE: {
            if (!user_range_ok(a1, sizeof(kimage_t), false)) return -E_INVAL;
            kimage_t req;
            memcpy(&req, (const void *)a1, sizeof req);
            /* The limit is the surface the picture is staged through, which
             * used to be sixty-four pixels square - an icon.  It turned out
             * the size was never what made that surface special; it was only
             * ever as big as it happened to be made at start-up. */
            if (!req.width || !req.height ||
                req.width > 10240 || req.height > 4320) return -E_INVAL;
            u32 stride = req.source_stride ? req.source_stride : req.width;
            if (stride < req.width) return -E_INVAL;

            /* The last row reaches only as far as the picture is wide, not a
             * whole stride - checking a full stride would refuse a rectangle
             * taken from the right-hand edge of a frame. */
            size_t bytes = (size_t)(req.height - 1) * stride * 4 +
                           (size_t)req.width * 4;
            if (!user_range_ok((u64)(uintptr_t)req.pixels, bytes, false))
                return -E_INVAL;
            int r = gpu_accel_draw_image((const u32 *)req.pixels,
                                         req.width, req.height, stride, req.x, req.y,
                                         req.draw_width, req.draw_height);
            return r < 0 ? -E_NOSYS : 0;
        }
        case GPUOP_SHADERS: {
            return gpu_shaders_request(a1);
        }
        case GPUOP_LAYOUT: {
            return gpu_layout_request(a1);
        }
        case GPUOP_START: {
            nv_card_t *c = nv_card(0);
            if (!c) return -E_NODEV;

            /* Asking for this IS the consent to write to the card.
             *
             * Register writes are refused by default on real silicon - see
             * nv_wr32 - so that a first boot on an unfamiliar machine cannot
             * lose the picture before anybody has chosen to risk it.  That is
             * the right default for the driver bringing itself up quietly
             * during start-up.  It is the wrong one here: `gpu start` prints a
             * paragraph saying the screen may not come back and then waits to
             * be typed, and the person who typed it has decided.
             *
             * Without this the command attached to the card, handed the
             * co-processor its firmware, and every one of those writes was
             * dropped on the floor - it would have failed with nothing wrong
             * except that nothing was ever written. */
            if (!nv_writes_allowed()) {
                kwarn("nvidia", "gpu start was asked for, so writing to the "
                                "card is now allowed - it was refused until "
                                "now, which is the default on hardware this "
                                "driver has not met");
                nv_allow_writes(true);
            }
            return nv_gsp_start(c) ? 0 : -E_IO;
        }
        case GPUOP_DETAIL: {
            if (!user_range_ok(a1, sizeof(kgpudetail_t), true)) return -E_INVAL;
            nv_card_t *c = nv_card(0);
            if (!c) return -E_NODEV;

            kgpudetail_t out;
            memset(&out, 0, sizeof out);
            out.chipset = c->chipset;
            out.revision = c->revision;
            out.outputs = (u8)c->outputs;
            out.modelled = c->modelled ? 1 : 0;
            out.temperature_c = c->temperature_c;
            out.fan_percent = c->fan_percent;
            out.vram_bytes = c->vram_bytes;
            strlcpy(out.architecture, c->architecture ? c->architecture : "",
                    sizeof out.architecture);
            strlcpy(out.codename, c->codename ? c->codename : "", sizeof out.codename);
            strlcpy(out.vbios_version, c->vbios_version, sizeof out.vbios_version);
            strlcpy(out.vbios_source, c->vbios_source ? c->vbios_source : "",
                    sizeof out.vbios_source);

            int shown = 0;
            for (int i = 0; i < c->outputs && shown < 8; i++) {
                const nv_output_t *o = &c->output[i];
                if (o->monitor_present) {
                    out.monitors++;
                    snprintf(out.connector[shown], sizeof out.connector[0],
                             "%s: \"%s\", %ux%u at %u Hz",
                             nv_output_type_name(o->type), o->monitor_name,
                             o->width, o->height, o->refresh_hz);
                } else {
                    snprintf(out.connector[shown], sizeof out.connector[0],
                             "%s: nothing connected", nv_output_type_name(o->type));
                }
                shown++;
            }

            memcpy((void *)a1, &out, sizeof out);
            return 0;
        }
        default:
            return -E_INVAL;
        }

    case SYS_BLKRESCAN: {
        char name[64];
        int r = copy_user_string(a0, name, sizeof name);
        if (r < 0) return r;
        const char *n = name;
        if (!strncmp(n, "/dev/", 5)) n += 5;
        blockdev_t *d = block_find(n);
        if (!d) return -E_NODEV;
        if (d->parent) return -E_INVAL;

        block_cache_invalidate(d);
        return block_scan_partitions(d);
    }

    case SYS_PROCLIST: {
        if (!user_range_ok(a1, sizeof(kprocinfo_t), true)) return -E_INVAL;
        proc_report_t row;
        if (a0 >= PROC_MAX || !proc_report_snapshot((u32)a0, &row)) return -E_NOENT;
        kprocinfo_t info;
        memset(&info, 0, sizeof info);
        info.pid = row.pid;
        info.parent = row.parent_pid;
        info.state = row.state;
        strlcpy(info.name, row.name, sizeof info.name);
        info.cpu_ms = row.cpu_ms;
        info.mem_bytes = row.image_bytes;
        /* No process pointer escapes the snapshot. User VM validation/copy is
         * a separate lock scope, after runqueue ownership has been released. */
        return user_copy(&info, a1, sizeof info, true) ? 0 : -E_INVAL;
    }

    case SYS_MEMINFO: {
        if (!user_range_ok(a0, sizeof(u64) * 6, true)) return -E_INVAL;
        u64 *out = (u64 *)a0;
        out[0] = pmm_total_bytes();
        out[1] = pmm_free_bytes();
        out[2] = pmm_used_bytes();
        out[3] = heap_total();
        out[4] = heap_used();
        out[5] = proc_shared(p)->heap_end - proc_shared(p)->heap_base;
        return 0;
    }

    case SYS_FRAMEBUFFER: {
        /* Map the display into the caller so a window system can draw straight
         * into it - and arbitrate, because two processes drawing on one screen
         * means whichever writes last wins and the other's pixels are gone. */
        if (a1 == FB_PRESENT) {
            return sys_fb_present(proc_shared(p)->pid,a2);
        }
        if (a1 == FB_UPDATE) {
            /* The whole reason for the display driver: say which rectangle
             * changed, rather than leaving the adapter to find out by taking
             * the framebuffer's pages away and waiting for a fault on each. */
            if (fb_owner_pid != proc_shared(p)->pid) return -E_PERM;
            if (!user_range_ok(a2, sizeof(s32) * 4, false)) return -E_INVAL;
            const s32 *r = (const s32 *)a2;
            svga_update(r[0], r[1], r[2], r[3]);
            return 0;
        }
        if (a1 == FB_SETMODE) {
            if (fb_owner_pid && fb_owner_pid != proc_shared(p)->pid) return -E_PERM;
            if (!svga_present()) return -E_NOSYS;
            if (!svga_set_mode((u32)a2, (u32)a3)) return -E_INVAL;
            svga_adopt_framebuffer();
            return 0;
        }
        if (a1 == FB_DISPLAYMODE) {
            return sys_fb_displaymode(proc_shared(p)->pid,a2);
        }
        if (a1 == FB_CHECK_CONFIGURATION) {
            return sys_fb_check_configuration(proc_shared(p)->pid,a2,a3);
        }
        if (a1 == FB_PREPARE_CONFIGURATION || a1 == FB_CANCEL_CONFIGURATION) {
            return sys_fb_prepare_configuration(p,a1,a2,a3);
        }
        if (a1 == FB_APPLY_OUTPUT_MODE || a1 == FB_CONFIRM_OUTPUT_MODE || a1 == FB_REVERT_OUTPUT_MODE) {
            return sys_fb_output_mode(p,a1,a2);
        }
        if (a1 == FB_CURSOR) {
            if (fb_owner_pid != proc_shared(p)->pid) return -E_PERM;
            if (!svga_cursor_available()) return -E_NOSYS;
            if (!a2) { svga_cursor_show(false); return 0; }
            if (!user_range_ok(a2, sizeof(kcursor_t), false)) return -E_INVAL;
            const kcursor_t *c = (const kcursor_t *)a2;
            if (c->width <= 0 || c->height <= 0 ||
                c->width > 64 || c->height > 64) return -E_INVAL;
            if (!svga_cursor_define(c->pixels, c->width, c->height,
                                    c->hot_x, c->hot_y)) return -E_IO;
            svga_cursor_show(true);
            return 0;
        }
        if (a1 == FB_FILL) {
            /* The adapter fills it, not the processor.  Refused rather than
             * done slowly when the adapter will not, so the caller knows to do
             * it itself instead of believing it was accelerated. */
            if (fb_owner_pid != proc_shared(p)->pid) return -E_PERM;
            if (!user_range_ok(a2, sizeof(s32) * 5, false)) return -E_INVAL;
            const s32 *r = (const s32 *)a2;
            if (!gpu_accel_fill(r[0], r[1], r[2], r[3], (u32)r[4])) return -E_NOSYS;
            return 0;
        }
        if (a1 == FB_COPY) {
            if (fb_owner_pid != proc_shared(p)->pid) return -E_PERM;
            if (!user_range_ok(a2, sizeof(s32) * 6, false)) return -E_INVAL;
            const s32 *r = (const s32 *)a2;
            if (!gpu_accel_copy(r[0], r[1], r[2], r[3], r[4], r[5])) return -E_NOSYS;
            return 0;
        }
        if (a1 == FB_ACCEL) {
            /* Whoever registered, not whoever this file happens to name. */
            u32 from_driver = gpu_accel_capabilities();
            u32 can = 0;
            if (from_driver & GPU_ACCEL_CAN_FILL)   can |= FB_ACCEL_FILL;
            if (from_driver & GPU_ACCEL_CAN_COPY)   can |= FB_ACCEL_COPY;
            if (from_driver & GPU_ACCEL_CAN_CURSOR) can |= FB_ACCEL_CURSOR;
            return (long)can;
        }
        if (a1 == FB_CURSORAT) {
            if (fb_owner_pid != proc_shared(p)->pid) return -E_PERM;
            if (!gpu_accel_cursor_move((int)a2, (int)a3)) return -E_NOSYS;
            return 0;
        }
        if (a1 == FB_RELEASE) {
            if (fb_owner_pid != proc_shared(p)->pid) return -E_PERM;
            fb_owner_pid = 0;
            console_take_framebuffer();
            return 0;
        }
        if (a1 == FB_REACQUIRE) {
            if (fb_owner_pid && fb_owner_pid != proc_shared(p)->pid) return -E_BUSY;
            fb_owner_pid = proc_shared(p)->pid;
            console_release_framebuffer();
            return 0;
        }

        return framebuffer_map(p, a0);
    }

    case SYS_MOUSEPOS: {
        if (!user_range_ok(a0, sizeof(int) * 2, true)) return -E_INVAL;
        int x = 0, y = 0;
        mouse_position(&x, &y);
        ((int *)a0)[0] = x;
        ((int *)a0)[1] = y;
        return mouse_present() ? 1 : 0;
    }

    case SYS_PIPE: {
        if (!user_range_ok(a0, sizeof(int) * 2, true)) return -E_INVAL;

        file_t *rf = NULL, *wf = NULL;
        int r = pipe_create(&rf, &wf);
        if (r < 0) return r;

        r = proc_fd_install_pipe(p, rf, wf, a0);
        if (r < 0) {
            vfs_close(rf);
            vfs_close(wf);
        }
        return r;
    }

    case SYS_DUP:
        return proc_fd_dup(p, (int)a0, -1);

    case SYS_DUP2:
        if ((int)a1 < 0) return -E_INVAL;
        return proc_fd_dup(p, (int)a0, (int)a1);

    case SYS_EFIVAR:
        return efivar_op((u32)a0, a1);

    default:
        kwarn("syscall", "pid %d called unknown syscall %lu", p->pid, nr);
        return -E_NOSYS;
    }
}

regs_t *syscall_dispatch(regs_t *r) {
    proc_t *p = proc_current();
    if (!p) { r->rax = (u64)-E_PERM; return r; }
    proc_kernel_enter();
    if (proc_stop_requested()) proc_kernel_leave(); /* terminates before dispatch */

    /* Interrupts were masked by SFMASK on entry; enable them so a long call
     * (a disk read, say) does not stall the timer. */
    sti();

    /* Which numbering this program is speaking was settled when it was
     * loaded, out of the byte in its own header. */
    s64 ret = p->linux_abi
        ? linux_dispatch(p, r)
        : dispatch(p, r->rax, r->rdi, r->rsi, r->rdx, r->r10, r->r8);

    cli();
    r->rax = (u64)ret;
    proc_kernel_leave();
    return r;
}
