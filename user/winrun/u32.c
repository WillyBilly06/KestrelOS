/* u32.c - user32: windows, messages and controls.
 *
 * A Windows program with a window is built around one loop: fetch a message,
 * translate it, dispatch it to the procedure belonging to the window it was
 * for.  Everything the program does happens inside that procedure, in response
 * to a message.  So the whole of this file exists to make that loop turn: real
 * windows on the screen, real input arriving as messages, and paint messages
 * at the moments Windows would send them.
 *
 * The windows are the system's own - the same manager the desktop uses draws
 * the frame, moves them and decides which has focus.  What a Windows program
 * sees inside its client area is entirely its own.
 */
#include "win.h"
#include "gui.h"
#include "window.h"

/* ------------------------------------------------------------- the messages */

#define WM_NULL           0x0000
#define WM_CREATE         0x0001
#define WM_DESTROY        0x0002
#define WM_MOVE           0x0003
#define WM_SIZE           0x0005
#define WM_ACTIVATE       0x0006
#define WM_SETFOCUS       0x0007
#define WM_KILLFOCUS      0x0008
#define WM_ENABLE         0x000A
#define WM_SETTEXT        0x000C
#define WM_GETTEXT        0x000D
#define WM_GETTEXTLENGTH  0x000E
#define WM_PAINT          0x000F
#define WM_CLOSE          0x0010
#define WM_QUIT           0x0012
#define WM_ERASEBKGND     0x0014
#define WM_SHOWWINDOW     0x0018
#define WM_SETFONT        0x0030
#define WM_GETFONT        0x0031
#define WM_NCDESTROY      0x0082
#define WM_KEYDOWN        0x0100
#define WM_KEYUP          0x0101
#define WM_CHAR           0x0102
#define WM_SYSKEYDOWN     0x0104
#define WM_SYSKEYUP       0x0105
#define WM_COMMAND        0x0111
#define WM_TIMER          0x0113
#define WM_MOUSEMOVE      0x0200
#define WM_LBUTTONDOWN    0x0201
#define WM_LBUTTONUP      0x0202
#define WM_LBUTTONDBLCLK  0x0203
#define WM_RBUTTONDOWN    0x0204
#define WM_RBUTTONUP      0x0205
#define WM_MBUTTONDOWN    0x0207
#define WM_MBUTTONUP      0x0208
#define WM_MOUSEWHEEL     0x020A
#define WM_USER           0x0400

#define WS_VISIBLE        0x10000000
#define WS_CHILD          0x40000000
#define WS_DISABLED       0x08000000
#define WS_BORDER         0x00800000

#define SW_HIDE           0
#define SW_SHOWNORMAL     1
#define SW_SHOW           5

#define BN_CLICKED        0
#define EN_CHANGE         0x0300

#define VK_BACK   0x08
#define VK_TAB    0x09
#define VK_RETURN 0x0D
#define VK_SHIFT  0x10
#define VK_CONTROL 0x11
#define VK_ESCAPE 0x1B
#define VK_SPACE  0x20
#define VK_PRIOR  0x21
#define VK_NEXT   0x22
#define VK_END    0x23
#define VK_HOME   0x24
#define VK_LEFT   0x25
#define VK_UP     0x26
#define VK_RIGHT  0x27
#define VK_DOWN   0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_F1     0x70

typedef struct {
    HANDLE  hwnd;
    UINT    message;
    WPARAM  wParam;
    LPARAM  lParam;
    DWORD   time;
    POINT   pt;
} MSG;

typedef LRESULT WINAPI (*WNDPROC)(HANDLE, UINT, WPARAM, LPARAM);

typedef struct {
    UINT     cbSize, style;
    WNDPROC  lpfnWndProc;
    int      cbClsExtra, cbWndExtra;
    HANDLE   hInstance, hIcon, hCursor, hbrBackground;
    const char *lpszMenuName, *lpszClassName;
    HANDLE   hIconSm;
} WNDCLASSEXA;

typedef struct {
    UINT     style;
    WNDPROC  lpfnWndProc;
    int      cbClsExtra, cbWndExtra;
    HANDLE   hInstance, hIcon, hCursor, hbrBackground;
    const char *lpszMenuName, *lpszClassName;
} WNDCLASSA;

typedef struct {
    HANDLE hdc;
    BOOL   fErase;
    RECT   rcPaint;
    BOOL   fRestore, fIncUpdate;
    BYTE   rgbReserved[32];
} PAINTSTRUCT;

/* --------------------------------------------------------------- the state */

#define MAX_WINDOWS  32
#define MAX_CLASSES  32
#define MAX_MESSAGES 128
#define MAX_TIMERS   16

typedef struct hwnd_rec {
    bool     used;
    char     cls[64];
    char     title[160];
    DWORD    style, exstyle;
    RECT     rect;                 /* client area: screen for top level,
                                    * parent-relative for a child */
    struct hwnd_rec *parent;
    WNDPROC  proc;
    LONG_PTR userdata;
    HANDLE   instance;
    int      id;
    bool     visible, enabled;
    bool     pressed, hover, focused;

    window_t *native;              /* the system window, top level only */
    surface_t *canvas;             /* where painting goes */
    /* An edit control keeps its own text and caret. */
    size_t   caret;
} hwnd_rec;

typedef struct {
    bool    used;
    char    name[64];
    WNDCLASSEXA info;
} class_rec;

typedef struct { bool used; hwnd_rec *owner; UINT_PTR id; DWORD period; uint64_t next; } timer_rec;

static hwnd_rec  windows[MAX_WINDOWS];
static class_rec classes[MAX_CLASSES];
static timer_rec timers[MAX_TIMERS];

static MSG  queue[MAX_MESSAGES];
static int  queue_head, queue_tail;
static bool quit_posted;
static int  quit_code;

static display_t display;
static wm_t      manager;
static bool      gui_up;
static hwnd_rec *focus_window;
static hwnd_rec *capture_window;

