/* layout.c - working out where everything goes.
 *
 * Two kinds of thing happen on a page.  Blocks stack: each one takes the full
 * width available to it, and the next starts below the last.  Inline content
 * flows: words are placed along a line until the line is full, and then a new
 * line starts.  Almost every page is those two behaviours nested inside each
 * other, and this file is the two of them.
 *
 * The part that takes the most care is where they meet.  An element's children
 * are not reliably all one kind - a paragraph can contain a stray block, a
 * division can contain loose text before its first child block.  When that
 * happens the loose inline content is gathered into lines of its own, which is
 * what the specification calls an anonymous box and what makes the two models
 * compose instead of conflict.
 *
 * The other part worth stating is what is deliberately absent.  There is no
 * floating, no absolute positioning, no flexible box layout and no grid.  An
 * element that asks for any of those is laid out as an ordinary block, which
 * means its content appears, in order, readable, and not where the author drew
 * it.  For a page that exists to be read, that is a far better failure than
 * the alternative.
 */
#include "layout.h"

/* ---------------------------------------------------------------- storage */

typedef struct layout_block {
    struct layout_block *next;
    size_t cap, at;
    char   data[];
} layout_block_t;

typedef struct {
    layout_block_t *blocks;
} layout_storage_t;

static void *box_alloc(layout_t *layout, size_t size) {
    layout_storage_t *storage = layout->storage;
    size = (size + 7) & ~(size_t)7;
    if (!storage->blocks || storage->blocks->at + size > storage->blocks->cap) {
        size_t cap = 64 * 1024;
        while (cap < size) cap *= 2;
        layout_block_t *block = malloc(sizeof(layout_block_t) + cap);
        if (!block) return NULL;
        block->cap = cap;
        block->at = 0;
        block->next = storage->blocks;
        storage->blocks = block;
    }
    void *p = storage->blocks->data + storage->blocks->at;
    storage->blocks->at += size;
    memset(p, 0, size);
    return p;
}

static box_t *new_box(layout_t *layout, box_kind kind, const style_t *style,
                      const dom_node_t *node) {
    box_t *box = box_alloc(layout, sizeof *box);
    if (!box) return NULL;
    box->kind = kind;
    box->style = style;
    box->node = node;
    return box;
}

static void add_child(box_t *parent, box_t *child) {
    child->parent = parent;
    if (parent->last_child) parent->last_child->next = child;
    else parent->first_child = child;
    parent->last_child = child;
}

/* ------------------------------------------------------------------ fonts */

const aafont_t *layout_font_for(const style_t *style) {
    if (!style) return &AA_BODY;
    if (style->monospace) return &AA_CODE;

    /* The heading face is a separate rasterisation at a much larger size; it
     * is used for anything large and heavy, because scaling the body face up
     * to a heading looks exactly like what it is. */
    if (style->font_weight >= 600) {
        if (style->font_size >= 22) return &AA_HEADING;
        return &AA_BODY_BOLD;
    }
    if (style->font_size >= 30) return &AA_HEADING;
    return &AA_BODY;
}

static int line_height_for(const style_t *style) {
    if (style->line_height > 0) return style->line_height;
    /* A little over the font's own height reads better than exactly it. */
    const aafont_t *font = layout_font_for(style);
    int natural = aa_line_height(font, style->font_size);
    int comfortable = style->font_size * 3 / 2;
    return natural > comfortable ? natural : comfortable;
}

/* ------------------------------------------------------------------ links */

static void remember_link(layout_t *layout, box_t *box, const char *href) {
    if (layout->link_count >= layout->link_capacity) {
        int bigger = layout->link_capacity ? layout->link_capacity * 2 : 64;
        void *grown = realloc(layout->links, (size_t)bigger * sizeof *layout->links);
        if (!grown) return;
        layout->links = grown;
        layout->link_capacity = bigger;
    }
    int i = layout->link_count++;
    layout->links[i].x = box->x;
    layout->links[i].y = box->y;
    layout->links[i].w = box->width;
    layout->links[i].h = box->height;
    layout->links[i].href = href;
    layout->links[i].node = box->node;
}

/* A control the user can type into: a textarea, or an input that is not a
 * button/checkbox/radio.  Shared by the browser (paint.c exposes it too). */
