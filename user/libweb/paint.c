/* paint.c - drawing the boxes.
 *
 * By the time anything gets here every question has been answered: what the
 * page says, what it should look like, and where each piece goes.  All that is
 * left is to put it on the screen, in the right order - backgrounds before the
 * things that sit on them, and nothing outside the part of the page that is
 * actually visible.
 *
 * The one thing worth care is doing as little as possible.  A page is often
 * several times taller than the window, and drawing all of it to show a
 * fraction is the difference between scrolling that feels immediate and
 * scrolling that does not.  Every box is checked against the visible band
 * first, and a box entirely above or below it costs nothing.
 */
#include "paint.h"

static void paint_box(surface_t *s, const box_t *box, int origin_x, int origin_y,
                      int top, int bottom, const paint_state_t *state);

static void draw_border_side(surface_t *s, rect_t r, int side, int width,
                             colour_t colour, int style) {
    if (width <= 0 || style == BORDER_NONE) return;

    rect_t edge = r;
    switch (side) {
    case SIDE_TOP:    edge.h = width; break;
    case SIDE_BOTTOM: edge.y = r.y + r.h - width; edge.h = width; break;
    case SIDE_LEFT:   edge.w = width; break;
    default:          edge.x = r.x + r.w - width; edge.w = width; break;
    }

    if (style == BORDER_SOLID) {
        gui_fill(s, edge, colour);
        return;
    }

    /* Dashes and dots are drawn as they are described rather than
     * approximated by a solid line: an author who asked for a dotted rule
     * meant something lighter than a solid one. */
    int step = (style == BORDER_DOTTED) ? 2 : 6;
    int on = (style == BORDER_DOTTED) ? 1 : 3;
    if (side == SIDE_TOP || side == SIDE_BOTTOM) {
        for (int x = edge.x; x < edge.x + edge.w; x += step)
            gui_fill(s, rect_make(x, edge.y, on, edge.h), colour);
    } else {
        for (int y = edge.y; y < edge.y + edge.h; y += step)
            gui_fill(s, rect_make(edge.x, y, edge.w, on), colour);
    }
}

static void paint_bullet(surface_t *s, const box_t *box, int x, int y) {
    const style_t *style = box->style;
    int baseline = y + box->baseline;

    if (style->list_style == LIST_DECIMAL) {
        char label[16];
        snprintf(label, sizeof label, "%d.", box->bullet_number);
        int width = aa_text_width(&AA_BODY, style->font_size, label);
        aa_text(s, &AA_BODY, style->font_size, x + box->width - width - 4,
                baseline, label, style->colour);
        return;
    }

    int size = style->font_size / 4;
    if (size < 3) size = 3;
    int centre_x = x + box->width - size - 4;
    int centre_y = baseline - style->font_size / 3;

    if (style->list_style == LIST_SQUARE) {
        gui_fill(s, rect_make(centre_x, centre_y, size, size), style->colour);
    } else if (style->list_style == LIST_CIRCLE) {
        gui_round_frame_aa(s, rect_make(centre_x, centre_y, size + 1, size + 1),
                           (size + 1) / 2, style->colour);
    } else {
        gui_round_rect_aa(s, rect_make(centre_x, centre_y, size + 1, size + 1),
                          (size + 1) / 2, style->colour);
    }
}

static void paint_text(surface_t *s, const box_t *box, int x, int y,
                       const paint_state_t *state) {
    const style_t *style = box->style;
    if (!box->text || box->text_len <= 0) return;

    colour_t colour = style->colour;

    /* A link the pointer is over is drawn differently, which is the only
     * feedback there is that it can be clicked. */
    bool hovered = false;
    if (style->is_link && state->hovered_node) {
        for (const dom_node_t *up = box->node; up; up = up->parent)
            if (up == state->hovered_node) { hovered = true; break; }
    }
    if (hovered) colour = state->link_hover_colour;

    const aafont_t *font = layout_font_for(style);
    int baseline = y + box->baseline;

    aa_text_n(s, font, style->font_size, x, baseline,
              box->text, (size_t)box->text_len, colour);

    if (style->underline || hovered) {
        int under = baseline + 2;
        gui_fill(s, rect_make(x, under, box->width, 1), colour);
    }
    if (style->strike) {
        int through = baseline - style->font_size / 3;
        gui_fill(s, rect_make(x, through, box->width, 1), colour);
    }
}

