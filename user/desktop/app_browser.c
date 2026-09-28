/* app_browser.c - Kestrel, the browser.
 *
 * Everything underneath this file exists so that this one can work: the
 * network stack, the secure connection, the certificate checks, the markup
 * parser, the style system, the layout engine, the text renderer.  This is
 * where they meet a person.
 *
 * What a browser has to be, beyond correct, is honest and responsive.  Honest
 * means the padlock appears only when the certificate really was checked and
 * really did trace back to an authority this machine trusted before the
 * connection started - and that when something is wrong, the page says what,
 * in a sentence rather than a code.  Responsive means the window never stops
 * answering while a page is being fetched, which is why the fetch happens on a
 * thread of its own and the window keeps painting.
 */
#include "desktop.h"
#include "aatext.h"
#include "web.h"
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "paint.h"
#include "image.h"

#define TOOLBAR_H     52
#define STATUS_H      24
#define HISTORY_MAX   64
#define ADDRESS_MAX   512

typedef enum {
    PAGE_EMPTY = 0,
    PAGE_LOADING,
    PAGE_READY,
    PAGE_FAILED,
} page_state;

typedef struct {
    wm_t     *wm;
    window_t *window;

    /* What is on screen. */
    document_t   *doc;
    stylesheet_t *page_sheet;
    stylesheet_t *default_sheet;
    layout_t     *layout;
    int           scroll;
    int           laid_out_for;      /* the width the layout was built at */

    page_state state;
    char       error[192];
    char       title[160];

    bool       secure;
    ktlspeer_t peer;

    /* The address bar. */
    char address[ADDRESS_MAX];
    int  address_len;
    int  caret;
    bool address_focused;
    int  address_scroll;

    /* Where we have been. */
    char history[HISTORY_MAX][ADDRESS_MAX];
    int  history_count;
    int  history_at;

    /* The fetch, which runs on its own thread so the window keeps answering. */
    uint64_t       last_spin_ms;
    volatile bool  loading;
    volatile bool  arrived;
    char           loading_url[ADDRESS_MAX];
    char           progress[160];
    web_response_t incoming;
    int            spinner_step;

    /* A pending POST body, when the load in flight is a form submission rather
     * than a plain GET.  is_post is cleared for every ordinary navigation, so
     * back/forward and typed addresses are always GET. */
    bool           is_post;
    char           post_body[2048];
    int            post_len;

    /* What the pointer is over. */
    const dom_node_t *hovered;
    const char       *hovered_link;
    bool              over_page;

    /* Form fields the user has typed into.  Each holds the text for one input
     * node; the node's form_value points into here so paint shows it.  Cleared
     * on navigate (the nodes are freed with the old document). */
    struct { dom_node_t *node; char value[128]; } fields[16];
    int         field_count;
    dom_node_t *focused_input;       /* the field keys go to, or NULL */

    /* Toolbar hit areas, so the event handler and the painter agree. */
    rect_t back_rect, forward_rect, reload_rect, home_rect, address_rect;
    int    hover_button;             /* 0 none, 1 back, 2 forward, 3 reload, 4 home */
} browser_t;

/* One browser at a time, because the fetch thread needs to find it. */
static browser_t *active;

static void navigate(browser_t *b, const char *url, bool record);
static void clear_forms(browser_t *b);

/* ------------------------------------------------------------- the start page
 *
 * Written as markup and rendered by the same engine as everything else.  A
 * start page drawn with drawing calls would be a second renderer to keep
 * working; this way it is a test of the first one every time the browser
 * opens.
 */
static const char *start_page(void) {
    return
    "<html><head><title>Kestrel</title><style>\n"
    "body { background: #fbfbfc; color: #27272a; padding: 0; }\n"
    ".wrap { max-width: 620px; margin: 54px auto 0 auto; padding: 0 28px; }\n"
    "h1 { font-size: 2.4em; margin: 0 0 6px 0; color: #18181b; }\n"
    ".sub { color: #71717a; font-size: 1.05em; margin: 0 0 34px 0; }\n"
    "h2 { font-size: 1.05em; color: #52525b; margin: 30px 0 10px 0; }\n"
    ".card { display: block; background: #ffffff; border: 1px solid #e7e7ea;\n"
    "        border-radius: 8px; padding: 14px 18px; margin: 0 0 10px 0; }\n"
    ".card a { font-size: 1.05em; text-decoration: none; }\n"
    ".card p { margin: 4px 0 0 0; color: #71717a; font-size: 0.92em; }\n"
    "ul { color: #52525b; }\n"
    "li { margin: 6px 0; }\n"
    "code { padding: 1px 5px; border-radius: 3px; }\n"
    "</style></head><body>\n"
    "<div class=\"wrap\">\n"
    "<h1>Kestrel</h1>\n"
    "<p class=\"sub\">A browser written for this system, from the socket up.</p>\n"

    "<h2>Somewhere to start</h2>\n"
    "<div class=\"card\"><a href=\"https://example.com/\">example.com</a>\n"
    "<p>The smallest real page on the internet, and the one worth trying first.</p></div>\n"
    "<div class=\"card\"><a href=\"https://en.wikipedia.org/wiki/Web_browser\">Wikipedia</a>\n"
    "<p>A long article, with headings, lists, tables and several hundred links.</p></div>\n"
    "<div class=\"card\"><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">The first website</a>\n"
    "<p>Still where it was put in 1991, and still perfectly readable.</p></div>\n"

    "<h2>What this can and cannot do</h2>\n"
    "<ul>\n"
    "<li>Secure connections are real: the certificate is checked against the "
    "authorities this machine already trusted, and the padlock appears only "
    "when every one of those checks passed.</li>\n"
    "<li>Markup, stylesheets, and page layout are handled here - headings, "
    "lists, tables, links, borders, colours and spacing.</li>\n"
    "<li>There is no scripting. A page that builds itself after loading will "
    "arrive nearly empty, and that is the honest result rather than a "
    "guess at what it meant to show.</li>\n"
    "<li>Pictures are not decoded yet; each one leaves a frame with whatever "
    "the page said it was of.</li>\n"
    "</ul>\n"
    "</div></body></html>";
}

