/* html.c - reading markup into a tree.
 *
 * The hard thing about HTML is not the syntax, which is simple, but that
 * almost nothing about it is required.  A paragraph need not be closed.  A
 * list item ends when the next one starts.  A table cell ends when its row
 * does.  Tags may be closed in the wrong order, or not at all, or closed when
 * they were never opened.  None of that is an error - a browser that refuses a
 * page because of it is a browser nobody uses - so all of it has to be
 * inferred, and the whole of the tree-building half of this file is that
 * inference.
 *
 * The other half is the tokeniser, and its one real subtlety is that some
 * elements do not contain markup at all.  Inside a <script> or a <style>, a
 * "<" is just a character, and the only thing that ends the element is its own
 * closing tag.  Treating those contents as markup produces a tree full of
 * nonsense elements named after fragments of code.
 *
 * What this is not is a conforming HTML5 parser.  That specification runs to
 * hundreds of pages of insertion modes and is the way it is because twenty
 * years of pages depend on every corner of it.  What is here covers the shapes
 * that actually appear in documents, and a page that trips over the difference
 * renders slightly wrong rather than not at all.
 */
#include "dom.h"

/* --------------------------------------------------------------- the arena
 *
 * A document owns one chain of blocks and everything in it - nodes, names,
 * attribute values, text - comes from there.  Freeing a page is then a walk
 * over a handful of blocks instead of thousands of individual frees, and there
 * is no way to leak one node or free one twice.
 */
#define ARENA_BLOCK (64 * 1024)

static bool arena_grow(document_t *doc, size_t least) {
    size_t cap = ARENA_BLOCK;
    while (cap < least) cap *= 2;

    arena_block_t *block = malloc(sizeof(arena_block_t) + cap);
    if (!block) return false;
    block->cap = cap;
    block->at = 0;
    block->next = doc->arena;
    doc->arena = block;
    return true;
}

void *doc_alloc(document_t *doc, size_t size) {
    size = (size + 7) & ~(size_t)7;              /* keep everything aligned */
    if (!doc->arena || doc->arena->at + size > doc->arena->cap)
        if (!arena_grow(doc, size)) return NULL;
    void *p = doc->arena->data + doc->arena->at;
    doc->arena->at += size;
    memset(p, 0, size);
    return p;
}

const char *doc_string(document_t *doc, const char *text, size_t len) {
    char *copy = doc_alloc(doc, len + 1);
    if (!copy) return "";
    memcpy(copy, text, len);
    copy[len] = 0;
    return copy;
}

/* ------------------------------------------------------------- the tag table */

static const struct { const char *name; tag_id id; } tag_table[] = {
    { "html", TAG_HTML }, { "head", TAG_HEAD }, { "body", TAG_BODY },
    { "title", TAG_TITLE }, { "meta", TAG_META }, { "link", TAG_LINK },
    { "base", TAG_BASE }, { "style", TAG_STYLE }, { "script", TAG_SCRIPT },
    { "noscript", TAG_NOSCRIPT },
    { "div", TAG_DIV }, { "span", TAG_SPAN }, { "p", TAG_P },
    { "br", TAG_BR }, { "hr", TAG_HR },
    { "h1", TAG_H1 }, { "h2", TAG_H2 }, { "h3", TAG_H3 },
    { "h4", TAG_H4 }, { "h5", TAG_H5 }, { "h6", TAG_H6 },
    { "a", TAG_A }, { "img", TAG_IMG },
    { "ul", TAG_UL }, { "ol", TAG_OL }, { "li", TAG_LI },
    { "dl", TAG_DL }, { "dt", TAG_DT }, { "dd", TAG_DD },
    { "table", TAG_TABLE }, { "thead", TAG_THEAD }, { "tbody", TAG_TBODY },
    { "tfoot", TAG_TFOOT }, { "tr", TAG_TR }, { "td", TAG_TD }, { "th", TAG_TH },
    { "caption", TAG_CAPTION }, { "colgroup", TAG_COLGROUP }, { "col", TAG_COL },
    { "b", TAG_B }, { "strong", TAG_STRONG }, { "i", TAG_I }, { "em", TAG_EM },
    { "u", TAG_U }, { "s", TAG_S }, { "strike", TAG_S }, { "del", TAG_S },
    { "small", TAG_SMALL }, { "big", TAG_BIG },
    { "code", TAG_CODE }, { "pre", TAG_PRE }, { "kbd", TAG_KBD },
    { "samp", TAG_SAMP }, { "tt", TAG_TT }, { "var", TAG_VAR },
    { "blockquote", TAG_BLOCKQUOTE }, { "q", TAG_Q }, { "cite", TAG_CITE },
    { "abbr", TAG_ABBR }, { "sub", TAG_SUB }, { "sup", TAG_SUP },
    { "mark", TAG_MARK },
    { "form", TAG_FORM }, { "input", TAG_INPUT }, { "button", TAG_BUTTON },
    { "select", TAG_SELECT }, { "option", TAG_OPTION }, { "textarea", TAG_TEXTAREA },
    { "label", TAG_LABEL }, { "fieldset", TAG_FIELDSET }, { "legend", TAG_LEGEND },
    { "header", TAG_HEADER }, { "footer", TAG_FOOTER }, { "nav", TAG_NAV },
    { "main", TAG_MAIN }, { "section", TAG_SECTION }, { "article", TAG_ARTICLE },
    { "aside", TAG_ASIDE }, { "figure", TAG_FIGURE },
    { "figcaption", TAG_FIGCAPTION }, { "time", TAG_TIME }, { "address", TAG_ADDRESS },
    { "iframe", TAG_IFRAME }, { "video", TAG_VIDEO }, { "audio", TAG_AUDIO },
    { "canvas", TAG_CANVAS }, { "svg", TAG_SVG }, { "path", TAG_PATH },
    { "template", TAG_TEMPLATE }, { "picture", TAG_PICTURE }, { "source", TAG_SOURCE },
    { "details", TAG_DETAILS }, { "summary", TAG_SUMMARY },
};

