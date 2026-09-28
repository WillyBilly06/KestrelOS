/* gdi.c - gdi32: device contexts and drawing.
 *
 * Windows drawing is done through a device context that holds the current pen,
 * brush, font and colours; the drawing calls themselves take almost no
 * arguments because the context already knows everything.  That model maps
 * cleanly onto the surfaces this system draws into - a context here is a
 * surface, an origin inside it, and the current state.
 *
 * One thing to watch: a Windows colour is 0x00BBGGRR and this system's is
 * 0xRRGGBB, so every colour crossing the boundary is turned around.
 */
#include "win.h"
#include "gui.h"
#include "window.h"

typedef DWORD COLORREF;

surface_t *u32_surface_for(void *hwnd);
void       u32_client_origin(void *hwnd, int *x, int *y);
void       u32_invalidate(void *hwnd);

/* Windows names its colours the other way round. */
static colour_t from_colorref(COLORREF c) {
    return RGB((c & 0xFF), ((c >> 8) & 0xFF), ((c >> 16) & 0xFF));
}
static COLORREF to_colorref(colour_t c) {
    return (COLORREF)((RGB_B(c) << 16) | (RGB_G(c) << 8) | RGB_R(c));
}

/* ------------------------------------------------------------ the objects */

typedef enum { GO_FREE = 0, GO_PEN, GO_BRUSH, GO_FONT, GO_BITMAP, GO_REGION } gokind;

typedef struct {
    gokind    kind;
    colour_t  colour;
    int       width;          /* a pen's */
    int       style;          /* PS_/BS_ */
    /* a font's */
    int       height;
    bool      bold, mono;
    /* a bitmap's */
    surface_t *surface;
} gdiobj;

#define MAX_GDI_OBJECTS 128
static gdiobj objects[MAX_GDI_OBJECTS];

#define GDI_BASE 0x3000

static gdiobj *obj_from(HANDLE h) {
    uintptr_t v = (uintptr_t)h;
    if (v < GDI_BASE || v >= GDI_BASE + MAX_GDI_OBJECTS) return NULL;
    gdiobj *o = &objects[v - GDI_BASE];
    return o->kind == GO_FREE ? NULL : o;
}

static HANDLE obj_new(gokind kind, gdiobj **out) {
    for (int i = 0; i < MAX_GDI_OBJECTS; i++) {
        if (objects[i].kind != GO_FREE) continue;
        memset(&objects[i], 0, sizeof objects[i]);
        objects[i].kind = kind;
        if (out) *out = &objects[i];
        return (HANDLE)(uintptr_t)(GDI_BASE + i);
    }
    if (out) *out = NULL;
    return NULL;
}

/* -------------------------------------------------------------- the contexts */

typedef struct {
    bool      used;
    surface_t *target;
    int       ox, oy;              /* where this context's origin sits */
    void     *hwnd;                /* set when the target is a window */
    bool      owns_surface;        /* a memory context made its own */

    colour_t  text_colour, back_colour;
    int       back_mode;           /* 1 transparent, 2 opaque */
    gdiobj   *pen, *brush, *font;
    int       x, y;                /* the current position */
} dc_t;

#define MAX_DCS 16
static dc_t dcs[MAX_DCS];
#define DC_BASE 0x4000

static dc_t *dc_from(HANDLE h) {
    uintptr_t v = (uintptr_t)h;
    if (v < DC_BASE || v >= DC_BASE + MAX_DCS) return NULL;
    dc_t *d = &dcs[v - DC_BASE];
    return d->used ? d : NULL;
}

static HANDLE dc_new(dc_t **out) {
    for (int i = 0; i < MAX_DCS; i++) {
        if (dcs[i].used) continue;
        memset(&dcs[i], 0, sizeof dcs[i]);
        dcs[i].used = true;
        dcs[i].text_colour = g_theme.text;
        dcs[i].back_colour = g_theme.window;
        dcs[i].back_mode = 2;
        if (out) *out = &dcs[i];
        return (HANDLE)(uintptr_t)(DC_BASE + i);
    }
    if (out) *out = NULL;
    return NULL;
}

/* Called from user32 when a window is about to be painted. */
HANDLE gdi_dc_for_window(void *hwnd) {
    dc_t *d;
    HANDLE h = dc_new(&d);
    if (!h) return NULL;
    d->target = u32_surface_for(hwnd);
    u32_client_origin(hwnd, &d->ox, &d->oy);
    d->hwnd = hwnd;
    return h;
}

