/* ath9k.c - Atheros AR9xxx.
 *
 * The one Wi-Fi family that needs no vendor firmware: everything the radio does
 * is driven by registers, so this driver can work with nothing supplied by the
 * user.  That is why it is the one written out in full.
 *
 * Both rings are chains of descriptors in ordinary memory that the hardware
 * walks by itself.  Transmit: fill a descriptor, point the queue at it, set the
 * queue's enable bit, and watch the done bit come back.  Receive: hand over a
 * chain of empty buffers, point the receive pointer at the first, and walk
 * forward over whatever has been filled in, handing each descriptor back as it
 * is consumed.  Both chains are circular - the last descriptor links to the
 * first - so the hardware never runs off the end.
 *
 * Nothing here takes an interrupt.  The status word in each descriptor says
 * when the hardware has finished with it, and polling that costs less at these
 * rates than an interrupt would, while keeping the receive path out of
 * interrupt context.
 *
 * ---------------------------------------------------------------------------
 * On testing.  This has never run against an Atheros card, because nothing
 * available to test on emulates one.  What it has run against is a model of the
 * hardware that answers these registers and walks these descriptor chains, so
 * the ring management, the descriptor formats, the DMA addressing, the state
 * machine and the handover to the 802.11 layer are all exercised.  What that
 * cannot check is whether these register offsets match real silicon, and
 * whether the synthesiser sequence tunes a real radio.  Those two remain
 * unverified and are called out where they occur.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "time.h"
#include "klog.h"
#include "firmware.h"
#include "wifi.h"
#include "ath9k.h"

void ieee80211_receive(wifi_device_t *dev, u8 *frame, int len, s8 signal_dbm);
void ieee80211_attach(wifi_device_t *dev);

/* The model, when one is standing in for hardware. */
bool ath_model_attach(u32 *span, volatile u8 **regs_out);
void ath_model_tick(void);
void ath_model_sync(void);

#define RX_RING     32
#define TX_RING     16
#define BUFFER_SIZE 2048
#define TX_QUEUE    0            /* one queue; see the note in ath9k.h */

typedef struct {
    volatile u8 *regs;
    wifi_device_t dev;

    ath_desc_t *rx;      u64 rx_phys;
    ath_desc_t *tx;      u64 tx_phys;
    u8         *rx_buf;  u64 rx_buf_phys;
    u8         *tx_buf;  u64 tx_buf_phys;

    u32  rx_next;        /* the descriptor to look at next        */
    u32  tx_next;        /* the descriptor to fill next           */
    bool modelled;       /* running against the model, not silicon */
} ath_t;

#define MAX_CARDS 2
static ath_t cards[MAX_CARDS];
static int   card_count;

static inline u32 rd(ath_t *c, u32 off) {
    /* Reading is when the driver observes the device.  Against a model that is
     * also when the model has to catch up with whatever was written since. */
    if (c->modelled) ath_model_sync();
    return *(volatile u32 *)(c->regs + off);
}
static inline void wr(ath_t *c, u32 off, u32 v) {
    *(volatile u32 *)(c->regs + off) = v;
}

/* ----------------------------------------------------------------- EEPROM */

/* One word out of the card's own memory.  Everything board-specific lives
 * there: the address, the regulatory domain, and the calibration the radio
 * needs to hit a frequency accurately. */
static bool eeprom_read(ath_t *c, u32 address, u16 *out) {
    wr(c, AR_EEPROM_ADDR, address);
    wr(c, AR_EEPROM_CMD, AR_EEPROM_CMD_READ);

    for (int i = 0; i < 1000; i++) {
        u32 status = rd(c, AR_EEPROM_STATUS);
        if (status & AR_EEPROM_STATUS_FAIL) return false;
        if (status & AR_EEPROM_STATUS_DONE) {
            *out = (u16)rd(c, AR_EEPROM_DATA);
            return true;
        }
        timer_udelay(10);
    }
    return false;
}

