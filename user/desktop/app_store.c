/* app_store.c - getting programs onto this machine and running them.
 *
 * A system that can only run what shipped with it is a demonstration.  What
 * makes it something a person can use is being able to go and get a program
 * that was not there before, put it somewhere, and run it - and to be able to
 * take it off again afterwards.
 *
 * Two kinds of program run here, and the difference is worth being plain
 * about.  A program built for this system is loaded and run directly.  A
 * Windows program is a different executable format entirely, calling into a
 * different set of libraries, and it runs under the translation layer in
 * user/winrun - which is real, and is not all of Windows.  A program that
 * asks for something that layer does not implement stops with a message
 * saying which function it wanted, rather than crashing or silently doing
 * nothing.
 *
 * Everything is downloaded over the same secure connection the browser uses,
 * with the same certificate checks, and the same padlock rule: it is shown
 * only when the checks passed.
 */
#include "desktop.h"
#include "aatext.h"
#include "web.h"

/* Where a fetched program is kept.
 *
 * The data volume when there is one, and the volume this was booted from when
 * there is not.  Named through a function rather than a constant because which
 * it is cannot be known until the machine has been looked at: a stick has no
 * data volume, and this used to write to one anyway - so every install on the
 * machine people actually carry went nowhere. */
#define APPS_DIR_DATA "/data/apps"
#define APPS_DIR_BOOT "/boot/KESTREL/apps"
#define CATALOGUE     "/etc/apps.list"
static const char *apps_dir(void) {
    static const char *chosen;
    if (chosen) return chosen;

    mkdir(APPS_DIR_DATA);
    DIR *d = opendir(APPS_DIR_DATA);
    if (d) { closedir(d); chosen = APPS_DIR_DATA; return chosen; }

    mkdir("/boot/KESTREL");
    mkdir(APPS_DIR_BOOT);
    d = opendir(APPS_DIR_BOOT);
    if (d) { closedir(d); chosen = APPS_DIR_BOOT; return chosen; }

    chosen = APPS_DIR_DATA;    /* nowhere writable; the caller reports it */
    return chosen;
}

static const char *installed_list(void) {
    static char path[128];
    if (!path[0]) snprintf(path, sizeof path, "%s/installed.list", apps_dir());
    return path;
}

#define MAX_ENTRIES   64
#define URL_MAX       512

typedef enum {
    KIND_NATIVE = 0,        /* built for this system */
    KIND_WINDOWS,           /* a Windows program, run under the translation layer */
} program_kind;

typedef struct {
    char name[64];
    char summary[160];
    char url[URL_MAX];
    char file[96];          /* what it is called once installed */
    program_kind kind;
    bool installed;
    long size;
} entry_t;

typedef enum {
    VIEW_CATALOGUE = 0,
    VIEW_INSTALLED,
    VIEW_FETCH,             /* typing an address by hand */
} view_kind;

typedef struct {
    wm_t     *wm;
    window_t *window;

    entry_t entries[MAX_ENTRIES];
    int     count;

    view_kind view;
    int  selected;
    int  hover_row;
    int  scroll;

    /* Typing an address. */
    char address[URL_MAX];
    int  address_len;
    bool address_focused;

    /* The download, on its own thread. */
    uint64_t      last_spin_ms;
    volatile bool loading;
    volatile bool arrived;
    char          fetch_url[URL_MAX];
    char          fetch_name[64];
    char          progress[160];
    web_response_t incoming;
    char          message[224];
    bool          message_is_error;

    rect_t sidebar_rects[3];
    rect_t action_rect, remove_rect, address_rect, fetch_rect;
    int    hover_button;
} store_t;

static store_t *active_store;

/* ------------------------------------------------------------- the lists */

static void trim(char *s) {
    size_t len = strlen(s);
    while (len && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' '))
        s[--len] = 0;
}

/* The catalogue is one program per line:
 *
 *     name | kind | file | url | summary
 *
 * Plain text on purpose: it can be read, edited and added to without a tool,
 * which matters for a list whose whole job is to be extended. */
