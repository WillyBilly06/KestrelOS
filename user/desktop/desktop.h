/* desktop.h - the shell that owns the screen, and the apps it hosts.
 *
 * Every app lives in this one process: it registers a launcher entry and gets
 * a window with a paint and event callback.  That keeps the whole desktop in a
 * single address space, which is what makes it possible without a display
 * protocol between processes.
 */
#ifndef KESTREL_DESKTOP_H
#define KESTREL_DESKTOP_H

#include "window.h"

typedef struct {
    const char *name;
    const char *description;
    icon_id     icon;
    void      (*launch)(wm_t *wm);
    bool        on_desktop;      /* also show an icon on the wallpaper */
} app_entry_t;

const app_entry_t *desktop_apps(int *count);

/* Decode the app-icon PNGs (/usr/share/icons) and register them with libgui so
 * gui_app_icon() draws real images.  Call once at start-up.  See appicons.c. */
void desktop_load_app_icons(void);

/* Each app supplies one of these. */
void app_terminal_launch(wm_t *wm);
/* A terminal with one command already typed into it. */
void app_terminal_run(wm_t *wm, const char *command);
void app_files_launch(wm_t *wm);
void app_events_launch(wm_t *wm);
void app_sysinfo_launch(wm_t *wm);
void app_taskmgr_launch(wm_t *wm);
void app_editor_launch(wm_t *wm);
void app_editor_open(wm_t *wm, const char *path);
void app_settings_launch(wm_t *wm);
void app_install_launch(wm_t *wm);
void app_about_launch(wm_t *wm);
void app_gl_launch(wm_t *wm);
void app_browser_launch(wm_t *wm);
void app_store_launch(wm_t *wm);
/* Open a particular address, from a link somewhere else or from a command. */
void app_browser_open(wm_t *wm, const char *url);

/* Shared helpers. */
window_t *desktop_new_window(wm_t *wm, const char *title, icon_id icon,
                             int w, int h, window_proc proc, void *data);
void      desktop_message(wm_t *wm, const char *title, const char *body, icon_id icon);
bool      desktop_wallpaper_index(int *index);
void      desktop_set_wallpaper(int index);
int       desktop_wallpaper_count(void);
const char *desktop_wallpaper_name(int index);
void      desktop_wallpaper_colours(int index, colour_t *top, colour_t *bottom);

/* Write a display size into the loader's configuration, so it takes effect on
 * the next start.  The mode itself is set by the firmware before this system
 * runs, so it cannot be changed while it is running. */
bool      desktop_save_mode(unsigned width, unsigned height, unsigned refresh_hz);
bool      desktop_save_displaymode(const char *mode);

/* Device Manager, which Settings can open. */
void      app_devices_launch(wm_t *wm);
void      desktop_paint_wallpaper(surface_t *s, rect_t area, int index);
void      desktop_request_console(wm_t *wm, const char *program);

bool desktop_prefs_load(int *theme, int *accent, int *wallpaper);
void desktop_prefs_save(int theme, int accent, int wallpaper);

#endif
