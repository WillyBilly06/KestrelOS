/* xhci.c - USB 3 host controller, and enough of USB to run a keyboard.
 *
 * A modern desktop or laptop has no PS/2 controller at all: once the firmware
 * hands over, the keyboard and mouse are USB devices behind an xHCI host
 * controller and nothing else will read them.  This driver brings the
 * controller up, enumerates whatever is plugged into the root hub, and hands
 * HID boot-protocol devices to the class driver.
 *
 * Everything runs on one kernel thread.  The controller reports through an
 * event ring, and a thread that owns both the command ring and the event ring
 * can wait for a command to complete simply by draining events until its own
 * shows up - no interrupt handler, no locking, and no half-finished
 * enumeration to unpick if a device is unplugged mid-sequence.  Interrupt
 * endpoints are polled off the same loop at 1 ms, which is faster than the 8 ms
 * a boot keyboard is scheduled at, so nothing is lost by not taking an IRQ.
 */
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "time.h"
#include "proc.h"
#include "klog.h"
#include "usb.h"

/* ------------------------------------------------------------- registers */

/* Capability registers, at the start of BAR 0. */
#define CAP_CAPLENGTH   0x00
#define CAP_HCIVERSION  0x02
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCSPARAMS3  0x0C
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* Operational registers, at BAR 0 + CAPLENGTH. */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_DNCTRL       0x14
#define OP_CRCR         0x18
#define OP_DCBAAP       0x30
#define OP_CONFIG       0x38
#define OP_PORTS        0x400
#define PORT_STRIDE     0x10

#define USBCMD_RS       (1u << 0)
#define USBCMD_HCRST    (1u << 1)
#define USBCMD_INTE     (1u << 2)
#define USBCMD_HSEE     (1u << 3)

#define USBSTS_HCH      (1u << 0)
#define USBSTS_HSE      (1u << 2)
#define USBSTS_EINT     (1u << 3)
#define USBSTS_PCD      (1u << 4)
#define USBSTS_CNR      (1u << 11)
#define USBSTS_HCE      (1u << 12)

#define PORTSC_CCS      (1u << 0)
#define PORTSC_PED      (1u << 1)
#define PORTSC_OCA      (1u << 3)
#define PORTSC_PR       (1u << 4)
#define PORTSC_PLS_SHIFT 5
#define PORTSC_PP       (1u << 9)
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_CSC      (1u << 17)
#define PORTSC_PEC      (1u << 18)
#define PORTSC_WRC      (1u << 19)
#define PORTSC_OCC      (1u << 20)
#define PORTSC_PRC      (1u << 21)
#define PORTSC_PLC      (1u << 22)
#define PORTSC_CEC      (1u << 23)
#define PORTSC_WPR      (1u << 31)

/* Write-1-to-clear bits, plus PED which disables the port when written 1.
 * Every read-modify-write of PORTSC has to mask these off or it will clear
 * status it never meant to touch. */
#define PORTSC_RW1C  (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | PORTSC_OCC | \
                      PORTSC_PRC | PORTSC_PLC | PORTSC_CEC)

/* Runtime registers, at BAR 0 + RTSOFF.  Interrupter 0 begins at 0x20. */
#define RT_IR0          0x20
#define IR_IMAN         0x00
#define IR_IMOD         0x04
#define IR_ERSTSZ       0x08
#define IR_ERSTBA       0x10
#define IR_ERDP         0x18

/* TRB types. */
#define TRB_NORMAL       1
#define TRB_SETUP        2
#define TRB_DATA         3
#define TRB_STATUS       4
#define TRB_ISOCH        5
#define TRB_LINK         6
#define TRB_ENABLE_SLOT  9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEV  11
#define TRB_CONFIG_EP    12
#define TRB_EVAL_CTX     13
#define TRB_RESET_EP     14
#define TRB_STOP_EP      15
#define TRB_SET_DEQ      16
#define TRB_RESET_DEV    17
#define TRB_NOOP_CMD     23
#define TRB_EV_TRANSFER  32
#define TRB_EV_CMD       33
#define TRB_EV_PORT      34
#define TRB_EV_HC        37

/* TRB control bits. */
#define TRB_C     (1u << 0)
#define TRB_TC    (1u << 1)
#define TRB_ISP   (1u << 2)
#define TRB_CH    (1u << 4)
#define TRB_IOC   (1u << 5)
#define TRB_IDT   (1u << 6)
#define TRB_BSR   (1u << 9)
#define TRB_TYPE(t) ((u32)(t) << 10)

/* Completion codes worth naming. */
#define CC_SUCCESS       1
#define CC_SHORT_PACKET  13

/* The completion codes that stop an endpoint dead.
 *
 * This is the part that matters and the part that was missing.  When a
 * transfer fails for any of these reasons the controller does not simply
 * report it and carry on - it puts the endpoint into the Halted state, and a
 * halted endpoint ignores its doorbell forever.  Recovering means two
 * commands: Reset Endpoint to take it out of Halted, and Set TR Dequeue to
 * tell it where to start again.  Queueing another transfer without doing that
 * achieves nothing at all.
 *
 * The driver used to recover from only two of them.  The one it did not
 * recover from that matters most is a transaction error, which is what a
 * device gives when a packet is lost - and a 2.4 GHz dongle, or anything two
 * hubs down, loses packets as a matter of course while it settles.  One such
 * error at the wrong moment and that device is silent for the rest of the
 * session, having enumerated perfectly.
 */
#define CC_DATA_BUFFER      2
#define CC_BABBLE           3
#define CC_USB_TRANSACTION  4
#define CC_TRB_ERROR        5
#define CC_STALL            6
#define CC_RESOURCE         7

static bool code_halts_endpoint(u32 code) {
    return code == CC_DATA_BUFFER || code == CC_BABBLE ||
           code == CC_USB_TRANSACTION || code == CC_TRB_ERROR ||
           code == CC_STALL;
}

/* USB speed IDs as they appear in PORTSC. */
#define SPEED_FULL   1
#define SPEED_LOW    2
#define SPEED_HIGH   3
#define SPEED_SUPER  4

/* ------------------------------------------------------------------- hubs
 *
 * A hub is an ordinary USB device of class 9 whose interesting parts are all
 * reached through class requests rather than through an endpoint.  Everything
 * below is from the USB specification's hub chapter and has not changed since
 * USB 1.1, which is why one piece of code covers hubs of every generation.
 */
#define USB_CLASS_HUB          0x09

#define HUB_DT_HUB             0x29      /* the descriptor, for USB 2 and below */
#define HUB_DT_SS_HUB          0x2A      /* and for SuperSpeed                  */

#define HUB_PORT_CONNECTION    0
#define HUB_PORT_ENABLE        1
#define HUB_PORT_RESET         4
#define HUB_PORT_POWER         8
#define HUB_C_PORT_CONNECTION  16
#define HUB_C_PORT_ENABLE      17
#define HUB_C_PORT_RESET       20

/* wPortStatus, the low half of what GetPortStatus returns. */
#define HUB_STAT_CONNECTION    0x0001
#define HUB_STAT_ENABLE        0x0002
#define HUB_STAT_RESET         0x0010
#define HUB_STAT_POWER         0x0100
#define HUB_STAT_LOW_SPEED     0x0200
#define HUB_STAT_HIGH_SPEED    0x0400

#define MAX_HUB_DEPTH          4         /* the route string holds five tiers */

typedef struct {
    u64 param;
    u32 status;
    u32 control;
} __attribute__((packed)) trb_t;

typedef struct {
    u64 base;
    u32 size;
    u32 reserved;
} __attribute__((packed)) erst_entry_t;

typedef struct {
    trb_t *trb;
    u64    phys;
    u32    size;        /* entries, the link TRB included on transfer rings */
    u32    index;
    u8     cycle;
} ring_t;

/* Devices per controller.  Sixteen was enough when only the machine's own
 * sockets were reachable; with hubs it is not - the report from a real desk
 * listed a hub, two more hubs behind it, a keyboard, a mouse, a microphone,
 * a Bluetooth radio and a memory stick, on one controller.  The controller
 * itself limits how many slots exist, and this is now large enough that its
 * limit is the one that applies rather than ours. */
#define MAX_DEVICES      64
/* Endpoints configured per device.  A disk needs two on its own - bulk in
 * and bulk out - and a composite device can want several more, so this is no
 * longer the same thing as "how many interfaces". */
#define MAX_INTERFACES    8
#define CMD_RING_SIZE   128
#define EVENT_RING_SIZE 128
#define XFER_RING_SIZE   64
#define REPORT_SLOT      64      /* bytes reserved per in-flight transfer */

typedef struct {
    bool     active;
    /* Whether this endpoint is polled forever or driven a transfer at a time.
     *
     * The difference decides what happens when a transfer completes.  An
     * interrupt endpoint belongs to a keyboard or a mouse: the report goes to
     * the class driver and another buffer is immediately queued, because the
     * next keystroke can arrive at any moment and nothing is going to ask for
     * it.  A bulk endpoint belongs to a disk and is the opposite - whoever
     * issued the transfer is waiting for it and will issue the next one, and
     * queueing another here would put a read on the wire that nobody wants
     * and hand its data to a driver that is not there. */
    bool     polled;
    bool     isoch;            /* scheduled every interval, never retried    */
    u32      errors;           /* transfers that halted it and were recovered */
    bool     needs_reset;      /* halted; RESET_EP/SET_DEQ owed at top level  */
    u32      completions;      /* transfers that came back, of any kind      */
    u64      armed_at_ms;      /* when it was last given buffers to fill     */
    int      rearms;           /* how many times it has been kicked          */
    u8       dci;              /* device context index: 2*n+1 for an IN ep  */
    u8       addr;             /* endpoint address as the descriptor gives */
    u16      max_packet;
    ring_t   ring;
    u8      *buf;
    u64      buf_phys;
    void    *drv;              /* class driver context */
    char     drv_name[24];
} usb_endpoint_t;

struct usb_device {
    bool  present;
    u8    slot;
    u8    port;                /* 1-based port on whatever it is plugged into */
    u8    speed;

    /* Where it is in the tree.
     *
     * A controller does not address a device by "port 3"; it addresses it by
     * the path from the root down to it, four bits per tier, which xHCI calls
     * the route string.  A device in a root port has an empty route, and that
     * is the only case this driver used to handle - so a keyboard in a hub, or
     * in a monitor, or in the front of a case that wires its sockets through
     * one, did not exist as far as the system was concerned.
     *
     * root_port stays the root port for everything below it, because that is
     * what the field in the slot context means: which port of the root hub
     * this whole branch hangs off.
     */
    u32   route;               /* four bits per tier, tier one lowest        */
    u8    root_port;
    u8    depth;               /* 0 for a device in a root port              */
    struct usb_device *parent; /* the hub it is plugged into, if any         */

    /* Hubs, and what hangs off them. */
    bool  is_hub;
    u8    hub_ports;
    u8    hub_powered;         /* ports told to switch their power on        */
    u8    port_fails[16];      /* how many times each port refused to come up */

    /* A full or low speed device below a high-speed hub is not spoken to
     * directly: the hub translates for it, and the controller has to be told
     * which hub is doing the translating and on which of its ports.  Getting
     * this wrong is the classic way an old keyboard in a USB 2 hub enumerates
     * and then never sends anything. */
    u8    tt_slot;
    u8    tt_port;
    u16   max_packet0;
    u16   vendor, product;
    char  name[48];

    u32  *dev_ctx;  u64 dev_ctx_phys;
    u32  *in_ctx;   u64 in_ctx_phys;
    u8   *xfer_buf; u64 xfer_buf_phys;     /* a page for control transfers */

    ring_t ep0;
    usb_endpoint_t ep[MAX_INTERFACES];
    int    ep_count;

    /* The bulk pair, when this device has one.  Indices into ep[], or -1.
     * Held here rather than searched for on every transfer because a storage
     * device does two of these per block read and the search would be the
     * most-executed loop in the driver. */
    int   bulk_in_ep;
    int   bulk_out_ep;

    /* The controller this device is plugged into.  Every operation on a device
     * goes through its controller's rings, and a device cannot move between
     * them, so carrying the pointer is both correct and simpler than passing
     * it alongside at every call. */
    struct xhci *hc;
};

/* ------------------------------------------------------------------ state
 *
 * All of this used to be one set of variables, on the assumption that a
 * machine has one USB controller.  A desktop has several, and which one a
 * given socket is wired to is a fact about the case rather than about the
 * chipset: on the machine this was fixed for, the processor package provides
 * the USB4 ports and a separate chipset provides everything the back panel and
 * the front-panel headers are connected to.  Driving one of them means the
 * keyboard works or does not depending on which socket it is in, which is not
 * a thing anybody should have to know.
 *
 * So the state is per controller and there is an array of them.  Each gets its
 * own rings, its own device table and its own thread; nothing is shared but
 * the counts of how many keyboards and mice exist altogether, which is a
 * question about the machine rather than about a controller.
 */
