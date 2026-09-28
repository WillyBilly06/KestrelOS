/* rtl8139.c - Realtek RTL8139 Ethernet.
 *
 * The other card worth having: it was on a great many desktop boards and add-in
 * cards, QEMU and VirtualBox both present it, and its interface is small enough
 * to fit in one page of code.
 *
 * Receive is unusual and worth understanding before reading further.  There are
 * no descriptors: the card writes every frame end to end into one circular
 * buffer, each preceded by a four-byte header giving its status and length, and
 * the driver walks that buffer with an offset it writes back to the card.  The
 * buffer is allocated 16 KiB larger than the card is told about, because the
 * card is allowed to run past the end of a frame that straddles the wrap rather
 * than splitting it - so the overrun has to be real memory.
 */
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "time.h"
#include "klog.h"
#include "net.h"

/* --------------------------------------------------------------- registers */

#define RTL_IDR0     0x00      /* the card's address, six bytes */
#define RTL_TSD0     0x10      /* transmit status, four of them */
#define RTL_TSAD0    0x20      /* transmit address, four of them */
#define RTL_RBSTART  0x30      /* receive buffer                 */
#define RTL_CMD      0x37
#define RTL_CAPR     0x38      /* where the driver has read to   */
#define RTL_CBR      0x3A
#define RTL_IMR      0x3C
#define RTL_ISR      0x3E
#define RTL_TCR      0x40
#define RTL_RCR      0x44
#define RTL_CONFIG1  0x52
#define RTL_MSR      0x58      /* media status                   */

#define CMD_RESET    (1u << 4)
#define CMD_RX_ENABLE (1u << 3)
#define CMD_TX_ENABLE (1u << 2)
#define CMD_BUFE     (1u << 0)  /* the receive buffer is empty */

#define RCR_AAP      (1u << 0)  /* all packets, whoever they are for */
#define RCR_APM      (1u << 1)  /* physical match                    */
#define RCR_AM       (1u << 2)  /* multicast                         */
#define RCR_AB       (1u << 3)  /* broadcast                         */
#define RCR_WRAP     (1u << 7)  /* do not split a frame at the wrap  */
#define RCR_RBLEN_32K (2u << 11)
#define RCR_MXDMA_UNLIMITED (7u << 8)

#define TSD_OWN      (1u << 13) /* the card has finished with it */
#define TSD_TOK      (1u << 15) /* transmitted without error     */

#define MSR_LINKB    (1u << 2)  /* set means the link is DOWN */
#define MSR_SPEED_10 (1u << 3)

/* The status word in front of each received frame. */
#define RX_OK        (1u << 0)

#define RX_BUFFER_SIZE   (32 * 1024)
#define RX_BUFFER_PAD    (16 * 1024 + 16)   /* room for the card to overrun */
#define TX_SLOTS         4
#define TX_SLOT_SIZE     2048

typedef struct {
    u16 io;                    /* this card is driven through i/o ports */
    netdev_t dev;

    u8 *rx_buf;   u64 rx_phys;
    u8 *tx_buf;   u64 tx_phys;
    u16 rx_offset;
    int tx_slot;
} rtl_t;

static rtl_t card;
static bool present;

/* ---------------------------------------------------------------- port i/o */

static inline u8  rd8(rtl_t *c, u16 o)  { return inb((u16)(c->io + o)); }
static inline u16 rd16(rtl_t *c, u16 o) { return inw((u16)(c->io + o)); }
static inline u32 rd32(rtl_t *c, u16 o) { return inl((u16)(c->io + o)); }
static inline void wr8(rtl_t *c, u16 o, u8 v)   { outb((u16)(c->io + o), v); }
static inline void wr16(rtl_t *c, u16 o, u16 v) { outw((u16)(c->io + o), v); }
static inline void wr32(rtl_t *c, u16 o, u32 v) { outl((u16)(c->io + o), v); }

/* --------------------------------------------------------------- transmit */

static int rtl_transmit(netdev_t *dev, const void *frame, int len) {
    rtl_t *c = dev->ctx;
    if (len <= 0 || len > TX_SLOT_SIZE) return -1;

    int slot = c->tx_slot;
    u16 tsd = (u16)(RTL_TSD0 + slot * 4);

    /* Four slots go round in turn; each is free once the card sets OWN. */
    if (!(rd32(c, tsd) & TSD_OWN)) {
        for (int i = 0; i < 10000; i++) {
            if (rd32(c, tsd) & TSD_OWN) break;
            timer_udelay(10);
        }
        if (!(rd32(c, tsd) & TSD_OWN)) {
            dev->tx_dropped++;
            return -1;
        }
    }

    memcpy(c->tx_buf + (size_t)slot * TX_SLOT_SIZE, frame, (size_t)len);
    wr32(c, (u16)(RTL_TSAD0 + slot * 4),
         (u32)(c->tx_phys + (u64)slot * TX_SLOT_SIZE));

    /* Writing the length clears OWN and starts the transmission.  The early
     * transmit threshold in the upper bits says how much to buffer before
     * putting it on the wire; 256 bytes is the usual choice. */
    wr32(c, tsd, (u32)len | (8u << 16));

    c->tx_slot = (slot + 1) % TX_SLOTS;
    return len;
}

/* ---------------------------------------------------------------- receive */