int layout_node_is_editable(const dom_node_t *node) {
    if (!node) return 0;
    if (node->tag == TAG_TEXTAREA) return 1;
    if (node->tag != TAG_INPUT) return 0;
    const char *t = dom_attr(node, "type");
    return !t || (strcmp(t, "submit") && strcmp(t, "button") &&
                  strcmp(t, "checkbox") && strcmp(t, "radio") &&
                  strcmp(t, "hidden"));
}

/* A control the user toggles rather than types into: a checkbox or a radio
 * button.  A click flips it (radios clear their same-name siblings); it carries
 * no keyboard focus. */
int layout_node_is_checkable(const dom_node_t *node) {
    if (!node || node->tag != TAG_INPUT) return 0;
    const char *t = dom_attr(node, "type");
    return t && (!strcmp(t, "checkbox") || !strcmp(t, "radio"));
}

/* A control whose click submits the form: <button> (which defaults to submit)
 * or <input type=submit>.  Clicking one is how a form with no text field - just
 * checkboxes, say - is sent, since there is no field to press Enter in. */
int layout_node_is_submit(const dom_node_t *node) {
    if (!node) return 0;
    const char *t = dom_attr(node, "type");
    if (node->tag == TAG_BUTTON) return !t || !strcmp(t, "submit");
    if (node->tag == TAG_INPUT)  return t && !strcmp(t, "submit");
    return 0;
}


static void remember_input(layout_t *layout, box_t *box) {
    if (layout->input_count >= layout->input_capacity) {
        int bigger = layout->input_capacity ? layout->input_capacity * 2 : 16;
        void *grown = realloc(layout->inputs, (size_t)bigger * sizeof *layout->inputs);
        if (!grown) return;
        layout->inputs = grown;
        layout->input_capacity = bigger;
    }
    int i = layout->input_count++;
    layout->inputs[i].x = box->x;
    layout->inputs[i].y = box->y;
    layout->inputs[i].w = box->width;
    layout->inputs[i].h = box->height;
    layout->inputs[i].node = box->node;
}

struct dom_node *layout_input_at(const layout_t *layout, int x, int y) {
    const dom_node_t *found = NULL;
    for (int i = 0; i < layout->input_count; i++) {
        if (x >= layout->inputs[i].x && x < layout->inputs[i].x + layout->inputs[i].w &&
            y >= layout->inputs[i].y && y < layout->inputs[i].y + layout->inputs[i].h)
            found = layout->inputs[i].node;
    }
    return (struct dom_node *)found;   /* the browser attaches a typed value */
}


const char *layout_link_at(const layout_t *layout, int x, int y) {
    /* Later links are drawn over earlier ones, so the last match wins. */
    const char *found = NULL;
    for (int i = 0; i < layout->link_count; i++) {
        if (x >= layout->links[i].x && x < layout->links[i].x + layout->links[i].w &&
            y >= layout->links[i].y && y < layout->links[i].y + layout->links[i].h)
            found = layout->links[i].href;
    }
    return found;
}

/* ------------------------------------------------------- the inline pass
 *
 * Inline content is gathered into a flat list of runs first, then broken into
 * lines.  Doing it in two steps rather than one is what makes wrapping work
 * across element boundaries: "the <b>quick</b> brown" is three runs and has to
 * break between any two words in it, including the ones on either side of the
 * bold.
 */
typedef struct {
    const style_t    *style;
    const dom_node_t *node;
    const char       *text;
    int               len;
    int               width;
    bool              is_space;         /* a breakable gap */
    bool              is_break;         /* a line break that must be taken */
    bool              is_image;
    bool              is_control;
    int               height;           /* for images and controls */
} run_t;

typedef struct {
    run_t *runs;
    int    count, capacity;
} runs_t;

static run_t *push_run(runs_t *list) {
    if (list->count >= list->capacity) {
        int bigger = list->capacity ? list->capacity * 2 : 128;
        void *grown = realloc(list->runs, (size_t)bigger * sizeof *list->runs);
        if (!grown) return NULL;
        list->runs = grown;
        list->capacity = bigger;
    }
    run_t *run = &list->runs[list->count++];
    memset(run, 0, sizeof *run);
    return run;
}