static void load_catalogue(store_t *st) {
    st->count = 0;

    static char buffer[16384];
    ssize_t got = read_file(CATALOGUE, buffer, sizeof buffer - 1);
    if (got <= 0) return;
    buffer[got] = 0;

    char *line = buffer;
    while (*line && st->count < MAX_ENTRIES) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;

        if (*line && *line != '#') {
            entry_t *e = &st->entries[st->count];
            memset(e, 0, sizeof *e);

            char *fields[5] = { NULL, NULL, NULL, NULL, NULL };
            int field = 0;
            char *at = line;
            fields[field++] = at;
            while (*at && field < 5) {
                if (*at == '|') { *at = 0; fields[field++] = at + 1; }
                at++;
            }

            if (field >= 4) {
                for (int i = 0; i < field; i++) {
                    while (*fields[i] == ' ') fields[i]++;
                    trim(fields[i]);
                }
                strlcpy(e->name, fields[0], sizeof e->name);
                e->kind = !strcmp(fields[1], "windows") ? KIND_WINDOWS : KIND_NATIVE;
                strlcpy(e->file, fields[2], sizeof e->file);
                strlcpy(e->url, fields[3], sizeof e->url);
                if (field >= 5) strlcpy(e->summary, fields[4], sizeof e->summary);
                st->count++;
            }
        }

        if (!end) break;
        line = end + 1;
    }
}

static void note_installed(store_t *st) {
    for (int i = 0; i < st->count; i++) {
        char path[256];
        snprintf(path, sizeof path, "%s/%s", apps_dir(), st->entries[i].file);
        kstat_t info;
        st->entries[i].installed = (stat(path, &info) == 0);
        st->entries[i].size = st->entries[i].installed ? (long)info.size : 0;
    }
}

/* Anything in the directory that the catalogue does not know about - which is
 * everything downloaded by address rather than chosen from the list. */
static void add_unlisted(store_t *st) {
    DIR *dir = opendir(apps_dir());
    if (!dir) return;

    kdirent_t entry;
    while (st->count < MAX_ENTRIES && readdir(dir, &entry) == 0) {
        if (entry.type != FT_FILE) continue;
        if (!strcmp(entry.name, "installed.list")) continue;

        bool known = false;
        for (int i = 0; i < st->count; i++)
            if (!strcmp(st->entries[i].file, entry.name)) { known = true; break; }
        if (known) continue;

        entry_t *e = &st->entries[st->count++];
        memset(e, 0, sizeof *e);
        strlcpy(e->file, entry.name, sizeof e->file);
        strlcpy(e->name, entry.name, sizeof e->name);
        strlcpy(e->summary, "Downloaded by address", sizeof e->summary);
        e->installed = true;
        e->size = (long)entry.size;

        size_t len = strlen(entry.name);
        e->kind = (len > 4 && !strcasecmp(entry.name + len - 4, ".exe"))
                ? KIND_WINDOWS : KIND_NATIVE;
    }
    closedir(dir);
}

static bool entry_in_view(const store_t *st, const entry_t *e) {
    switch (st->view) {
    case VIEW_INSTALLED: return e->installed;
    case VIEW_CATALOGUE: return e->url[0] != 0;
    default:             return false;
    }
}

/* Point the selection at something that is actually in front of the user.
 *
 * Reading the disk again renumbers everything: an entry that was removed is
 * gone and the ones after it have moved up. A selection kept across that
 * refers to whatever now happens to sit at that index - so the pane below the
 * list goes on offering Run and Remove for a program that is no longer there. */
static void reselect(store_t *st, const char *keep_file) {
    if (keep_file && keep_file[0]) {
        for (int i = 0; i < st->count; i++) {
            if (!entry_in_view(st, &st->entries[i])) continue;
            if (!strcmp(st->entries[i].file, keep_file)) { st->selected = i; return; }
        }
    }
    for (int i = 0; i < st->count; i++) {
        if (entry_in_view(st, &st->entries[i])) { st->selected = i; return; }
    }
    st->selected = -1;
}

static void refresh(store_t *st) {
    /* What was selected, by name rather than by position, so it can be found
     * again afterwards if it is still there. */
    char was[96];
    was[0] = 0;
    if (st->selected >= 0 && st->selected < st->count)
        strlcpy(was, st->entries[st->selected].file, sizeof was);

    load_catalogue(st);
    note_installed(st);
    add_unlisted(st);

    reselect(st, was);
}

