/* uhci.c - the USB 1.1 host controller, the other device the coverage report
 * named on the test machine.
 *
 * 15ad:0774, class 0c.03.00: an Intel-style UHCI controller, which VMware,
 * VirtualBox and QEMU all present and which sits on the chipset of every PC
 * built before xHCI took over.  This system drives xHCI and drove nothing
 * else, so a machine whose only USB was 1.1 had no working keyboard.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS DRIVER DOES AND WHAT IT DOES NOT
 *
 * It brings the controller up and moves data on it: reset, the frame list, the
 * schedule running, port power and port reset, and control transfers built out
 * of transfer descriptors.  It reads the device descriptor off anything
 * plugged in, so what is attached is known by asking it rather than by
 * guessing from the port.
 *
 * It does NOT bind class drivers - a keyboard on this controller is detected
 * and identified but not yet delivering keys.  The reason is structural and
 * worth stating plainly rather than leaving to be discovered: `struct
 * usb_device`, which usbhid, usbmsc and usbaudio are all written against,
 * is defined inside xhci.c.  The device model is xHCI's, not the system's.
 * Sharing it means lifting it out into a controller-independent core, and that
 * is a refactor of a working stack that currently carries this machine's only
 * keyboard and mouse.  Doing it as a side effect of adding a second controller
 * is how working input gets broken.
 *
 * So this file stops at the transport, deliberately and visibly, and says so
 * in the log rather than reporting a USB controller that appears supported and
 * silently carries nothing.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS EASY TO GET WRONG HERE
 *
 * UHCI is a 32-bit design from 1996.  Every pointer it follows - the frame
 * list, each descriptor, each data buffer - must live below 4 GiB, and nothing
 * in the hardware will tell you when one does not.  Every allocation here is
 * checked, and the driver declines rather than handing the controller an
 * address it will truncate.
 *
 * The controller walks the frame list continuously whether or not there is
 * anything to do, so the list must be valid before the schedule is started and
 * every unused entry must carry the terminate bit.  A zero there is not "no
 * work"; it is a pointer to physical address zero.
 *
 * A transfer descriptor's status word is written by the hardware.  It is read
 * once per poll and judged from that snapshot, for the same reason the ATA
 * driver reads its status register once: the alternative is a condition that
 * tests two different values and reports on neither.
 */
#include "kernel.h"
#include "klog.h"
#include "pci.h"
#include "mm.h"
#include "time.h"

/* Registers, as offsets from the I/O base. */
#define USBCMD      0x00
#define USBSTS      0x02
#define USBINTR     0x04
#define FRNUM       0x06
#define FRBASEADD   0x08
#define SOFMOD      0x0C
#define PORTSC1     0x10

#define CMD_RS      (1u << 0)       /* run                                  */
#define CMD_HCRESET (1u << 1)       /* reset the controller                 */
#define CMD_GRESET  (1u << 2)       /* reset everything on the bus          */
#define CMD_MAXP    (1u << 7)       /* 64-byte packets allowed              */

#define STS_HALTED  (1u << 5)

#define PORT_CCS    (1u << 0)       /* something is plugged in              */
#define PORT_CSC    (1u << 1)       /* ...and that changed since last asked */
#define PORT_PE     (1u << 2)       /* port enabled                         */
#define PORT_PEDC   (1u << 3)
#define PORT_RD     (1u << 6)
#define PORT_LSDA   (1u << 8)       /* the device is low speed              */
#define PORT_PR     (1u << 9)       /* hold the port in reset               */

/* A frame list entry, and the link fields inside descriptors, carry flags in
 * their low bits: bit 0 says "nothing here", bit 1 says the pointer is a queue
 * head rather than a transfer descriptor. */
#define LINK_TERMINATE  (1u << 0)
#define LINK_QH         (1u << 1)