void gdi_release_dc(HANDLE h) {
    dc_t *d = dc_from(h);
    if (!d) return;
    if (d->owns_surface && d->target) surface_destroy(d->target);
    d->used = false;
}

/* ---------------------------------------------------------------- creation */

#define PS_SOLID 0
#define PS_NULL  5
#define BS_SOLID 0
#define BS_NULL  1

static HANDLE WINAPI w_CreateSolidBrush(COLORREF c) {
    gdiobj *o;
    HANDLE h = obj_new(GO_BRUSH, &o);
    if (o) { o->colour = from_colorref(c); o->style = BS_SOLID; }
    return h;
}

static HANDLE WINAPI w_CreatePen(int style, int width, COLORREF c) {
    gdiobj *o;
    HANDLE h = obj_new(GO_PEN, &o);
    if (o) { o->colour = from_colorref(c); o->width = width < 1 ? 1 : width; o->style = style; }
    return h;
}

static HANDLE WINAPI w_CreateFontA(int height, int width, int escape, int orient, int weight,
                                   DWORD italic, DWORD underline, DWORD strike, DWORD charset,
                                   DWORD precision, DWORD clip, DWORD quality, DWORD pitch,
                                   const char *face) {
    (void)width; (void)escape; (void)orient; (void)italic; (void)underline;
    (void)strike; (void)charset; (void)precision; (void)clip; (void)quality;
    gdiobj *o;
    HANDLE h = obj_new(GO_FONT, &o);
    if (o) {
        o->height = height < 0 ? -height : height;
        o->bold = weight >= 700;
        /* Only two faces exist here, so anything fixed-pitch or named after a
         * fixed-pitch family gets the monospaced one. */
        o->mono = (pitch & 3) == 1 ||
                  (face && (!strcasecmp(face, "Courier New") || !strcasecmp(face, "Consolas") ||
                            !strcasecmp(face, "Terminal") || !strcasecmp(face, "Fixedsys")));
    }
    return h;
}

static BOOL WINAPI w_DeleteObject(HANDLE h) {
    gdiobj *o = obj_from(h);
    if (!o) return WIN_FALSE;
    if (o->kind == GO_BITMAP && o->surface) surface_destroy(o->surface);
    o->kind = GO_FREE;
    return WIN_TRUE;
}

/* The stock objects, which a program uses without ever creating them. */
#define WHITE_BRUSH   0
#define LTGRAY_BRUSH  1
#define GRAY_BRUSH    2
#define DKGRAY_BRUSH  3
#define BLACK_BRUSH   4
#define NULL_BRUSH    5
#define WHITE_PEN     6
#define BLACK_PEN     7
#define NULL_PEN      8
#define SYSTEM_FONT   13
#define DEFAULT_GUI_FONT 17

static HANDLE WINAPI w_GetStockObject(int which) {
    static HANDLE cache[24];
    if (which < 0 || which >= 24) return NULL;
    if (cache[which]) return cache[which];

    gdiobj *o = NULL;
    HANDLE h = NULL;
    switch (which) {
    case WHITE_BRUSH:  h = obj_new(GO_BRUSH, &o); if (o) o->colour = RGB(255, 255, 255); break;
    case LTGRAY_BRUSH: h = obj_new(GO_BRUSH, &o); if (o) o->colour = RGB(192, 192, 192); break;
    case GRAY_BRUSH:   h = obj_new(GO_BRUSH, &o); if (o) o->colour = RGB(128, 128, 128); break;
    case DKGRAY_BRUSH: h = obj_new(GO_BRUSH, &o); if (o) o->colour = RGB(64, 64, 64); break;
    case BLACK_BRUSH:  h = obj_new(GO_BRUSH, &o); if (o) o->colour = RGB(0, 0, 0); break;
    case NULL_BRUSH:   h = obj_new(GO_BRUSH, &o); if (o) o->style = BS_NULL; break;
    case WHITE_PEN:    h = obj_new(GO_PEN, &o); if (o) { o->colour = RGB(255, 255, 255); o->width = 1; } break;
    case BLACK_PEN:    h = obj_new(GO_PEN, &o); if (o) { o->colour = RGB(0, 0, 0); o->width = 1; } break;
    case NULL_PEN:     h = obj_new(GO_PEN, &o); if (o) { o->style = PS_NULL; o->width = 1; } break;
    default:           h = obj_new(GO_FONT, &o); if (o) o->height = gui_font_height(FONT_UI); break;
    }
    cache[which] = h;
    return h;
}