/* The address is three words, most significant first. */
static bool read_mac(ath_t *c) {
    u16 words[3];
    for (int i = 0; i < 3; i++)
        if (!eeprom_read(c, AR_EEPROM_MAC_WORD + (u32)i, &words[i]))
            return false;

    for (int i = 0; i < 3; i++) {
        c->dev.mac.addr[i * 2 + 0] = (u8)(words[i] >> 8);
        c->dev.mac.addr[i * 2 + 1] = (u8)(words[i] & 0xFF);
    }

    /* All zeroes or all ones means the EEPROM did not really answer. */
    bool all_zero = true, all_ones = true;
    for (int i = 0; i < ETH_ALEN; i++) {
        if (c->dev.mac.addr[i] != 0x00) all_zero = false;
        if (c->dev.mac.addr[i] != 0xFF) all_ones = false;
    }
    return !all_zero && !all_ones;
}

/* ------------------------------------------------------------------ rings */

/* Link every descriptor to the next and the last back to the first, so the
 * hardware walks the chain forever without being told where it ends. */
static void chain(ath_desc_t *ring, u64 phys, int count) {
    for (int i = 0; i < count; i++) {
        int next = (i + 1) % count;
        ring[i].link = (u32)(phys + (u64)next * sizeof(ath_desc_t));
    }
}

static void arm_rx_descriptor(ath_t *c, u32 index) {
    ath_desc_t *d = &c->rx[index];
    d->buffer = (u32)(c->rx_buf_phys + (u64)index * BUFFER_SIZE);
    d->control0 = 0;
    d->control1 = BUFFER_SIZE & ATH_RXC1_BUF_LEN;
    d->status0 = 0;                 /* clearing done gives it back */
    d->status1 = 0;
}

static bool setup_rings(ath_t *c) {
    u64 phys;

    c->rx = dma_alloc_pages((RX_RING * sizeof(ath_desc_t) + PAGE_SIZE - 1) / PAGE_SIZE,
                            &phys);
    if (!c->rx) return false;
    c->rx_phys = phys;

    c->tx = dma_alloc_pages((TX_RING * sizeof(ath_desc_t) + PAGE_SIZE - 1) / PAGE_SIZE,
                            &phys);
    if (!c->tx) return false;
    c->tx_phys = phys;

    c->rx_buf = dma_alloc_pages((RX_RING * BUFFER_SIZE) / PAGE_SIZE, &phys);
    if (!c->rx_buf) return false;
    c->rx_buf_phys = phys;

    c->tx_buf = dma_alloc_pages((TX_RING * BUFFER_SIZE) / PAGE_SIZE, &phys);
    if (!c->tx_buf) return false;
    c->tx_buf_phys = phys;

    /* This family addresses descriptors and buffers with 32 bits. */
    if ((c->rx_phys | c->tx_phys | c->rx_buf_phys | c->tx_buf_phys) >> 32) {
        kerr("ath9k", "the rings landed above 4 GiB, which this card cannot reach");
        return false;
    }

    chain(c->rx, c->rx_phys, RX_RING);
    chain(c->tx, c->tx_phys, TX_RING);

    for (u32 i = 0; i < RX_RING; i++) arm_rx_descriptor(c, i);
    for (int i = 0; i < TX_RING; i++) {
        c->tx[i].buffer = 0;
        c->tx[i].control0 = c->tx[i].control1 = 0;
        c->tx[i].status0 = ATH_TXS0_DONE;      /* free */
        c->tx[i].status1 = 0;
    }

    c->rx_next = 0;
    c->tx_next = 0;
    return true;
}

/* --------------------------------------------------------------- bring-up */

static bool reset_mac(ath_t *c) {
    /* Warm reset: the host interface and the bus block, then the MAC.  The
     * order matters - resetting the MAC while the bus block is held leaves the
     * card unable to answer. */
    wr(c, AR_RC, AR_RC_AHB | AR_RC_HOSTIF);
    timer_udelay(50);
    wr(c, AR_RC, AR_RC_AHB);
    timer_udelay(50);
    wr(c, AR_RC, 0);
    timer_udelay(50);

    u32 revision = rd(c, AR_SREV);
    if (revision == 0xFFFFFFFF || revision == 0) return false;
    return true;
}

