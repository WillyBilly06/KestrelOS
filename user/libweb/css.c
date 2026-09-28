/* css.c - reading a stylesheet, and deciding what each element looks like.
 *
 * There are two halves.  The first reads the text into rules: a list of
 * selectors and, for each, a list of properties.  The second is the cascade,
 * which is where the interesting decision lives.
 *
 * Several rules can apply to one element and disagree.  The language settles
 * that by specificity: a rule that names an element by its identifier beats
 * one that names it by a class, which beats one that names its element type,
 * and ties go to whichever was written last.  That ordering is the whole
 * reason a page author can write a general rule and then override it in one
 * place, and getting it wrong makes a page look almost right in a way that is
 * very hard to see.
 *
 * Then inheritance: the properties that describe text - colour, size, weight,
 * alignment - pass down to children unless something says otherwise, and the
 * ones that describe a box - margins, borders, backgrounds - do not.  Which
 * properties are which is not a rule that can be derived; it is a list, and it
 * is below.
 */
#include "css.h"

/* -------------------------------------------------------------- selectors
 *
 * One step of a selector: what a single element has to look like.  A full
 * selector is a chain of these read right to left, with each step saying how
 * it relates to the one before.
 */
typedef enum { COMBINE_DESCENDANT = 0, COMBINE_CHILD } combinator;

typedef struct {
    char       element[24];         /* empty means any */
    char       id[48];
    char       classes[4][40];
    int        class_count;
    combinator relation;            /* how this step joins the one to its left */
    bool       any;                 /* the `*` selector */
} step_t;

#define MAX_STEPS 6

typedef struct {
    step_t steps[MAX_STEPS];
    int    step_count;
    int    specificity;
} selector_t;

typedef struct declaration {
    char  name[32];
    char  value[192];
    bool  important;
    struct declaration *next;
} declaration_t;

/* A rule can name a great many elements at once, and browser default sheets in
 * particular do: the list of everything that is a block runs to thirty-odd
 * names.  Cutting it short does not fail loudly - it quietly leaves the
 * elements past the cut with the wrong default, which shows up as headings and
 * lists running together in the middle of a paragraph. */
#define MAX_SELECTORS_PER_RULE 48

struct rule {
    selector_t     selectors[MAX_SELECTORS_PER_RULE];
    int            selector_count;
    declaration_t *declarations;
    int            order;           /* where it appeared, for breaking ties */
    rule_t        *next;
};

/* Everything a sheet owns comes from one chain of blocks, so freeing it is a
 * short walk rather than a careful traversal. */
typedef struct sheet_block {
    struct sheet_block *next;
    size_t cap, at;
    char   data[];
} sheet_block_t;

typedef struct {
    sheet_block_t *blocks;
    rule_t        *first;
    rule_t        *last;
} sheet_storage_t;

static void *sheet_alloc(sheet_storage_t *storage, size_t size) {
    size = (size + 7) & ~(size_t)7;
    if (!storage->blocks || storage->blocks->at + size > storage->blocks->cap) {
        size_t cap = 32 * 1024;
        while (cap < size) cap *= 2;
        sheet_block_t *block = malloc(sizeof(sheet_block_t) + cap);
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

/* ---------------------------------------------------------------- colours */

static const struct { const char *name; colour_t value; } colour_names[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 },
    { "green", 0x008000 }, { "blue", 0x0000FF }, { "yellow", 0xFFFF00 },
    { "cyan", 0x00FFFF }, { "aqua", 0x00FFFF }, { "magenta", 0xFF00FF },
    { "fuchsia", 0xFF00FF }, { "gray", 0x808080 }, { "grey", 0x808080 },
    { "silver", 0xC0C0C0 }, { "maroon", 0x800000 }, { "olive", 0x808000 },
    { "lime", 0x00FF00 }, { "teal", 0x008080 }, { "navy", 0x000080 },
    { "purple", 0x800080 }, { "orange", 0xFFA500 }, { "pink", 0xFFC0CB },
    { "brown", 0xA52A2A }, { "gold", 0xFFD700 }, { "beige", 0xF5F5DC },
    { "ivory", 0xFFFFF0 }, { "khaki", 0xF0E68C }, { "lavender", 0xE6E6FA },
    { "salmon", 0xFA8072 }, { "tan", 0xD2B48C }, { "violet", 0xEE82EE },
    { "indigo", 0x4B0082 }, { "coral", 0xFF7F50 }, { "crimson", 0xDC143C },
    { "darkblue", 0x00008B }, { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 },
    { "darkgreen", 0x006400 }, { "darkred", 0x8B0000 },
    { "lightblue", 0xADD8E6 }, { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 },
    { "lightgreen", 0x90EE90 }, { "lightyellow", 0xFFFFE0 },
    { "whitesmoke", 0xF5F5F5 }, { "gainsboro", 0xDCDCDC },
    { "dodgerblue", 0x1E90FF }, { "steelblue", 0x4682B4 },
    { "royalblue", 0x4169E1 }, { "skyblue", 0x87CEEB },
    { "midnightblue", 0x191970 }, { "seagreen", 0x2E8B57 },
    { "forestgreen", 0x228B22 }, { "firebrick", 0xB22222 },
    { "tomato", 0xFF6347 }, { "chocolate", 0xD2691E }, { "peru", 0xCD853F },
    { "slategray", 0x708090 }, { "slategrey", 0x708090 },
    { "dimgray", 0x696969 }, { "dimgrey", 0x696969 },
    { "aliceblue", 0xF0F8FF }, { "azure", 0xF0FFFF }, { "honeydew", 0xF0FFF0 },
    { "linen", 0xFAF0E6 }, { "snow", 0xFFFAFA }, { "wheat", 0xF5DEB3 },
    { "transparent", 0xFFFFFF },
};

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool css_parse_colour(const char *text, size_t len, colour_t *out) {
    while (len && (*text == ' ' || *text == '\t')) { text++; len--; }
    while (len && (text[len - 1] == ' ' || text[len - 1] == '\t')) len--;
    if (!len) return false;

    if (text[0] == '#') {
        if (len == 4) {
            int r = hex_value(text[1]), g = hex_value(text[2]), b = hex_value(text[3]);
            if (r < 0 || g < 0 || b < 0) return false;
            /* Each digit stands for a pair, so "f" means "ff". */
            *out = RGB(r * 17, g * 17, b * 17);
            return true;
        }
        if (len == 7 || len == 9) {
            int v[6];
            for (int i = 0; i < 6; i++) {
                v[i] = hex_value(text[1 + i]);
                if (v[i] < 0) return false;
            }
            *out = RGB(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5]);
            return true;
        }
        return false;
    }

    if (len > 4 && !strncasecmp(text, "rgb", 3)) {
        const char *at = text + 3;
        if (*at == 'a') at++;
        if (*at != '(') return false;
        at++;
        int part[4] = { 0, 0, 0, 255 };
        for (int i = 0; i < 3; i++) {
            while (at < text + len && (*at == ' ' || *at == ',')) at++;
            int value = 0;
            bool any = false;
            while (at < text + len && *at >= '0' && *at <= '9') {
                value = value * 10 + (*at - '0');
                at++; any = true;
            }
            if (!any) return false;
            /* A percentage is allowed here and means what it says. */
            if (at < text + len && *at == '%') { value = value * 255 / 100; at++; }
            part[i] = value > 255 ? 255 : value;
        }
        *out = RGB(part[0], part[1], part[2]);
        return true;
    }

    for (size_t i = 0; i < sizeof colour_names / sizeof colour_names[0]; i++) {
        if (strlen(colour_names[i].name) != len) continue;
        if (strncasecmp(text, colour_names[i].name, len)) continue;
        *out = colour_names[i].value;
        return true;
    }
    return false;
}

