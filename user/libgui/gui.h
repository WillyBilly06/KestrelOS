/* gui.h - the KestrelOS user-interface toolkit.
 *
 * A single process owns the display: it maps the framebuffer, draws into an
 * off-screen surface and blits the parts that changed.  Windows, widgets and
 * the event loop all live in that one process, which avoids needing shared
 * memory or a display protocol between processes.
 */
#ifndef KESTREL_GUI_H
#define KESTREL_GUI_H

#include "kestrel.h"
#include "../../include/kestrel/input.h"

/* ------------------------------------------------------------------ colour */

typedef uint32_t colour_t;

#define RGB(r, g, b) ((colour_t)(((r) << 16) | ((g) << 8) | (b)))
#define RGB_R(c) (((c) >> 16) & 0xFF)
#define RGB_G(c) (((c) >> 8) & 0xFF)
#define RGB_B(c) ((c) & 0xFF)

colour_t colour_mix(colour_t a, colour_t b, int alpha);   /* alpha 0-255 towards b */
colour_t colour_shade(colour_t c, int percent);           /* +lighter, -darker     */

/* The palette the desktop and every app share. */
typedef struct {
    colour_t desktop_top, desktop_bottom;
    colour_t window, window_border, window_shadow;
    colour_t title_active, title_inactive, title_text, title_text_inactive;
    colour_t text, text_dim, text_bright;
    colour_t accent, accent_dark, accent_text;
    colour_t control, control_hover, control_press, control_border;
    colour_t field, field_border, field_text;
    colour_t taskbar, taskbar_text, taskbar_hover;
    colour_t selection, selection_text;
    colour_t warning, error, success;
    colour_t scrollbar, scrollbar_thumb;
} theme_t;

extern theme_t g_theme;

/* What actually differs between one look and another.  Everything else in the
 * palette above is derived from these, so a new theme is six values rather
 * than thirty, and an accent can be changed without touching a theme. */
typedef struct {
    const char *name;
    colour_t    window;
    colour_t    desktop_top, desktop_bottom;
    colour_t    accent;
    bool        dark;
} theme_spec_t;

void theme_build(const theme_spec_t *spec);
int  theme_count(void);
const theme_spec_t *theme_at(int index);
void theme_apply(int index, colour_t accent);   /* accent 0 keeps the theme's */

int         accent_count(void);
colour_t    accent_at(int index);
const char *accent_name(int index);
void theme_set_dark(void);
void theme_set_light(void);

/* ----------------------------------------------------------------- surface */

typedef struct {
    colour_t *pixels;
    int       width, height;
    int       stride;        /* pixels per row */
    bool      owns_pixels;
    struct gui_gpu_surface *gpu; /* shared by non-owning cropped views */
} surface_t;

typedef struct { int x, y, w, h; } rect_t;

static inline rect_t rect_make(int x, int y, int w, int h) {
    rect_t r = { x, y, w, h };
    return r;
}
bool   rect_contains(rect_t r, int x, int y);
bool   rect_intersects(rect_t a, rect_t b);
rect_t rect_intersection(rect_t a, rect_t b);
rect_t rect_union(rect_t a, rect_t b);
bool   rect_empty(rect_t r);

surface_t *surface_create(int w, int h);
/* GPU targets allocate metadata + cleared VRAM, never a temporary CPU bitmap.
 * A requested GPU allocation fails explicitly; it does not fall back to CPU. */
surface_t *surface_create_target(int w, int h, bool gpu);
void       surface_destroy(surface_t *s);
void       surface_set_clip(surface_t *s, rect_t clip);
void       surface_reset_clip(surface_t *s);
rect_t     surface_clip(const surface_t *s);
bool       gui_gpu_attach(surface_t *s);
bool       gui_gpu_validate(void); /* one-time native raster/readback gate */
void       gui_gpu_detach(surface_t *s);
bool       gui_gpu_cpu_access(surface_t *s); /* explicit legacy app boundary */
bool       gui_gpu_flush(void);
#include "../../include/kestrel/gpu3d.h"
bool       gui_gpu_draw3d(surface_t *s, const surface_t *texture,
                         const kg3d_command_t *commands, unsigned count, rect_t bounds);