static bool is_space_char(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* Break one text node into word runs and gap runs.  Runs of blank space in the
 * source collapse to a single gap, which is the rule that lets a page put line
 * breaks and indentation wherever it likes without them showing up. */
static void runs_from_text(runs_t *list, const dom_node_t *node,
                           const style_t *style, bool *pending_space) {
    const char *text = node->text;
    if (!text) return;

    const aafont_t *font = layout_font_for(style);
    int size = style->font_size;
    bool preformatted = (style->white_space == WHITE_PRE);

    int at = 0;
    while (text[at]) {
        if (!preformatted && is_space_char(text[at])) {
            while (text[at] && is_space_char(text[at])) at++;
            *pending_space = true;
            continue;
        }

        if (preformatted && text[at] == '\n') {
            run_t *run = push_run(list);
            if (!run) return;
            run->style = style;
            run->node = node;
            run->is_break = true;
            at++;
            continue;
        }

        int start = at;
        if (preformatted) {
            while (text[at] && text[at] != '\n') at++;
        } else {
            while (text[at] && !is_space_char(text[at])) at++;
        }
        if (at == start) { at++; continue; }

        if (*pending_space) {
            run_t *gap = push_run(list);
            if (!gap) return;
            gap->style = style;
            gap->node = node;
            gap->is_space = true;
            gap->width = aa_advance(font, size, ' ');
            *pending_space = false;
        }

        run_t *run = push_run(list);
        if (!run) return;
        run->style = style;
        run->node = node;
        run->text = text + start;
        run->len = at - start;
        run->width = aa_text_width_n(font, size, run->text, (size_t)run->len);
    }
}

/* ------------------------------------------------------------- the tree */

static bool is_block_level(const style_t *style) {
    if (!style) return false;
    switch (style->display) {
    case DISPLAY_BLOCK: case DISPLAY_LIST_ITEM: case DISPLAY_TABLE:
    case DISPLAY_TABLE_ROW: case DISPLAY_TABLE_CELL: case DISPLAY_TABLE_ROW_GROUP:
        return true;
    default:
        return false;
    }
}

/* Does this subtree contain anything that lays out as a block?  An element
 * whose children are all inline is laid out as lines; one that mixes them has
 * to wrap the loose inline parts. */
static bool has_block_child(const dom_node_t *node) {
    for (dom_node_t *child = node->first_child; child; child = child->next) {
        if (child->kind != NODE_ELEMENT) continue;
        const style_t *style = css_style_of(child);
        if (!style || style->display == DISPLAY_NONE) continue;
        if (is_block_level(style)) return true;
    }
    return false;
}

typedef struct {
    layout_t   *layout;
    document_t *doc;
    int         viewport_width;
} context_t;

static int layout_block_box(context_t *ctx, box_t *parent, const dom_node_t *node,
                            const style_t *style, int x, int y, int available);

/* Gather the inline content of `node` into `list`, descending through inline
 * elements so that their styles come with them. */
/* One <img> becomes one inline run.  Called both when an image sits among
 * other inline content and when it is a lone inline child of a block - the
 * second case used to be dropped, so a picture that was not wrapped in a
 * paragraph did not show at all. */
static void emit_image_run(context_t *ctx, runs_t *list, const dom_node_t *child,
                           const style_t *style) {
    run_t *run = push_run(list);
    if (!run) return;
    run->style = style;
    run->node = child;
    run->is_image = true;

    const char *alt = dom_attr(child, "alt");
    if (alt && *alt) {
        run->text = alt;
        run->len = (int)strlen(alt);
        run->width = aa_text_width_n(&AA_BODY, style->font_size,
                                     alt, (size_t)run->len) + 24;
    } else {
        run->width = 24;
    }
    run->height = style->font_size + 8;

    const char *wattr = dom_attr(child, "width");
    const char *hattr = dom_attr(child, "height");
    if (wattr && *wattr) { int w = atoi(wattr); if (w > 0) run->width = w; }
    if (hattr && *hattr) { int h = atoi(hattr); if (h > 0) run->height = h; }

    int given = len_resolve(style->width, ctx->viewport_width, 0);
    if (given > 0 && given < ctx->viewport_width) run->width = given;
    int tall = len_resolve(style->height, 0, 0);
    if (tall > 0) run->height = tall;
}

static void collect_inline(context_t *ctx, runs_t *list, const dom_node_t *node,
                           bool *pending_space) {
    for (dom_node_t *child = node->first_child; child; child = child->next) {
        if (child->kind == NODE_TEXT) {
            const style_t *style = css_style_of(node);
            if (style) runs_from_text(list, child, style, pending_space);
            continue;
        }

        const style_t *style = css_style_of(child);
        if (!style || style->display == DISPLAY_NONE) continue;

        if (child->tag == TAG_BR) {
            run_t *run = push_run(list);
            if (run) { run->style = style; run->node = child; run->is_break = true; }
            *pending_space = false;
            continue;
        }

        if (child->tag == TAG_IMG) {
            emit_image_run(ctx, list, child, style);
            continue;
        }

        if (child->tag == TAG_INPUT || child->tag == TAG_BUTTON ||
            child->tag == TAG_SELECT || child->tag == TAG_TEXTAREA) {
            run_t *run = push_run(list);
            if (!run) continue;
            run->style = style;
            run->node = child;
            run->is_control = true;

            const char *value = dom_attr(child, "value");
            const char *placeholder = dom_attr(child, "placeholder");
            static char label[128];
            if (child->tag == TAG_BUTTON) dom_text_content(child, label, sizeof label);
            else if (child->form_value && child->form_value[0])
                strlcpy(label, child->form_value, sizeof label);  /* what was typed */
            else if (value && *value) strlcpy(label, value, sizeof label);
            else if (placeholder && *placeholder) strlcpy(label, placeholder, sizeof label);
            else label[0] = 0;

            const char *kept = label[0] ? doc_string(ctx->doc, label, strlen(label)) : NULL;
            run->text = kept;
            run->len = kept ? (int)strlen(kept) : 0;
            run->width = 140;
            if (kept) {
                int text_width = aa_text_width_n(&AA_BODY, style->font_size,
                                                 kept, (size_t)run->len) + 24;
                if (text_width > run->width) run->width = text_width;
            }
            run->height = style->font_size + 14;
            continue;
        }

        collect_inline(ctx, list, child, pending_space);
    }
}

/* Break a run list into lines and place them.  Returns the height used. */
static int place_lines(context_t *ctx, box_t *parent, runs_t *list,
                       const style_t *style, int x, int y, int available) {
    int at = 0;
    int top = y;

    while (at < list->count) {
        /* Skip a gap at the start of a line: it would indent the line by a
         * space for no reason. */
        while (at < list->count && list->runs[at].is_space) at++;
        if (at >= list->count) break;

        /* How many runs fit. */
        int end = at;
        int used = 0;
        int last_breakable = -1;
        bool forced = false;

        while (end < list->count) {
            run_t *run = &list->runs[end];
            if (run->is_break) { forced = true; break; }

            if (used + run->width > available && used > 0) {
                /* Break at the last gap; if there was none, this run has to
                 * go on a line of its own however wide it is. */
                if (last_breakable > at) end = last_breakable;
                break;
            }
            if (run->is_space) last_breakable = end;
            used += run->width;
            end++;
        }
        if (end == at) end = at + 1;            /* always make progress */

        /* Trim a gap at the end of the line. */
        int line_end = end;
        while (line_end > at && list->runs[line_end - 1].is_space) line_end--;

        /* The line's height is the tallest thing on it. */
        int height = 0;
        int baseline = 0;
        for (int i = at; i < line_end; i++) {
            const style_t *run_style = list->runs[i].style;
            int run_height = list->runs[i].is_image || list->runs[i].is_control
                           ? list->runs[i].height + 4
                           : line_height_for(run_style);
            if (run_height > height) height = run_height;

            const aafont_t *font = layout_font_for(run_style);
            int run_baseline = aa_baseline(font, run_style->font_size);
            /* Centre the text within its line box, which is what the extra
             * line height is for. */
            int leading = (run_height - aa_line_height(font, run_style->font_size)) / 2;
            if (leading < 0) leading = 0;
            if (run_baseline + leading > baseline) baseline = run_baseline + leading;
        }
        if (!height) height = line_height_for(style);
        if (!baseline) baseline = height * 3 / 4;

        box_t *line = new_box(ctx->layout, BOX_LINE, style, NULL);
        if (!line) break;
        line->x = x;
        line->y = top;
        line->width = available;
        line->height = height;
        line->baseline = baseline;
        add_child(parent, line);

        /* Where the line starts, which is what alignment decides. */
        int content_width = 0;
        for (int i = at; i < line_end; i++) content_width += list->runs[i].width;

        int pen = x;
        if (style->text_align == ALIGN_CENTRE && content_width < available)
            pen += (available - content_width) / 2;
        else if (style->text_align == ALIGN_RIGHT && content_width < available)
            pen += available - content_width;

        for (int i = at; i < line_end; i++) {
            run_t *run = &list->runs[i];
            if (run->is_space) { pen += run->width; continue; }

            box_kind kind = run->is_image ? BOX_IMAGE
                          : run->is_control ? BOX_CONTROL : BOX_TEXT;
            box_t *box = new_box(ctx->layout, kind, run->style, run->node);
            if (!box) break;
            box->x = pen;
            box->width = run->width;
            box->text = run->text;
            box->text_len = run->len;

            if (kind == BOX_TEXT) {
                box->y = top;
                box->height = height;
                box->baseline = baseline;
            } else {
                box->height = run->height;
                box->y = top + baseline - run->height + 2;
                if (box->y < top) box->y = top;
                box->baseline = run->height * 3 / 4;
            }

            add_child(line, box);

            /* A link's clickable area is the box it drew into. */
            if (run->style->is_link) {
                for (const dom_node_t *up = run->node; up; up = up->parent) {
                    if (up->kind == NODE_ELEMENT && up->tag == TAG_A) {
                        const char *href = dom_attr(up, "href");
                        if (href && *href) {
                            box_t area = *box;
                            area.node = up;
                            remember_link(ctx->layout, &area, href);
                        }
                        break;
                    }
                }
            }

            /* An editable control's box is where a click focuses it; a checkbox
             * or radio's box is where a click toggles it.  Both go in the same
             * hit list so layout_input_at finds either. */
            if (run->is_control && (layout_node_is_editable(run->node) ||
                                    layout_node_is_checkable(run->node) ||
                                    layout_node_is_submit(run->node))) {
                box_t area = *box;
                area.node = run->node;
                remember_input(ctx->layout, &area);
            }

            pen += run->width;
        }

        top += height;
        at = forced ? end + 1 : end;
    }

    return top - y;
}

/* Margins between stacked blocks collapse: two blocks with a margin each are
 * separated by the larger of the two rather than their sum.  Without this
 * every page has twice the spacing its author expected. */
static int collapse(int previous_bottom_margin, int this_top_margin) {
    return previous_bottom_margin > this_top_margin
         ? previous_bottom_margin : this_top_margin;
}

static int count_list_position(const dom_node_t *item) {
    int number = 1;
    for (const dom_node_t *before = item->prev; before; before = before->prev)
        if (before->kind == NODE_ELEMENT && before->tag == TAG_LI) number++;
    return number;
}

static int layout_block_box(context_t *ctx, box_t *parent, const dom_node_t *node,
                            const style_t *style, int x, int y, int available);

/* The nearest <table> ancestor of a node, so a <tr> is attributed to the table
 * it actually belongs to and a nested table's rows are not stolen by the outer
 * one. */
static const dom_node_t *nearest_table(const dom_node_t *n) {
    for (const dom_node_t *a = n ? n->parent : NULL; a; a = a->parent)
        if (a->tag == TAG_TABLE) return a;
    return NULL;
}

/* Lay a <table> out as a grid.  Rows are <tr> belonging to this table (directly
 * or through thead/tbody/tfoot); cells are <td>/<th>.  Columns share the width
 * equally - a plain, readable grid, which is the difference between a table and
 * the same cells stacked one per line.  No colspan/rowspan or content-fitted
 * widths yet; those refine this, they do not replace it.  Fills `table_box`
 * with cell boxes and returns the height used. */
#define TBL_MAX_COLS 32
static int layout_table(context_t *ctx, box_t *table_box, const dom_node_t *table,
                        int x, int y, int avail) {
    int ncol = 0;
    for (const dom_node_t *n = table; n; n = dom_next(n, table)) {
        if (n->tag != TAG_TR || nearest_table(n) != table) continue;
        int cells = 0;
        for (const dom_node_t *c = n->first_child; c; c = c->next)
            if (c->tag == TAG_TD || c->tag == TAG_TH) cells++;
        if (cells > ncol) ncol = cells;
    }
    if (ncol < 1) return 0;
    if (ncol > TBL_MAX_COLS) ncol = TBL_MAX_COLS;

    /* Column widths from content: each column is as wide as its widest cell's
     * text (a min so an empty column is not a sliver), so a "City" column is
     * not forced as wide as a paragraph next to it - the way a real browser
     * sizes an auto-width table.  If the columns together overflow the space,
     * scale them down to fit rather than run off the edge. */
    int colw[TBL_MAX_COLS];
    for (int c = 0; c < ncol; c++) colw[c] = 40;   /* minimum */
    for (const dom_node_t *row = table; row; row = dom_next(row, table)) {
        if (row->tag != TAG_TR || nearest_table(row) != table) continue;
        int col = 0;
        for (const dom_node_t *cell = row->first_child;
             cell && col < ncol; cell = cell->next) {
            if (cell->tag != TAG_TD && cell->tag != TAG_TH) continue;
            const style_t *cs = css_style_of(cell);
            if (!cs) cs = css_style_of(row);
            char buf[512];
            dom_text_content(cell, buf, sizeof buf);
            int w = aa_text_width(layout_font_for(cs), cs->font_size, buf) + 24;
            if (w > colw[col]) colw[col] = w;
            col++;
        }
    }
    int total = 0;
    for (int c = 0; c < ncol; c++) total += colw[c];
    if (total > avail && total > 0)                 /* too wide: scale to fit */
        for (int c = 0; c < ncol; c++) {
            colw[c] = colw[c] * avail / total;
            if (colw[c] < 1) colw[c] = 1;
        }

#define TBL_MAX_ROWS 512
    int row_top[TBL_MAX_ROWS + 1];
    int nrows = 0;
    int cursor = y;
    for (const dom_node_t *row = table; row; row = dom_next(row, table)) {
        if (row->tag != TAG_TR || nearest_table(row) != table) continue;
        if (nrows < TBL_MAX_ROWS) row_top[nrows++] = cursor;
        int col = 0, row_h = 0, cx = x;
        for (const dom_node_t *cell = row->first_child;
             cell && col < ncol; cell = cell->next) {
            if (cell->tag != TAG_TD && cell->tag != TAG_TH) continue;
            const style_t *cs = css_style_of(cell);
            if (!cs) cs = css_style_of(row);
            int cw = colw[col] < 1 ? 1 : colw[col];
            int used = layout_block_box(ctx, table_box, cell, cs, cx, cursor, cw);
            if (used > row_h) row_h = used;
            cx += cw;
            col++;
        }
        if (row_h < 1) row_h = 1;
        cursor += row_h;
    }
    row_top[nrows] = cursor;                     /* the bottom edge */

    /* When the table asks for a border, draw the grid: a line at every column
     * edge (full height) and every row edge (full width), reusing BOX_RULE.
     * `border="0"` or no border attribute leaves the cells lineless. */
    const char *battr = dom_attr(table, "border");
    if (battr && strcmp(battr, "0") && nrows > 0) {
        const style_t *tstyle = css_style_of(table);
        int table_w = 0;
        for (int c = 0; c < ncol; c++) table_w += colw[c];
        int table_h = cursor - y;
        int ex = x;
        for (int c = 0; c <= ncol; c++) {        /* vertical column edges */
            box_t *v = new_box(ctx->layout, BOX_RULE, tstyle, NULL);
            if (v) { v->x = ex; v->y = y; v->width = 1; v->height = table_h;
                     add_child(table_box, v); }
            if (c < ncol) ex += colw[c];
        }
        for (int r = 0; r <= nrows; r++) {        /* horizontal row edges */
            box_t *h = new_box(ctx->layout, BOX_RULE, tstyle, NULL);
            if (h) { h->x = x; h->y = row_top[r]; h->width = table_w; h->height = 1;
                     add_child(table_box, h); }
        }
    }
    return cursor - y;
}

/* Lay out one block and everything inside it.  Returns its total height
 * including its own padding and borders but not its margins. */
static int layout_block_box(context_t *ctx, box_t *parent, const dom_node_t *node,
                            const style_t *style, int x, int y, int available) {
    box_t *box = new_box(ctx->layout, BOX_BLOCK, style, node);
    if (!box) return 0;

    int border_left = style->border_width[SIDE_LEFT];
    int border_right = style->border_width[SIDE_RIGHT];
    int border_top = style->border_width[SIDE_TOP];
    int border_bottom = style->border_width[SIDE_BOTTOM];

    int padding_left = len_resolve(style->padding[SIDE_LEFT], available, 0);
    int padding_right = len_resolve(style->padding[SIDE_RIGHT], available, 0);
    int padding_top = len_resolve(style->padding[SIDE_TOP], available, 0);
    int padding_bottom = len_resolve(style->padding[SIDE_BOTTOM], available, 0);

    int margin_left = len_resolve(style->margin[SIDE_LEFT], available, 0);
    int margin_right = len_resolve(style->margin[SIDE_RIGHT], available, 0);

    int outer_width = available - margin_left - margin_right;
    if (outer_width < 1) outer_width = 1;

    /* An explicit width applies to the content, not to the whole box. */
    if (!len_is_auto(style->width)) {
        int wanted = len_resolve(style->width, available, outer_width);
        int total = wanted + padding_left + padding_right + border_left + border_right;
        if (total > 0 && total < outer_width) {
            /* A block narrower than its room and with automatic side margins
             * is centred, which is how most pages centre their content. */
            if (len_is_auto(style->margin[SIDE_LEFT]) &&
                len_is_auto(style->margin[SIDE_RIGHT]))
                margin_left += (outer_width - total) / 2;
            outer_width = total;
        }
    }

    if (!len_is_auto(style->max_width)) {
        int limit = len_resolve(style->max_width, available, outer_width);
        if (limit > 0 && limit + padding_left + padding_right < outer_width) {
            int total = limit + padding_left + padding_right + border_left + border_right;
            if (len_is_auto(style->margin[SIDE_LEFT]) &&
                len_is_auto(style->margin[SIDE_RIGHT]))
                margin_left += (outer_width - total) / 2;
            outer_width = total;
        }
    }

    box->x = x + margin_left;
    box->y = y;
    box->width = outer_width;

    int content_x = box->x + border_left + padding_left;
    int content_width = outer_width - border_left - border_right
                                    - padding_left - padding_right;
    if (content_width < 1) content_width = 1;

    int content_y = y + border_top + padding_top;
    int content_height = 0;

    /* A list item's marker sits outside the content, in the space the list's
     * own padding left for it. */
    if (style->display == DISPLAY_LIST_ITEM && style->list_style != LIST_NONE) {
        box_t *bullet = new_box(ctx->layout, BOX_BULLET, style, node);
        if (bullet) {
            bullet->x = content_x - 22;
            bullet->y = content_y;
            bullet->width = 18;
            bullet->height = line_height_for(style);
            bullet->baseline = aa_baseline(layout_font_for(style), style->font_size);
            if (style->list_style == LIST_DECIMAL)
                bullet->bullet_number = count_list_position(node);
            add_child(box, bullet);
        }
    }

    if (style->display == DISPLAY_TABLE) {
        /* A grid, not the normal block flow that would stack the cells. */
        content_height = layout_table(ctx, box, node, content_x, content_y,
                                      content_width);
    } else if (node->tag == TAG_HR) {
        box_t *rule = new_box(ctx->layout, BOX_RULE, style, node);
        if (rule) {
            rule->x = content_x;
            rule->y = content_y;
            rule->width = content_width;
            rule->height = 1;
            add_child(box, rule);
        }
        content_height = 1;
    } else if (!has_block_child(node)) {
        /* Everything inside is inline, so it becomes lines. */
        runs_t list;
        memset(&list, 0, sizeof list);
        bool pending_space = false;
        collect_inline(ctx, &list, node, &pending_space);
        if (list.count)
            content_height = place_lines(ctx, box, &list, style,
                                         content_x, content_y, content_width);
        free(list.runs);
    } else {
        /* A mixture.  Consecutive inline children are gathered into lines of
         * their own between the blocks. */
        int cursor = content_y;
        int previous_margin = 0;

        runs_t pending;
        memset(&pending, 0, sizeof pending);
        bool pending_space = false;

        for (dom_node_t *child = node->first_child; child; child = child->next) {
            const style_t *child_style = child->kind == NODE_ELEMENT
                                       ? css_style_of(child) : style;
            if (child->kind == NODE_ELEMENT &&
                (!child_style || child_style->display == DISPLAY_NONE))
                continue;

            bool child_is_block = child->kind == NODE_ELEMENT &&
                                  is_block_level(child_style);

            if (!child_is_block) {
                if (child->kind == NODE_TEXT)
                    runs_from_text(&pending, child, style, &pending_space);
                else if (child->tag == TAG_BR) {
                    run_t *run = push_run(&pending);
                    if (run) { run->style = child_style; run->node = child;
                               run->is_break = true; }
                } else if (child->tag == TAG_IMG) {
                    /* A lone <img> among the block's children: it represents
                     * itself, so emit its run directly rather than gathering a
                     * subtree it does not have. */
                    emit_image_run(ctx, &pending, child, child_style);
                } else {
                    /* One inline element, with its own subtree. */
                    dom_node_t wrapper = *child;
                    (void)wrapper;
                    runs_t single;
                    memset(&single, 0, sizeof single);
                    bool space = pending_space;
                    collect_inline(ctx, &single, child, &space);
                    /* An inline element with only text of its own produces
                     * nothing above; take its text directly in that case. */
                    if (!single.count && child->first_child == NULL) {
                        free(single.runs);
                        continue;
                    }
                    for (int i = 0; i < single.count; i++) {
                        run_t *run = push_run(&pending);
                        if (run) *run = single.runs[i];
                    }
                    free(single.runs);
                    pending_space = space;
                }
                continue;
            }

            /* A block child: flush whatever inline content came before it. */
            if (pending.count) {
                cursor += place_lines(ctx, box, &pending, style,
                                      content_x, cursor, content_width);
                pending.count = 0;
                pending_space = false;
                previous_margin = 0;
            }

            int child_margin_top = len_resolve(child_style->margin[SIDE_TOP],
                                               content_width, 0);
            cursor += collapse(previous_margin, child_margin_top);

            int used = layout_block_box(ctx, box, child, child_style,
                                        content_x, cursor, content_width);
            cursor += used;
            previous_margin = len_resolve(child_style->margin[SIDE_BOTTOM],
                                          content_width, 0);
        }

        if (pending.count)
            cursor += place_lines(ctx, box, &pending, style,
                                  content_x, cursor, content_width);
        else
            cursor += previous_margin;

        free(pending.runs);
        content_height = cursor - content_y;
    }

    if (!len_is_auto(style->height)) {
        int wanted = len_resolve(style->height, 0, content_height);
        if (wanted > content_height) content_height = wanted;
    }

    box->height = content_height + padding_top + padding_bottom
                                 + border_top + border_bottom;

    add_child(parent, box);
    return box->height;
}

/* ---------------------------------------------------------------- the top */

layout_t *layout_document(document_t *doc, int width) {
    if (!doc || !doc->root) return NULL;

    layout_t *layout = calloc(1, sizeof *layout);
    if (!layout) return NULL;

    layout->storage = calloc(1, sizeof(layout_storage_t));
    if (!layout->storage) { free(layout); return NULL; }
    layout->width = width;

    context_t ctx = { layout, doc, width };

    const style_t *root_style = css_style_of(doc->body ? doc->body : doc->root);
    if (!root_style) {
        static style_t fallback;
        fallback.font_size = 16;
        fallback.font_weight = 400;
        fallback.colour = RGB(24, 24, 27);
        fallback.display = DISPLAY_BLOCK;
        root_style = &fallback;
    }

    box_t *root = new_box(layout, BOX_BLOCK, root_style, doc->body);
    if (!root) { layout_free(layout); return NULL; }
    root->width = width;
    layout->root = root;

    dom_node_t *start = doc->body ? doc->body : doc->root;
    int height = layout_block_box(&ctx, root, start, root_style, 0, 0, width);

    root->height = height;
    layout->height = height;
    return layout;
}

void layout_free(layout_t *layout) {
    if (!layout) return;
    layout_storage_t *storage = layout->storage;
    if (storage) {
        layout_block_t *block = storage->blocks;
        while (block) {
            layout_block_t *next = block->next;
            free(block);
            block = next;
        }
        free(storage);
    }
    if (layout->links) free(layout->links);
    if (layout->inputs) free(layout->inputs);
    free(layout);
}