/* ----------------------------------------------------------- downloading */

static void store_progress(const char *what, void *ctx) {
    store_t *st = ctx;
    strlcpy(st->progress, what, sizeof st->progress);
}

static void download_thread(void *arg) {
    store_t *st = arg;
    web_on_progress(store_progress, st);
    memset(&st->incoming, 0, sizeof st->incoming);
    web_fetch(st->fetch_url, 6, &st->incoming);
    st->arrived = true;
}

static void start_download(store_t *st, const char *url, const char *file) {
    if (st->loading) return;

    (void)apps_dir();

    strlcpy(st->fetch_url, url, sizeof st->fetch_url);
    strlcpy(st->fetch_name, file, sizeof st->fetch_name);
    strlcpy(st->progress, "Starting", sizeof st->progress);
    st->message[0] = 0;
    st->loading = true;
    st->arrived = false;

    if (thread_create(download_thread, st) < 0) {
        st->loading = false;
        download_thread(st);
        st->loading = true;
    }
}

/* An executable this system can run begins with a known pattern.  Checking it
 * before writing anything means a page that was fetched instead of a program -
 * a redirect to a sign-in form, say - is caught here rather than at the point
 * somebody tries to run it. */
static const char *looks_runnable(const char *data, int len, program_kind *kind) {
    if (len < 64) return "The download is too small to be a program.";

    if (data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        *kind = KIND_NATIVE;
        return NULL;
    }
    if (data[0] == 'M' && data[1] == 'Z') {
        /* The Windows form: an old header with a pointer to the real one. */
        unsigned offset = (unsigned char)data[0x3C] |
                          ((unsigned)(unsigned char)data[0x3D] << 8) |
                          ((unsigned)(unsigned char)data[0x3E] << 16) |
                          ((unsigned)(unsigned char)data[0x3F] << 24);
        if (offset + 6 < (unsigned)len &&
            data[offset] == 'P' && data[offset + 1] == 'E' &&
            data[offset + 2] == 0 && data[offset + 3] == 0) {

            /* Which processor it was built for.  Checking now rather than at
             * the point somebody tries to run it is the difference between
             * being told before the download is kept and finding out
             * afterwards that what was installed can never work. */
            unsigned machine = (unsigned char)data[offset + 4] |
                               ((unsigned)(unsigned char)data[offset + 5] << 8);
            if (machine == 0x014C)
                return "That is a 32-bit Windows program. This system runs "
                       "64-bit ones; the two are different instruction sets, "
                       "not different versions of the same thing.";
            if (machine == 0xAA64)
                return "That is a Windows program built for ARM, and this is "
                       "an x86-64 machine.";
            if (machine != 0x8664)
                return "That is a Windows program built for a processor this "
                       "system does not have.";

            *kind = KIND_WINDOWS;
            return NULL;
        }
        return "That is a Windows program of a kind this system cannot run.";
    }

    if (len > 14 && (!strncasecmp(data, "<!doctype", 9) || !strncasecmp(data, "<html", 5)))
        return "That address returned a web page rather than a program.";

    return "That does not look like a program this system can run.";
}

static void finish_download(store_t *st) {
    st->loading = false;
    st->arrived = false;

    web_response_t *r = &st->incoming;

    if (r->error[0]) {
        strlcpy(st->message, r->error, sizeof st->message);
        st->message_is_error = true;
        web_free(r);
        return;
    }

    if (r->status >= 400) {
        snprintf(st->message, sizeof st->message,
                 "The server answered with status %d, so there was nothing to "
                 "install.", r->status);
        st->message_is_error = true;
        web_free(r);
        return;
    }

    program_kind kind = KIND_NATIVE;
    const char *problem = looks_runnable(r->body, r->body_len, &kind);
    if (problem) {
        strlcpy(st->message, problem, sizeof st->message);
        st->message_is_error = true;
        web_free(r);
        return;
    }

    char path[256];
    snprintf(path, sizeof path, "%s/%s", apps_dir(), st->fetch_name);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        snprintf(st->message, sizeof st->message,
                 "%s could not be written: %s", path, strerror(errno));
        st->message_is_error = true;
        web_free(r);
        return;
    }
    int written = (int)write(fd, r->body, (size_t)r->body_len);
    close(fd);

    if (written != r->body_len) {
        snprintf(st->message, sizeof st->message,
                 "Only %d of %d bytes reached the disk, so it was not installed.",
                 written, r->body_len);
        st->message_is_error = true;
        unlink(path);
        web_free(r);
        return;
    }

    snprintf(st->message, sizeof st->message,
             "%s is installed (%d bytes%s%s).", st->fetch_name, written,
             r->secure ? ", over a checked connection from " : "",
             r->secure ? r->peer.subject : "");
    st->message_is_error = false;

    web_free(r);
    refresh(st);
}

