/* r8169.c - Realtek gigabit and 2.5-gigabit Ethernet.
 *
 * The ordinary wired network on a desktop.  Almost every consumer motherboard
 * of the last fifteen years has one of these behind its Ethernet socket: an
 * RTL8111/8168 where the socket is gigabit, an RTL8125 or 8126 where it is
 * 2.5-gigabit.  The system already had a driver for the RTL8139, which is the
 * part before this family and has not shipped on a board in twenty years.
 *
 * The card is a pair of descriptor rings and very little else.  Each entry
 * says where a frame is and who owns it, the card walks the rings, and the
 * ownership bit is the whole protocol: the driver sets it to hand a descriptor
 * over and the card clears it to hand it back.  Everything below the ring
 * setup is arranging for that bit to mean what both sides think it means.
 *
 * What differs between the gigabit parts and the 2.5-gigabit ones is smaller
 * than it looks and easy to get wrong: the interrupt mask and status registers
 * moved and widened.  Writing sixteen bits to the old place on an 8125 lands
 * in the middle of the new pair and does nothing useful, which is a card that
 * comes up, links, and never reports a single frame.
 */
#include "kernel.h"
#include "klog.h"
#include "time.h"
#include "pci.h"
#include "mm.h"
#include "net.h"
#include "r8169.h"

/* Keep the compiler from moving a descriptor's ownership bit ahead of the
 * fields it describes: the card reads them in memory order and a descriptor
 * handed over before it was filled in is a frame of whatever was there. */
static void barrier(void) { __asm__ volatile("" ::: "memory"); }

#define RX_RING     32
#define TX_RING     32
#define BUFFER_SIZE 2048
#define MAX_CARDS   4

typedef struct {
    netdev_t     dev;
    volatile u8 *regs;

    r8169_desc_t *rx, *tx;
    u64           rx_phys, tx_phys;
    u8           *rx_buf, *tx_buf;
    u64           rx_buf_phys, tx_buf_phys;

    u32 rx_next, tx_next;
    bool is_2500;                  /* an 8125 or 8126, where registers moved */
    bool modelled;
} r8169_t;

static r8169_t cards[MAX_CARDS];
static int card_count;

/* --------------------------------------------------------------- registers */

/* A modelled card is ordinary memory, so a write to it lands and nothing
 * happens.  Reading is where the model is given its chance to catch up - which
 * works because a driver waiting on hardware is, by definition, reading it. */
void r8169_model_tick(void);

static void settle(r8169_t *c) { if (c->modelled) r8169_model_tick(); }

static u8  rd8 (r8169_t *c, u32 o) { settle(c); return *(volatile u8  *)(c->regs + o); }
static u16 rd16(r8169_t *c, u32 o) { settle(c); return *(volatile u16 *)(c->regs + o); }
static u32 rd32(r8169_t *c, u32 o) { settle(c); return *(volatile u32 *)(c->regs + o); }
static void wr8 (r8169_t *c, u32 o, u8  v) { *(volatile u8  *)(c->regs + o) = v; }
static void wr16(r8169_t *c, u32 o, u16 v) { *(volatile u16 *)(c->regs + o) = v; }
static void wr32(r8169_t *c, u32 o, u32 v) { *(volatile u32 *)(c->regs + o) = v; }
static void wr64(r8169_t *c, u32 o, u64 v) {
    /* Two halves, low first: the card latches the ring address when the high
     * half arrives, so writing them the other way round hands it an address
     * that is half old. */
    wr32(c, o, (u32)v);
    wr32(c, o + 4, (u32)(v >> 32));
}

/* Silence the card's interrupts.  This driver polls, and where the mask and
 * status live is the one thing that moved between the two generations. */
static void mask_interrupts(r8169_t *c) {
    if (c->is_2500) {
        wr32(c, R8125_IMR, 0);
        wr32(c, R8125_ISR, 0xFFFFFFFFu);      /* write one to clear */
    } else {
        wr16(c, R8169_IMR, 0);
        wr16(c, R8169_ISR, 0xFFFF);
    }
}

