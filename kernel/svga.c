/* svga.c - a driver for the VMware display adapter.
 *
 * Until now the screen has been whatever the firmware left behind: a block of
 * memory that happens to be scanned out, written to directly, with nothing on
 * the other end being told anything. That works, and on real hardware it is
 * reasonably fast, because a write to video memory is a write to video memory.
 *
 * Under a hypervisor it is not, and the reason is worth stating because it is
 * invisible from the code. Something has to notice when the guest changes the
 * picture. With no driver, the only way to notice is to take the framebuffer's
 * pages away from the guest and wait for it to fault on them - so the first
 * write to each page, every frame, is not a store but a trap into the
 * hypervisor. Measured here, a copy of twelve hundred bytes took two hundred
 * microseconds the first time and none at all the second. That is not
 * bandwidth; that is a page being handed back.
 *
 * The whole point of this driver is the single command that ends it: after
 * writing pixels, say which rectangle changed. Then nothing has to be guessed,
 * the pages stay with the guest, and the copy costs what a copy costs.
 *
 * Everything else here follows from having the adapter's attention: the mode
 * can be changed while the system is running instead of only at start-up, the
 * pointer can be handed over so that moving it draws nothing, and rectangles
 * can be filled and copied by the adapter instead of by the processor.
 *
 * The interface is the one VMware documents and publishes: a pair of I/O ports
 * carrying an index and a value, a linear framebuffer, and a ring of commands
 * in a second region of memory.
 */
#include "kernel.h"
#include "gpu.h"
#include "klog.h"
#include "pci.h"
#include "mm.h"
#include "time.h"
#include "svga.h"

/* -------------------------------------------------------------- the device */

#define SVGA_VENDOR 0x15AD
#define SVGA_DEVICE 0x0405

/* Two ports: write an index to the first, read or write the value at the
 * second. Every register below is reached that way.
 *
 * How far apart they sit is the one thing here worth being careful about. The
 * published headers give the two as 0 and 1, which reads like a byte apart,
 * and the accesses are thirty-two bits wide, which reads like four. Rather
 * than pick one and be wrong on a machine nobody tested, both are tried: the
 * adapter refuses to store an identifier it does not understand, so whichever
 * layout answers the handshake is the right one. */
static u32 port_index_offset = 0;
static u32 port_value_offset = 1;

enum {
    REG_ID = 0,
    REG_ENABLE = 1,
    REG_WIDTH = 2,
    REG_HEIGHT = 3,
    REG_MAX_WIDTH = 4,
    REG_MAX_HEIGHT = 5,
    REG_DEPTH = 6,
    REG_BITS_PER_PIXEL = 7,
    REG_PSEUDOCOLOR = 8,
    REG_RED_MASK = 9,
    REG_GREEN_MASK = 10,
    REG_BLUE_MASK = 11,
    REG_BYTES_PER_LINE = 12,
    REG_FB_START = 13,
    REG_FB_OFFSET = 14,
    REG_VRAM_SIZE = 15,
    REG_FB_SIZE = 16,
    REG_CAPABILITIES = 17,
    REG_MEM_START = 18,
    REG_MEM_SIZE = 19,
    REG_CONFIG_DONE = 20,
    REG_SYNC = 21,
    REG_BUSY = 22,
    REG_GUEST_ID = 23,
    REG_CURSOR_ID = 24,
    REG_CURSOR_X = 25,
    REG_CURSOR_Y = 26,
    REG_CURSOR_ON = 27,
    REG_HOST_BITS_PER_PIXEL = 28,
    REG_SCRATCH_SIZE = 29,
    REG_MEM_REGS = 30,
    REG_NUM_DISPLAYS = 31,
    REG_PITCHLOCK = 32,
    REG_TRACES = 45,
    /* On a modern adapter the old three-dimensional capability bit is retired
     * and what the drawing engine can do is asked for one question at a time:
     * write which capability, read the answer. */
    REG_DEV_CAP = 52,
    /* Who the guest says it is.  The adapter keeps real resources for a
     * guest that has identified itself and not for one that has not. */
    REG_GUEST_DRIVER_ID = 61,
    REG_GUEST_DRIVER_VERSION1 = 62,
    REG_GUEST_DRIVER_VERSION2 = 63,
    REG_GUEST_DRIVER_VERSION3 = 64,
};

/* The adapter announces which version of the interface it speaks by refusing
 * to store an identifier it does not understand. */
#define SVGA_MAGIC   0x900000u
#define SVGA_ID(v)   ((SVGA_MAGIC << 8) | (v))
#define SVGA_ID_2    SVGA_ID(2)
#define SVGA_ID_1    SVGA_ID(1)
#define SVGA_ID_0    SVGA_ID(0)

#define CAP_RECT_COPY     0x00000002u
#define CAP_3D            0x00004000u
#define CAP_GBOBJECTS     0x08000000u
#define CAP_DX            0x10000000u
#define CAP_COMMAND_BUFFERS 0x01000000u
#define GUEST_DRIVER_ID_LINUX  2
#define GUEST_DRIVER_ID_SUBMIT 0xFFFFFFFFu
#define CAP_RECT_FILL     0x00000004u
#define CAP_CURSOR        0x00000020u
#define CAP_ALPHA_CURSOR  0x00000200u
#define CAP_EXTENDED_FIFO 0x00008000u
#define CAP_PITCHLOCK     0x00020000u
#define CAP_TRACES        0x00200000u