/* -------------------------------------------------------------- running */

static void run_entry(store_t *st, const entry_t *e) {
    char command[320];
    if (e->kind == KIND_WINDOWS)
        snprintf(command, sizeof command, "winrun %s/%s", apps_dir(), e->file);
    else
        snprintf(command, sizeof command, "%s/%s", apps_dir(), e->file);

    /* Programs get a terminal, because that is where their output goes and
     * where a message about something the translation layer does not have will
     * appear.  A program that opens a window of its own still can. */
    app_terminal_run(st->wm, command);
}

static void remove_entry(store_t *st, entry_t *e) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s", apps_dir(), e->file);
    if (unlink(path) == 0) {
        snprintf(st->message, sizeof st->message, "%s was removed.", e->name);
        st->message_is_error = false;
    } else {
        snprintf(st->message, sizeof st->message,
                 "%s could not be removed: %s", e->name, strerror(errno));
        st->message_is_error = true;
    }
    /* Not kept: it is the thing that was just removed. */
    if (st->selected >= 0 && st->selected < st->count)
        st->entries[st->selected].file[0] = 0;
    refresh(st);
}

/* ------------------------------------------------------------- painting */

/* Lengths here scale with the interface.
 *
 * Written as plain numbers they are correct on exactly one display and wrong
 * on every other: the text drawn against them grows with the interface scale
 * and the boxes holding it do not, so rows sit on top of one another and
 * columns run into the ones beside them.  This file used gui_scale() nowhere
 * at all before that was noticed on a 1440p screen.
 */
#define SIDEBAR_W (190 * gui_scale())
#define ROW_H     (62 * gui_scale())

static bool row_visible(const store_t *st, int index) {
    return index >= 0 && index < st->count;
}


static void paint_sidebar(store_t *st, surface_t *s) {
    rect_t side = rect_make(0, 0, SIDEBAR_W, s->height);
    gui_sidebar(s, side);

    aa_text(s, &AA_BODY_BOLD, 15, 18, 34, "Applications",
            colour_mix(g_theme.window, g_theme.text, 200));

    static const struct { const char *label; icon_id icon; } items[] = {
        { "Available", ICON_STORE },
        { "Installed", ICON_CHECK },
        { "From an address", ICON_NETWORK },
    };

    /* Scaled, like the rest of this window (ROW_H above is 62*gui_scale).  The
     * row height and spacing were bare pixels - 38 tall, 42 apart - while
     * gui_sidebar_item centres a scaled icon and a scaled label in the height
     * it is given, so on a 2x display the three entries drew their big labels
     * into 38-pixel boxes 42 pixels apart and ran into one another and their
     * own icons. */
    const int k = gui_scale();
    int row_h = 40 * k;
    int y = 54 * k;
    for (int i = 0; i < 3; i++) {
        st->sidebar_rects[i] = rect_make(8 * k, y, SIDEBAR_W - 16 * k, row_h);
        gui_sidebar_item(s, st->sidebar_rects[i], items[i].icon, items[i].label,
                         st->view == (view_kind)i, false);
        y += row_h + 6 * k;
    }
}