static const char *error_page(const char *heading, const char *detail,
                              const char *address) {
    static char page[2048];
    snprintf(page, sizeof page,
        "<html><head><title>%s</title><style>\n"
        "body { background: #fbfbfc; color: #27272a; }\n"
        ".wrap { max-width: 540px; margin: 76px auto 0 auto; padding: 0 28px; }\n"
        "h1 { font-size: 1.6em; margin: 0 0 12px 0; color: #b91c1c; }\n"
        "p { color: #3f3f46; line-height: 1.6; }\n"
        ".addr { color: #71717a; font-family: monospace; font-size: 0.9em;\n"
        "        background: #f4f4f5; border: 1px solid #e4e4e7;\n"
        "        border-radius: 4px; padding: 8px 10px; }\n"
        "</style></head><body><div class=\"wrap\">\n"
        "<h1>%s</h1>\n"
        "<p>%s</p>\n"
        "<p class=\"addr\">%s</p>\n"
        "</div></body></html>",
        heading, heading, detail, address);
    return page;
}

/* ------------------------------------------------------------- the document */

static void clear_page(browser_t *b) {
    if (b->layout) { layout_free(b->layout); b->layout = NULL; }
    if (b->page_sheet) { css_free(b->page_sheet); b->page_sheet = NULL; }
    if (b->doc) { document_free(b->doc); b->doc = NULL; }
    b->hovered = NULL;
    b->hovered_link = NULL;
}

static int page_width(const browser_t *b) {
    if (!b->window || !b->window->canvas) return 800;
    int width = b->window->canvas->width - 16;   /* room for the scrollbar */
    return width > 200 ? width : 200;
}

/* Fetch and decode every picture on the page, and hand each image box the
 * pixels to draw.  Runs after layout - a box knows its <img> node, the node
 * knows its src, and web_image_for turns that into decoded pixels (once,
 * cached), resolving a relative address against the page's own. */
static int load_images(box_t *box, const char *base) {
    if (!box) return 0;
    int n = 0;
    if (box->kind == BOX_IMAGE && box->node) {
        const char *src = dom_attr(box->node, "src");
        n++;
        if (src && src[0])
            box->image = web_image_for(base, src);
    }
    for (box_t *c = box->first_child; c; c = c->next)
        n += load_images(c, base);
    return n;
}

static void relayout(browser_t *b) {
    if (!b->doc) return;
    if (b->layout) { layout_free(b->layout); b->layout = NULL; }

    int width = page_width(b);
    b->layout = layout_document(b->doc, width);
    b->laid_out_for = width;
    if (b->layout && b->layout->root)
        load_images(b->layout->root, b->loading_url);

    int visible = b->window->canvas->height - TOOLBAR_H - STATUS_H;
    int limit = b->layout ? b->layout->height - visible : 0;
    if (limit < 0) limit = 0;
    if (b->scroll > limit) b->scroll = limit;
}

static void show_html(browser_t *b, const char *html, size_t len) {
    clear_forms(b);                  /* drop old-page fields while nodes valid */
    clear_page(b);
    web_image_forget_all();          /* a new page: its own pictures */

    b->doc = html_parse(html, len);
    if (!b->doc) return;

    if (b->doc->css && b->doc->css_len)
        b->page_sheet = css_parse(b->doc->css, b->doc->css_len);

    stylesheet_t *sheets[2] = { b->default_sheet, b->page_sheet };
    css_apply(b->doc, sheets, 2, 16);

    strlcpy(b->title, b->doc->title && b->doc->title[0] ? b->doc->title : "",
            sizeof b->title);

    b->scroll = 0;
    relayout(b);
}

/* ------------------------------------------------------------- the fetching
 *
 * On its own thread.  A page from a distant server can take seconds, and a
 * window that stops repainting for seconds looks broken however quickly it
 * finishes.
 */
static void fetch_progress(const char *what, void *ctx) {
    browser_t *b = ctx;
    strlcpy(b->progress, what, sizeof b->progress);
}