#include "image.h"

/* Draw a decoded picture into the box, scaled to fit (nearest sampling - a
 * page is not a photo editor, and this keeps it cheap).  Clipped to the
 * surface and to the box. */
static void blit_image(surface_t *s, const web_image_t *img,
                       int x, int y, int w, int h) {
    rect_t clip = surface_clip(s);
    for (int dy = 0; dy < h; dy++) {
        int py = y + dy;
        if (py < clip.y || py >= clip.y + clip.h || py < 0 || py >= s->height) continue;
        int sy = img->height > 1 ? dy * img->height / h : 0;
        if (sy >= img->height) sy = img->height - 1;
        const unsigned char *row = img->rgb + (size_t)sy * img->width * 3;
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            if (px < clip.x || px >= clip.x + clip.w || px < 0 || px >= s->width) continue;
            int sx = img->width > 1 ? dx * img->width / w : 0;
            if (sx >= img->width) sx = img->width - 1;
            const unsigned char *p = row + (size_t)sx * 3;
            gui_pixel(s,px,py,RGB(p[0],p[1],p[2]));
        }
    }
}

static void paint_image(surface_t *s, const box_t *box, int x, int y) {
    const style_t *style = box->style;

    /* If the picture has been fetched and decoded, draw it. */
    if (box->image && box->width > 0 && box->height > 0) {
        blit_image(s, (const web_image_t *)box->image, x, y,
                   box->width, box->height);
        return;
    }

    /* Otherwise a frame with whatever the markup said the picture was of - a
     * reader gets the information, and a broken-picture frame says there is
     * one here that could not be read. */
    rect_t r = rect_make(x, y, box->width, box->height);
    gui_round_rect_aa(s, r, 3, colour_mix(g_theme.window, g_theme.text, 12));
    gui_round_frame_aa(s, r, 3, colour_mix(g_theme.window, g_theme.text, 40));

    if (box->text && box->text_len > 0) {
        int size = style->font_size > 13 ? 13 : style->font_size;
        int baseline = y + box->height / 2 + size / 3;
        aa_text_clipped(s, &AA_BODY, size, x + 6, baseline, box->width - 12,
                        box->text, colour_mix(g_theme.window, g_theme.text, 150));
    } else {
        gui_icon(s, ICON_FILE, x + box->width / 2 - 6, y + box->height / 2 - 6,
                 12, colour_mix(g_theme.window, g_theme.text, 110));
    }
}

static void paint_control(surface_t *s, const box_t *box, int x, int y) {
    const style_t *style = box->style;
    rect_t r = rect_make(x, y, box->width, box->height);

    /* A checkbox or radio: a small box or circle, filled when checked.  Checked
     * state is form_value ("on"/"") once the user has clicked it, otherwise the
     * initial `checked` attribute. */
    if (box->node && layout_node_is_checkable(box->node)) {
        const char *t = dom_attr(box->node, "type");
        bool radio = t && !strcmp(t, "radio");
        bool checked = box->node->form_value
                     ? !strcmp(box->node->form_value, "on")
                     : (dom_attr(box->node, "checked") != NULL);
        int side = box->height < box->width ? box->height : box->width;
        if (side < 12) side = 12;
        rect_t g = rect_make(x, y + (box->height - side) / 2, side, side);
        int rad = radio ? side / 2 : 3;
        gui_round_rect_aa(s, g, rad, g_theme.field);
        gui_round_frame_aa(s, g, rad, g_theme.field_border);
        if (checked) {
            rect_t inner = rect_make(g.x + side / 4, g.y + side / 4,
                                     side - side / 2, side - side / 2);
            gui_round_rect_aa(s, inner, radio ? (side / 4) : 2, g_theme.accent);
        }
        return;
    }

    bool is_button = box->node && (box->node->tag == TAG_BUTTON ||
                                   (box->node->tag == TAG_INPUT &&
                                    (!dom_attr(box->node, "type") ||
                                     !strcmp(dom_attr(box->node, "type"), "submit") ||
                                     !strcmp(dom_attr(box->node, "type"), "button"))));

    if (is_button) {
        gui_round_rect_aa(s, r, 5, g_theme.control);
        gui_round_frame_aa(s, r, 5, g_theme.control_border);
    } else {
        gui_round_rect_aa(s, r, 4, g_theme.field);
        gui_round_frame_aa(s, r, 4, g_theme.field_border);
    }

    /* Show what has been typed if this control has been edited, live - the
     * browser sets node->form_value on each keystroke and repaints, so this is
     * current without waiting for a relayout.  Otherwise the laid-out text
     * (a value attribute or placeholder). */
    const char *shown = box->text;
    int shown_len = box->text_len;
    if (!is_button && box->node && box->node->form_value) {
        shown = box->node->form_value;
        shown_len = (int)strlen(shown);
    }
    if (shown && shown_len > 0) {
        int size = style->font_size;
        int baseline = y + box->height / 2 + size / 3;
        aa_text_clipped(s, &AA_BODY, size, x + 9, baseline, box->width - 18,
                        shown, g_theme.field_text);
    }
}