/* ------------------------------------------------------------------- rings */

static bool setup_rings(r8169_t *c) {
    u64 phys;

    c->rx = dma_alloc_pages((RX_RING * sizeof(r8169_desc_t) + PAGE_SIZE - 1) / PAGE_SIZE, &phys);
    if (!c->rx) return false;
    c->rx_phys = phys;

    c->tx = dma_alloc_pages((TX_RING * sizeof(r8169_desc_t) + PAGE_SIZE - 1) / PAGE_SIZE, &phys);
    if (!c->tx) return false;
    c->tx_phys = phys;

    c->rx_buf = dma_alloc_pages((RX_RING * BUFFER_SIZE + PAGE_SIZE - 1) / PAGE_SIZE, &phys);
    if (!c->rx_buf) return false;
    c->rx_buf_phys = phys;

    c->tx_buf = dma_alloc_pages((TX_RING * BUFFER_SIZE + PAGE_SIZE - 1) / PAGE_SIZE, &phys);
    if (!c->tx_buf) return false;
    c->tx_buf_phys = phys;

    memset(c->rx, 0, RX_RING * sizeof(r8169_desc_t));
    memset(c->tx, 0, TX_RING * sizeof(r8169_desc_t));

    for (int i = 0; i < RX_RING; i++) {
        c->rx[i].address = c->rx_buf_phys + (u64)i * BUFFER_SIZE;
        c->rx[i].flags = DESC_OWN | BUFFER_SIZE;
    }
    /* Exactly one descriptor carries the end-of-ring bit, and it has to be the
     * last: the card wraps on that bit rather than on a count, so a second one
     * anywhere would shorten the ring and a missing one would run it off the
     * end of the allocation. */
    c->rx[RX_RING - 1].flags |= DESC_EOR;
    c->tx[TX_RING - 1].flags = DESC_EOR;

    c->rx_next = 0;
    c->tx_next = 0;
    return true;
}

/* ------------------------------------------------------------------ bring up */

static bool reset_card(r8169_t *c) {
    wr8(c, R8169_CMD, CMD_RESET);
    for (int i = 0; i < 1000; i++) {
        if (!(rd8(c, R8169_CMD) & CMD_RESET)) return true;
        timer_mdelay(1);
    }
    return false;
}

static void read_mac(r8169_t *c) {
    for (int i = 0; i < 6; i++) c->dev.mac.addr[i] = rd8(c, R8169_MAC0 + i);
}

static void read_link(r8169_t *c) {
    u16 s = rd16(c, R8169_PHYSTATUS);
    c->dev.link_up = (s & PHY_LINK_OK) != 0;

    if (!c->dev.link_up)        c->dev.link_speed_mbps = 0;
    else if (s & PHY_2500M)     c->dev.link_speed_mbps = 2500;
    else if (s & PHY_1000M)     c->dev.link_speed_mbps = 1000;
    else if (s & PHY_100M)      c->dev.link_speed_mbps = 100;
    else                        c->dev.link_speed_mbps = 10;
}

