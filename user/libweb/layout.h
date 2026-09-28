/* layout.h - where everything goes on the page.
 *
 * The styled tree says what each element is and how it should look; it says
 * nothing about where anything is.  Layout answers that, and the answer is a
 * second tree - of boxes rather than elements - because the two do not
 * correspond one to one.  A paragraph of text that wraps over four lines is
 * one element and four boxes; an element with `display: none` is one element
 * and no boxes at all.
 *
 * The model here is the original one the web was built on, before anything
 * else was layered over it: blocks stack down the page, inline content flows
 * along a line and wraps when it runs out of room.  It is not everything a
 * modern page can ask for, but it is what almost all readable content is, and
 * a page laid out this way is a page that can be read.
 *
 * See layout.c.
 */
#ifndef KESTREL_LAYOUT_H
#define KESTREL_LAYOUT_H

#include "dom.h"
#include "css.h"
#include "aatext.h"

typedef enum {
    BOX_BLOCK,          /* stacks vertically                        */
    BOX_LINE,           /* one line of inline content               */
    BOX_TEXT,           /* a run of text on a line                  */
    BOX_IMAGE,          /* a picture, or a placeholder for one      */
    BOX_RULE,           /* a horizontal rule                        */
    BOX_BULLET,         /* a list marker                            */
    BOX_CONTROL,        /* a form control drawn as a control        */
} box_kind;

typedef struct box {
    box_kind kind;

    /* Where it ended up, in document coordinates - the top of the page is
     * zero and scrolling subtracts from it at painting time. */
    int x, y, width, height;

    /* Where the text sits within its own box. */
    int baseline;

    const style_t   *style;
    const dom_node_t *node;         /* what it came from, for links and hit tests */

    /* BOX_TEXT: which part of which text node. */
    const char *text;
    int         text_len;

    /* BOX_BULLET: what to draw. */
    int  bullet_number;

    /* BOX_IMAGE: the decoded picture, set by the browser after layout; NULL
     * until it has been fetched, or if it could not be decoded. */
    const void *image;

    struct box *first_child, *last_child, *next, *parent;
} box_t;

typedef struct {
    box_t *root;
    int    width;                   /* what it was laid out for */
    int    height;                  /* how tall it turned out    */
    void  *storage;

    /* The links found while laying out, so a click can be answered without
     * walking the tree again. */
    struct {
        int         x, y, w, h;
        const char *href;
        const dom_node_t *node;
    } *links;
    int link_count, link_capacity;

    /* Editable controls found while laying out, so a click can focus one and
     * typing can reach it - the same idea as `links`, for <input>/<textarea>. */
    struct {
        int         x, y, w, h;
        const dom_node_t *node;
    } *inputs;
    int input_count, input_capacity;
} layout_t;

/* Lay a styled document out for a given width. */
layout_t *layout_document(document_t *doc, int width);
void      layout_free(layout_t *layout);

/* Which link, if any, is under a point.  NULL when there is none. */
const char *layout_link_at(const layout_t *layout, int x, int y);

/* The editable control at a point, or NULL - so a click can focus an input.
 * Non-const so the caller (the browser) can attach a typed value to it. */
struct dom_node *layout_input_at(const layout_t *layout, int x, int y);

/* Whether a node is a control the user can type into (input text / textarea). */
int layout_node_is_editable(const struct dom_node *node);

/* Whether a node is a checkbox or radio (toggled by a click, not typed into). */
int layout_node_is_checkable(const struct dom_node *node);

/* Whether a node is a submit control (a click on it submits the form). */
int layout_node_is_submit(const struct dom_node *node);

/* The font a style asks for, and the size to draw it at. */
const aafont_t *layout_font_for(const style_t *style);

#endif