/* The command ring is a region of memory whose first few words describe it. */
enum {
    FIFO_MIN = 0,
    FIFO_MAX = 1,
    FIFO_NEXT_CMD = 2,
    FIFO_STOP = 3,
    FIFO_CAPABILITIES = 4,
    FIFO_FLAGS = 5,
    FIFO_FENCE = 6,
    /* What the adapter's three-dimensional engine is, and what it can do.  It
     * is not a separate device: the adapter forwards drawing to whatever real
     * graphics card the host has, so this is the version of that path rather
     * than of any silicon. */
    FIFO_3D_HWVERSION = 7,
    /* And where it moved to.  When the ring advertises the revised layout the
     * original register is left at zero and the real version is here - so a
     * driver that reads the obvious one concludes there is no engine while
     * looking straight at a working one. */
    FIFO_3D_HWVERSION_REVISED = 17,
    FIFO_3D_CAPS = 32,
    FIFO_3D_CAPS_LAST = 32 + 255,
    FIFO_GUEST_3D_HWVERSION = 288,
};

#define FIFO_CAP_FENCE 0x00000001u
#define FIFO_CAP_3D_HWVERSION_REVISED 0x00000100u

/* The version of the drawing interface this driver is written to. */
#define SVGA3D_HWVERSION_WE_SPEAK 0x00020000u

/* Commands. Only the ones this driver has a use for. */
#define CMD_UPDATE               1
#define CMD_RECT_FILL            2
#define CMD_RECT_COPY            3
#define CMD_DEFINE_CURSOR        19
#define CMD_DEFINE_ALPHA_CURSOR  22
#define CMD_FENCE                30

/* --------------------------------------------------------------- the state */

static struct {
    bool present;
    bool enabled;

    pci_dev_t *pci;
    u16  io_base;

    u64  fb_phys;
    u32  fb_size;
    u32  vram_size;

    volatile u32 *fifo;
    u32  fifo_size;

    u32  version;
    u32  caps;
    u32  fifo_caps;
    u32  hw3d;

    u32  width, height, pitch, bpp;
    u32  max_width, max_height;
    u32  num_displays;      /* independent displays the adapter exposes (#7) */

    bool cursor_defined;
    bool cursor_on;
    char name[48];
} svga;

static void reg_write(u32 index, u32 value) {
    outl((u16)(svga.io_base + port_index_offset), index);
    outl((u16)(svga.io_base + port_value_offset), value);
}

static u32 reg_read(u32 index) {
    outl((u16)(svga.io_base + port_index_offset), index);
    return inl((u16)(svga.io_base + port_value_offset));
}

/* ----------------------------------------------------------- the command ring
 *
 * A ring buffer of thirty-two bit words in memory the adapter can see. Writing
 * a command means placing its words where NEXT_CMD points and then moving
 * NEXT_CMD past them - in that order, because the adapter may be reading at
 * any moment and must never see a length before the words it counts.
 */
static bool fifo_ready(void) { return svga.fifo != NULL; }

static void fifo_reserve_write(const u32 *words, u32 count) {
    if (!fifo_ready() || !count) return;

    u32 min = svga.fifo[FIFO_MIN];
    u32 max = svga.fifo[FIFO_MAX];
    u32 next = svga.fifo[FIFO_NEXT_CMD];

    for (u32 i = 0; i < count; i++) {
        /* Wait for room. The adapter consumes from STOP; when the two meet
         * with nothing between them the ring is full, and the only thing to do
         * is ask the adapter to catch up. */
        u32 guard = 0;
        while (((next + sizeof(u32)) == svga.fifo[FIFO_STOP]) ||
               (svga.fifo[FIFO_STOP] == min &&
                next + sizeof(u32) == max)) {
            reg_write(REG_SYNC, 1);
            (void)reg_read(REG_BUSY);
            if (++guard > 100000) return;      /* it is not answering */
        }

        svga.fifo[next / sizeof(u32)] = words[i];

        next += sizeof(u32);
        if (next == max) next = min;
    }

    /* Only now is the command whole, and only now may the adapter see it. */
    __asm__ volatile("" ::: "memory");
    svga.fifo[FIFO_NEXT_CMD] = next;
}

static void fifo_command(u32 command, const u32 *args, u32 arg_count) {
    u32 words[16];
    if (arg_count > 15) return;
    words[0] = command;
    for (u32 i = 0; i < arg_count; i++) words[1 + i] = args[i];
    fifo_reserve_write(words, arg_count + 1);
}

/* Ask the adapter to finish what it has been given. Used when something has to
 * have happened before going on - defining a cursor, changing a mode. */
static void fifo_sync(void) {
    if (!svga.present) return;
    reg_write(REG_SYNC, 1);
    u32 guard = 0;
    while (reg_read(REG_BUSY) && ++guard < 1000000) { }
}

/* ------------------------------------------------------------ what changed */

void svga_update(int x, int y, int w, int h) {
    if (!svga.enabled || !fifo_ready()) return;
    if (w <= 0 || h <= 0) return;

    /* Clamp: a rectangle that runs off the screen is a request the adapter is
     * entitled to refuse in any way it likes, including badly. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int)svga.width || y >= (int)svga.height) return;
    if (x + w > (int)svga.width) w = (int)svga.width - x;
    if (y + h > (int)svga.height) h = (int)svga.height - y;
    if (w <= 0 || h <= 0) return;

    /* Where the display reads out of memory this system handed the card,
     * saying which rectangle changed is the whole of presenting it - there is
     * nothing to copy, because what was drawn was drawn in place. */
    if (svga3d_screen_attached()) {
        svga3d_screen_update((u32)x, (u32)y, (u32)w, (u32)h);
        return;
    }

    u32 args[4] = { (u32)x, (u32)y, (u32)w, (u32)h };
    fifo_command(CMD_UPDATE, args, 4);
}