static bool bring_up(r8169_t *c) {
    if (!reset_card(c)) return false;

    mask_interrupts(c);
    read_mac(c);

    if (!setup_rings(c)) return false;

    /* The configuration registers refuse writes until unlocked, and locking
     * them again afterwards is what stops a later stray write reconfiguring
     * the card. */
    wr8(c, R8169_9346CR, LOCK_CONFIG_UNLOCK);

    /* Frames larger than this are dropped by the card rather than split. */
    wr16(c, R8169_RMS, BUFFER_SIZE);
    wr8(c, R8169_MTPS, 0x3F);

    wr32(c, R8169_TCR, 0x03000700);           /* the documented default */
    wr16(c, R8169_CPCR, CPCR_RX_CHECKSUM);

    wr64(c, R8169_RDSAR, c->rx_phys);
    wr64(c, R8169_TNPDS, c->tx_phys);

    /* Transmit and receive have to be running before the receive filter is
     * set: the filter register is ignored while the receiver is stopped. */
    wr8(c, R8169_CMD, CMD_TX_ENABLE | CMD_RX_ENABLE);

    wr32(c, R8169_RCR, RCR_ACCEPT_OURS | RCR_ACCEPT_BROADCAST | RCR_ACCEPT_MULTI);
    wr32(c, R8169_MAR0, 0xFFFFFFFFu);
    wr32(c, R8169_MAR0 + 4, 0xFFFFFFFFu);

    wr8(c, R8169_9346CR, LOCK_CONFIG_LOCK);

    read_link(c);
    return true;
}

/* -------------------------------------------------------------- the two ends */

static int r8169_transmit(netdev_t *dev, const void *frame, int len) {
    r8169_t *c = dev->ctx;
    if (len <= 0 || len > BUFFER_SIZE) return -1;

    r8169_desc_t *d = &c->tx[c->tx_next];
    if (d->flags & DESC_OWN) {
        dev->tx_dropped++;
        return -1;                            /* the ring is full */
    }

    memcpy(c->tx_buf + (size_t)c->tx_next * BUFFER_SIZE, frame, (size_t)len);
    d->address = c->tx_buf_phys + (u64)c->tx_next * BUFFER_SIZE;

    /* A frame that fits in one descriptor is both the first fragment and the
     * last.  The end-of-ring bit belongs to the descriptor, not the frame, so
     * it is preserved rather than rewritten. */
    u32 eor = (c->tx_next == TX_RING - 1) ? DESC_EOR : 0;
    d->vlan = 0;
    barrier();
    d->flags = DESC_OWN | DESC_FS | DESC_LS | eor | ((u32)len & DESC_LEN_MASK);
    barrier();

    /* Ring the doorbell.  The 8125 moved it to 0x90 and takes bit 0 through a
     * sixteen-bit write; the older parts take bit 6 of the byte at 0x38.  On an
     * 8125 that byte is the interrupt mask, so ringing the old place there does
     * nothing but corrupt the mask - which is why this has to know the part. */
    if (c->is_2500)
        wr16(c, R8125_TPPOLL, TPPOLL_8125_KICK);
    else
        wr8(c, R8169_TPPOLL, TPPOLL_NPQ);       /* there is work */
    settle(c);

    c->tx_next = (c->tx_next + 1) % TX_RING;
    dev->tx_packets++;
    dev->tx_bytes += (u64)len;
    return len;
}

static void r8169_poll(netdev_t *dev) {
    r8169_t *c = dev->ctx;

    for (int handled = 0; handled < RX_RING; handled++) {
        r8169_desc_t *d = &c->rx[c->rx_next];
        if (d->flags & DESC_OWN) break;       /* still the card's */

        u32 len = d->flags & DESC_LEN_MASK;

        if ((d->flags & DESC_RX_RES) || !len) {
            dev->rx_errors++;
        } else {
            /* The length the card reports includes the four-byte checksum it
             * checked and kept; passing that up would append four bytes of
             * rubbish to every frame in the system. */
            if (len > 4) len -= 4;
            net_receive(dev, c->rx_buf + (size_t)c->rx_next * BUFFER_SIZE, (int)len);
        }

        /* Hand the descriptor back, preserving the end-of-ring bit. */
        u32 eor = (c->rx_next == RX_RING - 1) ? DESC_EOR : 0;
        d->address = c->rx_buf_phys + (u64)c->rx_next * BUFFER_SIZE;
        barrier();
        d->flags = DESC_OWN | eor | BUFFER_SIZE;

        c->rx_next = (c->rx_next + 1) % RX_RING;
    }

    read_link(c);
}