typedef struct xhci {
    bool          present;

    volatile u8  *cap_regs;
    volatile u8  *op_regs;
    volatile u8  *rt_regs;
    volatile u32 *doorbells;

    u32           max_slots, max_ports, context_size;
    bool          controller_up;
    volatile bool initial_scan_done;   /* first port+hub scan finished */
    bool          port_is_usb3[256];

    ring_t        cmd_ring;
    ring_t        event_ring;
    erst_entry_t *erst;
    u64          *dcbaa;
    u64           dcbaa_phys;

    usb_device_t  devices[MAX_DEVICES];
    u8            slot_to_device[256];     /* slot id -> index into devices+1 */

    /* Where it is on the bus, so every message says which controller it is
     * about.  With one controller that was never in question; with four it is
     * the first thing anyone needs to know. */
    u8            bus, slot, func;
    u16           pci_vendor, pci_device;

    /* What has come back from the hardware.  Counting these separates the
     * three things that look the same from outside when an input device is
     * silent: the controller posting nothing at all, posting events for other
     * endpoints but never this one, and posting them where nothing collects
     * them. */
    u32           events_seen;

    /* What the heartbeat last reported, so it can stay quiet when nothing has
     * moved.  See the note where it is used. */
    u32           beat_events, beat_transfers, beat_completions, beat_endpoints;
    u64           beat_forced_at;
    u32           transfer_events;
    u8            last_transfer_cc;

    /* Completion routing.  The event ring is one shared queue, but transfers
     * are started from many threads (disk I/O, and the housekeeping thread that
     * drains events for HID).  Whichever thread's pump() pops an event must give
     * it to the thread WAITING on that token rather than discarding it - or two
     * concurrent transfers steal each other's completions and both time out,
     * which wedged the USB disk the instant a second thread touched it.  Each
     * waiting pump registers its token here; any pump that pops a matching event
     * delivers it into the slot instead of throwing it away. */
    struct xfer_waiter {
        volatile u64 token;
        volatile u32 ready;
        trb_t        ev;
    }             waiters[16];
    volatile u32  waiters_lock;
    u32           class_poll_active; /* reject recursive class-level polling */
} xhci_t;

#define MAX_CONTROLLERS 8

static xhci_t controllers[MAX_CONTROLLERS];
static int    controller_count;

static int  keyboards, mice;

/* ------------------------------------------------------------ mmio helpers */

static inline u32 rd32(volatile u8 *base, u32 off) {
    return *(volatile u32 *)(base + off);
}
static inline void wr32(volatile u8 *base, u32 off, u32 v) {
    *(volatile u32 *)(base + off) = v;
}
/* 64-bit registers are written as two 32-bit stores: a controller that does
 * not implement 64-bit addressing will reject a single quadword write, and the
 * low-then-high order is what every controller latches on. */
static inline void wr64(volatile u8 *base, u32 off, u64 v) {
    volatile u32 *r = (volatile u32 *)(base + off);
    r[0] = (u32)v;
    r[1] = (u32)(v >> 32);
}
static inline u32 portsc(xhci_t *x, u32 port) {          /* port is 1-based */
    return rd32(x->op_regs, OP_PORTS + (port - 1) * PORT_STRIDE);
}
static inline void portsc_write(xhci_t *x, u32 port, u32 v) {
    wr32(x->op_regs, OP_PORTS + (port - 1) * PORT_STRIDE, v);
}
/* A PORTSC value safe to write back after modifying one control bit. */
static inline u32 portsc_keep(u32 v) {
    return v & ~(PORTSC_RW1C | PORTSC_PED);
}

static void barrier(void) { __asm__ volatile("" ::: "memory"); }

/* --------------------------------------------------------------- dma memory */

static void *dma_alloc(size_t bytes, u64 *phys_out) {
    return dma_alloc_pages((bytes + PAGE_SIZE - 1) / PAGE_SIZE, phys_out);
}

static void dma_free(void *v, size_t bytes) {
    dma_free_pages(v, (bytes + PAGE_SIZE - 1) / PAGE_SIZE);
}

/* ------------------------------------------------------------------- rings */

static bool ring_init(ring_t *r, u32 entries, bool linked) {
    memset(r, 0, sizeof *r);
    r->trb = dma_alloc(entries * sizeof(trb_t), &r->phys);
    if (!r->trb) return false;
    r->size = entries;
    r->index = 0;
    r->cycle = 1;
    if (linked) {
        /* The last slot sends the controller back to the start and flips the
         * cycle state it expects, which is what makes the ring circular. */
        trb_t *link = &r->trb[entries - 1];
        link->param = r->phys;
        link->status = 0;
        link->control = TRB_TYPE(TRB_LINK) | TRB_TC | TRB_C;
    }
    return true;
}

static void ring_free(ring_t *r) {
    if (r->trb) dma_free(r->trb, r->size * sizeof(trb_t));
    memset(r, 0, sizeof *r);
}

/* Append one TRB and return the address the controller will report it by. */
static u64 ring_push(ring_t *r, u64 param, u32 status, u32 control) {
    trb_t *t = &r->trb[r->index];
    u64 addr = r->phys + (u64)r->index * sizeof(trb_t);

    t->param = param;
    t->status = status;
    barrier();
    /* The cycle bit goes last: until it flips the controller treats the slot
     * as still owned by software, so it can never read a half-written TRB. */
    t->control = (control & ~TRB_C) | r->cycle;
    barrier();

    r->index++;
    if (r->index == r->size - 1) {          /* the link TRB */
        trb_t *link = &r->trb[r->size - 1];
        link->control = (link->control & ~TRB_C) | r->cycle;
        barrier();
        r->index = 0;
        r->cycle ^= 1;
    }
    return addr;
}

static void ring_doorbell(xhci_t *x, u32 slot, u32 target) {
    x->doorbells[slot] = target;
    (void)x->doorbells[slot];                  /* read back to flush the write */
}

/* ------------------------------------------------------------- event ring */

static bool event_pop(xhci_t *x, trb_t *out) {
    trb_t *t = &x->event_ring.trb[x->event_ring.index];
    if ((t->control & TRB_C) != x->event_ring.cycle) return false;
    *out = *t;
    barrier();
    x->event_ring.index++;
    if (x->event_ring.index == x->event_ring.size) {
        x->event_ring.index = 0;
        x->event_ring.cycle ^= 1;
    }
    return true;
}

static void event_ack(xhci_t *x) {
    u64 dq = x->event_ring.phys + (u64)x->event_ring.index * sizeof(trb_t);
    wr64(x->rt_regs, RT_IR0 + IR_ERDP, dq | (1ULL << 3));   /* with the busy bit */
}

/* ------------------------------------------------------------- device ctx */

/* Contexts are either 32 or 64 bytes wide depending on HCCPARAMS1.CSZ, so
 * every index into one has to be scaled. */
static u32 *ctx_at(xhci_t *x, u32 *base, int index) {
    return base + (size_t)index * (x->context_size / 4);
}

static u32 *slot_ctx(xhci_t *x, u32 *dev_or_in, bool input) {
    return ctx_at(x, dev_or_in, input ? 1 : 0);
}
static u32 *ep_ctx(xhci_t *x, u32 *dev_or_in, int dci, bool input) {
    return ctx_at(x, dev_or_in, input ? dci + 1 : dci);
}

/* Endpoint types in an endpoint context. */
#define EP_TYPE_CONTROL 4
/* Endpoint types, as the endpoint context encodes them.  The direction is part
 * of the type rather than a separate bit, which is why there are two names for
 * bulk and only one for the interrupt endpoints this driver polls. */
/* Isochronous endpoints, which are what audio and video arrive on.  They are
 * the two types the numbering puts either side of the bulk ones. */
#define EP_TYPE_ISOCH_OUT 1
#define EP_TYPE_ISOCH_IN  5

#define EP_TYPE_BULK_OUT 2
#define EP_TYPE_BULK_IN  6
#define EP_TYPE_INT_IN  7

/* ------------------------------------------------------- forward pieces */

static void handle_transfer(xhci_t *x, const trb_t *e);
static void handle_port_change(xhci_t *x, const trb_t *e);
static void queue_report(usb_device_t *dev, usb_endpoint_t *ep);

/* The completion-routing registry (see the note in xhci_t).  A brief spinlock
 * guards it; nothing that touches a disk or submits a transfer is done while it
 * is held, so it cannot deadlock. */
static void waiters_acquire(xhci_t *x) {
    while (__atomic_exchange_n(&x->waiters_lock, 1u, __ATOMIC_ACQUIRE)) pause_cpu();
}
static void waiters_release_lock(xhci_t *x) {
    __atomic_store_n(&x->waiters_lock, 0u, __ATOMIC_RELEASE);
}
static struct xfer_waiter *waiter_register(xhci_t *x, u64 token) {
    if (!token) return NULL;
    struct xfer_waiter *w = NULL;
    waiters_acquire(x);
    for (size_t i = 0; i < ARRAY_LEN(x->waiters); i++)
        if (x->waiters[i].token == 0) {
            w = &x->waiters[i]; w->token = token; w->ready = 0; break;
        }
    waiters_release_lock(x);
    return w;   /* NULL if full: that pump just matches its own popped events */
}
static void waiter_unregister(xhci_t *x, struct xfer_waiter *w) {
    if (!w) return;
    waiters_acquire(x);
    w->token = 0; w->ready = 0;
    waiters_release_lock(x);
}
/* Deliver an event to the thread waiting on its token, if any.  Returns true
 * when it was claimed - the caller must then NOT dispatch it as unsolicited. */
static bool waiter_deliver(xhci_t *x, u64 token, const trb_t *ev) {
    if (!token) return false;
    bool ok = false;
    waiters_acquire(x);
    for (size_t i = 0; i < ARRAY_LEN(x->waiters); i++)
        if (x->waiters[i].token == token && !x->waiters[i].ready) {
            x->waiters[i].ev = *ev;
            __atomic_store_n(&x->waiters[i].ready, 1u, __ATOMIC_RELEASE);
            ok = true; break;
        }
    waiters_release_lock(x);
    return ok;
}

/* Drain the event ring.  When `token` is non-zero, keep going until an event
 * arrives whose TRB pointer matches it, and return that event's completion
 * code; otherwise process what is pending and return 0.
 *
 * Events for other endpoints are dispatched on the way past, which is what
 * lets a keyboard keep reporting while another device is being enumerated.
 * An event whose token belongs to ANOTHER thread's transfer is handed to that
 * thread through the waiter registry rather than discarded - otherwise two
 * concurrent pumps steal each other's completions. */
static int pump(xhci_t *x, u64 token, trb_t *matched, u32 timeout_ms) {
    u64 deadline = g_uptime_ms + timeout_ms;
    struct xfer_waiter *self = waiter_register(x, token);

    for (;;) {
        trb_t e;
        bool any = false;
        int  result = -1;
        bool done = false;

        while (event_pop(x, &e)) {
            any = true;
            u32 type = (e.control >> 10) & 0x3F;
            u32 code = (e.status >> 24) & 0xFF;

            /* Counted here, before the match below takes one out of the
             * stream.  Counting after it meant every command completion and
             * every control transfer - which is nearly all of them - went
             * uncounted, and the total read as though the controller had
             * barely spoken. */
            x->events_seen++;
            if (type == TRB_EV_TRANSFER) {
                x->transfer_events++;
                x->last_transfer_cc = (u8)code;
            }

            if (token && e.param == token &&
                (type == TRB_EV_CMD || type == TRB_EV_TRANSFER)) {
                result = (int)code;
                if (matched) *matched = e;
                done = true;
                continue;
            }

            /* Not this pump's completion - if another thread is waiting on it,
             * hand it over rather than throwing it away. */
            if ((type == TRB_EV_CMD || type == TRB_EV_TRANSFER) && e.param &&
                waiter_deliver(x, e.param, &e))
                continue;

            switch (type) {
            case TRB_EV_TRANSFER: handle_transfer(x, &e); break;
            case TRB_EV_PORT:     handle_port_change(x, &e); break;
            case TRB_EV_HC:
                kerr("usb", "host controller error, completion code %u", code);
                break;
            default:
                break;
            }
        }

        if (any) event_ack(x);
        if (done) { waiter_unregister(x, self); return result; }

        /* Another thread's pump may have popped and delivered my completion. */
        if (self && __atomic_load_n(&self->ready, __ATOMIC_ACQUIRE)) {
            if (matched) *matched = self->ev;
            int r = (int)((self->ev.status >> 24) & 0xFF);
            waiter_unregister(x, self);
            return r;
        }

        if (!token) return 0;

        if (g_uptime_ms >= deadline) { waiter_unregister(x, self); return -1; }
        sched_sleep_ms(1);
    }
}

/* A class driver can wait inside enumeration on this controller's own thread.
 * Sleeping there does not run its outer event loop.  Drain only pending events:
 * never scan ports/hubs or recover endpoints recursively.  pump() retains the
 * existing waiter-token routing and event-pop locking for concurrent I/O. */
void usb_poll_device_events(usb_device_t *dev) {
    if (!dev || !dev->present || !dev->hc) return;
    xhci_t *x = dev->hc;
    if (!x->controller_up) return;
    u32 expected = 0;
    if (!__atomic_compare_exchange_n(&x->class_poll_active, &expected, 1u,
                                     false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    pump(x, 0, NULL, 0);
    __atomic_store_n(&x->class_poll_active, 0u, __ATOMIC_RELEASE);
}

/* Post a command and wait for its completion event. */
static int command(xhci_t *x, u64 param, u32 status, u32 control, trb_t *out) {
    u64 addr = ring_push(&x->cmd_ring, param, status, control);
    ring_doorbell(x, 0, 0);
    int cc = pump(x, addr, out, 1000);
    if (cc < 0) kwarn("usb", "command type %u timed out", (control >> 10) & 0x3F);
    return cc;
}

/* --------------------------------------------------------- control transfer */

int usb_control(usb_device_t *dev, u8 request_type, u8 request,
                u16 value, u16 index, void *data, u16 length) {
    if (!dev || !dev->present) return -1;
    if (length > PAGE_SIZE) return -1;

    xhci_t *x = dev->hc;   /* the controller this device is on */

    bool in = (request_type & USB_DIR_IN) != 0;

    /* The setup packet travels inside the TRB itself. */
    u64 setup = (u64)request_type | ((u64)request << 8) | ((u64)value << 16) |
                ((u64)index << 32) | ((u64)length << 48);

    u32 trt = length ? (in ? 3u : 2u) : 0u;

    if (length && !in && data) memcpy(dev->xfer_buf, data, length);
    if (length && in) memset(dev->xfer_buf, 0, length);

    ring_push(&dev->ep0, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));

    if (length)
        ring_push(&dev->ep0, dev->xfer_buf_phys, length,
                  TRB_TYPE(TRB_DATA) | (in ? (1u << 16) : 0));

    /* The status stage runs the other way round, and is the one that reports. */
    u64 token = ring_push(&dev->ep0, 0, 0,
                          TRB_TYPE(TRB_STATUS) | TRB_IOC |
                          ((length && in) ? 0 : (1u << 16)));

    ring_doorbell(x, dev->slot, 1);            /* endpoint 0 is DCI 1 */

    trb_t ev;
    int cc = pump(x, token, &ev, 1000);
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
        kdebug("usb", "control request %#x on slot %u failed, code %d",
               request, dev->slot, cc);
        return -1;
    }

    if (length && in && data) memcpy(data, dev->xfer_buf, length);
    return length;
}

