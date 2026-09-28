/* dom.h - a page, once it has been read.
 *
 * A document is a tree.  The markup is a flat stream of tags that only implies
 * the tree, and half of what the parser does is work out where the author left
 * the shape to be inferred - a paragraph nobody closed, a list item that ends
 * because the next one began, a table cell closed by its row ending.  What
 * comes out the other side is the tree the author meant.
 *
 * Everything in a document - nodes, names, text - is allocated out of one
 * block that belongs to that document, so closing a page is one free rather
 * than a walk over thousands of small allocations that has to be exactly
 * right.  See html.c.
 */
#ifndef KESTREL_DOM_H
#define KESTREL_DOM_H

#include "kestrel.h"

/* The elements the layout and painting code needs to distinguish.  Everything
 * else is TAG_OTHER and is treated by what its style says, which is how a
 * browser handles a tag it has never seen. */
typedef enum {
    TAG_OTHER = 0,
    TAG_HTML, TAG_HEAD, TAG_BODY, TAG_TITLE, TAG_META, TAG_LINK, TAG_BASE,
    TAG_STYLE, TAG_SCRIPT, TAG_NOSCRIPT,
    TAG_DIV, TAG_SPAN, TAG_P, TAG_BR, TAG_HR,
    TAG_H1, TAG_H2, TAG_H3, TAG_H4, TAG_H5, TAG_H6,
    TAG_A, TAG_IMG, TAG_UL, TAG_OL, TAG_LI, TAG_DL, TAG_DT, TAG_DD,
    TAG_TABLE, TAG_THEAD, TAG_TBODY, TAG_TFOOT, TAG_TR, TAG_TD, TAG_TH,
    TAG_CAPTION, TAG_COLGROUP, TAG_COL,
    TAG_B, TAG_STRONG, TAG_I, TAG_EM, TAG_U, TAG_S, TAG_SMALL, TAG_BIG,
    TAG_CODE, TAG_PRE, TAG_KBD, TAG_SAMP, TAG_TT, TAG_VAR,
    TAG_BLOCKQUOTE, TAG_Q, TAG_CITE, TAG_ABBR, TAG_SUB, TAG_SUP, TAG_MARK,
    TAG_FORM, TAG_INPUT, TAG_BUTTON, TAG_SELECT, TAG_OPTION, TAG_TEXTAREA,
    TAG_LABEL, TAG_FIELDSET, TAG_LEGEND,
    TAG_HEADER, TAG_FOOTER, TAG_NAV, TAG_MAIN, TAG_SECTION, TAG_ARTICLE,
    TAG_ASIDE, TAG_FIGURE, TAG_FIGCAPTION, TAG_TIME, TAG_ADDRESS,
    TAG_IFRAME, TAG_VIDEO, TAG_AUDIO, TAG_CANVAS, TAG_SVG, TAG_PATH,
    TAG_TEMPLATE, TAG_PICTURE, TAG_SOURCE, TAG_DETAILS, TAG_SUMMARY,
    TAG_COUNT
} tag_id;

typedef enum { NODE_ELEMENT, NODE_TEXT } node_kind;

typedef struct attr {
    const char  *name;              /* lowercased */
    const char  *value;
    struct attr *next;
} attr_t;

typedef struct dom_node {
    node_kind kind;
    tag_id    tag;
    const char *name;               /* the element's name, lowercased */
    const char *text;               /* NODE_TEXT only */

    attr_t *attrs;

    struct dom_node *parent;
    struct dom_node *first_child, *last_child;
    struct dom_node *next, *prev;

    /* Filled in by the style pass; see css.h. */
    void *style;

    /* What the user has typed into this control, for <input>/<textarea>.  NULL
     * until focused.  Points at a buffer the browser owns (a small per-window
     * table), not the arena - so it survives relayouts (which rebuild boxes but
     * keep the DOM) and paint can read it; the browser clears it on navigate. */
    char *form_value;
} dom_node_t;

/* One block per document.  Grows by chaining, so a large page does not need
 * one enormous contiguous allocation. */
typedef struct arena_block {
    struct arena_block *next;
    size_t cap, at;
    char   data[];
} arena_block_t;

typedef struct {
    dom_node_t    *root;            /* the html element */
    dom_node_t    *body;
    const char    *title;
    const char    *base_href;       /* from <base>, when present */
    arena_block_t *arena;
    int            node_count;

    /* Every stylesheet the page carried, joined: the contents of each <style>
     * element in order.  Linked sheets are fetched by the caller and appended. */
    char  *css;
    size_t css_len, css_cap;
} document_t;

/* Parse markup into a document.  Never returns NULL for well-formed input and
 * never fails on malformed input either - there is no such thing as a page a
 * browser is allowed to refuse to display. */
document_t *html_parse(const char *html, size_t len);
void        document_free(document_t *doc);

/* Arena allocation, exposed because the style pass allocates into it too. */
void *doc_alloc(document_t *doc, size_t size);
const char *doc_string(document_t *doc, const char *text, size_t len);

const char *dom_attr(const dom_node_t *node, const char *name);
bool        dom_has_class(const dom_node_t *node, const char *class_name);

/* Walk the tree in document order.  `dom_next` returns NULL at the end. */
dom_node_t *dom_next(const dom_node_t *node, const dom_node_t *root);

/* All the text under a node, joined, for a title or a link's label. */
void dom_text_content(const dom_node_t *node, char *out, size_t cap);

tag_id      tag_from_name(const char *name);
const char *tag_name_of(tag_id tag);

#endif
