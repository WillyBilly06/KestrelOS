/* mouse.c - PS/2 mouse on the i8042's second port.
 *
 * The device is put into the IntelliMouse mode that adds a scroll wheel when
 * it will accept it, and otherwise left in the standard three-byte protocol.
 * Movement is accumulated into an absolute position clamped to the screen, so
 * the window system gets a cursor rather than a stream of deltas it would have
 * to integrate itself.
 */
#include "kernel.h"
#include "input.h"
#include "cpu.h"
#include "apic.h"
#include "acpi.h"
#include "klog.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define ST_OUTPUT_FULL 0x01
#define ST_INPUT_FULL  0x02
#define ST_FROM_MOUSE  0x20

static bool present;
static int  packet_bytes = 3;     /* 4 once the scroll wheel is enabled */
static u8   packet[4];
static int  packet_index;

static int  cur_x, cur_y;
static int  screen_w = 640, screen_h = 480;
static u8   buttons;
static u8 merge_buttons(int source, u8 now);

void tty_wake_readers(void);
bool vmmouse_init(void);
void input_push_mouse(int x, int y, int dx, int dy, int wheel, u8 buttons, u8 changed);

/* ------------------------------------------------------------------------- */
/* controller helpers                                                        */
/* ------------------------------------------------------------------------- */

static bool wait_write(void) {
    for (int i = 0; i < 100000; i++) if (!(inb(PS2_STATUS) & ST_INPUT_FULL)) return true;
    return false;
}

static bool wait_read(void) {
    for (int i = 0; i < 100000; i++) if (inb(PS2_STATUS) & ST_OUTPUT_FULL) return true;
    return false;
}

/* Commands for the second port have to be prefixed with 0xD4. */
static int mouse_command(u8 cmd) {
    if (!wait_write()) return -1;
    outb(PS2_CMD, 0xD4);
    if (!wait_write()) return -1;
    outb(PS2_DATA, cmd);
    if (!wait_read()) return -1;
    return inb(PS2_DATA);
}

static int mouse_read(void) {
    if (!wait_read()) return -1;
    return inb(PS2_DATA);
}

/* ------------------------------------------------------------------------- */
/* interrupt                                                                 */
/* ------------------------------------------------------------------------- */

bool vmmouse_available(void);
bool vmmouse_read(u32 *x, u32 *y, s32 *wheel, u8 *buttons);

/* In absolute mode the host still sends PS/2 packets to raise the interrupt,
 * but the real position comes from the backdoor.  The bytes are drained and
 * discarded so the controller does not stall. */
static bool absolute_poll(void) {
    bool any = false;

    for (int guard = 0; guard < 16; guard++) {
        u32 rx, ry;
        s32 wheel;
        u8 pressed;
        if (!vmmouse_read(&rx, &ry, &wheel, &pressed)) break;

        /* The host reports 0..65535 across the screen. */
        int nx = (int)(((u64)rx * (u32)screen_w) >> 16);
        int ny = (int)(((u64)ry * (u32)screen_h) >> 16);
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx > screen_w - 1) nx = screen_w - 1;
        if (ny > screen_h - 1) ny = screen_h - 1;

        int dx = nx - cur_x, dy = ny - cur_y;
        u8 changed = (u8)(pressed ^ buttons);
        buttons = pressed;
        cur_x = nx;
        cur_y = ny;

        if (dx || dy || wheel || changed) {
            input_push_mouse(cur_x, cur_y, dx, dy, wheel, buttons, changed);
            any = true;
        }
    }
    return any;
}

static regs_t *mouse_isr(regs_t *r, void *ctx) {
    (void)ctx;

    int guard = 0;
    bool moved = false;

    if (vmmouse_available()) {
        /* Discard the PS/2 bytes that raised this interrupt. */
        while ((inb(PS2_STATUS) & (ST_OUTPUT_FULL | ST_FROM_MOUSE)) == (ST_OUTPUT_FULL | ST_FROM_MOUSE)
               && ++guard < 64) {
            (void)inb(PS2_DATA);
        }
        if (absolute_poll()) tty_wake_readers();
        lapic_eoi();
        return r;
    }

    while ((inb(PS2_STATUS) & (ST_OUTPUT_FULL | ST_FROM_MOUSE)) == (ST_OUTPUT_FULL | ST_FROM_MOUSE)
           && ++guard < 64) {
        u8 byte = inb(PS2_DATA);

        /* Bit 3 of the first byte is always set; use it to resynchronise if a
         * byte was ever lost. */
        if (packet_index == 0 && !(byte & 0x08)) continue;

        packet[packet_index++] = byte;
        if (packet_index < packet_bytes) continue;
        packet_index = 0;

        u8 flags = packet[0];
        if (flags & 0xC0) continue;            /* overflow: discard the packet */

        /* Movement is a 9-bit signed value: the sign lives in the flags byte. */
        int dx = packet[1];
        int dy = packet[2];
        if (flags & 0x10) dx |= ~0xFF;
        if (flags & 0x20) dy |= ~0xFF;

        int wheel = 0;
        if (packet_bytes == 4) {
            s8 z = (s8)(packet[3] & 0x0F);
            if (z & 0x08) z |= (s8)0xF0;
            wheel = -z;                        /* away from the user is up */
        }

        u8 now = (u8)(flags & 0x07);
        u8 all = merge_buttons(0, now);       /* the PS/2 port is source 0 */
        u8 changed = (u8)(all ^ buttons);
        buttons = all;

        cur_x += dx;
        cur_y -= dy;                           /* the mouse's Y grows upwards */
        if (cur_x < 0) cur_x = 0;
        if (cur_y < 0) cur_y = 0;
        if (cur_x > screen_w - 1) cur_x = screen_w - 1;
        if (cur_y > screen_h - 1) cur_y = screen_h - 1;

        input_push_mouse(cur_x, cur_y, dx, -dy, wheel, buttons, changed);
        moved = true;
    }

    if (moved) tty_wake_readers();
    lapic_eoi();
    return r;
}