/* ------------------------------------------------------------ enumeration */

const char *usb_speed_name(u8 speed) {
    switch (speed) {
    case SPEED_FULL:  return "full speed";
    case SPEED_LOW:   return "low speed";
    case SPEED_HIGH:  return "high speed";
    case SPEED_SUPER: return "SuperSpeed";
    case 5:           return "SuperSpeed+";
    default:          return "unknown speed";
    }
}

const char *usb_device_name(const usb_device_t *dev) {
    return dev ? dev->name : "";
}

static u16 default_max_packet(u8 speed) {
    switch (speed) {
    case SPEED_LOW:   return 8;
    case SPEED_FULL:  return 8;      /* corrected once the descriptor arrives */
    case SPEED_HIGH:  return 64;
    default:          return 512;
    }
}

/* The endpoint context wants an interval expressed as 2^n microframes.  Full
 * and low speed descriptors count whole 1 ms frames instead, so they have to be
 * converted rather than passed through. */
static u8 encode_interval(u8 speed, u8 b_interval) {
    if (speed == SPEED_HIGH || speed >= SPEED_SUPER) {
        if (b_interval == 0) return 0;
        return (u8)(b_interval > 16 ? 15 : b_interval - 1);
    }
    u32 microframes = (b_interval ? b_interval : 1) * 8;
    u8 n = 0;
    while (n < 15 && (1u << (n + 1)) <= microframes) n++;
    return n;
}

