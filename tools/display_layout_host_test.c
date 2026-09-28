/* display_layout_host_test.c - the three multi-display modes, off a single head.
 *
 * "extend / mirror / only the other display" is the capability; the geometry
 * behind it (which part of the one framebuffer each output scans, and how big
 * the framebuffer has to be) is pure arithmetic in kernel/display_layout.c and
 * this checks every mode against hand-computed expectations - so it is testable
 * with no second monitor, which on a single-headed build machine or VM is the
 * only way to test it at all.  The header promised this file; here it is.
 *
 * Build + run (from the repo root):
 *   clang -std=c11 -Wall -Wextra tools/display_layout_host_test.c \
 *         kernel/display_layout.c -o display_layout_test
 *   ./display_layout_test
 */
#include "../include/kestrel/display_layout.h"
#include <stdio.h>

static int fails = 0, total = 0;
static void ck(const char *what, int cond) {
    total++;
    if (!cond) { fails++; printf("    FAIL: %s\n", what); }
    else        printf("    ok:   %s\n", what);
}

/* Assert one output's whole scanout state in one call. */
static void ck_out(const char *tag, const dl_scanout_t *s, int active,
                   int sx, int sy, int sw, int sh, int ow, int oh) {
    char b[128];
    snprintf(b, sizeof b, "%s active=%d", tag, active);
    ck(b, s->active == active);
    if (!active) return;
    snprintf(b, sizeof b, "%s src=(%d,%d %dx%d)", tag, sx, sy, sw, sh);
    ck(b, s->src_x == sx && s->src_y == sy && s->src_w == sw && s->src_h == sh);
    snprintf(b, sizeof b, "%s out=%dx%d", tag, ow, oh);
    ck(b, s->out_w == ow && s->out_h == oh);
}

