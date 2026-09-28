/* paint.h - putting a laid-out page on the screen.
 *
 * See paint.c.  Everything is decided by the time this runs; the only inputs
 * beyond the boxes themselves are how far down the page has been scrolled and
 * what the pointer is over.
 */
#ifndef KESTREL_PAINT_H
#define KESTREL_PAINT_H

#include "layout.h"
#include "gui.h"

typedef struct {
    const dom_node_t *hovered_node;   /* NULL when the pointer is elsewhere */
    colour_t          link_hover_colour;
} paint_state_t;

void paint_page(surface_t *s, rect_t area, const layout_t *layout,
                int scroll, const paint_state_t *state);

/* Which element is under a point, in page coordinates. */
const dom_node_t *paint_node_at(const layout_t *layout, int x, int y);

#endif