static void fetch_thread(void *arg) {
    browser_t *b = arg;

    web_on_progress(fetch_progress, b);
    memset(&b->incoming, 0, sizeof b->incoming);

    bool ok = b->is_post
            ? web_fetch_post(b->loading_url, b->post_body, b->post_len,
                             NULL, 6, &b->incoming)
            : web_fetch(b->loading_url, 6, &b->incoming);
    if (!ok)
        b->incoming.status = 0;

    /* Set last: the window's thread watches this flag and reads everything
     * else once it is set, so nothing may be written after it. */
    b->arrived = true;
}

static void start_fetch(browser_t *b, const char *url) {
    if (b->loading) return;

    strlcpy(b->loading_url, url, sizeof b->loading_url);
    strlcpy(b->progress, "Starting", sizeof b->progress);
    b->state = PAGE_LOADING;
    b->loading = true;
    b->arrived = false;
    b->spinner_step = 0;

    if (thread_create(fetch_thread, b) < 0) {
        /* Without a thread it still has to work, so it happens here - the
         * window will simply not repaint until it finishes. */
        b->loading = false;
        fetch_thread(b);
        b->loading = true;
    }
}

static void finish_fetch(browser_t *b) {
    b->loading = false;
    b->arrived = false;

    web_response_t *r = &b->incoming;

    if (r->error[0]) {
        b->state = PAGE_FAILED;
        strlcpy(b->error, r->error, sizeof b->error);
        b->secure = false;
        const char *page = error_page("This page could not be opened",
                                      r->error, b->loading_url);
        show_html(b, page, strlen(page));
        strlcpy(b->title, "Not opened", sizeof b->title);
        web_free(r);
        return;
    }

    b->secure = r->secure;
    if (r->secure) b->peer = r->peer;

    if (r->final_url[0]) {
        strlcpy(b->address, r->final_url, sizeof b->address);
        b->address_len = (int)strlen(b->address);
        b->caret = b->address_len;
        if (b->history_count > 0 && b->history_at >= 0)
            strlcpy(b->history[b->history_at], b->address, ADDRESS_MAX);
    }

    if (r->status >= 400) {
        char detail[192];
        snprintf(detail, sizeof detail,
                 "The server answered with status %d. The page may have moved "
                 "or may never have been there.", r->status);
        /* A server that sends a page along with the error is showing it on
         * purpose, and that page is more useful than anything written here. */
        if (r->body_len < 200) {
            const char *page = error_page("Not found", detail, b->address);
            show_html(b, page, strlen(page));
            b->state = PAGE_FAILED;
            web_free(r);
            return;
        }
    }

    /* Anything that is not markup is shown as what it is rather than
     * interpreted as something it is not. */
    bool is_html = !r->content_type[0] ||
                   strstr(r->content_type, "html") != NULL;

    if (is_html) {
        show_html(b, r->body, (size_t)r->body_len);
    } else {
        /* Plain text, wrapped in just enough markup to lay it out. */
        size_t cap = (size_t)r->body_len * 6 + 512;
        char *wrapped = malloc(cap);
        if (wrapped) {
            size_t at = (size_t)snprintf(wrapped, cap,
                "<html><head><title>%s</title><style>body{padding:20px}"
                "pre{font-size:0.95em}</style></head><body><pre>",
                b->loading_url);
            for (int i = 0; i < r->body_len && at + 8 < cap; i++) {
                char c = r->body[i];
                if (c == '<') { memcpy(wrapped + at, "&lt;", 4); at += 4; }
                else if (c == '>') { memcpy(wrapped + at, "&gt;", 4); at += 4; }
                else if (c == '&') { memcpy(wrapped + at, "&amp;", 5); at += 5; }
                else wrapped[at++] = c;
            }
            at += (size_t)snprintf(wrapped + at, cap - at, "</pre></body></html>");
            show_html(b, wrapped, at);
            free(wrapped);
        }
    }

    b->state = PAGE_READY;
    if (!b->title[0]) strlcpy(b->title, b->address, sizeof b->title);

    char window_title[WIN_TITLE_MAX];
    snprintf(window_title, sizeof window_title, "%s - Kestrel",
             b->title[0] ? b->title : "Kestrel");
    strlcpy(b->window->title, window_title, WIN_TITLE_MAX);

    web_free(r);
}

/* -------------------------------------------------------------- navigation */

static void navigate_ex(browser_t *b, const char *url, bool record,
                        const char *post_body, int post_len) {
    if (!url || !*url) return;

    /* Every navigation is a GET unless a body is handed in; setting this here
     * means back/forward, home and typed addresses can never inherit a POST. */
    b->is_post = post_body != NULL;
    if (b->is_post) {
        if (post_len > (int)sizeof b->post_body) post_len = sizeof b->post_body;
        memcpy(b->post_body, post_body, (size_t)post_len);
        b->post_len = post_len;
    }

    if (!strncmp(url, "about:", 6) || !strcmp(url, "kestrel:start")) {
        clear_page(b);
        const char *page = start_page();
        show_html(b, page, strlen(page));
        b->state = PAGE_READY;
        b->secure = false;
        strlcpy(b->address, "kestrel:start", sizeof b->address);
        b->address_len = (int)strlen(b->address);
        b->caret = b->address_len;
        strlcpy(b->window->title, "Kestrel", WIN_TITLE_MAX);
        return;
    }

    if (record) {
        /* Going somewhere new from part-way back throws away what was ahead,
         * which is what every browser does and what people expect. */
        if (b->history_at < b->history_count - 1)
            b->history_count = b->history_at + 1;
        if (b->history_count >= HISTORY_MAX) {
            memmove(b->history[0], b->history[1],
                    (size_t)(HISTORY_MAX - 1) * ADDRESS_MAX);
            b->history_count--;
        }
        strlcpy(b->history[b->history_count], url, ADDRESS_MAX);
        b->history_at = b->history_count;
        b->history_count++;
    }

    strlcpy(b->address, url, sizeof b->address);
    b->address_len = (int)strlen(b->address);
    b->caret = b->address_len;
    b->address_focused = false;

    start_fetch(b, url);
}

