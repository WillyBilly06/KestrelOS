/* vmmouse.c - VMware's absolute pointing device.
 *
 * A PS/2 mouse only reports movement, so a virtual machine has to synthesise
 * deltas from wherever the host's cursor went.  The result drifts, picks up the
 * guest's acceleration, and never quite lines up with the host pointer.
 *
 * VMware offers a way out: a backdoor I/O port that reports the pointer's
 * absolute position directly.  When it is available the mouse driver uses it
 * and the cursor tracks the host exactly; on real hardware, or under any other
 * hypervisor, none of this is present and the PS/2 path is used unchanged.
 */
#include "kernel.h"
#include "klog.h"

#define BACKDOOR_MAGIC 0x564D5868u      /* "VMXh" */
#define BACKDOOR_PORT  0x5658

#define CMD_GETVERSION          10
#define CMD_ABSPOINTER_DATA     39
#define CMD_ABSPOINTER_STATUS   40
#define CMD_ABSPOINTER_COMMAND  41

#define ABS_ENABLE           0x45414552u
#define ABS_DISABLE          0x000000F5u
#define ABS_REQUEST_ABSOLUTE 0x53424152u
#define ABS_REQUEST_RELATIVE 0x4C455252u

#define STATUS_ERROR 0xFFFF0000u

static bool available;

/* The backdoor is entered with a magic value in EAX and the command in ECX;
 * the port read returns four registers' worth of reply. */
static void backdoor(u32 *eax, u32 *ebx, u32 *ecx, u32 *edx) {
    __asm__ volatile("inl %%dx, %%eax"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "0"(BACKDOOR_MAGIC), "1"(*ebx), "2"(*ecx), "3"(BACKDOOR_PORT)
                     : "memory");
}

static void send_command(u32 command) {
    u32 a = BACKDOOR_MAGIC, b = command, c = CMD_ABSPOINTER_COMMAND, d = BACKDOOR_PORT;
    backdoor(&a, &b, &c, &d);
}

static u32 read_status(void) {
    u32 a = BACKDOOR_MAGIC, b = 0, c = CMD_ABSPOINTER_STATUS, d = BACKDOOR_PORT;
    backdoor(&a, &b, &c, &d);
    return a;
}

bool vmmouse_available(void) { return available; }

void vmmouse_disable(void) {
    if (!available) return;
    send_command(ABS_DISABLE);
    available = false;
}

/* Try to switch the pointer into absolute mode.  Returns false when there is
 * no VMware backdoor, which is the normal case on real hardware. */
bool vmmouse_init(void) {
    available = false;

    /* A machine with no backdoor either faults or returns the magic value
     * unchanged, so check that the port actually answered. */
    u32 a = BACKDOOR_MAGIC, b = ~ABS_ENABLE, c = CMD_GETVERSION, d = BACKDOOR_PORT;
    backdoor(&a, &b, &c, &d);
    if (b != BACKDOOR_MAGIC || a == 0xFFFFFFFFu) return false;

    send_command(ABS_ENABLE);

    u32 status = read_status();
    if ((status & 0x0000FFFFu) == 0) return false;

    /* Drain whatever the host had queued before we asked. */
    u32 words = status & 0xFFFF;
    if (words) {
        u32 ra = BACKDOOR_MAGIC, rb = words > 4 ? 4 : words;
        u32 rc = CMD_ABSPOINTER_DATA, rd = BACKDOOR_PORT;
        backdoor(&ra, &rb, &rc, &rd);
    }

    send_command(ABS_REQUEST_ABSOLUTE);

    available = true;
    kinfo("mouse", "VMware absolute pointer enabled (version %#x)", a);
    return true;
}

/* Read one absolute report.  Returns false when nothing is pending.
 * `x` and `y` come back in the host's 0..65535 range. */
bool vmmouse_read(u32 *x, u32 *y, s32 *wheel, u8 *buttons) {
    if (!available) return false;

    u32 status = read_status();
    if ((status & STATUS_ERROR) == STATUS_ERROR) {
        kwarn("mouse", "the VMware pointer reported an error; falling back to PS/2");
        available = false;
        return false;
    }

    u32 words = status & 0xFFFF;
    if (words < 4) return false;

    u32 a = BACKDOOR_MAGIC, b = 4, c = CMD_ABSPOINTER_DATA, d = BACKDOOR_PORT;
    backdoor(&a, &b, &c, &d);

    /* eax: flags in the high half, buttons in the low half.
     * ebx/ecx: absolute x and y.  edx: wheel movement. */
    u16 raw_buttons = (u16)(a & 0xFFFF);
    u8 mapped = 0;
    if (raw_buttons & 0x20) mapped |= 0x01;      /* left   */
    if (raw_buttons & 0x10) mapped |= 0x02;      /* right  */
    if (raw_buttons & 0x08) mapped |= 0x04;      /* middle */

    *x = b;
    *y = c;
    *wheel = -(s32)(s8)(u8)d;
    *buttons = mapped;
    return true;
}
