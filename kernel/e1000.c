/* e1000.c - Intel gigabit Ethernet.
 *
 * One driver covers a wide range of parts, because Intel kept the descriptor
 * format and the core register layout across the whole family: the 82540 in a
 * desktop from 2002, the 82574L on a server board, the I217 and I219 in nearly
 * every business laptop of the last decade, the I210 and I225 on current
 * boards, and the virtual card VMware and QEMU present.  What differs between
 * them is initialisation detail, not how packets move.
 *
 * Transmit and receive are both rings of descriptors in ordinary memory.  The
 * card owns everything between its head and the driver's tail; the driver
 * advances the tail to hand buffers over and watches a status bit to learn when
 * one has come back.  Nothing here takes an interrupt: at these rates polling
 * the descriptor status is cheaper than the interrupt would be, and it keeps
 * the receive path out of interrupt context entirely.
 */
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "time.h"
#include "klog.h"
#include "net.h"

/* --------------------------------------------------------------- registers */

#define E1000_CTRL     0x0000
#define E1000_STATUS   0x0008
#define E1000_EECD     0x0010
#define E1000_EERD     0x0014
#define E1000_ICR      0x00C0
#define E1000_IMS      0x00D0
#define E1000_IMC      0x00D8
#define E1000_RCTL     0x0100
#define E1000_TCTL     0x0400
#define E1000_TIPG     0x0410

#define E1000_RDBAL    0x2800
#define E1000_RDBAH    0x2804
#define E1000_RDLEN    0x2808
#define E1000_RDH      0x2810
#define E1000_RDT      0x2818

#define E1000_TDBAL    0x3800
#define E1000_TDBAH    0x3804
#define E1000_TDLEN    0x3808
#define E1000_TDH      0x3810
#define E1000_TDT      0x3818

#define E1000_MTA      0x5200      /* multicast table, 128 entries */
#define E1000_RAL      0x5400      /* receive address, low then high */
#define E1000_RAH      0x5404

#define CTRL_SLU       (1u << 6)   /* set link up          */
#define CTRL_RST       (1u << 26)
#define CTRL_ASDE      (1u << 5)   /* auto speed detection */

#define STATUS_LU      (1u << 1)   /* link up              */
#define STATUS_SPEED   (3u << 6)

#define RCTL_EN        (1u << 1)
#define RCTL_SBP       (1u << 2)
#define RCTL_UPE       (1u << 3)   /* unicast promiscuous  */
#define RCTL_MPE       (1u << 4)   /* multicast promiscuous */
#define RCTL_LPE       (1u << 5)   /* long packets          */
#define RCTL_BAM       (1u << 15)  /* accept broadcast      */
#define RCTL_BSIZE_2048 0
#define RCTL_SECRC     (1u << 26)  /* strip the CRC         */

#define TCTL_EN        (1u << 1)
#define TCTL_PSP       (1u << 3)   /* pad short packets     */
#define TCTL_CT_SHIFT  4
#define TCTL_COLD_SHIFT 12

#define RAH_AV         (1u << 31)  /* this address is valid */

/* Receive descriptor status. */
#define RX_STATUS_DD   (1u << 0)   /* the card is done with it */
#define RX_STATUS_EOP  (1u << 1)

/* Transmit descriptor command and status. */
#define TX_CMD_EOP     (1u << 0)
#define TX_CMD_IFCS    (1u << 1)   /* insert the frame checksum */
#define TX_CMD_RS      (1u << 3)   /* report status             */
#define TX_STATUS_DD   (1u << 0)

/* ------------------------------------------------------------- descriptors */

typedef struct {
    u64 address;
    u16 length;
    u16 checksum;
    u8  status;
    u8  errors;
    u16 special;
} __attribute__((packed)) rx_desc_t;

typedef struct {
    u64 address;
    u16 length;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
} __attribute__((packed)) tx_desc_t;

#define RX_RING 32
#define TX_RING 16
#define BUFFER_SIZE 2048