/* ------------------------------------------------------------------ handles */

/* A window handle is the address of its record, which is unique and cannot be
 * confused with anything else a program holds. */
static HANDLE h_of(hwnd_rec *w) { return (HANDLE)w; }

static hwnd_rec *rec_of(HANDLE h) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].used && &windows[i] == (hwnd_rec *)h) return &windows[i];
    return NULL;
}

surface_t *u32_surface_for(void *h) {
    hwnd_rec *w = rec_of(h);
    if (!w) return NULL;
    while (w->parent) w = w->parent;             /* children draw on the parent */
    /* Straight from the window rather than from a copy taken when it was
     * created: resizing replaces the canvas, and a copy would then be a
     * pointer into memory that has been given back. */
    return w->native ? w->native->canvas : w->canvas;
}

/* Where a window's client area starts inside the top-level canvas. */
void u32_client_origin(void *h, int *x, int *y) {
    hwnd_rec *w = rec_of(h);
    int ox = 0, oy = 0;
    while (w && w->parent) { ox += w->rect.left; oy += w->rect.top; w = w->parent; }
    if (x) *x = ox;
    if (y) *y = oy;
}

void u32_invalidate(void *h) {
    hwnd_rec *w = rec_of(h);
    while (w && w->parent) w = w->parent;
    if (w && w->native && gui_up) wm_invalidate(&manager, w->native);
}

/* --------------------------------------------------------------- the queue */

static void post(hwnd_rec *w, UINT message, WPARAM wp, LPARAM lp) {
    int next = (queue_tail + 1) % MAX_MESSAGES;
    if (next == queue_head) return;              /* full: the oldest wins */
    queue[queue_tail].hwnd = w ? h_of(w) : NULL;
    queue[queue_tail].message = message;
    queue[queue_tail].wParam = wp;
    queue[queue_tail].lParam = lp;
    queue[queue_tail].time = (DWORD)uptime_ms();
    queue[queue_tail].pt.x = manager.mouse_x;
    queue[queue_tail].pt.y = manager.mouse_y;
    queue_tail = next;
}

static bool take(MSG *out) {
    if (queue_head == queue_tail) return false;
    *out = queue[queue_head];
    queue_head = (queue_head + 1) % MAX_MESSAGES;
    return true;
}

/* --------------------------------------------------------------- the classes */

static class_rec *find_class(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < MAX_CLASSES; i++)
        if (classes[i].used && !strcasecmp(classes[i].name, name)) return &classes[i];
    return NULL;
}

/* The built-in control classes.  A program that asks for a BUTTON gets one
 * drawn by this system rather than by its own code, which is what happens on
 * Windows too. */
static bool is_builtin_class(const char *name) {
    return !strcasecmp(name, "BUTTON") || !strcasecmp(name, "STATIC") ||
           !strcasecmp(name, "EDIT")   || !strcasecmp(name, "LISTBOX");
}

/* -------------------------------------------------------------- the painting */

static void paint_children(hwnd_rec *parent, surface_t *s, int ox, int oy);

static void paint_control(hwnd_rec *w, surface_t *s, int ox, int oy) {
    rect_t r = rect_make(ox + w->rect.left, oy + w->rect.top,
                         w->rect.right - w->rect.left, w->rect.bottom - w->rect.top);
    if (!strcasecmp(w->cls, "BUTTON")) {
        gui_button(s, r, w->title, w->hover, w->pressed, w->enabled);
    } else if (!strcasecmp(w->cls, "STATIC")) {
        gui_text(s, FONT_UI, r.x, r.y + (r.h - gui_font_height(FONT_UI)) / 2,
                 w->title, g_theme.text);
    } else if (!strcasecmp(w->cls, "EDIT")) {
        gui_fill(s, r, g_theme.field);
        gui_frame(s, r, w->focused ? g_theme.accent : g_theme.field_border);
        gui_text(s, FONT_UI, r.x + 6, r.y + (r.h - gui_font_height(FONT_UI)) / 2,
                 w->title, g_theme.field_text);
        if (w->focused && (uptime_ms() / 500) % 2 == 0) {
            int cx = r.x + 6 + gui_text_width_n(FONT_UI, w->title, w->caret);
            gui_vline(s, cx, r.y + 4, r.h - 8, g_theme.field_text);
        }
    } else if (!strcasecmp(w->cls, "LISTBOX")) {
        gui_fill(s, r, g_theme.field);
        gui_frame(s, r, g_theme.field_border);
    }
}

static void paint_children(hwnd_rec *parent, surface_t *s, int ox, int oy) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        hwnd_rec *w = &windows[i];
        if (!w->used || w->parent != parent || !w->visible) continue;
        if (is_builtin_class(w->cls)) {
            paint_control(w, s, ox, oy);
        } else if (w->proc) {
            /* A child of the program's own class paints itself, the same way
             * a top-level window does. */
            w->canvas = s;
            w->proc(h_of(w), WM_PAINT, 0, 0);
        }
        paint_children(w, s, ox + w->rect.left, oy + w->rect.top);
    }
}

/* Which child, if any, is under a point in the parent's client area. */
static hwnd_rec *child_at(hwnd_rec *parent, int x, int y) {
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        hwnd_rec *w = &windows[i];
        if (!w->used || w->parent != parent || !w->visible || !w->enabled) continue;
        if (x >= w->rect.left && x < w->rect.right && y >= w->rect.top && y < w->rect.bottom)
            return w;
    }
    return NULL;
}

/* ------------------------------------------------------ the system's window */