tag_id tag_from_name(const char *name) {
    for (size_t i = 0; i < sizeof tag_table / sizeof tag_table[0]; i++)
        if (!strcmp(tag_table[i].name, name)) return tag_table[i].id;
    return TAG_OTHER;
}

const char *tag_name_of(tag_id tag) {
    for (size_t i = 0; i < sizeof tag_table / sizeof tag_table[0]; i++)
        if (tag_table[i].id == tag) return tag_table[i].name;
    return "?";
}

/* Elements that never have contents.  A closing tag for one of these is a
 * mistake in the page and is ignored. */
static bool is_void(tag_id tag) {
    switch (tag) {
    case TAG_BR: case TAG_HR: case TAG_IMG: case TAG_INPUT: case TAG_META:
    case TAG_LINK: case TAG_BASE: case TAG_COL: case TAG_SOURCE:
        return true;
    default:
        return false;
    }
}

/* Elements whose contents are not markup.  Everything up to the matching
 * closing tag is text, "<" included. */
static bool is_raw_text(tag_id tag) {
    return tag == TAG_SCRIPT || tag == TAG_STYLE || tag == TAG_TEXTAREA;
}

/* Whether an element starts a new block, which decides what an unclosed
 * paragraph or list item does when it arrives. */
static bool is_block(tag_id tag) {
    switch (tag) {
    case TAG_ADDRESS: case TAG_ARTICLE: case TAG_ASIDE: case TAG_BLOCKQUOTE:
    case TAG_DETAILS: case TAG_DIV: case TAG_DL: case TAG_FIELDSET:
    case TAG_FIGCAPTION: case TAG_FIGURE: case TAG_FOOTER: case TAG_FORM:
    case TAG_H1: case TAG_H2: case TAG_H3: case TAG_H4: case TAG_H5: case TAG_H6:
    case TAG_HEADER: case TAG_HR: case TAG_LI: case TAG_MAIN: case TAG_NAV:
    case TAG_OL: case TAG_P: case TAG_PRE: case TAG_SECTION: case TAG_TABLE:
    case TAG_UL: case TAG_DD: case TAG_DT: case TAG_SUMMARY:
        return true;
    default:
        return false;
    }
}

/* --------------------------------------------------- character references
 *
 * Only the ones that appear in real prose, plus the numeric forms.  A
 * reference this does not know is left exactly as it was written, which is
 * what a reader would rather see than an empty space.
 */
