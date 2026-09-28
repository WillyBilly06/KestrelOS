/* nv_accel.c - offering the NVIDIA card's drawing engine to the shell.
 *
 * The system asks whichever adapter registered to fill and copy rectangles for
 * it, and falls back to the processor when none will.  This is the NVIDIA
 * side of that: it registers, and then answers honestly about whether it can
 * actually do the work yet.
 *
 * That distinction is the whole of this file, so it is worth stating plainly.
 * Blackwell's GSP-RM channel and display paths now meet here: once the CA7D
 * modeset binds its VRAM scanout, CAB5 executes fill/copy work and the desktop
 * presents damage through the copy engine.  Before that point the operations
 * report unavailable so the existing CPU fallback remains correct.
 */
#include "kernel.h"
#include "klog.h"
#include "gpu.h"
#include "nv.h"
#include "nvkms_kapi_client.h"

static bool ready(void *context) {
    /* The current channel implementation is still a singleton. Never let a
     * second registered card claim that singleton's readiness or commands. */
    nv_card_t *c = context;
    if (!c || nv_chan_display_card() != c) return false;
    return !nvkms_kapi_runtime_ready() ||
           nvkms_kapi_runtime_matches_gpu(c->pci_bus, c->pci_slot, c->pci_func);
}

static bool nv_accel_can_fill(void *context) { return ready(context); }
static bool nv_accel_can_copy(void *context) { return ready(context); }

static bool nv_accel_fill(void *context, int x, int y, int w, int h, u32 colour) {
    if (!ready(context)) return false;
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return false;

    if (nvkms_kapi_runtime_ready())
        return nvkms_kapi_runtime_fill(x, y, w, h, colour);
    return nv_chan_fill_scanout(x, y, w, h, colour);
}

static bool nv_accel_copy(void *context, int from_x, int from_y, int to_x, int to_y,
                           int w, int h) {
    if (!ready(context)) return false;
    if (from_x < 0 || from_y < 0 || to_x < 0 || to_y < 0 || w <= 0 || h <= 0)
        return false;

    if (nvkms_kapi_runtime_ready())
        return nvkms_kapi_runtime_copy(from_x, from_y, to_x, to_y, w, h);
    return nv_chan_copy_scanout(from_x, from_y, to_x, to_y, w, h);
}

static u32 nv_accel_draw_mode(void *context) {
    return ready(context) && nvkms_kapi_runtime_ready() &&
           nv_chan_raster_ready(context) ? 2u : 0u;
}

static int nv_accel_draw_triangles(void *context, const float *vertices, u32 triangles) {
    if (!nv_accel_draw_mode(context)) return -1;
    return nvkms_kapi_runtime_draw(vertices, triangles);
}

static int nv_accel_draw_image(void *context, const u32 *pixels,
                               u32 width, u32 height, u32 stride,
                               int x, int y, u32 draw_width, u32 draw_height) {
    /* Scaling is not implemented by this upload path. Never redirect an
     * unsupported operation to an unrelated adapter's framebuffer. */
    if (!ready(context) || !nvkms_kapi_runtime_ready() ||
        (draw_width && draw_width != width) ||
        (draw_height && draw_height != height)) return -1;
    return nvkms_kapi_runtime_present(pixels, width, height, stride, x, y) ? 0 : -1;
}

static bool nv_accel_read_pixel(void *context, int x, int y, u32 *pixel) {
    return ready(context) && nvkms_kapi_runtime_ready() &&
           nvkms_kapi_runtime_read_pixel(x, y, pixel);
}

static const gpu_accel_ops_t ops = {
    .can_fill = nv_accel_can_fill,
    .can_copy = nv_accel_can_copy,
    .fill     = nv_accel_fill,
    .copy     = nv_accel_copy,
    .draw_mode = nv_accel_draw_mode,
    .draw_triangles = nv_accel_draw_triangles,
    .draw_image = nv_accel_draw_image,
    .read_pixel = nv_accel_read_pixel,
};

/* Register a stable per-card context; activation is a separate display handoff. */
void nv_accel_init(nv_card_t *c) {
    if (!c) return;

    if (!gpu_accel_register(c->pci_bus, c->pci_slot, c->pci_func,
                            &ops, c, "NVIDIA"))
        kwarn("nv-accel", "could not register accelerator at %02x:%02x.%u",
              c->pci_bus, c->pci_slot, c->pci_func);
}

/* Handed a channel that somebody else managed to open, this is where
 * acceleration begins.  Separate from the above so that the day a channel can
 * be opened, nothing else has to change. */
bool nv_accel_use_channel(nv_card_t *c, const nv_fifo_t *f) {
    (void)f;
    /* Kept for the pre-GSP callers.  Modern Blackwell channels are owned by
     * nv_chan.c and become available through nv_chan_bind_scanout(). */
    return ready(c);
}