/* Transfer descriptor status bits. */
#define TD_ACTLEN_MASK  0x7FF
#define TD_STALLED      (1u << 22)
#define TD_ACTIVE       (1u << 23)
#define TD_IOC          (1u << 24)
#define TD_LOWSPEED     (1u << 26)
#define TD_ERRLIMIT(n)  ((u32)(n) << 27)
#define TD_ERROR_MASK   (0x7Eu << 16)   /* bitstuff, CRC, NAK, babble, buffer */

#define PID_SETUP  0x2D
#define PID_IN     0x69
#define PID_OUT    0xE1

/* A transfer descriptor is sixteen bytes to the hardware and must be aligned
 * to sixteen; the rest of the structure is this driver's own bookkeeping and
 * the controller never looks at it. */
typedef struct {
    volatile u32 link;
    volatile u32 status;
    volatile u32 token;
    volatile u32 buffer;
    u32 pad[4];                       /* to 32 bytes, so an array stays aligned */
} __attribute__((packed, aligned(16))) uhci_td_t;

typedef struct {
    volatile u32 head;
    volatile u32 element;
    u32 pad[2];
} __attribute__((packed, aligned(16))) uhci_qh_t;

#define MAX_PORTS 8
#define TD_COUNT  16

typedef struct {
    u16  io;                          /* the register window                */
    u32 *frames;                      /* 1024 entries                       */
    u64  frames_phys;

    uhci_qh_t *qh;                    /* one queue head for control work    */
    u64        qh_phys;
    uhci_td_t *td;                    /* the descriptors it runs            */
    u64        td_phys;
    u8        *buf;                   /* one page of bounce buffer          */
    u64        buf_phys;

    int  ports;
    bool attached[MAX_PORTS];
    bool low_speed[MAX_PORTS];
    u16  vendor[MAX_PORTS], product[MAX_PORTS];
} uhci_t;

#define MAX_CONTROLLERS 4
static uhci_t controllers[MAX_CONTROLLERS];
static int controller_count;
static int devices_found;

/* ----------------------------------------------------------------- helpers */

static inline u16 rd16(const uhci_t *u, u16 off) { return inw((u16)(u->io + off)); }
static inline void wr16(const uhci_t *u, u16 off, u16 v) { outw((u16)(u->io + off), v); }
static inline void wr32(const uhci_t *u, u16 off, u32 v) {
    __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"((u16)(u->io + off)));
}

/* Everything this controller follows has to fit in 32 bits.  Saying so at the
 * point of allocation is the difference between declining and corrupting
 * whatever happens to live at the truncated address. */
static void *dma32(size_t pages, u64 *phys, const char *what) {
    void *p = dma_alloc_pages(pages, phys);
    if (!p) return NULL;
    if (*phys + pages * PAGE_SIZE > 0x100000000ull) {
        kerr("uhci", "%s landed above 4 GiB (%p); this controller cannot "
                     "address it", what, (void *)*phys);
        return NULL;
    }
    return p;
}

/* ------------------------------------------------------------- the transfer */

/* Run a chain of descriptors already built into u->td and wait for it.
 *
 * The queue head is pointed at the first descriptor and the controller picks
 * it up on its next pass through the frame list.  Completion is the whole
 * chain going inactive; failure is any descriptor reporting an error, and both
 * are decided from one read of each status word.
 */
static bool run_chain(uhci_t *u, int count, int ms) {
    u->qh->element = (u32)u->td_phys;          /* a TD, so no LINK_QH flag */

    for (int spent = 0; spent < ms * 1000; spent += 50) {
        bool active = false;

        for (int i = 0; i < count; i++) {
            u32 st = u->td[i].status;          /* one read, then judged     */

            if (st & TD_ACTIVE) { active = true; break; }
            if (st & TD_STALLED) {
                u->qh->element = LINK_TERMINATE;
                return false;
            }
            if (st & TD_ERROR_MASK) {
                /* NAK on its own is not a failure - the device is asking for
                 * more time and the controller retries by itself.  Anything
                 * else has exhausted the error counter by the time it lands
                 * here, because the counter was set when the chain was built. */
                if (!(st & (1u << 19))) {
                    u->qh->element = LINK_TERMINATE;
                    return false;
                }
            }
        }

        if (!active) { u->qh->element = LINK_TERMINATE; return true; }
        timer_udelay(50);
    }

    u->qh->element = LINK_TERMINATE;
    return false;
}