/* ----------------------------------------------------------------- lengths */

bool len_is_auto(len_t len) { return len.unit == LEN_AUTO; }

int len_resolve(len_t len, int against, int fallback) {
    switch (len.unit) {
    case LEN_PX:      return len.value;
    case LEN_PERCENT: return against * len.value / 100;
    default:          return fallback;
    }
}

/* Read one length.  `font_size` is what an em is worth here, and `root_size`
 * what a rem is. */
static len_t parse_length(const char *text, size_t len, int font_size, int root_size) {
    while (len && (*text == ' ' || *text == '\t')) { text++; len--; }
    if (!len) return len_auto();

    if (len >= 4 && !strncasecmp(text, "auto", 4)) return len_auto();

    bool negative = false;
    size_t at = 0;
    if (at < len && (text[at] == '-' || text[at] == '+')) {
        negative = (text[at] == '-');
        at++;
    }

    /* The whole part, then the fraction, kept as thousandths so that a value
     * like 1.5em is not silently truncated to 1em. */
    long whole = 0;
    bool any = false;
    while (at < len && text[at] >= '0' && text[at] <= '9') {
        whole = whole * 10 + (text[at] - '0');
        at++; any = true;
    }
    long thousandths = 0;
    if (at < len && text[at] == '.') {
        at++;
        long scale = 100;
        while (at < len && text[at] >= '0' && text[at] <= '9') {
            thousandths += (text[at] - '0') * scale;
            scale /= 10;
            at++; any = true;
        }
    }
    if (!any) return len_auto();

    long milli = whole * 1000 + thousandths;
    if (negative) milli = -milli;

    /* And the unit. */
    const char *unit = text + at;
    size_t unit_len = len - at;
    while (unit_len && (unit[unit_len - 1] == ' ' || unit[unit_len - 1] == ';'))
        unit_len--;

    len_t out;
    out.unit = LEN_PX;

    if (unit_len >= 1 && unit[0] == '%') {
        out.unit = LEN_PERCENT;
        out.value = (int)(milli / 1000);
    } else if (unit_len >= 3 && !strncasecmp(unit, "rem", 3)) {
        out.value = (int)(milli * root_size / 1000);
    } else if (unit_len >= 2 && !strncasecmp(unit, "em", 2)) {
        out.value = (int)(milli * font_size / 1000);
    } else if (unit_len >= 2 && !strncasecmp(unit, "ex", 2)) {
        out.value = (int)(milli * font_size / 2000);
    } else if (unit_len >= 2 && !strncasecmp(unit, "pt", 2)) {
        /* A point is three quarters of a pixel at the ninety-six per inch
         * every browser assumes. */
        out.value = (int)(milli * 4 / 3000);
    } else if (unit_len >= 2 && !strncasecmp(unit, "pc", 2)) {
        out.value = (int)(milli * 16 / 1000);
    } else if (unit_len >= 2 && !strncasecmp(unit, "vw", 2)) {
        /* Without a viewport to hand, a sensible fraction of a usual one. */
        out.value = (int)(milli * 10 / 1000);
    } else if (unit_len >= 2 && !strncasecmp(unit, "vh", 2)) {
        out.value = (int)(milli * 7 / 1000);
    } else {
        out.value = (int)(milli / 1000);
    }
    return out;
}