static void navigate(browser_t *b, const char *url, bool record) {
    navigate_ex(b, url, record, NULL, 0);
}

static void go_back(browser_t *b) {
    if (b->history_at <= 0) return;
    b->history_at--;
    navigate(b, b->history[b->history_at], false);
}

static void go_forward(browser_t *b) {
    if (b->history_at >= b->history_count - 1) return;
    b->history_at++;
    navigate(b, b->history[b->history_at], false);
}

/* A link's target, made absolute against where we are. */
static void follow_link(browser_t *b, const char *href) {
    web_url_t here, target;
    char absolute[ADDRESS_MAX];

    if (web_parse_url(b->address, NULL, &here) &&
        web_parse_url(href, &here, &target)) {
        web_format_url(&target, absolute, sizeof absolute);
        navigate(b, absolute, true);
    } else {
        navigate(b, href, true);
    }
}

/* --------------------------------------------------------------- form fields
 *
 * Typing into a page's inputs.  Each focused field gets a slot whose text the
 * input node points at, so the same value the user types is what paint draws
 * and what a submit gathers.
 */

/* Forget every field - on navigate, since the nodes belong to the old page. */
static void clear_forms(browser_t *b) {
    for (int i = 0; i < b->field_count; i++)
        if (b->fields[i].node) b->fields[i].node->form_value = NULL;
    b->field_count = 0;
    b->focused_input = NULL;
}

/* The slot for a node, made if needed and seeded from its value attribute. */
static char *field_for(browser_t *b, dom_node_t *node) {
    for (int i = 0; i < b->field_count; i++)
        if (b->fields[i].node == node) return b->fields[i].value;
    if (b->field_count >= (int)(sizeof b->fields / sizeof b->fields[0])) return NULL;
    int i = b->field_count++;
    b->fields[i].node = node;
    const char *v = dom_attr(node, "value");
    strlcpy(b->fields[i].value, v ? v : "", sizeof b->fields[i].value);
    node->form_value = b->fields[i].value;
    return b->fields[i].value;
}

/* Whether a checkbox/radio is checked: the "on" it was set to once clicked,
 * else the initial `checked` attribute. */
static bool control_is_checked(browser_t *b, dom_node_t *node) {
    for (int i = 0; i < b->field_count; i++)
        if (b->fields[i].node == node)
            return !strcmp(b->fields[i].value, "on");
    return dom_attr(node, "checked") != NULL;
}

/* Record a checkbox/radio's checked state in its slot ("on" or empty), which is
 * what paint and submit read.  A checkbox's *submit value* is its value= attr;
 * this slot only carries whether it is checked. */
static void set_checked(browser_t *b, dom_node_t *node, bool on) {
    char *slot = field_for(b, node);
    if (slot) strlcpy(slot, on ? "on" : "", 4);
}

/* A click on a checkbox flips it; a click on a radio turns it on and its
 * same-name siblings off (one of a group is chosen at a time). */
static void toggle_control(browser_t *b, dom_node_t *node) {
    const char *type = dom_attr(node, "type");
    if (type && !strcmp(type, "radio")) {
        const dom_node_t *form = node;
        for (const dom_node_t *up = node; up; up = up->parent)
            if (up->tag == TAG_FORM) { form = up; break; }
        const char *name = dom_attr(node, "name");
        for (const dom_node_t *n = form; n; n = dom_next(n, form)) {
            if (!layout_node_is_checkable(n)) continue;
            const char *nt = dom_attr(n, "type");
            if (!nt || strcmp(nt, "radio")) continue;
            const char *nn = dom_attr(n, "name");
            if (name && nn && !strcmp(name, nn))
                set_checked(b, (dom_node_t *)n, n == node);
        }
        if (!name) set_checked(b, node, true);
    } else {
        set_checked(b, node, !control_is_checked(b, node));
    }
}

/* URL-encode `in` into `out` (percent-encoding all but the unreserved set). */
static void url_encode(const char *in, char *out, size_t cap) {
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else if (c == ' ') {
            out[o++] = '+';
        } else {
            out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 0xF];
        }
    }
    out[o] = 0;
}

/* Whether a form's method attribute asks for POST (default is GET). */
static bool form_is_post(const dom_node_t *form) {
    const char *m = dom_attr(form, "method");
    return m && (m[0] == 'p' || m[0] == 'P');   /* "post" in either case */
}

