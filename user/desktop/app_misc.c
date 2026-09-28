/* app_misc.c - settings, the installer launcher and the about box. */
#include "desktop.h"

/* --------------------------------------------------------------- installer */

typedef struct {
    char summary[512];
    bool has_target;
} install_t;

static rect_t install_go(surface_t *s) { return rect_make(s->width - 200, s->height - 56, 180, 36); }

static void install_paint(install_t *in, surface_t *s, int mx, int my) {
    gui_clear(s, g_theme.window);

    gui_icon(s, ICON_INSTALL, 24 * gui_scale(), 22 * gui_scale(),
             40 * gui_scale(), g_theme.accent);
    gui_text(s, FONT_UI, 80, 24, "Install KestrelOS", g_theme.text_bright);
    gui_text(s, FONT_UI, 80, 26 + gui_font_height(FONT_UI),
             "Put this system onto a disk so it starts without the boot medium.",
             g_theme.text_dim);
    gui_separator(s, 24, 82, s->width - 48);

    int y = 98;
    const char *p = in->summary;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        gui_text_n(s, FONT_UI, 24, y, p, len, g_theme.text);
        y += gui_font_height(FONT_UI) + 3;
        if (!nl) break;
        p = nl + 1;
    }

    gui_text(s, FONT_UI, 24, s->height - 96,
             "The installer runs on the text console and asks before writing anything.",
             g_theme.text_dim);

    rect_t go = install_go(s);
    gui_button_icon(s, go, ICON_INSTALL, "Start installer",
                    rect_contains(go, mx, my), false, in->has_target);
}

static bool install_proc(window_t *w, const wevent_t *ev) {
    install_t *in = w->data;

    switch (ev->kind) {
    case WE_PAINT:
        install_paint(in, w->canvas, ev->x, ev->y);
        return false;

    case WE_MOUSE_MOVE:
        return true;

    case WE_MOUSE_DOWN:
        if (in->has_target && rect_contains(install_go(w->canvas), ev->x, ev->y)) {
            desktop_request_console(w->wm, "/bin/installer");
            return false;
        }
        return false;

    case WE_CLOSE:
        free(in);
        return false;

    default:
        return false;
    }
}

void app_install_launch(wm_t *wm) {
    install_t *in = calloc(1, sizeof *in);
    if (!in) return;

    /* Show what is actually attached, so the choice is informed. */
    size_t n = 0;
    n += strlcpy(in->summary + n, "Disks found in this computer:\n\n", sizeof in->summary - n);

    int disks = 0;
    for (uint32_t i = 0; ; i++) {
        kblockinfo_t b;
        if (enum_block(i, &b) < 0) break;
        if (b.is_partition) continue;

        char size[24];
        format_size(size, sizeof size, b.size);
        char line[160];
        snprintf(line, sizeof line, "    %-8s %10s   %s\n", b.name, size, b.model);
        n += strlcpy(in->summary + n, line, sizeof in->summary - n);
        disks++;
        if (n > sizeof in->summary - 200) break;
    }

    if (!disks) {
        strlcpy(in->summary, "No disks were found.\n\n"
                "The kernel did not detect a storage controller it can drive.\n"
                "Open System and look at the Devices tab to see what is present.",
                sizeof in->summary);
    } else {
        strlcat(in->summary,
                "\nThe installer can take a whole disk, or use only unallocated\n"
                "space so an existing system is left alone.",
                sizeof in->summary);
    }
    in->has_target = disks > 0;

    window_t *w = desktop_new_window(wm, "Install", ICON_INSTALL, 620, 400, install_proc, in);
    if (!w) { free(in); return; }
    w->resizable = false;
}

/* ------------------------------------------------------------------- about */

static bool about_proc(window_t *w, const wevent_t *ev) {
    surface_t *s = w->canvas;

    if (ev->kind != WE_PAINT) return ev->kind == WE_MOUSE_MOVE;

    gui_gradient_v(s, rect_make(0, 0, s->width, s->height),
                   colour_shade(g_theme.window, 6), g_theme.window);

    /* Spaced by the font, not by pixel numbers chosen for one size of it.
     *
     * The title sat at 100 and the version line at 130 - thirty pixels apart,
     * enough for the small face and exactly half of what the doubled one
     * needs, so at the larger scale the two lines printed on top of each
     * other.  Every distance below is some multiple of the current line
     * height, which is what makes the page hold together at any size. */
    const int k = gui_scale();
    int line_h = gui_font_height(FONT_UI);
    int y = 26 * k;

    /* The full-colour brand tile (the detailed kestrel), centred and large. */
    int logo = 88 * k;
    gui_app_icon(s, ICON_KESTREL, s->width / 2 - logo / 2, y, logo);
    y += logo + line_h / 2;

    rect_t title = rect_make(0, y, s->width, line_h);
    gui_text_centred(s, FONT_UI, title, "KestrelOS", g_theme.text_bright);
    y += line_h + 4 * k;

    ksysinfo_t info;
    if (sysinfo(&info) == 0) {
        rect_t sub = rect_make(0, y, s->width, line_h);
        gui_text_centred(s, FONT_UI, sub, info.kernel, g_theme.text_dim);
        y += line_h + 4 * k;
    }

    static const char *lines[] = {
        "An operating system written from nothing:",
        "its own UEFI loader, kernel, drivers, filesystems,",
        "C library, window system and applications.",
        "",
        "x86-64, UEFI, NVMe and AHCI storage, FAT filesystems,",
        "a Windows console program loader, and an event log",
        "that records what the machine has been doing.",
    };

    y += line_h;
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) {
        rect_t r = rect_make(0, y, s->width, line_h);
        gui_text_centred(s, FONT_UI, r, lines[i],
                         i < 3 ? g_theme.text : g_theme.text_dim);
        y += line_h + 2 * k;
    }
    return false;
}

void app_about_launch(wm_t *wm) {
    window_t *w = desktop_new_window(wm, "About KestrelOS", ICON_KESTREL, 520, 380, about_proc, NULL);
    if (w) w->resizable = false;
}
