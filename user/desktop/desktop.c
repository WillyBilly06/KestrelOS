/* desktop.c - wallpaper, desktop icons, taskbar and the launcher menu. */
#include "desktop.h"

static wm_t     g_wm;
static display_t g_display;
static int      g_wallpaper = 0;

/* Set when an app needs the text console: the desktop stands down, runs the
 * program with the console to itself, then takes the screen back. */
static char pending_console[128];

/* ------------------------------------------------------------------ layout */

/* The chrome, in interface pixels rather than screen ones.
 *
 * Written as functions rather than constants because the multiplier is not
 * known until the display has been opened - and leaving them constant while
 * the text scaled would put a taller line of type into a box that stayed the
 * same height, which is worse than either alone. */
#define TASKBAR_H     (44 * gui_scale())
#define LAUNCHER_W    (150 * gui_scale())
/* Wide enough for the descriptions to fit.
 *
 * At 280 they did not: every second line ended in an ellipsis, so the menu
 * told you what each entry was called and then cut off the sentence saying
 * what it did.  A description that cannot be read is worse than no description
 * - it takes the space and gives nothing back - and the fix is the width, not
 * shorter words. */
#define MENU_W        (390 * gui_scale())
#define MENU_ITEM_H   (52 * gui_scale())
#define MENU_HEADER_H (52 * gui_scale())   /* the KestrelOS header band        */
#define MENU_ICON_SZ  (26 * gui_scale())   /* the tile in each row + the header */

static bool menu_open;
static int  menu_hover = -1;
static int  task_hover = -1;

/* ------------------------------------------------------- what the user chose
 *
 * A look that has to be chosen again on every boot is not a look somebody has
 * chosen; it is one they keep re-entering.  Six themes and a row of accents
 * are worth having only if the machine remembers which was picked.
 *
 * Two places, tried in order, because the same system runs from two kinds of
 * volume.  An installed machine has somewhere of its own to write; a stick
 * booted on a strange computer has only the volume it came from - and that is
 * the case where a remembered preference matters most, because the stick is
 * carried between machines and should look the same on each.
 */
typedef struct {
    uint32_t magic;
    uint32_t theme, accent, wallpaper;
    uint32_t scale;                      /* 0 means "whatever the screen says" */
} desk_prefs_t;

/* Bumped because the shape changed.  An older file is ignored rather than read
 * as though the new field were there - which would set the size from whatever
 * happened to follow the wallpaper number in memory. */
#define PREFS_MAGIC 0x4B505247u          /* "GRPK" */

static const char *prefs_paths[] = { "/data/desktop.prefs", "/boot/desktop.prefs" };
static const char *conf_paths[]  = { "/data/desktop.conf",  "/boot/desktop.conf"  };

/* ---------------------------------------------------------- the config file
 *
 * The settings above were kept in a small binary record, which works and is
 * the wrong shape for what this is.  A person who wants their desktop a
 * particular way should be able to say so in a file they can read, edit and
 * copy to another machine - which is how every system this is meant to feel
 * like has always worked, and is what "customisable" means beyond a row of
 * buttons.
 *
 * So the file is text.  It names its values rather than numbering them, it
 * carries a comment saying what each one may be, and anything it does not
 * understand is skipped rather than rejected: a file written by a later
 * version, or with a line somebody was in the middle of, still applies as far
 * as it goes.
 *
 * The old binary file is still read, so an existing machine keeps its choices;
 * it is no longer written.
 */

static void trim(char *t) {
    char *a = t;
    while (*a == ' ' || *a == '\t') a++;
    if (a != t) memmove(t, a, strlen(a) + 1);
    size_t n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '\t' ||
                 t[n - 1] == '\r' || t[n - 1] == '\n'))
        t[--n] = 0;
}

static bool same_fold(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
        a++; b++;
    }
    return !*a && !*b;
}

/* A value may be a name or a number.  Names first, because that is what the
 * file is for; a number still works so that a value with no name (a wallpaper,
 * say) can be written down. */
static int value_to_index(const char *value, int count,
                          const char *(*name_of)(int)) {
    if (name_of) {
        for (int i = 0; i < count; i++) {
            const char *n = name_of(i);
            if (n && same_fold(n, value)) return i;
        }
    }
    if (value[0] >= '0' && value[0] <= '9') {
        int n = 0;
        for (const char *c = value; *c >= '0' && *c <= '9'; c++)
            n = n * 10 + (*c - '0');
        if (n >= 0 && n < count) return n;
    }
    return -1;
}

static const char *theme_name_of(int i) {
    const theme_spec_t *t = theme_at(i);
    return t ? t->name : NULL;
}

/* ------------------------------------------------- colours, by their names
 *
 * Choosing a theme and an accent is choosing between what somebody else
 * decided.  This is the other kind of customisation - the kind where a person
 * who wants their title bars a particular colour writes down which colour, and
 * gets it:
 *
 *     theme = dark
 *     colour.title_active = #1f4d3a
 *     colour.accent       = #7fd1a8
 *
 * Applied after the theme, so a theme provides the whole set and these change
 * whichever ones somebody cared about.  Every colour the interface has is
 * named here; there is no shorter list of "the ones we thought you would
 * want", because the one somebody wants is always the one that is missing from
 * such a list.
 */
typedef struct { const char *name; size_t at; } theme_colour_t;

static const theme_colour_t theme_colours[] = {
    { "desktop_top", offsetof(theme_t, desktop_top) },
    { "desktop_bottom", offsetof(theme_t, desktop_bottom) },
    { "window", offsetof(theme_t, window) },
    { "window_border", offsetof(theme_t, window_border) },
    { "window_shadow", offsetof(theme_t, window_shadow) },
    { "title_active", offsetof(theme_t, title_active) },
    { "title_inactive", offsetof(theme_t, title_inactive) },
    { "title_text", offsetof(theme_t, title_text) },
    { "title_text_inactive", offsetof(theme_t, title_text_inactive) },
    { "text", offsetof(theme_t, text) },
    { "text_dim", offsetof(theme_t, text_dim) },
    { "text_bright", offsetof(theme_t, text_bright) },
    { "accent", offsetof(theme_t, accent) },
    { "accent_dark", offsetof(theme_t, accent_dark) },
    { "accent_text", offsetof(theme_t, accent_text) },
    { "control", offsetof(theme_t, control) },
    { "control_hover", offsetof(theme_t, control_hover) },
    { "control_press", offsetof(theme_t, control_press) },
    { "control_border", offsetof(theme_t, control_border) },
    { "field", offsetof(theme_t, field) },
    { "field_border", offsetof(theme_t, field_border) },
    { "field_text", offsetof(theme_t, field_text) },
    { "taskbar", offsetof(theme_t, taskbar) },
    { "taskbar_text", offsetof(theme_t, taskbar_text) },
    { "taskbar_hover", offsetof(theme_t, taskbar_hover) },
    { "selection", offsetof(theme_t, selection) },
    { "selection_text", offsetof(theme_t, selection_text) },
    { "warning", offsetof(theme_t, warning) },
    { "error", offsetof(theme_t, error) },
    { "success", offsetof(theme_t, success) },
    { "scrollbar", offsetof(theme_t, scrollbar) },
    { "scrollbar_thumb", offsetof(theme_t, scrollbar_thumb) },
};

/* Six hex digits, with or without a hash in front.  Returns false rather than
 * guessing at anything else: a colour that silently became black would be
 * harder to explain than one that was refused. */
static bool parse_colour(const char *text, colour_t *out) {
    if (*text == '#') text++;

    uint32_t v = 0;
    int digits = 0;
    for (; *text; text++, digits++) {
        int d;
        if (*text >= '0' && *text <= '9') d = *text - '0';
        else if (*text >= 'a' && *text <= 'f') d = *text - 'a' + 10;
        else if (*text >= 'A' && *text <= 'F') d = *text - 'A' + 10;
        else return false;
        if (digits >= 6) return false;
        v = (v << 4) | (uint32_t)d;
    }
    if (digits != 6) return false;

    *out = RGB((int)((v >> 16) & 0xFF), (int)((v >> 8) & 0xFF), (int)(v & 0xFF));
    return true;
}

/* One "colour.something = #rrggbb" line.  Returns false when the key is not
 * one of these at all, so the caller can go on trying other keys. */
static bool apply_colour_key(const char *key, const char *value) {
    static const char prefix[] = "colour.";
    static const char prefix_us[] = "color.";      /* both spellings */

    const char *name = NULL;
    if (!strncmp(key, prefix, sizeof prefix - 1)) name = key + sizeof prefix - 1;
    else if (!strncmp(key, prefix_us, sizeof prefix_us - 1))
        name = key + sizeof prefix_us - 1;
    if (!name) return false;

    for (size_t i = 0; i < sizeof theme_colours / sizeof theme_colours[0]; i++) {
        if (!same_fold(name, theme_colours[i].name)) continue;

        colour_t c;
        if (!parse_colour(value, &c)) {
            char line[128];
            snprintf(line, sizeof line,
                     "%s is not six hex digits, so %s was left alone",
                     value, theme_colours[i].name);
            log_write(2, "desktop", line);
            return true;                    /* it was ours, and it was wrong */
        }
        *(colour_t *)((char *)&g_theme + theme_colours[i].at) = c;
        return true;
    }

    /* A colour nobody has: worth saying, because the usual cause is a
     * misspelling and silence makes that very hard to find. */
    char line[128];
    snprintf(line, sizeof line, "there is no colour called \"%s\"", name);
    log_write(2, "desktop", line);
    return true;
}