static void paint_box(surface_t *s, const box_t *box, int origin_x, int origin_y,
                      int top, int bottom, const paint_state_t *state) {
    int x = box->x + origin_x;
    int y = box->y + origin_y;

    /* Entirely above or below what can be seen: neither it nor anything
     * inside it needs drawing.  On a long page this skips almost everything. */
    if (box->y + box->height < top || box->y > bottom) {
        /* A block whose own extent is off screen can still be the parent of
         * something on screen only if its height is wrong, which it is not.
         * Lines and text are always inside their block. */
        return;
    }

    const style_t *style = box->style;

    switch (box->kind) {
    case BOX_BLOCK: {
        rect_t r = rect_make(x, y, box->width, box->height);
        if (style->has_background) {
            if (style->border_radius > 0)
                gui_round_rect_aa(s, r, style->border_radius, style->background);
            else
                gui_fill(s, r, style->background);
        }
        for (int side = 0; side < 4; side++)
            draw_border_side(s, r, side, style->border_width[side],
                             style->border_colour[side], style->border_style[side]);
        break;
    }
    case BOX_LINE:
        break;                          /* lines are only a container */
    case BOX_TEXT:
        paint_text(s, box, x, y, state);
        break;
    case BOX_IMAGE:
        paint_image(s, box, x, y);
        break;
    case BOX_CONTROL:
        paint_control(s, box, x, y);
        break;
    case BOX_BULLET:
        paint_bullet(s, box, x, y);
        break;
    case BOX_RULE: {
        /* A thin filled line.  <hr> makes one a pixel tall; table gridlines make
         * them a pixel wide (vertical) or a row/table tall/wide (the fill is the
         * box's own width x height, so both shapes come out of the one kind). */
        colour_t line = colour_mix(g_theme.window, g_theme.text, 45);
        int w = box->width < 1 ? 1 : box->width;
        int h = box->height < 1 ? 1 : box->height;
        gui_fill(s, rect_make(x, y, w, h), line);
        break;
    }
    }

    for (const box_t *child = box->first_child; child; child = child->next)
        paint_box(s, child, origin_x, origin_y, top, bottom, state);
}

void paint_page(surface_t *s, rect_t area, const layout_t *layout,
                int scroll, const paint_state_t *state) {
    if (!layout || !layout->root) return;

    rect_t saved = surface_clip(s);
    surface_set_clip(s, rect_intersection(saved, area));

    paint_box(s, layout->root, area.x, area.y - scroll,
              scroll - 64, scroll + area.h + 64, state);

    surface_set_clip(s, saved);
}

/* Which element is under a point, so that hovering a link can be shown.  The
 * innermost match wins, because that is the one the pointer is really over. */
static const dom_node_t *node_at(const box_t *box, int x, int y) {
    const dom_node_t *found = NULL;

    if (x >= box->x && x < box->x + box->width &&
        y >= box->y && y < box->y + box->height && box->node)
        found = box->node;

    for (const box_t *child = box->first_child; child; child = child->next) {
        const dom_node_t *inner = node_at(child, x, y);
        if (inner) found = inner;
    }
    return found;
}