static uint32_t vk_from_key(uint32_t key) {
    switch (key) {
    case KK_UP: return VK_UP;
    case KK_DOWN: return VK_DOWN;
    case KK_LEFT: return VK_LEFT;
    case KK_RIGHT: return VK_RIGHT;
    case KK_HOME: return VK_HOME;
    case KK_END: return VK_END;
    case KK_PAGEUP: return VK_PRIOR;
    case KK_PAGEDOWN: return VK_NEXT;
    case KK_INSERT: return VK_INSERT;
    case KK_DELETE: return VK_DELETE;
    case '\b': return VK_BACK;
    case '\t': return VK_TAB;
    case '\n': case '\r': return VK_RETURN;
    case 27: return VK_ESCAPE;
    default: break;
    }
    if (key >= KK_F1 && key <= KK_F12) return VK_F1 + (key - KK_F1);
    if (key >= 'a' && key <= 'z') return key - 32;      /* virtual keys are upper case */
    if (key < 0x100) return key;
    return 0;
}

/* Everything the window manager sends about one window arrives here and comes
 * out the other side as Windows messages. */
static bool native_proc(window_t *nw, const wevent_t *ev) {
    hwnd_rec *w = nw->data;
    if (!w || !w->used) return false;

    switch (ev->kind) {
    case WE_PAINT:
        w->canvas = nw->canvas;
        /* Windows sends the erase first, then the paint; a program that draws
         * its own background answers the first and the second overdraws it. */
        if (w->proc) {
            gui_clear(nw->canvas, g_theme.window);
            w->proc(h_of(w), WM_PAINT, 0, 0);
        } else {
            gui_clear(nw->canvas, g_theme.window);
        }
        paint_children(w, nw->canvas, 0, 0);
        return false;

    case WE_MOUSE_MOVE: {
        hwnd_rec *under = child_at(w, ev->x, ev->y);
        bool changed = false;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            hwnd_rec *c = &windows[i];
            if (!c->used || c->parent != w) continue;
            bool hover = (c == under);
            if (c->hover != hover) { c->hover = hover; changed = true; }
        }
        post(w, WM_MOUSEMOVE, ev->buttons, (LPARAM)((ev->y << 16) | (ev->x & 0xFFFF)));
        return changed;
    }

    case WE_MOUSE_DOWN: {
        hwnd_rec *under = capture_window ? capture_window : child_at(w, ev->x, ev->y);
        if (under && is_builtin_class(under->cls)) {
            under->pressed = true;
            if (!strcasecmp(under->cls, "EDIT")) {
                if (focus_window) focus_window->focused = false;
                focus_window = under;
                under->focused = true;
                under->caret = strlen(under->title);
            }
            return true;
        }
        if (under && under->proc) {
            post(under, ev->button == MB_RIGHT ? WM_RBUTTONDOWN : WM_LBUTTONDOWN,
                 ev->buttons, (LPARAM)(((ev->y - under->rect.top) << 16) |
                                       ((ev->x - under->rect.left) & 0xFFFF)));
            return false;
        }
        if (focus_window) { focus_window->focused = false; focus_window = NULL; }
        post(w, ev->button == MB_RIGHT ? WM_RBUTTONDOWN : WM_LBUTTONDOWN,
             ev->buttons, (LPARAM)((ev->y << 16) | (ev->x & 0xFFFF)));
        return true;
    }

    case WE_MOUSE_UP: {
        hwnd_rec *under = child_at(w, ev->x, ev->y);
        bool repaint = false;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            hwnd_rec *c = &windows[i];
            if (!c->used || c->parent != w || !c->pressed) continue;
            c->pressed = false;
            repaint = true;
            /* A button releases into a command, which is the message a
             * program actually listens for. */
            if (c == under && !strcasecmp(c->cls, "BUTTON"))
                post(w, WM_COMMAND, (WPARAM)((BN_CLICKED << 16) | (c->id & 0xFFFF)),
                     (LPARAM)(uintptr_t)h_of(c));
        }
        post(w, ev->button == MB_RIGHT ? WM_RBUTTONUP : WM_LBUTTONUP,
             ev->buttons, (LPARAM)((ev->y << 16) | (ev->x & 0xFFFF)));
        return repaint;
    }

    case WE_MOUSE_WHEEL:
        post(w, WM_MOUSEWHEEL, (WPARAM)((int64_t)(ev->wheel * 120) << 16),
             (LPARAM)((ev->y << 16) | (ev->x & 0xFFFF)));
        return false;

    case WE_KEY_DOWN: {
        /* A focused edit control eats the key; anything else goes to the
         * window's own procedure. */
        if (focus_window && !strcasecmp(focus_window->cls, "EDIT")) {
            hwnd_rec *e = focus_window;
            size_t len = strlen(e->title);
            if (ev->key == '\b') {
                if (e->caret > 0) {
                    memmove(e->title + e->caret - 1, e->title + e->caret, len - e->caret + 1);
                    e->caret--;
                }
            } else if (ev->key == KK_LEFT) {
                if (e->caret) e->caret--;
            } else if (ev->key == KK_RIGHT) {
                if (e->caret < len) e->caret++;
            } else if (ev->key >= 0x20 && ev->key < 0x7F && len + 1 < sizeof e->title) {
                memmove(e->title + e->caret + 1, e->title + e->caret, len - e->caret + 1);
                e->title[e->caret++] = (char)ev->key;
            } else {
                post(w, WM_KEYDOWN, vk_from_key(ev->key), 1);
                return false;
            }
            if (e->parent)
                post(e->parent, WM_COMMAND,
                     (WPARAM)((EN_CHANGE << 16) | (e->id & 0xFFFF)), (LPARAM)(uintptr_t)h_of(e));
            return true;
        }
        post(w, WM_KEYDOWN, vk_from_key(ev->key), 1);
        if (ev->key >= 0x20 && ev->key < 0x7F) post(w, WM_CHAR, ev->key, 1);
        else if (ev->key == '\r' || ev->key == '\n') post(w, WM_CHAR, '\r', 1);
        else if (ev->key == '\b' || ev->key == '\t' || ev->key == 27) post(w, WM_CHAR, ev->key, 1);
        return false;
    }

    case WE_KEY_UP:
        post(w, WM_KEYUP, vk_from_key(ev->key), 1);
        return false;

    case WE_RESIZE:
        w->rect.right = w->rect.left + nw->canvas->width;
        w->rect.bottom = w->rect.top + nw->canvas->height;
        post(w, WM_SIZE, 0, (LPARAM)((nw->canvas->height << 16) | (nw->canvas->width & 0xFFFF)));
        return true;

    case WE_FOCUS:  post(w, WM_SETFOCUS, 0, 0); return false;
    case WE_BLUR:   post(w, WM_KILLFOCUS, 0, 0); return false;
    case WE_CLOSE:  post(w, WM_CLOSE, 0, 0); return false;

    case WE_TICK: {
        bool repaint = false;
        uint64_t now = uptime_ms();
        for (int i = 0; i < MAX_TIMERS; i++) {
            if (!timers[i].used || timers[i].owner != w) continue;
            if (now < timers[i].next) continue;
            timers[i].next = now + timers[i].period;
            post(w, WM_TIMER, (WPARAM)timers[i].id, 0);
        }
        /* The caret in a focused edit control blinks, which needs a repaint
         * twice a second and at no other time. */
        if (focus_window && focus_window->parent == w &&
            !strcasecmp(focus_window->cls, "EDIT")) repaint = true;
        return repaint;
    }

    default:
        return false;
    }
}

