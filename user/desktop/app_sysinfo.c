/* app_sysinfo.c - what this machine is, and what it is doing.
 *
 * Five tabs: an overview, memory, storage, graphics and the device list.
 * Everything is live, so the memory bars and process list move while the window
 * is open.
 */
#include "desktop.h"

typedef enum { TAB_OVERVIEW, TAB_MEMORY, TAB_STORAGE, TAB_GRAPHICS, TAB_DEVICES,
               TAB_COUNT } tab_t;

typedef struct {
    tab_t     tab;
    int       scroll;
    ksysinfo_t info;
    uint64_t  mem[6];
    uint64_t  refreshed;
} sysinfo_t;

static const char *tab_names[TAB_COUNT] = { "Overview", "Memory", "Storage",
                                           "Graphics", "Devices" };

/* These scale with the interface.  Left bare (38/26) the tab strip was half
 * height and every row was 26px tall for a 32px font on a 2x display - the
 * "words on top of each other" this whole app showed there. */
#define TAB_H     (38 * gui_scale())
#define ROW_H     (26 * gui_scale())

static rect_t tab_rect(int i) {
    const int k = gui_scale();
    return rect_make(10 * k + i * 96 * k, 6 * k, 92 * k, TAB_H - 12 * k);
}
static rect_t body_rect(surface_t *s) { return rect_make(0, TAB_H, s->width, s->height - TAB_H); }

static void refresh(sysinfo_t *si) {
    sysinfo(&si->info);
    meminfo(si->mem);
    si->refreshed = uptime_ms();
}

/* A labelled value pair, which is most of what this window is. */
/* One labelled row.
 *
 * The label is clipped to its own column, and that is not decoration.  It used
 * to be drawn with no bound at all while the value beside it was carefully
 * clipped - so a label longer than the column ran straight through the gap and
 * printed on top of its own value.  "Video memory in use" did exactly that,
 * and what appeared on screen was "Video memory in" with the value written
 * across the rest of it.
 *
 * Whichever side is left unbounded is the side that collides, and there was no
 * reason for it to be this one. */
/* Every caller passes the label column as a plain number - 150 - because that
 * is what it measures in the interface's own pixels.  The TEXT in that column
 * is drawn at the interface scale, so at twice the size a 150 pixel column
 * holds a label that wants 300, and the label runs straight into the value
 * beside it.  Which is what "everything smushed into each other" is.
 *
 * Scaling it here rather than at forty call sites means a caller cannot forget,
 * and the numbers at those call sites go on reading as the sizes somebody
 * chose while looking at the screen. */
static int field(surface_t *s, int x, int y, int label_w, const char *label, const char *value) {
    const int k = gui_scale();
    const int gap = 8 * k;
    label_w *= k;

    gui_text_clipped(s, FONT_UI, x, y, label_w - gap, label, g_theme.text_dim);
    gui_text_clipped(s, FONT_UI, x + label_w, y, s->width - x - label_w - 20 * k,
                     value, g_theme.text);
    return y + gui_font_height(FONT_UI) + 6 * k;
}

static void bar(surface_t *s, rect_t r, uint64_t used, uint64_t total, colour_t c) {
    gui_round_rect_aa(s, r, r.h / 2, g_theme.field);
    gui_round_frame_aa(s, r, r.h / 2, g_theme.field_border);
    if (!total) return;

    int w = (int)(((uint64_t)(r.w - 4) * used) / total);
    if (w > 0) gui_round_rect_aa(s, rect_make(r.x + 2, r.y + 2, w, r.h - 4), (r.h - 4) / 2, c);
}

/* --------------------------------------------------------------- the tabs */