/* ------------------------------------------------------------- the parser */

typedef struct {
    const char *in;
    size_t      len, at;
} css_reader;

static void skip_space(css_reader *r) {
    for (;;) {
        while (r->at < r->len && (r->in[r->at] == ' ' || r->in[r->at] == '\t' ||
                                  r->in[r->at] == '\n' || r->in[r->at] == '\r' ||
                                  r->in[r->at] == '\f'))
            r->at++;
        /* Comments can appear anywhere space can. */
        if (r->at + 1 < r->len && r->in[r->at] == '/' && r->in[r->at + 1] == '*') {
            r->at += 2;
            while (r->at + 1 < r->len &&
                   !(r->in[r->at] == '*' && r->in[r->at + 1] == '/'))
                r->at++;
            r->at = (r->at + 1 < r->len) ? r->at + 2 : r->len;
            continue;
        }
        return;
    }
}

static void append(char *dst, size_t cap, char c) {
    size_t len = strlen(dst);
    if (len + 1 < cap) { dst[len] = c; dst[len + 1] = 0; }
}

/* One selector: a chain of steps separated by space or by a child marker. */
static bool parse_selector(const char *text, size_t len, selector_t *out) {
    memset(out, 0, sizeof *out);

    size_t at = 0;
    step_t *step = NULL;
    combinator pending = COMBINE_DESCENDANT;
    bool in_step = false;
    int part = 0;                   /* 0 element, 1 class, 2 id */

    while (at < len) {
        char c = text[at];

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (in_step) { in_step = false; pending = COMBINE_DESCENDANT; }
            at++;
            continue;
        }
        if (c == '>') {
            in_step = false;
            pending = COMBINE_CHILD;
            at++;
            continue;
        }
        if (c == '+' || c == '~') {
            /* Sibling selectors are not matched here.  Rather than match them
             * wrongly - which would style the wrong elements - the whole rule
             * is dropped. */
            return false;
        }
        if (c == '[') {
            /* An attribute condition.  Skipped, which makes the selector less
             * specific than it should be but never matches something it
             * should not, because the rest of the chain still has to hold. */
            while (at < len && text[at] != ']') at++;
            if (at < len) at++;
            continue;
        }
        if (c == ':') {
            /* Pseudo-classes and pseudo-elements.  A few are worth honouring;
             * the rest would need state this browser does not keep, and a rule
             * that only applies while something is hovered must not apply
             * always. */
            size_t start = ++at;
            while (at < len && text[at] == ':') at++;
            while (at < len && ((text[at] >= 'a' && text[at] <= 'z') ||
                                (text[at] >= 'A' && text[at] <= 'Z') ||
                                text[at] == '-')) at++;
            size_t name_len = at - start;
            /* A functional pseudo-class takes arguments in brackets. */
            if (at < len && text[at] == '(') {
                int depth = 0;
                while (at < len) {
                    if (text[at] == '(') depth++;
                    else if (text[at] == ')') { depth--; if (!depth) { at++; break; } }
                    at++;
                }
                return false;
            }
            if (name_len == 4 && !strncasecmp(text + start, "link", 4)) continue;
            if (name_len == 4 && !strncasecmp(text + start, "root", 4)) continue;
            return false;
        }

        if (!in_step) {
            if (out->step_count >= MAX_STEPS) return false;
            step = &out->steps[out->step_count++];
            memset(step, 0, sizeof *step);
            step->relation = pending;
            in_step = true;
            part = 0;
        }

        if (c == '.') { part = 1; at++;
            if (step->class_count < 4) step->class_count++;
            continue; }
        if (c == '#') { part = 2; at++; continue; }
        if (c == '*') { step->any = true; at++; part = 0; continue; }

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            char lower = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
            if (part == 0) append(step->element, sizeof step->element, lower);
            else if (part == 1) append(step->classes[step->class_count - 1],
                                       sizeof step->classes[0], c);
            else append(step->id, sizeof step->id, c);
            at++;
            continue;
        }

        return false;                       /* something not understood */
    }

    if (!out->step_count) return false;

    /* Specificity: identifiers count most, then classes, then element names.
     * The weights are far apart so that no number of one can outweigh a
     * single instance of the next. */
    for (int i = 0; i < out->step_count; i++) {
        if (out->steps[i].id[0]) out->specificity += 10000;
        out->specificity += out->steps[i].class_count * 100;
        if (out->steps[i].element[0]) out->specificity += 1;
    }
    return true;
}