static void build_td(uhci_t *u, int i, int next, u8 pid, u8 addr, u8 endpoint,
                     bool toggle, int len, u64 buffer, bool low_speed) {
    uhci_td_t *td = &u->td[i];

    td->link = next >= 0
        ? (u32)(u->td_phys + (u64)next * sizeof(uhci_td_t))
        : LINK_TERMINATE;

    td->status = TD_ACTIVE | TD_ERRLIMIT(3) | (low_speed ? TD_LOWSPEED : 0);

    /* The length field holds one less than the number of bytes, and all ones
     * means zero bytes - which is why it cannot simply be written as `len`. */
    u32 maxlen = len ? (u32)(len - 1) : 0x7FF;
    td->token = (u32)pid | ((u32)addr << 8) | ((u32)endpoint << 15) |
                ((u32)(toggle ? 1 : 0) << 19) | (maxlen << 21);

    td->buffer = (u32)buffer;
}

/* One control transfer: SETUP, then however many data packets, then a status
 * packet in the opposite direction. */
static bool control_in(uhci_t *u, u8 addr, bool low_speed,
                       u8 request_type, u8 request, u16 value, u16 index,
                       void *out, u16 length) {
    if (length > PAGE_SIZE - 8) return false;

    /* The setup packet goes in the first eight bytes of the bounce page and
     * the data lands after it, so one page serves the whole transfer. */
    u8 *setup = u->buf;
    setup[0] = request_type;
    setup[1] = request;
    setup[2] = (u8)value; setup[3] = (u8)(value >> 8);
    setup[4] = (u8)index; setup[5] = (u8)(index >> 8);
    setup[6] = (u8)length; setup[7] = (u8)(length >> 8);

    u8 *data = u->buf + 8;
    memset(data, 0, length);

    const u16 packet = low_speed ? 8 : 64;
    int i = 0;
    build_td(u, i, i + 1, PID_SETUP, addr, 0, false, 8, u->buf_phys, low_speed);
    i++;

    /* Data stage: the toggle alternates from one, per packet, and the first
     * data packet is always DATA1. */
    bool toggle = true;
    u16 done = 0;
    while (done < length && i < TD_COUNT - 1) {
        u16 chunk = (u16)(length - done);
        if (chunk > packet) chunk = packet;
        build_td(u, i, i + 1, PID_IN, addr, 0, toggle, chunk,
                 u->buf_phys + 8 + done, low_speed);
        toggle = !toggle;
        done = (u16)(done + chunk);
        i++;
    }

    /* Status: the opposite direction, no data, and always DATA1. */
    build_td(u, i, -1, PID_OUT, addr, 0, true, 0, 0, low_speed);
    i++;

    if (!run_chain(u, i, 1000)) return false;

    if (out && length) memcpy(out, data, length);
    return true;
}

/* --------------------------------------------------------------- the ports */

static void reset_port(uhci_t *u, int port) {
    u16 off = (u16)(PORTSC1 + port * 2);

    wr16(u, off, PORT_PR);
    timer_mdelay(50);                          /* the specification says 10ms
                                                * minimum; hubs in the wild
                                                * want more */
    wr16(u, off, 0);
    timer_udelay(300);

    /* Enable, then wait for the controller to agree it is enabled.  Writing
     * the change bits back is what clears them. */
    for (int i = 0; i < 10; i++) {
        u16 s = rd16(u, off);
        if (s & PORT_PE) break;
        wr16(u, off, (u16)((s & ~(PORT_PR)) | PORT_PE | PORT_CSC | PORT_PEDC));
        timer_mdelay(10);
    }
}