static void program_filters(ath_t *c) {
    wifi_device_t *dev = &c->dev;

    u32 low = (u32)dev->mac.addr[0] | ((u32)dev->mac.addr[1] << 8) |
              ((u32)dev->mac.addr[2] << 16) | ((u32)dev->mac.addr[3] << 24);
    u32 high = (u32)dev->mac.addr[4] | ((u32)dev->mac.addr[5] << 8);
    wr(c, AR_STA_ID0, low);
    wr(c, AR_STA_ID1, high);

    /* Everything addressed to us, plus broadcasts and beacons - beacons being
     * how a scan finds anything at all. */
    wr(c, AR_RX_FILTER, AR_RX_FILTER_UCAST | AR_RX_FILTER_BCAST |
                        AR_RX_FILTER_MCAST | AR_RX_FILTER_BEACON);
    wr(c, AR_MCAST_FIL0, 0xFFFFFFFF);
    wr(c, AR_MCAST_FIL1, 0xFFFFFFFF);
}

static void set_bssid(ath_t *c, const mac_t *bssid) {
    u32 low = (u32)bssid->addr[0] | ((u32)bssid->addr[1] << 8) |
              ((u32)bssid->addr[2] << 16) | ((u32)bssid->addr[3] << 24);
    u32 high = (u32)bssid->addr[4] | ((u32)bssid->addr[5] << 8);
    wr(c, AR_BSS_ID0, low);
    wr(c, AR_BSS_ID1, high);
}

static bool ath_start(wifi_device_t *dev) {
    ath_t *c = dev->ctx;

    if (!reset_mac(c)) {
        kwarn("ath9k", "%s: the card is not answering after reset", dev->name);
        return false;
    }

    u32 revision = rd(c, AR_SREV);

    if (!read_mac(c)) {
        kwarn("ath9k", "%s: could not read the address out of the EEPROM",
              dev->name);
        return false;
    }

    if (!c->rx && !setup_rings(c)) {
        kerr("ath9k", "%s: out of memory setting up the rings", dev->name);
        return false;
    }

    program_filters(c);

    /* Interrupts stay masked; the descriptors say what has happened. */
    wr(c, AR_IER, 0);
    wr(c, AR_IMR, 0);

    /* Hand the receive chain over and start it. */
    wr(c, AR_RXDP, (u32)c->rx_phys);
    wr(c, AR_DIAG_SW, rd(c, AR_DIAG_SW) & ~AR_DIAG_RX_DIS);
    wr(c, AR_CR, AR_CR_RXE);

    /* Turn the radio on.  Everything up to here is the digital side and is
     * exercised by the model; this write and the synthesiser below are the
     * parts only real silicon can answer. */
    wr(c, AR_PHY_ACTIVE, AR_PHY_ACTIVE_EN);

    dev->radio_up = true;

    kinfo("ath9k", "%s: up, silicon revision %u.%u%s", dev->name,
          (revision >> AR_SREV_VERSION_S) & 0xFF, revision & 0xF,
          c->modelled ? " (model)" : "");
    if (!c->modelled)
        kwarn("ath9k", "%s: this driver has not been run against an Atheros "
                       "card; the register offsets are unverified", dev->name);
    return true;
}

static void ath_stop(wifi_device_t *dev) {
    ath_t *c = dev->ctx;
    wr(c, AR_CR, AR_CR_RXD);
    wr(c, AR_Q_TXD, 0xFFFF);                    /* stop every queue */
    wr(c, AR_PHY_ACTIVE, 0);
    dev->radio_up = false;
}

/* ---------------------------------------------------------------- channel */

static bool ath_set_channel(wifi_device_t *dev, u8 channel) {
    ath_t *c = dev->ctx;
    if (channel < 1 || channel > 14) return false;

    /* Channel one is 2412 MHz and they are five apart, except fourteen. */
    u32 mhz = (channel == 14) ? 2484u : (2412u + (u32)(channel - 1) * 5u);

    /* Take the radio bus, program the synthesiser, give it back.  The card
     * must not be transmitting while the frequency moves, which is what the
     * request and grant handshake is for. */
    wr(c, AR_PHY_RFBUS_REQ, 1);
    for (int i = 0; i < 100; i++) {
        if (rd(c, AR_PHY_RFBUS_GRANT) & 1) break;
        timer_udelay(5);
    }

    /* The 2.4 GHz band is programmed as an offset above 2300 MHz in quarter
     * megahertz, with the top bit selecting that band.  This encoding is from
     * the AR9280 documentation and is one of the two things in this driver
     * that only a real radio can confirm. */
    u32 synth = ((mhz - 2300u) * 10u) / 25u;
    synth = (synth << 17) | (1u << 29) | (1u << 28);
    wr(c, AR_PHY_SYNTH_CONTROL, synth);

    /* Give the loop time to settle before anything is sent. */
    timer_udelay(1000);
    wr(c, AR_PHY_RFBUS_REQ, 0);

    dev->channel = channel;
    return true;
}