/* Read a USB string descriptor and flatten its UTF-16 into ASCII. */
static void read_string(usb_device_t *dev, u8 index, char *out, size_t cap) {

    out[0] = 0;
    if (!index || cap < 2) return;

    u8 buf[128];
    /* Language id 0x0409; asking for the first two bytes gives the length. */
    if (usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR, (u16)((USB_DT_STRING << 8) | index),
                    0x0409, buf, 2) < 0)
        return;
    u8 len = buf[0];
    if (len < 4 || len > sizeof buf) return;
    if (usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR, (u16)((USB_DT_STRING << 8) | index),
                    0x0409, buf, len) < 0)
        return;

    size_t n = 0;
    for (int i = 2; i + 1 < len && n + 1 < cap; i += 2) {
        u16 ch = (u16)(buf[i] | (buf[i + 1] << 8));
        out[n++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
    }
    /* Trim the trailing spaces vendors like to pad with. */
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

static void free_device(usb_device_t *dev) {
    xhci_t *x = dev->hc;   /* the controller this device is on */

    for (int i = 0; i < dev->ep_count; i++) {
        if (!dev->ep[i].active) continue;
        if (dev->ep[i].drv) usbhid_detach(dev->ep[i].drv);
        ring_free(&dev->ep[i].ring);
        if (dev->ep[i].buf) dma_free(dev->ep[i].buf, PAGE_SIZE);
    }
    ring_free(&dev->ep0);
    if (dev->dev_ctx)  dma_free(dev->dev_ctx, x->context_size * 32);
    if (dev->in_ctx)   dma_free(dev->in_ctx, x->context_size * 33);
    if (dev->xfer_buf) dma_free(dev->xfer_buf, PAGE_SIZE);
    if (dev->slot) {
        x->dcbaa[dev->slot] = 0;
        x->slot_to_device[dev->slot] = 0;
    }
    memset(dev, 0, sizeof *dev);
}

/* Bring one interface's interrupt IN endpoint up and start it reporting. */
static bool configure_endpoint(usb_device_t *dev, const usb_interface_t *ifc,
                               void *drv, const char *drv_name) {
    xhci_t *x = dev->hc;   /* the controller this device is on */

    if (dev->ep_count >= MAX_INTERFACES) return false;
    usb_endpoint_t *ep = &dev->ep[dev->ep_count];

    u8 num = ifc->ep_addr & 0x0F;
    ep->dci = (u8)(num * 2 + 1);            /* IN endpoints are odd */
    ep->addr = ifc->ep_addr;
    ep->max_packet = ifc->ep_max_packet ? ifc->ep_max_packet : 8;
    if (ep->max_packet > REPORT_SLOT) ep->max_packet = REPORT_SLOT;

    if (!ring_init(&ep->ring, XFER_RING_SIZE, true)) return false;
    ep->buf = dma_alloc(PAGE_SIZE, &ep->buf_phys);
    if (!ep->buf) { ring_free(&ep->ring); return false; }

    /* Ask the controller to add just this endpoint, leaving everything already
     * configured in place. */
    memset(dev->in_ctx, 0, x->context_size * 33);
    dev->in_ctx[0] = 0;                                   /* drop nothing */
    dev->in_ctx[1] = (1u << 0) | (1u << ep->dci);         /* slot + this ep */

    u32 *sc = slot_ctx(x, dev->in_ctx, true);
    u32 *dsc = slot_ctx(x, dev->dev_ctx, false);

    /* Context entries is the highest endpoint index in use, so it can only
     * ever grow.  Writing this endpoint's index flat would LOWER it whenever a
     * device's endpoints are configured out of order - and the controller
     * treats everything above the count as not configured, so an earlier
     * endpoint would silently stop being serviced.  A mouse that presents
     * three interfaces is exactly the case where that happens. */
    u32 entries = (dsc[0] >> 27) & 0x1F;
    if (ep->dci > entries) entries = ep->dci;
    sc[0] = (dsc[0] & 0x07FFFFFF) | (entries << 27);
    sc[1] = dsc[1];
    sc[2] = dsc[2];
    sc[3] = dsc[3];

    u32 *ec = ep_ctx(x, dev->in_ctx, ep->dci, true);
    ec[0] = (u32)encode_interval(dev->speed, ifc->ep_interval) << 16;
    ec[1] = (3u << 1) |                                   /* three retries   */
            ((u32)EP_TYPE_INT_IN << 3) |
            ((u32)ifc->int_burst << 8) |                  /* max burst size  */
            ((u32)ep->max_packet << 16);
    ec[2] = (u32)(ep->ring.phys | 1);                     /* dequeue + cycle */
    ec[3] = (u32)(ep->ring.phys >> 32);
    ec[4] = ep->max_packet | ((u32)ep->max_packet << 16); /* average, ESIT   */

    int cc = command(x, dev->in_ctx_phys, 0,
                     TRB_TYPE(TRB_CONFIG_EP) | ((u32)dev->slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        kwarn("usb", "%s: cannot configure endpoint %#x (code %d)",
              dev->name, ifc->ep_addr, cc);
        ring_free(&ep->ring);
        dma_free(ep->buf, PAGE_SIZE);
        memset(ep, 0, sizeof *ep);
        return false;
    }

    {   /* What the controller now thinks of this endpoint, read back from its
         * own copy rather than assumed from the command succeeding.
         *
         * State 1 is Running; 0 is Disabled and 2 is Halted, and either of
         * those means it will never fetch a transfer however often the
         * doorbell is rung - while the command that configured it still
         * reported success.  On a machine where an input device is claimed and
         * then silent, this line is what separates "the controller is not
         * polling it" from "it is polling and the device sends nothing". */
        u32 *live = ep_ctx(x, dev->dev_ctx, ep->dci, false);
        u32 state = live[0] & 7;
        if (state != 1)
            kwarn("usb", "%s: endpoint %#x is %s, not running - it will never "
                         "report", dev->name, ifc->ep_addr,
                  state == 2 ? "halted" : "disabled");
        else
            kdebug("usb", "%s: endpoint %#x running, polling every %u frames",
                   dev->name, ifc->ep_addr, 1u << ((live[0] >> 16) & 0xFF));
    }

    ep->drv = drv;
    strlcpy(ep->drv_name, drv_name, sizeof ep->drv_name);
    ep->polled = true;
    ep->active = true;
    ep->armed_at_ms = g_uptime_ms;
    dev->ep_count++;

    /* Keep several transfers outstanding so a report is never waiting on the
     * thread to come round and queue a buffer for it. */
    for (int i = 0; i < 4; i++) queue_report(dev, ep);
    return true;
}

static void queue_report(usb_device_t *dev, usb_endpoint_t *ep) {
    xhci_t *x = dev->hc;   /* the controller this device is on */

    u32 slot_index = ep->ring.index;
    u64 buf = ep->buf_phys + (u64)slot_index * REPORT_SLOT;
    ring_push(&ep->ring, buf, ep->max_packet, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    ring_doorbell(x, dev->slot, ep->dci);
}

/* How much an isochronous endpoint moves in one service interval: one packet,
 * times however many the burst allows.  The multiplier a SuperSpeed device may
 * also declare is not used here, so one. */
static u32 isoch_per_interval(u16 max_packet, u8 burst) {
    return (u32)max_packet * ((u32)burst + 1);
}

/* Fill the first two words of an isochronous endpoint context.
 *
 * Separated from the configuring so it can be checked without a controller:
 * every field here is a bit position in a structure the hardware reads, and
 * being one shift out is not something a running system reports - it just
 * quietly carries nothing.
 */
static void isoch_ep_ctx(u32 *ec, u8 speed, bool in, u16 max_packet,
                         u8 interval, u8 burst) {
    ec[0] = (u32)encode_interval(speed, interval) << 16;

    /* Error count zero, which for an isochronous endpoint means "do not
     * retry" - and is the only value the specification allows here, because
     * retrying a transfer whose moment has passed is meaningless. */
    ec[1] = ((u32)(in ? EP_TYPE_ISOCH_IN : EP_TYPE_ISOCH_OUT) << 3) |
            ((u32)burst << 8) |
            ((u32)max_packet << 16);

    /* Average TRB length in the low half, and what one interval carries in the
     * high half.  The second of those is what the controller reserves against;
     * left at zero it reserves nothing and the endpoint is silent. */
    ec[4] = max_packet | (isoch_per_interval(max_packet, burst) << 16);
}

/* Configure one isochronous endpoint.
 *
 * Audio and video arrive on these, and they are unlike anything else this
 * driver carries.  A bulk or interrupt transfer is a request: it is queued,
 * the controller gets to it when it can, and it is answered.  An isochronous
 * endpoint instead owns a slot in every interval whether or not there is
 * anything to put in it - the bandwidth is reserved when the endpoint is
 * configured and it is spent regardless, because sound that arrives late is
 * not sound.
 *
 * Two consequences shape everything below.
 *
 * The controller has to be told how much will move in each interval, before
 * any of it moves, so that it can decide whether the bandwidth exists at all -
 * that is what Max ESIT Payload is, and an isochronous endpoint configured
 * with it left at zero is one the controller schedules no time for and which
 * therefore never carries anything.  It is the same field the interrupt
 * endpoints needed, and for the same reason, but here getting it wrong is not
 * a subtle loss of throughput: it is silence.
 *
 * And there is no retrying.  A transfer that misses its interval is gone; the
 * error is reported and the next interval is already being filled.  So the
 * ring is kept several transfers deep and refilled as they complete, rather
 * than one at a time on demand.
 */
static int configure_isoch(usb_device_t *dev, u8 addr, u16 max_packet,
                           u8 interval, u8 burst, void *drv,
                           const char *drv_name) {
    xhci_t *x = dev->hc;

    if (dev->ep_count >= MAX_INTERFACES) return -1;
    usb_endpoint_t *ep = &dev->ep[dev->ep_count];
    memset(ep, 0, sizeof *ep);

    bool in = (addr & USB_DIR_IN) != 0;
    u8 num = addr & 0x0F;
    ep->dci = (u8)(num * 2 + (in ? 1 : 0));
    ep->addr = addr;
    ep->max_packet = max_packet;

    if (!ring_init(&ep->ring, XFER_RING_SIZE, true)) return -1;

    ep->buf = dma_alloc(PAGE_SIZE, &ep->buf_phys);
    if (!ep->buf) { ring_free(&ep->ring); return -1; }

    memset(dev->in_ctx, 0, x->context_size * 33);
    dev->in_ctx[0] = 0;
    dev->in_ctx[1] = (1u << 0) | (1u << ep->dci);

    u32 *sc = slot_ctx(x, dev->in_ctx, true);
    u32 *dsc = slot_ctx(x, dev->dev_ctx, false);
    u32 entries = (dsc[0] >> 27) & 0x1F;
    if (ep->dci > entries) entries = ep->dci;
    sc[0] = (dsc[0] & 0x07FFFFFF) | (entries << 27);
    sc[1] = dsc[1]; sc[2] = dsc[2]; sc[3] = dsc[3];

    u32 per_interval = isoch_per_interval(max_packet, burst);

    u32 *ec = ep_ctx(x, dev->in_ctx, ep->dci, true);
    isoch_ep_ctx(ec, dev->speed, in, max_packet, interval, burst);
    ec[2] = (u32)(ep->ring.phys | 1);
    ec[3] = (u32)(ep->ring.phys >> 32);

    if (command(x, dev->in_ctx_phys, 0,
                TRB_TYPE(TRB_CONFIG_EP) | ((u32)dev->slot << 24), NULL) != CC_SUCCESS) {
        kwarn("usb", "%s: cannot configure isochronous endpoint %#x - most "
                     "often the controller has no bandwidth left for the "
                     "%u bytes per interval it asks for",
              dev->name, addr, per_interval);
        ring_free(&ep->ring);
        dma_free(ep->buf, PAGE_SIZE);
        memset(ep, 0, sizeof *ep);
        return -1;
    }

    ep->drv = drv;
    strlcpy(ep->drv_name, drv_name, sizeof ep->drv_name);
    ep->active = true;
    ep->isoch = true;
    dev->ep_count++;

    kinfo("usb", "%s: isochronous endpoint %#x, %u bytes every interval",
          dev->name, addr, per_interval);
    return dev->ep_count - 1;
}

/* Check the endpoint context against a device whose descriptors are known.
 *
 * The numbers below are read from a real headset - a HyperX, 03f0:0d84 - whose
 * configuration descriptor offers sixteen, twenty-four and thirty-two bit
 * recording and sixteen and twenty-four bit playback.  Its best settings are
 * 248-byte packets in and 186-byte packets out, every microframe.
 */
int usb_isoch_selftest(void) {
    int failures = 0;
    u32 ec[5];

    /* Recording: endpoint 0x81, 248-byte packets, bInterval 1, no burst. */
    memset(ec, 0, sizeof ec);
    isoch_ep_ctx(ec, SPEED_HIGH, true, 248, 1, 0);

    if (((ec[0] >> 16) & 0xFF) != 0) {
        kwarn("usb", "selftest: interval encoded %u, expected 0 - bInterval 1 "
                     "means every microframe, which is 2^0",
              (ec[0] >> 16) & 0xFF);
        failures++;
    }
    /* Two bits, at one and two.  Reading three of them takes in the bottom
     * bit of the endpoint type as well - and the isochronous IN type is five,
     * whose bottom bit is set, so a correct context read back as an error
     * count of four.  The context was right and the check was wrong, which is
     * the more embarrassing way round and exactly what a self-test is for. */
    if (((ec[1] >> 1) & 0x3) != 0) {
        kwarn("usb", "selftest: error count %u, expected 0 - an isochronous "
                     "endpoint may not retry", (ec[1] >> 1) & 0x3);
        failures++;
    }
    if (((ec[1] >> 3) & 0x7) != EP_TYPE_ISOCH_IN) {
        kwarn("usb", "selftest: endpoint type %u, expected %u",
              (ec[1] >> 3) & 0x7, EP_TYPE_ISOCH_IN);
        failures++;
    }
    if (((ec[1] >> 16) & 0xFFFF) != 248) {
        kwarn("usb", "selftest: max packet %u, expected 248",
              (ec[1] >> 16) & 0xFFFF);
        failures++;
    }
    if ((ec[4] >> 16) != 248) {
        kwarn("usb", "selftest: Max ESIT Payload %u, expected 248 - zero here "
                     "is the failure that leaves the endpoint silent",
              ec[4] >> 16);
        failures++;
    }

    /* Playback: endpoint 0x04, 186-byte packets, and an OUT type. */
    memset(ec, 0, sizeof ec);
    isoch_ep_ctx(ec, SPEED_HIGH, false, 186, 1, 0);
    if (((ec[1] >> 3) & 0x7) != EP_TYPE_ISOCH_OUT) {
        kwarn("usb", "selftest: OUT endpoint typed %u, expected %u",
              (ec[1] >> 3) & 0x7, EP_TYPE_ISOCH_OUT);
        failures++;
    }
    if ((ec[4] >> 16) != 186) {
        kwarn("usb", "selftest: OUT Max ESIT Payload %u, expected 186",
              ec[4] >> 16);
        failures++;
    }

    /* A burst of four packets reserves four times the bandwidth.  This is the
     * SuperSpeed case, where getting it wrong underfeeds the endpoint. */
    if (isoch_per_interval(1024, 3) != 4096) {
        kwarn("usb", "selftest: a burst of 4 x 1024 reserved %u, expected 4096",
              isoch_per_interval(1024, 3));
        failures++;
    }

    if (!failures)
        kinfo("usb", "isochronous endpoint contexts check out against a real "
                     "headset's descriptors");
    return failures;
}

/* Feed every open isochronous endpoint whatever its driver has ready.
 *
 * Called from the controller's own loop rather than from the writer, because
 * an isochronous endpoint's timing belongs to the controller: the writer says
 * what to play and this decides when each piece of it goes out.
 */
static bool queue_isoch(usb_device_t *dev, usb_endpoint_t *ep,
                        const void *data, u32 len);

static void pump_isoch(xhci_t *x) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        usb_device_t *d = &x->devices[i];
        if (!d->present) continue;

        for (int e = 0; e < d->ep_count; e++) {
            usb_endpoint_t *ep = &d->ep[e];
            if (!ep->active || !ep->isoch || !ep->drv) continue;
            if ((ep->addr & USB_DIR_IN)) continue;   /* playback only, for now */

            /* Two intervals in flight is enough to cover the gap between one
             * pass of this loop and the next without building up delay. */
            for (int ahead = 0; ahead < 2; ahead++) {
                u8 staging[REPORT_SLOT];
                u32 want = ep->max_packet;
                if (want > sizeof staging) want = sizeof staging;

                u32 got = usbaudio_next_samples(ep->drv, ep->addr, staging, want);
                if (!got) break;

                if (!queue_isoch(d, ep, staging, got)) break;
            }
        }
    }
}

/* Queue one interval's worth on an isochronous endpoint.
 *
 * "Start as soon as possible" rather than a named frame: naming one means
 * knowing what frame the controller is on and being certain the work will be
 * ready before it arrives, and being wrong means the transfer is rejected
 * rather than late.  Asking for the next available slot is what a driver
 * without a clock of its own should do.
 */
static bool queue_isoch(usb_device_t *dev, usb_endpoint_t *ep,
                        const void *data, u32 len) {
    xhci_t *x = dev->hc;
    if (len > REPORT_SLOT) return false;

    /* The slot matching this transfer's place in the ring, the same way
     * the interrupt endpoints work.  Sharing one buffer would mean a
     * transfer still waiting for its interval being overwritten by the
     * next one queued behind it. */
    u32 slot = ep->ring.index % ep->ring.size;
    u64 offset = (u64)slot * REPORT_SLOT;
    if (offset + len > PAGE_SIZE) return false;

    if (data && len) memcpy(ep->buf + offset, data, len);

    /* SIA is bit 31 of the control word: start at the next opportunity. */
    ring_push(&ep->ring, ep->buf_phys + offset, len,
              TRB_TYPE(TRB_ISOCH) | TRB_IOC | TRB_ISP | (1u << 31));
    ring_doorbell(x, dev->slot, ep->dci);
    return true;
}

/* Configure one bulk endpoint.  Almost the same as an interrupt endpoint, and
 * different in the three ways that matter: the type says bulk, the direction
 * comes from the address rather than being assumed inward, and there is no
 * polling interval because nothing arrives unless it was asked for. */
static int configure_bulk(usb_device_t *dev, u8 addr, u16 max_packet,
                          u8 burst) {
    xhci_t *x = dev->hc;

    if (dev->ep_count >= MAX_INTERFACES) return -1;
    usb_endpoint_t *ep = &dev->ep[dev->ep_count];
    memset(ep, 0, sizeof *ep);

    bool in = (addr & USB_DIR_IN) != 0;
    u8 num = addr & 0x0F;
    ep->dci = (u8)(num * 2 + (in ? 1 : 0));
    ep->addr = addr;
    ep->max_packet = max_packet ? max_packet : 64;

    if (!ring_init(&ep->ring, XFER_RING_SIZE, true)) return -1;
    /* A page per transfer.  Storage moves blocks, and a page holds eight of
     * the usual five hundred and twelve byte ones. */
    ep->buf = dma_alloc(PAGE_SIZE, &ep->buf_phys);
    if (!ep->buf) { ring_free(&ep->ring); return -1; }

    memset(dev->in_ctx, 0, x->context_size * 33);
    dev->in_ctx[0] = 0;
    dev->in_ctx[1] = (1u << 0) | (1u << ep->dci);

    u32 *sc = slot_ctx(x, dev->in_ctx, true);
    u32 *dsc = slot_ctx(x, dev->dev_ctx, false);
    u32 entries = (dsc[0] >> 27) & 0x1F;
    if (ep->dci > entries) entries = ep->dci;
    sc[0] = (dsc[0] & 0x07FFFFFF) | (entries << 27);
    sc[1] = dsc[1]; sc[2] = dsc[2]; sc[3] = dsc[3];

    u32 *ec = ep_ctx(x, dev->in_ctx, ep->dci, true);
    ec[0] = 0;                                   /* no interval for bulk */
    ec[1] = (3u << 1) |
            ((u32)(in ? EP_TYPE_BULK_IN : EP_TYPE_BULK_OUT) << 3) |
            ((u32)burst << 8) |                  /* max burst size */
            ((u32)ep->max_packet << 16);
    ec[2] = (u32)(ep->ring.phys | 1);
    ec[3] = (u32)(ep->ring.phys >> 32);
    ec[4] = ep->max_packet;

    if (command(x, dev->in_ctx_phys, 0,
                TRB_TYPE(TRB_CONFIG_EP) | ((u32)dev->slot << 24), NULL) != CC_SUCCESS) {
        kwarn("usb", "%s: cannot configure bulk endpoint %#x", dev->name, addr);
        ring_free(&ep->ring);
        dma_free(ep->buf, PAGE_SIZE);
        memset(ep, 0, sizeof *ep);
        return -1;
    }

    strlcpy(ep->drv_name, "bulk", sizeof ep->drv_name);
    ep->polled = false;
    ep->active = true;
    return dev->ep_count++;
}

bool usb_bulk_open(usb_device_t *dev, const usb_interface_t *ifc) {
    if (!dev || !ifc->bulk_in || !ifc->bulk_out) return false;

    dev->bulk_in_ep = configure_bulk(dev, ifc->bulk_in, ifc->bulk_in_max,
                                     ifc->bulk_in_burst);
    if (dev->bulk_in_ep < 0) return false;

    dev->bulk_out_ep = configure_bulk(dev, ifc->bulk_out, ifc->bulk_out_max,
                                      ifc->bulk_out_burst);
    if (dev->bulk_out_ep < 0) return false;

    return true;
}

int usb_bulk(usb_device_t *dev, bool in, void *data, u32 length, u32 timeout_ms) {
    if (!dev || !dev->present) return -1;
    if (length > PAGE_SIZE) return -1;

    xhci_t *x = dev->hc;
    int index = in ? dev->bulk_in_ep : dev->bulk_out_ep;
    if (index < 0 || index >= dev->ep_count) return -1;

    usb_endpoint_t *ep = &dev->ep[index];
    if (!ep->active) return -1;

    if (!in && length && data) memcpy(ep->buf, data, length);
    if (in && length) memset(ep->buf, 0, length);

    u64 token = ring_push(&ep->ring, ep->buf_phys, length,
                          TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ring_doorbell(x, dev->slot, ep->dci);

    trb_t ev;
    int cc = pump(x, token, &ev, timeout_ms ? timeout_ms : 5000);
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) return -1;

    /* What actually moved: the event reports how much of the request was left
     * over, which for a short read is the only way to know the length. */
    u32 left = ev.status & 0x00FFFFFFu;
    u32 moved = length > left ? length - left : 0;

    if (in && moved && data) memcpy(data, ep->buf, moved);
    return (int)moved;
}

/* Offer an interface to every class driver in turn.  One place decides who
 * gets what, so adding a driver is adding a line here rather than editing the
 * walk below. */
static bool offer(usb_device_t *dev, const usb_interface_t *ifc,
                  const u8 *cfg, int cfg_len) {
    if (ifc->dev_class == USB_CLASS_HID && ifc->ep_addr) {
        void *drv = usbhid_probe(dev, ifc);
        if (drv && configure_endpoint(dev, ifc, drv, "hid")) return true;
        return false;
    }

    if (ifc->dev_class == USB_CLASS_STORAGE && ifc->bulk_in && ifc->bulk_out)
        return usbmsc_probe(dev, ifc);

    /* An audio device.  It is given the whole configuration rather than the
     * one interface, because what it needs - the alternate settings and their
     * formats - is exactly what the walk below skips. */
    if (ifc->dev_class == 0x01) {
        void *drv = usbaudio_probe(dev, ifc, cfg, cfg_len);
        if (!drv) return false;

        /* And open the endpoints it chose, if it chose any.  This is only
         * reached for the control interface, because the streaming interfaces
         * carry their endpoints on alternate settings the walk above skips -
         * so the driver names them and they are configured from here. */
        u8 addr = 0, interval = 0, burst = 0, ifnum = 0, altnum = 0;
        u16 packet = 0;
        while (usbaudio_next_endpoint(drv, &addr, &packet, &interval, &burst,
                                      &ifnum, &altnum)) {
            /* Bandwidth first, then the device.  The controller has to have
             * accepted the reservation before the device is told to start
             * producing, or the first packets arrive with nowhere to go. */
            int index = configure_isoch(dev, addr, packet, interval, burst,
                                        drv, "audio");
            bool ok = index >= 0;

            if (ok && usb_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD |
                                       USB_RECIP_INTERFACE,
                                  USB_REQ_SET_INTERFACE, altnum, ifnum,
                                  NULL, 0) < 0) {
                kwarn("usb", "%s: endpoint %#x is reserved but the device "
                             "refused setting %u on interface %u, so nothing "
                             "will come out of it",
                      dev->name, addr, altnum, ifnum);
                ok = false;
            }
            usbaudio_endpoint_open(drv, addr, ok);
        }
        return true;
    }

    /* A Bluetooth adapter, which reports events on an interrupt endpoint the
     * same way an input device does - so it is configured the same way, and
     * only what happens to the bytes differs. */
    if (ifc->dev_class == 0xE0 && ifc->subclass == 0x01 &&
        ifc->protocol == 0x01 && ifc->ep_addr) {
        void *drv = btusb_probe(dev, ifc);
        if (drv && configure_endpoint(dev, ifc, drv, "bluetooth")) {
            /* And the bulk pair, which is where the data of a connection
             * moves.  Commands and events have their own paths - control and
             * interrupt - so an adapter without these still answers everything
             * and carries nothing, which is what it did before. */
            if (!usb_bulk_open(dev, ifc))
                kwarn("usb", "%s: no bulk pair, so links can be opened but "
                             "will carry nothing", dev->name);

            btusb_start(drv);
            return true;
        }
        return false;
    }

    return false;
}