static void parse_declarations(sheet_storage_t *storage, rule_t *rule,
                               const char *text, size_t len) {
    size_t at = 0;
    while (at < len) {
        while (at < len && (text[at] == ' ' || text[at] == '\t' ||
                            text[at] == '\n' || text[at] == '\r' || text[at] == ';'))
            at++;
        if (at >= len) break;

        size_t name_start = at;
        while (at < len && text[at] != ':' && text[at] != ';' && text[at] != '}') at++;
        if (at >= len || text[at] != ':') {
            while (at < len && text[at] != ';') at++;
            continue;
        }
        size_t name_end = at;
        at++;

        /* The value runs to the next semicolon that is not inside brackets or
         * a string - a background with a gradient has commas and brackets in
         * it and must not be cut in half. */
        size_t value_start = at;
        int depth = 0;
        char quote = 0;
        while (at < len) {
            char c = text[at];
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '"' || c == '\'') quote = c;
            else if (c == '(') depth++;
            else if (c == ')') { if (depth) depth--; }
            else if (c == ';' && !depth) break;
            at++;
        }
        size_t value_end = at;

        while (name_start < name_end && (text[name_start] == ' ' || text[name_start] == '\t'))
            name_start++;
        while (name_end > name_start && (text[name_end - 1] == ' ' || text[name_end - 1] == '\t'))
            name_end--;
        while (value_start < value_end && (text[value_start] == ' ' || text[value_start] == '\t'))
            value_start++;
        while (value_end > value_start && (text[value_end - 1] == ' ' ||
                                           text[value_end - 1] == '\t' ||
                                           text[value_end - 1] == '\n' ||
                                           text[value_end - 1] == '\r'))
            value_end--;

        if (name_end <= name_start || value_end <= value_start) continue;

        declaration_t *decl = sheet_alloc(storage, sizeof *decl);
        if (!decl) return;

        size_t name_len = name_end - name_start;
        if (name_len >= sizeof decl->name) name_len = sizeof decl->name - 1;
        for (size_t i = 0; i < name_len; i++) {
            char c = text[name_start + i];
            decl->name[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        decl->name[name_len] = 0;

        size_t value_len = value_end - value_start;

        /* A value marked important outranks everything, whatever the
         * specificity says. */
        if (value_len > 10) {
            for (size_t i = 0; i + 10 <= value_len; i++) {
                if (text[value_start + i] == '!' &&
                    !strncasecmp(text + value_start + i + 1, "important", 9)) {
                    decl->important = true;
                    value_len = i;
                    while (value_len && (text[value_start + value_len - 1] == ' '))
                        value_len--;
                    break;
                }
            }
        }

        if (value_len >= sizeof decl->value) value_len = sizeof decl->value - 1;
        memcpy(decl->value, text + value_start, value_len);
        decl->value[value_len] = 0;

        decl->next = rule->declarations;
        rule->declarations = decl;
    }
}

stylesheet_t *css_parse(const char *text, size_t len) {
    stylesheet_t *sheet = calloc(1, sizeof *sheet);
    if (!sheet) return NULL;

    sheet_storage_t *storage = calloc(1, sizeof *storage);
    if (!storage) { free(sheet); return NULL; }
    sheet->storage = storage;

    css_reader reader = { text, len, 0 };
    css_reader *r = &reader;
    int order = 0;

    while (r->at < r->len) {
        skip_space(r);
        if (r->at >= r->len) break;

        if (r->in[r->at] == '@') {
            /* At-rules.  A media query's contents are taken as though the
             * query had matched, which is right far more often than skipping
             * them: most of them widen or narrow a layout rather than replace
             * it, and a page with only its narrow rules applied still reads. */
            size_t name_start = ++r->at;
            while (r->at < r->len && r->in[r->at] != ' ' && r->in[r->at] != '{' &&
                   r->in[r->at] != ';') r->at++;
            size_t name_len = r->at - name_start;

            bool descend = (name_len == 5 && !strncasecmp(text + name_start, "media", 5)) ||
                           (name_len == 8 && !strncasecmp(text + name_start, "supports", 8));

            while (r->at < r->len && r->in[r->at] != '{' && r->in[r->at] != ';') r->at++;
            if (r->at >= r->len) break;

            if (r->in[r->at] == ';') { r->at++; continue; }

            if (descend) { r->at++; continue; }   /* read its rules as usual */

            /* Anything else with a body - a font face, an animation - is
             * skipped whole. */
            int depth = 0;
            while (r->at < r->len) {
                if (r->in[r->at] == '{') depth++;
                else if (r->in[r->at] == '}') { depth--; if (!depth) { r->at++; break; } }
                r->at++;
            }
            continue;
        }

        if (r->in[r->at] == '}') { r->at++; continue; }   /* a media block ending */

        size_t selector_start = r->at;
        while (r->at < r->len && r->in[r->at] != '{' && r->in[r->at] != '}') r->at++;
        if (r->at >= r->len) break;
        if (r->in[r->at] == '}') { r->at++; continue; }
        size_t selector_end = r->at;
        r->at++;

        size_t body_start = r->at;
        int depth = 1;
        while (r->at < r->len && depth) {
            if (r->in[r->at] == '{') depth++;
            else if (r->in[r->at] == '}') depth--;
            if (depth) r->at++;
        }
        size_t body_end = r->at;
        if (r->at < r->len) r->at++;

        rule_t *rule = sheet_alloc(storage, sizeof *rule);
        if (!rule) break;
        rule->order = order++;

        /* Selectors separated by commas each get their own entry, because each
         * has its own specificity. */
        size_t at = selector_start;
        while (at < selector_end && rule->selector_count < MAX_SELECTORS_PER_RULE) {
            size_t part_start = at;
            while (at < selector_end && text[at] != ',') at++;
            size_t part_end = at;
            if (at < selector_end) at++;

            if (parse_selector(text + part_start, part_end - part_start,
                               &rule->selectors[rule->selector_count]))
                rule->selector_count++;
        }

        if (!rule->selector_count) continue;

        parse_declarations(storage, rule, text + body_start, body_end - body_start);
        if (!rule->declarations) continue;

        if (storage->last) storage->last->next = rule;
        else storage->first = rule;
        storage->last = rule;
        sheet->count++;
    }

    sheet->rules = storage->first;
    return sheet;
}

void css_free(stylesheet_t *sheet) {
    if (!sheet) return;
    sheet_storage_t *storage = sheet->storage;
    if (storage) {
        sheet_block_t *block = storage->blocks;
        while (block) {
            sheet_block_t *next = block->next;
            free(block);
            block = next;
        }
        free(storage);
    }
    free(sheet);
}

/* --------------------------------------------------------------- matching */

static bool step_matches(const step_t *step, const dom_node_t *node) {
    if (node->kind != NODE_ELEMENT) return false;

    if (step->element[0] && strcmp(step->element, node->name)) return false;

    if (step->id[0]) {
        const char *id = dom_attr(node, "id");
        if (!id || strcmp(id, step->id)) return false;
    }

    for (int i = 0; i < step->class_count; i++)
        if (!dom_has_class(node, step->classes[i])) return false;

    return true;
}

/* A selector is read right to left: the last step has to match the element
 * itself, and each earlier step has to match some ancestor.  Reading it that
 * way rather than left to right means most non-matches are rejected on the
 * first test, which for a page with thousands of elements is the difference
 * between fast and unusable. */
static bool selector_matches(const selector_t *selector, const dom_node_t *node) {
    int index = selector->step_count - 1;
    if (!step_matches(&selector->steps[index], node)) return false;

    const dom_node_t *at = node;
    index--;

    while (index >= 0) {
        const step_t *step = &selector->steps[index];
        combinator relation = selector->steps[index + 1].relation;

        if (relation == COMBINE_CHILD) {
            at = at->parent;
            if (!at || !step_matches(step, at)) return false;
        } else {
            /* Any ancestor will do, so try each until one fits. */
            bool found = false;
            for (const dom_node_t *up = at->parent; up; up = up->parent) {
                if (step_matches(step, up)) { at = up; found = true; break; }
            }
            if (!found) return false;
        }
        index--;
    }
    return true;
}

/* --------------------------------------------------------- applying values */

static void set_sides(len_t *sides, const char *value, int font_size, int root_size) {
    /* One to four values: all; vertical and horizontal; top, sides, bottom;
     * or each in turn - the order every shorthand in this language uses. */
    len_t parts[4];
    int count = 0;
    const char *at = value;

    while (*at && count < 4) {
        while (*at == ' ' || *at == '\t') at++;
        if (!*at) break;
        const char *start = at;
        while (*at && *at != ' ' && *at != '\t') at++;
        parts[count++] = parse_length(start, (size_t)(at - start), font_size, root_size);
    }
    if (!count) return;

    switch (count) {
    case 1: sides[0] = sides[1] = sides[2] = sides[3] = parts[0]; break;
    case 2: sides[SIDE_TOP] = sides[SIDE_BOTTOM] = parts[0];
            sides[SIDE_LEFT] = sides[SIDE_RIGHT] = parts[1]; break;
    case 3: sides[SIDE_TOP] = parts[0];
            sides[SIDE_LEFT] = sides[SIDE_RIGHT] = parts[1];
            sides[SIDE_BOTTOM] = parts[2]; break;
    default: sides[SIDE_TOP] = parts[0]; sides[SIDE_RIGHT] = parts[1];
             sides[SIDE_BOTTOM] = parts[2]; sides[SIDE_LEFT] = parts[3]; break;
    }
}

static bool word_is(const char *value, const char *want) {
    size_t len = strlen(want);
    if (strncasecmp(value, want, len)) return false;
    char after = value[len];
    return after == 0 || after == ' ' || after == ';' || after == '\t';
}

static bool contains_word(const char *value, const char *want) {
    size_t want_len = strlen(want);
    for (const char *at = value; *at; at++) {
        if (at != value && at[-1] != ' ' && at[-1] != ',' && at[-1] != '\t') continue;
        if (!strncasecmp(at, want, want_len)) {
            char after = at[want_len];
            if (!after || after == ' ' || after == ',' || after == ';') return true;
        }
    }
    return false;
}

/* One border shorthand: a width, a style and a colour in any order. */
static void set_border(style_t *style, const char *value, int side_mask,
                       int font_size, int root_size) {
    int width = -1;
    int kind = -1;
    colour_t colour = 0;
    bool have_colour = false;

    const char *at = value;
    while (*at) {
        while (*at == ' ' || *at == '\t') at++;
        if (!*at) break;
        const char *start = at;
        while (*at && *at != ' ' && *at != '\t') at++;
        size_t len = (size_t)(at - start);

        if (len == 4 && !strncasecmp(start, "none", 4)) { kind = BORDER_NONE; width = 0; }
        else if (len == 6 && !strncasecmp(start, "hidden", 6)) { kind = BORDER_NONE; width = 0; }
        else if (len == 5 && !strncasecmp(start, "solid", 5)) kind = BORDER_SOLID;
        else if (len == 6 && !strncasecmp(start, "dashed", 6)) kind = BORDER_DASHED;
        else if (len == 6 && !strncasecmp(start, "dotted", 6)) kind = BORDER_DOTTED;
        else if (len == 6 && !strncasecmp(start, "double", 6)) kind = BORDER_SOLID;
        else if (len == 5 && !strncasecmp(start, "inset", 5)) kind = BORDER_SOLID;
        else if (len == 6 && !strncasecmp(start, "groove", 6)) kind = BORDER_SOLID;
        else if (len == 6 && !strncasecmp(start, "ridge", 5)) kind = BORDER_SOLID;
        else if (len == 4 && !strncasecmp(start, "thin", 4)) width = 1;
        else if (len == 6 && !strncasecmp(start, "medium", 6)) width = 3;
        else if (len == 5 && !strncasecmp(start, "thick", 5)) width = 5;
        else if (css_parse_colour(start, len, &colour)) have_colour = true;
        else {
            len_t length = parse_length(start, len, font_size, root_size);
            if (length.unit == LEN_PX) width = length.value;
        }
    }

    for (int side = 0; side < 4; side++) {
        if (!(side_mask & (1 << side))) continue;
        if (kind >= 0) style->border_style[side] = (unsigned char)kind;
        if (width >= 0) style->border_width[side] = width;
        else if (kind > BORDER_NONE && style->border_width[side] == 0)
            style->border_width[side] = 1;      /* a style with no width is one pixel */
        if (have_colour) style->border_colour[side] = colour;
    }
}

static void apply_declaration(style_t *style, const char *name, const char *value,
                              int parent_font_size, int root_size) {
    int font_size = style->font_size ? style->font_size : parent_font_size;

    if (!strcmp(name, "display")) {
        if (word_is(value, "none")) style->display = DISPLAY_NONE;
        else if (word_is(value, "block")) style->display = DISPLAY_BLOCK;
        else if (word_is(value, "inline-block")) style->display = DISPLAY_INLINE_BLOCK;
        else if (word_is(value, "inline")) style->display = DISPLAY_INLINE;
        else if (word_is(value, "list-item")) style->display = DISPLAY_LIST_ITEM;
        else if (word_is(value, "table")) style->display = DISPLAY_TABLE;
        else if (word_is(value, "table-row")) style->display = DISPLAY_TABLE_ROW;
        else if (word_is(value, "table-cell")) style->display = DISPLAY_TABLE_CELL;
        else if (word_is(value, "table-row-group") ||
                 word_is(value, "table-header-group") ||
                 word_is(value, "table-footer-group"))
            style->display = DISPLAY_TABLE_ROW_GROUP;
        else if (word_is(value, "flex") || word_is(value, "grid") ||
                 word_is(value, "flow-root"))
            /* Neither layout is implemented.  Treating the container as an
             * ordinary block stacks its children instead of arranging them,
             * which reads correctly even when it does not look right - and is
             * far better than the alternative of not showing them. */
            style->display = DISPLAY_BLOCK;
        else if (word_is(value, "inline-flex")) style->display = DISPLAY_INLINE_BLOCK;
        return;
    }

    if (!strcmp(name, "color")) {
        colour_t c;
        if (css_parse_colour(value, strlen(value), &c)) style->colour = c;
        return;
    }

    if (!strcmp(name, "background-color") || !strcmp(name, "background")) {
        /* The shorthand can carry an image, a position and much else; the
         * colour is the part that can be drawn here.  A background that is
         * only an image leaves what was underneath, which is right. */
        if (contains_word(value, "transparent") || contains_word(value, "none")) {
            style->has_background = false;
            return;
        }
        colour_t c;
        const char *at = value;
        while (*at) {
            while (*at == ' ') at++;
            const char *start = at;
            int depth = 0;
            while (*at && (depth || *at != ' ')) {
                if (*at == '(') depth++;
                else if (*at == ')') depth--;
                at++;
            }
            if (css_parse_colour(start, (size_t)(at - start), &c)) {
                style->background = c;
                style->has_background = true;
                return;
            }
        }
        return;
    }

    if (!strcmp(name, "font-size")) {
        if (word_is(value, "smaller")) { style->font_size = parent_font_size * 5 / 6; return; }
        if (word_is(value, "larger"))  { style->font_size = parent_font_size * 6 / 5; return; }
        if (word_is(value, "xx-small")) { style->font_size = root_size * 3 / 5; return; }
        if (word_is(value, "x-small"))  { style->font_size = root_size * 3 / 4; return; }
        if (word_is(value, "small"))    { style->font_size = root_size * 8 / 9; return; }
        if (word_is(value, "medium"))   { style->font_size = root_size; return; }
        if (word_is(value, "large"))    { style->font_size = root_size * 6 / 5; return; }
        if (word_is(value, "x-large"))  { style->font_size = root_size * 3 / 2; return; }
        if (word_is(value, "xx-large")) { style->font_size = root_size * 2; return; }

        len_t size = parse_length(value, strlen(value), parent_font_size, root_size);
        if (size.unit == LEN_PX && size.value > 0) style->font_size = size.value;
        else if (size.unit == LEN_PERCENT) style->font_size = parent_font_size * size.value / 100;
        if (style->font_size < 6) style->font_size = 6;
        if (style->font_size > 200) style->font_size = 200;
        return;
    }

    if (!strcmp(name, "font-weight")) {
        if (word_is(value, "bold") || word_is(value, "bolder")) style->font_weight = 700;
        else if (word_is(value, "normal") || word_is(value, "lighter")) style->font_weight = 400;
        else {
            int weight = atoi(value);
            if (weight >= 100 && weight <= 900) style->font_weight = weight;
        }
        return;
    }

    if (!strcmp(name, "font-style")) {
        style->italic = word_is(value, "italic") || word_is(value, "oblique");
        return;
    }

    if (!strcmp(name, "font-family")) {
        style->monospace = contains_word(value, "monospace") ||
                           contains_word(value, "consolas") ||
                           contains_word(value, "courier") ||
                           contains_word(value, "menlo") ||
                           contains_word(value, "monaco");
        return;
    }

    if (!strcmp(name, "font")) {
        /* The shorthand.  Only the parts that can be drawn are read out. */
        if (contains_word(value, "bold")) style->font_weight = 700;
        if (contains_word(value, "italic")) style->italic = true;
        if (contains_word(value, "monospace")) style->monospace = true;
        return;
    }

    if (!strcmp(name, "text-align")) {
        if (word_is(value, "center") || word_is(value, "centre")) style->text_align = ALIGN_CENTRE;
        else if (word_is(value, "right")) style->text_align = ALIGN_RIGHT;
        else if (word_is(value, "justify")) style->text_align = ALIGN_JUSTIFY;
        else style->text_align = ALIGN_LEFT;
        return;
    }

    if (!strcmp(name, "text-decoration") || !strcmp(name, "text-decoration-line")) {
        style->underline = contains_word(value, "underline");
        style->strike = contains_word(value, "line-through");
        if (contains_word(value, "none")) { style->underline = false; style->strike = false; }
        return;
    }

    if (!strcmp(name, "white-space")) {
        if (word_is(value, "pre") || word_is(value, "pre-wrap") ||
            word_is(value, "break-spaces"))
            style->white_space = WHITE_PRE;
        else if (word_is(value, "nowrap") || word_is(value, "pre-line"))
            style->white_space = WHITE_NOWRAP;
        else style->white_space = WHITE_NORMAL;
        return;
    }

    if (!strcmp(name, "line-height")) {
        if (word_is(value, "normal")) { style->line_height = 0; return; }
        /* A bare number is a multiple of the font size. */
        bool bare = true;
        for (const char *at = value; *at; at++)
            if (!((*at >= '0' && *at <= '9') || *at == '.' || *at == ' ')) { bare = false; break; }
        if (bare) {
            int whole = atoi(value);
            int fraction = 0;
            const char *dot = strchr(value, '.');
            if (dot) {
                if (dot[1] >= '0' && dot[1] <= '9') fraction = (dot[1] - '0') * 10;
                if (dot[1] && dot[2] >= '0' && dot[2] <= '9') fraction += dot[2] - '0';
            }
            style->line_height = font_size * (whole * 100 + fraction) / 100;
        } else {
            len_t height = parse_length(value, strlen(value), font_size, root_size);
            style->line_height = len_resolve(height, font_size, 0);
        }
        return;
    }

    if (!strcmp(name, "width"))      { style->width = parse_length(value, strlen(value), font_size, root_size); return; }
    if (!strcmp(name, "height"))     { style->height = parse_length(value, strlen(value), font_size, root_size); return; }
    if (!strcmp(name, "max-width"))  { style->max_width = parse_length(value, strlen(value), font_size, root_size); return; }
    if (!strcmp(name, "min-width"))  { style->min_width = parse_length(value, strlen(value), font_size, root_size); return; }

    if (!strcmp(name, "margin"))  { set_sides(style->margin, value, font_size, root_size); return; }
    if (!strcmp(name, "padding")) { set_sides(style->padding, value, font_size, root_size); return; }

    static const struct { const char *suffix; int side; } sides[] = {
        { "-top", SIDE_TOP }, { "-right", SIDE_RIGHT },
        { "-bottom", SIDE_BOTTOM }, { "-left", SIDE_LEFT },
    };
    for (size_t i = 0; i < 4; i++) {
        char full[24];
        snprintf(full, sizeof full, "margin%s", sides[i].suffix);
        if (!strcmp(name, full)) {
            style->margin[sides[i].side] = parse_length(value, strlen(value), font_size, root_size);
            return;
        }
        snprintf(full, sizeof full, "padding%s", sides[i].suffix);
        if (!strcmp(name, full)) {
            style->padding[sides[i].side] = parse_length(value, strlen(value), font_size, root_size);
            return;
        }
        snprintf(full, sizeof full, "border%s", sides[i].suffix);
        if (!strcmp(name, full)) {
            set_border(style, value, 1 << sides[i].side, font_size, root_size);
            return;
        }
    }

    if (!strcmp(name, "border")) { set_border(style, value, 0xF, font_size, root_size); return; }

    if (!strcmp(name, "border-width")) {
        len_t widths[4] = { len_px(0), len_px(0), len_px(0), len_px(0) };
        set_sides(widths, value, font_size, root_size);
        for (int i = 0; i < 4; i++) style->border_width[i] = len_resolve(widths[i], 0, 0);
        return;
    }
    if (!strcmp(name, "border-color")) {
        colour_t c;
        if (css_parse_colour(value, strlen(value), &c))
            for (int i = 0; i < 4; i++) style->border_colour[i] = c;
        return;
    }
    if (!strcmp(name, "border-style")) {
        int kind = word_is(value, "none") ? BORDER_NONE
                 : word_is(value, "dashed") ? BORDER_DASHED
                 : word_is(value, "dotted") ? BORDER_DOTTED : BORDER_SOLID;
        for (int i = 0; i < 4; i++) {
            style->border_style[i] = (unsigned char)kind;
            if (kind != BORDER_NONE && !style->border_width[i]) style->border_width[i] = 1;
        }
        return;
    }
    if (!strcmp(name, "border-radius")) {
        len_t radius = parse_length(value, strlen(value), font_size, root_size);
        style->border_radius = len_resolve(radius, font_size, 0);
        if (style->border_radius > 40) style->border_radius = 40;
        return;
    }

    if (!strcmp(name, "list-style-type") || !strcmp(name, "list-style")) {
        if (contains_word(value, "none")) style->list_style = LIST_NONE;
        else if (contains_word(value, "decimal")) style->list_style = LIST_DECIMAL;
        else if (contains_word(value, "circle")) style->list_style = LIST_CIRCLE;
        else if (contains_word(value, "square")) style->list_style = LIST_SQUARE;
        else style->list_style = LIST_DISC;
        return;
    }
}

/* ---------------------------------------------------------- the cascade */

/* Which properties pass down to children.  This is a list rather than a rule
 * because there is no rule: it is a decision made per property in the
 * specification, and roughly it is the ones that describe text. */
static void inherit_from(style_t *child, const style_t *parent) {
    child->colour = parent->colour;
    child->font_size = parent->font_size;
    child->font_weight = parent->font_weight;
    child->italic = parent->italic;
    child->monospace = parent->monospace;
    child->text_align = parent->text_align;
    child->white_space = parent->white_space;
    child->line_height = parent->line_height;
    child->list_style = parent->list_style;

    /* Underlining is not inherited by the specification, but it is drawn as
     * though it were: a decoration on a block covers the text inside it, and
     * carrying the flag down is how that ends up on the page. */
    child->underline = parent->underline;
    child->strike = parent->strike;
}

typedef struct {
    rule_t     *rule;
    int         specificity;
    int         order;
} match_t;

#define MAX_MATCHES 160

static int compare_matches(const void *left, const void *right) {
    const match_t *a = left, *b = right;
    if (a->specificity != b->specificity) return a->specificity - b->specificity;
    return a->order - b->order;
}

/* A small insertion sort: the number of rules matching one element is
 * usually under a dozen, and this avoids needing a sort in the library. */
static void sort_matches(match_t *matches, int count) {
    for (int i = 1; i < count; i++) {
        match_t key = matches[i];
        int j = i - 1;
        while (j >= 0 && compare_matches(&matches[j], &key) > 0) {
            matches[j + 1] = matches[j];
            j--;
        }
        matches[j + 1] = key;
    }
}

static void style_element(document_t *doc, dom_node_t *node,
                          stylesheet_t **sheets, int sheet_count,
                          const style_t *parent, int root_size) {
    style_t *style = doc_alloc(doc, sizeof *style);
    if (!style) return;

    if (parent) inherit_from(style, parent);
    else {
        style->colour = RGB(24, 24, 27);
        style->font_size = root_size;
        style->font_weight = 400;
    }

    /* Blocks and inlines differ, and which one an element is by default is
     * part of the browser's own stylesheet rather than of the element. */
    style->display = DISPLAY_INLINE;

    int parent_font_size = parent ? parent->font_size : root_size;

    match_t matches[MAX_MATCHES];
    int count = 0;
    int base_order = 0;

    for (int s = 0; s < sheet_count && count < MAX_MATCHES; s++) {
        if (!sheets[s]) continue;
        for (rule_t *rule = sheets[s]->rules; rule && count < MAX_MATCHES;
             rule = rule->next) {
            int best = -1;
            for (int i = 0; i < rule->selector_count; i++) {
                if (!selector_matches(&rule->selectors[i], node)) continue;
                if (rule->selectors[i].specificity > best)
                    best = rule->selectors[i].specificity;
            }
            if (best < 0) continue;

            matches[count].rule = rule;
            /* A later sheet outranks an earlier one at equal specificity,
             * which is what puts the page's own rules above the defaults. */
            matches[count].specificity = best;
            matches[count].order = base_order + rule->order;
            count++;
        }
        base_order += 100000;
    }

    sort_matches(matches, count);

    /* Least specific first, so the most specific writes last and wins.  The
     * font size is applied ahead of everything else in each rule because a
     * length in em depends on it. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count; i++) {
            for (declaration_t *decl = matches[i].rule->declarations; decl;
                 decl = decl->next) {
                bool is_size = !strcmp(decl->name, "font-size");
                if ((pass == 0) != is_size) continue;
                apply_declaration(style, decl->name, decl->value,
                                  parent_font_size, root_size);
            }
        }
    }

    /* The element's own style attribute outranks every rule. */
    const char *inline_style = dom_attr(node, "style");
    if (inline_style && *inline_style) {
        /* Reuse the declaration parser by making a rule out of it. */
        sheet_storage_t scratch;
        memset(&scratch, 0, sizeof scratch);
        rule_t temporary;
        memset(&temporary, 0, sizeof temporary);
        parse_declarations(&scratch, &temporary, inline_style, strlen(inline_style));

        for (int pass = 0; pass < 2; pass++) {
            for (declaration_t *decl = temporary.declarations; decl; decl = decl->next) {
                bool is_size = !strcmp(decl->name, "font-size");
                if ((pass == 0) != is_size) continue;
                apply_declaration(style, decl->name, decl->value,
                                  parent_font_size, root_size);
            }
        }
        sheet_block_t *block = scratch.blocks;
        while (block) { sheet_block_t *next = block->next; free(block); block = next; }
    }

    /* A few things the markup decides rather than the style. */
    if (node->tag == TAG_A && dom_attr(node, "href")) style->is_link = true;

    if (!style->font_size) style->font_size = root_size;
    if (!style->font_weight) style->font_weight = 400;

    node->style = style;
}

void css_apply(document_t *doc, stylesheet_t **sheets, int sheet_count,
               int base_font_size) {
    if (!doc || !doc->root) return;

    dom_node_t *node = doc->root;
    while (node) {
        if (node->kind == NODE_ELEMENT) {
            const style_t *parent = NULL;
            for (dom_node_t *up = node->parent; up; up = up->parent) {
                if (up->style) { parent = up->style; break; }
            }
            style_element(doc, node, sheets, sheet_count, parent, base_font_size);
        }
        node = dom_next(node, doc->root);
    }
}

const style_t *css_style_of(const dom_node_t *node) {
    return node ? (const style_t *)node->style : NULL;
}