static HANDLE WINAPI w_SelectObject(HANDLE hdc, HANDLE obj) {
    dc_t *d = dc_from(hdc);
    gdiobj *o = obj_from(obj);
    if (!d || !o) return NULL;

    gdiobj *old = NULL;
    switch (o->kind) {
    case GO_PEN:   old = d->pen; d->pen = o; break;
    case GO_BRUSH: old = d->brush; d->brush = o; break;
    case GO_FONT:  old = d->font; d->font = o; break;
    case GO_BITMAP:
        /* Selecting a bitmap into a memory context is how a program chooses
         * where its drawing lands. */
        if (o->surface) { d->target = o->surface; d->ox = d->oy = 0; }
        break;
    default: break;
    }
    if (!old) return NULL;
    return (HANDLE)(uintptr_t)(GDI_BASE + (old - objects));
}

/* --------------------------------------------------------------- the state */

static COLORREF WINAPI w_SetTextColor(HANDLE hdc, COLORREF c) {
    dc_t *d = dc_from(hdc);
    if (!d) return 0;
    COLORREF old = to_colorref(d->text_colour);
    d->text_colour = from_colorref(c);
    return old;
}
static COLORREF WINAPI w_SetBkColor(HANDLE hdc, COLORREF c) {
    dc_t *d = dc_from(hdc);
    if (!d) return 0;
    COLORREF old = to_colorref(d->back_colour);
    d->back_colour = from_colorref(c);
    return old;
}
static int WINAPI w_SetBkMode(HANDLE hdc, int mode) {
    dc_t *d = dc_from(hdc);
    if (!d) return 0;
    int old = d->back_mode;
    d->back_mode = mode;
    return old;
}
static int WINAPI w_SetROP2(HANDLE hdc, int mode) { (void)hdc; (void)mode; return 13 /* R2_COPYPEN */; }

/* ------------------------------------------------------------------ drawing */

static font_id font_of(dc_t *d) {
    return (d->font && d->font->mono) ? FONT_MONO : FONT_UI;
}

static BOOL WINAPI w_TextOutA(HANDLE hdc, int x, int y, const char *text, int len) {
    dc_t *d = dc_from(hdc);
    if (!d || !d->target || !text) return WIN_FALSE;
    char buf[512];
    int n = len < 0 ? (int)strlen(text) : len;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    memcpy(buf, text, (size_t)n);
    buf[n] = 0;

    font_id f = font_of(d);
    if (d->back_mode == 2) {
        rect_t r = rect_make(d->ox + x, d->oy + y, gui_text_width(f, buf), gui_font_height(f));
        gui_fill(d->target, r, d->back_colour);
    }
    gui_text(d->target, f, d->ox + x, d->oy + y, buf, d->text_colour);
    return WIN_TRUE;
}

static BOOL WINAPI w_TextOutW(HANDLE hdc, int x, int y, const WCHAR *text, int len) {
    char buf[512];
    int n = 0;
    if (text) for (; text[n] && n < (int)sizeof buf - 1 && (len < 0 || n < len); n++)
        buf[n] = (char)(text[n] < 0x80 ? text[n] : '?');
    buf[n] = 0;
    return w_TextOutA(hdc, x, y, buf, n);
}

static BOOL WINAPI w_ExtTextOutA(HANDLE hdc, int x, int y, UINT options, const RECT *clip,
                                 const char *text, UINT len, const INT *spacing) {
    (void)options; (void)clip; (void)spacing;
    return w_TextOutA(hdc, x, y, text, (int)len);
}

#define DT_CENTER   0x0001
#define DT_RIGHT    0x0002
#define DT_VCENTER  0x0004
#define DT_SINGLELINE 0x0020
#define DT_WORDBREAK 0x0010