typedef struct {
    volatile u8 *regs;
    netdev_t     dev;

    rx_desc_t *rx;   u64 rx_phys;
    tx_desc_t *tx;   u64 tx_phys;
    u8        *rx_buf; u64 rx_buf_phys;
    u8        *tx_buf; u64 tx_buf_phys;

    u32 rx_next;
    u32 tx_next;
} e1000_t;

#define MAX_CARDS 2
static e1000_t cards[MAX_CARDS];
static int card_count;

/* ------------------------------------------------------------------- mmio */

static inline u32 rd(e1000_t *c, u32 off) {
    return *(volatile u32 *)(c->regs + off);
}
static inline void wr(e1000_t *c, u32 off, u32 v) {
    *(volatile u32 *)(c->regs + off) = v;
}

/* ------------------------------------------------------------------ address */

/* The card's address is in its EEPROM, but on most recent parts the firmware
 * has already copied it into the receive address registers.  Reading those
 * first means the EEPROM interface - which differs across the family - is only
 * needed on the older cards that require it. */
static bool read_mac(e1000_t *c) {
    u32 low = rd(c, E1000_RAL);
    u32 high = rd(c, E1000_RAH);

    if (low || (high & 0xFFFF)) {
        c->dev.mac.addr[0] = (u8)(low >> 0);
        c->dev.mac.addr[1] = (u8)(low >> 8);
        c->dev.mac.addr[2] = (u8)(low >> 16);
        c->dev.mac.addr[3] = (u8)(low >> 24);
        c->dev.mac.addr[4] = (u8)(high >> 0);
        c->dev.mac.addr[5] = (u8)(high >> 8);
        return true;
    }

    /* Fall back to the EEPROM.  Whether the address bits start at 8 or 2
     * differs by part, so a read is tried both ways and the one that answers
     * is the right one. */
    for (int shift = 8; shift >= 2; shift -= 6) {
        bool ok = true;
        u16 words[3];

        for (int i = 0; i < 3 && ok; i++) {
            wr(c, E1000_EERD, ((u32)i << shift) | 1);
            ok = false;
            for (int spin = 0; spin < 10000; spin++) {
                u32 v = rd(c, E1000_EERD);
                if (v & (shift == 8 ? (1u << 4) : (1u << 1))) {
                    words[i] = (u16)(v >> 16);
                    ok = true;
                    break;
                }
                timer_udelay(1);
            }
        }
        if (!ok) continue;

        for (int i = 0; i < 3; i++) {
            c->dev.mac.addr[i * 2 + 0] = (u8)(words[i] & 0xFF);
            c->dev.mac.addr[i * 2 + 1] = (u8)(words[i] >> 8);
        }
        return true;
    }
    return false;
}

/* --------------------------------------------------------------- transmit */

static int e1000_transmit(netdev_t *dev, const void *frame, int len) {
    e1000_t *c = dev->ctx;
    if (len <= 0 || len > BUFFER_SIZE) return -1;

    u32 slot = c->tx_next;
    tx_desc_t *d = &c->tx[slot];

    /* Wait for the card to finish with this slot.  With a ring this size and
     * one frame in flight at a time it is almost never actually busy. */
    if (d->cmd && !(d->status & TX_STATUS_DD)) {
        for (int i = 0; i < 10000; i++) {
            if (d->status & TX_STATUS_DD) break;
            timer_udelay(10);
        }
        if (!(d->status & TX_STATUS_DD)) {
            dev->tx_dropped++;
            return -1;
        }
    }

    memcpy(c->tx_buf + (size_t)slot * BUFFER_SIZE, frame, (size_t)len);
    d->address = c->tx_buf_phys + (u64)slot * BUFFER_SIZE;
    d->length = (u16)len;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->status = 0;
    d->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;

    c->tx_next = (slot + 1) % TX_RING;
    __asm__ volatile("" ::: "memory");
    wr(c, E1000_TDT, c->tx_next);
    return len;
}