static void paint_overview(sysinfo_t *si, surface_t *s) {
    rect_t body = body_rect(s);
    int x = 24 * gui_scale(), y = body.y + 20 * gui_scale(), lw = 150;

    const int k = gui_scale();
    gui_icon(s, ICON_KESTREL, x, y, 40 * k, g_theme.accent);
    gui_text(s, FONT_UI, x + 56 * k, y + 2 * k, "KestrelOS", g_theme.text_bright);
    gui_text(s, FONT_UI, x + 56 * k, y + 4 * k + gui_font_height(FONT_UI), si->info.kernel, g_theme.text_dim);
    /* The logo is 40*k tall and carries two lines beside it; a bare 60 left the
     * version line and the first row drawn over it on a 2x display. */
    y += 60 * k;

    gui_separator(s, x, y, body.w - 48 * k);
    y += 14 * k;

    y = field(s, x, y, lw, "Processor", si->info.cpu);

    char text[160];

    /* Sockets, cores and logical processors are three different numbers and
     * only the last of them used to be shown - labelled as though it were the
     * middle one.  They are shown separately now because on most machines they
     * genuinely differ, and somebody reading this window is usually here
     * because they want to know which is which. */
    {
        int n = 0;
        if (si->info.cpu_sockets > 1)
            n += snprintf(text + n, sizeof text - n, "%u sockets, ", si->info.cpu_sockets);
        n += snprintf(text + n, sizeof text - n, "%u core%s, %u logical",
                      si->info.cpu_cores, si->info.cpu_cores == 1 ? "" : "s",
                      si->info.cpu_threads);
        if (si->info.cpu_threads_per_core > 1)
            n += snprintf(text + n, sizeof text - n, " (%u per core)",
                          si->info.cpu_threads_per_core);
        /* On a hybrid part, the exact split once every core has named its own
         * kind - "8 performance + 12 efficiency" is what the user asked to see.
         * Falls back to just "hybrid" until that pass has run. */
        if (si->info.cpu_perf_cores || si->info.cpu_eff_cores)
            n += snprintf(text + n, sizeof text - n, ", %u P + %u E",
                          si->info.cpu_perf_cores, si->info.cpu_eff_cores);
        else if (si->info.cpu_hybrid && si->info.cpu_boot_core_kind[0])
            n += snprintf(text + n, sizeof text - n, ", hybrid");

        /* And how many are actually being used, when that is not all of them.
         * Silent when everything is running, because then there is nothing to
         * say - a line that always appears stops being read. */
        if (si->info.cpu_running && si->info.cpu_running < si->info.cpu_threads)
            snprintf(text + n, sizeof text - n, " - %u in use",
                     si->info.cpu_running);

        y = field(s, x, y, lw, "", text);
    }

    /* Either figure, whichever the machine was willing to give.  Some
     * processors carry the leaf that reports clocks and answer it with
     * zeroes, and then the firmware's table is the only source. */
    if (si->info.cpu_base_mhz || si->info.cpu_max_mhz) {
        int n = 0;
        if (si->info.cpu_base_mhz)
            n += snprintf(text + n, sizeof text - n, "%u.%02u GHz base",
                          si->info.cpu_base_mhz / 1000,
                          (si->info.cpu_base_mhz % 1000) / 10);
        if (si->info.cpu_max_mhz)
            snprintf(text + n, sizeof text - n, "%s%u.%02u GHz maximum",
                     n ? ", " : "", si->info.cpu_max_mhz / 1000,
                     (si->info.cpu_max_mhz % 1000) / 10);
        y = field(s, x, y, lw, "Speed", text);
    }

    /* Cache is shown as three totals rather than a list of every cache in the
     * machine: the list is what the log is for. */
    if (si->info.cpu_l1 || si->info.cpu_l2 || si->info.cpu_l3) {
        char l1[24], l2[24], l3[24];
        format_size(l1, sizeof l1, si->info.cpu_l1);
        format_size(l2, sizeof l2, si->info.cpu_l2);
        format_size(l3, sizeof l3, si->info.cpu_l3);
        /* On a hybrid processor the smaller caches are one core's, because
         * CPUID answers for the core that asked and the other kind of core has
         * a different amount.  Saying so is shorter than the alternative,
         * which is a total that cannot be arrived at honestly. */
        snprintf(text, sizeof text, "L1 %s   L2 %s   L3 %s%s", l1, l2, l3,
                 si->info.cpu_hybrid ? "   (L1 and L2 per core)" : "");
        y = field(s, x, y, lw, "Cache", text);
    }

    y = field(s, x, y, lw, "Virtualisation",
              si->info.cpu_virtualization
                  ? (si->info.cpu_virt_name[0] ? si->info.cpu_virt_name : "supported")
                  : "not supported");

    char total[24], freemem[24];
    format_size(total, sizeof total, si->info.mem_total);
    format_size(freemem, sizeof freemem, si->info.mem_free);
    snprintf(text, sizeof text, "%s total, %s free", total, freemem);
    y = field(s, x, y, lw, "Memory", text);

    if (si->info.mem_slots_total || si->info.mem_speed_mts) {
        int n = 0;
        if (si->info.mem_slots_total)
            n += snprintf(text + n, sizeof text - n, "%u of %u slot%s filled",
                          si->info.mem_slots_used, si->info.mem_slots_total,
                          si->info.mem_slots_total == 1 ? "" : "s");
        if (si->info.mem_kind[0])
            n += snprintf(text + n, sizeof text - n, "%s%s", n ? ", " : "", si->info.mem_kind);
        if (si->info.mem_form[0])
            n += snprintf(text + n, sizeof text - n, " %s", si->info.mem_form);
        if (si->info.mem_speed_mts)
            snprintf(text + n, sizeof text - n, "%s%u MT/s", n ? " at " : "",
                     si->info.mem_speed_mts);
        y = field(s, x, y, lw, "", text);
    }

    if (si->info.system_product[0] || si->info.board_product[0]) {
        snprintf(text, sizeof text, "%s %s", si->info.system_maker, si->info.system_product);
        y = field(s, x, y, lw, "Machine", text);
        if (si->info.board_product[0]) {
            snprintf(text, sizeof text, "%s %s", si->info.board_maker, si->info.board_product);
            y = field(s, x, y, lw, "Board", text);
        }
    }
    if (si->info.bios_version[0]) {
        snprintf(text, sizeof text, "%s%s%s", si->info.bios_version,
                 si->info.bios_date[0] ? ", " : "", si->info.bios_date);
        y = field(s, x, y, lw, "Firmware", text);
    }

    snprintf(text, sizeof text, "%u x %u", si->info.fb_width, si->info.fb_height);
    y = field(s, x, y, lw, "Display", text);

    uint64_t ms = si->info.uptime_ms;
    snprintf(text, sizeof text, "%llu:%02llu:%02llu",
             (unsigned long long)(ms / 3600000),
             (unsigned long long)((ms / 60000) % 60),
             (unsigned long long)((ms / 1000) % 60));
    y = field(s, x, y, lw, "Running for", text);

    char stamp[32];
    format_time(stamp, sizeof stamp, time_now());
    y = field(s, x, y, lw, "Clock", stamp);

    snprintf(text, sizeof text, "%u process%s, %u thread%s",
             si->info.proc_count, si->info.proc_count == 1 ? "" : "es",
             si->info.thread_count, si->info.thread_count == 1 ? "" : "s");
    y = field(s, x, y, lw, "Running", text);

    snprintf(text, sizeof text, "%u PCI device%s, %u disk%s",
             si->info.pci_count, si->info.pci_count == 1 ? "" : "s",
             si->info.block_count, si->info.block_count == 1 ? "" : "s");
    y = field(s, x, y, lw, "Hardware", text);
}