static int WINAPI w_DrawTextA(HANDLE hdc, const char *text, int len, RECT *r, UINT format) {
    dc_t *d = dc_from(hdc);
    if (!d || !d->target || !text || !r) return 0;
    char buf[1024];
    int n = len < 0 ? (int)strlen(text) : len;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    memcpy(buf, text, (size_t)n);
    buf[n] = 0;

    font_id f = font_of(d);
    rect_t box = rect_make(d->ox + r->left, d->oy + r->top,
                           r->right - r->left, r->bottom - r->top);

    if (format & DT_WORDBREAK) return gui_text_wrapped(d->target, f, box, buf, d->text_colour);

    int th = gui_font_height(f);
    int y = (format & DT_VCENTER) ? box.y + (box.h - th) / 2 : box.y;
    if (format & DT_CENTER) {
        gui_text_centred(d->target, f, rect_make(box.x, y, box.w, th), buf, d->text_colour);
    } else if (format & DT_RIGHT) {
        gui_text_right(d->target, f, rect_make(box.x, y, box.w, th), buf, d->text_colour);
    } else {
        gui_text(d->target, f, box.x, y, buf, d->text_colour);
    }
    return th;
}

static BOOL WINAPI w_GetTextExtentPoint32A(HANDLE hdc, const char *text, int len, SIZE *out) {
    dc_t *d = dc_from(hdc);
    if (!d || !text || !out) return WIN_FALSE;
    font_id f = font_of(d);
    out->cx = gui_text_width_n(f, text, len < 0 ? strlen(text) : (size_t)len);
    out->cy = gui_font_height(f);
    return WIN_TRUE;
}

typedef struct {
    LONG tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading;
    LONG tmAveCharWidth, tmMaxCharWidth, tmWeight, tmOverhang;
    LONG tmDigitizedAspectX, tmDigitizedAspectY;
    BYTE tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    BYTE tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet;
} TEXTMETRICA;

static BOOL WINAPI w_GetTextMetricsA(HANDLE hdc, TEXTMETRICA *tm) {
    dc_t *d = dc_from(hdc);
    if (!d || !tm) return WIN_FALSE;
    font_id f = font_of(d);
    memset(tm, 0, sizeof *tm);
    tm->tmHeight = gui_font_height(f);
    tm->tmAscent = tm->tmHeight - 3;
    tm->tmDescent = 3;
    tm->tmAveCharWidth = gui_font_advance(f, 'x');
    tm->tmMaxCharWidth = gui_font_advance(f, 'W');
    tm->tmWeight = d->font && d->font->bold ? 700 : 400;
    tm->tmFirstChar = 32;
    tm->tmLastChar = 126;
    return WIN_TRUE;
}

static BOOL WINAPI w_MoveToEx(HANDLE hdc, int x, int y, POINT *old) {
    dc_t *d = dc_from(hdc);
    if (!d) return WIN_FALSE;
    if (old) { old->x = d->x; old->y = d->y; }
    d->x = x;
    d->y = y;
    return WIN_TRUE;
}

static BOOL WINAPI w_LineTo(HANDLE hdc, int x, int y) {
    dc_t *d = dc_from(hdc);
    if (!d || !d->target) return WIN_FALSE;
    if (!d->pen || d->pen->style != PS_NULL) {
        colour_t c = d->pen ? d->pen->colour : RGB(0, 0, 0);
        int w = d->pen ? d->pen->width : 1;
        for (int i = 0; i < w; i++)
            gui_line(d->target, d->ox + d->x, d->oy + d->y + i, d->ox + x, d->oy + y + i, c);
    }
    d->x = x;
    d->y = y;
    return WIN_TRUE;
}

static BOOL WINAPI w_Polyline(HANDLE hdc, const POINT *points, int count) {
    if (!points || count < 2) return WIN_FALSE;
    w_MoveToEx(hdc, points[0].x, points[0].y, NULL);
    for (int i = 1; i < count; i++) w_LineTo(hdc, points[i].x, points[i].y);
    return WIN_TRUE;
}

static BOOL WINAPI w_Rectangle(HANDLE hdc, int l, int t, int r, int b) {
    dc_t *d = dc_from(hdc);
    if (!d || !d->target) return WIN_FALSE;
    rect_t box = rect_make(d->ox + l, d->oy + t, r - l, b - t);
    if (d->brush && d->brush->style != BS_NULL) gui_fill(d->target, box, d->brush->colour);
    if (!d->pen || d->pen->style != PS_NULL)
        gui_frame_thick(d->target, box, d->pen ? d->pen->width : 1,
                        d->pen ? d->pen->colour : RGB(0, 0, 0));
    return WIN_TRUE;
}