/* Walk a configuration descriptor, offering each interface to the class
 * driver and configuring the endpoint of any it claims. */
static void bind_interfaces(usb_device_t *dev, const u8 *cfg, int total) {

    const usb_interface_t empty = {0};
    usb_interface_t ifc = empty;
    bool have = false;
    int  claimed = 0;
    bool last_was_bulk_in = false, last_was_bulk_out = false, last_was_int = false;

    for (int off = 0; off + 2 <= total; ) {
        u8 len = cfg[off];
        u8 type = cfg[off + 1];
        if (len < 2 || off + len > total) break;

        if (type == USB_DT_INTERFACE && len >= (int)sizeof(usb_interface_desc_t)) {
            /* The previous interface is complete: offer it now, if there was
             * one.  Without the guard the first interface descriptor offers an
             * empty structure, and an alternate setting offers a half-built
             * one. */
            if (have && offer(dev, &ifc, cfg, total)) claimed++;
            const usb_interface_desc_t *d = (const void *)(cfg + off);
            ifc = empty;
            ifc.number = d->interface_num;
            ifc.dev_class = d->dev_class;
            ifc.subclass = d->subclass;
            ifc.protocol = d->protocol;
            /* Alternate settings other than 0 are not used by boot devices. */
            have = (d->alternate == 0);
        } else if (type == USB_DT_HID && have && len >= 9) {
            /* The HID descriptor is what says how long the report descriptor
             * is; without it the report format cannot be read at all. */
            if (cfg[off + 6] == USB_DT_REPORT)
                ifc.report_desc_len = (u16)(cfg[off + 7] | (cfg[off + 8] << 8));
        } else if (type == USB_DT_SS_EP_COMP && have && len >= 6) {
            /* Belongs to the endpoint just described.  bMaxBurst is stored
             * one less than the real number, the same way max packet sizes
             * are, so a device that sends one packet at a time reports zero. */
            u8 burst = cfg[off + 2];
            if (ifc.bulk_in && !ifc.bulk_in_burst && last_was_bulk_in)
                ifc.bulk_in_burst = burst;
            else if (ifc.bulk_out && !ifc.bulk_out_burst && last_was_bulk_out)
                ifc.bulk_out_burst = burst;
            else if (ifc.ep_addr && last_was_int)
                ifc.int_burst = burst;
        } else if (type == USB_DT_ENDPOINT && have &&
                   len >= (int)sizeof(usb_endpoint_desc_t)) {
            const usb_endpoint_desc_t *e = (const void *)(cfg + off);
            u8 kind = e->attributes & USB_EP_XFER_MASK;
            bool in = (e->address & USB_DIR_IN) != 0;

            /* A companion descriptor describes whichever endpoint came
             * immediately before it, so which one that was has to be
             * remembered across the two. */
            last_was_bulk_in = last_was_bulk_out = last_was_int = false;

            if (kind == USB_EP_XFER_INT && in && !ifc.ep_addr) {
                ifc.ep_addr = e->address;
                ifc.ep_max_packet = e->max_packet & 0x7FF;
                ifc.ep_interval = e->interval;
                last_was_int = true;
            } else if (kind == USB_EP_XFER_BULK && in && !ifc.bulk_in) {
                ifc.bulk_in = e->address;
                ifc.bulk_in_max = e->max_packet & 0x7FF;
                last_was_bulk_in = true;
            } else if (kind == USB_EP_XFER_BULK && !in && !ifc.bulk_out) {
                ifc.bulk_out = e->address;
                ifc.bulk_out_max = e->max_packet & 0x7FF;
                last_was_bulk_out = true;
            }
        }
        off += len;
    }

    if (have) { if (offer(dev, &ifc, cfg, total)) claimed++; }

    /* A hub is driven, just not through an interface: the hub driver talks to
     * it over endpoint zero, so nothing claims an interface and the old
     * message called every hub undriven while it was working perfectly. */
    if (!claimed && !dev->is_hub)
        /* The numbers as well as the name.  A name says what somebody called
         * the device; these four say what a driver would have to be written
         * against, and without them a log of an undriven device does not
         * answer the only question worth asking about it. */
        kinfo("usb", "%s: no driver for this device (%04x:%04x, last interface "
                     "class %02x subclass %02x protocol %02x)",
              dev->name, dev->vendor, dev->product,
              ifc.dev_class, ifc.subclass, ifc.protocol);
}

/* Bring up a device, wherever it is in the tree.
 *
 * `parent` is the hub it is plugged into, or NULL for a device in one of the
 * controller's own root ports.  Everything that differs between those two
 * cases is worked out here rather than duplicated: the route the controller
 * uses to reach it, which root port the branch hangs off, and whether some hub
 * along the way has to translate for it.
 */
static bool enumerate_at(xhci_t *x, usb_device_t *parent, u32 port, u8 speed) {
    /* Find a free device slot of our own first, so a failure part-way through
     * never leaves the controller holding a slot we cannot address. */
    usb_device_t *dev = NULL;
    for (int i = 0; i < MAX_DEVICES; i++)
        if (!x->devices[i].present) { dev = &x->devices[i]; break; }
    if (!dev) { kwarn("usb", "no free device slot for port %u", port); return false; }

    memset(dev, 0, sizeof *dev);
    /* Before anything else touches it: every operation on a device goes
     * through its controller's rings, and the clear above wiped the field that
     * says which controller that is. */
    dev->hc = x;
    dev->port = (u8)port;
    dev->speed = speed;
    dev->parent = parent;

    if (!parent) {
        /* A root port: no route, and the port is the root port. */
        dev->route = 0;
        dev->depth = 0;
        dev->root_port = (u8)port;
    } else {
        /* One more tier down.  Four bits per tier, and a port number above
         * fifteen is clamped because that is all the field holds - hubs with
         * more ports than that are internally several hubs, so the clamp is
         * not reachable in practice, but a silent wrap would be. */
        u32 nibble = port > 15 ? 15 : port;
        dev->route = parent->route | (nibble << (4 * parent->depth));
        dev->depth = (u8)(parent->depth + 1);
        dev->root_port = parent->root_port;

        /* Who translates for it, if anybody.  A full or low speed device
         * below a high-speed hub is spoken to through that hub's transaction
         * translator; if the parent is itself below one, the same translator
         * serves, so the answer is inherited rather than recomputed. */
        if (speed == SPEED_FULL || speed == SPEED_LOW) {
            if (parent->tt_slot) {
                dev->tt_slot = parent->tt_slot;
                dev->tt_port = parent->tt_port;
            } else if (parent->speed == SPEED_HIGH) {
                dev->tt_slot = parent->slot;
                dev->tt_port = (u8)port;
            }
        }
    }

    trb_t ev;
    int cc = command(x, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &ev);
    if (cc != CC_SUCCESS) {
        kwarn("usb", "port %u: the controller would not give out a slot (code %d)",
              port, cc);
        return false;
    }
    dev->slot = (u8)(ev.control >> 24);
    if (!dev->slot || dev->slot > x->max_slots) {
        kwarn("usb", "port %u: nonsense slot id %u", port, dev->slot);
        return false;
    }

    dev->present = true;
    x->slot_to_device[dev->slot] = (u8)((dev - x->devices) + 1);

    /* One page holds any context array we need; 32 entries at 64 bytes is
     * 2 KiB, so this is never tight. */
    dev->dev_ctx = dma_alloc(x->context_size * 32, &dev->dev_ctx_phys);
    dev->in_ctx  = dma_alloc(x->context_size * 33, &dev->in_ctx_phys);
    dev->xfer_buf = dma_alloc(PAGE_SIZE, &dev->xfer_buf_phys);
    if (!dev->dev_ctx || !dev->in_ctx || !dev->xfer_buf ||
        !ring_init(&dev->ep0, XFER_RING_SIZE, true)) {
        kerr("usb", "out of memory bringing up port %u", port);
        goto fail;
    }

    x->dcbaa[dev->slot] = dev->dev_ctx_phys;
    dev->max_packet0 = default_max_packet(speed);

    /* Address the device: one input context describing the slot and endpoint
     * zero is all the controller needs to start talking to it. */
    memset(dev->in_ctx, 0, x->context_size * 33);
    dev->in_ctx[1] = (1u << 0) | (1u << 1);          /* add slot and EP0 */

    u32 *sc = slot_ctx(x, dev->in_ctx, true);
    sc[0] = ((u32)1 << 27) | ((u32)speed << 20) | (dev->route & 0xFFFFF);
    sc[1] = (u32)dev->root_port << 16;
    sc[2] = (u32)dev->tt_slot | ((u32)dev->tt_port << 8);
    sc[3] = 0;

    u32 *ec = ep_ctx(x, dev->in_ctx, 1, true);
    ec[0] = 0;
    ec[1] = (3u << 1) | ((u32)EP_TYPE_CONTROL << 3) | ((u32)dev->max_packet0 << 16);
    ec[2] = (u32)(dev->ep0.phys | 1);
    ec[3] = (u32)(dev->ep0.phys >> 32);
    ec[4] = 8;

    cc = command(x, dev->in_ctx_phys, 0,
                 TRB_TYPE(TRB_ADDRESS_DEV) | ((u32)dev->slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        kwarn("usb", "port %u: address device failed (code %d)", port, cc);
        goto fail;
    }

    /* Full speed devices only reveal their real endpoint-zero packet size in
     * the descriptor, and the first eight bytes are all that can be read until
     * the controller has been told about it. */
    usb_device_desc_t dd;
    memset(&dd, 0, sizeof dd);
    if (usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, &dd, 8) < 0) {
        kwarn("usb", "port %u: the device will not describe itself", port);
        goto fail;
    }

    if (dd.max_packet0 && dd.max_packet0 != dev->max_packet0) {
        u16 want = dd.max_packet0;
        if (speed >= SPEED_SUPER) want = (u16)(1u << dd.max_packet0);
        dev->max_packet0 = want;

        memset(dev->in_ctx, 0, x->context_size * 33);
        dev->in_ctx[1] = (1u << 1);                  /* endpoint zero only */
        u32 *e0 = ep_ctx(x, dev->in_ctx, 1, true);
        e0[1] = (3u << 1) | ((u32)EP_TYPE_CONTROL << 3) | ((u32)want << 16);
        e0[2] = (u32)(dev->ep0.phys | dev->ep0.cycle);
        e0[3] = (u32)(dev->ep0.phys >> 32);
        command(x, dev->in_ctx_phys, 0,
                TRB_TYPE(TRB_EVAL_CTX) | ((u32)dev->slot << 24), NULL);
    }

    if (usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, &dd,
                    sizeof dd) < 0) {
        kwarn("usb", "port %u: short device descriptor", port);
        goto fail;
    }
    dev->vendor = dd.vendor;
    dev->product = dd.product;

    char vendor[32], product[32];
    read_string(dev, dd.manufacturer_str, vendor, sizeof vendor);
    read_string(dev, dd.product_str, product, sizeof product);
    if (product[0] && vendor[0]) snprintf(dev->name, sizeof dev->name, "%s %s", vendor, product);
    else if (product[0])         strlcpy(dev->name, product, sizeof dev->name);
    else                         snprintf(dev->name, sizeof dev->name, "%04x:%04x",
                                          dd.vendor, dd.product);

    if (parent)
        kinfo("usb", "%s port %u: %s (%s, %04x:%04x)", parent->name, port,
              dev->name, usb_speed_name(speed), dd.vendor, dd.product);
    else
        kinfo("usb", "port %u: %s (%s, %04x:%04x)", port, dev->name,
              usb_speed_name(speed), dd.vendor, dd.product);

    /* A hub is worth knowing about before its interfaces are offered around,
     * because nothing claims a hub - it is the driver's own business. */
    dev->is_hub = (dd.dev_class == USB_CLASS_HUB);

    /* The configuration descriptor arrives in two parts: the header says how
     * much there is, then the whole thing comes back in one go. */
    usb_config_desc_t cd;
    if (usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, &cd,
                    sizeof cd) < 0) {
        kwarn("usb", "%s: no configuration descriptor", dev->name);
        goto fail;
    }

    u16 total = cd.total_length;
    if (total < sizeof cd || total > PAGE_SIZE) total = sizeof cd;

    u8 *cfg = kmalloc(total);
    if (!cfg) goto fail;
    if (usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, cfg, total) < 0) {
        kfree(cfg);
        goto fail;
    }

    if (usb_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_SET_CONFIGURATION, cd.config_value, 0, NULL, 0) < 0) {
        kwarn("usb", "%s: will not accept its configuration", dev->name);
        kfree(cfg);
        goto fail;
    }

    bind_interfaces(dev, cfg, total);
    kfree(cfg);
    return true;

fail:
    if (dev->slot)
        command(x, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((u32)dev->slot << 24), NULL);
    free_device(dev);
    return false;
}

/* --------------------------------------------------------------- events in */