/* ------------------------------------------------------------- bringing up */

bool u32_start_gui(void) {
    if (gui_up) return true;
    if (!display_open(&display)) return false;
    wm_init(&manager, &display);
    manager.work_area = rect_make(0, 0, display.back->width, display.back->height);
    gui_up = true;
    return true;
}

bool u32_gui_running(void) { return gui_up; }

void u32_shutdown(void) {
    if (!gui_up) return;
    gui_up = false;
    display_close(&display);
}

/* One turn of the manager: input in, painting out, without blocking. */
bool wm_pump(wm_t *wm);

static void pump_once(void) {
    if (!gui_up) return;
    wm_pump(&manager);
}

/* --------------------------------------------------------------- the windows */

static hwnd_rec *new_window(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].used) continue;
        memset(&windows[i], 0, sizeof windows[i]);
        windows[i].used = true;
        windows[i].enabled = true;
        return &windows[i];
    }
    return NULL;
}

static LRESULT WINAPI w_DefWindowProcA(HANDLE h, UINT msg, WPARAM wp, LPARAM lp);

static HANDLE WINAPI w_CreateWindowExA(DWORD exstyle, const char *cls, const char *title,
                                       DWORD style, int x, int y, int cw, int ch,
                                       HANDLE parent, HANDLE menu, HANDLE instance, void *param) {
    if (!cls) { win_set_error(ERROR_INVALID_PARAMETER); return NULL; }

    hwnd_rec *w = new_window();
    if (!w) { win_set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }

    strlcpy(w->cls, cls, sizeof w->cls);
    strlcpy(w->title, title ? title : "", sizeof w->title);
    w->style = style;
    w->exstyle = exstyle;
    w->instance = instance;
    w->parent = rec_of(parent);
    w->id = (int)(uintptr_t)menu;
    w->visible = (style & WS_VISIBLE) != 0;

    /* CW_USEDEFAULT, which means "you choose". */
    if (x == (int)0x80000000) x = 80;
    if (y == (int)0x80000000) y = 60;
    if (cw == (int)0x80000000 || cw <= 0) cw = 640;
    if (ch == (int)0x80000000 || ch <= 0) ch = 480;

    w->rect.left = x;
    w->rect.top = y;
    w->rect.right = x + cw;
    w->rect.bottom = y + ch;

    class_rec *c = find_class(cls);
    if (c) w->proc = c->info.lpfnWndProc;
    else if (!is_builtin_class(cls)) {
        win_trace("window class \"%s\" was never registered", cls);
        w->proc = NULL;
    }

    if (!(style & WS_CHILD) || !w->parent) {
        if (!u32_start_gui()) {
            w->used = false;
            win_fail("a window was asked for and there is no display to put it on");
        }
        /* A top-level window gets a real one, positioned where it asked. */
        rect_t client = rect_make(x, y + WIN_TITLE_H, cw, ch);
        w->native = wm_create(&manager, w->title, ICON_FILE, client, native_proc, w);
        if (!w->native) { w->used = false; win_set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
        w->canvas = w->native->canvas;
        w->native->visible = w->visible;
        w->rect.left = 0;
        w->rect.top = 0;
        w->rect.right = cw;
        w->rect.bottom = ch;
    }

    if (w->proc) {
        w->proc(h_of(w), WM_CREATE, 0, (LPARAM)(uintptr_t)param);
        w->proc(h_of(w), WM_SIZE, 0, (LPARAM)((ch << 16) | (cw & 0xFFFF)));
    }
    return h_of(w);
}

static HANDLE WINAPI w_CreateWindowExW(DWORD exstyle, const WCHAR *cls, const WCHAR *title,
                                       DWORD style, int x, int y, int cw, int ch,
                                       HANDLE parent, HANDLE menu, HANDLE instance, void *param) {
    char c[64], t[160];
    win_wide_to_utf8(cls, c, sizeof c);
    win_wide_to_utf8(title, t, sizeof t);
    return w_CreateWindowExA(exstyle, c, t, style, x, y, cw, ch, parent, menu, instance, param);
}

static BOOL WINAPI w_DestroyWindow(HANDLE h) {
    hwnd_rec *w = rec_of(h);
    if (!w) { win_set_error(ERROR_INVALID_HANDLE); return WIN_FALSE; }
    if (w->proc) w->proc(h, WM_DESTROY, 0, 0);
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].parent == w) windows[i].used = false;
    if (w->native && gui_up) wm_close(&manager, w->native);
    w->used = false;
    if (focus_window == w) focus_window = NULL;
    return WIN_TRUE;
}