const dom_node_t *paint_node_at(const layout_t *layout, int x, int y) {
    if (!layout || !layout->root) return NULL;
    return node_at(layout->root, x, y);
}

/* -------------------------------------------------------- the default sheet
 *
 * What an element looks like before the page has said anything.  Every browser
 * has one of these and they are all substantially the same, because pages have
 * been written against them for thirty years: a page that sets no styles at
 * all still has to come out looking like a document.
 */
const char *css_default_stylesheet(void) {
    return
    "html, body { display: block; margin: 0; padding: 0; }\n"
    "body { color: #18181b; background: #ffffff; padding: 0; }\n"
    "div, p, section, article, header, footer, nav, main, aside, figure,\n"
    "figcaption, address, blockquote, form, fieldset, dl, dt, dd, details,\n"
    "summary, hr, pre, tbody, thead, tfoot, tr, caption, ul, ol,\n"
    "h1, h2, h3, h4, h5, h6 { display: block; }\n"
    "li { display: list-item; }\n"
    "table { display: table; }\n"
    "td, th { display: table-cell; }\n"
    "head, style, script, title, meta, link, template, noscript { display: none; }\n"

    "p { margin: 0.9em 0; }\n"
    "h1 { font-size: 2em; font-weight: bold; margin: 0.6em 0 0.4em 0; }\n"
    "h2 { font-size: 1.5em; font-weight: bold; margin: 0.7em 0 0.4em 0; }\n"
    "h3 { font-size: 1.25em; font-weight: bold; margin: 0.8em 0 0.4em 0; }\n"
    "h4 { font-size: 1.1em; font-weight: bold; margin: 1em 0 0.4em 0; }\n"
    "h5 { font-size: 1em; font-weight: bold; margin: 1.1em 0 0.4em 0; }\n"
    "h6 { font-size: 0.9em; font-weight: bold; margin: 1.2em 0 0.4em 0; }\n"

    "b, strong { font-weight: bold; }\n"
    "i, em, cite, var, address { font-style: italic; }\n"
    "u, ins { text-decoration: underline; }\n"
    "s, del, strike { text-decoration: line-through; }\n"
    "small { font-size: 0.85em; }\n"
    "big { font-size: 1.15em; }\n"
    "mark { background: #fff3a3; }\n"

    "a { color: #1a5fd0; text-decoration: underline; }\n"

    "code, kbd, samp, tt, pre { font-family: monospace; font-size: 0.94em; }\n"
    "pre { white-space: pre; margin: 1em 0; padding: 12px 14px;\n"
    "      background: #f6f6f7; border: 1px solid #e3e3e6; border-radius: 4px; }\n"
    "code { background: #f2f2f4; }\n"

    "ul, ol { margin: 0.9em 0; padding-left: 34px; }\n"
    "ul { list-style-type: disc; }\n"
    "ol { list-style-type: decimal; }\n"
    "ul ul, ol ol, ul ol, ol ul { margin: 0; }\n"
    "li { margin: 0.25em 0; }\n"
    "dl { margin: 0.9em 0; }\n"
    "dd { margin-left: 32px; }\n"
    "dt { font-weight: bold; margin-top: 0.5em; }\n"

    "blockquote { margin: 1em 0 1em 20px; padding-left: 14px;\n"
    "             border-left: 3px solid #d8d8dc; color: #52525b; }\n"

    "hr { margin: 1.2em 0; }\n"

    "table { margin: 1em 0; }\n"
    /* Cells carry no border of their own; a table's gridlines are drawn by the
     * layout only when the table asks (border=), so a borderless table is
     * borderless, as HTML means it. */
    "th { font-weight: bold; text-align: left; padding: 6px 10px;\n"
    "     background: #f6f6f7; }\n"
    "td { padding: 6px 10px; }\n"
    "caption { text-align: center; font-weight: bold; margin-bottom: 0.4em; }\n"

    "figure { margin: 1em 24px; }\n"
    "figcaption { font-size: 0.9em; color: #52525b; margin-top: 0.4em; }\n"
    "sub, sup { font-size: 0.75em; }\n"
    "abbr { text-decoration: none; }\n"
    "iframe, video, audio, canvas, svg, object, embed { display: none; }\n"
    ;
}