static void paint_row(store_t *st, surface_t *s, const entry_t *e, rect_t r,
                      bool selected, bool hover) {
    if (selected)
        gui_round_rect_aa(s, r, 6, colour_mix(g_theme.window, g_theme.accent, 45));
    else if (hover)
        gui_round_rect_aa(s, r, 6, colour_mix(g_theme.window, g_theme.text, 16));

    colour_t title = colour_mix(g_theme.window, g_theme.text, 220);
    colour_t dim = colour_mix(g_theme.window, g_theme.text, 130);

    gui_icon(s, e->kind == KIND_WINDOWS ? ICON_INSTALL : ICON_CHIP,
             r.x + 14, r.y + (r.h - 22) / 2, 22,
             colour_mix(g_theme.window, g_theme.accent, 200));

    int text_x = r.x + 48;
    aa_text_clipped(s, &AA_BODY_BOLD, 15, text_x, r.y + 25, r.w - 180,
                    e->name, title);
    aa_text_clipped(s, &AA_BODY, 12, text_x, r.y + 45, r.w - 180,
                    e->summary[0] ? e->summary : e->url, dim);

    int badge_x = r.x + r.w - 16;
    if (e->installed) {
        char size[32];
        if (e->size >= 1024 * 1024)
            snprintf(size, sizeof size, "%ld.%ld MB", e->size / (1024 * 1024),
                     (e->size % (1024 * 1024)) * 10 / (1024 * 1024));
        else
            snprintf(size, sizeof size, "%ld KB", (e->size + 512) / 1024);
        int width = aa_text_width(&AA_BODY, 12, size);
        aa_text(s, &AA_BODY, 12, badge_x - width, r.y + 39, size, dim);
        badge_x -= width + 12;

        int badge = gui_badge_width("Installed");
        gui_badge(s, badge_x - badge, r.y + 14, "Installed", RGB(22, 138, 78));
    } else if (e->kind == KIND_WINDOWS) {
        int badge = gui_badge_width("Windows");
        gui_badge(s, badge_x - badge, r.y + 22, "Windows",
                  colour_mix(g_theme.window, g_theme.accent, 190));
    }
}

static void paint_detail(store_t *st, surface_t *s, rect_t area) {
    /* Nothing selected, or the selection belongs to the other view: there is
     * nothing to offer, and offering it anyway is how a button ends up acting
     * on something that is not there. */
    if (st->selected < 0 || st->selected >= st->count) {
        st->action_rect = rect_make(0, 0, 0, 0);
        st->remove_rect = rect_make(0, 0, 0, 0);
        return;
    }
    const entry_t *e = &st->entries[st->selected];
    if (!entry_in_view(st, e)) {
        st->action_rect = rect_make(0, 0, 0, 0);
        st->remove_rect = rect_make(0, 0, 0, 0);
        return;
    }

    gui_fill(s, rect_make(area.x, area.y, area.w, 1),
             colour_mix(g_theme.window, g_theme.text, 26));

    colour_t dim = colour_mix(g_theme.window, g_theme.text, 145);

    aa_text_clipped(s, &AA_BODY_BOLD, 15, area.x + 18, area.y + 28,
                    area.w - 300, e->name,
                    colour_mix(g_theme.window, g_theme.text, 215));

    const char *what = e->kind == KIND_WINDOWS
                     ? "A Windows program, run through the translation layer"
                     : "Built for this system";
    aa_text_clipped(s, &AA_BODY, 12, area.x + 18, area.y + 48, area.w - 300,
                    what, dim);

    if (e->url[0])
        aa_text_clipped(s, &AA_CODE, 11, area.x + 18, area.y + 68, area.w - 300,
                        e->url, dim);

    /* The two things that can be done with it. */
    int button_w = 108, button_h = 34;
    int y = area.y + (area.h - button_h) / 2;
    int x = area.x + area.w - 18 - button_w;

    if (e->installed) {
        st->remove_rect = rect_make(x, y, button_w, button_h);
        bool hover = st->hover_button == 2;
        gui_round_rect_aa(s, st->remove_rect, 6,
                          hover ? colour_mix(g_theme.control, g_theme.text, 40)
                                : g_theme.control);
        gui_round_frame_aa(s, st->remove_rect, 6, g_theme.control_border);
        int width = aa_text_width(&AA_BODY, 14, "Remove");
        aa_text(s, &AA_BODY, 14, x + (button_w - width) / 2, y + 22, "Remove",
                colour_mix(g_theme.control, g_theme.text, 200));
        x -= button_w + 10;
    } else {
        st->remove_rect = rect_make(0, 0, 0, 0);
    }

    const char *label = e->installed ? "Run" : "Install";
    st->action_rect = rect_make(x, y, button_w, button_h);
    bool hover = st->hover_button == 1;
    colour_t fill = hover ? colour_shade(g_theme.accent, 12) : g_theme.accent;
    gui_round_rect_aa(s, st->action_rect, 6, fill);
    int width = aa_text_width(&AA_BODY_BOLD, 14, label);
    aa_text(s, &AA_BODY_BOLD, 14, x + (button_w - width) / 2, y + 22, label,
            g_theme.accent_text);
}