/* ------------------------------------------------- what the three-dimensional
 *                                                    engine needs from here
 *
 * The engine's commands go into the same ring as everything else, so it needs
 * a way to put words in it and a way to wait; and it needs to ask the device
 * questions one at a time, which is how a modern adapter reports what it will
 * do rather than through one capability word.
 */
bool svga_fifo_raw(const u32 *words, u32 count) {
    if (!svga.enabled || !fifo_ready() || !count) return false;
    fifo_reserve_write(words, count);
    return true;
}

void svga_fifo_sync(void) {
    if (!svga.enabled || !fifo_ready()) return;
    fifo_sync();
}

u32 svga_legacy_3d_version(void) { return svga.hw3d; }

/* How many independent displays the adapter offers - 1 unless the VM was told
 * to give more (svga.numDisplays), or, on hardware, however many the card has. */
u32 svga_num_displays(void) { return svga.present ? svga.num_displays : 0; }

/* Tell the driver the size of the picture the window system is presenting, when
 * it is not the mode the adapter was set to - which is exactly the extend case,
 * where the logical desktop is as wide as two displays together and a present
 * rectangle may reach past the first.  Without this svga_update would clamp
 * every rectangle to the primary's width and the second display would never see
 * a change. */
void svga_set_present_size(u32 w, u32 h, u32 pitch) {
    if (!svga.present || !w || !h) return;
    svga.width = w;
    svga.height = h;
    svga.pitch = pitch ? pitch : w * 4;
}

/* Writing a register from outside this file, for the command-buffer path -
 * which hands the card an address in a register rather than through the ring. */
u32 svga_reg_read_ext(u32 index) {
    if (!svga.present) return 0;
    return reg_read(index);
}

void svga_reg_write(u32 index, u32 value) {
    if (!svga.present) return;
    reg_write(index, value);
}

/* Whether the adapter takes command buffers, which is how anything addressed
 * to a drawing context reaches it. */
bool svga_command_buffers(void) {
    return svga.present && (svga.caps & CAP_COMMAND_BUFFERS) != 0;
}

/* Whether the adapter has the interface that replaced it. */
bool svga_gbobjects(void) {
    return svga.present && (svga.caps & CAP_GBOBJECTS) != 0;
}

u32 svga_devcap(u32 index) {
    if (!svga.present) return 0;
    if (!(svga.caps & (CAP_GBOBJECTS | CAP_DX | CAP_3D))) return 0;
    reg_write(REG_DEV_CAP, index);
    return reg_read(REG_DEV_CAP);
}

void svga_mode(u32 *width, u32 *height, u32 *pitch) {
    if (width) *width = svga.width;
    if (height) *height = svga.height;
    if (pitch) *pitch = svga.pitch;
}

/* One pixel, read back through the framebuffer.  Used to check that what the
 * card was asked to draw actually arrived. */
u32 svga_read_pixel(int x, int y) {
    if (!svga.enabled || !g_boot.fb.base) return 0;
    if (x < 0 || y < 0 || (u32)x >= svga.width || (u32)y >= svga.height) return 0;

    static volatile u8 *mapped;
    static u64 mapped_phys;
    if (!mapped || mapped_phys != svga.fb_phys) {
        mapped = vmm_map_mmio(svga.fb_phys, (size_t)svga.pitch * svga.height);
        mapped_phys = svga.fb_phys;
    }
    if (!mapped) return 0;

    return *(volatile u32 *)(mapped + (size_t)y * svga.pitch + (size_t)x * 4);
}

void svga_write_pixel(int x, int y, u32 colour) {
    if (!svga.enabled || !g_boot.fb.base) return;
    if (x < 0 || y < 0 || (u32)x >= svga.width || (u32)y >= svga.height) return;

    volatile u8 *at = vmm_map_mmio(svga.fb_phys, (size_t)svga.pitch * svga.height);
    if (!at) return;
    *(volatile u32 *)(at + (size_t)y * svga.pitch + (size_t)x * 4) = colour;
}

/* ------------------------------------------------------------ acceleration
 *
 * What the adapter will do without the processor touching a pixel.  A caller
 * asks first rather than trying and seeing, because a fill that quietly did
 * nothing would leave the screen wrong with nothing to show why.
 */
bool svga_can_fill(void) {
    return svga.enabled && (svga.caps & CAP_RECT_FILL) && fifo_ready();
}

bool svga_can_copy(void) {
    return svga.enabled && (svga.caps & CAP_RECT_COPY) && fifo_ready();
}

/* How much work has been handed over, so that "it is accelerated" is a number
 * rather than a claim. */
static u32 accelerated_fills, accelerated_copies;
static u64 accelerated_pixels;

void svga_accel_counts(u32 *fills, u32 *copies, u64 *pixels) {
    if (fills) *fills = accelerated_fills;
    if (copies) *copies = accelerated_copies;
    if (pixels) *pixels = accelerated_pixels;
}

bool svga_fill(int x, int y, int w, int h, u32 colour) {
    if (!svga.enabled || !fifo_ready()) return false;
    if (!(svga.caps & CAP_RECT_FILL)) return false;
    if (w <= 0 || h <= 0) return false;

    u32 args[5] = { colour, (u32)x, (u32)y, (u32)w, (u32)h };
    fifo_command(CMD_RECT_FILL, args, 5);
    accelerated_fills++;
    accelerated_pixels += (u64)w * (u64)h;
    return true;
}

bool svga_copy(int from_x, int from_y, int to_x, int to_y, int w, int h) {
    if (!svga.enabled || !fifo_ready()) return false;
    if (!(svga.caps & CAP_RECT_COPY)) return false;
    if (w <= 0 || h <= 0) return false;

    u32 args[6] = { (u32)from_x, (u32)from_y, (u32)to_x, (u32)to_y,
                    (u32)w, (u32)h };
    fifo_command(CMD_RECT_COPY, args, 6);
    accelerated_copies++;
    accelerated_pixels += (u64)w * (u64)h;
    return true;
}

