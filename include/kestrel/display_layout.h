/* display_layout.h - how several monitors share one framebuffer.
 *
 * A machine with two outputs still has ONE picture in memory; what differs
 * between "extend", "mirror" and "show only the other screen" is which part of
 * that picture each output scans out, and how big the picture has to be.  This
 * is the pure geometry of that decision, with no hardware in it: give it the
 * native size of each output and a mode, and it returns the framebuffer to
 * allocate and, per output, the source rectangle to scan and whether it is on.
 *
 * The display driver then programs each CRTC from the result; the compositor
 * lays out windows across `fb_width x fb_height`; the settings page offers the
 * mode.  Splitting the geometry out means it can be tested without a second
 * monitor (display_layout_host_test.c), which on a single-headed VM is the
 * only way to test it at all.
 */
#ifndef KESTREL_DISPLAY_LAYOUT_H
#define KESTREL_DISPLAY_LAYOUT_H

#define DISPLAY_MAX_OUTPUTS 8

/* Group modes plus a generated "only output N" range.  The range avoids an
 * enum member per connector: hot-plugging an eighth screen automatically makes
 * DL_ONLY_OUTPUT(7) valid without changing this ABI. */
typedef enum {
    DL_ONLY_PRIMARY = 0,   /* one output, or the others turned off        */
    DL_EXTEND,             /* the desktop spans every output side by side */
    DL_MIRROR,             /* every output shows the same picture         */
    DL_ONLY_SECONDARY,     /* the primary is dark; the second shows       */
    DL_ONLY_OUTPUT_BASE = 16,
} dl_mode_t;
#define DL_ONLY_OUTPUT(index) ((dl_mode_t)(DL_ONLY_OUTPUT_BASE + (index)))
#define DL_ONLY_OUTPUT_INDEX(mode) ((int)(mode) - (int)DL_ONLY_OUTPUT_BASE)

typedef struct {
    int width, height;      /* an output's own native size (what it displays) */
} dl_output_size_t;

typedef struct {
    int active;             /* does this output light up in this mode?     */
    int src_x, src_y;       /* top-left of the region it scans, in the fb  */
    int src_w, src_h;       /* size of that region (== native unless mirror
                             * scales a differently-sized panel)           */
    int out_w, out_h;       /* the size it actually displays at (native)   */
    int rotation;          /* clockwise logical-image rotation: 0/90/180/270 */
} dl_scanout_t;

typedef struct {
    int fb_width, fb_height;              /* the framebuffer to allocate    */
    int n;                                /* number of outputs described    */
    int origin_x, origin_y;               /* signed desktop origin of fb(0,0) */
    dl_scanout_t out[DISPLAY_MAX_OUTPUTS];
} dl_layout_t;

typedef struct {
    int x, y;                  /* signed position in the logical desktop */
    int width, height;          /* physical scanout dimensions */
    int rotation;              /* 0, 90, 180 or 270 degrees clockwise */
    int active;
} dl_placement_t;

/* Validate/build a proposed layout without modifying live state. Positions
 * apply in Extend; Mirror samples the primary's complete logical viewport.
 * Explicit Only-output indices are connector ordinals (not primary-first).
 * Reject invalid/overlapping Extend rectangles and oversized framebuffers;
 * gaps are legal and the window/input layer must keep content reachable.
 * Returns 1 on success; failure leaves *out untouched. */
int display_layout_arrange(const dl_placement_t *outputs, int n, int primary,
                           dl_mode_t mode, int max_width, int max_height,
                           dl_layout_t *out);

/* Compute the layout.  `outputs[0]` is the primary.  `n` is clamped to
 * [1, DISPLAY_MAX_OUTPUTS].  DL_ONLY_SECONDARY with n < 2 falls back to
 * DL_ONLY_PRIMARY (there is no second output to move to).  Never fails. */
void display_layout_compute(const dl_output_size_t *outputs, int n,
                            dl_mode_t mode, dl_layout_t *out);

/* The mode's short name, for logs and the settings page. */
const char *display_layout_mode_name(dl_mode_t mode);

/* Map logical framebuffer damage to the exact output pixels which sample it
 * under nearest-neighbour viewport scaling. 1=nonempty, 0=empty/inactive,
 * -1=invalid geometry. Outputs remain unchanged unless the result is 1. */
int display_layout_damage(const dl_scanout_t *scanout,
                          int x, int y, int w, int h,
                          int *out_x, int *out_y, int *out_w, int *out_h);

#endif
