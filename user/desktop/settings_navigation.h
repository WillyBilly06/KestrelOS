/* Responsive navigation geometry shared by Settings paint and hit testing. */
#ifndef KESTREL_SETTINGS_NAVIGATION_H
#define KESTREL_SETTINGS_NAVIGATION_H
typedef struct {
    int sidebar, top, pad;
    rect_t body, status, previous, next, caption;
} settings_navigation_t;

static settings_navigation_t settings_navigation(int width, int height, int scale,
                                                 int font_height, int full_sidebar,
                                                 int page_count) {
    settings_navigation_t n = {0};
    if (width <= 0 || height <= 0 || scale < 1 || scale > 8 || font_height < 1 ||
        font_height > 512 || full_sidebar < 0 || page_count < 1 || page_count > 32)
        return n;
    n.pad = 24 * scale;
    if (n.pad > (width - 1) / 2) n.pad = (width - 1) / 2;
    if (n.pad > (height - 1) / 2) n.pad = (height - 1) / 2;
    int sidebar_height = 36 * scale + font_height + page_count * (font_height * 5 / 3);
    if ((int64_t)width >= (int64_t)full_sidebar + 400 * scale && height >= sidebar_height)
        n.sidebar = full_sidebar;
    else {
        n.top = font_height + 12 * scale;
        if (n.top > height) n.top = height;
        int button = 32 * scale;
        if (button > width / 3) button = width / 3;
        int inset = 4 * scale;
        if (inset > n.top / 3) inset = n.top / 3;
        n.previous = rect_make(0, inset, button, n.top - 2 * inset);
        n.next = rect_make(width - button, inset, button, n.top - 2 * inset);
        n.caption = rect_make(button, inset, width - 2 * button, n.top - 2 * inset);
    }
    int bw = width - n.sidebar - 2 * n.pad;
    int bh = height - n.top - 2 * n.pad;
    int by = n.top + n.pad;
    if (by > height) by = height;
    n.body = rect_make(n.sidebar + n.pad, by, bw > 0 ? bw : 0, bh > 0 ? bh : 0);
    /* Reserve a stable status footer. A message must not cover or move the
     * page controls that generated it, even while text scale is changing. */
    int footer = font_height + 14 * scale;
    if (footer > n.body.h) footer = n.body.h;
    n.status = rect_make(n.body.x,n.body.y+n.body.h-footer,n.body.w,footer);
    int gap = 8 * scale;
    if (gap > n.body.h-footer) gap = n.body.h-footer;
    n.body.h -= footer + gap;
    return n;
}
#endif