typedef struct {uint64_t colour,depth;unsigned pitch,depth_pitch;} gui_gpu_target_t;
bool       gui_gpu_target(surface_t *s, bool needs_depth, gui_gpu_target_t *out);
void       gui_gpu_invalidate(surface_t *s); /* reject an incomplete application batch */
surface_t *gui_gpu_image(const colour_t *pixels, int width, int height);
/* Explicit VRAM snapshot; does not attach CPU storage or demote the target.
 * capacity is the number of colour_t elements available at pixels. */
bool       gui_gpu_colour_readback(const surface_t *s, colour_t *pixels,
                                   unsigned stride, size_t capacity);
bool       gui_gpu_depth_readback(const surface_t *s, float *pixels, unsigned stride);
bool       gui_gpu_present(surface_t *s, rect_t damage);
bool       gui_gpu_shadow(surface_t *s, rect_t r, int spread, int strength, int radius, colour_t colour);
bool       gui_gpu_rounded(surface_t *s, rect_t r, int radius, unsigned style, colour_t top, colour_t bottom);
bool       gui_gpu_line(surface_t *s, int x0, int y0, int x1, int y1, colour_t colour);
bool       gui_gpu_line_aa(surface_t *s, int x0, int y0, int x1, int y1, colour_t colour);
bool       gui_gpu_glyph(surface_t *s, const unsigned char *data, int w, int h, int stride,
                         bool coverage, int scale, int x, int y, colour_t colour);
bool       gui_gpu_paint(surface_t *s, rect_t r, unsigned op,
                         colour_t a, colour_t b, int alpha);
bool       gui_gpu_blit(surface_t *dst, const surface_t *src, rect_t from,
                        rect_t to, int alpha);
bool       gui_gpu_icon(surface_t *s, surface_t **cache, const void *pixels,
                       int w, int h, bool alpha_only, int x, int y, int size,
                       bool tinted, colour_t tint);
bool       gui_gpu_mask(surface_t *s, const uint8_t *mask, int w, int h,
                        int stride, int x, int y, colour_t c, int alpha);
void       gui_blend_pixel(surface_t *s, int x, int y, colour_t c, int alpha);

/* ------------------------------------------------------------ the pixel engine
 *
 * The row operations everything else is built from.  They use the processor's
 * vector unit, which moves and blends four pixels at a time; at the sizes a
 * modern display has, that is the difference between immediate and sluggish.
 * See blit.c. */
void blit_fill_row(colour_t *dst, int count, colour_t c);
void blit_copy_row(colour_t *dst, const colour_t *src, int count);
void blit_blend_row(colour_t *dst, int count, colour_t c, int alpha);
void blit_blend_copy_row(colour_t *dst, const colour_t *src, int count, int alpha);
void blit_gradient_row(colour_t *dst, int count, colour_t left, colour_t right);
/* Copying to video memory, which is write-combining and never read back. */
void blit_present_row(colour_t *dst, const colour_t *src, int count);
void blit_present_done(void);
/* What the engine is using on this processor, for the system information. */
const char *blit_engine_name(void);

/* ---------------------------------------------------------------- drawing */

void gui_clear(surface_t *s, colour_t c);
void gui_pixel(surface_t *s, int x, int y, colour_t c);
void gui_fill(surface_t *s, rect_t r, colour_t c);
void gui_frame(surface_t *s, rect_t r, colour_t c);              /* 1px outline */
void gui_frame_thick(surface_t *s, rect_t r, int thickness, colour_t c);
void gui_hline(surface_t *s, int x, int y, int w, colour_t c);
void gui_vline(surface_t *s, int x, int y, int h, colour_t c);
void gui_line(surface_t *s, int x0, int y0, int x1, int y1, colour_t c);
void gui_gradient_v(surface_t *s, rect_t r, colour_t top, colour_t bottom);
void gui_gradient_h(surface_t *s, rect_t r, colour_t left, colour_t right);
void gui_round_rect(surface_t *s, rect_t r, int radius, colour_t c);
void gui_round_frame(surface_t *s, rect_t r, int radius, colour_t c);
void gui_shadow(surface_t *s, rect_t r, int depth);
void gui_blit(surface_t *dst, const surface_t *src, int x, int y);
void gui_blit_rect(surface_t *dst, const surface_t *src, rect_t src_rect, int x, int y);
void gui_blend_rect(surface_t *s, rect_t r, colour_t c, int alpha);

