/* html_parse_host_test.c - exercise the real html.c tree builder off-target.
 *
 * HTML's tree is mostly inferred: an <li> ends when the next one begins, a
 * nested <ul> lives inside the <li> that holds it, and getting the close rules
 * subtly wrong reparents whole subtrees in a way that only shows up as a page
 * that lays out wrong - the hardest kind of bug to see on real hardware.  This
 * runs the unmodified user/libweb/html.c against literal markup and checks the
 * parent/child shape of the tree it builds, so a list-nesting regression fails
 * here in a millisecond instead of on screen.
 *
 * It caught a real one: `close_implied(TAG_LI)` ran two `open_within` checks
 * that each stopped at only one list container (<ul> OR <ol>) and OR'd them,
 * so inside a <ul> the <ol> check walked straight past the enclosing <ul> and
 * closed the <li> that contained a nested list - flattening the nested items
 * into the outer list and dropping the following sibling out of the list.
 *
 * Build + run (from the repo root):
 *   clang -std=c11 -Wall -Wextra -Wno-unused-function \
 *         -I user/libc -I user/libweb \
 *         tools/html_parse_host_test.c -o html_parse_test
 *   ./html_parse_test
 */

/* The real dom.h includes the OS libc header (kestrel.h); define its guard so
 * that expands to nothing and host libc stands in, exactly as the code under
 * test would see on target. */
#define KESTREL_USER_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef _WIN32
#define strncasecmp _strnicmp
#define strcasecmp  _stricmp
#endif

#include "html.c"   /* the code under test, verbatim */

/* ------------------------------------------------------------------ checks */

static int fails = 0, total = 0;

static void ck(const char *what, int cond) {
    total++;
    if (!cond) { fails++; printf("  FAIL: %s\n", what); }
    else        printf("  ok:   %s\n", what);
}

static const char *pname(const dom_node_t *n) {
    return (n && n->parent && n->parent->name) ? n->parent->name : "(none)";
}

/* The <li> whose first text child is exactly `s`. */
static const dom_node_t *li_text(const dom_node_t *root, const char *s) {
    for (const dom_node_t *x = root; x; x = dom_next(x, root)) {
        if (x->kind != NODE_ELEMENT || !x->name || strcmp(x->name, "li")) continue;
        const dom_node_t *t = x->first_child;
        while (t && t->kind != NODE_TEXT) t = t->next;
        if (t && t->text && !strcmp(t->text, s)) return x;
    }
    return NULL;
}

static int count_tag(const dom_node_t *root, const char *name) {
    int n = 0;
    for (const dom_node_t *x = root; x; x = dom_next(x, root))
        if (x->kind == NODE_ELEMENT && x->name && !strcmp(x->name, name)) n++;
    return n;
}

int main(void) {
    /* 1. A nested <ul> inside an <li>: the nested items stay inside it, and the
     *    sibling after the nested list stays in the outer list. */
    {
        const char *h = "<html><body><ul>"
            "<li>Alpha</li>"
            "<li>Beta<ul><li>Beta-1</li><li>Beta-2</li></ul></li>"
            "<li>Gamma</li></ul></body></html>";
        document_t *d = html_parse(h, strlen(h));
        const dom_node_t *b1 = li_text(d->root, "Beta-1");
        const dom_node_t *g  = li_text(d->root, "Gamma");
        const dom_node_t *be = li_text(d->root, "Beta");
        printf("[nested ul]\n");
        ck("Beta-1's parent is <ul>", b1 && !strcmp(pname(b1), "ul"));
        ck("Beta-1 nests under <li>Beta",
           b1 && b1->parent && b1->parent->parent == be);
        ck("Gamma's parent is <ul>", g && !strcmp(pname(g), "ul"));
        ck("Beta and Gamma share the outer <ul>",
           be && g && be->parent == g->parent);
        document_free(d);
    }

    /* 2. A flat list with unclosed <li>: siblings must still auto-close. */
    {
        const char *h = "<html><body><ul><li>One<li>Two<li>Three</ul></body></html>";
        document_t *d = html_parse(h, strlen(h));
        const dom_node_t *o = li_text(d->root, "One"),
                         *t = li_text(d->root, "Two"),
                         *r = li_text(d->root, "Three");
        printf("[flat ul, unclosed li]\n");
        ck("three siblings under one <ul>",
           o && t && r && o->parent == t->parent && t->parent == r->parent);
        ck("Two is not a child of One", t && t->parent != o);
        document_free(d);
    }

    /* 3. Nested <ol>. */
    {
        const char *h = "<html><body><ol><li>a<ol><li>a1</li></ol></li>"
                        "<li>b</li></ol></body></html>";
        document_t *d = html_parse(h, strlen(h));
        const dom_node_t *a1 = li_text(d->root, "a1"),
                         *b  = li_text(d->root, "b"),
                         *a  = li_text(d->root, "a");
        printf("[nested ol]\n");
        ck("a1's parent is <ol>", a1 && !strcmp(pname(a1), "ol"));
        ck("a1 nests under <li>a", a1 && a1->parent && a1->parent->parent == a);
        ck("b is sibling of a in the outer <ol>", b && a && b->parent == a->parent);
        document_free(d);
    }

    /* 4. Three levels deep, with a sibling after the whole nest. */
    {
        const char *h = "<html><body><ul><li>L1<ul><li>L2<ul><li>L3</li></ul>"
                        "</li></ul></li><li>after</li></ul></body></html>";
        document_t *d = html_parse(h, strlen(h));
        const dom_node_t *l3 = li_text(d->root, "L3"),
                         *af = li_text(d->root, "after"),
                         *l1 = li_text(d->root, "L1");
        printf("[three-level]\n");
        ck("L3 is <ul>>li>ul>li>ul>li deep",
           l3 && l3->parent && l3->parent->parent &&
           l3->parent->parent->parent &&
           !strcmp(l3->parent->parent->parent->name, "ul"));
        ck("'after' is a sibling of L1 in the outer <ul>",
           af && l1 && af->parent == l1->parent);
        document_free(d);
    }

    /* 5. Table row/cell auto-close (the sibling code to the <li> rule). */
    {
        const char *h = "<html><body><table><tr><td>A<td>B<tr><td>C"
                        "</table></body></html>";
        document_t *d = html_parse(h, strlen(h));
        printf("[table auto-close]\n");
        ck("2 rows", count_tag(d->root, "tr") == 2);
        ck("3 cells", count_tag(d->root, "td") == 3);
        document_free(d);
    }

    printf("\n%d/%d checks passed%s\n", total - fails, total,
           fails ? "  <<< REGRESSION" : "  ALL GOOD");
    return fails ? 1 : 0;
}