/* ------------------------------------------------------------ what it is */

static const char *model_name(u16 device, bool *is_2500) {
    *is_2500 = false;
    switch (device) {
    case 0x8125: *is_2500 = true;  return "Realtek RTL8125 2.5 gigabit";
    case 0x8126: *is_2500 = true;  return "Realtek RTL8126 5 gigabit";
    case 0x3000: *is_2500 = true;  return "Realtek RTL8125 2.5 gigabit";
    case 0x8168: return "Realtek RTL8111/8168 gigabit";
    case 0x8161: return "Realtek RTL8168 gigabit";
    case 0x8167: return "Realtek RTL8169 gigabit";
    case 0x8169: return "Realtek RTL8169 gigabit";
    case 0x8136: return "Realtek RTL8101/8102 fast Ethernet";
    default:     return NULL;
    }
}

void r8169_init(void) {
    pci_dev_t *pci = NULL;

    while ((pci = pci_find(0x02, 0x00, 0xFF, pci)) != NULL) {
        if (pci->vendor != 0x10EC) continue;
        if (card_count >= MAX_CARDS) break;

        bool is_2500 = false;
        const char *name = model_name(pci->device, &is_2500);
        if (!name) {
            kinfo("r8169", "%02x:%02x.%u is a Realtek network card this driver "
                           "does not know (%04x)", pci->bus, pci->slot,
                  pci->func, pci->device);
            continue;
        }

        r8169_t *c = &cards[card_count];
        memset(c, 0, sizeof *c);
        c->is_2500 = is_2500;

        /* The registers are behind whichever bar is memory; on these cards it
         * is the second or the third depending on the part, and the first is
         * often the old port-mapped window. */
        int bar = -1;
        for (int i = 0; i < 6; i++)
            if (pci->bar[i] && !pci->bar_is_io[i]) { bar = i; break; }
        if (bar < 0) {
            kwarn("r8169", "%s has no memory window", name);
            continue;
        }

        pci_enable_memory(pci);
        pci_enable_bus_master(pci);
        pci_disable_interrupts(pci);          /* this driver polls */

        size_t len = pci->bar_size[bar] ? (size_t)pci->bar_size[bar] : 0x1000;
        if (len > 0x10000) len = 0x10000;
        c->regs = vmm_map_mmio(pci->bar[bar], len);
        if (!c->regs) continue;

        snprintf(c->dev.name, sizeof c->dev.name, "eth%d", netdev_count());
        strlcpy(c->dev.model, name, sizeof c->dev.model);
        c->dev.ctx = c;
        c->dev.transmit = r8169_transmit;
        c->dev.poll = r8169_poll;

        if (!bring_up(c)) {
            kwarn("r8169", "%s would not start", name);
            continue;
        }

        netdev_register(&c->dev);
        pci_claim(pci, "r8169");
        card_count++;

        kinfo("r8169", "%s: %s, %02x:%02x:%02x:%02x:%02x:%02x, link %s",
              c->dev.name, name,
              c->dev.mac.addr[0], c->dev.mac.addr[1], c->dev.mac.addr[2],
              c->dev.mac.addr[3], c->dev.mac.addr[4], c->dev.mac.addr[5],
              c->dev.link_up ? "up" : "down");
    }
}

/* --------------------------------------------------------------- self-test */

void r8169_model_set_2500(bool yes);

/* One generation of the card.
 *
 * Which generation matters more than it looks.  The two put their interrupt
 * mask and status in different places and at different widths - sixteen bits
 * at one offset on the older cards, thirty-two at another on the 8125 and
 * 8126 - and that is the whole of what `is_2500` decides.
 *
 * A driver that had them the wrong way round would silence nothing: it would
 * write a mask into some other register and leave the real one enabled, and
 * the card would then interrupt a system that polls and has nothing to answer
 * with.  Nothing about that shows up in a frame going out correctly, which is
 * what the rest of this checks - so both have to be run, and until now only
 * one was.
 */