/* The same blit, mixed into what is already there.  Alpha runs 0 to 255. */
void gui_blit_alpha(surface_t *dst, const surface_t *src, int x, int y, int alpha);

/* Draw `src` into the rectangle `dst_rect`, scaling it to fit, mixed in at
 * `alpha`.  This is what an opening window is drawn with: the window paints
 * itself once at its real size, and the animation scales that one image
 * rather than making the window lay itself out again on every frame. */
void gui_blit_scaled(surface_t *dst, const surface_t *src, rect_t dst_rect, int alpha);

/* A soft drop shadow.  `spread` is how far it reaches and `strength` how dark
 * it starts; the falloff is smooth rather than a stack of visible frames. */
void gui_soft_shadow(surface_t *s, rect_t r, int spread, int strength, int radius);

/* A rounded rectangle with anti-aliased corners, and the same as an outline.
 * The plain versions leave visible steps at these radii, which is exactly
 * where a window frame is looked at most closely. */
void gui_round_rect_aa(surface_t *s, rect_t r, int radius, colour_t c);
void gui_round_frame_aa(surface_t *s, rect_t r, int radius, colour_t c);
/* A rounded rectangle whose top corners are rounded and bottom ones square,
 * which is the shape of a title bar. */
void gui_round_top(surface_t *s, rect_t r, int radius, colour_t c);
void gui_round_gradient_top(surface_t *s, rect_t r, int radius, colour_t top, colour_t bottom);

/* Interpolation, for animation.  `t` runs 0 to 1 in thousandths so that no
 * floating point is needed in the compositor. */
#define ANIM_ONE 1000
int      anim_lerp(int from, int to, int t);
rect_t   anim_lerp_rect(rect_t from, rect_t to, int t);
/* Eases: t in and out of [0, ANIM_ONE]. */
int      anim_ease_out(int t);
int      anim_ease_in(int t);
int      anim_ease_in_out(int t);

/* --------------------------------------------------------------------- text */

typedef enum { FONT_UI, FONT_MONO } font_id;

int  gui_text_width(font_id font, const char *text);
int  gui_text_width_n(font_id font, const char *text, size_t n);

/* How many times text has been drawn on top of other text.  Every string in
 * this system goes through one function, so this sees all of them. */
int  gui_text_overlaps(void);
/* Tell the text bookkeeping a new frame has started. */
void gui_text_frame_begin(void);

/* A line with soft edges.  Straight lines fall through to the hard version,
 * which for them is the same picture and cheaper. */
void gui_line_aa(surface_t *s, int x0, int y0, int x1, int y1, colour_t c);
int  gui_font_height(font_id font);

/* How many screen pixels one interface pixel is drawn as.  Whole numbers only
 * - see the note in draw.c.  Everything that lays out in pixels should be
 * multiplied by this, which is what makes a taskbar the same physical size on
 * a 1080p screen and a 4K one. */
int  gui_scale(void);
void gui_set_scale(int scale);
void gui_set_default_scale(int scale); /* does not override an explicit preference */
int  gui_scale_for_width(int width);

/* The display's own scale, ignoring any per-window override (chrome/desktop). */
int  gui_screen_scale(void);
/* Push a per-window content scale so gui_scale() reports the window's size step
 * while its content is painted or hit-tested; pop restores the previous value.
 * The window manager brackets each window's paint and events with these; apps
 * do not call them.  Returns the previous override to hand back to pop. */