static void handle_transfer(xhci_t *x, const trb_t *e) {
    u32 slot = (e->control >> 24) & 0xFF;
    u32 dci  = (e->control >> 16) & 0x1F;
    u32 code = (e->status >> 24) & 0xFF;
    u32 residual = e->status & 0x00FFFFFF;

    if (!slot || !x->slot_to_device[slot]) return;
    usb_device_t *dev = &x->devices[x->slot_to_device[slot] - 1];
    if (!dev->present) return;

    usb_endpoint_t *ep = NULL;
    for (int i = 0; i < dev->ep_count; i++)
        if (dev->ep[i].active && dev->ep[i].dci == dci) { ep = &dev->ep[i]; break; }
    if (!ep) return;

    /* A bulk transfer that reaches here is one nobody was waiting on - it
     * completed after its caller gave up.  There is no driver to hand it to
     * and nothing to queue in its place, so it is dropped.  Passing it to the
     * HID layer would mean calling a driver that was never attached to this
     * endpoint, with a length computed for a different kind of transfer. */
    if (!ep->polled) return;

    ep->completions++;

    if (code == CC_SUCCESS || code == CC_SHORT_PACKET) {
        int len = (int)ep->max_packet - (int)residual;
        if (len > 0) {
            /* Which buffer this transfer filled.
             *
             * A transfer event's parameter is the address of the TRB that
             * generated it, not the address of the data that TRB moved - the
             * same convention usb_bulk() relies on when it matches an event
             * against the token ring_push() handed back.  Subtracting the
             * buffer base from it therefore yielded a number with no meaning,
             * which failed the bounds check below, and every report a device
             * ever sent was dropped here without a word.  The transfer had
             * completed, the endpoint was healthy, and the counters agreed
             * that data had moved: only the last step, handing it to the
             * driver, quietly did not happen.
             *
             * Each queued transfer uses the buffer slot matching its position
             * in the ring, so the TRB's index recovers the buffer directly. */
            u64 slot_index = (e->param - ep->ring.phys) / sizeof(trb_t);
            u64 offset = slot_index * REPORT_SLOT;

            if (slot_index < ep->ring.size && offset + (u64)len <= PAGE_SIZE) {
                /* Which driver claimed this endpoint decides what the bytes
                 * mean.  They arrive the same way for an input device and a
                 * Bluetooth adapter - an interrupt endpoint, a few bytes at a
                 * time - and handing one to the other's parser produces a
                 * plausible-looking result from data that means nothing of the
                 * kind. */
                if (ep->isoch) {
                    /* Audio, which is not parsed here: an isochronous
                     * completion says a slot was filled or missed, and what to
                     * do about it belongs to whatever is producing the sound.
                     * It must not fall through to a report parser - the bytes
                     * are samples, and any parser would read them as a
                     * device saying something. */
                } else if (!strcmp(ep->drv_name, "bluetooth")) {
                    btusb_event(ep->drv, ep->buf + offset, len);
                } else if (!strcmp(ep->drv_name, "hid")) {
                    usbhid_report(ep->drv, ep->buf + offset, len);
                } else {
                    kwarn("usb", "%s: endpoint %#x completed a transfer but "
                                 "'%s' has nowhere to deliver it",
                          dev->name, ep->addr, ep->drv_name);
                }
            }
            else
                kwarn("usb", "%s: endpoint %#x completed a transfer this "
                             "driver cannot place (trb %llu)",
                      dev->name, ep->addr, (unsigned long long)slot_index);
        }
    } else if (code_halts_endpoint(code)) {
        /* The endpoint is Halted and will ignore its doorbell until it has
         * been reset.  Both commands are needed: the first clears the halt,
         * the second says where to carry on from.  Losing a device for the
         * rest of the session over one dropped packet is not acceptable when
         * recovery costs two commands. */
        ep->errors++;
        if (ep->errors <= 3 || (ep->errors % 64) == 0)
            kwarn("usb", "%s: endpoint %#x halted (code %u), resetting it "
                         "(%u so far)", dev->name, ep->addr, code, ep->errors);

        /* Do NOT reset the endpoint from inside the drain: RESET_EP/SET_DEQ go
         * through command()->pump(), which re-enters this very drain on the same
         * stack.  Under a flood of transaction-error halts from a high-rate
         * device (a 1000 Hz mouse or 2.4 GHz dongle), that recursion deepens with
         * every queued halt event until the kernel stack overflows and the
         * machine hard-freezes.  Mark it and let the top-level loop recover after
         * pump() returns; skip the re-arm here (the reset re-arms it). */
        ep->needs_reset = true;
        return;
    }

    queue_report(dev, ep);
}

/* Recover every endpoint the drain marked halted - RESET_EP then SET_DEQ, then
 * re-arm.  Run ONLY from the top-level controller loop, never from inside a
 * drain, so the command()->pump() these issue is at most one level deep and can
 * never recurse into itself. */
static void process_pending_resets(xhci_t *x) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        usb_device_t *d = &x->devices[i];
        if (!d->present) continue;
        for (int e = 0; e < d->ep_count; e++) {
            usb_endpoint_t *ep = &d->ep[e];
            if (!ep->active || !ep->needs_reset) continue;
            ep->needs_reset = false;
            u8 dci = ep->dci;
            command(x, 0, 0, TRB_TYPE(TRB_RESET_EP) | ((u32)d->slot << 24) | (dci << 16), NULL);
            u64 dq = ep->ring.phys + (u64)ep->ring.index * sizeof(trb_t);
            command(x, dq | ep->ring.cycle, 0,
                    TRB_TYPE(TRB_SET_DEQ) | ((u32)d->slot << 24) | (dci << 16), NULL);
            queue_report(d, ep);
        }
    }
}

/* A device appeared or vanished on a root hub port. */
static void handle_port_change(xhci_t *x, const trb_t *e) {
    u32 port = (u32)((e->param >> 24) & 0xFF);
    if (!port || port > x->max_ports) return;

    u32 sc = portsc(x, port);
    portsc_write(x, port, portsc_keep(sc) | (sc & PORTSC_RW1C));   /* ack changes */

    if (sc & PORTSC_CSC) {
        if (sc & PORTSC_CCS) {
            kinfo("usb", "port %u: device connected", port);
            /* Leave the reset and enumeration to the scan pass, which runs
             * with nothing else in flight. */
        } else {
            for (int i = 0; i < MAX_DEVICES; i++) {
                usb_device_t *d = &x->devices[i];
                if (!d->present || d->port != port) continue;
                kinfo("usb", "port %u: %s disconnected", port, d->name);
                u8 slot = d->slot;
                free_device(d);
                if (slot) command(x, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) |
                                        ((u32)slot << 24), NULL);
            }
        }
    }
}

/* -------------------------------------------------------------- port scan */

/* Is one of the controller's own ports already occupied?
 *
 * The parent check is the whole point and was missing.  A device records the
 * port it is plugged into, and for a device behind a hub that is a port on the
 * HUB - so a keyboard in port 4 of a monitor's hub made the controller's own
 * port 4 look occupied, and whatever was really in it was never looked at.
 * The two numberings have nothing to do with each other and comparing them
 * only ever gives a wrong answer by coincidence. */
static bool port_has_device(xhci_t *x, u32 port) {
    for (int i = 0; i < MAX_DEVICES; i++)
        if (x->devices[i].present && !x->devices[i].parent &&
            x->devices[i].port == port)
            return true;
    return false;
}

/* Reset a port and wait for the controller to enable it.  USB 3 ports train
 * their own link and come up enabled; USB 2 ports need to be told. */
static bool reset_port(xhci_t *x, u32 port) {
    u32 sc = portsc(x, port);
    if (sc & PORTSC_PED) return true;

    portsc_write(x, port, portsc_keep(sc) | PORTSC_PR);

    for (int i = 0; i < 500; i++) {
        sched_sleep_ms(1);
        sc = portsc(x, port);
        if (sc & PORTSC_PRC) {
            portsc_write(x, port, portsc_keep(sc) | PORTSC_PRC);
            break;
        }
    }

    /* The port needs a moment after reset before it will answer. */
    sched_sleep_ms(20);
    sc = portsc(x, port);
    return (sc & PORTSC_PED) != 0;
}

/* =============================================================== hubs
 *
 * A monitor with USB sockets is a hub.  So is the front of most cases, so is
 * every dock, and so is the thing on the desk with four ports on it.  Until
 * now this driver saw only what was plugged directly into the machine, which
 * meant a keyboard in any of those did not exist.
 *
 * A hub is an ordinary device of class 9, and everything about its ports is
 * reached with class requests over endpoint zero - no different in kind from
 * asking any device for a descriptor.  What makes hubs work is doing the
 * sequence in the right order: a port has no power until it is told to have
 * some, reports nothing until it has power, and cannot be talked to until it
 * has been reset.
 */

/* Ask a hub about one of its ports.  Returns false if the hub did not answer;
 * the status and change words are the two halves of what comes back. */
static bool hub_port_status(usb_device_t *hub, u32 port, u16 *status, u16 *change) {
    u8 buf[4] = {0, 0, 0, 0};
    if (usb_control(hub, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
                    USB_REQ_GET_STATUS, 0, (u16)port, buf, 4) < 0)
        return false;
    if (status) *status = (u16)(buf[0] | (buf[1] << 8));
    if (change) *change = (u16)(buf[2] | (buf[3] << 8));
    return true;
}

static bool hub_set_port(usb_device_t *hub, u32 port, u16 feature) {
    return usb_control(hub, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
                       USB_REQ_SET_FEATURE, feature, (u16)port, NULL, 0) >= 0;
}

static bool hub_clear_port(usb_device_t *hub, u32 port, u16 feature) {
    return usb_control(hub, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
                       USB_REQ_CLEAR_FEATURE, feature, (u16)port, NULL, 0) >= 0;
}

/* Read how many ports a hub has, and switch them all on.
 *
 * The controller also has to be told that this device is a hub and how many
 * ports it has, because it keeps its own idea of the topology and will refuse
 * to address anything below a device it thinks is not a hub.
 */
static bool hub_configure(usb_device_t *hub) {
    xhci_t *x = hub->hc;

    u8 desc[16];
    memset(desc, 0, sizeof desc);
    u16 type = (hub->speed >= SPEED_SUPER) ? HUB_DT_SS_HUB : HUB_DT_HUB;

    if (usb_control(hub, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR, (u16)(type << 8), 0, desc,
                    sizeof desc) < 0) {
        kwarn("usb", "%s: will not describe itself as a hub", hub->name);
        return false;
    }

    hub->hub_ports = desc[2];
    if (!hub->hub_ports || hub->hub_ports > 15) {
        kwarn("usb", "%s: reports %u ports, which cannot be right",
              hub->name, desc[2]);
        return false;
    }

    /* Tell the controller.  Bit 26 of the first word says "this is a hub", and
     * the port count goes in the top byte of the second; without both, every
     * attempt to address a device below it is refused. */
    memset(hub->in_ctx, 0, x->context_size * 33);
    hub->in_ctx[1] = (1u << 0);                    /* the slot context only */
    u32 *sc = slot_ctx(x, hub->in_ctx, true);
    u32 *cur = slot_ctx(x, hub->dev_ctx, false);
    sc[0] = cur[0] | (1u << 26);
    sc[1] = (cur[1] & 0x00FFFFFFu) | ((u32)hub->hub_ports << 24);
    sc[2] = cur[2];
    sc[3] = cur[3];

    if (command(x, hub->in_ctx_phys, 0,
                TRB_TYPE(TRB_EVAL_CTX) | ((u32)hub->slot << 24), NULL) != CC_SUCCESS)
        kwarn("usb", "%s: the controller would not accept it as a hub",
              hub->name);

    /* Power.  A bus-powered hub brings its ports up one at a time and the
     * specification allows it to take a hundred milliseconds each; asking for
     * them all and then waiting once is the same thing and much faster. */
    for (u32 p = 1; p <= hub->hub_ports; p++) hub_set_port(hub, p, HUB_PORT_POWER);
    hub->hub_powered = hub->hub_ports;

    kinfo("usb", "%s: a hub with %u port(s), powered", hub->name, hub->hub_ports);
    return true;
}

/* What speed the thing on this port is, from the status word. */
static u8 hub_port_speed(usb_device_t *hub, u16 status) {
    if (hub->speed >= SPEED_SUPER) return SPEED_SUPER;
    if (status & HUB_STAT_LOW_SPEED) return SPEED_LOW;
    if (status & HUB_STAT_HIGH_SPEED) return SPEED_HIGH;
    return SPEED_FULL;
}

/* Is something already brought up on this port of this hub? */
static bool hub_port_taken(xhci_t *x, usb_device_t *hub, u32 port) {
    for (int i = 0; i < MAX_DEVICES; i++)
        if (x->devices[i].present && x->devices[i].parent == hub &&
            x->devices[i].port == port)
            return true;
    return false;
}

/* Look at every port of one hub and bring up anything new. */
static void hub_scan(usb_device_t *hub) {
    xhci_t *x = hub->hc;

    if (hub->depth >= MAX_HUB_DEPTH) return;   /* the route string is full */

    for (u32 p = 1; p <= hub->hub_ports; p++) {
        u16 status = 0, change = 0;
        if (!hub_port_status(hub, p, &status, &change)) continue;

        /* Acknowledge whatever changed, so the hub stops reporting it. */
        if (change & 0x0001) hub_clear_port(hub, p, HUB_C_PORT_CONNECTION);
        if (change & 0x0002) hub_clear_port(hub, p, HUB_C_PORT_ENABLE);
        if (change & 0x0010) hub_clear_port(hub, p, HUB_C_PORT_RESET);

        if (!(status & HUB_STAT_CONNECTION)) { hub->port_fails[p & 15] = 0; continue; }
        if (hub_port_taken(x, hub, p)) { hub->port_fails[p & 15] = 0; continue; }
        if (hub->port_fails[p & 15] >= 3) continue;      /* given up on */

        /* Reset it.  A device is not addressable until its port has been
         * reset, and the reset is what settles what speed it runs at. */
        if (!hub_set_port(hub, p, HUB_PORT_RESET)) continue;

        bool enabled = false;
        for (int wait = 0; wait < 50; wait++) {
            sched_sleep_ms(10);
            if (!hub_port_status(hub, p, &status, &change)) break;
            if (status & HUB_STAT_RESET) continue;     /* still resetting */
            enabled = (status & HUB_STAT_ENABLE) != 0;
            break;
        }
        hub_clear_port(hub, p, HUB_C_PORT_RESET);

        if (!enabled) {
            if (++hub->port_fails[p & 15] == 3)
                kwarn("usb", "%s port %u: will not enable; leaving it alone",
                      hub->name, p);
            continue;
        }

        /* The specification gives a device ten milliseconds to be ready after
         * its reset finishes, and a good many need every one of them. */
        sched_sleep_ms(10);

        if (!enumerate_at(x, hub, p, hub_port_speed(hub, status)))
            hub->port_fails[p & 15]++;
    }
}

