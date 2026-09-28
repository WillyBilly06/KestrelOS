/* aatext.h - text with smooth edges, at any size.
 *
 * See aatext.c.  Four faces, each rasterised once at a nominal size and
 * resampled to whatever a caller asks for.  Coordinates are given as a
 * baseline, which is how text is positioned everywhere else in typography and
 * what makes two different sizes on one line align properly.
 */
#ifndef KESTREL_AATEXT_H
#define KESTREL_AATEXT_H

#include "gui.h"

typedef struct {
    const uint8_t *pixels;      /* 256 glyphs of w*h coverage bytes */
    const uint8_t *width;       /* how far the pen moves, per glyph */
    int  w, h;
    int  baseline;              /* rows from the top of the cell */
    int  nominal;               /* the size it was rasterised at */
    bool fixed_pitch;
} aafont_t;

extern const aafont_t AA_BODY;        /* Segoe UI, for prose            */
extern const aafont_t AA_BODY_BOLD;   /* the same, heavier              */
extern const aafont_t AA_HEADING;     /* large and heavy, for headings  */
extern const aafont_t AA_CODE;        /* fixed pitch, for code          */

int aa_line_height(const aafont_t *font, int size);
int aa_baseline(const aafont_t *font, int size);
int aa_advance(const aafont_t *font, int size, unsigned char ch);
int aa_text_width(const aafont_t *font, int size, const char *text);
int aa_text_width_n(const aafont_t *font, int size, const char *text, size_t n);

/* All of these return the x the pen ended at. */
int aa_text(surface_t *s, const aafont_t *font, int size, int x, int baseline_y,
            const char *text, colour_t colour);
int aa_text_n(surface_t *s, const aafont_t *font, int size, int x, int baseline_y,
              const char *text, size_t n, colour_t colour);
int aa_text_alpha(surface_t *s, const aafont_t *font, int size, int x, int baseline_y,
                  const char *text, size_t n, colour_t colour, int alpha);
int aa_text_clipped(surface_t *s, const aafont_t *font, int size, int x,
                    int baseline_y, int max_width, const char *text,
                    colour_t colour);

/* Release every cached size.  Worth doing after a burst of unusual sizes. */
void aa_forget_cached(void);

#endif