static void paint_memory(sysinfo_t *si, surface_t *s) {
    rect_t body = body_rect(s);
    int x = 24 * gui_scale(), y = body.y + 20 * gui_scale();
    int w = body.w - 48;

    char a[24], b[24], c[24];
    format_size(a, sizeof a, si->mem[0]);
    format_size(b, sizeof b, si->mem[2]);
    format_size(c, sizeof c, si->mem[1]);

    gui_text(s, FONT_UI, x, y, "Physical memory", g_theme.text);
    y += gui_font_height(FONT_UI) + 6;
    bar(s, rect_make(x, y, w, 18), si->mem[2], si->mem[0], g_theme.accent);
    y += 26;

    char line[120];
    snprintf(line, sizeof line, "%s in use of %s  (%s free)", b, a, c);
    gui_text(s, FONT_UI, x, y, line, g_theme.text_dim);
    y += gui_font_height(FONT_UI) + 24;

    format_size(a, sizeof a, si->mem[3]);
    format_size(b, sizeof b, si->mem[4]);
    gui_text(s, FONT_UI, x, y, "Kernel heap", g_theme.text);
    y += gui_font_height(FONT_UI) + 6;
    bar(s, rect_make(x, y, w, 18), si->mem[4], si->mem[3], g_theme.success);
    y += 26;
    snprintf(line, sizeof line, "%s in use of %s reserved", b, a);
    gui_text(s, FONT_UI, x, y, line, g_theme.text_dim);
    y += gui_font_height(FONT_UI) + 24;

    /* Processes, which is where the memory actually goes. */
    gui_text(s, FONT_UI, x, y, "Processes", g_theme.text);
    y += gui_font_height(FONT_UI) + 8;

    gui_text(s, FONT_UI, x, y, "PID", g_theme.text_dim);
    gui_text(s, FONT_UI, x + 50 * gui_scale(), y, "NAME", g_theme.text_dim);
    gui_text(s, FONT_UI, x + 210 * gui_scale(), y, "STATE", g_theme.text_dim);
    gui_text(s, FONT_UI, x + 320 * gui_scale(), y, "CPU (ms)", g_theme.text_dim);
    y += gui_font_height(FONT_UI) + 4;
    gui_separator(s, x, y, w);
    y += 6;

    static const char *states[] = { "unused", "starting", "ready", "running", "sleeping", "blocked", "zombie", "exiting" };
    for (uint32_t i = 0; y + ROW_H < body.y + body.h; i++) {
        kprocinfo_t p;
        if (proclist(i, &p) < 0) break;

        char num[16];
        snprintf(num, sizeof num, "%d", p.pid);
        gui_text(s, FONT_UI, x, y, num, g_theme.text);
        gui_text_clipped(s, FONT_UI, x + 50 * gui_scale(), y, 150 * gui_scale(), p.name, g_theme.text);
        gui_text(s, FONT_UI, x + 210 * gui_scale(), y, p.state < sizeof states / sizeof states[0] ? states[p.state] : "?",
                 p.state == 3 ? g_theme.success : g_theme.text_dim);
        snprintf(num, sizeof num, "%llu", (unsigned long long)p.cpu_ms);
        gui_text(s, FONT_UI, x + 320 * gui_scale(), y, num, g_theme.text_dim);
        y += ROW_H - 4;
    }
}