/* ------------------------------------------------------------------------- */
/* bring-up                                                                  */
/* ------------------------------------------------------------------------- */

/* Movement from a USB mouse.  It shares the cursor with the PS/2 driver so
 * both devices drive the same pointer, which is what a machine with a laptop
 * touchpad and a plugged-in mouse needs. */
/* One button state per DEVICE, merged by OR.
 *
 * There used to be a single `buttons` that every pointer overwrote, and on the
 * machine this system exists for that is not a corner case: a gaming mouse, a
 * wireless keyboard with a pointer interface, and a microphone whose control
 * surface is a HID pointer polled every eight frames all inject here.  Hold
 * the mouse's button to drag a window and the microphone's next idle report -
 * buttons zero, because nothing on the microphone is pressed - overwrote the
 * held state, and the desktop saw a release.  Clicks worked, because a click
 * is over before the next idle report; a drag could never survive one.  "Why
 * are the windows not movable by mouse" was this line of code.
 *
 * Now each source keeps its own state and what the desktop sees is the OR of
 * all of them: a button is down while ANY device holds it down. */
#define MOUSE_SOURCES 64   /* room for every HID interface without sharing */
static u8 source_buttons[MOUSE_SOURCES];

static u8 merge_buttons(int source, u8 now) {
    source_buttons[source & (MOUSE_SOURCES - 1)] = now;
    u8 all = 0;
    for (int i = 0; i < MOUSE_SOURCES; i++) all |= source_buttons[i];
    return all;
}

void mouse_inject_relative_from(int source, int dx, int dy, int wheel, u8 now) {

    bool irq = irq_save();

    /* A pointer exists because something is moving it.
     *
     * This flag used to be set only where the PS/2 mouse is initialised, on
     * the assumption that a pointer is a PS/2 mouse.  A machine built in the
     * last decade has no PS/2 port at all, so on the first real one this ran
     * on the flag stayed false while a USB mouse sat here injecting movement
     * into it - and the desktop, which asks this question before deciding
     * whether it has a pointer, said it had none and drew no cursor.
     *
     * Asking "did the PS/2 driver find something" and asking "does this
     * machine have a pointer" are different questions, and only the second one
     * is ever what a caller wants. */
    present = true;

    u8 all = merge_buttons(source, now);
    u8 changed = (u8)(all ^ buttons);
    buttons = all;

    cur_x += dx;
    cur_y += dy;                     /* HID reports Y growing downwards */
    if (cur_x < 0) cur_x = 0;
    if (cur_y < 0) cur_y = 0;
    if (cur_x > screen_w - 1) cur_x = screen_w - 1;
    if (cur_y > screen_h - 1) cur_y = screen_h - 1;

    int x = cur_x, y = cur_y;
    irq_restore(irq);

    if (dx || dy || wheel || changed)
        input_push_mouse(x, y, dx, dy, wheel, buttons, changed);
    tty_wake_readers();
}

/* The old entry point, for callers that are a single device.  They all share
 * source 1, which keeps them merged with everything else rather than
 * overwriting it. */
void mouse_inject_relative(int dx, int dy, int wheel, u8 now) {
    mouse_inject_relative_from(1, dx, dy, wheel, now);
}

/* A tablet, touchscreen or virtual pointer reports where it is rather than how
 * far it moved.  Scaling straight to the screen means the cursor lands exactly
 * under the user's finger, with none of the drift that integrating deltas
 * accumulates. */
