/* r8169_model.c - a Realtek Ethernet card, in software.
 *
 * The driver next door has never been run against one of these.  A model is
 * not the same as hardware and does not pretend to be: what it establishes is
 * that the driver's idea of the card is self-consistent and that the parts
 * which can be checked - the reset handshake, where the rings are handed over,
 * the ownership protocol, what a descriptor means - are done the way the
 * documentation says.
 *
 * It is written to be awkward where the hardware is awkward.  It refuses reads
 * of the station address until the card has been reset, it will not look at a
 * transmit descriptor until the doorbell is rung, it holds the frame it was
 * given rather than the pointer to it, and it appends the four-byte checksum
 * that a real card keeps on a received frame - which is the detail a driver
 * silently gets wrong and then delivers four bytes of rubbish on the end of
 * every packet in the system.
 */
#include "kernel.h"
#include "klog.h"
#include "time.h"
#include "mm.h"
#include "r8169.h"

#define MODEL_REG_BYTES 0x1000

static struct {
    bool present;
    u8  *regs;

    bool reset_done;
    bool tx_enabled, rx_enabled;
    bool config_unlocked;
    bool is_2500;

    u64 rx_ring, tx_ring;
    u32 rx_at, tx_at;

    /* The last frame the driver handed over, copied rather than referenced. */
    u8  sent[2048];
    int sent_len;
    bool sent_valid;

    int complaints;
} model;

static const u8 MODEL_MAC[6] = { 0x00, 0xE0, 0x4C, 0x11, 0x22, 0x33 };

static void complain(const char *what) {
    model.complaints++;
    kwarn("r8169-model", "%s", what);
}

static u8  *reg8 (u32 o) { return model.regs + o; }
static u16 *reg16(u32 o) { return (u16 *)(model.regs + o); }
static u32 *reg32(u32 o) { return (u32 *)(model.regs + o); }

/* Read the descriptor at `index` out of a ring the driver placed in memory the
 * card can reach - which for the model is the direct map. */
static r8169_desc_t *desc_at(u64 ring_phys, u32 index) {
    if (!ring_phys) return NULL;
    r8169_desc_t *ring = phys_to_virt(ring_phys);
    return &ring[index];
}

/* --------------------------------------------------------------- behaviour */

/* Called after every write the driver makes, because a model that only acts
 * when asked cannot catch a driver that never asks. */
static void settle(void) {
    /* The reset bit clears itself once the reset is done, which is what the
     * driver spins on. */
    u8 cmd = *reg8(R8169_CMD);
    if (cmd & CMD_RESET) {
        model.reset_done = true;
        model.tx_enabled = model.rx_enabled = false;
        model.rx_ring = model.tx_ring = 0;
        model.rx_at = model.tx_at = 0;
        *reg8(R8169_CMD) = 0;

        /* The address only becomes readable once the card has been reset; a
         * driver that reads it first gets zeroes on real silicon. */
        for (int i = 0; i < 6; i++) *reg8(R8169_MAC0 + i) = MODEL_MAC[i];
        return;
    }

    model.tx_enabled = (cmd & CMD_TX_ENABLE) != 0;
    model.rx_enabled = (cmd & CMD_RX_ENABLE) != 0;

    model.config_unlocked = (*reg8(R8169_9346CR) == LOCK_CONFIG_UNLOCK);

    model.rx_ring = *(u64 *)(model.regs + R8169_RDSAR);
    model.tx_ring = *(u64 *)(model.regs + R8169_TNPDS);

    /* The doorbell: the driver says there is something to send.  Insist on the
     * register the REAL part uses - the 8125's is at 0x90 (bit 0, 16-bit), the
     * older parts' at 0x38 (bit 6, 8-bit).  Checking the right one for the chip
     * this model is being is what makes the self-test catch a driver that rings
     * the wrong place: on an 8125 the old 0x38 is the interrupt mask, so a card
     * rung there would never send on silicon, and now the model says so too. */
    bool rung;
    if (model.is_2500) {
        rung = (*reg16(R8125_TPPOLL) & TPPOLL_8125_KICK) != 0;
        if (rung) *reg16(R8125_TPPOLL) = 0;
    } else {
        rung = (*reg8(R8169_TPPOLL) & TPPOLL_NPQ) != 0;
        if (rung) *reg8(R8169_TPPOLL) = 0;
    }
    if (rung) {
        if (!model.tx_enabled) { complain("told to send with the transmitter off"); return; }
        if (!model.tx_ring)    { complain("told to send with no transmit ring"); return; }

        r8169_desc_t *d = desc_at(model.tx_ring, model.tx_at);
        if (!d) return;

        if (!(d->flags & DESC_OWN)) {
            complain("the doorbell was rung on a descriptor the card does not own");
            return;
        }
        if (!(d->flags & DESC_FS) || !(d->flags & DESC_LS))
            complain("a whole frame was sent without being marked first and last");

        u32 len = d->flags & DESC_LEN_MASK;
        if (!len || len > sizeof model.sent) {
            complain("a frame of an impossible length was handed over");
        } else {
            memcpy(model.sent, phys_to_virt(d->address), len);
            model.sent_len = (int)len;
            model.sent_valid = true;
        }

        /* Hand it back, keeping the end-of-ring bit where the driver put it. */
        d->flags &= ~DESC_OWN;
        model.tx_at = (model.tx_at + 1) % 32;
    }
}