/* ---------------------------------------------------------------- receive */

static void e1000_poll(netdev_t *dev) {
    e1000_t *c = dev->ctx;

    /* Walk forward over every descriptor the card has finished with, then hand
     * the whole run back at once by moving the tail. */
    int handled = 0;
    while (handled < RX_RING) {
        rx_desc_t *d = &c->rx[c->rx_next];
        if (!(d->status & RX_STATUS_DD)) break;

        if ((d->status & RX_STATUS_EOP) && !d->errors && d->length) {
            net_receive(dev, c->rx_buf + (size_t)c->rx_next * BUFFER_SIZE,
                        d->length);
        } else if (d->errors) {
            dev->rx_errors++;
        }

        d->status = 0;
        u32 done = c->rx_next;
        c->rx_next = (c->rx_next + 1) % RX_RING;
        wr(c, E1000_RDT, done);
        handled++;
    }

    /* Keep the link state current, so `net` reports what is actually there. */
    u32 status = rd(c, E1000_STATUS);
    dev->link_up = (status & STATUS_LU) != 0;
    if (dev->link_up) {
        switch ((status & STATUS_SPEED) >> 6) {
        case 0: dev->link_speed_mbps = 10; break;
        case 1: dev->link_speed_mbps = 100; break;
        default: dev->link_speed_mbps = 1000; break;
        }
    } else {
        dev->link_speed_mbps = 0;
    }
}

/* --------------------------------------------------------------- bring-up */

static bool setup_rings(e1000_t *c) {
    u64 phys;

    c->rx = dma_alloc_pages((RX_RING * sizeof(rx_desc_t) + PAGE_SIZE - 1) / PAGE_SIZE,
                            &phys);
    if (!c->rx) return false;
    c->rx_phys = phys;

    c->tx = dma_alloc_pages((TX_RING * sizeof(tx_desc_t) + PAGE_SIZE - 1) / PAGE_SIZE,
                            &phys);
    if (!c->tx) return false;
    c->tx_phys = phys;

    c->rx_buf = dma_alloc_pages((RX_RING * BUFFER_SIZE) / PAGE_SIZE, &phys);
    if (!c->rx_buf) return false;
    c->rx_buf_phys = phys;

    c->tx_buf = dma_alloc_pages((TX_RING * BUFFER_SIZE) / PAGE_SIZE, &phys);
    if (!c->tx_buf) return false;
    c->tx_buf_phys = phys;

    for (int i = 0; i < RX_RING; i++) {
        c->rx[i].address = c->rx_buf_phys + (u64)i * BUFFER_SIZE;
        c->rx[i].status = 0;
    }
    for (int i = 0; i < TX_RING; i++) {
        c->tx[i].address = 0;
        c->tx[i].status = TX_STATUS_DD;    /* free */
        c->tx[i].cmd = 0;
    }

    /* Receive: the ring, then the tail one behind the head so the card owns
     * every descriptor but the last. */
    wr(c, E1000_RDBAL, (u32)c->rx_phys);
    wr(c, E1000_RDBAH, (u32)(c->rx_phys >> 32));
    wr(c, E1000_RDLEN, RX_RING * (u32)sizeof(rx_desc_t));
    wr(c, E1000_RDH, 0);
    wr(c, E1000_RDT, RX_RING - 1);
    c->rx_next = 0;

    wr(c, E1000_TDBAL, (u32)c->tx_phys);
    wr(c, E1000_TDBAH, (u32)(c->tx_phys >> 32));
    wr(c, E1000_TDLEN, TX_RING * (u32)sizeof(tx_desc_t));
    wr(c, E1000_TDH, 0);
    wr(c, E1000_TDT, 0);
    c->tx_next = 0;

    return true;
}

/* Which Intel part this is.  The device id is the only thing that says so, and
 * naming it is worth more than a number when something does not work. */