static BOOL WINAPI w_RoundRect(HANDLE hdc, int l, int t, int r, int b, int rw, int rh) {
    dc_t *d = dc_from(hdc);
    if (!d || !d->target) return WIN_FALSE;
    rect_t box = rect_make(d->ox + l, d->oy + t, r - l, b - t);
    int radius = (rw + rh) / 4;
    if (d->brush && d->brush->style != BS_NULL) gui_round_rect(d->target, box, radius, d->brush->colour);
    if (!d->pen || d->pen->style != PS_NULL)
        gui_round_frame(d->target, box, radius, d->pen ? d->pen->colour : RGB(0, 0, 0));
    return WIN_TRUE;
}

/* An ellipse, drawn by the midpoint method: for each row the horizontal extent
 * comes from the ellipse equation, which is exact enough at these sizes and
 * needs no floating point. */
static BOOL WINAPI w_Ellipse(HANDLE hdc, int l, int t, int r, int b) {
    dc_t *d = dc_from(hdc);
    if (!d || !d->target) return WIN_FALSE;
    int cx = (l + r) / 2, cy = (t + b) / 2;
    int a = (r - l) / 2, c = (b - t) / 2;
    if (a <= 0 || c <= 0) return WIN_FALSE;

    colour_t fill = d->brush ? d->brush->colour : RGB(255, 255, 255);
    colour_t edge = d->pen ? d->pen->colour : RGB(0, 0, 0);
    bool do_fill = d->brush && d->brush->style != BS_NULL;
    bool do_edge = !d->pen || d->pen->style != PS_NULL;

    for (int y = -c; y <= c; y++) {
        /* x = a * sqrt(1 - y^2/c^2), computed in integers by squaring. */
        int64_t inside = (int64_t)a * a * ((int64_t)c * c - (int64_t)y * y);
        int x = 0;
        while ((int64_t)(x + 1) * (x + 1) * c * c <= inside) x++;
        if (do_fill && x > 0)
            gui_hline(d->target, d->ox + cx - x, d->oy + cy + y, 2 * x, fill);
        if (do_edge) {
            gui_pixel(d->target, d->ox + cx - x, d->oy + cy + y, edge);
            gui_pixel(d->target, d->ox + cx + x, d->oy + cy + y, edge);
        }
    }
    return WIN_TRUE;
}

static int WINAPI w_FillRect(HANDLE hdc, const RECT *r, HANDLE brush) {
    dc_t *d = dc_from(hdc);
    gdiobj *b = obj_from(brush);
    if (!d || !d->target || !r) return 0;
    colour_t c = b ? b->colour : d->back_colour;
    if (b && b->style == BS_NULL) return 1;
    gui_fill(d->target, rect_make(d->ox + r->left, d->oy + r->top,
                                  r->right - r->left, r->bottom - r->top), c);
    return 1;
}

static int WINAPI w_FrameRect(HANDLE hdc, const RECT *r, HANDLE brush) {
    dc_t *d = dc_from(hdc);
    gdiobj *b = obj_from(brush);
    if (!d || !d->target || !r) return 0;
    gui_frame(d->target, rect_make(d->ox + r->left, d->oy + r->top,
                                   r->right - r->left, r->bottom - r->top),
              b ? b->colour : d->text_colour);
    return 1;
}

static COLORREF WINAPI w_SetPixel(HANDLE hdc, int x, int y, COLORREF c) {
    dc_t *d = dc_from(hdc);
    if (!d || !d->target) return (COLORREF)-1;
    gui_pixel(d->target, d->ox + x, d->oy + y, from_colorref(c));
    return c;
}

static COLORREF WINAPI w_GetPixel(HANDLE hdc, int x, int y) {
    dc_t *d = dc_from(hdc);
    if (!d || !d->target) return (COLORREF)-1;
    int px = d->ox + x, py = d->oy + y;
    if (px < 0 || py < 0 || px >= d->target->width || py >= d->target->height) return (COLORREF)-1;
    return to_colorref(d->target->pixels[py * d->target->stride + px]);
}

/* ------------------------------------------------------------ memory contexts */