static bool read_conf(int *theme, int *accent, int *wallpaper) {
    for (int i = 0; i < 2; i++) {
        int fd = open(conf_paths[i], O_RDONLY);
        if (fd < 0) continue;

        static char text[2048];
        ssize_t n = read(fd, text, sizeof text - 1);
        close(fd);
        if (n <= 0) continue;
        text[n] = 0;

        char *line = text;
        while (line && *line) {
            char *next = strchr(line, '\n');
            if (next) *next++ = 0;

            char *hash = strchr(line, '#');
            if (hash) *hash = 0;

            char *eq = strchr(line, '=');
            if (!eq) { line = next; continue; }
            *eq = 0;

            char key[64], value[64];
            strlcpy(key, line, sizeof key);
            strlcpy(value, eq + 1, sizeof value);
            trim(key);
            trim(value);
            if (!key[0] || !value[0]) { line = next; continue; }

            if (same_fold(key, "theme")) {
                int v = value_to_index(value, theme_count(), theme_name_of);
                if (v >= 0 && theme) *theme = v;
            } else if (same_fold(key, "accent")) {
                int v = value_to_index(value, accent_count(), accent_name);
                if (v >= 0 && accent) *accent = v;
            } else if (same_fold(key, "wallpaper")) {
                int v = value_to_index(value, desktop_wallpaper_count(),
                                       desktop_wallpaper_name);
                if (v >= 0 && wallpaper) *wallpaper = v;
            } else if (same_fold(key, "scale")) {
                int v = value_to_index(value, 4, NULL);
                if (v >= 1 && v <= 3) gui_set_scale(v);
            } else if (apply_colour_key(key, value)) {
                /* Handled, and any complaint already made. */
            }
            line = next;
        }
        return true;
    }
    return false;
}

bool desktop_prefs_load(int *theme, int *accent, int *wallpaper) {
    /* The readable file wins when both are there, because it is the one
     * somebody may have just edited. */
    if (read_conf(theme, accent, wallpaper)) return true;

    for (int i = 0; i < 2; i++) {
        int fd = open(prefs_paths[i], O_RDONLY);
        if (fd < 0) continue;

        desk_prefs_t p;
        ssize_t n = read(fd, &p, sizeof p);
        close(fd);

        if (n != (ssize_t)sizeof p || p.magic != PREFS_MAGIC) continue;
        if ((int)p.theme >= theme_count() || (int)p.accent >= accent_count())
            continue;

        if (theme) *theme = (int)p.theme;
        if (accent) *accent = (int)p.accent;
        if (wallpaper) *wallpaper = (int)p.wallpaper;
        if (p.scale >= 1 && p.scale <= 3) gui_set_scale((int)p.scale);
        return true;
    }
    return false;
}

void desktop_prefs_save(int theme, int accent, int wallpaper) {
    /* Written with the list of what each value may be, so the file explains
     * itself to whoever opens it next.  That is most of what makes a config
     * file usable rather than merely present. */
    static char out[2048];
    int at = 0;

    at += snprintf(out + at, sizeof out - at,
                   "# KestrelOS desktop\n"
                   "#\n"
                   "# Edit this and the change applies next time the desktop\n"
                   "# starts.  Names or numbers both work.  Anything not\n"
                   "# understood is skipped, so a line can be commented out\n"
                   "# with a # rather than deleted.\n\n");

    at += snprintf(out + at, sizeof out - at, "# one of:");
    for (int i = 0; i < theme_count(); i++) {
        const theme_spec_t *t = theme_at(i);
        at += snprintf(out + at, sizeof out - at, " %s%s",
                       t ? t->name : "?", i + 1 < theme_count() ? "," : "");
    }
    at += snprintf(out + at, sizeof out - at, "\ntheme = %s\n\n",
                   theme_name_of(theme) ? theme_name_of(theme) : "Midnight");

    at += snprintf(out + at, sizeof out - at, "# one of:");
    for (int i = 0; i < accent_count(); i++)
        at += snprintf(out + at, sizeof out - at, " %s%s",
                       accent_name(i) ? accent_name(i) : "?",
                       i + 1 < accent_count() ? "," : "");
    at += snprintf(out + at, sizeof out - at, "\naccent = %s\n\n",
                   accent_name(accent) ? accent_name(accent) : "Blue");

    at += snprintf(out + at, sizeof out - at, "# one of:");
    for (int i = 0; i < desktop_wallpaper_count(); i++)
        at += snprintf(out + at, sizeof out - at, " %s%s",
                       desktop_wallpaper_name(i),
                       i + 1 < desktop_wallpaper_count() ? "," : "");
    at += snprintf(out + at, sizeof out - at, "\nwallpaper = %s\n\n",
                   desktop_wallpaper_name(wallpaper));

    at += snprintf(out + at, sizeof out - at,
                   "# how large everything is drawn: 1, 2 or 3\n"
                   "scale = %d\n", gui_scale());

    for (int i = 0; i < 2; i++) {
        int fd = open(conf_paths[i], O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) continue;
        ssize_t n = write(fd, out, (size_t)at);
        close(fd);
        if (n == at) return;
    }

}

static rect_t taskbar_rect(void) {
    return rect_make(0, g_display.back->height - TASKBAR_H, g_display.back->width, TASKBAR_H);
}

static rect_t launcher_rect(void) {
    rect_t t = taskbar_rect();
    const int k = gui_scale();
    return rect_make(t.x + 6 * k, t.y + 6 * k, LAUNCHER_W, t.h - 12 * k);
}

/* The power button and the clock live at the right end of the bar.  Both are
 * defined here, once, so the paint pass and the click handler cannot disagree
 * about where they are - the reason the old pair drifted was that each place
 * wrote its own rect_make with its own pixel numbers, none of them scaled. */
static rect_t power_rect(void) {
    rect_t t = taskbar_rect();
    const int k = gui_scale();
    int sz = t.h - 12 * k;                       /* a square, inset top and bottom */
    return rect_make(t.x + t.w - sz - 8 * k, t.y + 6 * k, sz, sz);
}

static rect_t clock_rect(void) {
    rect_t t = taskbar_rect();
    const int k = gui_scale();
    rect_t p = power_rect();
    int w = 120 * k;
    return rect_make(p.x - w - 4 * k, t.y, w, t.h);
}

static rect_t menu_rect(void) {
    int count = 0;
    desktop_apps(&count);
    int k = gui_scale();
    int h = count * MENU_ITEM_H + MENU_HEADER_H + 8 * k;
    rect_t t = taskbar_rect();
    return rect_make(6 * k, t.y - h - 6 * k, MENU_W, h);
}

static rect_t menu_item_rect(int index) {
    rect_t m = menu_rect();
    int k = gui_scale();
    return rect_make(m.x + 6 * k, m.y + MENU_HEADER_H + index * MENU_ITEM_H,
                     m.w - 12 * k, MENU_ITEM_H);
}

/* Buttons for each open window, laid out after the launcher. */
/* How wide a taskbar button is, given how much room there is.
 *
 * Every number here used to be a plain pixel count - 190 wide, 16 in from the
 * launcher, 200 kept back for the clock - while the icon and the text inside
 * the button were both multiplied by the interface scale.  At scale two the
 * button stayed 190 pixels and its contents doubled, so every title was cut to
 * a few characters and the icon was drawn over the first letter of it.  That
 * is the taskbar reading "Files" as an icon and "iles".
 *
 * Sized like a real one now: a preferred width that scales, shrunk to fit when
 * there are many windows and never below what an icon and a few letters need.
 * This is what makes the row adapt to the window count as well as the screen,
 * rather than to neither. */
/* How many windows want a button right now.  Counted by the paint pass, which
 * runs every frame and sees them all; the hit tests that run between frames
 * use the same figure.  Zero means "not counted yet" and is treated as one. */
static int task_windows;

static int task_button_span(int *first_x, int *count_max) {
    rect_t t = taskbar_rect();
    const int k = gui_scale();

    int x = t.x + LAUNCHER_W + 16 * k;
    int reserved = 200 * k;                 /* the clock and power button */
    int room = t.w - x - reserved;
    if (room < 0) room = 0;

    int prefer = 190 * k;

    /* Shrink to fit before cutting off.
     *
     * At full width a large screen holds five buttons, and the sweep that
     * found this had eight windows open: three buttons showed and five
     * windows were simply unreachable from the taskbar.  Every desktop this
     * imitates narrows the buttons as windows accumulate, because a narrow
     * button that exists beats a wide one that does not.  Only below the
     * width of an icon and a few letters does it give up and cut the list. */
    int want = task_windows > 0 ? task_windows : 1;
    if (want * prefer > room) {
        prefer = room / want;
        int least = 16 * k + 16 * k + gui_text_width(FONT_UI, "MMMM");
        if (prefer < least) {
            /* Not even a short label each.  Fall back to icon-only buttons,
             * which is what every taskbar this imitates does when the row is
             * full - a recognisable icon beats four letters, and it beats a
             * window with no button at all by more.  The floor below is an
             * icon and its breathing room; past that the row is simply full
             * and the remainder are cut, as before. */
            prefer = room / want;
            int icon_only = 40 * k;
            if (prefer < icon_only) prefer = icon_only;
        }
    }

    int fits = prefer ? room / prefer : 0;
    if (fits < 1) fits = 1;

    if (first_x) *first_x = x;
    if (count_max) *count_max = fits;
    return prefer;
}

static rect_t task_button_rect(int index) {
    rect_t t = taskbar_rect();
    const int k = gui_scale();

    int x = 0, max = 0;
    int w = task_button_span(&x, &max);
    if (index >= max) return rect_make(0, 0, 0, 0);
    return rect_make(x + index * w, t.y + 6 * k, w - 6 * k, t.h - 12 * k);
}

/* Where a window goes when it is put away.  The manager asks for this so a
 * minimising window collapses towards its own button rather than towards a
 * corner, which is what lets the eye follow it. */
static rect_t taskbar_slot_for(wm_t *wm, window_t *w) {
    int index = wm_window_index(wm, w);
    rect_t r = task_button_rect(index);
    if (rect_empty(r)) {
        /* Off the end of the taskbar: aim at where the buttons run out. */
        rect_t t = taskbar_rect();
        return rect_make(t.x + t.w - 220, t.y + 6, 40, t.h - 12);
    }
    return r;
}

/* ------------------------------------------------------------------- apps */