/* ---------------------------------------------------------------- the pointer
 *
 * Handed to the adapter, which draws it over the picture without the picture
 * knowing. That removes the whole business of saving what is under the pointer
 * and putting it back - and, more to the point here, it means moving the mouse
 * changes nothing in the framebuffer at all.
 */
bool svga_cursor_available(void) {
    return svga.enabled && (svga.caps & CAP_ALPHA_CURSOR) && fifo_ready();
}

bool svga_cursor_define(const u32 *argb, int w, int h, int hot_x, int hot_y) {
    if (!svga_cursor_available()) return false;
    if (w <= 0 || h <= 0 || w > 64 || h > 64) return false;

    /* The header, then the image, one word per pixel, alpha in the top byte.
     * Sixteen kilobytes of it, so not on the stack: this runs underneath a
     * system call, on the stack the filesystem and disk code share. */
    static u32 words[1 + 5 + 64 * 64];
    u32 header[5] = { 0, (u32)hot_x, (u32)hot_y, (u32)w, (u32)h };
    u32 at = 0;

    words[at++] = CMD_DEFINE_ALPHA_CURSOR;
    for (int i = 0; i < 5; i++) words[at++] = header[i];
    for (int i = 0; i < w * h; i++) words[at++] = argb[i];

    fifo_reserve_write(words, at);
    fifo_sync();

    svga.cursor_defined = true;
    return true;
}

void svga_cursor_move(int x, int y) {
    if (!svga.enabled || !svga.cursor_defined) return;
    reg_write(REG_CURSOR_ID, 0);
    reg_write(REG_CURSOR_X, (u32)(x < 0 ? 0 : x));
    reg_write(REG_CURSOR_Y, (u32)(y < 0 ? 0 : y));
    reg_write(REG_CURSOR_ON, svga.cursor_on ? 1 : 0);
}

void svga_cursor_show(bool on) {
    if (!svga.enabled || !svga.cursor_defined) return;
    svga.cursor_on = on;
    reg_write(REG_CURSOR_ID, 0);
    reg_write(REG_CURSOR_ON, on ? 1 : 0);
}

/* ------------------------------------------------------------------ modes */

static void describe_mode(void) {
    svga.width = reg_read(REG_WIDTH);
    svga.height = reg_read(REG_HEIGHT);
    svga.pitch = reg_read(REG_BYTES_PER_LINE);
    svga.bpp = reg_read(REG_BITS_PER_PIXEL);
    svga.fb_size = reg_read(REG_FB_SIZE);

    u32 offset = reg_read(REG_FB_OFFSET);
    svga.fb_phys = reg_read(REG_FB_START) + offset;
}

bool svga_set_mode(u32 width, u32 height) {
    if (!svga.present) return false;
    if (width < 320 || height < 200) return false;
    if (width > svga.max_width || height > svga.max_height) {
        kwarn("svga", "%ux%u is past what this adapter will do (%ux%u)",
              width, height, svga.max_width, svga.max_height);
        return false;
    }

    /* And it has to fit in video memory, with the whole picture scanned out of
     * it.  Asking for a mode the memory cannot hold gets a picture made of
     * whatever else is in there. */
    u64 needed = (u64)width * height * 4ull;
    if (svga.vram_size && needed > svga.vram_size) {
        kwarn("svga", "%ux%u needs %lu MiB and this adapter has %u",
              width, height, (unsigned long)(needed / (1024 * 1024)),
              svga.vram_size / (1024 * 1024));
        return false;
    }

    /* The picture must stop before its shape changes, or the adapter is
     * scanning out of memory that is being reorganised underneath it. */
    reg_write(REG_ENABLE, 0);
    reg_write(REG_WIDTH, width);
    reg_write(REG_HEIGHT, height);
    reg_write(REG_BITS_PER_PIXEL, 32);
    reg_write(REG_ENABLE, 1);

    describe_mode();

    if (svga.width != width || svga.height != height) {
        kwarn("svga", "asked for %ux%u and got %ux%u", width, height,
              svga.width, svga.height);
        return false;
    }

    /* The adapter no longer has to watch for changes: it will be told. */
    if (svga.caps & CAP_TRACES) reg_write(REG_TRACES, 0);

    svga.enabled = true;
    return true;
}

void svga_limits(u32 *max_width, u32 *max_height, u32 *vram_bytes) {
    if (max_width) *max_width = svga.max_width;
    if (max_height) *max_height = svga.max_height;
    if (vram_bytes) *vram_bytes = svga.vram_size;
}

bool svga_present(void) { return svga.present && svga.enabled; }
const char *svga_name(void) { return svga.name; }

/* ------------------------------------------------------------------- start */

/* Generic dispatch carries the actual adapter context even while this driver
 * supports only one SVGA device. Do not silently accept another device's ctx. */
static bool svga_accel_can_fill(void *context) {
    return context == &svga && svga_can_fill();
}
static bool svga_accel_can_copy(void *context) {
    return context == &svga && svga_can_copy();
}
static bool svga_accel_can_cursor(void *context) {
    return context == &svga && svga_cursor_available();
}
static bool svga_accel_fill(void *context, int x, int y, int w, int h, u32 colour) {
    return context == &svga && svga_fill(x, y, w, h, colour);
}
static bool svga_accel_copy(void *context, int sx, int sy, int dx, int dy, int w, int h) {
    return context == &svga && svga_copy(sx, sy, dx, dy, w, h);
}
static void svga_accel_cursor_move(void *context, int x, int y) {
    if (context == &svga) svga_cursor_move(x, y);
}