/* Resolve a form's action against the current page into an absolute URL. */
static void form_action_url(browser_t *b, const dom_node_t *form,
                            char *out, size_t cap) {
    const char *action = dom_attr(form, "action");
    const char *raw = (action && *action) ? action : b->address;
    web_url_t here, target;
    if (web_parse_url(b->address, NULL, &here) &&
        web_parse_url(raw, &here, &target))
        web_format_url(&target, out, cap);
    else
        strlcpy(out, raw, cap);
}

/* Submit the form the focused input is in: gather its named editable fields
 * into an application/x-www-form-urlencoded string.  A GET carries it in the
 * query part of the action URL; a POST sends it as the request body. */
static void submit_form(browser_t *b, dom_node_t *input) {
    const dom_node_t *form = NULL;
    for (const dom_node_t *up = input; up; up = up->parent)
        if (up->tag == TAG_FORM) { form = up; break; }
    if (!form) return;

    char query[2048]; size_t q = 0; query[0] = 0;
    for (const dom_node_t *n = form; n; n = dom_next(n, form)) {
        bool editable  = layout_node_is_editable(n);
        bool checkable = layout_node_is_checkable(n);
        if (!editable && !checkable) continue;
        const char *name = dom_attr(n, "name");
        if (!name || !*name) continue;
        const char *val;
        if (checkable) {
            /* A checkbox/radio is submitted only when checked, and then it sends
             * its value= attr (defaulting to "on"), not its checked flag. */
            if (!control_is_checked(b, (dom_node_t *)n)) continue;
            val = dom_attr(n, "value") ? dom_attr(n, "value") : "on";
        } else {
            val = n->form_value ? n->form_value :
                  (dom_attr(n, "value") ? dom_attr(n, "value") : "");
        }
        char en[192], ev[256];
        url_encode(name, en, sizeof en);
        url_encode(val, ev, sizeof ev);
        int wrote = snprintf(query + q, sizeof query - q, "%s%s=%s",
                             q ? "&" : "", en, ev);
        if (wrote > 0) q += (size_t)wrote;
        if (q >= sizeof query - 1) break;
    }

    char target[ADDRESS_MAX];
    form_action_url(b, form, target, sizeof target);

    if (form_is_post(form)) {
        navigate_ex(b, target, true, query, (int)q);
    } else {
        /* For GET the fields become the query part, replacing whatever query
         * or fragment the action URL already carried (as the HTML form spec
         * says: the form data set is the new query). */
        size_t t = 0;
        while (target[t] && target[t] != '?' && target[t] != '#') t++;
        snprintf(target + t, sizeof target - t, "%s%s", q ? "?" : "", query);
        navigate(b, target, true);
    }
}

/* ------------------------------------------------------------- the toolbar */

static void paint_toolbar(browser_t *b, surface_t *s) {
    int width = s->width;
    rect_t bar = rect_make(0, 0, width, TOOLBAR_H);

    gui_fill(s, bar, colour_mix(g_theme.window, g_theme.text, 8));
    gui_fill(s, rect_make(0, TOOLBAR_H - 1, width, 1),
             colour_mix(g_theme.window, g_theme.text, 30));

    int x = 8;
    int button = 34;
    int y = (TOOLBAR_H - button) / 2;

    struct { rect_t *rect; icon_id icon; bool enabled; int id; } buttons[] = {
        { &b->back_rect,    ICON_ARROW_LEFT,  b->history_at > 0, 1 },
        { &b->forward_rect, ICON_ARROW_RIGHT, b->history_at < b->history_count - 1, 2 },
        { &b->reload_rect,  ICON_RELOAD,      true, 3 },
        { &b->home_rect,    ICON_HOME,        true, 4 },
    };

    for (size_t i = 0; i < 4; i++) {
        *buttons[i].rect = rect_make(x, y, button, button);
        bool hover = (b->hover_button == buttons[i].id) && buttons[i].enabled;
        if (hover)
            gui_round_rect_aa(s, *buttons[i].rect, 6,
                              colour_mix(g_theme.window, g_theme.text, 22));
        colour_t tint = buttons[i].enabled
                      ? colour_mix(g_theme.window, g_theme.text, 190)
                      : colour_mix(g_theme.window, g_theme.text, 55);
        gui_icon(s, buttons[i].icon, x + (button - 16) / 2, y + (button - 16) / 2,
                 16, tint);
        x += button + 2;
    }

    x += 6;
    int address_w = width - x - 12;
    b->address_rect = rect_make(x, 9, address_w, TOOLBAR_H - 18);

    rect_t field = b->address_rect;
    gui_round_rect_aa(s, field, 7, g_theme.field);
    gui_round_frame_aa(s, field, 7,
                       b->address_focused ? g_theme.accent : g_theme.field_border);

    /* The padlock is the whole point of the toolbar, so it goes first and it
     * only appears when the connection really was checked. */
    int text_x = field.x + 10;
    if (b->secure) {
        gui_icon(s, ICON_LOCK, text_x, field.y + (field.h - 14) / 2, 14,
                 RGB(22, 138, 78));
        text_x += 20;
    } else if (b->state == PAGE_READY && strncmp(b->address, "kestrel:", 8)) {
        gui_icon(s, ICON_INFO, text_x, field.y + (field.h - 14) / 2, 14,
                 colour_mix(g_theme.field, g_theme.text, 110));
        text_x += 20;
    }

    int baseline = field.y + field.h / 2 + 5;
    int room = field.x + field.w - text_x - 10;

    if (b->address_len == 0 && !b->address_focused) {
        aa_text(s, &AA_BODY, 15, text_x, baseline, "Search or type an address",
                colour_mix(g_theme.field, g_theme.field_text, 110));
    } else {
        /* When the text is longer than the field, keep the caret in view by
         * sliding the text rather than clipping the end off. */
        int caret_x = aa_text_width_n(&AA_BODY, 15, b->address, (size_t)b->caret);
        if (caret_x - b->address_scroll > room - 8)
            b->address_scroll = caret_x - room + 8;
        if (caret_x - b->address_scroll < 0) b->address_scroll = caret_x;
        if (b->address_scroll < 0) b->address_scroll = 0;

        rect_t saved = surface_clip(s);
        surface_set_clip(s, rect_intersection(saved,
                         rect_make(text_x, field.y, room, field.h)));
        aa_text_n(s, &AA_BODY, 15, text_x - b->address_scroll, baseline,
                  b->address, (size_t)b->address_len, g_theme.field_text);
        surface_set_clip(s, saved);

        if (b->address_focused) {
            int at = text_x + caret_x - b->address_scroll;
            gui_fill(s, rect_make(at, field.y + 6, 1, field.h - 12), g_theme.accent);
        }
    }

    if (b->loading) {
        gui_spinner(s, field.x + field.w - 18, field.y + field.h / 2, 7,
                    g_theme.accent);
    }
}