static BOOL WINAPI w_ShowWindow(HANDLE h, int how) {
    hwnd_rec *w = rec_of(h);
    if (!w) return WIN_FALSE;
    bool was = w->visible;
    w->visible = (how != SW_HIDE);
    if (w->native) {
        w->native->visible = w->visible;
        if (gui_up) wm_damage_all(&manager);
    }
    return was ? WIN_TRUE : WIN_FALSE;
}
static BOOL WINAPI w_UpdateWindow(HANDLE h) { u32_invalidate(h); return WIN_TRUE; }

static BOOL WINAPI w_InvalidateRect(HANDLE h, const RECT *r, BOOL erase) {
    (void)r; (void)erase;
    u32_invalidate(h);
    return WIN_TRUE;
}

static BOOL WINAPI w_GetClientRect(HANDLE h, RECT *out) {
    hwnd_rec *w = rec_of(h);
    if (!w || !out) return WIN_FALSE;
    out->left = 0;
    out->top = 0;
    out->right = w->rect.right - w->rect.left;
    out->bottom = w->rect.bottom - w->rect.top;
    return WIN_TRUE;
}

static BOOL WINAPI w_GetWindowRect(HANDLE h, RECT *out) {
    hwnd_rec *w = rec_of(h);
    if (!w || !out) return WIN_FALSE;
    if (w->native) {
        out->left = w->native->frame.x;
        out->top = w->native->frame.y;
        out->right = out->left + w->native->frame.w;
        out->bottom = out->top + w->native->frame.h;
    } else {
        *out = w->rect;
    }
    return WIN_TRUE;
}

static BOOL WINAPI w_MoveWindow(HANDLE h, int x, int y, int cw, int ch, BOOL repaint) {
    hwnd_rec *w = rec_of(h);
    if (!w) return WIN_FALSE;
    if (w->parent) {
        w->rect.left = x; w->rect.top = y;
        w->rect.right = x + cw; w->rect.bottom = y + ch;
    }
    if (repaint) u32_invalidate(h);
    return WIN_TRUE;
}

static BOOL WINAPI w_SetWindowTextA(HANDLE h, const char *text) {
    hwnd_rec *w = rec_of(h);
    if (!w) return WIN_FALSE;
    strlcpy(w->title, text ? text : "", sizeof w->title);
    if (w->native) strlcpy(w->native->title, w->title, WIN_TITLE_MAX);
    u32_invalidate(h);
    return WIN_TRUE;
}
static int WINAPI w_GetWindowTextA(HANDLE h, char *out, int cap) {
    hwnd_rec *w = rec_of(h);
    if (!w || !out || cap <= 0) return 0;
    return (int)strlcpy(out, w->title, (size_t)cap);
}
static int WINAPI w_GetWindowTextLengthA(HANDLE h) {
    hwnd_rec *w = rec_of(h);
    return w ? (int)strlen(w->title) : 0;
}

static HANDLE WINAPI w_GetDlgItem(HANDLE parent, int id) {
    hwnd_rec *p = rec_of(parent);
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].parent == p && windows[i].id == id)
            return h_of(&windows[i]);
    return NULL;
}
static BOOL WINAPI w_SetDlgItemTextA(HANDLE parent, int id, const char *text) {
    return w_SetWindowTextA(w_GetDlgItem(parent, id), text);
}
static UINT WINAPI w_GetDlgItemTextA(HANDLE parent, int id, char *out, int cap) {
    return (UINT)w_GetWindowTextA(w_GetDlgItem(parent, id), out, cap);
}

static LONG_PTR WINAPI w_SetWindowLongPtrA(HANDLE h, int index, LONG_PTR value) {
    hwnd_rec *w = rec_of(h);
    if (!w) return 0;
    LONG_PTR old = 0;
    if (index == -21 /* GWLP_USERDATA */) { old = w->userdata; w->userdata = value; }
    else if (index == -4 /* GWLP_WNDPROC */) { old = (LONG_PTR)(uintptr_t)w->proc; w->proc = (WNDPROC)(uintptr_t)value; }
    else if (index == -16 /* GWL_STYLE */) { old = w->style; w->style = (DWORD)value; }
    return old;
}
static LONG_PTR WINAPI w_GetWindowLongPtrA(HANDLE h, int index) {
    hwnd_rec *w = rec_of(h);
    if (!w) return 0;
    if (index == -21) return w->userdata;
    if (index == -4) return (LONG_PTR)(uintptr_t)w->proc;
    if (index == -16) return w->style;
    return 0;
}

static HANDLE WINAPI w_SetFocus(HANDLE h) {
    hwnd_rec *old = focus_window;
    if (focus_window) focus_window->focused = false;
    focus_window = rec_of(h);
    if (focus_window) focus_window->focused = true;
    return old ? h_of(old) : NULL;
}
static HANDLE WINAPI w_GetFocus(void) { return focus_window ? h_of(focus_window) : NULL; }
static HANDLE WINAPI w_SetCapture(HANDLE h) { capture_window = rec_of(h); return h; }
static BOOL   WINAPI w_ReleaseCapture(void) { capture_window = NULL; return WIN_TRUE; }
static BOOL   WINAPI w_EnableWindow(HANDLE h, BOOL enable) {
    hwnd_rec *w = rec_of(h);
    if (!w) return WIN_FALSE;
    bool was = w->enabled;
    w->enabled = enable != 0;
    u32_invalidate(h);
    return was ? WIN_TRUE : WIN_FALSE;
}

/* ---------------------------------------------------------------- the loop */

static BOOL WINAPI w_GetMessageA(MSG *msg, HANDLE filter, UINT first, UINT last) {
    (void)filter; (void)first; (void)last;
    if (!msg) return WIN_FALSE;
    for (;;) {
        if (quit_posted && queue_head == queue_tail) {
            msg->hwnd = NULL;
            msg->message = WM_QUIT;
            msg->wParam = (WPARAM)quit_code;
            msg->lParam = 0;
            return WIN_FALSE;                    /* false is how the loop ends */
        }
        if (take(msg)) return msg->message == WM_QUIT ? WIN_FALSE : WIN_TRUE;
        pump_once();
        if (queue_head == queue_tail && !quit_posted) sleep_ms(4);
    }
}