static const char *e1000_model(u16 device) {
    if (device >= 0x1000 && device <= 0x101F) return "Intel 8254x gigabit";
    if (device >= 0x1049 && device <= 0x105F) return "Intel 8256x/ICH gigabit";
    if (device >= 0x105E && device <= 0x1060) return "Intel 82571 gigabit";
    if (device >= 0x10A4 && device <= 0x10BC) return "Intel 82571/82573 gigabit";
    if (device == 0x10D3) return "Intel 82574L gigabit";   /* before the range
                                                            * below, which
                                                            * contains it */
    if (device >= 0x10C9 && device <= 0x10E3) return "Intel 82576 gigabit";
    if (device >= 0x10EA && device <= 0x10F5) return "Intel 82577/82579 gigabit";
    if (device >= 0x1502 && device <= 0x1503) return "Intel 82579 gigabit";
    if (device >= 0x1521 && device <= 0x1533) return "Intel I210/I350 gigabit";
    if (device >= 0x153A && device <= 0x155A) return "Intel I217 gigabit";
    if (device >= 0x156F && device <= 0x1570) return "Intel I219 gigabit";
    if (device >= 0x15A0 && device <= 0x15B9) return "Intel I218/I219 gigabit";
    if (device >= 0x15D6 && device <= 0x15E3) return "Intel I219 gigabit";
    if (device >= 0x15F4 && device <= 0x15F9) return "Intel I219 gigabit";
    if (device >= 0x0D4C && device <= 0x0D55) return "Intel I219 gigabit";
    if (device >= 0x125B && device <= 0x125F) return "Intel I226 2.5 gigabit";
    if (device >= 0x15F2 && device <= 0x15F3) return "Intel I225 2.5 gigabit";
    return "Intel gigabit Ethernet";
}

/* Can this driver actually drive that part, or only name it?
 *
 * The two are not the same and the difference is invisible from a device id.
 * Every card below is an Intel Ethernet controller, they all answer on the
 * same class, and the table above names all of them - but this driver puts
 * down the ORIGINAL receive descriptor: an address, a length, a checksum, a
 * status byte.  Intel changed that.  The I210, the I350, the I225 and the I226
 * take a different descriptor entirely and are set up through different
 * registers, which is why Linux drives them with separate drivers rather than
 * a wider version of this one.
 *
 * Handed the original descriptor, those cards do not refuse.  They come up,
 * report a link, accept the ring, and pass nothing - and an interface that
 * exists and carries no traffic is the worst shape a fault can take, because
 * every diagnosis starts by checking whether the driver loaded, and it did.
 *
 * So they are named and declined.  A card this system cannot drive should say
 * so in the log, once, with the reason - not appear to work.
 */
static bool needs_a_different_driver(u16 device, const char **why) {
    /* I210, I211, I350 - the "igb" family. */
    if (device >= 0x1521 && device <= 0x1533) {
        if (why) *why = "an I210, I211 or I350";
        return true;
    }
    /* 82575 and 82576, the same family a generation earlier. */
    if (device >= 0x10C9 && device <= 0x10E3 && device != 0x10D3) {
        if (why) *why = "an 82575 or 82576";
        return true;
    }
    /* I225 and I226 - two and a half gigabit, the "igc" family, and the one
     * most likely to be on a board bought recently. */
    if ((device >= 0x15F2 && device <= 0x15F3) ||
        (device >= 0x125B && device <= 0x125F)) {
        if (why) *why = "an I225 or I226 at two and a half gigabit";
        return true;
    }
    return false;
}