/* ---------------------------------------------------------------- the trap
 *
 * The model's registers are ordinary memory, so a write lands and nothing
 * happens.  Something has to run afterwards, and the driver is not going to
 * call the model - so every read of the command register, which the driver
 * does constantly while it waits, is the moment the model catches up. */
static u8 last_cmd;

/* The generation the NEXT attach should be, chosen before attach by
 * set_2500.  Held outside `model` so the memset in attach does not wipe it -
 * which is exactly what used to force every attach back to an 8125 and made the
 * gigabit self-test run against an 8125 model. */
static bool want_2500 = true;

bool r8169_model_attach(volatile u8 **regs, size_t *size) {
    memset(&model, 0, sizeof model);

    u64 phys;
    model.regs = dma_alloc_pages(MODEL_REG_BYTES / PAGE_SIZE, &phys);
    if (!model.regs) return false;
    memset(model.regs, 0, MODEL_REG_BYTES);

    model.present = true;
    model.is_2500 = want_2500;           /* the generation set_2500 asked for */

    /* Linked, so the driver's first read of the link state finds one, at the
     * speed the part this model is being would report. */
    *reg16(R8169_PHYSTATUS) = PHY_LINK_OK | (want_2500 ? PHY_2500M : PHY_1000M);

    *regs = model.regs;
    *size = MODEL_REG_BYTES;
    last_cmd = 0;
    return true;
}

void r8169_model_detach(void) {
    if (model.regs) dma_free_pages(model.regs, MODEL_REG_BYTES / PAGE_SIZE);
    memset(&model, 0, sizeof model);
}

bool r8169_model_is_2500(void) { return model.is_2500; }
void r8169_model_set_2500(bool yes) { want_2500 = yes; model.is_2500 = yes; }

/* Hand a frame to the card, as the wire would.  The four-byte checksum a real
 * card keeps on the end is added here, because a driver that forgets to strip
 * it is the failure this is here to catch. */
void r8169_model_receive(const void *frame, int len) {
    if (!model.present) return;
    settle();

    if (!model.rx_enabled) { complain("a frame arrived with the receiver off"); return; }
    if (!model.rx_ring)    { complain("a frame arrived with no receive ring"); return; }

    r8169_desc_t *d = desc_at(model.rx_ring, model.rx_at);
    if (!d) return;
    if (!(d->flags & DESC_OWN)) { complain("no descriptor was free for an arriving frame"); return; }

    u8 *into = phys_to_virt(d->address);
    memcpy(into, frame, (size_t)len);

    /* Four bytes of checksum, whose contents do not matter - only that they
     * are there and counted in the length. */
    into[len + 0] = 0xDE; into[len + 1] = 0xAD;
    into[len + 2] = 0xBE; into[len + 3] = 0xEF;

    u32 eor = d->flags & DESC_EOR;
    d->flags = eor | (u32)(len + 4);        /* ours no longer: OWN cleared */
    model.rx_at = (model.rx_at + 1) % 32;
}

int r8169_model_transmitted(const void **frame, int *len) {
    settle();
    if (!model.sent_valid) return 0;
    *frame = model.sent;
    *len = model.sent_len;
    model.sent_valid = false;
    return 1;
}

/* The driver reads the command register while it waits for the reset, and
 * again on every poll; that is where the model is given a chance to run. */
void r8169_model_tick(void) {
    if (!model.present) return;
    u8 cmd = *reg8(R8169_CMD);
    bool doorbell = model.is_2500 ? (*reg16(R8125_TPPOLL) & TPPOLL_8125_KICK)
                                  : (*reg8(R8169_TPPOLL) & TPPOLL_NPQ);
    if (cmd != last_cmd || doorbell) {
        settle();
        last_cmd = *reg8(R8169_CMD);
    }
}