/* --------------------------------------------------------------- transmit */

static int ath_transmit(wifi_device_t *dev, const void *frame, int len) {
    ath_t *c = dev->ctx;
    if (len <= 0 || len > BUFFER_SIZE) return -1;
    if (!c->rx) return -1;

    u32 slot = c->tx_next;
    ath_desc_t *d = &c->tx[slot];

    /* Wait for the hardware to be finished with this descriptor.  With this
     * many and one frame in flight it is almost never actually busy. */
    if (!(d->status0 & ATH_TXS0_DONE)) {
        for (int i = 0; i < 5000; i++) {
            if (c->modelled) ath_model_sync();
            if (d->status0 & ATH_TXS0_DONE) break;
            timer_udelay(10);
        }
        if (!(d->status0 & ATH_TXS0_DONE)) {
            dev->net.tx_dropped++;
            return -1;
        }
    }

    memcpy(c->tx_buf + (size_t)slot * BUFFER_SIZE, frame, (size_t)len);

    d->buffer = (u32)(c->tx_buf_phys + (u64)slot * BUFFER_SIZE);
    d->control0 = ((u32)len & ATH_TXC0_LENGTH) | ATH_TXC0_TX_INTERRUPT;
    d->control1 = (u32)len & ATH_TXC1_BUF_LEN;     /* the whole frame, no more */
    d->status0 = 0;                                 /* hand it over */
    d->status1 = 0;

    __asm__ volatile("" ::: "memory");

    /* Point the queue at this descriptor and let it go. */
    wr(c, AR_QTXDP(TX_QUEUE), (u32)(c->tx_phys + (u64)slot * sizeof(ath_desc_t)));
    wr(c, AR_Q_TXE, 1u << TX_QUEUE);

    c->tx_next = (slot + 1) % TX_RING;
    dev->net.tx_packets++;
    dev->net.tx_bytes += (u64)len;
    return len;
}

/* ---------------------------------------------------------------- receive */

static void ath_poll(wifi_device_t *dev) {
    ath_t *c = dev->ctx;
    if (!c->rx) return;

    /* A model, if that is what is behind the registers, needs a chance to do
     * whatever it was going to do before the descriptors are looked at. */
    if (c->modelled) ath_model_tick();

    int handled = 0;
    while (handled < RX_RING) {
        ath_desc_t *d = &c->rx[c->rx_next];
        if (!(d->status0 & ATH_RXS0_DONE)) break;

        u32 status = d->status0;
        int len = (int)((status & ATH_RXS0_LENGTH) >> ATH_RXS0_LENGTH_S);

        if ((status & ATH_RXS0_OK) && !(status & ATH_RXS0_CRC_ERROR) &&
            len > 0 && len <= BUFFER_SIZE) {
            /* The strength is reported above the noise floor, which sits at
             * about -95 dBm; the 802.11 layer wants the absolute value. */
            s8 rssi = (s8)(-95 + (int)(d->status1 & ATH_RXS1_RSSI));

            dev->net.rx_packets++;
            dev->net.rx_bytes += (u64)len;
            ieee80211_receive(dev, c->rx_buf + (size_t)c->rx_next * BUFFER_SIZE,
                              len, rssi);
        } else if (status & (ATH_RXS0_CRC_ERROR | ATH_RXS0_DECRYPT_ERROR)) {
            dev->net.rx_errors++;
        }

        /* Give the descriptor back and move on. */
        arm_rx_descriptor(c, c->rx_next);
        c->rx_next = (c->rx_next + 1) % RX_RING;
        handled++;
    }

    /* If the hardware ran out of descriptors while we were away, it will have
     * stopped; pointing it at the current one starts it again. */
    if (handled == RX_RING) {
        wr(c, AR_RXDP,
           (u32)(c->rx_phys + (u64)c->rx_next * sizeof(ath_desc_t)));
        wr(c, AR_CR, AR_CR_RXE);
    }
}