static BOOL WINAPI w_PeekMessageA(MSG *msg, HANDLE filter, UINT first, UINT last, UINT remove) {
    (void)filter; (void)first; (void)last;
    if (!msg) return WIN_FALSE;
    pump_once();
    if (quit_posted && queue_head == queue_tail) {
        msg->hwnd = NULL;
        msg->message = WM_QUIT;
        msg->wParam = (WPARAM)quit_code;
        return WIN_TRUE;
    }
    if (queue_head == queue_tail) return WIN_FALSE;
    if (remove & 1 /* PM_REMOVE */) return take(msg) ? WIN_TRUE : WIN_FALSE;
    *msg = queue[queue_head];
    return WIN_TRUE;
}

static BOOL WINAPI w_TranslateMessage(const MSG *msg) { (void)msg; return WIN_TRUE; }

static LRESULT WINAPI w_DispatchMessageA(const MSG *msg) {
    if (!msg) return 0;
    hwnd_rec *w = rec_of(msg->hwnd);
    if (!w) return 0;
    if (msg->message == WM_TIMER && !w->proc) return 0;
    if (!w->proc) return w_DefWindowProcA(msg->hwnd, msg->message, msg->wParam, msg->lParam);
    return w->proc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
}

static void WINAPI w_PostQuitMessage(int code) { quit_posted = true; quit_code = code; }

static BOOL WINAPI w_PostMessageA(HANDLE h, UINT msg, WPARAM wp, LPARAM lp) {
    post(rec_of(h), msg, wp, lp);
    return WIN_TRUE;
}

static LRESULT WINAPI w_SendMessageA(HANDLE h, UINT msg, WPARAM wp, LPARAM lp) {
    hwnd_rec *w = rec_of(h);
    if (!w) return 0;
    /* The messages that operate on a control are answered here rather than by
     * the program, because this system owns the control. */
    switch (msg) {
    case WM_SETTEXT:  w_SetWindowTextA(h, (const char *)lp); return WIN_TRUE;
    case WM_GETTEXT:  return w_GetWindowTextA(h, (char *)lp, (int)wp);
    case WM_GETTEXTLENGTH: return w_GetWindowTextLengthA(h);
    case WM_SETFONT:  return 0;
    default: break;
    }
    if (!w->proc) return w_DefWindowProcA(h, msg, wp, lp);
    return w->proc(h, msg, wp, lp);
}

static LRESULT WINAPI w_DefWindowProcA(HANDLE h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)wp; (void)lp;
    switch (msg) {
    case WM_CLOSE:
        w_DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        w_PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        return 0;
    }
}

/* --------------------------------------------------------------- the classes */

static WORD WINAPI w_RegisterClassExA(const WNDCLASSEXA *cls) {
    if (!cls || !cls->lpszClassName) { win_set_error(ERROR_INVALID_PARAMETER); return 0; }
    for (int i = 0; i < MAX_CLASSES; i++) {
        if (classes[i].used) continue;
        classes[i].used = true;
        strlcpy(classes[i].name, cls->lpszClassName, sizeof classes[i].name);
        classes[i].info = *cls;
        return (WORD)(i + 1);
    }
    win_set_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

static WORD WINAPI w_RegisterClassA(const WNDCLASSA *cls) {
    if (!cls) return 0;
    WNDCLASSEXA ex;
    memset(&ex, 0, sizeof ex);
    ex.cbSize = sizeof ex;
    ex.style = cls->style;
    ex.lpfnWndProc = cls->lpfnWndProc;
    ex.cbClsExtra = cls->cbClsExtra;
    ex.cbWndExtra = cls->cbWndExtra;
    ex.hInstance = cls->hInstance;
    ex.hIcon = cls->hIcon;
    ex.hCursor = cls->hCursor;
    ex.hbrBackground = cls->hbrBackground;
    ex.lpszClassName = cls->lpszClassName;
    return w_RegisterClassExA(&ex);
}
static BOOL WINAPI w_UnregisterClassA(const char *name, HANDLE inst) {
    (void)inst;
    class_rec *c = find_class(name);
    if (c) c->used = false;
    return WIN_TRUE;
}

/* ---------------------------------------------------------------- painting */

static HANDLE WINAPI w_BeginPaint(HANDLE h, PAINTSTRUCT *ps) {
    extern HANDLE gdi_dc_for_window(void *hwnd);
    hwnd_rec *w = rec_of(h);
    if (!w || !ps) return NULL;
    memset(ps, 0, sizeof *ps);
    ps->rcPaint.right = w->rect.right - w->rect.left;
    ps->rcPaint.bottom = w->rect.bottom - w->rect.top;
    ps->hdc = gdi_dc_for_window(h);
    return ps->hdc;
}
static BOOL WINAPI w_EndPaint(HANDLE h, const PAINTSTRUCT *ps) {
    extern void gdi_release_dc(HANDLE dc);
    (void)h;
    if (ps) gdi_release_dc(ps->hdc);
    return WIN_TRUE;
}
static HANDLE WINAPI w_GetDC(HANDLE h) {
    extern HANDLE gdi_dc_for_window(void *hwnd);
    return gdi_dc_for_window(h);
}
static int WINAPI w_ReleaseDC(HANDLE h, HANDLE dc) {
    extern void gdi_release_dc(HANDLE dc);
    (void)h;
    gdi_release_dc(dc);
    return 1;
}

/* ----------------------------------------------------------------- timers */

static UINT_PTR WINAPI w_SetTimer(HANDLE h, UINT_PTR id, UINT period, void *fn) {
    (void)fn;
    hwnd_rec *w = rec_of(h);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].used && timers[i].owner == w && timers[i].id == id) {
            timers[i].period = period;
            timers[i].next = uptime_ms() + period;
            return id;
        }
    }
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].used) continue;
        timers[i].used = true;
        timers[i].owner = w;
        timers[i].id = id;
        timers[i].period = period ? period : 1;
        timers[i].next = uptime_ms() + period;
        return id;
    }
    return 0;
}
static BOOL WINAPI w_KillTimer(HANDLE h, UINT_PTR id) {
    hwnd_rec *w = rec_of(h);
    for (int i = 0; i < MAX_TIMERS; i++)
        if (timers[i].used && timers[i].owner == w && timers[i].id == id) timers[i].used = false;
    return WIN_TRUE;
}