static int svga_accel_set_shaders(void *context, u32 format,
                                const gpu_shader_program_t *vertex, const gpu_shader_program_t *pixel) {
    if (context != &svga || format != GPU_PROGRAM_SVGA_DX || !svga3d_can_draw()) return -1;
    return svga3d_set_shaders(vertex->code, vertex->words, pixel->code, pixel->words,
                             vertex->inputs, vertex->input_count, vertex->outputs, vertex->output_count,
                             pixel->inputs, pixel->input_count, pixel->outputs, pixel->output_count);
}

static int svga_accel_set_layout(void *context, u32 format, const u32 *elements, u32 count, u32 stride) {
    if (context != &svga || format != GPU_PROGRAM_SVGA_DX || !svga3d_can_draw()) return -1;
    return svga3d_set_layout(elements, count, stride);
}

static u32 svga_accel_draw_mode(void *context) {
    return context == &svga && svga3d_can_draw() ? 1u : 0u;
}
static int svga_accel_draw_triangles(void *context, const float *vertices, u32 triangles) {
    return svga_accel_draw_mode(context) ? svga3d_draw_user(vertices, triangles) : -1;
}
static int svga_accel_draw_image(void *context, const u32 *pixels,
                                 u32 width, u32 height, u32 stride,
                                 int x, int y, u32 draw_width, u32 draw_height) {
    if (!svga_accel_draw_mode(context) || width > 1024 || height > 1024) return -1;
    return svga3d_draw_image_strided(pixels, width, height, stride, x, y,
                                     draw_width, draw_height);
}