static const struct { const char *name; unsigned code; } entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
    { "apos", '\'' }, { "nbsp", 0xA0 }, { "copy", 0xA9 }, { "reg", 0xAE },
    { "trade", 0x2122 }, { "hellip", 0x2026 }, { "mdash", 0x2014 },
    { "ndash", 0x2013 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
    { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "bull", 0x2022 },
    { "deg", 0xB0 }, { "plusmn", 0xB1 }, { "times", 0xD7 }, { "divide", 0xF7 },
    { "frac12", 0xBD }, { "frac14", 0xBC }, { "sup2", 0xB2 }, { "sup3", 0xB3 },
    { "middot", 0xB7 }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "sect", 0xA7 },
    { "para", 0xB6 }, { "dagger", 0x2020 }, { "euro", 0x20AC }, { "pound", 0xA3 },
    { "yen", 0xA5 }, { "cent", 0xA2 }, { "larr", 0x2190 }, { "rarr", 0x2192 },
    { "uarr", 0x2191 }, { "darr", 0x2193 }, { "harr", 0x2194 },
    { "eacute", 0xE9 }, { "egrave", 0xE8 }, { "agrave", 0xE0 }, { "ccedil", 0xE7 },
    { "uuml", 0xFC }, { "ouml", 0xF6 }, { "auml", 0xE4 }, { "szlig", 0xDF },
    { "ntilde", 0xF1 }, { "aacute", 0xE1 }, { "iacute", 0xED }, { "oacute", 0xF3 },
    { "uacute", 0xFA }, { "minus", 0x2212 }, { "prime", 0x2032 },
    /* The rest of the common accented letters, so a page that spells them as
     * named references (rather than raw UTF-8) reads right too.  Every code
     * point here maps to a real glyph in fold_to_byte's Latin-1 table. */
    { "iuml", 0xEF }, { "euml", 0xEB }, { "yuml", 0xFF },
    { "acirc", 0xE2 }, { "ecirc", 0xEA }, { "icirc", 0xEE },
    { "ocirc", 0xF4 }, { "ucirc", 0xFB },
    { "igrave", 0xEC }, { "ograve", 0xF2 }, { "ugrave", 0xF9 },
    { "aring", 0xE5 }, { "aelig", 0xE6 },
};

/* This system's text is one byte per character in the layout the rest of it
 * uses, so anything outside that range becomes the nearest sensible thing
 * rather than a box.  A quotation mark that came out as a plain one is far
 * better than a quotation mark that came out as nothing. */
static int fold_to_byte(unsigned code) {
    switch (code) {
    case 0xA0: return ' ';
    case 0x2018: case 0x2019: case 0x2032: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2026: return 0x85;                    /* the ellipsis in this layout */
    case 0x2022: return 0x07;                    /* a bullet */
    case 0x2122: return 'T';
    case 0x2190: return 0x1B;
    case 0x2192: return 0x1A;
    case 0x2191: return 0x18;
    case 0x2193: return 0x19;
    case 0x2194: return 0x1D;
    case 0x20AC: return 'E';
    default: break;
    }
    if (code < 0x80) return (int)code;

    /* Latin-1 maps onto this layout's upper half at known places; the common
     * accented letters are the ones worth carrying across. */
    static const struct { unsigned from; int to; } latin[] = {
        { 0xA1, 0xAD }, { 0xA2, 0x9B }, { 0xA3, 0x9C }, { 0xA5, 0x9D },
        { 0xA7, 0x15 }, { 0xA9, 'c' }, { 0xAA, 0xA6 }, { 0xAB, 0xAE },
        { 0xAC, 0xAA }, { 0xAE, 'R' }, { 0xB0, 0xF8 }, { 0xB1, 0xF1 },
        { 0xB2, 0xFD }, { 0xB5, 0xE6 }, { 0xB6, 0x14 }, { 0xB7, 0xFA },
        { 0xBA, 0xA7 }, { 0xBB, 0xAF }, { 0xBC, 0xAC }, { 0xBD, 0xAB },
        { 0xBF, 0xA8 }, { 0xC4, 0x8E }, { 0xC5, 0x8F }, { 0xC6, 0x92 },
        { 0xC7, 0x80 }, { 0xC9, 0x90 }, { 0xD1, 0xA5 }, { 0xD6, 0x99 },
        { 0xDC, 0x9A }, { 0xDF, 0xE1 }, { 0xE0, 0x85 }, { 0xE1, 0xA0 },
        { 0xE2, 0x83 }, { 0xE4, 0x84 }, { 0xE5, 0x86 }, { 0xE6, 0x91 },
        { 0xE7, 0x87 }, { 0xE8, 0x8A }, { 0xE9, 0x82 }, { 0xEA, 0x88 },
        { 0xEB, 0x89 }, { 0xEC, 0x8D }, { 0xED, 0xA1 }, { 0xEE, 0x8C },
        { 0xEF, 0x8B }, { 0xF1, 0xA4 }, { 0xF2, 0x95 }, { 0xF3, 0xA2 },
        { 0xF4, 0x93 }, { 0xF6, 0x94 }, { 0xF7, 0xF6 }, { 0xF9, 0x97 },
        { 0xFA, 0xA3 }, { 0xFB, 0x96 }, { 0xFC, 0x81 }, { 0xFF, 0x98 },
    };
    for (size_t i = 0; i < sizeof latin / sizeof latin[0]; i++)
        if (latin[i].from == code) return latin[i].to;
    return '?';
}