int  gui_scale_push(int scale);
void gui_scale_pop(int prev);
int  gui_font_advance(font_id font, unsigned char ch);
void gui_text(surface_t *s, font_id font, int x, int y, const char *text, colour_t c);
void gui_text_n(surface_t *s, font_id font, int x, int y, const char *text, size_t n, colour_t c);
void gui_text_centred(surface_t *s, font_id font, rect_t r, const char *text, colour_t c);
void gui_text_right(surface_t *s, font_id font, rect_t r, const char *text, colour_t c);
/* Truncate with an ellipsis so a long name never spills out of its box. */
void gui_text_clipped(surface_t *s, font_id font, int x, int y, int max_w, const char *text, colour_t c);
/* Break on spaces to fit `r`, and return the y just past the last line drawn. */
int  gui_text_wrapped(surface_t *s, font_id font, rect_t r, const char *text, colour_t c);

/* ------------------------------------------------------------------- icons */

typedef enum {
    ICON_NONE = 0, ICON_TERMINAL, ICON_FOLDER, ICON_FILE, ICON_DISK, ICON_LOG,
    ICON_INFO, ICON_SETTINGS, ICON_EDITOR, ICON_INSTALL, ICON_POWER, ICON_CLOSE,
    ICON_MINIMISE, ICON_MAXIMISE, ICON_RESTORE, ICON_ARROW_UP, ICON_ARROW_DOWN,
    ICON_ARROW_LEFT, ICON_ARROW_RIGHT, ICON_CHECK, ICON_WARNING, ICON_KESTREL,
    ICON_DISPLAY, ICON_USB,
    /* The ones Settings and the Device Manager needed: a thing is far easier
     * to find in a list when its icon says what it is. */
    ICON_WIFI, ICON_SOUND, ICON_PALETTE, ICON_CHIP, ICON_NETWORK, ICON_SEARCH,
    ICON_BROWSER, ICON_STORE, ICON_LOCK, ICON_RELOAD, ICON_HOME,
} icon_id;

void gui_icon(surface_t *s, icon_id icon, int x, int y, int size, colour_t c);

/* Real image app-icons: the desktop registers an ARGB bitmap per app icon_id
 * (decoded from the icon PNGs), and gui_app_icon draws it scaled with
 * per-pixel alpha - falling back to the drawn gui_icon glyph when none is
 * registered.  Used for the desktop grid, taskbar and window title bars. */
void gui_register_app_icon(icon_id id, const colour_t *argb, int w, int h);
bool gui_has_app_icon(icon_id id);
void gui_app_icon(surface_t *s, icon_id icon, int x, int y, int size);

/* Draw a registered glyph image in a chosen colour (the image is coverage, the
 * colour is the caller's) - for monochrome control glyphs (power, window
 * buttons) that must follow the theme and light up on hover.  Falls back to the
 * drawn gui_icon glyph when no image is registered. */
void gui_app_icon_tinted(surface_t *s, icon_id icon, int x, int y, int size,
                         colour_t tint);

/* Register a single-channel alpha mask (the OS logo) that gui_icon draws
 * tinted with the caller's colour - so the brand mark follows the accent. */
void gui_register_icon_mask(icon_id id, const unsigned char *alpha, int w, int h);

/* ------------------------------------------------------------- the controls
 *
 * See controls.c.  Hover and press are eased rather than switched, so a window
 * repaints until gui_controls_settling() says everything has arrived. */
bool   gui_controls_settling(void);

rect_t gui_switch_rect(int x, int y);
bool   gui_switch(surface_t *s, rect_t r, bool on, bool hover, bool enabled);
bool   gui_slider(surface_t *s, rect_t r, int value, int min, int max,
                  bool hover, bool dragging);
int    gui_slider_value_at(rect_t r, int x, int min, int max);
void   gui_dropdown(surface_t *s, rect_t r, const char *text, bool hover, bool open);
bool   gui_list_row(surface_t *s, rect_t r, const char *text, const char *detail,
                    icon_id icon, bool selected, bool hover);