static void paint_storage(sysinfo_t *si, surface_t *s) {
    rect_t body = body_rect(s);
    int x = 24 * gui_scale(), y = body.y + 18 * gui_scale();
    int w = body.w - 48;
    const int k = gui_scale();

    /* Where the log is going, and why, at the top of the page about disks.
     *
     * This is the question somebody actually has when they open this window on
     * a machine that produced no log: not "what disks are there" but "why is
     * there nothing to read". The list below answers the first; without this
     * it answers the second only by implication, and only to somebody who
     * already knows what they are looking for.
     *
     * The USB count and the stage matter most on a stick: the loader reads it
     * through the firmware, and the moment the kernel takes over that firmware
     * is gone and the kernel needs its own driver to reach the same device. A
     * stick that booted the machine and then cannot be written to is exactly
     * what that gap looks like. */
    char line[200];
    if (si->info.logging && si->info.log_path[0])
        snprintf(line, sizeof line, "The log is being written to %s",
                 si->info.log_path);
    else
        snprintf(line, sizeof line, "NOTHING is being written - there is no "
                                    "volume this system may write a log to");

    gui_text_clipped(s, FONT_UI, x, y, w, line,
                     si->info.logging ? g_theme.text : g_theme.warning);
    y += gui_font_height(FONT_UI) + 4 * k;

    snprintf(line, sizeof line, "USB storage: %u device(s) found - %s",
             si->info.usb_disks,
             si->info.storage_stage[0] ? si->info.storage_stage : "not started");
    gui_text_clipped(s, FONT_UI, x, y, w, line,
                     si->info.usb_disks ? g_theme.text_dim : g_theme.warning);
    y += gui_font_height(FONT_UI) + 10 * k;

    gui_separator(s, x, y, w);
    y += 10 * k;

    gui_text(s, FONT_UI, x, y, "NAME", g_theme.text_dim);
    gui_text(s, FONT_UI, x + 110 * gui_scale(), y, "SIZE", g_theme.text_dim);
    gui_text(s, FONT_UI, x + 210 * gui_scale(), y, "TYPE", g_theme.text_dim);
    gui_text(s, FONT_UI, x + 290 * gui_scale(), y, "MODEL / LABEL", g_theme.text_dim);
    y += gui_font_height(FONT_UI) + 4;
    gui_separator(s, x, y, w);
    y += 8;

    for (uint32_t i = 0; y + ROW_H < body.y + body.h; i++) {
        kblockinfo_t bi;
        if (enum_block(i, &bi) < 0) break;

        icon_id icon = bi.is_partition ? ICON_FILE : ICON_DISK;
        gui_icon(s, icon, x, y + 2, 14 * gui_scale(),
                 bi.is_partition ? g_theme.text_dim : g_theme.accent);
        gui_text(s, FONT_UI, x + 20 * gui_scale(), y, bi.name, g_theme.text);

        char size[24];
        format_size(size, sizeof size, bi.size);
        gui_text(s, FONT_UI, x + 110 * gui_scale(), y, size, g_theme.text);
        gui_text(s, FONT_UI, x + 210 * gui_scale(), y, bi.is_partition ? "partition" : "disk", g_theme.text_dim);
        gui_text_clipped(s, FONT_UI, x + 290 * gui_scale(), y, w - 300,
                         bi.label[0] ? bi.label : bi.model, g_theme.text_dim);
        y += ROW_H - 2;
    }

    y += 14;
    gui_text(s, FONT_UI, x, y, "Mounted filesystems", g_theme.text);
    y += gui_font_height(FONT_UI) + 8;

    char mounts[1024];
    if (mountlist(mounts, sizeof mounts) >= 0) {
        const char *p = mounts;
        while (*p && y + gui_font_height(FONT_UI) < body.y + body.h) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len) gui_text_n(s, FONT_MONO, x, y, p, len, g_theme.text_dim);
            y += gui_font_height(FONT_MONO) + 2;
            if (!nl) break;
            p = nl + 1;
        }
    }
}

