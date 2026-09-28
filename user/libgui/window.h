/* window.h - windows, widgets and the event loop.
 *
 * Windows are owned by one process; each has a callback that paints its client
 * area and another that receives input already translated into client
 * coordinates.  The manager handles the frame, dragging, resizing, focus and
 * z-order so an app only ever thinks about its own contents.
 */
#ifndef KESTREL_WINDOW_H
#define KESTREL_WINDOW_H

#include "gui.h"

typedef struct window window_t;
typedef struct wm wm_t;

/* Client-area event, in the window's own coordinates. */
typedef enum {
    WE_PAINT = 1,
    WE_MOUSE_DOWN, WE_MOUSE_UP, WE_MOUSE_MOVE, WE_MOUSE_WHEEL,
    WE_KEY_DOWN, WE_KEY_UP,
    WE_FOCUS, WE_BLUR,
    WE_RESIZE,
    WE_CLOSE,
    WE_TICK,
} wevent_kind;

typedef struct {
    wevent_kind kind;
    int      x, y;          /* client coordinates      */
    int      dx, dy;
    int      wheel;
    uint32_t button;        /* MB_*                    */
    uint32_t buttons;
    uint32_t key;           /* character or KK_*       */
    uint32_t mods;
} wevent_t;

/* Returning true means the window wants a repaint. */
typedef bool (*window_proc)(window_t *w, const wevent_t *ev);

#define WIN_TITLE_MAX 64

typedef enum {
    WIN_NORMAL = 0,
    WIN_MINIMISED,
    WIN_MAXIMISED,
} win_state;

/* ---------------------------------------------------------------- animation
 *
 * A window that appears, closes, or is put away to the taskbar moves and fades
 * rather than snapping.  The window paints itself once at its real size and
 * the compositor scales that one image for each frame, so an animation costs
 * the same whatever the window is drawing.
 */
typedef enum {
    ANIM_NONE = 0,
    ANIM_OPEN,        /* growing in from smaller, fading up            */
    ANIM_CLOSE,       /* shrinking away, fading down, then destroyed   */
    ANIM_MINIMISE,    /* collapsing towards its place on the taskbar   */
    ANIM_RESTORE,     /* coming back out of it                         */
    ANIM_GEOMETRY,    /* moving and resizing, as maximise does         */
} anim_kind;

typedef struct {
    anim_kind kind;
    uint64_t  started_ms;
    int       duration_ms;
    rect_t    from, to;
    int       alpha_from, alpha_to;
    /* One position per compositor frame; damage and drawing use this sample. */
    int       paint_progress;
    rect_t    damage_frame;
} window_anim;

struct window {
    char        title[WIN_TITLE_MAX];
    icon_id     icon;
    rect_t      frame;          /* outer rectangle, screen coordinates  */
    rect_t      restore;        /* geometry to return to from maximised */
    win_state   state;
    bool        visible;
    bool        resizable;
    bool        closable;
    int         min_w, min_h;

    surface_t  *canvas;         /* the client area                      */
    window_proc proc;
    void       *data;           /* whatever the app wants               */

    /* A window that has something moving in it of its own - a scene being
     * rendered, a video - and wants a frame whenever there is time for one,
     * rather than one every tick.  Without this the fastest anything can
     * animate is the tick rate, which is eight times a second: fine for a
     * clock and a caret, and hopeless for anything that is meant to look like
     * motion. */
    bool        continuous;

    bool        needs_paint;
    window_anim anim;
    bool        dying;          /* closing: destroyed when the animation ends */
    wm_t       *wm;
    bool        linked;         /* present in the z-order chain         */
    window_t   *below;          /* z-order: nearer the back             */
    window_t   *above;
};

/* What the compositor has been costing: frames drawn, microseconds each, and
 * how much of the screen each one covered. */
bool wm_has_damage(const wm_t *wm);
void wm_frame_report(char *out, size_t cap);

/* Move a window, using the display adapter to relocate its pixels when the
 * adapter will.  This is what dragging does; it is separate from the mouse so
 * that it can be driven without one. */
void wm_move_window(wm_t *wm, window_t *w, int x, int y);

/* What the adapter has drawn instead of the processor, for the session. */
void wm_adapter_counts(uint64_t *moves, uint64_t *pixels);
void wm_frame_reset(void);

#define WM_MAX_WINDOWS 24

struct wm {
    display_t  *display;
    window_t   *bottom, *top;   /* z-order chain                        */
    window_t   *focus;
    int         count;

    int         mouse_x, mouse_y;
    uint32_t    buttons;
    uint32_t    mods;

    /* Interaction in progress. */
    window_t   *drag;
    int         drag_dx, drag_dy;
    window_t   *resize;
    int         resize_edge;    /* bitmask: 1 left, 2 right, 4 top, 8 bottom */

    /* What must be redrawn this frame.
     *
     * A list rather than one rectangle, and that is the whole difference
     * between a compositor that redraws what changed and one that redraws
     * nearly everything.  A single bounding box round the two things that
     * actually moved - a taskbar clock at the bottom and a pointer in the
     * middle - covers most of the screen, and every pixel of it gets the full
     * treatment: background, window frames, shadows, and a copy out to video
     * memory.  Kept apart, the same frame is two small rectangles. */
#define WM_MAX_DAMAGE 12
    rect_t      damage[WM_MAX_DAMAGE];
    int         damage_count;
    bool        full_redraw;
    bool        running;
    bool        animating;      /* something is moving, so tick faster  */
    bool        drawing_continuously;  /* a window is rendering its own motion */
    bool        animations_on;  /* the user can turn them off           */