static void paint_status(browser_t *b, surface_t *s) {
    int y = s->height - STATUS_H;
    rect_t bar = rect_make(0, y, s->width, STATUS_H);
    gui_fill(s, bar, colour_mix(g_theme.window, g_theme.text, 8));
    gui_fill(s, rect_make(0, y, s->width, 1),
             colour_mix(g_theme.window, g_theme.text, 25));

    colour_t dim = colour_mix(g_theme.window, g_theme.text, 145);
    int baseline = y + STATUS_H / 2 + 4;

    if (b->loading) {
        aa_text_clipped(s, &AA_BODY, 12, 10, baseline, s->width - 20,
                        b->progress, dim);
        return;
    }

    if (b->hovered_link) {
        aa_text_clipped(s, &AA_BODY, 12, 10, baseline, s->width - 220,
                        b->hovered_link, dim);
    } else if (b->state == PAGE_FAILED) {
        aa_text_clipped(s, &AA_BODY, 12, 10, baseline, s->width - 220,
                        b->error, g_theme.error);
    } else if (b->doc) {
        char note[96];
        snprintf(note, sizeof note, "%d elements", b->doc->node_count);
        aa_text(s, &AA_BODY, 12, 10, baseline, note, dim);
    }

    /* Who the connection is with, on the right, where the padlock's meaning
     * can actually be read. */
    if (b->secure && b->peer.subject[0]) {
        char who[128];
        snprintf(who, sizeof who, "%s - %s", b->peer.subject, b->peer.cipher);
        int width = aa_text_width(&AA_BODY, 12, who);
        int max = s->width / 2;
        if (width > max) width = max;
        aa_text_clipped(s, &AA_BODY, 12, s->width - width - 10, baseline, width,
                        who, RGB(22, 138, 78));
    }
}

static void paint_scrollbar(browser_t *b, surface_t *s) {
    if (!b->layout) return;

    int top = TOOLBAR_H;
    int height = s->height - TOOLBAR_H - STATUS_H;
    if (b->layout->height <= height) return;

    int x = s->width - 12;
    gui_fill(s, rect_make(x, top, 12, height), g_theme.scrollbar);

    int thumb = height * height / b->layout->height;
    if (thumb < 28) thumb = 28;
    int travel = height - thumb;
    int limit = b->layout->height - height;
    int at = limit > 0 ? travel * b->scroll / limit : 0;

    gui_round_rect_aa(s, rect_make(x + 3, top + at + 1, 6, thumb - 2), 3,
                      g_theme.scrollbar_thumb);
}

/* ------------------------------------------------------------ the callback */