/* The graphics tab is where the honest answer about the card lives.  Naming the
 * silicon and then saying plainly what is and is not driven is more use than a
 * vendor logo and a number. */
static void paint_graphics(sysinfo_t *si, surface_t *s) {
    rect_t body = body_rect(s);
    int x = 24 * gui_scale(), y = body.y + 18 * gui_scale(), lw = 150;
    int w = body.w - 48;

    /* How many there are, before drawing any - so running out of room can say
     * how much was left rather than simply stopping.
     *
     * A machine with a graphics card usually has TWO adapters: the card and
     * the one built into the processor.  This list stopped when it ran out of
     * vertical space and said nothing, so on such a machine the second one
     * disappeared and the page looked complete - which is the failure mode
     * worth avoiding, because a reader has no way to tell a short list from a
     * truncated one. */
    uint32_t adapters = 0;
    for (;; adapters++) {
        kgpuinfo_t probe;
        if (enum_gpu(adapters, &probe) < 0) break;
    }

    uint32_t shown = 0;
    for (uint32_t i = 0; ; i++) {
        kgpuinfo_t g;
        if (enum_gpu(i, &g) < 0) break;
        if (y + 140 > body.y + body.h) {
            char more[96];
            snprintf(more, sizeof more,
                     "%u more adapter%s below - this page does not scroll yet",
                     adapters - shown, adapters - shown == 1 ? "" : "s");
            gui_text(s, FONT_UI, x, y + 6, more, g_theme.text_dim);
            break;
        }
        shown++;

        gui_icon(s, ICON_DISPLAY, x, y, 28 * gui_scale(), g_theme.accent);
        gui_text(s, FONT_UI, x + 40 * gui_scale(), y + 2 * gui_scale(), g.name, g_theme.text_bright);
        y += 40 * gui_scale();   /* the adapter's name sits over the first row at 2x if this is bare */

        y = field(s, x, y, lw, "Architecture", g.arch);

        char text[128];
        if (g.vram_bytes) {
            char size[24];
            format_size(size, sizeof size, g.vram_bytes);
            snprintf(text, sizeof text, "%s%s", g.vram_exact ? "" : "up to ", size);
        } else {
            snprintf(text, sizeof text, "unknown");
        }
        y = field(s, x, y, lw, "Video memory", text);

        snprintf(text, sizeof text, "%04x:%04x at %02x:%02x.%u",
                 g.pci_vendor, g.pci_device, g.bus, g.slot, g.func);
        y = field(s, x, y, lw, "Device", text);

        /* What the card is doing, engine by engine.
         *
         * The two states that are not a percentage are spelled out rather than
         * shown as zero.  An engine the card does not have and one whose
         * activity cannot be seen are different facts, and both are different
         * from an engine sitting idle - which is exactly what a bare 0% would
         * suggest for all three. */
        {
            static const char *names[4] = { "3D", "Copy", "Video encode",
                                            "Video decode" };
            for (int e = 0; e < 4; e++) {
                if (g.engine_percent[e] == -1) {
                    /* Named rather than left out: "this card has no video
                     * encoder" is an answer, and an absent row is not. */
                    snprintf(text, sizeof text, "not on this card");
                } else if (g.engine_percent[e] == -2) {
                    snprintf(text, sizeof text, "fitted; usage needs the card's "
                                                "firmware to read");
                } else if (!g.engines_sampled) {
                    snprintf(text, sizeof text, "measuring");
                } else {
                    snprintf(text, sizeof text, "%d%%", g.engine_percent[e]);
                }
                y = field(s, x, y, lw, names[e], text);
            }
        }

        /* The rows below are always drawn, even when there is nothing to put
         * in them.
         *
         * They used to be skipped when empty, and that quietly cost the reader
         * the thing they most needed: a row that is not there cannot be told
         * apart from a field this system has never heard of.  "Temperature -
         * this card does not report one" and no temperature row at all look
         * identical on screen and mean completely different things, and only
         * one of them is a gap in this system. */
        if (g.vram_used) {
            char used[24];
            format_size(used, sizeof used, g.vram_used);
            snprintf(text, sizeof text, "%s handed out by this driver (it "
                                        "cannot see what firmware reserved)", used);
        } else {
            snprintf(text, sizeof text, "not tracked - this driver has not "
                                        "handed any out");
        }
        y = field(s, x, y, lw, "Video memory in use", text);

        if (g.temperature_c > -1000) {
            if (g.fan_percent > 0)
                snprintf(text, sizeof text, "%d C, fan at %d%%",
                         g.temperature_c, g.fan_percent);
            else
                snprintf(text, sizeof text, "%d C", g.temperature_c);
        } else {
            snprintf(text, sizeof text, "this card does not report one to this "
                                        "driver");
        }
        y = field(s, x, y, lw, "Temperature", text);

        y = field(s, x, y, lw, "Graphics APIs",
                  g.graphics_apis[0] ? g.graphics_apis : "none on this card");
        y = field(s, x, y, lw, "Drawn by",
                  g.renderer[0] ? g.renderer : "the processor");

        if (g.driver_version[0]) {
            snprintf(text, sizeof text, "%s%s%s", g.driver_version,
                     g.driver_date[0] ? ", built " : "", g.driver_date);
        } else {
            snprintf(text, sizeof text, "this system's own, built with the "
                                        "rest of it");
        }
        y = field(s, x, y, lw, "Driver", text);

        if (g.boot_display) {
            snprintf(text, sizeof text, "%u x %u, write-combined framebuffer",
                     si->info.fb_width, si->info.fb_height);
            y = field(s, x, y, lw, "Display", text);

            kdisplayinfo_t d;
            if (enum_display(&d) == 0) {
                if (d.present) {
                    snprintf(text, sizeof text, "%s%s%s, native %u x %u at %u.%02u Hz",
                             d.manufacturer, d.model[0] ? " " : "", d.model,
                             d.native_width, d.native_height,
                             d.refresh_mhz / 1000, (d.refresh_mhz % 1000) / 10);
                    y = field(s, x, y, lw, "Monitor", text);
                }
                if (d.mode_count) {
                    /* The largest mode the firmware offered is the ceiling on
                     * what this machine can be asked for. */
                    unsigned best_w = 0, best_h = 0;
                    for (uint32_t k = 0; k < d.mode_count; k++) {
                        kvideomode_t m;
                        if (enum_videomode(k, &m) < 0) break;
                        if ((unsigned)m.width * m.height > best_w * best_h) {
                            best_w = m.width;
                            best_h = m.height;
                        }
                    }
                    snprintf(text, sizeof text, "%u available, largest %u x %u",
                             d.mode_count, best_w, best_h);
                    y = field(s, x, y, lw, "Modes", text);
                }
            }
        }

        if (g.firmware_needed) {
            snprintf(text, sizeof text, "%s  -  %s", g.firmware_name,
                     g.firmware_present ? "present" : "not present");
            y = field(s, x, y, lw, "Firmware", text);
        }

        if (g.note[0]) {
            y += 6;
            gui_text(s, FONT_UI, x, y, "What is driven", g_theme.text_dim);
            y += gui_font_height(FONT_UI) + 4;
            y = gui_text_wrapped(s, FONT_UI, rect_make(x + 12, y, w - 24,
                                                       body.y + body.h - y),
                                 g.note, g_theme.text);
        }

        y += 16;
        gui_separator(s, x, y, w);
        y += 14;
    }

    /* Sound, because a machine with a controller but no usable output path
     * looks the same as one with no sound card until it is said out loud. */
    kaudioinfo_t audio;
    if (enum_audio(&audio) == 0 && audio.present && y + 60 < body.y + body.h) {
        gui_text(s, FONT_UI, x, y, "Sound", g_theme.text);
        y += gui_font_height(FONT_UI) + 6;

        char line[160];
        if (audio.output_ready)
            snprintf(line, sizeof line, "%s, %s  -  %u Hz, %u channel%s, %u-bit",
                     audio.controller, audio.codec, audio.sample_rate,
                     audio.channels, audio.channels == 1 ? "" : "s", audio.bits);
        else
            snprintf(line, sizeof line, "%s  -  %s", audio.controller,
                     audio.note[0] ? audio.note : "no usable output");
        gui_text_clipped(s, FONT_UI, x + 12 * gui_scale(), y, w - 24, line, g_theme.text_dim);
        y += ROW_H;
    }

    /* USB is worth showing here too: on a machine with no PS/2 port it is the
     * only reason the keyboard works at all. */
    kusbinfo_t u;
    if (enum_usb(0, &u) >= 0 && y + 40 < body.y + body.h) {
        gui_text(s, FONT_UI, x, y, "USB devices", g_theme.text);
        y += gui_font_height(FONT_UI) + 8;
        for (uint32_t i = 0; y + ROW_H < body.y + body.h; i++) {
            if (enum_usb(i, &u) < 0) break;
            char line[128];
            snprintf(line, sizeof line, "port %u   %04x:%04x   %s%s%s",
                     u.port, u.vendor, u.product, u.name,
                     u.driver[0] ? "  -  " : "", u.driver);
            gui_text_clipped(s, FONT_UI, x + 12 * gui_scale(), y, w - 24, line, g_theme.text_dim);
            y += ROW_H - 4;
        }
    }
}