/* Every hub on this controller, in the order they were found - which is
 * parents before children, so a hub plugged into a hub comes up in one pass
 * rather than needing another. */
static void scan_hubs(xhci_t *x) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        usb_device_t *d = &x->devices[i];
        if (!d->present || !d->is_hub) continue;
        if (!d->hub_ports && !hub_configure(d)) { d->is_hub = false; continue; }
        hub_scan(d);
    }
}

static void scan_ports(xhci_t *x) {
    /* How many times each port has been tried and failed.  Without this, a
     * port holding something that will not enumerate is reset once a second
     * forever - which keeps that device permanently in reset, and spends the
     * thread's time doing it instead of servicing the devices that do work. */
    static u8 failures[MAX_CONTROLLERS][256];
    int which = (int)(x - controllers);

    /* Said once per port, the first time it is looked at with something in it.
     *
     * A device that is present and never enumerated is invisible in a log that
     * only reports success: the port is scanned, nothing comes of it, and the
     * next line is about something else entirely.  On a machine where the boot
     * device is one of those, this is the difference between knowing which
     * port it is on and how far it got, and knowing nothing at all. */
    static bool announced[MAX_CONTROLLERS][256];

    for (u32 port = 1; port <= x->max_ports; port++) {
        u32 sc = portsc(x, port);
        if (!(sc & PORTSC_CCS)) { failures[which][port & 0xFF] = 0; continue; }

        if (!announced[which][port & 0xFF]) {
            announced[which][port & 0xFF] = true;
            kinfo("usb", "port %u: something is connected - status %08x, "
                         "speed %u, %s, %s", port, sc,
                  (sc >> PORTSC_SPEED_SHIFT) & 0x0F,
                  x->port_is_usb3[port] ? "a SuperSpeed port" : "a USB 2 port",
                  (sc & PORTSC_PED) ? "enabled" : "not enabled");
        }

        if (port_has_device(x, port)) { failures[which][port & 0xFF] = 0; continue; }
        if (failures[which][port & 0xFF] >= 3) continue;    /* given up on */

        if (!reset_port(x, port)) {
            if (++failures[which][port & 0xFF] == 3)
                kwarn("usb", "port %u: will not enable after three resets; "
                             "leaving it alone", port);
            continue;
        }

        sc = portsc(x, port);
        u8 speed = (u8)((sc >> PORTSC_SPEED_SHIFT) & 0x0F);
        if (!speed) speed = x->port_is_usb3[port] ? SPEED_SUPER : SPEED_HIGH;

        if (!enumerate_at(x, NULL, port, speed)) {
            failures[which][port & 0xFF]++;
            kwarn("usb", "port %u: the device there did not come up (attempt %u, "
                         "speed %u)", port, failures[which][port & 0xFF], speed);
        }
    }
}

/* --------------------------------------------------------------- bring-up */

/* The firmware may still own the controller through its SMI-based legacy
 * keyboard emulation.  Taking ownership stops it fighting us for the device. */
static void bios_handoff(xhci_t *x) {
    u32 hcc = rd32(x->cap_regs, CAP_HCCPARAMS1);
    u32 off = (hcc >> 16) & 0xFFFF;
    if (!off) return;

    volatile u32 *ext = (volatile u32 *)(x->cap_regs + (size_t)off * 4);
    for (int guard = 0; guard < 64; guard++) {
        u32 v = ext[0];
        u8 id = (u8)(v & 0xFF);
        u8 next = (u8)((v >> 8) & 0xFF);

        if (id == 1) {                                  /* USB legacy support */
            if (v & (1u << 16)) {                       /* the firmware owns it */
                ext[0] = v | (1u << 24);                /* claim it for the OS */
                for (int i = 0; i < 500; i++) {
                    if (!(ext[0] & (1u << 16))) break;
                    timer_mdelay(1);
                }
                if (ext[0] & (1u << 16))
                    kwarn("usb", "the firmware would not release the controller");
                else
                    kinfo("usb", "took the controller over from the firmware");
            }
            /* Silence every legacy SMI and clear the sticky status bits. */
            u32 ctl = ext[1];
            ctl &= ~0x0000FFFFu;
            ctl |= 0xE0000000u;
            ext[1] = ctl;
        } else if (id == 2) {                           /* supported protocol */
            u8 major = (u8)((ext[0] >> 24) & 0xFF);
            u8 first = (u8)(ext[2] & 0xFF);
            u8 count = (u8)((ext[2] >> 8) & 0xFF);
            for (u32 p = first; p < (u32)first + count && p < ARRAY_LEN(x->port_is_usb3); p++)
                x->port_is_usb3[p] = (major >= 3);
        }

        if (!next) break;
        ext += next;
    }
}

static bool reset_controller(xhci_t *x) {
    /* Stop it first: resetting a running controller is undefined. */
    u32 cmd = rd32(x->op_regs, OP_USBCMD);
    wr32(x->op_regs, OP_USBCMD, cmd & ~USBCMD_RS);
    for (int i = 0; i < 1000; i++) {
        if (rd32(x->op_regs, OP_USBSTS) & USBSTS_HCH) break;
        timer_mdelay(1);
    }
    if (!(rd32(x->op_regs, OP_USBSTS) & USBSTS_HCH)) {
        kerr("usb", "the controller will not halt");
        return false;
    }

    wr32(x->op_regs, OP_USBCMD, USBCMD_HCRST);
    for (int i = 0; i < 1000; i++) {
        u32 c = rd32(x->op_regs, OP_USBCMD);
        u32 s = rd32(x->op_regs, OP_USBSTS);
        if (!(c & USBCMD_HCRST) && !(s & USBSTS_CNR)) return true;
        timer_mdelay(1);
    }
    kerr("usb", "the controller will not come out of reset");
    return false;
}

static bool setup_memory(xhci_t *x) {
    /* The device context base address array, plus the scratchpad pages the
     * controller wants for its own bookkeeping. */
    x->dcbaa = dma_alloc((x->max_slots + 1) * sizeof(u64), &x->dcbaa_phys);
    if (!x->dcbaa) return false;

    u32 hcs2 = rd32(x->cap_regs, CAP_HCSPARAMS2);
    u32 scratch = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
    if (scratch) {
        u64 arr_phys;
        u64 *arr = dma_alloc(scratch * sizeof(u64), &arr_phys);
        if (!arr) return false;
        for (u32 i = 0; i < scratch; i++) {
            u64 p;
            if (!dma_alloc(PAGE_SIZE, &p)) return false;
            arr[i] = p;
        }
        x->dcbaa[0] = arr_phys;
        kdebug("usb", "%u scratchpad page(s)", scratch);
    }

    if (!ring_init(&x->cmd_ring, CMD_RING_SIZE, true)) return false;
    if (!ring_init(&x->event_ring, EVENT_RING_SIZE, false)) return false;

    u64 erst_phys;
    x->erst = dma_alloc(sizeof(erst_entry_t), &erst_phys);
    if (!x->erst) return false;
    x->erst[0].base = x->event_ring.phys;
    x->erst[0].size = EVENT_RING_SIZE;
    x->erst[0].reserved = 0;

    /* Slots, then the arrays, then the interrupter, then run. */
    wr32(x->op_regs, OP_CONFIG, x->max_slots);
    wr64(x->op_regs, OP_DCBAAP, x->dcbaa_phys);
    wr64(x->op_regs, OP_CRCR, x->cmd_ring.phys | 1);          /* with the cycle bit */

    wr32(x->rt_regs, RT_IR0 + IR_ERSTSZ, 1);
    wr64(x->rt_regs, RT_IR0 + IR_ERDP, x->event_ring.phys);
    wr64(x->rt_regs, RT_IR0 + IR_ERSTBA, erst_phys);
    /* Interrupts stay masked: this driver polls the ring from its own thread. */
    wr32(x->rt_regs, RT_IR0 + IR_IMAN, 0);
    wr32(x->rt_regs, RT_IR0 + IR_IMOD, 0);

    wr32(x->op_regs, OP_USBCMD, USBCMD_RS | USBCMD_HSEE);

    for (int i = 0; i < 200; i++) {
        if (!(rd32(x->op_regs, OP_USBSTS) & USBSTS_HCH)) return true;
        timer_mdelay(1);
    }
    kerr("usb", "the controller will not start");
    return false;
}

/* Take one controller: map its registers and read what it says about itself.
 *
 * Everything here is per controller, because on any real desktop there is more
 * than one and they differ - different numbers of ports, different context
 * sizes, different versions.  Sharing one set of these across all of them is
 * what this driver used to do, and the effect was that the keyboard worked or
 * did not depending on which socket it was in.
 */
static bool claim(xhci_t *x, pci_dev_t *d) {
    u64 bar = d->bar[0];
    if (!bar || d->bar_is_io[0]) {
        kwarn("usb", "%02x:%02x.%u has no memory window", d->bus, d->slot, d->func);
        return false;
    }

    pci_enable_memory(d);
    pci_enable_bus_master(d);

    /* This driver reads the event ring from its own thread and wants no
     * interrupts from the controller.  The firmware had it interrupting before
     * we arrived, and those interrupts do not stop being delivered just
     * because nobody is listening for them any more. */
    pci_disable_interrupts(d);

    size_t len = d->bar_size[0] ? (size_t)d->bar_size[0] : 0x10000;
    if (len > 0x100000) len = 0x100000;

    x->cap_regs = vmm_map_mmio(bar, len);
    if (!x->cap_regs) {
        kerr("usb", "cannot map the registers of %02x:%02x.%u",
             d->bus, d->slot, d->func);
        return false;
    }

    u8  caplen  = *(volatile u8 *)(x->cap_regs + CAP_CAPLENGTH);
    u16 version = *(volatile u16 *)(x->cap_regs + CAP_HCIVERSION);
    u32 hcs1 = rd32(x->cap_regs, CAP_HCSPARAMS1);
    u32 hcc1 = rd32(x->cap_regs, CAP_HCCPARAMS1);

    x->op_regs   = x->cap_regs + caplen;
    x->rt_regs   = x->cap_regs + (rd32(x->cap_regs, CAP_RTSOFF) & ~0x1Fu);
    x->doorbells = (volatile u32 *)(x->cap_regs + (rd32(x->cap_regs, CAP_DBOFF) & ~0x3u));

    x->max_slots    = hcs1 & 0xFF;
    x->max_ports    = (hcs1 >> 24) & 0xFF;
    x->context_size = (hcc1 & (1u << 2)) ? 64 : 32;

    if (x->max_slots > MAX_DEVICES) x->max_slots = MAX_DEVICES;
    if (!x->max_slots || !x->max_ports) {
        kerr("usb", "%02x:%02x.%u reports no slots or ports",
             d->bus, d->slot, d->func);
        return false;
    }

    x->bus = d->bus; x->slot = d->slot; x->func = d->func;
    x->pci_vendor = d->vendor; x->pci_device = d->device;
    x->present = true;

    kinfo("usb", "%02x:%02x.%u xHCI %u.%u [%04x:%04x] at %#lx: %u port(s), "
                 "%u slot(s), %u-byte contexts",
          d->bus, d->slot, d->func, version >> 8, (version >> 4) & 0xF,
          d->vendor, d->device, bar, x->max_ports, x->max_slots,
          x->context_size);
    return true;
}

/* Say exactly what this controller can see, port by port.
 *
 * Printed when a machine ends up with no keyboard and no mouse, which is the
 * one situation where nobody can ask the system anything afterwards: there is
 * no keyboard to type a command with, and a desktop nobody can drive tells you
 * nothing.  So the machine has to volunteer it.
 *
 * Every line here answers a question that would otherwise need a guess.  A
 * port with nothing connected means the socket is wired somewhere else; a port
 * connected but not enabled means the reset failed; a device enumerated but
 * not claimed means the driver for it is what is missing rather than the
 * plumbing underneath.
 */
static void report_ports(xhci_t *x) {
    kinfo("usb", "controller %02x:%02x.%u [%04x:%04x]: %u ports; "
                 "input discovery inventory:",
          x->bus, x->slot, x->func, x->pci_vendor, x->pci_device, x->max_ports);

    int connected = 0;
    for (u32 p = 1; p <= x->max_ports; p++) {
        u32 sc = portsc(x, p);
        if (sc == 0xFFFFFFFFu) { kwarn("usb", "  port %u: does not answer", p); continue; }

        bool ccs = (sc & PORTSC_CCS) != 0;
        bool ped = (sc & PORTSC_PED) != 0;
        u8 speed = (u8)((sc >> PORTSC_SPEED_SHIFT) & 0x0F);
        if (ccs) connected++;

        if (!ccs) continue;         /* empty sockets are not the interesting ones */

        kwarn("usb", "  port %u: something connected, %s, speed code %u, "
                     "status %08x", p, ped ? "enabled" : "NOT enabled", speed, sc);
    }
    if (!connected)
        kinfo("usb", "  no connected root ports; an empty controller is not an input failure");

    int listed = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
        usb_device_t *d = &x->devices[i];
        if (!d->present) continue;
        listed++;
        kwarn("usb", "  device: %s (%04x:%04x)%s, %d endpoint(s) claimed",
              d->name, d->vendor, d->product, d->is_hub ? ", a hub" : "",
              d->ep_count);
    }
    if (!listed && connected)
        kwarn("usb", "  %d port(s) have something in them but nothing "
                     "enumerated", connected);
}