static const app_entry_t apps[] = {
    { "Kestrel",      "Browse the web",                  ICON_BROWSER,  app_browser_launch,  true },
    { "Applications", "Get programs and run them",       ICON_STORE,    app_store_launch,    true },
    { "Terminal",     "Run commands",                    ICON_TERMINAL, app_terminal_launch, true },
    { "Files",        "Browse the filesystem",           ICON_FOLDER,   app_files_launch,    true },
    { "Event Viewer", "Warnings and errors the system recorded", ICON_LOG, app_events_launch, true },
    { "System",       "What this machine is and is doing", ICON_INFO,   app_sysinfo_launch,  true },
    { "Task Manager", "What is running, and what it is using", ICON_CHIP, app_taskmgr_launch, true },
    { "3D",           "GPU shader graphics demo",     ICON_DISPLAY,  app_gl_launch,       true },
    { "Text Editor",  "Read and write files",            ICON_EDITOR,   app_editor_launch,   false },
    { "Install",      "Put KestrelOS on a disk",         ICON_INSTALL,  app_install_launch,  true },
    { "Settings",     "Wi-Fi, display, sound and more",  ICON_SETTINGS, app_settings_launch, true },
    { "Device Manager", "Everything in this machine",    ICON_USB,      app_devices_launch,  false },
    { "About",        "About KestrelOS",                 ICON_KESTREL,  app_about_launch,    false },
};

const app_entry_t *desktop_apps(int *count) {
    if (count) *count = (int)(sizeof apps / sizeof apps[0]);
    return apps;
}

/* What to open when the desktop first paints.  Normally the Terminal, so a
 * machine nobody can type into still shows something and a photograph of it
 * reads.  If /etc/startapp exists it is a list of app names, one per line, to
 * open instead - the way to bring the desktop up on a chosen app (used to
 * capture a single app's window for review off-hardware, and a plain autostart
 * list besides). */
static void desktop_autostart(wm_t *wm) {
    char buf[256];
    if (!file_exists("/etc/startapp")) { app_terminal_launch(wm); return; }
    ssize_t n = read_file("/etc/startapp", buf, sizeof buf - 1);
    if (n <= 0) { app_terminal_launch(wm); return; }
    buf[n] = 0;

    int count = 0, launched = 0;
    const app_entry_t *a = desktop_apps(&count);
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        while (*line == ' ' || *line == '\t' || *line == '\r') line++;
        size_t len = strlen(line);
        while (len && (line[len-1]==' '||line[len-1]=='\t'||line[len-1]=='\r')) line[--len]=0;
        if (len) {
            for (int i = 0; i < count; i++)
                if (!strcasecmp(a[i].name, line) && a[i].launch) {
                    a[i].launch(wm); launched++; break;
                }
        }
        line = nl ? nl + 1 : NULL;
    }
    if (!launched) app_terminal_launch(wm);
}

/* Desktop icons, down the left-hand side. */
/* Wide enough for the longest name that has no space in it to break at.
 * "Applications" is that name, and a column narrower than it leaves the label
 * either overflowing into its neighbours or shortened to "Applicati...", which
 * is a worse answer than a slightly wider column. */
#define DESK_ICON_W   (116 * gui_scale())
/* Tall enough for the icon and two lines of label, with the descenders of the
 * second line inside the cell rather than cut off by it.
 *
 * The arithmetic underneath is: ten above the icon, forty for the icon, six
 * below it, then two lines - and a line's height does not include the room a
 * "g" or a "y" needs below the baseline.  At 104 the second line lost its
 * tails, which reads as a slightly wrong font rather than as a box being too
 * short, and is why it survived being looked at. */
#define DESK_ICON_H   (116 * gui_scale())

static rect_t desktop_icon_rect(int slot) {
    /* Down the left edge, and into a second column when the first runs out of
     * screen.  A fixed single column means that on a shorter display the last
     * few programs are simply not there - which is not a smaller list, it is a
     * list with things missing from it and no way to tell. */
    int height = g_display.back ? g_display.back->height : 768;
    int usable = height - TASKBAR_H - 24;
    int per_column = usable / DESK_ICON_H;
    if (per_column < 1) per_column = 1;

    int column = slot / per_column;
    int row = slot % per_column;

    return rect_make(24 * gui_scale() + column * (DESK_ICON_W + 16 * gui_scale()),
                     24 * gui_scale() + row * DESK_ICON_H,
                     DESK_ICON_W, DESK_ICON_H - 8);
}

static int desktop_icon_count(void) {
    int n = 0, count = 0;
    const app_entry_t *a = desktop_apps(&count);
    for (int i = 0; i < count; i++) if (a[i].on_desktop) n++;
    return n;
}

static const app_entry_t *desktop_icon_app(int slot) {
    int n = 0, count = 0;
    const app_entry_t *a = desktop_apps(&count);
    for (int i = 0; i < count; i++) {
        if (!a[i].on_desktop) continue;
        if (n == slot) return &a[i];
        n++;
    }
    return NULL;
}

static int desk_hover = -1;
static int desk_selected = -1;

/* --------------------------------------------------------------- wallpaper */

#define WALLPAPER_COUNT 4

static const char *wallpaper_names[WALLPAPER_COUNT] = {
    "Dusk", "Slate", "Ink", "Aurora",
};

int desktop_wallpaper_count(void) { return WALLPAPER_COUNT; }
const char *desktop_wallpaper_name(int i) {
    return (i >= 0 && i < WALLPAPER_COUNT) ? wallpaper_names[i] : "";
}
bool desktop_wallpaper_index(int *index) { if (index) *index = g_wallpaper; return true; }

void desktop_set_wallpaper(int index) {
    if (index < 0 || index >= WALLPAPER_COUNT) return;
    g_wallpaper = index;
    wm_damage_all(&g_wm);
}

/* A deterministic pseudo-random value, so the same wallpaper redraws
 * identically every frame without storing a bitmap. */
static uint32_t hash2(int x, int y, uint32_t salt) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + salt * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* Shared by the real desktop and Appearance swatches; previews must not carry
 * a second palette that drifts from the wallpaper the user actually gets. */
void desktop_wallpaper_colours(int index, colour_t *top, colour_t *bottom) {
    switch (index) {
    case 1:
        *top = RGB(0x2A,0x2F,0x38); *bottom = RGB(0x14,0x17,0x1C);
        break;
    case 2:
        *top = *bottom = RGB(0x0C,0x0E,0x14);
        break;
    case 3:
        *top = RGB(0x0B,0x1E,0x2A); *bottom = RGB(0x12,0x3A,0x38);
        break;
    default:
        *top = g_theme.desktop_top; *bottom = g_theme.desktop_bottom;
        break;
    }
}