/* Ask whatever is on this port who it is.
 *
 * A device answers on address zero until it is given one, and this driver does
 * not assign addresses: there is nothing yet to hand the device to, so moving
 * it off address zero would only make it harder for whatever runs next. */
static void identify_port(uhci_t *u, int port) {
    u8 desc[18];
    memset(desc, 0, sizeof desc);

    if (!control_in(u, 0, u->low_speed[port], 0x80, 0x06 /* GET_DESCRIPTOR */,
                    0x0100 /* device, index 0 */, 0, desc, 18)) {
        kwarn("uhci", "port %d: a device is attached but did not answer",
              port + 1);
        return;
    }

    if (desc[0] < 18 || desc[1] != 0x01) {
        kwarn("uhci", "port %d: the descriptor it returned is not a device "
                      "descriptor (length %u, type %u)", port + 1, desc[0],
              desc[1]);
        return;
    }

    u->vendor[port]  = (u16)(desc[8]  | (desc[9]  << 8));
    u->product[port] = (u16)(desc[10] | (desc[11] << 8));
    devices_found++;

    kinfo("uhci", "port %d: %s-speed device %04x:%04x, USB %u.%u, class %02x",
          port + 1, u->low_speed[port] ? "low" : "full",
          u->vendor[port], u->product[port],
          desc[3], desc[2] >> 4, desc[4]);
}

static void scan_ports(uhci_t *u) {
    for (int p = 0; p < u->ports; p++) {
        u16 s = rd16(u, (u16)(PORTSC1 + p * 2));
        if (!(s & PORT_CCS)) continue;

        u->attached[p] = true;
        u->low_speed[p] = (s & PORT_LSDA) != 0;

        reset_port(u, p);

        s = rd16(u, (u16)(PORTSC1 + p * 2));
        if (!(s & PORT_PE)) {
            kwarn("uhci", "port %d: a device is attached but the port would "
                          "not enable (status %04x)", p + 1, s);
            continue;
        }

        identify_port(u, p);
    }
}

/* How many ports this controller has.
 *
 * There is no register that says.  The published way to find out is to read
 * consecutive port registers until one does not look like a port: a real one
 * always has bit 7 set, and reads as all-ones once past the end. */
static int count_ports(uhci_t *u) {
    int n = 0;
    while (n < MAX_PORTS) {
        u16 s = rd16(u, (u16)(PORTSC1 + n * 2));
        if (s == 0xFFFF || !(s & 0x0080)) break;
        n++;
    }
    /* Every UHCI controller has at least two, and a controller that answers
     * oddly is more likely to be reporting badly than to have none. */
    return n ? n : 2;
}

/* ------------------------------------------------------------------ bring-up */

static bool start_controller(uhci_t *u) {
    /* Interrupts off first: everything here is polled, and a controller left
     * asserting a line nothing handles spins the machine. */
    wr16(u, USBINTR, 0);

    wr16(u, USBCMD, CMD_HCRESET);
    for (int i = 0; i < 100; i++) {
        if (!(rd16(u, USBCMD) & CMD_HCRESET)) break;
        timer_mdelay(1);
    }
    if (rd16(u, USBCMD) & CMD_HCRESET) {
        kerr("uhci", "the controller did not come out of reset");
        return false;
    }

    u->frames = dma32(1, &u->frames_phys, "the frame list");
    if (!u->frames) return false;

    void *p = dma32(1, &u->td_phys, "the transfer descriptors");
    if (!p) return false;
    u->td = p;

    p = dma32(1, &u->qh_phys, "the queue head");
    if (!p) return false;
    u->qh = p;

    u->buf = dma32(1, &u->buf_phys, "the transfer buffer");
    if (!u->buf) return false;

    memset(u->td, 0, PAGE_SIZE);
    memset(u->qh, 0, PAGE_SIZE);

    u->qh->head = LINK_TERMINATE;
    u->qh->element = LINK_TERMINATE;

    /* Every frame points at the one queue head.  A zero entry would be a
     * pointer to physical address zero rather than an empty frame, so the
     * terminate bit is what "nothing here" is spelled with - and none of these
     * are empty anyway. */
    for (int i = 0; i < 1024; i++)
        u->frames[i] = (u32)u->qh_phys | LINK_QH;

    wr32(u, FRBASEADD, (u32)u->frames_phys);
    wr16(u, FRNUM, 0);
    outb((u16)(u->io + SOFMOD), 64);           /* the default 1ms frame */
    wr16(u, USBSTS, 0xFFFF);                   /* clear anything pending */

    wr16(u, USBCMD, CMD_RS | CMD_MAXP);

    for (int i = 0; i < 100; i++) {
        if (!(rd16(u, USBSTS) & STS_HALTED)) break;
        timer_mdelay(1);
    }
    if (rd16(u, USBSTS) & STS_HALTED) {
        kerr("uhci", "the controller would not start");
        return false;
    }

    return true;
}