void mouse_inject_absolute(int x, int y, int max_x, int max_y, int wheel, u8 now) {
    present = true;      /* see mouse_inject_relative */
    if (max_x <= 0 || max_y <= 0) return;
    if (x < 0) x = 0; if (x > max_x) x = max_x;
    if (y < 0) y = 0; if (y > max_y) y = max_y;

    int nx = (int)(((s64)x * (screen_w - 1)) / max_x);
    int ny = (int)(((s64)y * (screen_h - 1)) / max_y);

    bool irq = irq_save();
    u8 all = merge_buttons(2, now);
    u8 changed = (u8)(all ^ buttons);
    buttons = all;
    int dx = nx - cur_x, dy = ny - cur_y;
    cur_x = nx;
    cur_y = ny;
    irq_restore(irq);

    if (dx || dy || wheel || changed)
        input_push_mouse(nx, ny, dx, dy, wheel, buttons, changed);
    tty_wake_readers();
}

void mouse_set_bounds(int w, int h) {
    screen_w = w > 1 ? w : 1;
    screen_h = h > 1 ? h : 1;
    cur_x = screen_w / 2;
    cur_y = screen_h / 2;
}

void mouse_position(int *x, int *y) {
    if (x) *x = cur_x;
    if (y) *y = cur_y;
}

bool mouse_present(void) { return present; }

/* Called when a pointing device is claimed, by whatever bus it is on.  See the
 * note in mouse_inject_relative for why this is not the PS/2 driver's flag to
 * own. */
void mouse_set_present(void) { present = true; }

void mouse_init(void) {
    mouse_set_bounds((int)g_boot.fb.width, (int)g_boot.fb.height);

    if (!acpi_has_8042()) {
        kinfo("mouse", "firmware reports no 8042; legacy mouse probe skipped");
        return;
    }

    /* Programming the controller makes it put replies in the shared output
     * buffer, which raises the keyboard interrupt.  With interrupts enabled
     * the keyboard handler would consume those replies before this code sees
     * them, so the whole sequence runs with them masked. */
    bool irq = irq_save();

    /* Enable the second port and let its interrupt through. */
    if (!wait_write()) { irq_restore(irq); kwarn("mouse", "the keyboard controller is not responding"); return; }
    outb(PS2_CMD, 0xA8);

    if (!wait_write()) { irq_restore(irq); return; }
    outb(PS2_CMD, 0x20);
    if (!wait_read()) { irq_restore(irq); kwarn("mouse", "cannot read the controller configuration"); return; }
    u8 cfg = inb(PS2_DATA);
    cfg |= 0x02;              /* second port interrupt on  */
    cfg &= (u8)~0x20;         /* second port clock enabled */
    if (!wait_write()) { irq_restore(irq); return; }
    outb(PS2_CMD, 0x60);
    if (!wait_write()) { irq_restore(irq); return; }
    outb(PS2_DATA, cfg);

    /* Reset, then check the device announces itself. */
    int r = mouse_command(0xFF);
    if (r != 0xFA) { irq_restore(irq); kinfo("mouse", "no PS/2 mouse attached"); return; }
    int self_test = mouse_read();
    int id = mouse_read();
    if (self_test != 0xAA) {
        irq_restore(irq);
        kwarn("mouse", "reset returned %#x rather than 0xAA; no mouse", self_test);
        return;
    }
    (void)id;

    mouse_command(0xF6);      /* restore defaults */

    /* The magic sample-rate sequence 200, 100, 80 switches an IntelliMouse
     * into its four-byte mode; a plain mouse ignores it and keeps ID 0. */
    mouse_command(0xF3); mouse_command(200);
    mouse_command(0xF3); mouse_command(100);
    mouse_command(0xF3); mouse_command(80);
    if (mouse_command(0xF2) == 0xFA) {
        int new_id = mouse_read();
        if (new_id == 3) {
            packet_bytes = 4;
            kinfo("mouse", "scroll wheel enabled");
        }
    }

    mouse_command(0xF3); mouse_command(100);     /* 100 reports per second */
    mouse_command(0xE8); mouse_command(3);       /* 8 counts per millimetre */

    if (mouse_command(0xF4) != 0xFA) {           /* start reporting */
        irq_restore(irq);
        kwarn("mouse", "the mouse would not start reporting");
        return;
    }

    packet_index = 0;

    /* Prefer absolute reporting where the platform offers it: the cursor then
     * lands exactly where the host's pointer is, with no drift and none of the
     * guest's own acceleration. */
    vmmouse_init();

    irq_restore(irq);

    bool active_low = false, level = false;
    u32 gsi = acpi_irq_to_gsi(12, &active_low, &level);
    irq_install(VEC_IRQ_BASE + 12, mouse_isr, NULL);
    if (!ioapic_route(gsi, VEC_IRQ_BASE + 12, active_low, level, lapic_id())) {
        kerr("mouse", "cannot route mouse IRQ 12 (GSI %u)", gsi);
        return;
    }
    ioapic_mask(gsi, false);

    present = true;
    if (vmmouse_available())
        kinfo("mouse", "ready on GSI %u -> vector %#x, absolute positioning",
              gsi, VEC_IRQ_BASE + 12);
    else
        kinfo("mouse", "ready on GSI %u -> vector %#x, %d-byte packets",
              gsi, VEC_IRQ_BASE + 12, packet_bytes);
}