    /* Where a window goes when it is minimised - the desktop fills this in so
     * the animation can aim at the right taskbar button. */
    rect_t    (*taskbar_slot)(wm_t *wm, window_t *w);

    /* Filled in by the desktop so the manager can leave room for it. */
    rect_t      work_area;
    void      (*paint_background)(wm_t *wm, surface_t *s, rect_t area);
    bool      (*background_event)(wm_t *wm, const kinput_event_t *ev);
    /* Drawn above every window - a taskbar, a menu.  It is given the
     * rectangle being repainted so it can decline: with damage tracked as
     * several rectangles, an overlay that draws itself unconditionally draws
     * itself once per rectangle. */
    void      (*paint_overlay)(wm_t *wm, surface_t *s, rect_t area);
    /* Told when the screen itself changed size, so the shell can lay its own
     * furniture out again. */
    void      (*on_resize)(wm_t *wm, int width, int height);
    void      (*on_tick)(wm_t *wm);
    /* Pure sizing query, before allocation/hardware commit. It must not
     * mutate the manager, windows or display. on_resize is post-commit only. */
    rect_t    (*query_work_area)(wm_t *wm, int width, int height);
};

/* Frame metrics. */
/* Scaled, for the same reason the taskbar is: the title bar holds a line of
 * text and a row of buttons, and both grow with the interface. */
#define WIN_TITLE_H   (32 * gui_scale())
#define WIN_BORDER    1
/* The band along each edge that grabs a resize.  It MUST scale: a flat 6 px is
 * three logical pixels on a 2x display - a target almost impossible to hit,
 * which is what made resizing feel fiddly.  Ten scaled pixels is a comfortable
 * grab at every size. */
#define WIN_RESIZE_GRIP (10 * gui_scale())
/* The corner radius scales with everything else.
 *
 * Flat 7 pixels meant that at twice the interface size the corners read as
 * half as round - a 64 pixel title bar with the corner of a 32 pixel one,
 * which is the sort of thing that makes a window look slightly wrong without
 * anyone being able to say why.
 *
 * Every use of this means the same thing, including the insets that decide
 * whether one window covers another and which part of a window the adapter may
 * move, so they all follow together. */
#define WIN_RADIUS    (7 * gui_scale())

void      wm_init(wm_t *wm, display_t *d);
window_t *wm_create(wm_t *wm, const char *title, icon_id icon, rect_t client,
                    window_proc proc, void *data);
void      wm_close(wm_t *wm, window_t *w);
/* Close a window the way the user does: it animates away first, and is
 * destroyed when that finishes.  Everything the user can press goes through
 * this rather than wm_close. */
void      wm_request_close(wm_t *wm, window_t *w);
void      wm_focus(wm_t *wm, window_t *w);
void      wm_invalidate(wm_t *wm, window_t *w);
void      wm_damage(wm_t *wm, rect_t r);
void      wm_damage_all(wm_t *wm);
/* Change resolution while running.  False when the display will not, in which
 * case the caller falls back to asking for it on the next start. */
bool      wm_set_mode(wm_t *wm, int width, int height);
void      wm_run(wm_t *wm);
/* One turn of the loop without blocking, for a caller with a loop of its own.
 * True when something happened. */
bool      wm_pump(wm_t *wm);
void      wm_stop(wm_t *wm);
void      wm_minimise(wm_t *wm, window_t *w);

/* Start an animation on a window.  `duration_ms` of zero uses the default. */
void      wm_animate(wm_t *wm, window_t *w, anim_kind kind, rect_t from, rect_t to,
                     int alpha_from, int alpha_to, int duration_ms);
/* True while anything is moving. */
bool      wm_is_animating(wm_t *wm);
void      wm_restore(wm_t *wm, window_t *w);
void      wm_toggle_maximise(wm_t *wm, window_t *w);
window_t *wm_window_at(wm_t *wm, int x, int y);
rect_t    wm_client_rect(const window_t *w);   /* screen coordinates */
int       wm_window_index(wm_t *wm, window_t *w);
window_t *wm_window_by_index(wm_t *wm, int index);

/* ----------------------------------------------------------------- widgets */

/* Widgets are drawn immediately and hit-tested by the caller: with a single
 * process and small dialogs this stays far simpler than a retained tree, and
 * every app in the desktop fits comfortably in the model. */

typedef struct {
    rect_t   bounds;
    bool     hover;
    bool     pressed;
} widget_state;

bool gui_button(surface_t *s, rect_t r, const char *label, bool hover, bool pressed, bool enabled);
bool gui_button_icon(surface_t *s, rect_t r, icon_id icon, const char *label,
                     bool hover, bool pressed, bool enabled);
void gui_panel(surface_t *s, rect_t r, const char *title);
void gui_separator(surface_t *s, int x, int y, int w);
void gui_checkbox(surface_t *s, rect_t r, const char *label, bool checked, bool hover);
void gui_progress(surface_t *s, rect_t r, int percent);
void gui_scrollbar(surface_t *s, rect_t r, int offset, int visible, int total);
int  gui_scrollbar_hit(rect_t r, int offset, int visible, int total, int y);

/* A single-line text field with a caret and selection-free editing. */
typedef struct {
    char  *text;
    size_t cap;
    size_t len;
    size_t caret;
    int    scroll;
    bool   focused;
} textfield_t;

void gui_textfield_init(textfield_t *tf, char *storage, size_t cap);
void gui_textfield_draw(surface_t *s, rect_t r, textfield_t *tf);
bool gui_textfield_key(textfield_t *tf, uint32_t key, uint32_t mods);

#endif
