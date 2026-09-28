/* appicons.c - load the app-icon PNGs and register them with libgui.
 *
 * The icons are real images (see tools/genicons.py): rounded gradient tiles
 * with clean white glyphs, bundled in the initrd under /usr/share/icons.  We
 * decode each once at start-up into an ARGB bitmap and hand it to
 * gui_register_app_icon(); gui_app_icon() then draws them scaled with
 * per-pixel alpha wherever an app is shown (desktop grid, taskbar, title bar).
 * If an icon file is missing or fails to decode, libgui simply falls back to
 * the drawn glyph, so the desktop still works. */
#include "desktop.h"
#include "png.h"

static void load_one(icon_id id, const char *path) {
    static unsigned char filebuf[96 * 1024];
    ssize_t n = read_file(path, filebuf, sizeof filebuf);
    if (n <= 0) return;

    png_image_t img;
    if (png_decode(filebuf, (size_t)n, &img) != PNG_OK) return;

    size_t px = (size_t)img.width * img.height;
    colour_t *argb = malloc(px * sizeof(colour_t));
    if (argb) {
        for (size_t i = 0; i < px; i++) {
            unsigned char r = img.rgb[i * 3 + 0];
            unsigned char g = img.rgb[i * 3 + 1];
            unsigned char b = img.rgb[i * 3 + 2];
            unsigned char a = img.alpha ? img.alpha[i] : 255;
            argb[i] = ((colour_t)a << 24) | ((colour_t)r << 16) |
                      ((colour_t)g << 8) | (colour_t)b;
        }
        gui_register_app_icon(id, argb, img.width, img.height);
        free(argb);
    }
    png_free(&img);
}

/* Register a decoded PNG's alpha channel as a tint mask (the OS logo). */
static void load_mask(icon_id id, const char *path) {
    static unsigned char filebuf[96 * 1024];
    ssize_t n = read_file(path, filebuf, sizeof filebuf);
    if (n <= 0) return;
    png_image_t img;
    if (png_decode(filebuf, (size_t)n, &img) != PNG_OK) return;
    if (img.alpha)
        gui_register_icon_mask(id, img.alpha, img.width, img.height);
    png_free(&img);
}

/* icon_id -> file.  These match the icons the desktop app table assigns and
 * the names tools/genicons.py writes. */
void desktop_load_app_icons(void) {
    load_one(ICON_BROWSER,  "/usr/share/icons/browser.png");
    load_one(ICON_STORE,    "/usr/share/icons/store.png");
    load_one(ICON_TERMINAL, "/usr/share/icons/terminal.png");
    load_one(ICON_FOLDER,   "/usr/share/icons/files.png");
    load_one(ICON_LOG,      "/usr/share/icons/events.png");
    load_one(ICON_INFO,     "/usr/share/icons/system.png");
    load_one(ICON_CHIP,     "/usr/share/icons/task.png");
    load_one(ICON_DISPLAY,  "/usr/share/icons/gl.png");
    load_one(ICON_EDITOR,   "/usr/share/icons/editor.png");
    load_one(ICON_INSTALL,  "/usr/share/icons/install.png");
    load_one(ICON_SETTINGS, "/usr/share/icons/settings.png");
    load_one(ICON_USB,      "/usr/share/icons/devices.png");
    /* Monochrome control glyphs (power button, window frame buttons) as real
     * images - drawn tinted (gui_app_icon_tinted) so they follow the theme and
     * light up on hover, instead of being pixel-drawn by gui_icon. */
    load_one(ICON_POWER,    "/usr/share/icons/glyph_power.png");
    load_one(ICON_CLOSE,    "/usr/share/icons/glyph_close.png");
    load_one(ICON_MINIMISE, "/usr/share/icons/glyph_min.png");
    load_one(ICON_MAXIMISE, "/usr/share/icons/glyph_max.png");
    load_one(ICON_RESTORE,  "/usr/share/icons/glyph_restore.png");
    /* The OS logo: the detailed kestrel.  logo_tile is the full-colour App/About
     * icon; logo_mask is the tintable silhouette gui_icon draws in the accent on
     * the taskbar and start-menu header. */
    load_one(ICON_KESTREL,  "/usr/share/icons/logo_tile.png");
    load_mask(ICON_KESTREL, "/usr/share/icons/logo_mask.png");
}