/* Decode one reference starting at `&`.  Returns how many input characters it
 * consumed, or zero when it is not a reference after all. */
static size_t decode_entity(const char *in, size_t len, int *out) {
    if (len < 3 || in[0] != '&') return 0;

    if (in[1] == '#') {
        size_t at = 2;
        unsigned code = 0;
        bool hex = (at < len && (in[at] == 'x' || in[at] == 'X'));
        if (hex) at++;
        size_t digits = 0;
        while (at < len) {
            char c = in[at];
            int value;
            if (c >= '0' && c <= '9') value = c - '0';
            else if (hex && c >= 'a' && c <= 'f') value = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') value = c - 'A' + 10;
            else break;
            code = code * (hex ? 16u : 10u) + (unsigned)value;
            if (code > 0x10FFFF) code = '?';
            at++; digits++;
        }
        if (!digits) return 0;
        if (at < len && in[at] == ';') at++;
        *out = fold_to_byte(code);
        return at;
    }

    /* A name.  The semicolon is optional in practice and omitted often
     * enough that requiring it would leave "&amp" on screen. */
    size_t name_len = 0;
    while (1 + name_len < len && name_len < 12) {
        char c = in[1 + name_len];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9'))) break;
        name_len++;
    }
    if (!name_len) return 0;

    for (size_t i = 0; i < sizeof entities / sizeof entities[0]; i++) {
        size_t entity_len = strlen(entities[i].name);
        if (entity_len != name_len) continue;
        if (strncasecmp(in + 1, entities[i].name, name_len)) continue;
        size_t used = 1 + name_len;
        if (used < len && in[used] == ';') used++;
        *out = fold_to_byte(entities[i].code);
        return used;
    }
    return 0;
}

/* --------------------------------------------------------- building the tree */

typedef struct {
    document_t *doc;
    const char *in;
    size_t      len, at;

    /* The elements currently open, innermost last. */
    dom_node_t *stack[64];
    int         depth;
} parser_t;

static dom_node_t *current(parser_t *p) {
    return p->depth > 0 ? p->stack[p->depth - 1] : NULL;
}

static void attach(dom_node_t *parent, dom_node_t *child) {
    child->parent = parent;
    child->prev = parent->last_child;
    if (parent->last_child) parent->last_child->next = child;
    else parent->first_child = child;
    parent->last_child = child;
}