bool svga_init(void) {
    memset(&svga, 0, sizeof svga);

    pci_dev_t *d = pci_find_id(SVGA_VENDOR, SVGA_DEVICE, NULL);
    if (!d) return false;

    if (!d->bar_is_io[0] || !d->bar[0]) {
        kwarn("svga", "the display adapter has no I/O ports; leaving the "
                      "firmware's framebuffer alone");
        return false;
    }

    /* Driving this adapter for real - its registers, its FIFO, its cursor. */
    pci_claim(d, "svga");

    svga.pci = d;
    svga.io_base = (u16)d->bar[0];
    pci_enable_memory(d);

    /* Which version of the interface it speaks, and where its two ports are.
     * It stores the newest identifier it understands and rejects anything
     * later, which makes the handshake a test of both at once. */
    static const struct { u32 index, value; } layout[] = { { 0, 1 }, { 0, 4 } };
    static const u32 wanted[] = { SVGA_ID_2, SVGA_ID_1, SVGA_ID_0 };

    for (size_t l = 0; l < sizeof layout / sizeof layout[0] && !svga.version; l++) {
        port_index_offset = layout[l].index;
        port_value_offset = layout[l].value;

        for (size_t i = 0; i < sizeof wanted / sizeof wanted[0]; i++) {
            reg_write(REG_ID, wanted[i]);
            if (reg_read(REG_ID) == wanted[i]) { svga.version = wanted[i]; break; }
        }
    }

    if (!svga.version) {
        kwarn("svga", "the display adapter at ports %04x did not answer the "
                      "version handshake; the firmware's framebuffer stays in use",
              svga.io_base);
        return false;
    }

    svga.caps = reg_read(REG_CAPABILITIES);

    svga.vram_size = reg_read(REG_VRAM_SIZE);
    svga.max_width = reg_read(REG_MAX_WIDTH);
    svga.max_height = reg_read(REG_MAX_HEIGHT);

    u32 fifo_phys = reg_read(REG_MEM_START);
    svga.fifo_size = reg_read(REG_MEM_SIZE);

    snprintf(svga.name, sizeof svga.name, "VMware SVGA II (interface %u)",
             svga.version & 0xFF);

    svga.present = true;

    /* The command ring lives in memory the adapter provides; it has to be
     * mapped before anything can be put in it. */
    if (fifo_phys && svga.fifo_size >= 16 * sizeof(u32)) {
        void *mapped = vmm_map_mmio(fifo_phys, svga.fifo_size);
        if (mapped) {
            svga.fifo = mapped;

            /* Where the commands go, and where the adapter should read from.
             * The first words of the ring are not commands: they are a block
             * of registers the adapter and the driver share, and the driver
             * has to reserve room for all of them before the commands start.
             *
             * How many there are is not a number to guess.  The adapter
             * publishes its own minimum, and a driver that reserves less than
             * that gets the basic ring rather than the extended one - which is
             * silent, and takes the three-dimensional engine with it, because
             * the registers that negotiate it are in the part that was not
             * reserved. */
            u32 header = (u32)(4 * sizeof(u32));

            if (svga.caps & CAP_EXTENDED_FIFO) {
                u32 wanted = svga.fifo[FIFO_MIN] * (u32)sizeof(u32);
                /* A whole page, which covers every register the adapter has
                 * ever had and leaves the commands page-aligned. */
                if (wanted < PAGE_SIZE) wanted = PAGE_SIZE;
                header = wanted;
            }

            svga.fifo[FIFO_MIN] = header;
            svga.fifo[FIFO_MAX] = svga.fifo_size;
            svga.fifo[FIFO_NEXT_CMD] = header;
            svga.fifo[FIFO_STOP] = header;

            reg_write(REG_CONFIG_DONE, 1);

            if (svga.caps & CAP_EXTENDED_FIFO)
                svga.fifo_caps = svga.fifo[FIFO_CAPABILITIES];

            /* And tell the adapter which version of the drawing interface this
             * driver speaks.  Without it the adapter has no reason to believe
             * the guest can use the engine at all. */
            if ((svga.caps & (CAP_3D | CAP_GBOBJECTS | CAP_DX)) &&
                header > FIFO_GUEST_3D_HWVERSION * sizeof(u32)) {
                svga.fifo[FIFO_GUEST_3D_HWVERSION] = SVGA3D_HWVERSION_WE_SPEAK;
                svga.hw3d = (svga.fifo_caps & FIFO_CAP_3D_HWVERSION_REVISED)
                          ? svga.fifo[FIFO_3D_HWVERSION_REVISED]
                          : svga.fifo[FIFO_3D_HWVERSION];
                kinfo("svga", "ring header %u bytes, features %08x, caps "
                              "%08x, original 3D version %08x",
                      header, svga.fifo_caps, svga.caps, svga.hw3d);
            }
        } else {
            kwarn("svga", "the command ring could not be mapped; the adapter "
                          "will have to find changes for itself");
        }
    }

    describe_mode();

    kinfo("svga", "%s, %u MiB of video memory, up to %ux%u, command ring %u KiB",
          svga.name, svga.vram_size / (1024 * 1024),
          svga.max_width, svga.max_height, svga.fifo_size / 1024);

    /* How many independent displays the adapter exposes (REG_NUM_DISPLAYS).
     * VMware reports what its vmx asks for (svga.numDisplays), so with two
     * configured this reads 2 - which is what makes multi-display (#7) real in
     * the VM, not only on hardware.  Kept as a plain fact the boot log states. */
    svga.num_displays = reg_read(REG_NUM_DISPLAYS);
    if (!svga.num_displays) svga.num_displays = 1;
    kinfo("svga", "the adapter exposes %u independent display(s)",
          svga.num_displays);

    /* A list of what it can do, because the difference between telling the
     * adapter what changed and letting it work that out is the difference this
     * driver exists for. */
    kinfo("svga", "%s%s%s%s",
          fifo_ready() ? "changed regions are reported rather than searched for"
                       : "no command ring, so changes must be searched for",
          (svga.caps & CAP_ALPHA_CURSOR) ? ", the adapter draws the pointer" : "",
          (svga.caps & CAP_RECT_FILL) ? ", it fills rectangles" : "",
          (svga.caps & CAP_RECT_COPY) ? ", it copies them" : "");

    /* Offer what it can do through the system's own door, rather than being
     * reached by name from the system call.  On a machine with this adapter
     * the two are the same thing; on a machine without one, being reached by
     * name is why nothing else was ever asked. */
    {
        static const gpu_accel_ops_t ops = {
            .can_fill    = svga_accel_can_fill,
            .can_copy    = svga_accel_can_copy,
            .can_cursor  = svga_accel_can_cursor,
            .fill        = svga_accel_fill,
            .copy        = svga_accel_copy,
            .cursor_move = svga_accel_cursor_move,
            .draw_mode = svga_accel_draw_mode,
            .draw_triangles = svga_accel_draw_triangles,
            .draw_image = svga_accel_draw_image,
            .set_shaders = svga_accel_set_shaders,
            .set_layout = svga_accel_set_layout,
        };
        if (!gpu_accel_register(d->bus, d->slot, d->func,
                                &ops, &svga, "VMware SVGA"))
            kwarn("svga", "screen accelerator registration failed");
    }

    /* Say what this driver is, before asking the adapter for anything that
     * costs it real resources.  A drawing context is allocated on the host's
     * own graphics card, and the adapter will not do that for a guest that has
     * not identified itself - it answers every capability question honestly
     * and then refuses to make one. */
    if (svga.caps & (CAP_GBOBJECTS | CAP_DX | CAP_3D)) {
        /* Three plain numbers - major, minor, patch - not one packed word.
         * The adapter reads them as a version and decides from it what this
         * driver is allowed to ask for, so a nonsense value is not ignored:
         * it identifies a driver too old to be given the newer interfaces. */
        reg_write(REG_GUEST_DRIVER_ID, GUEST_DRIVER_ID_LINUX);
        reg_write(REG_GUEST_DRIVER_VERSION1, 2);
        reg_write(REG_GUEST_DRIVER_VERSION2, 20);
        reg_write(REG_GUEST_DRIVER_VERSION3, 0);
        reg_write(REG_GUEST_DRIVER_ID, GUEST_DRIVER_ID_SUBMIT);
    }

    /* What the drawing engine will actually do.  The capability register says
     * which interfaces exist; these say whether there is a real graphics card
     * on the other side of them, and on a host without one the interfaces are
     * still advertised and every answer here is zero. */
    if (svga.caps & (CAP_GBOBJECTS | CAP_DX | CAP_3D)) {
        static const struct { u32 index; const char *name; } ask[] = {
            { 0,  "3D" },
            { 1,  "maximum light sources" },
            { 2,  "maximum render targets" },
            { 3,  "shader model 1.1" },
            { 4,  "vertex shader version" },
            { 5,  "fragment shader support" },
            { 8,  "maximum texture width" },
            { 9,  "maximum texture height" },
        };

        u32 answer[8];
        for (size_t i = 0; i < sizeof ask / sizeof ask[0]; i++) {
            reg_write(REG_DEV_CAP, ask[i].index);
            answer[i] = reg_read(REG_DEV_CAP);
        }

        if (answer[0]) {
            /* Note: this says a drawing engine exists, not that it speaks the
             * original command set.  Those are separate facts and they are
             * kept in separate places, because conflating them is how a driver
             * ends up sending commands the adapter has retired. */
            kinfo("svga", "the adapter draws in three dimensions on the host's "
                          "own graphics card: %u render target(s)", answer[2]);
        } else {
            kinfo("svga", "the adapter offers the drawing interfaces "
                          "(%08x) and the host answers no to all of them",
                  svga.caps);
        }
        kinfo("svga", "drawing engine: present=%u targets=%u shaders=%08x/%u, "
                      "original command set version %08x",
              answer[0], answer[2], answer[4], answer[5], svga.hw3d);
    } else {
        kinfo("svga", "it does not offer three-dimensional drawing "
                      "(capabilities %08x)", svga.caps);
    }

    return true;
}