/* Proof that the controller is executing the schedule rather than merely
 * being configured.
 *
 * The frame number advances only while the controller is walking the frame
 * list this driver built, one step every millisecond.  A controller that was
 * set up but never started, or started against a frame list it could not
 * reach, sits still - and every register read would still look correct.  This
 * is the difference between a driver that is written and one that runs.
 */
static bool schedule_is_running(uhci_t *u) {
    u16 first = (u16)(rd16(u, FRNUM) & 0x7FF);
    timer_mdelay(20);
    u16 then = (u16)(rd16(u, FRNUM) & 0x7FF);

    if (first == then) {
        kerr("uhci", "the frame counter is not moving (%u); the controller is "
                     "not running the schedule", first);
        return false;
    }

    u16 moved = (u16)((then - first) & 0x7FF);
    kinfo("uhci", "the schedule is running: the frame counter advanced %u "
                  "frames in 20 ms", moved);
    return true;
}

void uhci_init(void) {
    pci_dev_t *pci = NULL;

    /* Class 0c subclass 03 programming interface 00 is UHCI.  10 would be
     * OHCI, 20 EHCI and 30 xHCI, and each is a different controller entirely. */
    while ((pci = pci_find(0x0C, 0x03, 0x00, pci)) != NULL) {
        if (controller_count >= MAX_CONTROLLERS) break;

        /* UHCI lives in I/O space, always at BAR 4. */
        if (!pci->bar[4] || !pci->bar_is_io[4]) {
            kwarn("uhci", "%02x:%02x.%u reports UHCI with no I/O window",
                  pci->bus, pci->slot, pci->func);
            continue;
        }

        uhci_t *u = &controllers[controller_count];
        memset(u, 0, sizeof *u);
        u->io = (u16)(pci->bar[4] & ~3u);

        pci_enable_bus_master(pci);

        /* The firmware may have left its own USB support running on this
         * controller; taking it over means telling the chipset to stop.  The
         * register is Intel's and harmless to write on parts that lack it. */
        pci_write16(pci, 0xC0, 0x8F00);

        if (!start_controller(u)) continue;
        if (!schedule_is_running(u)) continue;

        u->ports = count_ports(u);
        pci_claim(pci, "uhci");
        controller_count++;

        kinfo("uhci", "%02x:%02x.%u: USB 1.1 controller at %04x, %d ports",
              pci->bus, pci->slot, pci->func, u->io, u->ports);

        scan_ports(u);
    }

    if (!controller_count) { kdebug("uhci", "no UHCI controller present"); return; }

    /* Said plainly, because a controller that is up and a controller that
     * carries devices are different things and the log is where that gets
     * confused. */
    if (devices_found)
        kinfo("uhci", "%d controller(s), %d device(s) identified; class "
                      "drivers are not attached to this controller yet, so "
                      "nothing here is in use",
              controller_count, devices_found);
    else
        kinfo("uhci", "%d controller(s) running, nothing plugged into them",
              controller_count);
}