static HANDLE WINAPI w_CreateCompatibleDC(HANDLE hdc) {
    (void)hdc;
    dc_t *d;
    HANDLE h = dc_new(&d);
    if (d) { d->target = NULL; d->owns_surface = false; }
    return h;
}

static HANDLE WINAPI w_CreateCompatibleBitmap(HANDLE hdc, int w, int h) {
    (void)hdc;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { win_set_error(ERROR_INVALID_PARAMETER); return NULL; }
    gdiobj *o;
    HANDLE handle = obj_new(GO_BITMAP, &o);
    if (!o) return NULL;
    o->surface = surface_create(w, h);
    if (!o->surface) { o->kind = GO_FREE; return NULL; }
    gui_clear(o->surface, RGB(0, 0, 0));
    return handle;
}

static BOOL WINAPI w_DeleteDC(HANDLE h) { gdi_release_dc(h); return WIN_TRUE; }

#define SRCCOPY 0x00CC0020

static BOOL WINAPI w_BitBlt(HANDLE dst_dc, int x, int y, int w, int h,
                            HANDLE src_dc, int sx, int sy, DWORD rop) {
    dc_t *dd = dc_from(dst_dc), *sd = dc_from(src_dc);
    if (!dd || !dd->target) return WIN_FALSE;
    if (rop != SRCCOPY && sd) win_trace("BitBlt was asked for raster operation %08x", rop);
    if (!sd || !sd->target) {
        /* No source: the usual meaning is a solid fill from the brush. */
        gui_fill(dd->target, rect_make(dd->ox + x, dd->oy + y, w, h),
                 dd->brush ? dd->brush->colour : RGB(0, 0, 0));
        return WIN_TRUE;
    }
    gui_blit_rect(dd->target, sd->target, rect_make(sd->ox + sx, sd->oy + sy, w, h),
                  dd->ox + x, dd->oy + y);
    return WIN_TRUE;
}

static BOOL WINAPI w_StretchBlt(HANDLE dst_dc, int x, int y, int w, int h,
                                HANDLE src_dc, int sx, int sy, int sw, int sh, DWORD rop) {
    dc_t *dd = dc_from(dst_dc), *sd = dc_from(src_dc);
    (void)rop;
    if (!dd || !dd->target || !sd || !sd->target || sw <= 0 || sh <= 0) return WIN_FALSE;
    for (int row = 0; row < h; row++) {
        int srow = sy + row * sh / h;
        for (int col = 0; col < w; col++) {
            int scol = sx + col * sw / w;
            if (scol < 0 || srow < 0 || scol >= sd->target->width || srow >= sd->target->height) continue;
            gui_pixel(dd->target, dd->ox + x + col, dd->oy + y + row,
                      sd->target->pixels[srow * sd->target->stride + scol]);
        }
    }
    return WIN_TRUE;
}

/* A device-independent bitmap: the program hands over raw pixels and says how
 * they are laid out.  Only the twenty-four and thirty-two bit forms are
 * handled, which is what anything written this century produces. */