/* ------------------------------------------------------------------- test */

int svga_selftest(void) {
    if (!svga.present) {
        kinfo("svga", "no VMware display adapter in this machine");
        return 0;
    }

    int failures = 0;

    if (!fifo_ready()) {
        kerr("svga", "the command ring is not usable");
        failures++;
    } else {
        /* A fence goes in, and the adapter is asked to catch up. When it has,
         * the fence it reports back must be the one that was sent - which is
         * the only proof from this side that the ring is really being read. */
        if (svga.fifo_caps & FIFO_CAP_FENCE) {
            u32 marker = 0x4B657374;              /* "Kest" */
            u32 args[1] = { marker };
            fifo_command(CMD_FENCE, args, 1);
            fifo_sync();

            if (svga.fifo[FIFO_FENCE] != marker) {
                kerr("svga", "a fence was sent and %08x came back, not %08x",
                     svga.fifo[FIFO_FENCE], marker);
                failures++;
            } else {
                kinfo("svga", "the adapter read a command out of the ring and "
                              "acknowledged it");
            }
        } else {
            /* No fence support: the weaker check is that the adapter consumed
             * what it was given, which moves STOP up to NEXT_CMD. */
            svga_update(0, 0, 1, 1);
            fifo_sync();
            if (svga.fifo[FIFO_STOP] != svga.fifo[FIFO_NEXT_CMD]) {
                kerr("svga", "the adapter did not consume a command");
                failures++;
            } else {
                kinfo("svga", "the adapter consumed a command from the ring");
            }
        }
    }

    /* The largest mode this adapter says it can do, actually set - and then
     * put back.  A maximum a device reports and a maximum it will accept are
     * not the same number, and the difference only shows up when something
     * asks for it. Doing it here means the answer is known at start-up rather
     * than the first time somebody picks the top of a list. */
    {
        u32 was_w = svga.width, was_h = svga.height;
        u32 want_w = svga.max_width, want_h = svga.max_height;

        /* Whatever the largest mode is that the memory will actually hold. */
        while (want_w > 640 && (u64)want_w * want_h * 4ull > svga.vram_size) {
            want_w = want_w * 3 / 4;
            want_h = want_h * 3 / 4;
        }

        if (want_w != was_w || want_h != was_h) {
            if (svga_set_mode(want_w, want_h)) {
                kinfo("svga", "the largest mode this adapter will take is "
                              "%ux%u (%lu MiB of its %u)",
                      svga.width, svga.height,
                      (unsigned long)((u64)svga.width * svga.height * 4 /
                                      (1024 * 1024)),
                      svga.vram_size / (1024 * 1024));
            } else {
                kwarn("svga", "the adapter would not take %ux%u even though it "
                              "reports that as its maximum", want_w, want_h);
                failures++;
            }
            /* Back to what was on the screen before. */
            svga_set_mode(was_w, was_h);
        }
    }


    /* ------------------------------------------------- the adapter drawing
     *
     * Everything above establishes that the adapter reads the ring.  This
     * establishes that it does the work: a rectangle is copied from one part
     * of the screen to another by the device, and the pixels are read back to
     * see whether they arrived.
     *
     * This one is not a model.  There is no NVIDIA card in this machine and no
     * AMD card, so those drivers are exercised against models of what the
     * documentation says - but this adapter is really here, its command ring is
     * really being read, and the pixels below are really moved by something
     * that is not this code.
     */
    if (svga_can_copy() && g_boot.fb.base && svga.width >= 64 &&
        svga.height >= 32) {
        volatile u32 *fb = (volatile u32 *)(uintptr_t)vmm_map_mmio(
            svga.fb_phys, (size_t)svga.pitch * svga.height);

        if (!fb) {
            kwarn("svga", "the framebuffer could not be mapped to check the "
                          "copy engine");
        } else {
            u32 stride = svga.pitch / 4;

            /* Somewhere out of the way: the bottom-left corner, which the
             * console has not written to yet at this point in start-up. */
            int sx = 0, sy = (int)svga.height - 32;
            int dx = 32, dy = (int)svga.height - 32;
            int w = 32, h = 16;

            /* A pattern that could not be there by accident, and a
             * destination deliberately filled with something else. */
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    fb[(size_t)(sy + y) * stride + sx + x] =
                        0x00A50000u | (u32)(y << 8) | (u32)x;
                    fb[(size_t)(dy + y) * stride + dx + x] = 0x00123456u;
                }

            /* The processor's stores have to have landed before the device
             * reads them.  The framebuffer is write-combining, so they are
             * sitting in a buffer until something makes them go. */
            __asm__ volatile("sfence" ::: "memory");

            u32 fills_before = 0, copies_before = 0;
            svga_accel_counts(&fills_before, &copies_before, NULL);

            u64 started = rdtsc();
            bool asked = svga_copy(sx, sy, dx, dy, w, h);
            fifo_sync();
            u64 took = rdtsc() - started;

            if (!asked) {
                kerr("svga", "the adapter advertises the copy and refused it");
                failures++;
            } else {
                int wrong = 0;
                for (int y = 0; y < h && wrong < 4; y++)
                    for (int x = 0; x < w; x++) {
                        u32 want = 0x00A50000u | (u32)(y << 8) | (u32)x;
                        u32 got = fb[(size_t)(dy + y) * stride + dx + x] & 0xFFFFFF;
                        if (got != want) { wrong++; break; }
                    }

                u32 copies_after = 0;
                svga_accel_counts(NULL, &copies_after, NULL);

                if (wrong) {
                    kerr("svga", "the adapter was asked to copy %dx%d and %d "
                                 "row(s) did not arrive", w, h, wrong);
                    failures++;
                } else if (copies_after != copies_before + 1) {
                    kerr("svga", "the copy was not counted");
                    failures++;
                } else {
                    /* What the processor would have cost for the same work, so
                     * the comparison is a number rather than an assertion. */
                    u64 cpu_started = rdtsc();
                    for (int y = 0; y < h; y++)
                        for (int x = 0; x < w; x++)
                            fb[(size_t)(dy + y) * stride + dx + x] =
                                fb[(size_t)(sy + y) * stride + sx + x];
                    __asm__ volatile("sfence" ::: "memory");
                    u64 cpu_took = rdtsc() - cpu_started;

                    kinfo("svga", "the adapter copied %dx%d pixels itself and "
                                  "they arrived", w, h);

                    /* And the same again at a size worth accelerating.  The
                     * cost of going through the ring is almost all fixed - a
                     * command and a wait - so whether handing work over is
                     * worth it is entirely a question of how much work.  A
                     * driver that accelerates everything is slower than one
                     * that accelerates nothing. */
                    int bw = (int)svga.width / 2 - 32;
                    int bh = 120;
                    if (bw > 480) bw = 480;
                    if (bh > (int)svga.height / 4) bh = (int)svga.height / 4;

                    if (bw >= 64 && bh >= 16) {
                        int bsy = (int)svga.height - bh;
                        int bdx = (int)svga.width / 2;

                        u64 a0 = rdtsc();
                        svga_copy(0, bsy, bdx, bsy, bw, bh);
                        fifo_sync();
                        u64 adapter = rdtsc() - a0;

                        u64 c0 = rdtsc();
                        for (int y = 0; y < bh; y++)
                            for (int x = 0; x < bw; x++)
                                fb[(size_t)(bsy + y) * stride + bdx + x] =
                                    fb[(size_t)(bsy + y) * stride + x];
                        __asm__ volatile("sfence" ::: "memory");
                        u64 processor = rdtsc() - c0;

                        kinfo("svga", "%d pixels: %llu cycles on the adapter, "
                                      "%llu on the processor - %llux",
                              bw * bh, (unsigned long long)adapter,
                              (unsigned long long)processor,
                              (unsigned long long)(adapter ? processor / adapter
                                                           : 0));
                        kinfo("svga", "and %d pixels: %llu on the adapter "
                                      "against %llu on the processor, so small "
                                      "work is not worth handing over",
                              w * h, (unsigned long long)took,
                              (unsigned long long)cpu_took);

                        for (int y = 0; y < bh; y++)
                            for (int x = 0; x < bw; x++) {
                                fb[(size_t)(bsy + y) * stride + x] = 0;
                                fb[(size_t)(bsy + y) * stride + bdx + x] = 0;
                            }
                        __asm__ volatile("sfence" ::: "memory");
                        svga_update(0, bsy, (int)svga.width, bh);
                    }
                }
            }

            /* Put the corner back to black, so nothing is left on the screen. */
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w * 2; x++)
                    fb[(size_t)(dy + y) * stride + sx + x] = 0;
            __asm__ volatile("sfence" ::: "memory");
            svga_update(sx, dy, w * 2, h);
        }
    } else if (!svga_can_copy()) {
        kinfo("svga", "this adapter does not offer to copy rectangles, so that "
                      "work stays with the processor");
    }

    if (svga.width == 0 || svga.height == 0 || svga.pitch < svga.width * 4) {
        kerr("svga", "the mode reads back as %ux%u with %u bytes a line",
             svga.width, svga.height, svga.pitch);
        failures++;
    }

    return failures;
}