/* An interrupt endpoint that has reported nothing is not a broken one.
 *
 * There used to be a watchdog here that reset any polled endpoint which had
 * not completed a transfer for two seconds.  It was written to explain a mouse
 * that never reported, and it was wrong about what it was looking at: an
 * interrupt IN endpoint only produces a transfer event when the device
 * actually has something to say.  A mouse nobody is touching answers every
 * poll with NAK, which generates no event at all, so silence is the normal and
 * correct state of an idle input device rather than evidence of a fault.
 *
 * Acting on that misreading did real harm.  Reset Endpoint is only valid
 * against an endpoint in the Halted state; issued against a Running one it is
 * a context state error, and it was being issued at every device on the bus
 * every two seconds.  It also tripped a fallback that switched keyboards into
 * boot protocol - discarding the report descriptor that had been parsed
 * correctly - on the strength of the same false signal.
 *
 * The endpoint halt recovery in handle_transfer() remains: that one acts on a
 * completion code the controller actually reported, rather than on the absence
 * of any report at all.
 */

/* --------------------------------------------------------------- the thread
 *
 * One per controller.  They share nothing but the counts of how many keyboards
 * and mice the machine has altogether, so they need no locking between them -
 * which is most of why one thread each is simpler than one thread juggling
 * several, as well as being what lets a slow device on one controller not hold
 * up another.
 */
static void usb_thread(void *arg) {
    xhci_t *x = (xhci_t *)arg;

    bios_handoff(x);

    if (!reset_controller(x) || !setup_memory(x)) {
        kerr("usb", "%02x:%02x.%u did not come up; nothing plugged into it "
                    "will work", x->bus, x->slot, x->func);
        x->initial_scan_done = true;   /* nothing more will come from it */
        return;
    }
    x->controller_up = true;

    /* Powering a port is optional on some controllers and required on others;
     * doing it unconditionally is harmless. */
    for (u32 p = 1; p <= x->max_ports; p++) {
        u32 sc = portsc(x, p);
        if (!(sc & PORTSC_PP)) portsc_write(x, p, portsc_keep(sc) | PORTSC_PP);
    }
    sched_sleep_ms(100);          /* let the ports settle before looking */

    scan_ports(x);
    /* And then everything hanging off whatever those ports found.  A hub has
     * to be enumerated as a device before its own ports can be asked about,
     * so this cannot be folded into the scan above. */
    scan_hubs(x);

    /* The initial enumeration is complete: any USB disk this system booted from
     * is now a block device.  Announced by a flag rather than by joining the
     * thread, because the thread lives on to service the bus - and the storage
     * mount, which runs on the boot thread, has to be able to wait for THIS
     * moment or it looks for the boot
     * volume before the stick it is on has appeared and finds nothing. */
    x->initial_scan_done = true;

    /* If this controller turned up nothing usable, say what it did see.  On
     * a machine with no working keyboard there is no other way to ask. */
    if (!keyboards && !mice) report_ports(x);

    /* An idle HID endpoint may NAK indefinitely. Enter normal servicing now:
     * waiting for user input delays endpoint recovery and isochronous audio. */

    /* From here on: drain events, and rescan now and then so a keyboard
     * plugged in after boot still works - in a hub as much as in the machine,
     * which is where most keyboards on a desk actually are. */
    u64 next_scan = g_uptime_ms + 1000;
    u64 next_beat = g_uptime_ms + 2000;
    u32 last_health_faults = 0;
    for (;;) {
        pump(x, 0, NULL, 0);
        process_pending_resets(x);   /* recover halted endpoints OUT of the drain */
        usbhid_tick();
        pump_isoch(x);

        /* A heartbeat, because the log is now read after the fact rather than
         * watched as it happens.
         *
         * Two very different faults look identical from the desktop - the
         * controller reporting nothing, and this thread no longer running to
         * ask it - and nothing already logged tells them apart: both simply
         * stop producing lines.  A line every two seconds carrying the
         * counters distinguishes them.  If it stops appearing, the thread
         * stopped; if it keeps appearing with the numbers frozen, the thread
         * is fine and the controller has gone quiet. */
        /* Rate-limited to what it actually has to say.
         *
         * A line every two seconds regardless was costing about thirty lines a
         * minute per controller, and with two controllers that is the whole of
         * an eight-kilobyte log tail spent on an idle machine repeating that
         * nothing has happened.  It buried the thing somebody had run the
         * machine to find out - `gpu start`'s report was gone before the
         * shutdown that collected the log.
         *
         * The heartbeat still distinguishes the two faults it was written for.
         * A change is reported at once; an unchanged controller still reports
         * every thirty seconds, so a thread that has stopped still shows as
         * lines that stop, and a controller gone quiet still shows as numbers
         * that stand still. */
        if (g_uptime_ms >= next_beat) {
            next_beat = g_uptime_ms + 2000;

            /* Judge controller health from status, not whether a person moved
             * the mouse. CNR is bit 11; bit 12 is a controller error. */
            u32 health = rd32(x->op_regs, OP_USBSTS);
            u32 faults = health & (USBSTS_HCH | USBSTS_HSE | USBSTS_CNR | USBSTS_HCE);
            if (faults != last_health_faults) {
                if (faults)
                    kerr("usb", "%02x:%02x.%u: USBSTS %08x%s%s%s%s",
                         x->bus, x->slot, x->func, health,
                         (faults & USBSTS_HCH) ? " HALTED" : "",
                         (faults & USBSTS_HSE) ? " HOST-SYSTEM-ERROR" : "",
                         (faults & USBSTS_CNR) ? " NOT-READY" : "",
                         (faults & USBSTS_HCE) ? " HOST-CONTROLLER-ERROR" : "");
                else
                    kinfo("usb", "%02x:%02x.%u: controller status recovered",
                          x->bus, x->slot, x->func);
                last_health_faults = faults;
            }

            u32 eps = 0, live = 0, done = 0;
            for (int i = 0; i < MAX_DEVICES; i++) {
                usb_device_t *d = &x->devices[i];
                if (!d->present) continue;
                for (int e = 0; e < d->ep_count; e++) {
                    if (!d->ep[e].active || !d->ep[e].polled) continue;
                    eps++;
                    done += d->ep[e].completions;
                    u32 *lc = ep_ctx(x, d->dev_ctx, d->ep[e].dci, false);
                    if ((lc[0] & 7) == 1) live++;
                }
            }

            bool changed = (x->events_seen    != x->beat_events ||
                            x->transfer_events != x->beat_transfers ||
                            done               != x->beat_completions ||
                            eps                != x->beat_endpoints);

            if (changed || g_uptime_ms >= x->beat_forced_at) {
                x->beat_events      = x->events_seen;
                x->beat_transfers   = x->transfer_events;
                x->beat_completions = done;
                x->beat_endpoints   = eps;
                x->beat_forced_at   = g_uptime_ms + 30000;

                kinfo("usb", "hc alive: events %u transfers %u last code %u; "
                             "polled endpoints %u (%u running) completions %u",
                      x->events_seen, x->transfer_events,
                      x->last_transfer_cc, eps, live, done);
            }
        }

        if (g_uptime_ms >= next_scan) {
            next_scan = g_uptime_ms + 1000;
            if (rd32(x->op_regs, OP_USBSTS) & USBSTS_PCD)
                wr32(x->op_regs, OP_USBSTS, USBSTS_PCD);
            scan_ports(x);
            scan_hubs(x);
        }
        sched_sleep_ms(1);
    }
}

/* ------------------------------------------------------------------- public */

void usb_init(void) {
    /* Class 0x0C subclass 0x03 prog-if 0x30 is xHCI, and every one of them is
     * taken rather than the first.
     *
     * A desktop has several.  The machine this was written for has three: one
     * on the processor package for the USB4 ports, the router behind it, and
     * one on the chipset - and it is the chipset's that the back panel and the
     * front-panel headers are wired to, which is to say the one a keyboard is
     * usually in.  Driving whichever the bus walk returned first made a
     * working keyboard a matter of luck, and made a keyboard on one controller
     * and a mouse on another impossible.
     *
     * There is nothing clever about the fix and there should not be: they are
     * independent pieces of hardware, so each gets its own state and its own
     * thread, and none of them has to know the others exist.
     */
    int found = 0;

    for (pci_dev_t *d = pci_find(0x0C, 0x03, 0x30, NULL); d;
         d = pci_find(0x0C, 0x03, 0x30, d)) {
        if (controller_count >= MAX_CONTROLLERS) {
            kwarn("usb", "more than %d USB controllers; the rest are ignored",
                  MAX_CONTROLLERS);
            break;
        }
        found++;

        xhci_t *x = &controllers[controller_count];
        memset(x, 0, sizeof *x);
        if (!claim(x, d)) continue;

        char name[16];
        snprintf(name, sizeof name, "usb%d", controller_count);
        if (kthread_create(name, usb_thread, x) < 0) {
            kerr("usb", "cannot start a thread for %02x:%02x.%u",
                 x->bus, x->slot, x->func);
            x->present = false;
            continue;
        }
        pci_claim(d, "xhci");
        controller_count++;
    }

    if (!found) { kdebug("usb", "no xHCI controller present"); return; }

    kinfo("usb", "%d of %d USB controller(s) brought up", controller_count, found);

    /* Wait for each controller's first port+hub scan before returning, so a USB
     * disk this system booted from has become a block device by the time the
     * boot sequence mounts storage.  Without this, "mounting storage" runs while
     * enumeration is still in flight on the usb threads, finds no data partition
     * on the stick it is running from, and the whole machine falls back to
     * running from RAM - no /data, no firmware, no log.  Bounded so a controller
     * that never settles cannot hold the boot forever; the flusher thread still
     * picks up a disk that turns up after the deadline. */
    u64 deadline = g_uptime_ms + 8000;
    for (;;) {
        bool all = true;
        for (int i = 0; i < controller_count; i++)
            if (!controllers[i].initial_scan_done) { all = false; break; }
        if (all) break;
        if (g_uptime_ms >= deadline) {
            kwarn("usb", "gave up waiting for enumeration to settle; a boot disk "
                         "on USB may not be mounted yet");
            break;
        }
        sched_sleep_ms(2);
    }
}

void usb_count_hid(int kbd_delta, int mouse_delta) {
    keyboards += kbd_delta;
    mice += mouse_delta;
}

bool usb_keyboard_present(void) { return keyboards > 0; }
bool usb_mouse_present(void)    { return mice > 0; }

/* Every device on the machine, not every device on one controller.  These two
 * are what the device list and the shell ask, and "how many USB devices are
 * there" was never a question about a particular piece of silicon. */
/* What the controllers have actually seen, summed across all of them.
 *
 * This exists so the answer can reach a screen.  On a machine with no working
 * input the console is covered by a desktop that cannot be driven, so a
 * warning printed at boot is written where nobody can read it - which is
 * exactly what happened to the first version of this report.  These numbers go
 * into a panel the desktop draws instead. */
void usb_event_counts(u32 *events, u32 *transfers, u8 *last_code,
                      u8 *worst_ep_state) {
    u32 ev = 0, tr = 0;
    u8  cc = 0, state = 1;

    for (int c = 0; c < controller_count; c++) {
        xhci_t *x = &controllers[c];
        if (!x->present) continue;
        ev += x->events_seen;
        tr += x->transfer_events;
        if (x->last_transfer_cc) cc = x->last_transfer_cc;

        for (int i = 0; i < MAX_DEVICES; i++) {
            usb_device_t *d = &x->devices[i];
            if (!d->present) continue;
            for (int e = 0; e < d->ep_count; e++) {
                if (!d->ep[e].active || !d->ep[e].polled) continue;
                u32 *live = ep_ctx(x, d->dev_ctx, d->ep[e].dci, false);
                u32 st = live[0] & 7;
                if (st != 1) state = (u8)st;      /* anything not Running */
            }
        }
    }

    if (events) *events = ev;
    if (transfers) *transfers = tr;
    if (last_code) *last_code = cc;
    if (worst_ep_state) *worst_ep_state = state;
}

int usb_device_count(void) {
    int n = 0;
    for (int c = 0; c < controller_count; c++) {
        xhci_t *x = &controllers[c];
        if (!x->present) continue;
        for (int i = 0; i < MAX_DEVICES; i++) if (x->devices[i].present) n++;
    }
    return n;
}

int usb_snapshot(usb_devinfo_t *out, int max) {
    int n = 0;
    for (int c = 0; c < controller_count && n < max; c++) {
      xhci_t *x = &controllers[c];
      if (!x->present || !x->controller_up) continue;
      for (int i = 0; i < MAX_DEVICES && n < max; i++) {
        usb_device_t *d = &x->devices[i];
        if (!d->present) continue;
        memset(&out[n], 0, sizeof out[n]);
        out[n].port = d->port;
        out[n].slot = d->slot;
        out[n].speed = d->speed;
        out[n].vendor = d->vendor;
        out[n].product = d->product;
        strlcpy(out[n].name, d->name, sizeof out[n].name);
        for (int e = 0; e < d->ep_count; e++)
            if (d->ep[e].active && d->ep[e].drv_name[0]) {
                strlcpy(out[n].driver, d->ep[e].drv_name, sizeof out[n].driver);
                break;
            }
        n++;
      }
    }
    return n;
}