typedef struct {
    DWORD biSize;
    LONG  biWidth, biHeight;
    WORD  biPlanes, biBitCount;
    DWORD biCompression, biSizeImage;
    LONG  biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;

static int WINAPI w_StretchDIBits(HANDLE hdc, int x, int y, int w, int h,
                                  int sx, int sy, int sw, int sh,
                                  const void *bits, const BITMAPINFOHEADER *info,
                                  UINT usage, DWORD rop) {
    (void)usage; (void)rop;
    dc_t *d = dc_from(hdc);
    if (!d || !d->target || !bits || !info) return 0;
    if (info->biBitCount != 24 && info->biBitCount != 32) {
        win_trace("a %u-bit bitmap was handed over, which is not supported", info->biBitCount);
        return 0;
    }

    int src_w = info->biWidth;
    int src_h = info->biHeight < 0 ? -info->biHeight : info->biHeight;
    bool bottom_up = info->biHeight > 0;         /* the usual, and the confusing one */
    int bpp = info->biBitCount / 8;
    int stride = (src_w * bpp + 3) & ~3;         /* rows are padded to four bytes */
    const uint8_t *p = bits;

    if (sw <= 0) sw = src_w;
    if (sh <= 0) sh = src_h;

    for (int row = 0; row < h; row++) {
        int srow = sy + (h > 1 ? row * sh / h : 0);
        if (srow < 0 || srow >= src_h) continue;
        int file_row = bottom_up ? (src_h - 1 - srow) : srow;
        const uint8_t *line = p + (size_t)file_row * stride;
        for (int col = 0; col < w; col++) {
            int scol = sx + (w > 1 ? col * sw / w : 0);
            if (scol < 0 || scol >= src_w) continue;
            const uint8_t *px = line + (size_t)scol * bpp;
            gui_pixel(d->target, d->ox + x + col, d->oy + y + row, RGB(px[2], px[1], px[0]));
        }
    }
    return h;
}

static int WINAPI w_SetDIBitsToDevice(HANDLE hdc, int x, int y, DWORD w, DWORD h,
                                      int sx, int sy, UINT first, UINT lines,
                                      const void *bits, const BITMAPINFOHEADER *info, UINT usage) {
    (void)first; (void)lines;
    return w_StretchDIBits(hdc, x, y, (int)w, (int)h, sx, sy, (int)w, (int)h, bits, info, usage, SRCCOPY);
}

static int WINAPI w_SaveDC(HANDLE hdc) { (void)hdc; return 1; }
static BOOL WINAPI w_RestoreDC(HANDLE hdc, int state) { (void)hdc; (void)state; return WIN_TRUE; }

static int WINAPI w_GetDeviceCaps(HANDLE hdc, int index) {
    dc_t *d = dc_from(hdc);
    switch (index) {
    case 8:  return d && d->target ? d->target->width : 1024;   /* HORZRES */
    case 10: return d && d->target ? d->target->height : 768;   /* VERTRES */
    case 12: return 32;                                          /* BITSPIXEL */
    case 14: return 1;                                           /* PLANES */
    case 88: case 90: return 96;                                 /* LOGPIXELSX/Y */
    default: return 0;
    }
}

/* ---------------------------------------------------------------- the table */

static const win_export_t gdi32[] = {
    { "CreateSolidBrush",       (void *)w_CreateSolidBrush },
    { "CreatePen",              (void *)w_CreatePen },
    { "CreateFontA",            (void *)w_CreateFontA },
    { "DeleteObject",           (void *)w_DeleteObject },
    { "GetStockObject",         (void *)w_GetStockObject },
    { "SelectObject",           (void *)w_SelectObject },

    { "SetTextColor",           (void *)w_SetTextColor },
    { "SetBkColor",             (void *)w_SetBkColor },
    { "SetBkMode",              (void *)w_SetBkMode },
    { "SetROP2",                (void *)w_SetROP2 },

    { "TextOutA",               (void *)w_TextOutA },
    { "TextOutW",               (void *)w_TextOutW },
    { "ExtTextOutA",            (void *)w_ExtTextOutA },
    { "DrawTextA",              (void *)w_DrawTextA },
    { "GetTextExtentPoint32A",  (void *)w_GetTextExtentPoint32A },
    { "GetTextMetricsA",        (void *)w_GetTextMetricsA },

    { "MoveToEx",               (void *)w_MoveToEx },
    { "LineTo",                 (void *)w_LineTo },
    { "Polyline",               (void *)w_Polyline },
    { "Rectangle",              (void *)w_Rectangle },
    { "RoundRect",              (void *)w_RoundRect },
    { "Ellipse",                (void *)w_Ellipse },
    { "FillRect",               (void *)w_FillRect },
    { "FrameRect",              (void *)w_FrameRect },
    { "SetPixel",               (void *)w_SetPixel },
    { "GetPixel",               (void *)w_GetPixel },

    { "CreateCompatibleDC",     (void *)w_CreateCompatibleDC },
    { "CreateCompatibleBitmap", (void *)w_CreateCompatibleBitmap },
    { "DeleteDC",               (void *)w_DeleteDC },
    { "BitBlt",                 (void *)w_BitBlt },
    { "StretchBlt",             (void *)w_StretchBlt },
    { "StretchDIBits",          (void *)w_StretchDIBits },
    { "SetDIBitsToDevice",      (void *)w_SetDIBitsToDevice },
    { "SaveDC",                 (void *)w_SaveDC },
    { "RestoreDC",              (void *)w_RestoreDC },
    { "GetDeviceCaps",          (void *)w_GetDeviceCaps },
    { NULL, NULL }
};

void gdi_init(void) { win_register("gdi32.dll", gdi32); }
