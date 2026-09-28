/* svga.h - the VMware display adapter.
 *
 * See svga.c.  Three things come out of driving this device rather than
 * writing into whatever framebuffer the firmware left behind: the screen can
 * change resolution while the system is running, the pointer stops costing
 * anything to move, and - the reason this was written - telling the adapter
 * what changed is enormously faster than letting it find out.
 */
#ifndef KESTREL_SVGA_H
#define KESTREL_SVGA_H

#include "kernel.h"

/* Take over the display, if this machine has one of these.  Returns false when
 * it does not, and the firmware's framebuffer stays in use. */
bool svga_init(void);

/* Take the adapter's word for where the framebuffer is and how it is laid
 * out, now that it is being driven rather than inherited. */
void svga_adopt_framebuffer(void);

bool svga_present(void);
const char *svga_name(void);

/* Tell the adapter which part of the screen changed.  Everything else this
 * driver does is a convenience; this is the part that matters. */
void svga_update(int x, int y, int w, int h);

/* Change resolution while running.  Returns false if the adapter will not take
 * it, leaving the previous mode in place. */
bool svga_set_mode(u32 width, u32 height);

/* The largest the adapter will go, and how much video memory it has. */
void svga_limits(u32 *max_width, u32 *max_height, u32 *vram_bytes);

/* A pointer the adapter draws itself, so moving it costs nothing. */
bool svga_cursor_define(const u32 *argb, int w, int h, int hot_x, int hot_y);
void svga_cursor_move(int x, int y);
void svga_cursor_show(bool on);
bool svga_cursor_available(void);

/* Fill a rectangle, and copy one, using the adapter rather than the
 * processor - where this adapter says it can. */
/* What the three-dimensional engine needs: the ring, and the device's own
 * answers about what it will do. */
bool svga_fifo_raw(const u32 *words, u32 count);
void svga_fifo_sync(void);
u32  svga_devcap(u32 index);
/* Zero when the adapter has retired the original drawing interface. */
u32  svga_legacy_3d_version(void);
u32  svga_num_displays(void);
void svga_set_present_size(u32 w, u32 h, u32 pitch);
bool svga_gbobjects(void);
bool svga_command_buffers(void);
void svga_reg_write(u32 index, u32 value);
u32  svga_reg_read_ext(u32 index);
int  svga3d_gb_selftest(void);
int  svga3d_render_selftest(void);
/* The display reading out of memory this system owns and handed to the card,
 * so presenting a frame is a command rather than a copy. */
bool svga3d_screen_attach(u32 width, u32 height);
bool svga3d_screen_attached(void);
u64  svga3d_screen_memory(u32 *width, u32 *height, u32 *pitch);
void svga3d_screen_update(u32 x, u32 y, u32 w, u32 h);
u32  svga3d_screen_presents(void);
/* A second, independent display (#7): its own screen target, surface and
 * memory beside the primary's, so the card drives two real scan-outs. */
bool svga3d_screen_attach_second(u32 width, u32 height, u32 x_root);
bool svga3d_second_attached(void);
void svga3d_second_update(u32 x, u32 y, u32 w, u32 h);
void svga3d_second_screen_selftest(void);
/* The multi-display mode for the second screen, numbered as dl_mode_t:
 * 0 only-primary, 1 extend, 2 mirror, 3 only-secondary. */
void svga3d_set_second_mode(int mode);
int  svga3d_second_mode(void);
/* Extend: allocate a desktop as wide as both displays; hands back its physical
 * base + size for the caller to point the framebuffer at.  Both screens must be
 * attached.  The present path then splits each frame across the two. */
bool svga3d_extend_setup(u64 *out_phys, u32 *out_w, u32 *out_h, u32 *out_pitch);
bool svga3d_extend_active(void);   /* extend is set up (wide framebuffer) */
/* Drawing a program can ask for: corners in, pixels on the screen, done by the
 * card.  Eight numbers to a vertex - where it is and what colour it carries. */
bool svga3d_can_draw(void);
int  svga3d_draw_user(const float *verts, u32 triangles);
/* A rectangle of pixels put on the screen at a place and a size, by the card,
 * with what is behind it showing through where it is not solid. */
int  svga3d_draw_image_strided(const u32 *pixels, u32 iw, u32 ih, u32 src_stride,
                               int x, int y, u32 dw, u32 dh);
int  svga3d_draw_image(const u32 *pixels, u32 iw, u32 ih,
                       int x, int y, u32 dw, u32 dh);
/* How a program lays its own vertices out, rather than being made to match. */
int  svga3d_set_layout(const u32 *elements, u32 count, u32 stride);
/* A program's own shaders, as the instructions its compiler produced. */
int  svga3d_set_shaders(const u32 *vs, u32 vs_words,
                        const u32 *ps, u32 ps_words,
                        const u32 *vs_takes, u32 vs_takes_n,
                        const u32 *vs_gives, u32 vs_gives_n,
                        const u32 *ps_takes, u32 ps_takes_n,
                        const u32 *ps_gives, u32 ps_gives_n);
void svga_mode(u32 *width, u32 *height, u32 *pitch);
u32  svga_read_pixel(int x, int y);
void svga_write_pixel(int x, int y, u32 colour);

/* Drawing on the host's own graphics card.  See svga3d.c. */
bool svga3d_available(void);
bool svga3d_begin(u32 width, u32 height);
void svga3d_end(void);
bool svga3d_clear(u32 colour);
bool svga3d_read_back(u32 fb_offset, u32 pitch, u32 width, u32 height);
bool svga3d_present(int x, int y, u32 width, u32 height);
void svga3d_counts(int *contexts, int *surfaces, int *clears, int *reads);
int  svga3d_selftest(void);

bool svga_can_fill(void);
bool svga_can_copy(void);
void svga_accel_counts(u32 *fills, u32 *copies, u64 *pixels);
bool svga_fill(int x, int y, int w, int h, u32 colour);
bool svga_copy(int from_x, int from_y, int to_x, int to_y, int w, int h);

int svga_selftest(void);

#endif