static void paint_fetch(store_t *st, surface_t *s, rect_t area) {
    int x = area.x + 28;
    int y = area.y + 34;

    aa_text(s, &AA_BODY_BOLD, 19, x, y + 16, "Install from an address",
            colour_mix(g_theme.window, g_theme.text, 220));
    y += 44;

    {
        /* Built rather than concatenated: which directory this is depends on
         * what the machine has, so it is not known until it runs. */
        char where[160];
        snprintf(where, sizeof where,
                 "Give the address of a program and it will be fetched and "
                 "put in %s.", apps_dir());
        aa_text(s, &AA_BODY, 13, x, y + 14, where,
                colour_mix(g_theme.window, g_theme.text, 150));
    }
    y += 26;
    aa_text(s, &AA_BODY, 13, x, y + 14,
            "A secure address is checked against this machine's certificate "
            "authorities before anything is written.",
            colour_mix(g_theme.window, g_theme.text, 150));
    y += 40;

    st->address_rect = rect_make(x, y, area.w - 56 - 120, 38);
    gui_round_rect_aa(s, st->address_rect, 6, g_theme.field);
    gui_round_frame_aa(s, st->address_rect, 6,
                       st->address_focused ? g_theme.accent : g_theme.field_border);

    int baseline = st->address_rect.y + 25;
    if (st->address_len)
        aa_text_clipped(s, &AA_BODY, 14, x + 12, baseline,
                        st->address_rect.w - 24, st->address,
                        g_theme.field_text);
    else
        aa_text(s, &AA_BODY, 14, x + 12, baseline, "https://example.com/program.exe",
                colour_mix(g_theme.field, g_theme.field_text, 100));

    if (st->address_focused) {
        int caret = x + 12 + aa_text_width_n(&AA_BODY, 14, st->address,
                                             (size_t)st->address_len);
        int limit = st->address_rect.x + st->address_rect.w - 10;
        if (caret > limit) caret = limit;
        gui_fill(s, rect_make(caret, st->address_rect.y + 8, 1, 22), g_theme.accent);
    }

    st->fetch_rect = rect_make(st->address_rect.x + st->address_rect.w + 12, y,
                               108, 38);
    colour_t fill = st->hover_button == 3 ? colour_shade(g_theme.accent, 12)
                                          : g_theme.accent;
    gui_round_rect_aa(s, st->fetch_rect, 6, fill);
    int width = aa_text_width(&AA_BODY_BOLD, 14, "Install");
    aa_text(s, &AA_BODY_BOLD, 14, st->fetch_rect.x + (108 - width) / 2,
            y + 25, "Install", g_theme.accent_text);
}