/* -------------------------------------------------------------- message box */

#define MB_OK             0x0000
#define MB_OKCANCEL       0x0001
#define MB_YESNO          0x0004
#define IDOK              1
#define IDCANCEL          2
#define IDYES             6
#define IDNO              7

typedef struct { const char *text; int result; } mb_button;

static int messagebox_result;
static const char *messagebox_text;
static const char *messagebox_title;
static mb_button messagebox_buttons[3];
static int messagebox_count;
static int messagebox_hover = -1;
static window_t *messagebox_window;

static rect_t mb_button_rect(surface_t *s, int i) {
    int bw = 96, bh = 30, gap = 10;
    int total = messagebox_count * bw + (messagebox_count - 1) * gap;
    int x = (s->width - total) / 2 + i * (bw + gap);
    return rect_make(x, s->height - bh - 14, bw, bh);
}

static bool messagebox_proc(window_t *nw, const wevent_t *ev) {
    switch (ev->kind) {
    case WE_PAINT:
        gui_clear(nw->canvas, g_theme.window);
        gui_text_wrapped(nw->canvas, FONT_UI, rect_make(16, 16, nw->canvas->width - 32, 80),
                         messagebox_text, g_theme.text);
        for (int i = 0; i < messagebox_count; i++)
            gui_button(nw->canvas, mb_button_rect(nw->canvas, i), messagebox_buttons[i].text,
                       messagebox_hover == i, false, true);
        return false;
    case WE_MOUSE_MOVE: {
        int was = messagebox_hover;
        messagebox_hover = -1;
        for (int i = 0; i < messagebox_count; i++)
            if (rect_contains(mb_button_rect(nw->canvas, i), ev->x, ev->y)) messagebox_hover = i;
        return was != messagebox_hover;
    }
    case WE_MOUSE_UP:
        for (int i = 0; i < messagebox_count; i++)
            if (rect_contains(mb_button_rect(nw->canvas, i), ev->x, ev->y))
                messagebox_result = messagebox_buttons[i].result;
        return false;
    case WE_KEY_DOWN:
        if (ev->key == '\r' || ev->key == '\n') messagebox_result = messagebox_buttons[0].result;
        if (ev->key == 27) messagebox_result = messagebox_count > 1 ? IDCANCEL : IDOK;
        return false;
    case WE_CLOSE:
        messagebox_result = messagebox_count > 1 ? IDCANCEL : IDOK;
        return false;
    default:
        return false;
    }
}

static int WINAPI w_MessageBoxA(HANDLE owner, const char *text, const char *title, UINT type) {
    (void)owner;
    /* Without a display there is still something useful to do: say it. */
    if (!gui_up && !u32_start_gui()) {
        printf("\n[%s] %s\n", title ? title : "Message", text ? text : "");
        flush_output();
        return IDOK;
    }

    messagebox_text = text ? text : "";
    messagebox_title = title ? title : "Message";
    messagebox_result = 0;
    messagebox_hover = -1;
    messagebox_count = 0;

    switch (type & 0x0F) {
    case MB_OKCANCEL:
        messagebox_buttons[messagebox_count++] = (mb_button){ "OK", IDOK };
        messagebox_buttons[messagebox_count++] = (mb_button){ "Cancel", IDCANCEL };
        break;
    case MB_YESNO:
        messagebox_buttons[messagebox_count++] = (mb_button){ "Yes", IDYES };
        messagebox_buttons[messagebox_count++] = (mb_button){ "No", IDNO };
        break;
    default:
        messagebox_buttons[messagebox_count++] = (mb_button){ "OK", IDOK };
        break;
    }

    int w = 380, h = 170;
    rect_t client = rect_make((display.back->width - w) / 2,
                              (display.back->height - h) / 2, w, h);
    messagebox_window = wm_create(&manager, messagebox_title, ICON_INFO, client,
                                  messagebox_proc, NULL);
    if (!messagebox_window) return IDOK;

    while (!messagebox_result) {
        pump_once();
        sleep_ms(4);
    }
    wm_close(&manager, messagebox_window);
    messagebox_window = NULL;
    wm_damage_all(&manager);
    return messagebox_result;
}

static int WINAPI w_MessageBoxW(HANDLE owner, const WCHAR *text, const WCHAR *title, UINT type) {
    char t[512], c[128];
    win_wide_to_utf8(text, t, sizeof t);
    win_wide_to_utf8(title, c, sizeof c);
    return w_MessageBoxA(owner, t, c, type);
}

/* --------------------------------------------------------------- the rest */

static HANDLE WINAPI w_LoadCursorA(HANDLE inst, const char *name) { (void)inst; (void)name; return (HANDLE)(uintptr_t)1; }
static HANDLE WINAPI w_LoadIconA(HANDLE inst, const char *name) { (void)inst; (void)name; return (HANDLE)(uintptr_t)1; }
static BOOL   WINAPI w_SetCursor(HANDLE c) { (void)c; return WIN_TRUE; }
static int    WINAPI w_ShowCursor(BOOL show) { (void)show; return 0; }

static int WINAPI w_GetSystemMetrics(int index) {
    switch (index) {
    case 0:  return gui_up ? display.back->width : 1024;    /* SM_CXSCREEN */
    case 1:  return gui_up ? display.back->height : 768;    /* SM_CYSCREEN */
    case 4:  return WIN_TITLE_H;                            /* SM_CYCAPTION */
    case 5:  return WIN_BORDER;                             /* SM_CXBORDER */
    case 6:  return WIN_BORDER;                             /* SM_CYBORDER */
    case 32: return WIN_BORDER;                             /* SM_CXFRAME */
    case 33: return WIN_BORDER;                             /* SM_CYFRAME */
    default: return 0;
    }
}

