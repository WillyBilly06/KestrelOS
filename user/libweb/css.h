/* css.h - what a page says it should look like.
 *
 * Markup says what things are; style says how they appear.  The two are
 * separate on purpose, and joining them is the cascade: for every element,
 * gather every rule that matches it, order those rules by how specifically
 * they picked it out, and let the most specific one win each property.
 * Properties that describe text then pass down to children, and properties
 * that describe boxes do not.
 *
 * See css.c.
 */
#ifndef KESTREL_CSS_H
#define KESTREL_CSS_H

#include "dom.h"
#include "gui.h"

/* ------------------------------------------------------------------ values */

typedef enum {
    LEN_AUTO = 0,
    LEN_PX,
    LEN_PERCENT,
} len_unit;

typedef struct {
    int      value;
    len_unit unit;
} len_t;

static inline len_t len_px(int px) { len_t l = { px, LEN_PX }; return l; }
static inline len_t len_auto(void) { len_t l = { 0, LEN_AUTO }; return l; }

/* Resolve against the width a percentage would be of.  `auto` stays auto and
 * the caller decides what that means in its context. */
int  len_resolve(len_t len, int against, int fallback);
bool len_is_auto(len_t len);

typedef enum {
    DISPLAY_INLINE = 0,
    DISPLAY_BLOCK,
    DISPLAY_INLINE_BLOCK,
    DISPLAY_LIST_ITEM,
    DISPLAY_TABLE,
    DISPLAY_TABLE_ROW,
    DISPLAY_TABLE_CELL,
    DISPLAY_TABLE_ROW_GROUP,
    DISPLAY_NONE,
} display_kind;

typedef enum { ALIGN_LEFT = 0, ALIGN_CENTRE, ALIGN_RIGHT, ALIGN_JUSTIFY } text_align_kind;
typedef enum { WHITE_NORMAL = 0, WHITE_PRE, WHITE_NOWRAP } white_space_kind;
typedef enum { BORDER_NONE = 0, BORDER_SOLID, BORDER_DASHED, BORDER_DOTTED } border_kind;
typedef enum { LIST_DISC = 0, LIST_DECIMAL, LIST_CIRCLE, LIST_SQUARE, LIST_NONE } list_kind;

/* The sides, in the order every shorthand in this language uses. */
enum { SIDE_TOP = 0, SIDE_RIGHT, SIDE_BOTTOM, SIDE_LEFT };

typedef struct {
    unsigned char display;
    unsigned char text_align;
    unsigned char white_space;
    unsigned char list_style;

    colour_t colour;
    colour_t background;
    bool     has_background;

    int  font_size;                 /* always resolved to pixels */
    int  font_weight;               /* 400 for normal, 700 for bold */
    bool italic, underline, strike, monospace;

    int  line_height;               /* pixels; zero means work it out */

    len_t width, height;
    len_t max_width, min_width;
    len_t margin[4];
    len_t padding[4];

    int           border_width[4];
    colour_t      border_colour[4];
    unsigned char border_style[4];
    int           border_radius;

    /* Set when the element is a link, so painting knows without asking the
     * tree again. */
    bool is_link;
} style_t;

/* ------------------------------------------------------------- stylesheets */

typedef struct rule rule_t;

typedef struct {
    rule_t *rules;
    int     count;
    void   *storage;                /* everything the sheet owns */
} stylesheet_t;

/* Parse a stylesheet.  Malformed rules are skipped and the rest is kept, which
 * is what the language requires and also what makes a page with one typo still
 * look right. */
stylesheet_t *css_parse(const char *text, size_t len);
void          css_free(stylesheet_t *sheet);

/* Work out the style of every element in the document and hang it on the node.
 * `sheets` are applied in order, so the browser's own defaults go first. */
void css_apply(document_t *doc, stylesheet_t **sheets, int sheet_count,
               int base_font_size);

/* The style attached to a node by css_apply. */
const style_t *css_style_of(const dom_node_t *node);

/* The rules a browser applies before the page's own - what an element looks
 * like when nobody has said otherwise. */
const char *css_default_stylesheet(void);

/* Parse a colour the way a stylesheet writes one. */
bool css_parse_colour(const char *text, size_t len, colour_t *out);

#endif
