/* gpu.h - what the kernel knows about the graphics hardware.
 *
 * When firmware supplies a usable UEFI GOP framebuffer, the boot display can
 * remain on firmware scanout before a native driver takes over.  This is not
 * universal GPU support or hardware-accelerated rendering.  A driver knows which
 * card it is, how much memory it has, and whether anything more than the
 * framebuffer is reachable - and being honest when it is not.
 */
#ifndef KESTREL_GPU_H
#define KESTREL_GPU_H

#include "kernel.h"

typedef enum {
    GPU_UNKNOWN = 0,
    GPU_NVIDIA,
    GPU_AMD,
    GPU_INTEL,
    GPU_VMWARE,
    GPU_QEMU,
    GPU_MATROX,
    GPU_ASPEED,
} gpu_vendor_t;

/* How far this driver can drive the card. */
typedef enum {
    GPU_ACCEL_NONE = 0,     /* identified only                              */
    GPU_ACCEL_FRAMEBUFFER,  /* the firmware framebuffer, write-combined      */
    GPU_ACCEL_2D,           /* the card's own blitter is in use              */
} gpu_accel_t;

typedef struct {
    gpu_vendor_t vendor;
    u16   pci_vendor, pci_device;
    u16   subsys_vendor, subsys_device;
    u8    bus, slot, func;

    char  name[64];         /* "NVIDIA GeForce RTX 5070 Ti"                  */
    char  arch[40];         /* "Blackwell (GB20x)"                           */
    u32   chipset;          /* the card's own architecture id, 0 if unread   */
    u8    revision;

    u64   vram_bytes;       /* 0 when it could not be determined             */
    bool  vram_exact;       /* false when this is the aperture, not the size */
    u64   bar_mmio;         /* register window                               */
    u64   bar_vram;         /* memory aperture                               */
    u64   bar_vram_size;

    gpu_accel_t accel;
    char  note[112];        /* one honest line about what works              */
    bool  firmware_needed;  /* the card is driven through firmware it needs  */
    bool  firmware_present;
    char  firmware_name[64];
    bool  is_boot_display;  /* the one the firmware left a framebuffer on    */
} gpu_info_t;

/* The legacy shader/layout syscall uses SVGA DX bytecode and element formats.
 * This tag prevents a future backend from mistaking these for its native ISA.
 * All pointers below refer to synchronous, kernel-owned immutable snapshots. */
#define GPU_PROGRAM_SVGA_DX 1u
typedef struct {
    const u32 *code, *inputs, *outputs;
    u32 words, input_count, output_count;
} gpu_shader_program_t;

/* Device-specific screen operations. A capability needs both its readiness
 * callback and operation. Context and ops must remain alive after registration. */
typedef struct {
    bool (*can_fill)(void *context);
    bool (*can_copy)(void *context);
    bool (*can_cursor)(void *context);
    bool (*fill)(void *context, int x, int y, int w, int h, u32 colour);
    bool (*copy)(void *context, int from_x, int from_y, int to_x, int to_y, int w, int h);
    void (*cursor_move)(void *context, int x, int y);
    /* Existing GPUOP_CANDRAW ABI: 0 unavailable, 1 framebuffer-backed,
     * 2 native VRAM scanout (no CPU framebuffer fallback). */
    u32 (*draw_mode)(void *context);
    int (*draw_triangles)(void *context, const float *vertices, u32 triangles);
    int (*draw_image)(void *context, const u32 *pixels, u32 width, u32 height,
                      u32 stride, int x, int y, u32 draw_width, u32 draw_height);
    bool (*read_pixel)(void *context, int x, int y, u32 *pixel);
    int (*set_shaders)(void *context, u32 format,
                       const gpu_shader_program_t *vertex, const gpu_shader_program_t *pixel);
    int (*set_layout)(void *context, u32 format, const u32 *elements, u32 count, u32 stride);
} gpu_accel_ops_t;

#define GPU_ACCEL_CAN_FILL   0x1
#define GPU_ACCEL_CAN_COPY   0x2
#define GPU_ACCEL_CAN_CURSOR 0x4

/* Registration alone never takes ownership of the screen. Call select at a
 * framebuffer handoff, with drawing quiesced. Registration is boot-serialized;
 * records are immutable and never freed. This is one desktop target, not yet
 * a cross-adapter compositor. An unknown selection clears the previous target. */
bool gpu_accel_register(u8 bus, u8 slot, u8 func, const gpu_accel_ops_t *ops,
                        void *context, const char *owner);
bool gpu_accel_select(u8 bus, u8 slot, u8 func);
void gpu_accel_clear(void);
bool gpu_accel_is_selected(u8 bus, u8 slot, u8 func);
u32 gpu_accel_draw_mode(void);
int gpu_accel_draw_triangles(const float *vertices, u32 triangles);
int gpu_accel_draw_image(const u32 *pixels, u32 width, u32 height, u32 stride,
                         int x, int y, u32 draw_width, u32 draw_height);
bool gpu_accel_read_pixel(int x, int y, u32 *pixel);
int gpu_accel_set_shaders(u32 format, const gpu_shader_program_t *vertex,
                         const gpu_shader_program_t *pixel);
int gpu_accel_set_layout(u32 format, const u32 *elements, u32 count, u32 stride);
u32  gpu_accel_capabilities(void);
bool gpu_accel_fill(int x, int y, int w, int h, u32 colour);
bool gpu_accel_copy(int from_x, int from_y, int to_x, int to_y, int w, int h);
bool gpu_accel_cursor_move(int x, int y);
const char *gpu_accel_owner(void);

void gpu_init(void);
void display_report(void);
int  gpu_count(void);
bool gpu_get(int index, gpu_info_t *out);

/* Dedicated hardware-test boot sequencing.  The synchronous entry point is
 * called only after the rest of kernel bring-up has completed; it returns true
 * only when a committed NVIDIA scanout passed the final live GUI/2D/3D check.
 * The countdown worker lets the scheduler/userland run during the 20-second
 * observation window before rebooting. */
bool gpu_boot_test_sync(void);
void gpu_test_watchdog_arm(void);
void gpu_test_watchdog_progress(const char *stage);
void gpu_test_reboot_kthread(void *desktop_started);
void gpu_test_watchdog_kthread(void *unused);

/* Fill in everything an NVIDIA card can be asked without a firmware blob.
 * Separate from gpu.c because the decode is long and entirely NVIDIA's. */
void nvidia_identify(gpu_info_t *g, volatile u8 *regs, size_t regs_size);
const char *nvidia_arch_from_device_id(u16 device, u32 *family_out);

/* The NVIDIA driver.  See nv_core.c. */
void nvidia_driver_init(void);
void amd_driver_init(void);
bool nvidia_attach_model(void);
int  nvidia_drive_test(void);

/* The AMD driver.  See amd_core.c; the same method, a very different card. */
struct pci_dev;
void amd_identify_gpu(gpu_info_t *g, struct pci_dev *d);
int  amd_drive_test(void);

#endif