static bool bring_up(e1000_t *c, pci_dev_t *pci) {
    /* A reset puts the card in a known state whatever the firmware left it in.
     * Interrupts are masked first: a reset can raise one. */
    wr(c, E1000_IMC, 0xFFFFFFFF);
    (void)rd(c, E1000_ICR);

    wr(c, E1000_CTRL, rd(c, E1000_CTRL) | CTRL_RST);
    timer_mdelay(10);
    for (int i = 0; i < 1000; i++) {
        if (!(rd(c, E1000_CTRL) & CTRL_RST)) break;
        timer_mdelay(1);
    }
    wr(c, E1000_IMC, 0xFFFFFFFF);
    (void)rd(c, E1000_ICR);

    if (!read_mac(c)) {
        kwarn("e1000", "cannot read the card's address");
        return false;
    }

    /* Bring the link up and let the card work out its own speed. */
    wr(c, E1000_CTRL, rd(c, E1000_CTRL) | CTRL_SLU | CTRL_ASDE);

    /* Program our address into filter zero, and clear the multicast table so
     * nothing is accepted through a stale entry. */
    u32 low = (u32)c->dev.mac.addr[0] | ((u32)c->dev.mac.addr[1] << 8) |
              ((u32)c->dev.mac.addr[2] << 16) | ((u32)c->dev.mac.addr[3] << 24);
    u32 high = (u32)c->dev.mac.addr[4] | ((u32)c->dev.mac.addr[5] << 8) | RAH_AV;
    wr(c, E1000_RAL, low);
    wr(c, E1000_RAH, high);
    for (int i = 0; i < 128; i++) wr(c, E1000_MTA + i * 4, 0);

    if (!setup_rings(c)) {
        kerr("e1000", "out of memory setting up the descriptor rings");
        return false;
    }

    /* Accept unicast to our address and broadcast, strip the frame checksum,
     * and use 2 KiB buffers so one frame is never split across two. */
    wr(c, E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);

    /* Pad short frames, and use the collision settings the specification gives
     * for full duplex. */
    wr(c, E1000_TCTL, TCTL_EN | TCTL_PSP | (0x0F << TCTL_CT_SHIFT) |
                      (0x40 << TCTL_COLD_SHIFT));
    wr(c, E1000_TIPG, 0x0060200A);

    /* Interrupts stay masked; the network thread polls the descriptors. */
    wr(c, E1000_IMC, 0xFFFFFFFF);

    strlcpy(c->dev.model, e1000_model(pci->device), sizeof c->dev.model);
    return true;
}

void e1000_init(void) {
    pci_dev_t *pci = NULL;

    /* Class 2 subclass 0 is an Ethernet controller; the vendor decides whether
     * this driver is the right one for it. */
    while ((pci = pci_find(0x02, 0x00, 0xFF, pci)) != NULL) {
        if (pci->vendor != 0x8086) continue;
        if (card_count >= MAX_CARDS) break;

        const char *family = NULL;
        if (needs_a_different_driver(pci->device, &family)) {
            kwarn("e1000", "%02x:%02x.%u is %s [8086:%04x] - this driver puts "
                           "down the original receive descriptor and that part "
                           "takes a different one, so it would come up, report "
                           "a link and carry nothing.  Left alone rather than "
                           "half-driven.",
                  pci->bus, pci->slot, pci->func, family, pci->device);
            continue;
        }

        e1000_t *c = &cards[card_count];
        memset(c, 0, sizeof *c);

        if (!pci->bar[0] || pci->bar_is_io[0]) continue;

        pci_enable_memory(pci);
        pci_enable_bus_master(pci);

        size_t len = pci->bar_size[0] ? (size_t)pci->bar_size[0] : 0x20000;
        if (len > 0x80000) len = 0x80000;
        c->regs = vmm_map_mmio(pci->bar[0], len);
        if (!c->regs) continue;

        snprintf(c->dev.name, sizeof c->dev.name, "eth%d", card_count);
        c->dev.ctx = c;
        c->dev.transmit = e1000_transmit;
        c->dev.poll = e1000_poll;

        if (!bring_up(c, pci)) continue;

        e1000_poll(&c->dev);              /* pick up the link state */
        netdev_register(&c->dev);
        pci_claim(pci, "e1000");
        card_count++;
    }
}