static void paint_devices(sysinfo_t *si, surface_t *s, int scroll) {
    (void)si;
    rect_t body = body_rect(s);
    int x = 24 * gui_scale(), y = body.y + 18 * gui_scale();
    int w = body.w - 48;

    gui_text(s, FONT_UI, x, y, "ADDRESS", g_theme.text_dim);
    gui_text(s, FONT_UI, x + 110 * gui_scale(), y, "DEVICE", g_theme.text_dim);
    y += gui_font_height(FONT_UI) + 4;
    gui_separator(s, x, y, w);
    y += 8;

    rect_t saved = surface_clip(s);
    surface_set_clip(s, rect_make(body.x, y, body.w, body.y + body.h - y));

    for (uint32_t i = (uint32_t)scroll; ; i++) {
        kpciinfo_t p;
        if (enum_pci(i, &p) < 0) break;
        if (y + ROW_H > body.y + body.h) break;

        char addr[24];
        snprintf(addr, sizeof addr, "%02x:%02x.%u", p.bus, p.slot, p.func);
        gui_text(s, FONT_MONO, x, y + 2, addr, g_theme.text_dim);
        gui_text_clipped(s, FONT_UI, x + 110 * gui_scale(), y, w - 120, p.description, g_theme.text);
        y += ROW_H - 3;
    }
    surface_set_clip(s, saved);
}