static void rtl_poll(netdev_t *dev) {
    rtl_t *c = dev->ctx;

    int handled = 0;
    while (!(rd8(c, RTL_CMD) & CMD_BUFE) && handled < 64) {
        u8 *p = c->rx_buf + c->rx_offset;
        u16 status = (u16)(p[0] | (p[1] << 8));
        u16 length = (u16)(p[2] | (p[3] << 8));

        if (!(status & RX_OK) || length < 4 || length > ETH_FRAME_MAX + 4) {
            /* A bad frame means the offset can no longer be trusted; resetting
             * the receiver is the only way back to a known position. */
            dev->rx_errors++;
            wr8(c, RTL_CMD, CMD_TX_ENABLE);
            wr8(c, RTL_CMD, CMD_TX_ENABLE | CMD_RX_ENABLE);
            c->rx_offset = 0;
            wr16(c, RTL_CAPR, (u16)(c->rx_offset - 16));
            return;
        }

        /* The length includes the four-byte checksum the card appends. */
        net_receive(dev, p + 4, length - 4);

        /* Frames start on a four-byte boundary, header included. */
        c->rx_offset = (u16)((c->rx_offset + length + 4 + 3) & ~3u);
        c->rx_offset %= RX_BUFFER_SIZE;

        /* The card's read pointer runs sixteen bytes behind the driver's. */
        wr16(c, RTL_CAPR, (u16)(c->rx_offset - 16));
        handled++;
    }

    /* Acknowledge whatever it was telling us about. */
    u16 isr = rd16(c, RTL_ISR);
    if (isr) wr16(c, RTL_ISR, isr);

    u8 msr = rd8(c, RTL_MSR);
    dev->link_up = !(msr & MSR_LINKB);
    dev->link_speed_mbps = dev->link_up ? ((msr & MSR_SPEED_10) ? 10 : 100) : 0;
}

/* --------------------------------------------------------------- bring-up */

void rtl8139_init(void) {
    if (present) return;

    pci_dev_t *pci = pci_find_id(0x10EC, 0x8139, NULL);
    if (!pci) return;

    rtl_t *c = &card;
    memset(c, 0, sizeof *c);

    /* This card lives in i/o space, unlike almost everything else here. */
    for (int i = 0; i < 6; i++) {
        if (pci->bar[i] && pci->bar_is_io[i]) { c->io = (u16)pci->bar[i]; break; }
    }
    if (!c->io) {
        kwarn("rtl8139", "the card has no i/o window");
        return;
    }

    pci_enable_bus_master(pci);

    /* Power the card on, then reset it and wait for the bit to clear. */
    wr8(c, RTL_CONFIG1, 0x00);
    wr8(c, RTL_CMD, CMD_RESET);
    for (int i = 0; i < 1000; i++) {
        if (!(rd8(c, RTL_CMD) & CMD_RESET)) break;
        timer_mdelay(1);
    }
    if (rd8(c, RTL_CMD) & CMD_RESET) {
        kerr("rtl8139", "the card will not come out of reset");
        return;
    }

    for (int i = 0; i < ETH_ALEN; i++)
        c->dev.mac.addr[i] = rd8(c, (u16)(RTL_IDR0 + i));

    u64 phys;
    c->rx_buf = dma_alloc_pages((RX_BUFFER_SIZE + RX_BUFFER_PAD) / PAGE_SIZE, &phys);
    if (!c->rx_buf) {
        kerr("rtl8139", "out of memory for the receive buffer");
        return;
    }
    c->rx_phys = phys;

    c->tx_buf = dma_alloc_pages((TX_SLOTS * TX_SLOT_SIZE) / PAGE_SIZE, &phys);
    if (!c->tx_buf) {
        kerr("rtl8139", "out of memory for the transmit buffers");
        return;
    }
    c->tx_phys = phys;

    /* This card can only address the low 4 GiB. */
    if (c->rx_phys >> 32 || c->tx_phys >> 32) {
        kwarn("rtl8139", "buffers landed above 4 GiB, which this card cannot reach");
        return;
    }

    wr32(c, RTL_RBSTART, (u32)c->rx_phys);

    /* Accept what is ours plus broadcast, and do not split a frame across the
     * wrap - which is what the padding after the buffer is for. */
    wr32(c, RTL_RCR, RCR_APM | RCR_AB | RCR_AM | RCR_WRAP |
                     RCR_RBLEN_32K | RCR_MXDMA_UNLIMITED);
    wr32(c, RTL_TCR, (6u << 8) | (3u << 24));   /* unlimited DMA, normal mode */

    /* Interrupts stay masked; the network thread polls. */
    wr16(c, RTL_IMR, 0);
    wr16(c, RTL_ISR, 0xFFFF);

    wr8(c, RTL_CMD, CMD_TX_ENABLE | CMD_RX_ENABLE);

    c->rx_offset = 0;
    c->tx_slot = 0;

    snprintf(c->dev.name, sizeof c->dev.name, "eth%d", netdev_count());
    strlcpy(c->dev.model, "Realtek RTL8139 Ethernet", sizeof c->dev.model);
    c->dev.ctx = c;
    c->dev.transmit = rtl_transmit;
    c->dev.poll = rtl_poll;

    rtl_poll(&c->dev);
    netdev_register(&c->dev);
    present = true;
}