static BOOL WINAPI w_GetCursorPos(POINT *p) {
    if (!p) return WIN_FALSE;
    p->x = manager.mouse_x;
    p->y = manager.mouse_y;
    return WIN_TRUE;
}
static BOOL WINAPI w_ScreenToClient(HANDLE h, POINT *p) {
    hwnd_rec *w = rec_of(h);
    if (!w || !p) return WIN_FALSE;
    if (w->native) {
        rect_t c = wm_client_rect(w->native);
        p->x -= c.x;
        p->y -= c.y;
    }
    return WIN_TRUE;
}
static BOOL WINAPI w_ClientToScreen(HANDLE h, POINT *p) {
    hwnd_rec *w = rec_of(h);
    if (!w || !p) return WIN_FALSE;
    if (w->native) {
        rect_t c = wm_client_rect(w->native);
        p->x += c.x;
        p->y += c.y;
    }
    return WIN_TRUE;
}

static BOOL WINAPI w_SetRect(RECT *r, int l, int t, int rr, int b) {
    if (!r) return WIN_FALSE;
    r->left = l; r->top = t; r->right = rr; r->bottom = b;
    return WIN_TRUE;
}
static BOOL WINAPI w_PtInRect(const RECT *r, POINT p) {
    return r && p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom;
}
static BOOL WINAPI w_InflateRect(RECT *r, int dx, int dy) {
    if (!r) return WIN_FALSE;
    r->left -= dx; r->right += dx; r->top -= dy; r->bottom += dy;
    return WIN_TRUE;
}

/* ---------------------------------------------------------------- the table */

static const win_export_t user32[] = {
    { "RegisterClassA",       (void *)w_RegisterClassA },
    { "RegisterClassExA",     (void *)w_RegisterClassExA },
    { "UnregisterClassA",     (void *)w_UnregisterClassA },
    { "CreateWindowExA",      (void *)w_CreateWindowExA },
    { "CreateWindowExW",      (void *)w_CreateWindowExW },
    { "DestroyWindow",        (void *)w_DestroyWindow },
    { "ShowWindow",           (void *)w_ShowWindow },
    { "UpdateWindow",         (void *)w_UpdateWindow },
    { "InvalidateRect",       (void *)w_InvalidateRect },
    { "GetClientRect",        (void *)w_GetClientRect },
    { "GetWindowRect",        (void *)w_GetWindowRect },
    { "MoveWindow",           (void *)w_MoveWindow },
    { "SetWindowTextA",       (void *)w_SetWindowTextA },
    { "GetWindowTextA",       (void *)w_GetWindowTextA },
    { "GetWindowTextLengthA", (void *)w_GetWindowTextLengthA },
    { "GetDlgItem",           (void *)w_GetDlgItem },
    { "SetDlgItemTextA",      (void *)w_SetDlgItemTextA },
    { "GetDlgItemTextA",      (void *)w_GetDlgItemTextA },
    { "SetWindowLongPtrA",    (void *)w_SetWindowLongPtrA },
    { "GetWindowLongPtrA",    (void *)w_GetWindowLongPtrA },
    { "SetWindowLongA",       (void *)w_SetWindowLongPtrA },
    { "GetWindowLongA",       (void *)w_GetWindowLongPtrA },
    { "SetFocus",             (void *)w_SetFocus },
    { "GetFocus",             (void *)w_GetFocus },
    { "SetCapture",           (void *)w_SetCapture },
    { "ReleaseCapture",       (void *)w_ReleaseCapture },
    { "EnableWindow",         (void *)w_EnableWindow },

    { "GetMessageA",          (void *)w_GetMessageA },
    { "GetMessageW",          (void *)w_GetMessageA },
    { "PeekMessageA",         (void *)w_PeekMessageA },
    { "PeekMessageW",         (void *)w_PeekMessageA },
    { "TranslateMessage",     (void *)w_TranslateMessage },
    { "DispatchMessageA",     (void *)w_DispatchMessageA },
    { "DispatchMessageW",     (void *)w_DispatchMessageA },
    { "DefWindowProcA",       (void *)w_DefWindowProcA },
    { "DefWindowProcW",       (void *)w_DefWindowProcA },
    { "PostQuitMessage",      (void *)w_PostQuitMessage },
    { "PostMessageA",         (void *)w_PostMessageA },
    { "SendMessageA",         (void *)w_SendMessageA },
    { "SendMessageW",         (void *)w_SendMessageA },

    { "BeginPaint",           (void *)w_BeginPaint },
    { "EndPaint",             (void *)w_EndPaint },
    { "GetDC",                (void *)w_GetDC },
    { "ReleaseDC",            (void *)w_ReleaseDC },

    { "SetTimer",             (void *)w_SetTimer },
    { "KillTimer",            (void *)w_KillTimer },

    { "MessageBoxA",          (void *)w_MessageBoxA },
    { "MessageBoxW",          (void *)w_MessageBoxW },

    { "LoadCursorA",          (void *)w_LoadCursorA },
    { "LoadIconA",            (void *)w_LoadIconA },
    { "SetCursor",            (void *)w_SetCursor },
    { "ShowCursor",           (void *)w_ShowCursor },
    { "GetSystemMetrics",     (void *)w_GetSystemMetrics },
    { "GetCursorPos",         (void *)w_GetCursorPos },
    { "ScreenToClient",       (void *)w_ScreenToClient },
    { "ClientToScreen",       (void *)w_ClientToScreen },
    { "SetRect",              (void *)w_SetRect },
    { "PtInRect",             (void *)w_PtInRect },
    { "InflateRect",          (void *)w_InflateRect },
    { NULL, NULL }
};

void u32_init(void) { win_register("user32.dll", user32); }