/* --------------------------------------------------- taking over the screen
 *
 * The firmware handed over a framebuffer before any of this ran, and the rest
 * of the system has been using it. Now that the adapter is being driven
 * properly, what it reports is the truth: where the pixels are, and how far
 * apart the lines. Usually those are the same numbers the firmware gave, since
 * it is the same memory - but they do not have to be, and quietly carrying on
 * with the old ones would put the picture in the wrong place.
 */
void svga_adopt_framebuffer(void) {
    if (!svga.enabled) return;

    /* Select only when this adapter actually replaces the desktop target. */
    if (!gpu_accel_select(svga.pci->bus, svga.pci->slot, svga.pci->func))
        kwarn("svga", "framebuffer adopted without a registered accelerator");

    u64 old_base = g_boot.fb.base;
    u32 old_pitch = g_boot.fb.pitch;

    g_boot.fb.base = svga.fb_phys;
    g_boot.fb.width = svga.width;
    g_boot.fb.height = svga.height;
    g_boot.fb.pitch = svga.pitch;
    g_boot.fb.bpp = 32;
    g_boot.fb.size = svga.pitch * svga.height;

    /* The adapter's channel layout for a thirty-two bit mode. */
    g_boot.fb.red_shift = 16;   g_boot.fb.red_bits = 8;
    g_boot.fb.green_shift = 8;  g_boot.fb.green_bits = 8;
    g_boot.fb.blue_shift = 0;   g_boot.fb.blue_bits = 8;

    if (old_base != svga.fb_phys || old_pitch != svga.pitch) {
        kinfo("svga", "the framebuffer moved from %016lx (%u bytes a line) to "
                      "%016lx (%u)", old_base, old_pitch, svga.fb_phys,
              svga.pitch);
        console_remap_wc();
    }

    kinfo("svga", "driving the display at %ux%u; changed regions are now "
                  "reported to the adapter instead of being searched for",
          svga.width, svga.height);
}