int main(void) {
    dl_layout_t L;

    /* The user's setup: two identical 2560x1440 panels. */
    dl_output_size_t twins[2] = { {2560,1440}, {2560,1440} };

    printf("[two 2560x1440 - ONLY_PRIMARY]\n");
    display_layout_compute(twins, 2, DL_ONLY_PRIMARY, &L);
    ck("fb is the primary 2560x1440", L.fb_width==2560 && L.fb_height==1440);
    ck_out("out0", &L.out[0], 1, 0,0, 2560,1440, 2560,1440);
    ck_out("out1", &L.out[1], 0, 0,0,0,0,0,0);

    printf("[two 2560x1440 - EXTEND]\n");
    display_layout_compute(twins, 2, DL_EXTEND, &L);
    ck("fb spans both: 5120x1440", L.fb_width==5120 && L.fb_height==1440);
    ck_out("out0", &L.out[0], 1, 0,0,    2560,1440, 2560,1440);
    ck_out("out1 tiled right", &L.out[1], 1, 2560,0, 2560,1440, 2560,1440);

    printf("[two 2560x1440 - MIRROR]\n");
    display_layout_compute(twins, 2, DL_MIRROR, &L);
    ck("fb is the primary 2560x1440", L.fb_width==2560 && L.fb_height==1440);
    ck_out("out0", &L.out[0], 1, 0,0, 2560,1440, 2560,1440);
    ck_out("out1 same picture", &L.out[1], 1, 0,0, 2560,1440, 2560,1440);

    printf("[two 2560x1440 - ONLY_SECONDARY]\n");
    display_layout_compute(twins, 2, DL_ONLY_SECONDARY, &L);
    ck("fb is the second's 2560x1440", L.fb_width==2560 && L.fb_height==1440);
    ck_out("out0 (primary) dark", &L.out[0], 0, 0,0,0,0,0,0);
    ck_out("out1 lit", &L.out[1], 1, 0,0, 2560,1440, 2560,1440);

    /* Mixed sizes: primary 2560x1440, second 1920x1080 - exercises mirror
     * scaling and extend's tallest-height framebuffer. */
    dl_output_size_t mixed[2] = { {2560,1440}, {1920,1080} };

    printf("[mixed - EXTEND (tallest sets fb height, shorter top-aligned)]\n");
    display_layout_compute(mixed, 2, DL_EXTEND, &L);
    ck("fb is 4480x1440 (sum wide, tallest high)", L.fb_width==4480 && L.fb_height==1440);
    ck_out("out0", &L.out[0], 1, 0,0,    2560,1440, 2560,1440);
    ck_out("out1 shorter, top-aligned", &L.out[1], 1, 2560,0, 1920,1080, 1920,1080);

    printf("[mixed - MIRROR (second scales the whole primary to its panel)]\n");
    display_layout_compute(mixed, 2, DL_MIRROR, &L);
    ck("fb is the primary 2560x1440", L.fb_width==2560 && L.fb_height==1440);
    ck_out("out0", &L.out[0], 1, 0,0, 2560,1440, 2560,1440);
    /* src is the WHOLE primary; out is the panel's own size - the driver scales. */
    ck_out("out1 src=primary, out=panel", &L.out[1], 1, 0,0, 2560,1440, 1920,1080);

    printf("[mixed - ONLY_SECONDARY (fb takes the second's size)]\n");
    display_layout_compute(mixed, 2, DL_ONLY_SECONDARY, &L);
    ck("fb is the second's 1920x1080", L.fb_width==1920 && L.fb_height==1080);
    ck_out("out0 dark", &L.out[0], 0, 0,0,0,0,0,0);
    ck_out("out1 lit at its size", &L.out[1], 1, 0,0, 1920,1080, 1920,1080);

    /* One output: every mode collapses to only-primary (nothing to extend to). */
    printf("[single output - modes fall back to only-primary]\n");
    dl_output_size_t one[1] = { {3840,2160} };
    dl_mode_t modes[] = { DL_ONLY_PRIMARY, DL_EXTEND, DL_MIRROR, DL_ONLY_SECONDARY };
    for (size_t m = 0; m < sizeof modes/sizeof modes[0]; m++) {
        display_layout_compute(one, 1, modes[m], &L);
        char b[96];
        snprintf(b, sizeof b, "mode %s -> single 3840x2160, out0 whole",
                 display_layout_mode_name(modes[m]));
        ck(b, L.fb_width==3840 && L.fb_height==2160 && L.out[0].active==1 &&
              L.out[0].out_w==3840 && L.out[0].out_h==2160);
    }

    /* Three outputs extend: tiles accumulate left to right. */
    printf("[three outputs - EXTEND tiles accumulate]\n");
    dl_output_size_t three[3] = { {2560,1440}, {2560,1440}, {1920,1080} };
    display_layout_compute(three, 3, DL_EXTEND, &L);
    ck("fb 7040x1440", L.fb_width==(2560+2560+1920) && L.fb_height==1440);
    ck_out("out0 @0",    &L.out[0], 1, 0,0,    2560,1440, 2560,1440);
    ck_out("out1 @2560", &L.out[1], 1, 2560,0, 2560,1440, 2560,1440);
    ck_out("out2 @5120", &L.out[2], 1, 5120,0, 1920,1080, 1920,1080);

    printf("[four outputs - generated single-screen modes]\n");
    dl_output_size_t four[4] = { {2560,1440}, {1920,1080}, {3840,2160}, {1280,1024} };
    display_layout_compute(four, 4, DL_ONLY_OUTPUT(2), &L);
    ck("only screen 3 uses its native 3840x2160 framebuffer",
       L.fb_width==3840 && L.fb_height==2160);
    ck_out("out0 dark", &L.out[0], 0, 0,0,0,0,0,0);
    ck_out("out1 dark", &L.out[1], 0, 0,0,0,0,0,0);
    ck_out("out2 lit", &L.out[2], 1, 0,0,3840,2160,3840,2160);
    ck_out("out3 dark", &L.out[3], 0, 0,0,0,0,0,0);
    display_layout_compute(four, 4, DL_ONLY_OUTPUT(3), &L);
    ck("only screen 4 uses its native 1280x1024 framebuffer",
       L.fb_width==1280 && L.fb_height==1024 && L.out[3].active);
    display_layout_compute(four, 4, DL_ONLY_OUTPUT(7), &L);
    ck("missing screen selection safely falls back to main",
       L.fb_width==2560 && L.fb_height==1440 && L.out[0].active);

    printf("\n%d/%d checks passed%s\n", total - fails, total,
           fails ? "  <<< FAILURE" : "  ALL GOOD");
    return fails ? 1 : 0;
}