static bool store_proc(window_t *w, const wevent_t *ev) {
    store_t *st = w->data;
    surface_t *s = w->canvas;

    rect_t content = rect_make(SIDEBAR_W, 0, s->width - SIDEBAR_W, s->height);
    int detail_h = 84;
    int status_h = 28;

    switch (ev->kind) {
    case WE_PAINT: {
        gui_clear(s, g_theme.window);
        paint_sidebar(st, s);

        if (st->view == VIEW_FETCH) {
            paint_fetch(st, s, content);
        } else {
            int y = content.y + 14 - st->scroll;
            int shown = 0;
            rect_t list = rect_make(content.x, 0, content.w,
                                    content.h - detail_h - status_h);
            rect_t saved = surface_clip(s);
            surface_set_clip(s, rect_intersection(saved, list));

            for (int i = 0; i < st->count; i++) {
                entry_t *e = &st->entries[i];
                if (!entry_in_view(st, e)) continue;
                rect_t r = rect_make(content.x + 12, y, content.w - 24, ROW_H - 6);
                if (r.y + r.h > list.y && r.y < list.y + list.h)
                    paint_row(st, s, e, r, st->selected == i, st->hover_row == i);
                y += ROW_H;
                shown++;
            }
            surface_reset_clip(s);
            surface_set_clip(s, saved);

            if (!shown) {
                const char *empty = st->view == VIEW_INSTALLED
                    ? "Nothing has been installed yet."
                    : "The catalogue is empty.";
                int width = aa_text_width(&AA_BODY, 14, empty);
                aa_text(s, &AA_BODY, 14, content.x + (content.w - width) / 2,
                        content.h / 2, empty,
                        colour_mix(g_theme.window, g_theme.text, 120));
            }

            paint_detail(st, s, rect_make(content.x, content.h - detail_h - status_h,
                                          content.w, detail_h));
        }

        /* The status line, which is where a download's progress and the result
         * of the last thing that happened both appear. */
        int y = s->height - status_h;
        gui_fill(s, rect_make(SIDEBAR_W, y, content.w, status_h),
                 colour_mix(g_theme.window, g_theme.text, 10));
        gui_fill(s, rect_make(SIDEBAR_W, y, content.w, 1),
                 colour_mix(g_theme.window, g_theme.text, 25));

        if (st->loading) {
            gui_spinner(s, SIDEBAR_W + 18, y + status_h / 2, 7, g_theme.accent);
            aa_text_clipped(s, &AA_BODY, 12, SIDEBAR_W + 34, y + 18,
                            content.w - 50, st->progress,
                            colour_mix(g_theme.window, g_theme.text, 150));
        } else if (st->message[0]) {
            aa_text_clipped(s, &AA_BODY, 12, SIDEBAR_W + 14, y + 18,
                            content.w - 28, st->message,
                            st->message_is_error ? g_theme.error
                                                 : colour_mix(g_theme.window,
                                                              g_theme.text, 160));
        } else {
            aa_text(s, &AA_BODY, 12, SIDEBAR_W + 14, y + 18, apps_dir(),
                    colour_mix(g_theme.window, g_theme.text, 110));
        }
        return false;
    }

    case WE_TICK: {
        if (st->arrived) { finish_download(st); return true; }
        if (st->loading) {
            /* Eight times a second, not sixty: the thread doing the fetching
             * needs the processor more than the spinner does. */
            uint64_t now = uptime_ms();
            if (now - st->last_spin_ms < 125) return false;
            st->last_spin_ms = now;
            return true;
        }
        return gui_controls_settling();
    }

    case WE_MOUSE_MOVE: {
        int was_row = st->hover_row;
        int was_button = st->hover_button;
        st->hover_row = -1;
        st->hover_button = 0;

        if (rect_contains(st->action_rect, ev->x, ev->y)) st->hover_button = 1;
        else if (rect_contains(st->remove_rect, ev->x, ev->y)) st->hover_button = 2;
        else if (rect_contains(st->fetch_rect, ev->x, ev->y)) st->hover_button = 3;
        else if (ev->x > SIDEBAR_W && st->view != VIEW_FETCH) {
            int y = content.y + 14 - st->scroll;
            for (int i = 0; i < st->count; i++) {
                if (!entry_in_view(st, &st->entries[i])) continue;
                if (ev->y >= y && ev->y < y + ROW_H - 6 &&
                    ev->y < content.h - detail_h - status_h)
                    st->hover_row = i;
                y += ROW_H;
            }
        }
        return was_row != st->hover_row || was_button != st->hover_button;
    }

    case WE_MOUSE_DOWN: {
        for (int i = 0; i < 3; i++) {
            if (rect_contains(st->sidebar_rects[i], ev->x, ev->y)) {
                st->view = (view_kind)i;
                st->scroll = 0;
                st->address_focused = (i == VIEW_FETCH);
                reselect(st, NULL);
                return true;
            }
        }

        if (st->view == VIEW_FETCH) {
            if (rect_contains(st->address_rect, ev->x, ev->y)) {
                st->address_focused = true;
                return true;
            }
            if (rect_contains(st->fetch_rect, ev->x, ev->y) && st->address_len) {
                /* The name it will be saved under: the last part of the
                 * address, which is what a person would expect it to be
                 * called. */
                const char *slash = strrchr(st->address, '/');
                char name[64];
                strlcpy(name, slash && slash[1] ? slash + 1 : "program",
                        sizeof name);
                char *query = strchr(name, '?');
                if (query) *query = 0;
                if (!name[0]) strlcpy(name, "program", sizeof name);
                start_download(st, st->address, name);
                return true;
            }
            st->address_focused = false;
            return true;
        }

        if (rect_contains(st->action_rect, ev->x, ev->y) &&
            st->selected >= 0 && st->selected < st->count) {
            entry_t *e = &st->entries[st->selected];
            if (e->installed) run_entry(st, e);
            else if (e->url[0]) start_download(st, e->url, e->file);
            return true;
        }

        if (rect_contains(st->remove_rect, ev->x, ev->y) &&
            st->selected >= 0 && st->selected < st->count) {
            remove_entry(st, &st->entries[st->selected]);
            return true;
        }

        if (st->hover_row >= 0) { st->selected = st->hover_row; return true; }
        return false;
    }

    case WE_MOUSE_WHEEL: {
        if (st->view == VIEW_FETCH) return false;
        int shown = 0;
        for (int i = 0; i < st->count; i++)
            if (entry_in_view(st, &st->entries[i])) shown++;
        int height = content.h - detail_h - status_h - 28;
        int limit = shown * ROW_H - height;
        if (limit < 0) limit = 0;
        st->scroll -= ev->wheel * 44;
        if (st->scroll < 0) st->scroll = 0;
        if (st->scroll > limit) st->scroll = limit;
        return true;
    }

    case WE_KEY_DOWN: {
        if (st->view == VIEW_FETCH && st->address_focused) {
            if (ev->key == '\n' || ev->key == '\r') {
                if (st->address_len) {
                    const char *slash = strrchr(st->address, '/');
                    char name[64];
                    strlcpy(name, slash && slash[1] ? slash + 1 : "program",
                            sizeof name);
                    char *query = strchr(name, '?');
                    if (query) *query = 0;
                    start_download(st, st->address, name);
                }
                return true;
            }
            if (ev->key == '\b') {
                if (st->address_len) st->address[--st->address_len] = 0;
                return true;
            }
            if (ev->key >= 32 && ev->key < 127 && st->address_len < URL_MAX - 1) {
                st->address[st->address_len++] = (char)ev->key;
                st->address[st->address_len] = 0;
                return true;
            }
            return false;
        }

        if (ev->key == KK_DOWN || ev->key == KK_UP) {
            int step = ev->key == KK_DOWN ? 1 : -1;
            for (int i = st->selected + step; row_visible(st, i); i += step) {
                if (entry_in_view(st, &st->entries[i])) { st->selected = i; break; }
            }
            return true;
        }
        if ((ev->key == '\n' || ev->key == '\r') &&
            st->selected >= 0 && st->selected < st->count) {
            entry_t *e = &st->entries[st->selected];
            if (e->installed) run_entry(st, e);
            else if (e->url[0]) start_download(st, e->url, e->file);
            return true;
        }
        return false;
    }

    case WE_CLOSE:
        if (active_store == st) active_store = NULL;
        free(st);
        w->data = NULL;
        return false;

    default:
        return false;
    }
}

void app_store_launch(wm_t *wm) {
    store_t *st = calloc(1, sizeof *st);
    if (!st) return;

    st->wm = wm;
    st->selected = -1;
    st->hover_row = -1;

    (void)apps_dir();          /* creates whichever one this machine has */
    refresh(st);

    for (int i = 0; i < st->count; i++)
        if (entry_in_view(st, &st->entries[i])) { st->selected = i; break; }

    st->window = desktop_new_window(wm, "Applications", ICON_STORE, 860, 620,
                                    store_proc, st);
    if (!st->window) { free(st); return; }

    active_store = st;
    st->window->needs_paint = true;
}