/* ------------------------------------------------------------------ window */

static void sysinfo_paint(sysinfo_t *si, surface_t *s, int mx, int my) {
    gui_clear(s, g_theme.window);

    gui_fill(s, rect_make(0, 0, s->width, TAB_H), colour_shade(g_theme.window, 5));
    gui_hline(s, 0, TAB_H - 1, s->width, g_theme.window_border);

    for (int i = 0; i < TAB_COUNT; i++) {
        rect_t r = tab_rect(i);
        if (si->tab == (tab_t)i) {
            gui_round_rect_aa(s, r, 4, g_theme.accent);
            gui_text_centred(s, FONT_UI, r, tab_names[i], g_theme.accent_text);
        } else {
            bool hover = rect_contains(r, mx, my);
            if (hover) gui_round_rect_aa(s, r, 4, g_theme.control_hover);
            gui_text_centred(s, FONT_UI, r, tab_names[i], g_theme.text);
        }
    }

    switch (si->tab) {
    case TAB_OVERVIEW: paint_overview(si, s); break;
    case TAB_MEMORY:   paint_memory(si, s); break;
    case TAB_STORAGE:  paint_storage(si, s); break;
    case TAB_GRAPHICS: paint_graphics(si, s); break;
    case TAB_DEVICES:  paint_devices(si, s, si->scroll); break;
    default: break;
    }
}