static bool browser_proc(window_t *w, const wevent_t *ev) {
    browser_t *b = w->data;
    surface_t *s = w->canvas;

    switch (ev->kind) {
    case WE_PAINT: {
        colour_t background = RGB(255, 255, 255);
        if (b->doc && b->doc->body) {
            const style_t *body = css_style_of(b->doc->body);
            if (body && body->has_background) background = body->background;
        }

        rect_t page = rect_make(0, TOOLBAR_H, s->width,
                                s->height - TOOLBAR_H - STATUS_H);
        gui_fill(s, page, background);

        if (b->layout) {
            paint_state_t state = { b->hovered, RGB(196, 40, 40) };
            paint_page(s, page, b->layout, b->scroll, &state);
        } else if (b->loading) {
            gui_spinner(s, s->width / 2, s->height / 2, 16, g_theme.accent);
        }

        paint_scrollbar(b, s);
        paint_toolbar(b, s);
        paint_status(b, s);
        return false;
    }

    case WE_TICK: {
        bool repaint = false;

        if (b->arrived) { finish_fetch(b); repaint = true; }

        /* While a page is on its way, the only thing that changes is the
         * spinner - and repainting the whole window sixty times a second to
         * turn it costs more than the fetch itself, on a machine with one
         * processor.  Eight times a second looks the same and leaves the
         * thread doing the fetching the time it needs. */
        if (b->loading) {
            uint64_t now = uptime_ms();
            if (now - b->last_spin_ms >= 125) {
                b->last_spin_ms = now;
                b->spinner_step++;
                repaint = true;
            }
        }
        if (gui_controls_settling()) repaint = true;

        /* Animated GIFs on the page: advance any whose frame is due, and
         * repaint if one did.  web_image_tick points each image at its current
         * frame, which paint_image already draws. */
        if (web_image_tick(uptime_ms())) repaint = true;

        return repaint;
    }

    case WE_RESIZE:
        if (page_width(b) != b->laid_out_for) relayout(b);
        return true;

    case WE_MOUSE_MOVE: {
        int was = b->hover_button;
        b->hover_button = 0;
        if (rect_contains(b->back_rect, ev->x, ev->y)) b->hover_button = 1;
        else if (rect_contains(b->forward_rect, ev->x, ev->y)) b->hover_button = 2;
        else if (rect_contains(b->reload_rect, ev->x, ev->y)) b->hover_button = 3;
        else if (rect_contains(b->home_rect, ev->x, ev->y)) b->hover_button = 4;

        const char *was_link = b->hovered_link;
        const dom_node_t *was_node = b->hovered;
        b->hovered_link = NULL;
        b->hovered = NULL;
        b->over_page = false;

        if (ev->y >= TOOLBAR_H && ev->y < s->height - STATUS_H && b->layout) {
            b->over_page = true;
            int page_x = ev->x;
            int page_y = ev->y - TOOLBAR_H + b->scroll;
            b->hovered_link = layout_link_at(b->layout, page_x, page_y);
            if (b->hovered_link) {
                /* Highlight the whole link, not just the word under the
                 * pointer: a link that spans three words should light up as
                 * one thing. */
                const dom_node_t *node = paint_node_at(b->layout, page_x, page_y);
                for (const dom_node_t *up = node; up; up = up->parent)
                    if (up->kind == NODE_ELEMENT && up->tag == TAG_A) {
                        b->hovered = up;
                        break;
                    }
            }
        }

        return was != b->hover_button || was_link != b->hovered_link ||
               was_node != b->hovered;
    }

    case WE_MOUSE_DOWN: {
        if (ev->y < TOOLBAR_H) {
            b->address_focused = false;
            if (rect_contains(b->back_rect, ev->x, ev->y)) { go_back(b); return true; }
            if (rect_contains(b->forward_rect, ev->x, ev->y)) { go_forward(b); return true; }
            if (rect_contains(b->reload_rect, ev->x, ev->y)) {
                if (b->address[0]) navigate(b, b->address, false);
                return true;
            }
            if (rect_contains(b->home_rect, ev->x, ev->y)) {
                navigate(b, "kestrel:start", true);
                return true;
            }
            if (rect_contains(b->address_rect, ev->x, ev->y)) {
                b->address_focused = true;
                b->caret = b->address_len;
                return true;
            }
            return true;
        }

        /* The scrollbar. */
        if (ev->x >= s->width - 12 && b->layout) {
            int height = s->height - TOOLBAR_H - STATUS_H;
            if (b->layout->height > height) {
                int at = ev->y - TOOLBAR_H;
                int limit = b->layout->height - height;
                b->scroll = limit * at / (height > 0 ? height : 1);
                if (b->scroll < 0) b->scroll = 0;
                if (b->scroll > limit) b->scroll = limit;
                return true;
            }
        }

        b->address_focused = false;

        /* Clicking a page input focuses it for typing.  A link wins if both
         * are under the pointer (a linked button is rare); otherwise clicking
         * empty page area drops focus. */
        if (b->layout && !b->hovered_link) {
            int page_x = ev->x, page_y = ev->y - TOOLBAR_H + b->scroll;
            dom_node_t *in = layout_input_at(b->layout, page_x, page_y);
            if (in && layout_node_is_submit(in)) {
                b->focused_input = NULL;
                submit_form(b, in);          /* a click on the submit button */
                return true;
            }
            if (in && layout_node_is_checkable(in)) {
                toggle_control(b, in);       /* checkbox / radio: flip it */
                b->focused_input = NULL;      /* it takes no keystrokes */
                return true;
            }
            if (in) {
                b->focused_input = in;
                field_for(b, in);            /* ensure it has an editable slot */
                return true;
            }
            b->focused_input = NULL;
        }

        if (b->hovered_link) {
            /* Copy it: following a link rebuilds the page, and the string
             * belongs to the document that is about to be freed. */
            char href[ADDRESS_MAX];
            strlcpy(href, b->hovered_link, sizeof href);
            follow_link(b, href);
        }
        return true;
    }

    case WE_MOUSE_WHEEL: {
        if (!b->layout) return false;
        int height = s->height - TOOLBAR_H - STATUS_H;
        int limit = b->layout->height - height;
        if (limit < 0) limit = 0;
        b->scroll -= ev->wheel * 60;
        if (b->scroll < 0) b->scroll = 0;
        if (b->scroll > limit) b->scroll = limit;
        return true;
    }

    case WE_KEY_DOWN: {
        /* The address bar, when it has the keyboard. */
        if (b->address_focused) {
            if (ev->key == '\n' || ev->key == '\r') {
                b->address_focused = false;
                if (b->address_len) {
                    char url[ADDRESS_MAX];
                    strlcpy(url, b->address, sizeof url);
                    navigate(b, url, true);
                }
                return true;
            }
            if (ev->key == 27) { b->address_focused = false; return true; }
            if (ev->key == '\b') {
                if (b->caret > 0) {
                    memmove(b->address + b->caret - 1, b->address + b->caret,
                            (size_t)(b->address_len - b->caret + 1));
                    b->caret--;
                    b->address_len--;
                }
                return true;
            }
            if (ev->key == KK_DELETE) {
                if (b->caret < b->address_len) {
                    memmove(b->address + b->caret, b->address + b->caret + 1,
                            (size_t)(b->address_len - b->caret));
                    b->address_len--;
                }
                return true;
            }
            if (ev->key == KK_LEFT)  { if (b->caret > 0) b->caret--; return true; }
            if (ev->key == KK_RIGHT) { if (b->caret < b->address_len) b->caret++; return true; }
            if (ev->key == KK_HOME)  { b->caret = 0; return true; }
            if (ev->key == KK_END)   { b->caret = b->address_len; return true; }

            if (ev->key >= 32 && ev->key < 127 && b->address_len < ADDRESS_MAX - 1) {
                memmove(b->address + b->caret + 1, b->address + b->caret,
                        (size_t)(b->address_len - b->caret + 1));
                b->address[b->caret] = (char)ev->key;
                b->caret++;
                b->address_len++;
                return true;
            }
            return false;
        }

        /* A focused page input: type into it, Enter submits, Esc drops it. */
        if (b->focused_input) {
            char *val = field_for(b, b->focused_input);
            if (!val) { b->focused_input = NULL; return false; }
            if (ev->key == '\n' || ev->key == '\r') {
                dom_node_t *in = b->focused_input;
                b->focused_input = NULL;
                submit_form(b, in);
                return true;
            }
            if (ev->key == 27) { b->focused_input = NULL; return true; }
            if (ev->key == '\b') {
                size_t n = strlen(val);
                if (n > 0) val[n - 1] = 0;
                return true;
            }
            if (ev->key >= 32 && ev->key < 127) {
                size_t n = strlen(val);
                if (n < 127) { val[n] = (char)ev->key; val[n + 1] = 0; }
                return true;
            }
            return false;
        }

        if ((ev->mods & KMOD_CTRL) && (ev->key == 'l' || ev->key == 'L')) {
            b->address_focused = true;
            b->caret = b->address_len;
            return true;
        }
        if ((ev->mods & KMOD_CTRL) && (ev->key == 'r' || ev->key == 'R')) {
            if (b->address[0]) navigate(b, b->address, false);
            return true;
        }

        if (!b->layout) return false;
        int height = s->height - TOOLBAR_H - STATUS_H;
        int limit = b->layout->height - height;
        if (limit < 0) limit = 0;

        switch (ev->key) {
        case KK_DOWN:     b->scroll += 48; break;
        case KK_UP:       b->scroll -= 48; break;
        case KK_PAGEDOWN: b->scroll += height - 40; break;
        case KK_PAGEUP:   b->scroll -= height - 40; break;
        case ' ':         b->scroll += height - 40; break;
        case KK_HOME:     b->scroll = 0; break;
        case KK_END:      b->scroll = limit; break;
        case '\b': go_back(b); return true;
        default: return false;
        }
        if (b->scroll < 0) b->scroll = 0;
        if (b->scroll > limit) b->scroll = limit;
        return true;
    }

    case WE_CLOSE:
        clear_page(b);
        if (b->default_sheet) css_free(b->default_sheet);
        if (active == b) active = NULL;
        free(b);
        w->data = NULL;
        return false;

    default:
        return false;
    }
}

void app_browser_launch(wm_t *wm) {
    browser_t *b = calloc(1, sizeof *b);
    if (!b) return;

    b->wm = wm;
    b->history_at = -1;

    const char *defaults = css_default_stylesheet();
    b->default_sheet = css_parse(defaults, strlen(defaults));

    b->window = desktop_new_window(wm, "Kestrel", ICON_BROWSER, 980, 700,
                                   browser_proc, b);
    if (!b->window) {
        if (b->default_sheet) css_free(b->default_sheet);
        free(b);
        return;
    }

    active = b;
    navigate(b, "kestrel:start", true);
    b->window->needs_paint = true;
}

/* Opening a particular address from elsewhere - a link in another app, or a
 * command. */
void app_browser_open(wm_t *wm, const char *url) {
    if (!active) app_browser_launch(wm);
    if (active && url && *url) navigate(active, url, true);
}