void   gui_sidebar(surface_t *s, rect_t r);
int    gui_sidebar_item_width(icon_id icon, const char *label);
bool   gui_sidebar_item(surface_t *s, rect_t r, icon_id icon, const char *label,
                        bool selected, bool hover);
int    gui_section(surface_t *s, int x, int y, int width, const char *title);
int    gui_page_header(surface_t *s, int x, int y, int width, const char *title,
                       const char *description);
rect_t gui_setting_row(surface_t *s, int x, int y, int width, const char *label,
                       const char *detail, int control_w, int control_h);
int    gui_setting_row_height(const char *detail);
void   gui_badge(surface_t *s, int x, int y, const char *text, colour_t colour);
int    gui_badge_width(const char *text);
void   gui_search_field(surface_t *s, rect_t r, const char *text, bool focused,
                        const char *placeholder);
void   gui_progress_bar(surface_t *s, rect_t r, int percent, bool indeterminate);
void   gui_spinner(surface_t *s, int cx, int cy, int radius, colour_t colour);

/* ------------------------------------------------------------------ cursor */

/* The pointer shapes.  The window manager picks one from its hover test - a
 * window edge gives a resize shape, a text field the I-beam, a link the hand. */
typedef enum {
    CURSOR_ARROW = 0,
    CURSOR_SIZE_WE,     /* left/right edge   <->  */
    CURSOR_SIZE_NS,     /* top/bottom edge    |   */
    CURSOR_SIZE_NWSE,   /* NW/SE corner       \   */
    CURSOR_SIZE_NESW,   /* NE/SW corner       /   */
    CURSOR_TEXT,        /* over editable text I    */
    CURSOR_HAND,        /* over a link/button      */
} cursor_kind;

void        gui_set_cursor(cursor_kind k);   /* set the shape to draw          */
cursor_kind gui_cursor_get(void);
bool        gui_cursor_take_dirty(void);      /* did the shape just change?     */

void gui_cursor(surface_t *s, int x, int y);
/* The current pointer as a picture, for a display adapter that draws it itself.
 * Fills width/height/hotspot; returns the width. */
int  gui_cursor_image(uint32_t *out, int *width, int *height, int *hot_x, int *hot_y);

/* ------------------------------------------------------------------ display */

typedef struct {
    surface_t     screen;      /* the mapped framebuffer                 */
    surface_t    *back;        /* what everything draws into             */
    kframebuffer_t info;
    int           input_fd;
    bool          have_mouse;

    /* Whether there is a driver on the other end that wants to be told which
     * part of the screen changed.  Where there is, saying so is what makes a
     * frame cost what the copying costs; where there is not, the adapter has
     * to find out for itself and every frame pays for the search. */
    bool          driver_updates;
    /* And whether the adapter will draw the pointer, so that moving it neither
     * disturbs the picture nor costs a frame. */
    bool          hardware_cursor;
} display_t;

bool display_open(display_t *d);
bool display_open_gpu(display_t *d); /* native toolkit targets; legacy APIs opt out */
void display_close(display_t *d);
void display_refresh_cursor(display_t *d);   /* re-hand the current shape       */

/* Hand the screen back without giving up the mapping, so that a program run in
 * the foreground can have it, and take it again afterwards.  `display_take`
 * returns false if somebody else got there first. */
void display_give_up(display_t *d);
bool display_take(display_t *d);
/* Push a region of the back buffer to the screen. */
void display_present(display_t *d, rect_t area);
/* What video memory will actually accept, in words - see display.c. */
void display_measure(display_t *d, char *out, size_t cap);
/* Wait for input, or for the timeout - see display.c. */
bool display_wait_input(display_t *d, int timeout_ms);
/* Change resolution while running.  False where the adapter will not, in which
 * case the caller falls back to asking for it on the next start. */
bool display_set_mode(display_t *d, int width, int height);
void display_present_all(display_t *d);

/* Blocking read of the next input event; false when the device fails. */
bool display_wait_event(display_t *d, kinput_event_t *ev);
/* Non-blocking; false when nothing is queued. */
bool display_poll_event(display_t *d, kinput_event_t *ev);

#endif