static dom_node_t *new_element(parser_t *p, const char *name, size_t name_len) {
    dom_node_t *node = doc_alloc(p->doc, sizeof *node);
    if (!node) return NULL;

    char *lower = doc_alloc(p->doc, name_len + 1);
    if (!lower) return NULL;
    for (size_t i = 0; i < name_len; i++) {
        char c = name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    lower[name_len] = 0;

    node->kind = NODE_ELEMENT;
    node->name = lower;
    node->tag = tag_from_name(lower);
    p->doc->node_count++;
    return node;
}

static void push(parser_t *p, dom_node_t *node) {
    dom_node_t *parent = current(p);
    if (parent) attach(parent, node);
    else if (!p->doc->root) p->doc->root = node;

    if (p->depth < (int)(sizeof p->stack / sizeof p->stack[0]))
        p->stack[p->depth++] = node;
}

/* Close elements down to and including `tag`, if it is open at all.  A closing
 * tag for something that was never opened is ignored, which is what stops one
 * stray tag from unwinding the whole document. */
static void close_to(parser_t *p, tag_id tag) {
    int found = -1;
    for (int i = p->depth - 1; i >= 0; i--)
        if (p->stack[i]->tag == tag) { found = i; break; }
    if (found < 0) return;
    p->depth = found;
}

static bool open_within(parser_t *p, tag_id tag, tag_id stop_at) {
    for (int i = p->depth - 1; i >= 0; i--) {
        if (p->stack[i]->tag == tag) return true;
        if (p->stack[i]->tag == stop_at) return false;
    }
    return false;
}

/* What an opening tag implies about what is already open.  These are the rules
 * that let a page write a list without ever closing an item. */
static void close_implied(parser_t *p, tag_id tag) {
    switch (tag) {
    case TAG_LI:
        /* A new <li> closes the previous item, but only within the SAME list.
         * Walk out to the nearest list container: an open <li> before any
         * <ul>/<ol> means a sibling to close; hitting the container first
         * means we are the first item of a nested list, whose own <li> must
         * stay open (it is what contains us).  The old code ran two checks
         * that each stopped at only one container type and OR'd them, so the
         * <ol> check walked straight past an enclosing <ul> and closed the
         * item that held the nested list. */
        for (int i = p->depth - 1; i >= 0; i--) {
            tag_id t = p->stack[i]->tag;
            if (t == TAG_LI) { close_to(p, TAG_LI); break; }
            if (t == TAG_UL || t == TAG_OL) break;
        }
        break;
    case TAG_DT: case TAG_DD:
        if (open_within(p, TAG_DT, TAG_DL)) close_to(p, TAG_DT);
        if (open_within(p, TAG_DD, TAG_DL)) close_to(p, TAG_DD);
        break;
    case TAG_TR:
        if (open_within(p, TAG_TD, TAG_TABLE)) close_to(p, TAG_TD);
        if (open_within(p, TAG_TH, TAG_TABLE)) close_to(p, TAG_TH);
        if (open_within(p, TAG_TR, TAG_TABLE)) close_to(p, TAG_TR);
        break;
    case TAG_TD: case TAG_TH:
        if (open_within(p, TAG_TD, TAG_TR)) close_to(p, TAG_TD);
        if (open_within(p, TAG_TH, TAG_TR)) close_to(p, TAG_TH);
        break;
    case TAG_OPTION:
        if (open_within(p, TAG_OPTION, TAG_SELECT)) close_to(p, TAG_OPTION);
        break;
    case TAG_THEAD: case TAG_TBODY: case TAG_TFOOT:
        if (open_within(p, TAG_TD, TAG_TABLE)) close_to(p, TAG_TD);
        if (open_within(p, TAG_TH, TAG_TABLE)) close_to(p, TAG_TH);
        if (open_within(p, TAG_TR, TAG_TABLE)) close_to(p, TAG_TR);
        break;
    default:
        break;
    }

    /* A paragraph cannot contain a block, so any block tag ends it. */
    if (is_block(tag)) {
        for (int i = p->depth - 1; i >= 0; i--) {
            if (p->stack[i]->tag == TAG_P) { p->depth = i; break; }
            if (is_block(p->stack[i]->tag) && p->stack[i]->tag != TAG_P) break;
        }
    }
}

static void add_text(parser_t *p, const char *text, size_t len, bool decode) {
    if (!len) return;

    dom_node_t *parent = current(p);
    if (!parent) return;

    char *buffer = doc_alloc(p->doc, len + 1);
    if (!buffer) return;

    size_t out = 0;
    for (size_t i = 0; i < len; ) {
        if (decode && text[i] == '&') {
            int decoded = 0;
            size_t used = decode_entity(text + i, len - i, &decoded);
            if (used) {
                buffer[out++] = (char)decoded;
                i += used;
                continue;
            }
        }
        /* Raw UTF-8 in the page text: decode the code point and fold it to the
         * font's byte layout, exactly as an entity is - so a literal em dash or
         * accented letter in the source reads as a real glyph, not the raw UTF-8
         * bytes shown one garbage glyph each. */
        unsigned char b = (unsigned char)text[i];
        if (decode && b >= 0x80) {
            unsigned cp; int extra;
            if      ((b & 0xE0) == 0xC0) { cp = b & 0x1F; extra = 1; }
            else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; extra = 2; }
            else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; extra = 3; }
            else { buffer[out++] = '?'; i++; continue; }
            size_t j = i + 1;
            bool ok = true;
            for (int k = 0; k < extra; k++) {
                if (j >= len || ((unsigned char)text[j] & 0xC0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | ((unsigned char)text[j] & 0x3F);
                j++;
            }
            if (!ok) { buffer[out++] = '?'; i++; continue; }
            buffer[out++] = (char)fold_to_byte(cp);
            i = j;
            continue;
        }
        buffer[out++] = text[i++];
    }
    buffer[out] = 0;
    if (!out) return;

    dom_node_t *node = doc_alloc(p->doc, sizeof *node);
    if (!node) return;
    node->kind = NODE_TEXT;
    node->text = buffer;
    node->name = "#text";
    attach(parent, node);
    p->doc->node_count++;
}

static bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':' || c == '.';
}