void desktop_paint_wallpaper(surface_t *s, rect_t area, int index) {
    rect_t full = rect_make(0, 0, s->width, s->height);
    colour_t top, bottom;
    desktop_wallpaper_colours(index,&top,&bottom);
    if (index == 2) gui_clear(s,top);
    else gui_gradient_v(s,full,top,bottom);

    if (index == 3) {
        /* Soft diagonal bands, brightest towards the top left. */
        for (int y = area.y; y < area.y + area.h; y++) {
            for (int x = area.x; x < area.x + area.w; x++) {
                int band = ((x + y * 2) / 90) % 3;
                if (band == 0) gui_blend_pixel(s,x,y,RGB(0x2E,0x7E,0x74),26);
                else if (band == 1) gui_blend_pixel(s,x,y,RGB(0x1A,0x4E,0x6E),16);
            }
        }
    }

    if (index == 2) {
        /* A faint grid, which reads as a technical backdrop. */
        for (int y = area.y; y < area.y + area.h; y++) {
            if (y % 32) continue;
            gui_hline(s, area.x, y, area.w, RGB(0x16, 0x1A, 0x24));
        }
        for (int x = area.x; x < area.x + area.w; x++) {
            if (x % 32) continue;
            gui_vline(s, x, area.y, area.h, RGB(0x16, 0x1A, 0x24));
        }
    }

    if (index == 0) {
        /* Scattered faint stars. */
        for (int y = area.y; y < area.y + area.h; y++) {
            for (int x = area.x; x < area.x + area.w; x++) {
                uint32_t h = hash2(x, y, 7);
                if ((h & 0x3FFF) < 6) {
                    int bright = 40 + (int)((h >> 20) & 0x3F);
                    gui_blend_pixel(s,x,y,RGB(0xFF,0xFF,0xFF),bright);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ chrome */

static void paint_desktop_icons(surface_t *s, rect_t area) {
    int n = desktop_icon_count();
    for (int i = 0; i < n; i++) {
        const app_entry_t *a = desktop_icon_app(i);
        if (!a) continue;
        rect_t r = desktop_icon_rect(i);

        /* An icon nowhere near what is being repainted costs nothing to skip
         * and a dozen drawing calls to clip away. */
        if (!rect_intersects(r, area)) continue;

        int radius = 8 * gui_scale();

        if (i == desk_selected) {
            gui_round_rect_aa(s, r, radius,
                              colour_mix(g_theme.desktop_bottom, g_theme.accent, 90));
            gui_round_frame_aa(s, r, radius, g_theme.accent);
        } else if (i == desk_hover) {
            /* A soft panel rather than a flat wash: the wash tinted the
             * wallpaper right to a hard square edge, which is the one thing
             * that reads as unfinished on an otherwise rounded desktop. */
            gui_round_rect_aa(s, r, radius,
                              colour_mix(g_theme.desktop_bottom,
                                         RGB(0xFF, 0xFF, 0xFF), 18));
        }

        /* The icon's own size scales with everything else.
         *
         * This was a flat 40 pixels while the cell around it, the font under
         * it and the spacing beside it were all multiplied by the interface
         * scale.  At 1024 wide that is a 40 pixel icon in a 116 pixel cell -
         * about a third of it, which looks right.  At 1920 the cell doubles to
         * 232 and the icon does not, so it occupies a sixth: the labels
         * dominate, the icons read as shrunken, and the whole desktop looks
         * sparse and unfinished at exactly the resolution most machines run.
         *
         * Nothing reports this.  It is only visible by putting the two
         * resolutions side by side, which is why it survived. */
        int size = 40 * gui_scale();
        int pad  = 10 * gui_scale();

        gui_app_icon(s, a->icon, r.x + (r.w - size) / 2, r.y + pad, size);

        /* The label, on one line if it fits and two if it does not.
         *
         * It used to be drawn centred on its width whatever that width was,
         * so a name wider than the icon's column started to the left of it and
         * ended to the right of it - over its neighbours, and under whatever
         * window happened to be there.  "Applications" and "Event Viewer" are
         * both wider than the column, which is why they read as "Application"
         * and "Event Viewe": nothing was truncating them, they were running
         * off both ends and being painted over.
         *
         * Two lines rather than an ellipsis, because these names are short and
         * the second word is usually the one that distinguishes them: "Task
         * Manager" cut to "Task M..." is worse than the same name on two
         * lines, and there is room underneath. */
        int line_h = gui_font_height(FONT_UI);
        int ty = r.y + pad + size + 6 * gui_scale();
        int room = r.w - 4 * gui_scale();

        char first[64], second[64];
        first[0] = second[0] = 0;

        if (gui_text_width(FONT_UI, a->name) <= room) {
            strlcpy(first, a->name, sizeof first);
        } else {
            /* Break at the last space that still fits, so the split falls
             * between words rather than inside one. */
            int best = -1;
            for (int at = 0; a->name[at]; at++) {
                if (a->name[at] != ' ') continue;
                char trial[64];
                int n = at < (int)sizeof trial - 1 ? at : (int)sizeof trial - 1;
                memcpy(trial, a->name, (size_t)n);
                trial[n] = 0;
                if (gui_text_width(FONT_UI, trial) <= room) best = at;
            }
            if (best > 0) {
                int n = best < (int)sizeof first - 1 ? best : (int)sizeof first - 1;
                memcpy(first, a->name, (size_t)n);
                first[n] = 0;
                strlcpy(second, a->name + best + 1, sizeof second);
            } else {
                /* One long word with nowhere to break.
                 *
                 * This branch said "shorten it instead" and then copied the
                 * name whole, so the only name it applies to - "Applications",
                 * which has no space to break at - was drawn at full width and
                 * cut off by the edge of the surface.  It read as
                 * "Applicatio", with no ellipsis to say that anything was
                 * missing, at every scale.
                 *
                 * Now it is actually shortened, with the ellipsis that says
                 * so.  Characters come off the end until the name and the
                 * ellipsis together fit. */
                int n = (int)strlen(a->name);
                if (n > (int)sizeof first - 4) n = (int)sizeof first - 4;

                for (; n > 0; n--) {
                    char trial[64];
                    memcpy(trial, a->name, (size_t)n);
                    trial[n] = 0;
                    strlcat(trial, "...", sizeof trial);
                    if (gui_text_width(FONT_UI, trial) <= room) {
                        strlcpy(first, trial, sizeof first);
                        break;
                    }
                }
                if (!n) strlcpy(first, "...", sizeof first);
            }
        }

        /* A dark halo keeps a label legible over any wallpaper. */
        for (int line = 0; line < 2; line++) {
            const char *text = line ? second : first;
            if (!text[0]) continue;

            int tw = gui_text_width(FONT_UI, text);
            if (tw > room) tw = room;
            int tx = r.x + (r.w - tw) / 2;
            int y = ty + line * line_h;

            /* Four offsets rather than eight.  The corners of a halo add
             * almost nothing a person can see and cost a full pass of text
             * each - and on a label this size the thicker outline reads as a
             * smudge rather than as a separation. */
            static const int ox[4] = { -1, 1, 0, 0 };
            static const int oy[4] = { 0, 0, -1, 1 };
            int spread = gui_scale();
            for (int k = 0; k < 4; k++)
                gui_text_clipped(s, FONT_UI, tx + ox[k] * spread,
                                 y + oy[k] * spread, room, text,
                                 RGB(0x06, 0x09, 0x10));
            gui_text_clipped(s, FONT_UI, tx, y, room, text, RGB(0xF2, 0xF6, 0xFC));
        }
    }
}

static void paint_menu(surface_t *s) {
    rect_t m = menu_rect();
    gui_shadow(s, m, 6);
    gui_round_rect_aa(s, m, 8, g_theme.window);
    gui_round_frame_aa(s, m, 8, g_theme.window_border);

    /* The menu's background is opaque and has just covered whatever was under
     * it - desktop icons included.  The overlap detector compares text
     * positions and cannot see that the icons beneath are now hidden, so
     * without this it reports every menu entry as landing on the icon it
     * covers.  Clearing the tracker here means the menu's text is checked
     * against the menu's own text, which is what "is the menu smushed" asks. */
    gui_text_frame_begin();

    /* Header: the brand mark, the name centred beside it, and a divider at the
     * bottom of the header band.  Every offset scales so nothing collides at
     * 2x - the old bare 46/13/40 drew the name over the mark and the divider
     * through the first row. */
    int hk = gui_scale();
    int hicon = 30 * hk;
    gui_icon(s, ICON_KESTREL, m.x + 16 * hk, m.y + (MENU_HEADER_H - hicon) / 2,
             hicon, g_theme.accent);
    gui_text(s, FONT_UI, m.x + 16 * hk + hicon + 12 * hk,
             m.y + (MENU_HEADER_H - gui_font_height(FONT_UI)) / 2,
             "KestrelOS", g_theme.text_bright);
    gui_hline(s, m.x + 10 * hk, m.y + MENU_HEADER_H - 1, m.w - 20 * hk,
              g_theme.window_border);

    int count = 0;
    const app_entry_t *a = desktop_apps(&count);
    for (int i = 0; i < count; i++) {
        rect_t r = menu_item_rect(i);
        if (i == menu_hover) gui_round_rect_aa(s, r, 5, g_theme.control_hover);

        int mk = gui_scale();
        int isz = MENU_ICON_SZ;
        gui_app_icon(s, a[i].icon, r.x + 10 * mk, r.y + (r.h - isz) / 2, isz);
        int text_x = r.x + 10 * mk + isz + 12 * mk;   /* clear of the icon */
        /* The name, then the description under it.
         *
         * The second line used to start four pixels above the bottom of the
         * first, so the descenders of one sat in the tops of the other: the
         * two lines were drawn over each other by design of the arithmetic
         * rather than by accident of the font.  A line of text occupies its
         * full height whether or not the particular letters in it reach the
         * edges, and the next line begins after that. */
        /* A name and the sentence belonging to it, set as one block.
         *
         * The two lines sit directly on top of one another and the space is
         * put between the entries instead.  With the gap inside a pair and the
         * gap between pairs both a few pixels, which is what it was, nothing
         * says which sentence belongs to which name - the eye groups by
         * proximity before it reads anything, and every description looked
         * like the heading of the entry below it. */
        int line = gui_font_height(FONT_UI);
        int top = r.y + (r.h - line * 2) / 2;

        gui_text(s, FONT_UI, text_x, top, a[i].name,
                 i == menu_hover ? g_theme.text_bright : g_theme.text);
        gui_text_clipped(s, FONT_UI, text_x, top + line,
                         r.x + r.w - text_x - 8 * mk, a[i].description,
                         g_theme.text_dim);
    }
}

static void paint_taskbar(wm_t *wm, surface_t *s) {
    rect_t t = taskbar_rect();

    /* A shallow gradient rather than one flat colour, and a light edge along
     * the top.  Neither is decoration: a bar that is a single value reads as a
     * gap in the screen rather than as a surface in front of the desktop, and
     * the edge is what gives it a near side. */
    gui_gradient_v(s, t, colour_shade(g_theme.taskbar, 8), g_theme.taskbar);
    gui_hline(s, t.x, t.y, t.w, colour_shade(g_theme.taskbar, 26));

    /* Opaque, and now covering the desktop beneath it - reset the overlap
     * tracker so the taskbar's own text is not reported as landing on the
     * icons it hides.  See the same call in paint_menu. */
    gui_text_frame_begin();

    /* Launcher: the brand mark and the name, both sized to the bar and centred
     * in it together.  The mark and the offsets beside it were bare pixel
     * numbers (20 wide, 10 in, text at 38) while the bar around them scaled, so
     * on a 2x display the logo was a fifth of the launcher's height and the name
     * sat off to one side of it rather than beside it. */
    const int lk = gui_scale();
    rect_t l = launcher_rect();
    bool hover = rect_contains(l, wm->mouse_x, wm->mouse_y);
    if (menu_open || hover)
        gui_round_rect_aa(s, l, 6 * lk, menu_open ? g_theme.accent : g_theme.taskbar_hover);
    int logo = l.h - 12 * lk;                       /* fills the bar, with a margin */
    int lx   = l.x + 12 * lk;
    gui_icon(s, ICON_KESTREL, lx, l.y + (l.h - logo) / 2, logo,
             menu_open ? g_theme.accent_text : g_theme.accent);
    gui_text(s, FONT_UI, lx + logo + 10 * lk,
             l.y + (l.h - gui_font_height(FONT_UI)) / 2,
             "KestrelOS", menu_open ? g_theme.accent_text : g_theme.taskbar_text);

    /* One button per window - counted first, so the buttons can share the row
     * out between them before any is drawn. */
    task_windows = 0;
    for (window_t *w = wm->bottom; w; w = w->above)
        if (w->visible) task_windows++;

    /* Diagnostic while the row is being reworked: say how the arithmetic came
     * out, once per change rather than per frame. */
    {
        static int said_for;
        if (task_windows != said_for) {
            said_for = task_windows;
            int fx = 0, cm = 0;
            int span = task_button_span(&fx, &cm);
            char line[120];
            snprintf(line, sizeof line,
                     "taskbar: %d window(s), buttons %d wide from x=%d, %d fit",
                     task_windows, span, fx, cm);
            log_write(1, "desktop", line);
        }
    }

    int index = 0;
    for (window_t *w = wm->bottom; w; w = w->above) {
        if (!w->visible) continue;
        rect_t r = task_button_rect(index);
        if (rect_empty(r)) break;

        bool active = (wm->focus == w && w->state != WIN_MINIMISED);
        bool over = (index == task_hover);

        /* Which window has focus, said once and clearly.  A two-pixel line
         * under the button is easy to miss on a bright screen and impossible
         * to see at a glance across a row of them; a filled shape with an
         * accent bar under it is the same information, read without looking
         * for it. */
        if (active) {
            gui_round_rect_aa(s, r, 6, g_theme.control_hover);
            rect_t bar = rect_make(r.x + 8, r.y + r.h - 3, r.w - 16, 3);
            gui_round_rect_aa(s, bar, 1, g_theme.accent);
        } else if (over) {
            gui_round_rect_aa(s, r, 6, g_theme.taskbar_hover);
        }

        /* The label begins after the icon, wherever the icon ends.
         *
         * It used to begin at a flat `r.x + 30` while the icon was placed at
         * `r.x + 8 * scale` and drawn `16 * scale` wide.  At scale two the icon
         * runs from 16 to 48 and the text started at 30 - a third of the way
         * through it.  Every taskbar button showed its icon painted over the
         * first letter of its title, which is what "Files" looked like when it
         * read as "iles".
         *
         * Derived from the icon's own geometry now, so the two cannot drift
         * apart again. */
        colour_t tc = w->state == WIN_MINIMISED ? g_theme.text_dim : g_theme.taskbar_text;
        const int k = gui_scale();
        int ts = 16 * k;
        bool icon_only = (r.w < 16 * k + 16 * k + gui_text_width(FONT_UI, "MMMM"));
        int icon_x = icon_only ? r.x + (r.w - ts) / 2 : r.x + 8 * k;
        gui_app_icon(s, w->icon, icon_x, r.y + (r.h - ts) / 2, ts);

        /* The label, when there is room for one.  On a crowded row the
         * buttons shrink to icon width and a two-letter fragment of a title
         * says less than the icon it would crowd, so past that point the icon
         * stands alone, centred. */
        int text_x = icon_x + ts + 8 * k;
        int text_w = r.x + r.w - text_x - 8 * k;
        if (text_w >= gui_text_width(FONT_UI, "MMM")) {
            gui_text_clipped(s, FONT_UI, text_x,
                             r.y + (r.h - gui_font_height(FONT_UI)) / 2 + 1 * k,
                             text_w, w->title, tc);
        } else {
            /* Re-centre the icon in the narrow button; it was placed for a
             * row that had text beside it. */
        }
        index++;
    }

    /* Clock and a power button on the right. */
    const int k = gui_scale();
    char stamp[32], hhmm[8], date[16];
    format_time(stamp, sizeof stamp, time_now());
    /* "YYYY-MM-DD HH:MM:SS" -> "HH:MM" on top, "YYYY-MM-DD" below it. */
    snprintf(hhmm, sizeof hhmm, "%.5s", stamp + 11);
    snprintf(date, sizeof date, "%.10s", stamp);

    rect_t power = power_rect();
    bool power_hover = rect_contains(power, wm->mouse_x, wm->mouse_y);
    if (power_hover) gui_round_rect_aa(s, power, 6 * k, g_theme.error);
    /* The glyph is a real image (see tools/genicons.py), scaled to the button
     * and tinted white on the red hover so it reads against it. */
    int isz = power.h * 11 / 20;
    gui_app_icon_tinted(s, ICON_POWER, power.x + (power.w - isz) / 2,
                        power.y + (power.h - isz) / 2, isz,
                        power_hover ? RGB(0xFF, 0xFF, 0xFF) : g_theme.taskbar_text);

    /* Two stacked lines when the bar is tall enough: the time, and the date dim
     * beneath it - a clock that reads at a glance and gives the day without
     * opening anything.  On a short bar the date would collide with the time,
     * so there it falls back to the time alone, centred. */
    rect_t cr = clock_rect();
    int fh = gui_font_height(FONT_UI);
    int block = fh * 2 + 2 * k;
    if (block + 4 * k <= cr.h) {
        int cy = cr.y + (cr.h - block) / 2;
        gui_text_centred(s, FONT_UI, rect_make(cr.x, cy, cr.w, fh),
                         hhmm, g_theme.taskbar_text);
        gui_text_centred(s, FONT_UI, rect_make(cr.x, cy + fh + 2 * k, cr.w, fh),
                         date, g_theme.text_dim);
    } else {
        /* Two lines will not fit this bar - keep both facts on one line rather
         * than dropping the date: "MM-DD  HH:MM". */
        char both[24];
        snprintf(both, sizeof both, "%.5s  %s", stamp + 5, hhmm);
        gui_text_centred(s, FONT_UI, cr, both, g_theme.taskbar_text);
    }
}

/* ------------------------------------------------------------------ hooks */

/* The wallpaper, drawn once and kept.
 *
 * It is the same picture every frame, and producing it costs a hash and a
 * blend for every pixel - eight hundred thousand of them at this size, several
 * million at a larger one.  Doing that again for every repaint, including the
 * ones caused by nothing more than the pointer moving, is most of what makes a
 * desktop feel slow.  Drawing it once into a surface turns the per-frame cost
 * into a copy, which the row primitives do several pixels at a time. */
static surface_t *wallpaper_cache;
static int        cached_index = -1;
static int        cached_w, cached_h;

static void paint_background(wm_t *wm, surface_t *s, rect_t area) {
    (void)wm;

    if (!wallpaper_cache || cached_index != g_wallpaper ||
        cached_w != s->width || cached_h != s->height) {
        if (wallpaper_cache) surface_destroy(wallpaper_cache);
        wallpaper_cache = surface_create_target(s->width, s->height, s->gpu!=NULL);
        if (wallpaper_cache) {
            surface_reset_clip(wallpaper_cache);
            desktop_paint_wallpaper(wallpaper_cache,
                                    rect_make(0, 0, s->width, s->height),
                                    g_wallpaper);
            cached_index = g_wallpaper;
            cached_w = s->width;
            cached_h = s->height;
        }
    }

    if (wallpaper_cache) {
        rect_t a = rect_intersection(area, surface_clip(s));
        gui_blit_rect(s,wallpaper_cache,a,a.x,a.y);
    } else {
        /* No memory for the cache: draw it the slow way rather than nothing. */
        desktop_paint_wallpaper(s, area, g_wallpaper);
    }

    paint_desktop_icons(s, area);
}

/* What the input hardware is doing, drawn while there is nothing to drive the
 * desktop with.
 *
 * The numbers are chosen to separate the three things that look identical from
 * outside: a device that was never found, one that was found and claimed but
 * sends nothing, and one that sends reports which are then dropped somewhere
 * above.  The count of reports moving while the pointer stays still means the
 * fault is above the driver; the count staying at zero means it is below. */
/* Whether the machine is missing something worth saying so about, and the one
 * sentence that says it. */
static bool health_problem(const ksysinfo_t *si, const char **headline) {
    /* No pointing device at all.  A real fault, and true the moment the
     * machine starts, so it is said straight away. */
    if (!si->pointers) {
        *headline = "No pointing device was found";
        return true;
    }

    /* A device was found and has sent nothing.
     *
     * This used to be the same complaint, and it was wrong in the way that
     * matters: a machine that has just started and has not been touched has
     * sent no reports because nobody has moved anything, not because anything
     * is broken.  Every fresh boot accused itself of a fault, and the accusation
     * went away the instant the mouse moved - which is the behaviour of a
     * warning nobody should trust.
     *
     * The comment above this function says its whole purpose is to separate a
     * device that was never found from one that was found and says nothing.
     * It was not doing that.  Now it is, and the second case waits half a
     * minute before saying anything, because before then "nothing has been
     * touched yet" is the likelier explanation by far.
     */
    if (!si->hid_reports && si->uptime_ms > 30000) {
        *headline = "An input device is present but has sent nothing";
        return true;
    }

    if (!si->logging) {
        *headline = "This boot is not being recorded to disk";
        return true;
    }
    return false;
}

/* Whether the details are wanted.  Collapsed is the resting state: a machine
 * that is working should not be explaining itself, and a machine that is not
 * should say what is wrong in one line before it says it in twelve. */
static bool notice_expanded;
static bool notice_dismissed;

static rect_t notice_rect(void) {
    /* Scaled with everything else.  Left at a fixed width it held text that
     * had grown and ran off the side of the screen - which is the whole
     * failure mode of scaling one half of a layout. */
    int k = gui_scale();
    int w = (notice_expanded ? 560 : 360) * k;
    int h = (notice_expanded ? 210 : 40) * k;
    /* Bottom right, above the taskbar, where a notification belongs - not
     * across the middle of the desktop, which is where this used to sit and
     * why a machine with a full log looked like a machine with a fault. */
    return rect_make(g_display.back->width - w - 20 * k,
                     g_display.back->height - TASKBAR_H - h - 20 * k, w, h);
}

static void paint_input_status(surface_t *s) {
    ksysinfo_t si;
    if (sysinfo(&si) != 0) return;

    const char *headline = NULL;
    if (!health_problem(&si, &headline)) { notice_dismissed = false; return; }
    if (notice_dismissed) return;

    rect_t r = notice_rect();
    int w = r.w;

    gui_soft_shadow(s, r, 10, 70, 10);
    gui_round_rect_aa(s, r, 10, colour_mix(g_theme.window, RGB(0, 0, 0), 30));
    gui_round_frame_aa(s, r, 10, g_theme.warning);

    /* A close affordance, and a hint that there is more behind it. */
    rect_t close = rect_make(r.x + r.w - 30 * gui_scale(),
                             r.y + 10 * gui_scale(),
                             20 * gui_scale(), 20 * gui_scale());
    gui_text_centred(s, FONT_UI, close, "x", g_theme.text_dim);

    int k = gui_scale();
    int x = r.x + 16 * k, y = r.y + 12 * k;

    if (!notice_expanded) {
        gui_text_clipped(s, FONT_UI, x, y, w - 54 * k, headline, g_theme.warning);
        return;
    }

    gui_text(s, FONT_UI, x, y, headline, g_theme.text_bright);
    y += gui_font_height(FONT_UI) + 6;

    char line[128];
    snprintf(line, sizeof line,
             "keyboards %u   pointers %u   reports %u   dropped %u",
             si.keyboards, si.pointers, si.hid_reports, si.hid_rejected);
    gui_text(s, FONT_UI, x, y, line, g_theme.text);
    y += gui_font_height(FONT_UI) + 4;

    if (si.hid_last_len) {
        int n = snprintf(line, sizeof line, "last report:");
        for (int i = 0; i < si.hid_last_len && n < (int)sizeof line - 4; i++)
            n += snprintf(line + n, sizeof line - n, " %02x", si.hid_last[i]);
        gui_text(s, FONT_UI, x, y, line, g_theme.text_dim);
    } else {
        gui_text(s, FONT_UI, x, y, "no report has arrived from any device",
                 g_theme.text_dim);
    }
    y += gui_font_height(FONT_UI) + 6;

    /* The same question asked of the controller rather than of the driver.
     * Events posted at all, transfers among them, what the last one said, and
     * whether the endpoint is still Running - four numbers that between them
     * name which layer is at fault. */
    snprintf(line, sizeof line,
             "controller: %u events, %u transfers, last code %u, endpoint %s",
             si.usb_events, si.usb_transfers, si.usb_last_code,
             si.usb_ep_state == 1 ? "running"
             : si.usb_ep_state == 2 ? "HALTED" : "DISABLED");
    gui_text_clipped(s, FONT_UI, x, y, w - 32, line, g_theme.text_dim);
    y += gui_font_height(FONT_UI) + 6;

    const char *verdict =
        !si.keyboards && !si.pointers
            ? "nothing was claimed - the device or its driver is missing"
        : si.usb_ep_state != 1
            ? "the endpoint is not running - the controller refused it"
        : !si.usb_transfers
            ? "the controller has completed no transfer at all"
        : si.hid_rejected
            ? "reports arrive but are turned away with no driver to take them"
        : !si.hid_reports
            ? "transfers complete but no report is delivered"
            : "reports are arriving - the fault is above the driver";
    gui_text_clipped(s, FONT_UI, x, y, w - 32, verdict, g_theme.warning);
    y += gui_font_height(FONT_UI) + 8;

    /* And where the log is going, which is the other thing that cannot be
     * asked without a keyboard - and the reason these numbers are on a screen
     * being photographed rather than in a file. */
    if (si.logging && si.log_path[0])
        snprintf(line, sizeof line, "log on disk: %s    USB disks: %u    %s",
                 si.log_path, si.usb_disks, si.storage_stage);
    else
        snprintf(line, sizeof line, "log on disk: NO    USB disks: %u    %s",
                 si.usb_disks, si.storage_stage);
    gui_text_clipped(s, FONT_UI, x, y, w - 32, line,
                     si.logging ? g_theme.text_dim : g_theme.warning);
}

static void paint_overlay(wm_t *wm, surface_t *s, rect_t area) {
    paint_input_status(s);
    if (rect_intersects(taskbar_rect(), area)) paint_taskbar(wm, s);
    if (menu_open && rect_intersects(menu_rect(), area)) paint_menu(s);
}

/* Returns true when the desktop consumed the event. */
static bool background_event(wm_t *wm, const kinput_event_t *ev) {
    (void)wm;


    (void)ev;
    return false;
}

static void launch(wm_t *wm, const app_entry_t *a) {
    menu_open = false;
    wm_damage_all(wm);
    if (a && a->launch) a->launch(wm);
}

/* Intercepts that must run before the window manager sees the event, because
 * the taskbar and the menu sit above every window. */
static void desktop_update_hover(wm_t *wm, const kinput_event_t *ev) {
    int old_task = task_hover, old_menu = menu_hover, old_desk = desk_hover;
    rect_t bar = taskbar_rect(), menu = menu_rect();
    bool on_bar = rect_contains(bar, ev->x, ev->y);
    bool on_menu = menu_open && rect_contains(menu, ev->x, ev->y);

    task_hover = -1;
    if (on_bar) for (int i = 0; i < task_windows && i < WM_MAX_WINDOWS; i++) {
        rect_t r = task_button_rect(i);
        if (rect_empty(r)) break;
        if (rect_contains(r, ev->x, ev->y)) { task_hover = i; break; }
    }
    menu_hover = -1;
    if (on_menu) {
        int count = 0;
        desktop_apps(&count);
        for (int i = 0; i < count; i++)
            if (rect_contains(menu_item_rect(i), ev->x, ev->y)) { menu_hover = i; break; }
    }
    desk_hover = -1;
    if (!on_bar && !on_menu && !wm_window_at(wm, ev->x, ev->y)) {
        int n = desktop_icon_count();
        for (int i = 0; i < n; i++)
            if (rect_contains(desktop_icon_rect(i), ev->x, ev->y)) { desk_hover = i; break; }
    }

    /* These painters contain their changing highlight within the hit rect.
     * Recompose BOTH old and new locations, not all monitors. The compositor
     * restores the background and redraws overlying surfaces in each region. */
    if (task_hover != old_task) {
        if (old_task >= 0) wm_damage(wm, task_button_rect(old_task));
        if (task_hover >= 0) wm_damage(wm, task_button_rect(task_hover));
    }
    if (menu_hover != old_menu) {
        if (old_menu >= 0) wm_damage(wm, menu_item_rect(old_menu));
        if (menu_hover >= 0) wm_damage(wm, menu_item_rect(menu_hover));
    }
    if (desk_hover != old_desk) {
        if (old_desk >= 0) wm_damage(wm, desktop_icon_rect(old_desk));
        if (desk_hover >= 0) wm_damage(wm, desktop_icon_rect(desk_hover));
    }
    /* The interceptor runs before handle_event updates the pointer. These
     * buttons derive their highlight from coordinates, not the indices above. */
    rect_t launcher = launcher_rect(), power = power_rect();
    if (!menu_open && rect_contains(launcher, wm->mouse_x, wm->mouse_y) !=
                      rect_contains(launcher, ev->x, ev->y)) wm_damage(wm, launcher);
    if (rect_contains(power, wm->mouse_x, wm->mouse_y) !=
        rect_contains(power, ev->x, ev->y)) wm_damage(wm, power);
}

static bool desktop_intercept(wm_t *wm, const kinput_event_t *ev) {
    /* The health notice sits above every window, so it takes its own clicks
     * before anything underneath sees them - otherwise closing it would also
     * activate whatever it happens to be covering. */
    if (ev->type == KEV_MOUSE_BUTTON && ev->pressed && !notice_dismissed) {
        ksysinfo_t si;
        const char *ignored = NULL;
        if (sysinfo(&si) == 0 && health_problem(&si, &ignored)) {
            rect_t r = notice_rect();
            rect_t close = rect_make(r.x + r.w - 30 * gui_scale(),
                             r.y + 10 * gui_scale(),
                             20 * gui_scale(), 20 * gui_scale());

            if (rect_contains(close, ev->x, ev->y)) {
                notice_dismissed = true;
                wm_damage(wm, r);
                return true;
            }
            if (rect_contains(r, ev->x, ev->y)) {
                notice_expanded = !notice_expanded;
                wm_damage(wm, r);
                wm_damage(wm, notice_rect());
                return true;
            }
        }
    }

    /* Shortcuts the desktop claims before any window sees them.  The console
     * one matters: without a way out that does not depend on finding a button,
     * a broken window could leave the machine unusable. */
    if (ev->type == KEV_KEY && ev->pressed && (ev->mods & KMOD_CTRL) && (ev->mods & KMOD_ALT)) {
        uint32_t key = ev->code;
        /* Ctrl turns letters into control codes, so accept either form. */
        if (key == 'c' || key == 'C' || key == 3) {
            desktop_request_console(wm, "/bin/shell");
            return true;
        }
        if (key == 't' || key == 'T' || key == 20) {
            app_terminal_launch(wm);
            return true;
        }
        if (key == 'e' || key == 'E' || key == 5) {
            app_events_launch(wm);
            return true;
        }
    }

    if (ev->type == KEV_MOUSE_MOVE) {
        desktop_update_hover(wm, ev);

        /* Hovering the taskbar or menu must not reach a window underneath. */
        if (rect_contains(taskbar_rect(), ev->x, ev->y)) return false;
        if (menu_open && rect_contains(menu_rect(), ev->x, ev->y)) return false;
        return false;
    }

    if (ev->type != KEV_MOUSE_BUTTON || !ev->pressed) return false;

    /* The launcher menu. */
    if (menu_open) {
        rect_t m = menu_rect();
        if (rect_contains(m, ev->x, ev->y)) {
            int count = 0;
            const app_entry_t *a = desktop_apps(&count);
            for (int i = 0; i < count; i++) {
                if (!rect_contains(menu_item_rect(i), ev->x, ev->y)) continue;
                launch(wm, &a[i]);
                return true;
            }
            return true;
        }
        if (!rect_contains(launcher_rect(), ev->x, ev->y)) {
            menu_open = false;
            wm_damage_all(wm);
            /* Fall through so the click also lands where the user aimed. */
        }
    }

    rect_t t = taskbar_rect();
    if (rect_contains(t, ev->x, ev->y)) {
        if (rect_contains(launcher_rect(), ev->x, ev->y)) {
            menu_open = !menu_open;
            wm_damage_all(wm);
            return true;
        }
        if (rect_contains(power_rect(), ev->x, ev->y)) {
            desktop_message(wm, "Shut down",
                            "Restart keeps this boot's log: it is held in\n"
                            "memory and written to the stick the next time\n"
                            "the machine starts, and memory survives a\n"
                            "restart but not the power going off.\n\n"
                            "Anything not written to disk is flushed first.",
                            ICON_POWER);
            return true;
        }
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            rect_t r = task_button_rect(i);
            if (rect_empty(r)) break;
            if (!rect_contains(r, ev->x, ev->y)) continue;

            window_t *w = wm_window_by_index(wm, i);
            if (!w) break;
            if (w->state == WIN_MINIMISED) wm_restore(wm, w);
            else if (wm->focus == w) wm_minimise(wm, w);
            else wm_focus(wm, w);
            return true;
        }
        return true;    /* an empty part of the taskbar swallows the click */
    }

    /* Desktop icons: one click selects, a second launches.
     *
     * Only where no window covers the point.  The test below used to be on the
     * icon rectangle alone, so an icon stayed clickable through whatever was
     * drawn on top of it: clicking inside an open window, over the place an
     * icon happens to sit, selected that icon and a second click launched the
     * program - from a part of the screen showing something else entirely.
     *
     * The window manager already answers this exactly, and the very next
     * statement in this function was already asking it.  The icons simply were
     * not. */
    if (wm_window_at(wm, ev->x, ev->y)) return false;

    int n = desktop_icon_count();
    for (int i = 0; i < n; i++) {
        if (!rect_contains(desktop_icon_rect(i), ev->x, ev->y)) continue;
        if (desk_selected == i) { launch(wm, desktop_icon_app(i)); desk_selected = -1; }
        else desk_selected = i;
        wm_damage_all(wm);
        return true;
    }

    if (!wm_window_at(wm, ev->x, ev->y) && desk_selected >= 0) {
        desk_selected = -1;
        wm_damage_all(wm);
    }
    return false;
}

/* ------------------------------------------------------------ message box */

typedef struct {
    char  title[64];
    char  body[512];
    icon_id icon;
    bool  confirm;      /* a power-off prompt gets two buttons */
} msgbox_t;

static bool msgbox_proc(window_t *w, const wevent_t *ev) {
    msgbox_t *m = w->data;
    surface_t *s = w->canvas;

    const int k = gui_scale();
    rect_t ok = rect_make(s->width - 110 * k, s->height - 46 * k, 96 * k, 32 * k);
    rect_t cancel = rect_make(s->width - 216 * k, s->height - 46 * k, 96 * k, 32 * k);

    /* Restarting is not the same as powering off, and on this system the
     * difference decides whether there is a log to read.
     *
     * The kernel keeps its log in memory and the LOADER writes it to the boot
     * volume the next time the machine starts.  Memory keeps its contents
     * across a restart and loses them when the power goes - so shutting down
     * after something interesting happened throws away the account of it, and
     * there was no other way to leave. */
    rect_t restart = rect_make(s->width - 322 * k, s->height - 46 * k, 96 * k, 32 * k);

    switch (ev->kind) {
    case WE_PAINT: {
        gui_clear(s, g_theme.window);
        gui_icon(s, m->icon ? m->icon : ICON_INFO, 20 * gui_scale(),
                 22 * gui_scale(), 34 * gui_scale(), g_theme.accent);

        /* Wrap the body on its newlines.
         *
         * Past the icon, not over it.  The icon is drawn at 20*k with a width
         * of 34*k, so at three times the size it reaches x=162 while this text
         * started at a flat 70 - the words were painted straight across it. */
        int y = 20 * k;
        int text_x = (20 + 34 + 16) * k;
        const char *p = m->body;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            gui_text_n(s, FONT_UI, text_x, y, p, len, g_theme.text);
            y += gui_font_height(FONT_UI) + 2 * k;
            if (!nl) break;
            p = nl + 1;
        }

        gui_button(s, ok, m->confirm ? "Shut down" : "OK",
                   rect_contains(ok, ev->x, ev->y), false, true);
        if (m->confirm) {
            gui_button(s, cancel, "Cancel", rect_contains(cancel, ev->x, ev->y),
                       false, true);
            gui_button(s, restart, "Restart",
                       rect_contains(restart, ev->x, ev->y), false, true);
        }
        return false;
    }

    case WE_MOUSE_MOVE:
        return true;      /* repaint so the hover state follows the pointer */

    case WE_MOUSE_DOWN:
        if (rect_contains(ok, ev->x, ev->y)) {
            if (m->confirm) { sync(); poweroff(); }
            wm_request_close(w->wm, w);
            return false;
        }
        if (m->confirm && rect_contains(restart, ev->x, ev->y)) {
            sync();
            reboot();
            return false;
        }
        if (m->confirm && rect_contains(cancel, ev->x, ev->y)) {
            wm_request_close(w->wm, w);
            return false;
        }
        return false;

    case WE_KEY_DOWN:
        if (ev->key == '\n' || ev->key == 27) { wm_request_close(w->wm, w); return false; }
        return false;

    case WE_CLOSE:
        free(m);
        return false;

    default:
        return false;
    }
}

void desktop_message(wm_t *wm, const char *title, const char *body, icon_id icon) {
    msgbox_t *m = calloc(1, sizeof *m);
    if (!m) return;
    strlcpy(m->title, title, sizeof m->title);
    strlcpy(m->body, body, sizeof m->body);
    m->icon = icon;
    m->confirm = (icon == ICON_POWER);

    /* Wide enough for three buttons at the interface's own size, and tall
     * enough for the longest thing this box says. */
    window_t *w = desktop_new_window(wm, title, icon,
                                     m->confirm ? 470 : 430,
                                     m->confirm ? 250 : 190, msgbox_proc, m);
    if (!w) { free(m); return; }
    w->resizable = false;
}

/* ------------------------------------------------------------------ shared */

window_t *desktop_new_window(wm_t *wm, const char *title, icon_id icon,
                             int w, int h, window_proc proc, void *data) {
    /* Cascade so a second window of the same app is not hidden by the first. */
    static int cascade;
    int k = gui_scale();
    int sw = g_display.back->width, sh = g_display.back->height - TASKBAR_H;

    /* The size an application asks for is in interface pixels, like every
     * other measurement it makes: a window sized for eighty columns of text
     * has to grow with the text, or the same eighty columns stop fitting and
     * every line wraps one word early. */
    w *= k;
    h *= k;

    if (w > sw - 60 * k) w = sw - 60 * k;
    if (h > sh - 60 * k) h = sh - 60 * k;

    int x = (120 + (cascade % 6) * 34) * k;
    int y = (70 + (cascade % 6) * 28) * k;
    cascade++;

    if (x + w > sw - 20 * k) x = sw - w - 20 * k;
    if (y + h > sh - 20 * k) y = sh - h - 20 * k;
    if (x < 10 * k) x = 10 * k;
    if (y < 10 * k) y = 10 * k;

    return wm_create(wm, title, icon, rect_make(x, y, w, h), proc, data);
}

void desktop_request_console(wm_t *wm, const char *program) {
    strlcpy(pending_console, program, sizeof pending_console);
    wm_stop(wm);
}

/* -------------------------------------------------------------------- main */

/* Every few seconds, what the compositor has been costing.  It goes to the
 * event log rather than the screen, so it is there to read afterwards without
 * being in the way. */
static void report_frames(void) {
    static uint64_t last_report;
    uint64_t now = uptime_ms();
    if (!last_report) { last_report = now; return; }
    if (now - last_report < 5000) return;
    last_report = now;

    char line[160], detail[192];
    wm_frame_report(line, sizeof line);
    snprintf(detail, sizeof detail, "compositor: %s", line);
    log_write(1, "desktop", detail);
    wm_frame_reset();
}

/* The screen changed size: the taskbar runs the width of it and the work area
 * is everything above.  The wallpaper has to be made again too, since the one
 * that was kept is the wrong shape now. */
static rect_t query_work_area(wm_t *wm, int width, int height) {
    (void)wm;
    return rect_make(0, 0, width, height - TASKBAR_H);
}

static void on_resize(wm_t *wm, int width, int height) {
    (void)wm; (void)width; (void)height;
    if (wallpaper_cache) {
        surface_destroy(wallpaper_cache);
        wallpaper_cache = NULL;
        cached_index = -1;
    }
}

/* Move a window once, on purpose, and see whether the adapter did it.
 *
 * This is here because of what could not otherwise be established.  The
 * compositor asks the display adapter to relocate a window's pixels rather
 * than redrawing them, and the only way that normally happens is somebody
 * dragging a title bar - which the automated path into this desktop cannot
 * reliably do.  So "does the adapter draw any of this desktop" had no answer
 * that did not depend on a person being there.
 *
 * It waits for the opening animation to finish, because a window that is still
 * animating is being blended rather than moved and is deliberately refused.
 * That was not obvious the first time: the check ran at startup, found the
 * terminal mid-animation, and reported that the adapter does nothing - which
 * was true of that moment and wrong about the system.
 */
static void adapter_check(wm_t *wm) {
    static bool done;
    if (done) return;

    window_t *w = wm->top;
    if (!w || w->anim.kind != ANIM_NONE) return;   /* not yet settled */
    done = true;

    uint64_t before = 0, after = 0;
    wm_adapter_counts(&before, NULL);

    rect_t home = w->frame;
    wm_move_window(wm, w, home.x + 4, home.y + 4);
    wm_move_window(wm, w, home.x, home.y);

    wm_adapter_counts(&after, NULL);

    char line[176];
    if (after > before)
        snprintf(line, sizeof line,
                 "the display adapter moved this desktop's pixels: %llu "
                 "rectangle(s) for two window moves",
                 (unsigned long long)(after - before));
    else if (wm->display->back->gpu)
        snprintf(line, sizeof line,
                 "GPU-resident window composition is active; legacy framebuffer-copy counters do not measure this path");
    else
        snprintf(line, sizeof line,
                 "the display adapter moved nothing; the processor is drawing "
                 "every pixel of this desktop");
    log_write(1, "wm", line);
}

static void on_tick(wm_t *wm) {
    adapter_check(wm);

    report_frames();

    /* The taskbar only needs redrawing when something on it has changed.
     * Repainting it on every tick means a strip the full width of the screen,
     * eight times a second, for a clock that moves once a minute - and because
     * damage used to be a single bounding box, that strip dragged the rest of
     * the frame along with it. */
    static uint64_t last_minute = 0;
    static int last_window_count = -1;

    uint64_t minute = time_now() / 60;

    if (minute != last_minute || g_wm.count != last_window_count) {
        last_minute = minute;
        last_window_count = g_wm.count;
        wm_damage(wm, taskbar_rect());
    }
}

/* The card drawing for this program, if there is one to draw.
 *
 * Everything else the window system puts on screen it draws itself, pixel by
 * pixel, and then names the rectangle that changed.  This asks the graphics
 * card to draw instead: three corners go down through the system to the card,
 * the card works out which pixels lie between them and what colour each one
 * is, and the result appears in the same memory the display is reading.
 *
 * It is checked by reading those pixels back afterwards.  The corners carry
 * three different colours, so the middle of the shape has to be a mixture of
 * all three - which is something this program could not have produced by
 * accident, because it never touched those pixels at all.
 */
/* Say which card was found and why it is not drawing.
 *
 * "No graphics card to draw with" is true of a machine with no card and false
 * of a machine with an RTX 5070 Ti in it that this system cannot yet drive -
 * and those two need completely different things done about them.  A message
 * that reads the same for both sends its reader looking in the wrong place,
 * which is the whole cost of a message that is nearly right.
 *
 * The kernel already keeps one honest sentence per card about what is and is
 * not driven.  This prints it instead of guessing.
 */
static void say_why_not_the_card(void) {
    kgpuinfo_t g;
    int found = 0;

    for (uint32_t i = 0; enum_gpu(i, &g) == 0; i++) {
        found++;
        char line[256];
        snprintf(line, sizeof line,
                 "%s is present but is not drawing this screen; every pixel "
                 "is drawn by the processor. %s",
                 g.name[0] ? g.name : "a graphics card",
                 g.note[0] ? g.note : "no reason was recorded");
        log_write(1, "desktop", line);
    }

    if (!found)
        log_write(1, "desktop", "there is no graphics card here at all, so "
                                "every pixel on this screen is drawn by the "
                                "processor");
}

static void try_the_card(void) {
    int card = gpu_can_draw();
    if (card <= 0) {
        say_why_not_the_card();
        return;
    }

    /* Across and up from the middle of the screen, from minus one to one. */
    static const kvertex_t corners[3] = {
        {  0.00f,  0.60f, 0.5f, 1.0f,  1.0f, 0.0f, 0.0f, 1.0f },
        {  0.55f, -0.50f, 0.5f, 1.0f,  0.0f, 1.0f, 0.0f, 1.0f },
        { -0.55f, -0.50f, 0.5f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f },
    };

    int drew = gpu_draw(corners, 1);
    if (drew != 1) {
        char line[128];
        snprintf(line, sizeof line,
                 "the card would not draw for this program (%d)", drew);
        log_write(2, "desktop", line);
        return;
    }

    /* Read the live NVKMS allocation on native NVIDIA, never its CPU shadow.
     * Other adapters expose their scanout directly and retain the old read. */
    int cx = g_display.screen.width / 2;
    int cy = g_display.screen.height / 2;
    colour_t px = 0;
    if (card == 2) {
        if (gpu_read_pixel(cx, cy, &px) != 0) {
            log_write(2, "desktop", "the card accepted the triangle but live VRAM readback failed");
            return;
        }
    } else {
        px = g_display.screen.pixels[(size_t)cy * g_display.screen.stride + cx];
    }
    int r = (int)((px >> 16) & 0xFF);
    int g = (int)((px >> 8) & 0xFF);
    int b = (int)(px & 0xFF);
    int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);

    /* A distinctive opaque rectangle uploaded through the card's 2D/present
     * path, followed by a read from the actual live allocation. */
    {
        static unsigned int patch[32 * 32];
        for (int i = 0; i < 32 * 32; i++) patch[i] = 0xff19d3ffu;

        int px = g_display.screen.width / 2 - 64;
        int py = g_display.screen.height / 2 - 64;
        kimage_t img = { patch, 32, 32, px, py, 32, 32, 0 };

        if (gpu_image(&img) == 0) {
            unsigned int after = 0;
            int read_ok = card == 2
                        ? gpu_read_pixel(px + 16, py + 16, &after) == 0
                        : ((after = g_display.screen.pixels[
                              (size_t)(py + 16) * g_display.screen.stride +
                              (px + 16)]), 1);
            char l2[224];
            if (read_ok && after == 0xff19d3ffu)
                snprintf(l2, sizeof l2,
                         "the card's 2D/present path returned the exact live "
                         "VRAM pixel %#x", after);
            else
                snprintf(l2, sizeof l2,
                         "the card accepted the 2D patch but live VRAM returned %#x",
                         after);
            log_write(read_ok && after == 0xff19d3ffu ? 1 : 2, "desktop", l2);
        }
    }

    char line[224];
    if (lo > 30 && hi < 160 && hi - lo < 70)
        snprintf(line, sizeof line,
                 "a program drew on the graphics card: three corners handed "
                 "down, and the middle of the shape came back %d,%d,%d - a "
                 "mixture of all three, which this program never wrote",
                 r, g, b);
    else
        snprintf(line, sizeof line,
                 "the card drew for this program but the middle of the shape "
                 "is %d,%d,%d, which is not three corners mixed", r, g, b);
    log_write(1, "desktop", line);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Whatever was chosen last time, if this machine has been used before. */
    {
        /* This also restores the interface size, and has to run before any
         * window exists: everything is measured against it. */
        int theme = -1, accent = 0, wallpaper = -1;
        if (desktop_prefs_load(&theme, &accent, &wallpaper) && theme >= 0) {
            theme_apply(theme, accent_at(accent));
            if (wallpaper >= 0) desktop_set_wallpaper(wallpaper);
        } else {
            theme_set_dark();
        }
    }

    if (!display_open_gpu(&g_display)) {
        fprintf(STDERR_FD,
                "desktop: cannot take over the display: %s\n"
                "The graphical shell needs a linear framebuffer and /dev/input.\n",
                strerror(errno));
        return 1;
    }

    /* Kernel messages would otherwise paint straight over the desktop, so the
     * console echo is silenced while the display belongs to us.  Everything
     * still reaches the ring buffer, the serial port and the Event Viewer. */
    log_console_level(5);
    console_cursor(false);
    log_write(1, "desktop", "graphical shell started");

    {
        char speed[192], line[256];
        display_measure(&g_display, speed, sizeof speed);
        snprintf(line, sizeof line,
                 "%s; %u bpp, red at %u, green at %u, blue at %u, pitch %u",
                 speed, g_display.info.bpp, g_display.info.red_shift,
                 g_display.info.green_shift, g_display.info.blue_shift,
                 g_display.info.pitch);
        log_write(1, "desktop", line);
    }

    /* Before the desktop paints over everything: whether the card will draw
     * for a program, and proof either way.  What it puts on screen lasts only
     * until the first full repaint below. */
    try_the_card();

    /* Decode the app-icon images before the first paint so the desktop, the
     * taskbar and the window title bars show real icons rather than glyphs. */
    desktop_load_app_icons();

    wm_init(&g_wm, &g_display);
    g_wm.work_area = rect_make(0, 0, g_display.back->width, g_display.back->height - TASKBAR_H);
    g_wm.on_resize = on_resize;
    g_wm.query_work_area = query_work_area;
    g_wm.paint_background = paint_background;
    g_wm.paint_overlay = paint_overlay;
    g_wm.background_event = background_event;
    g_wm.on_tick = on_tick;
    g_wm.taskbar_slot = taskbar_slot_for;

    /* Nothing here now.  A message box that says "no pointer" is a box that
     * cannot be dismissed on a machine with no pointer, and it says the same
     * thing forever whether or not the situation changes.  What replaces it is
     * a live panel drawn in paint_overlay, which keeps answering the question
     * while the machine runs - and can be read off a photograph, which on a
     * machine nobody can type into is the only channel there is. */

    desktop_autostart(&g_wm);

    for (;;) {
        /* The window manager loop returns when an app asks for the console. */
        g_wm.running = true;
        pending_console[0] = 0;

        /* Intercepts run first; wm_run only sees what the desktop did not use. */
        g_wm.background_event = desktop_intercept;
        wm_run(&g_wm);

        if (!pending_console[0]) break;

        /* Hand the screen back to the text console, run the program, then
         * take the display and the log echo back.  Giving up ownership rather
         * than merely stopping is what lets the program that runs in the
         * foreground have the screen if it wants one - a graphical Windows
         * program, for instance. */
        log_console_level(1);
        display_give_up(&g_display);
        console_cursor(true);
        console_clear();

        const char *leaf = strrchr(pending_console, '/');
        const char *argvp[1] = { leaf ? leaf + 1 : pending_console };
        run(pending_console, argvp, 1);

        log_console_level(5);
        if (!display_take(&g_display)) {
            /* Somebody else has it and has not finished; there is nothing
             * sensible to draw on. */
            printf("The screen is in use by another program.\n");
            break;
        }
        console_cursor(false);
        console_clear();
        wm_damage_all(&g_wm);
    }

    log_console_level(1);
    console_cursor(true);
    display_close(&g_display);
    return 0;
}

/* --------------------------------------------------------- the boot mode
 * set by the firmware before the kernel runs - the graphics protocol that can
 * change it only exists during boot - so a new mode takes effect on the next
 * start-up rather than immediately. */
/* Set one "key=value" line in the loader's BOOT.CFG, replacing an existing one
 * for that key or adding it.  The loader reads this on the next start, which is
 * how a choice made in Settings (a resolution, a refresh rate, a multi-display
 * arrangement) reaches the boot before the desktop even exists. */
static bool set_boot_cfg(const char *key, const char *value) {
    static const char *paths[] = { "/boot/BOOT.CFG", "/boot/boot.cfg",
                                   "/esp/BOOT.CFG", "/data/BOOT.CFG" };
    char prefix[32];
    int plen = snprintf(prefix, sizeof prefix, "%s=", key);

    for (size_t p = 0; p < sizeof paths / sizeof paths[0]; p++) {
        char text[1024];
        int fd = open(paths[p], O_RDONLY);
        if (fd < 0) continue;
        int n = (int)read(fd, text, sizeof text - 1);
        close(fd);
        if (n < 0) n = 0;
        text[n] = 0;

        char out[1200];
        int len = 0;
        bool replaced = false;
        const char *cur = text;
        while (*cur) {
            const char *nl = strchr(cur, '\n');
            size_t seg = nl ? (size_t)(nl - cur) : strlen(cur);
            if (!strncmp(cur, prefix, (size_t)plen)) {
                len += snprintf(out + len, sizeof out - len, "%s%s\n", prefix, value);
                replaced = true;
            } else if (seg && len + (int)seg + 2 < (int)sizeof out) {
                memcpy(out + len, cur, seg);
                len += (int)seg;
                out[len++] = '\n';
            }
            if (!nl) break;
            cur = nl + 1;
        }
        if (!replaced && len + (int)strlen(value) + plen + 2 < (int)sizeof out)
            len += snprintf(out + len, sizeof out - len, "%s%s\n", prefix, value);

        fd = open(paths[p], O_WRONLY | O_TRUNC);
        if (fd < 0) continue;
        int wrote = (int)write(fd, out, (size_t)len);
        close(fd);
        if (wrote == len) return true;
    }
    return false;
}

bool desktop_save_mode(unsigned width, unsigned height, unsigned refresh_hz) {
    if (!width || !height) return false;
    /* "WxH", or "WxH@R" when a refresh rate was chosen.  The loader reads the
     * WxH and stops at the '@' (its firmware framebuffer has one fixed rate);
     * the '@R' is for a display driver, which is the only thing that can put a
     * panel onto a rate other than the one it powered on at. */
    char line[64];
    if (refresh_hz)
        snprintf(line, sizeof line, "%ux%u@%u", width, height, refresh_hz);
    else
        snprintf(line, sizeof line, "%ux%u", width, height);
    return set_boot_cfg("video", line);
}

/* The multi-display arrangement (#7): "extend", "mirror" or "onlyother", read
 * by the loader as displaymode= and applied at the next start. */
bool desktop_save_displaymode(const char *mode) {
    if (!mode || !*mode) return false;
    return set_boot_cfg("displaymode", mode);
}