static bool sysinfo_proc(window_t *w, const wevent_t *ev) {
    sysinfo_t *si = w->data;

    switch (ev->kind) {
    case WE_PAINT:
        sysinfo_paint(si, w->canvas, ev->x, ev->y);
        return false;

    case WE_MOUSE_MOVE:
        return true;

    case WE_MOUSE_DOWN:
        for (int i = 0; i < TAB_COUNT; i++) {
            if (!rect_contains(tab_rect(i), ev->x, ev->y)) continue;
            si->tab = (tab_t)i;
            si->scroll = 0;
            refresh(si);
            return true;
        }
        return false;

    case WE_MOUSE_WHEEL:
        if (si->tab == TAB_DEVICES) {
            si->scroll -= ev->wheel * 3;
            if (si->scroll < 0) si->scroll = 0;
            return true;
        }
        return false;

    case WE_TICK:
        refresh(si);
        return true;

    case WE_CLOSE:
        free(si);
        return false;

    default:
        return false;
    }
}

void app_sysinfo_launch(wm_t *wm) {
    sysinfo_t *si = calloc(1, sizeof *si);
    if (!si) return;
    refresh(si);

    window_t *w = desktop_new_window(wm, "System", ICON_INFO, 660, 470, sysinfo_proc, si);
    if (!w) { free(si); return; }
    w->min_w = 480;
    w->min_h = 300;
}