/* --------------------------------------------------------------- the key */

/* This family can decrypt in hardware through a key cache, but the software
 * path is already written, tested against its published vectors, and shared
 * with every other driver.  Returning false leaves frames to it. */
static bool ath_set_key(wifi_device_t *dev, const u8 *key, int len,
                        bool pairwise) {
    (void)dev; (void)key; (void)len; (void)pairwise;
    return false;
}

static const wifi_driver_t ath_driver = {
    "ath9k", ath_start, ath_stop, ath_set_channel,
    ath_transmit, ath_poll, ath_set_key,
};

/* Exposed so the 802.11 layer can tell the card which network it has joined;
 * without this the hardware filters out everything the access point sends. */
void ath9k_set_bssid(wifi_device_t *dev, const mac_t *bssid) {
    if (!dev || dev->driver != &ath_driver) return;
    set_bssid(dev->ctx, bssid);
}

/* ------------------------------------------------------------------- probe */

static void bring_up_card(ath_t *c, volatile u8 *regs, u16 vendor, u16 device,
                          bool modelled) {
    memset(&c->dev, 0, sizeof c->dev);
    c->regs = regs;
    c->modelled = modelled;

    wifi_device_t *dev = &c->dev;
    if (!wifi_identify(vendor, device, dev)) {
        strlcpy(dev->model, "Atheros wireless", sizeof dev->model);
        dev->vendor = WIFI_VENDOR_ATHEROS;
    }

    if (modelled)
        strlcpy(dev->model, "Atheros AR9280 (hardware model)", sizeof dev->model);

    snprintf(dev->name, sizeof dev->name, "wlan%d", wifi_count());
    dev->driver = &ath_driver;
    dev->ctx = c;
    dev->firmware_needed = false;
    dev->firmware_present = true;

    if (!ath_start(dev)) return;

    wifi_register(dev);
    ieee80211_attach(dev);
    card_count++;
}

void ath9k_init(void) {
    pci_dev_t *pci = NULL;

    while ((pci = pci_find(0x02, 0x80, 0xFF, pci)) != NULL) {
        if (pci->vendor != 0x168C) continue;
        if (card_count >= MAX_CARDS) break;

        ath_t *c = &cards[card_count];
        memset(c, 0, sizeof *c);

        /* The QCA parts under this vendor id need firmware and are a different
         * driver; this one is for the register-driven family. */
        wifi_device_t probe;
        memset(&probe, 0, sizeof probe);
        if (wifi_identify(pci->vendor, pci->device, &probe) &&
            probe.firmware_needed) {
            firmware_declare(probe.firmware_name, "the linux-firmware package");
            kwarn("ath9k", "%s needs %s and a different driver",
                  probe.model, probe.firmware_name);
            continue;
        }

        if (!pci->bar[0] || pci->bar_is_io[0]) continue;
        pci_enable_memory(pci);
        pci_enable_bus_master(pci);

        size_t len = pci->bar_size[0] ? (size_t)pci->bar_size[0] : 0x20000;
        if (len > 0x100000) len = 0x100000;
        volatile u8 *regs = vmm_map_mmio(pci->bar[0], len);
        if (!regs) continue;

        int before = card_count;
        bring_up_card(c, regs, pci->vendor, pci->device, false);
        if (card_count > before) pci_claim(pci, "ath9k");
    }

}

/* Bring the model up in place of a card.  Only ever called by the self-test, so
 * an ordinary boot never grows an interface that is not real - and a machine
 * with a genuine Atheros card uses that instead. */
bool ath9k_attach_model(void) {
    if (card_count >= MAX_CARDS) return false;

    u32 span = 0;
    volatile u8 *regs = NULL;
    if (!ath_model_attach(&span, &regs) || !regs) return false;

    int before = card_count;
    bring_up_card(&cards[card_count], regs, 0x168C, 0x0029, true);
    return card_count > before;
}

/* Whether a real card was found, as opposed to the model standing in. */
bool ath9k_is_modelled(void) {
    return card_count > 0 && cards[0].modelled;
}