static void add_attribute(parser_t *p, dom_node_t *node,
                          const char *name, size_t name_len,
                          const char *value, size_t value_len) {
    attr_t *attr = doc_alloc(p->doc, sizeof *attr);
    if (!attr) return;

    char *lower = doc_alloc(p->doc, name_len + 1);
    if (!lower) return;
    for (size_t i = 0; i < name_len; i++) {
        char c = name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    lower[name_len] = 0;
    attr->name = lower;

    /* Attribute values carry references too - a link with an ampersand in its
     * query string is written that way in every page. */
    char *decoded = doc_alloc(p->doc, value_len + 1);
    if (!decoded) return;
    size_t out = 0;
    for (size_t i = 0; i < value_len; ) {
        if (value[i] == '&') {
            int code = 0;
            size_t used = decode_entity(value + i, value_len - i, &code);
            if (used) { decoded[out++] = (char)code; i += used; continue; }
        }
        decoded[out++] = value[i++];
    }
    decoded[out] = 0;
    attr->value = decoded;

    /* Kept in source order, which is what the cascade expects. */
    attr->next = NULL;
    if (!node->attrs) node->attrs = attr;
    else {
        attr_t *last = node->attrs;
        while (last->next) last = last->next;
        last->next = attr;
    }
}

/* Read the attributes of an opening tag.  On return `at` is just past the
 * closing angle bracket, and `self_closing` says whether it ended with a
 * slash. */
static void read_attributes(parser_t *p, dom_node_t *node, bool *self_closing) {
    *self_closing = false;

    for (;;) {
        while (p->at < p->len && (p->in[p->at] == ' ' || p->in[p->at] == '\t' ||
                                  p->in[p->at] == '\n' || p->in[p->at] == '\r'))
            p->at++;
        if (p->at >= p->len) return;

        if (p->in[p->at] == '>') { p->at++; return; }
        if (p->in[p->at] == '/') {
            *self_closing = true;
            p->at++;
            continue;
        }

        size_t name_start = p->at;
        while (p->at < p->len && name_char(p->in[p->at])) p->at++;
        if (p->at == name_start) { p->at++; continue; }   /* something odd */
        size_t name_len = p->at - name_start;

        while (p->at < p->len && (p->in[p->at] == ' ' || p->in[p->at] == '\t' ||
                                  p->in[p->at] == '\n' || p->in[p->at] == '\r'))
            p->at++;

        if (p->at >= p->len || p->in[p->at] != '=') {
            /* A bare attribute, like `disabled`. */
            add_attribute(p, node, p->in + name_start, name_len, "", 0);
            continue;
        }
        p->at++;

        while (p->at < p->len && (p->in[p->at] == ' ' || p->in[p->at] == '\t' ||
                                  p->in[p->at] == '\n' || p->in[p->at] == '\r'))
            p->at++;
        if (p->at >= p->len) return;

        size_t value_start, value_len;
        char quote = p->in[p->at];
        if (quote == '"' || quote == '\'') {
            p->at++;
            value_start = p->at;
            while (p->at < p->len && p->in[p->at] != quote) p->at++;
            value_len = p->at - value_start;
            if (p->at < p->len) p->at++;
        } else {
            value_start = p->at;
            while (p->at < p->len && p->in[p->at] != '>' && p->in[p->at] != ' ' &&
                   p->in[p->at] != '\t' && p->in[p->at] != '\n' && p->in[p->at] != '\r')
                p->at++;
            value_len = p->at - value_start;
        }
        add_attribute(p, node, p->in + name_start, name_len,
                      p->in + value_start, value_len);
    }
}

/* The contents of a script or style element: everything up to its own closing
 * tag, taken as text. */
static void read_raw_text(parser_t *p, dom_node_t *node) {
    const char *name = node->name;
    size_t name_len = strlen(name);
    size_t start = p->at;

    while (p->at < p->len) {
        if (p->in[p->at] == '<' && p->at + 2 + name_len <= p->len &&
            p->in[p->at + 1] == '/' &&
            !strncasecmp(p->in + p->at + 2, name, name_len)) {
            char after = (p->at + 2 + name_len < p->len)
                       ? p->in[p->at + 2 + name_len] : '>';
            if (after == '>' || after == ' ' || after == '\t' ||
                after == '\n' || after == '\r' || after == '/') break;
        }
        p->at++;
    }

    size_t len = p->at - start;

    /* A stylesheet inside the page is the one raw text worth keeping: it is
     * collected here so the style pass has it all in one place. */
    if (node->tag == TAG_STYLE) {
        document_t *doc = p->doc;
        if (doc->css_len + len + 2 > doc->css_cap) {
            size_t bigger = doc->css_cap ? doc->css_cap * 2 : 8192;
            while (bigger < doc->css_len + len + 2) bigger *= 2;
            char *grown = realloc(doc->css, bigger);
            if (grown) { doc->css = grown; doc->css_cap = bigger; }
        }
        if (doc->css && doc->css_len + len + 2 <= doc->css_cap) {
            memcpy(doc->css + doc->css_len, p->in + start, len);
            doc->css_len += len;
            doc->css[doc->css_len++] = '\n';
            doc->css[doc->css_len] = 0;
        }
    } else if (node->tag != TAG_SCRIPT) {
        add_text(p, p->in + start, len, false);
    }

    /* Step over the closing tag. */
    while (p->at < p->len && p->in[p->at] != '>') p->at++;
    if (p->at < p->len) p->at++;
}

/* ------------------------------------------------------------------ parsing */

document_t *html_parse(const char *html, size_t len) {
    document_t *doc = calloc(1, sizeof *doc);
    if (!doc) return NULL;

    parser_t parser;
    memset(&parser, 0, sizeof parser);
    parser.doc = doc;
    parser.in = html;
    parser.len = len;

    parser_t *p = &parser;

    /* The two elements every document has, whether or not the page wrote
     * them.  Building them up front means nothing below has to ask whether
     * there is somewhere to put what it just read. */
    dom_node_t *html_element = new_element(p, "html", 4);
    push(p, html_element);
    dom_node_t *body = new_element(p, "body", 4);
    push(p, body);
    doc->body = body;

    while (p->at < p->len) {
        if (p->in[p->at] != '<') {
            size_t start = p->at;
            while (p->at < p->len && p->in[p->at] != '<') p->at++;
            add_text(p, p->in + start, p->at - start, true);
            continue;
        }

        /* A "<" that is not the start of a tag is just a character, which
         * happens in text about markup and in careless pages. */
        if (p->at + 1 >= p->len) {
            add_text(p, p->in + p->at, 1, false);
            break;
        }

        char next = p->in[p->at + 1];

        if (next == '!') {
            if (p->at + 4 <= p->len && !strncmp(p->in + p->at, "<!--", 4)) {
                p->at += 4;
                while (p->at + 3 <= p->len && strncmp(p->in + p->at, "-->", 3))
                    p->at++;
                p->at = (p->at + 3 <= p->len) ? p->at + 3 : p->len;
            } else {
                /* A doctype or a declaration; nothing here acts on either. */
                while (p->at < p->len && p->in[p->at] != '>') p->at++;
                if (p->at < p->len) p->at++;
            }
            continue;
        }

        if (next == '?') {
            while (p->at < p->len && p->in[p->at] != '>') p->at++;
            if (p->at < p->len) p->at++;
            continue;
        }

        if (next == '/') {
            size_t name_start = p->at + 2;
            size_t at = name_start;
            while (at < p->len && name_char(p->in[at])) at++;
            size_t name_len = at - name_start;
            while (at < p->len && p->in[at] != '>') at++;
            p->at = (at < p->len) ? at + 1 : p->len;

            if (!name_len) continue;

            char lower[32];
            size_t copy = name_len < sizeof lower - 1 ? name_len : sizeof lower - 1;
            for (size_t i = 0; i < copy; i++) {
                char c = p->in[name_start + i];
                lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
            }
            lower[copy] = 0;

            tag_id tag = tag_from_name(lower);
            if (is_void(tag)) continue;

            /* Never let a closing tag unwind past the body: a page with a
             * stray </html> in the middle still has to keep going. */
            if (tag == TAG_HTML || tag == TAG_BODY) continue;
            close_to(p, tag);
            if (p->depth < 2) p->depth = 2;
            continue;
        }

        if (!name_char(next)) {
            add_text(p, p->in + p->at, 1, false);
            p->at++;
            continue;
        }

        /* An opening tag. */
        size_t name_start = p->at + 1;
        size_t at = name_start;
        while (at < p->len && name_char(p->in[at])) at++;
        size_t name_len = at - name_start;
        p->at = at;

        dom_node_t *node = new_element(p, p->in + name_start, name_len);
        if (!node) break;

        bool self_closing = false;
        read_attributes(p, node, &self_closing);

        /* An html or body the page wrote itself: its attributes are worth
         * keeping, but a second element is not. */
        if (node->tag == TAG_HTML) {
            for (attr_t *a = node->attrs; a; a = a->next)
                add_attribute(p, html_element, a->name, strlen(a->name),
                              a->value, strlen(a->value));
            continue;
        }
        if (node->tag == TAG_BODY) {
            for (attr_t *a = node->attrs; a; a = a->next)
                add_attribute(p, body, a->name, strlen(a->name),
                              a->value, strlen(a->value));
            p->depth = 2;
            continue;
        }
        if (node->tag == TAG_HEAD) continue;    /* its contents go where they land */

        close_implied(p, node->tag);
        push(p, node);

        if (node->tag == TAG_TITLE) {
            /* The title is text, and it belongs to the document rather than to
             * anything that gets drawn. */
            size_t start = p->at;
            while (p->at < p->len && p->in[p->at] != '<') p->at++;
            char buffer[256];
            size_t take = p->at - start;
            if (take > sizeof buffer - 1) take = sizeof buffer - 1;
            size_t out = 0;
            for (size_t i = 0; i < take; ) {
                if (p->in[start + i] == '&') {
                    int code = 0;
                    size_t used = decode_entity(p->in + start + i, take - i, &code);
                    if (used) { buffer[out++] = (char)code; i += used; continue; }
                }
                buffer[out++] = p->in[start + i++];
            }
            buffer[out] = 0;
            doc->title = doc_string(doc, buffer, out);
            while (p->at < p->len && p->in[p->at] != '>') p->at++;
            if (p->at < p->len) p->at++;
            p->depth--;
            continue;
        }

        if (node->tag == TAG_BASE) {
            const char *href = dom_attr(node, "href");
            if (href && *href) doc->base_href = href;
        }

        if (is_void(node->tag) || self_closing) {
            p->depth--;
            continue;
        }

        if (is_raw_text(node->tag)) {
            read_raw_text(p, node);
            p->depth--;
            continue;
        }
    }

    doc->root = html_element;
    if (!doc->title) doc->title = "";
    return doc;
}

void document_free(document_t *doc) {
    if (!doc) return;
    arena_block_t *block = doc->arena;
    while (block) {
        arena_block_t *next = block->next;
        free(block);
        block = next;
    }
    if (doc->css) free(doc->css);
    free(doc);
}

/* ------------------------------------------------------------ getting about */

const char *dom_attr(const dom_node_t *node, const char *name) {
    if (!node) return NULL;
    for (attr_t *a = node->attrs; a; a = a->next)
        if (!strcmp(a->name, name)) return a->value;
    return NULL;
}

bool dom_has_class(const dom_node_t *node, const char *class_name) {
    const char *classes = dom_attr(node, "class");
    if (!classes) return false;
    size_t want = strlen(class_name);

    const char *at = classes;
    while (*at) {
        while (*at == ' ' || *at == '\t' || *at == '\n') at++;
        const char *start = at;
        while (*at && *at != ' ' && *at != '\t' && *at != '\n') at++;
        if ((size_t)(at - start) == want && !strncmp(start, class_name, want))
            return true;
    }
    return false;
}

dom_node_t *dom_next(const dom_node_t *node, const dom_node_t *root) {
    if (!node) return NULL;
    if (node->first_child) return node->first_child;

    const dom_node_t *at = node;
    while (at && at != root) {
        if (at->next) return at->next;
        at = at->parent;
    }
    return NULL;
}

void dom_text_content(const dom_node_t *node, char *out, size_t cap) {
    size_t at = 0;
    out[0] = 0;
    if (!node) return;

    const dom_node_t *walk = node;
    while (walk) {
        if (walk->kind == NODE_TEXT && walk->text) {
            for (const char *c = walk->text; *c && at + 1 < cap; c++) {
                /* Runs of blank space become one, which is what makes a title
                 * spread over three lines of source read as one line. */
                if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') {
                    if (at > 0 && out[at - 1] != ' ') out[at++] = ' ';
                } else {
                    out[at++] = *c;
                }
            }
        }
        walk = dom_next(walk, node);
    }
    while (at > 0 && out[at - 1] == ' ') at--;
    out[at] = 0;
}