static int drive_one_generation(bool as_2500) {
    int failures = 0;

    volatile u8 *regs = NULL;
    size_t size = 0;
    r8169_model_set_2500(as_2500);
    if (!r8169_model_attach(&regs, &size)) {
        kerr("r8169", "the model would not attach");
        return 1;
    }

    static r8169_t c;
    memset(&c, 0, sizeof c);
    c.regs = regs;
    c.modelled = true;
    c.is_2500 = as_2500;
    strlcpy(c.dev.name, "eth-test", sizeof c.dev.name);
    c.dev.ctx = &c;

    if (!bring_up(&c)) {
        kerr("r8169", "the model card would not start");
        r8169_model_detach();
        return 1;
    }

    /* The address it reports has to be the address the model holds: reading it
     * a byte at a time from the wrong base gives six plausible bytes. */
    static const u8 want[6] = { 0x00, 0xE0, 0x4C, 0x11, 0x22, 0x33 };
    if (memcmp(c.dev.mac.addr, want, 6)) {
        kerr("r8169", "the station address came out as "
                      "%02x:%02x:%02x:%02x:%02x:%02x",
             c.dev.mac.addr[0], c.dev.mac.addr[1], c.dev.mac.addr[2],
             c.dev.mac.addr[3], c.dev.mac.addr[4], c.dev.mac.addr[5]);
        failures++;
    }

    /* A frame out.  The model checks it owns the descriptor and that the
     * length and flags say what a single-fragment frame should. */
    static const u8 frame[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0xE0, 0x4C, 0x11, 0x22, 0x33,
        0x08, 0x06, 'k', 'e', 's', 't', 'r', 'e', 'l',
    };
    if (r8169_transmit(&c.dev, frame, sizeof frame) != (int)sizeof frame) {
        kerr("r8169", "the frame was not accepted for transmission");
        failures++;
    }

    const void *sent = NULL;
    int sent_len = 0;
    if (!r8169_model_transmitted(&sent, &sent_len)) {
        kerr("r8169", "the card was never told there was anything to send");
        failures++;
    } else if (sent_len != (int)sizeof frame || memcmp(sent, frame, sizeof frame)) {
        kerr("r8169", "%d byte(s) left the card, and not the ones handed to it",
             sent_len);
        failures++;
    }

    /* And a frame in.  The model appends a checksum the way a card does, so
     * this also checks that those four bytes are taken off again. */
    static int received_len;
    static u8  received[64];
    extern void net_test_capture(u8 *buf, size_t cap, int *len_out);
    net_test_capture(received, sizeof received, &received_len);

    r8169_model_receive(frame, sizeof frame);
    r8169_poll(&c.dev);

    net_test_capture(NULL, 0, NULL);

    if (received_len != (int)sizeof frame) {
        kerr("r8169", "a %u-byte frame arrived as %d bytes - the checksum the "
                      "card keeps was not taken off",
             (unsigned)sizeof frame, received_len);
        failures++;
    } else if (memcmp(received, frame, sizeof frame)) {
        kerr("r8169", "the frame that arrived is not the one that was sent");
        failures++;
    }

    r8169_model_detach();

    if (!failures)
        kinfo("r8169", "a %s Realtek card is driven correctly: its address is "
                       "read, a frame goes out whole, and one arrives with the "
                       "card's own checksum removed",
              as_2500 ? "two-and-a-half gigabit" : "gigabit");
    return failures;
}

int r8169_selftest(void) {
    /* Both generations, because the registers this driver has to get right are
     * exactly the ones that moved between them.  The 8125 first: it is the
     * card in this machine. */
    int failures = drive_one_generation(true);
    failures += drive_one_generation(false);

    /* And back to the default, so anything running after this finds the model
     * as it expects it. */
    r8169_model_set_2500(true);
    return failures;
}
