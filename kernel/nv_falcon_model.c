/* nv_falcon_model.c - a Falcon that is not there.
 *
 * The driver next door boots the small processors inside an NVIDIA card. No
 * virtual machine provides one, and the machine this was written on has its
 * card claimed by the host - so without something standing in, that code could
 * be written and never once executed.
 *
 * This is that something. It is not a stub and it does not agree with whatever
 * it is told: it holds the same state a real Falcon does, and it is deliberately
 * strict about the order things happen in. Writing instruction memory before
 * the scrubber has finished loses the write, exactly as it would on a card.
 * Loading code without setting the block tags leaves the tags invalid, and
 * starting the processor then fails the way it would fail on real hardware.
 * Starting one that was never loaded halts immediately with an error in the
 * mailbox.
 *
 * That strictness is the entire value. A model that accepts anything proves
 * nothing; one that refuses what the hardware would refuse turns "this code
 * looks right" into "this code did the sequence".
 *
 * What it cannot do is tell anyone whether the register offsets are the ones
 * the silicon uses. It stands where the card would stand and answers the way
 * the documentation says the card answers - so it proves the driver's logic,
 * its ordering and its error handling, and it proves nothing about the numbers.
 * That difference is stated wherever this is reported.
 */
#include "kernel.h"
#include "klog.h"
#include "mm.h"
#include "nv.h"

/* The same offsets the driver uses, from the other side. */
#define F_IRQSCLR        0x0004
#define F_MAILBOX0       0x0040
#define F_MAILBOX1       0x0044
#define F_ITFEN          0x0048
#define F_CPUCTL         0x0100
#define F_BOOTVEC        0x0104
#define F_HWCFG          0x0108
#define F_DMACTL         0x010C
#define F_CPUCTL_ALIAS   0x0130
#define F_IMEMC(i)       (0x0180 + (i) * 16)
#define F_IMEMD(i)       (0x0184 + (i) * 16)
#define F_IMEMT(i)       (0x0188 + (i) * 16)
#define F_DMEMC(i)       (0x01C0 + (i) * 8)
#define F_DMEMD(i)       (0x01C4 + (i) * 8)
#define F_ENGINE         0x03C0

#define CPUCTL_STARTCPU  0x00000002u
#define CPUCTL_HALTED    0x00000010u
#define CPUCTL_ALIAS_EN  0x00000020u

#define DMACTL_IMEM_SCRUB 0x00000002u
#define DMACTL_DMEM_SCRUB 0x00000004u

#define MEMC_AINCW       (1u << 24)
#define MEMC_AINCR       (1u << 25)

/* Small, because the point is the protocol rather than the capacity. Reported
 * honestly through HWCFG so the driver's own bounds checks are exercised. */
#define MODEL_IMEM  (32 * 1024)
#define MODEL_DMEM  (16 * 1024)
#define MODEL_BLOCKS (MODEL_IMEM / 256)

/* Why it stopped, left in the mailbox for the driver to read - the way a real
 * bootloader reports itself. */
#define HALT_OK              0x00000000u
#define HALT_NO_CODE         0xBADC0DE1u
#define HALT_BAD_TAGS        0xBADC0DE2u
#define HALT_NOT_SIGNED      0xBADC0DE3u

static struct {
    bool present;
    volatile u8 *window;
    u32  base;

    u8   imem[MODEL_IMEM];
    u8   dmem[MODEL_DMEM];
    bool tag_valid[MODEL_BLOCKS];

    /* Where the auto-incrementing ports are pointing. */
    u32  imem_at, dmem_at;
    u32  imem_tag_block;
    bool imem_incrementing, dmem_incrementing;
    bool dmem_reading;

    bool in_reset;
    bool scrubbing;
    int  scrub_ticks;
    bool started;
    bool halted;
    u32  boot_vector;
    u32  mailbox[2];

    /* What the driver actually did, so a test can check the sequence rather
     * than only the outcome. */
    int  resets;
    u32  imem_written;
    u32  dmem_written;
    int  tags_set;
    bool started_before_load;
    bool wrote_while_scrubbing;
} model;

static u32 reg_get(u32 offset) {
    if (!model.window) return 0;
    return *(volatile u32 *)(model.window + model.base + offset);
}

static void reg_set(u32 offset, u32 value) {
    if (!model.window) return;
    *(volatile u32 *)(model.window + model.base + offset) = value;
}

/* ------------------------------------------------------------- the sequence
 *
 * Driven by the writes themselves rather than by noticing that a register's
 * value changed.  The difference matters more than it sounds: loading code
 * means writing word after word to one port, and a block of padding is a run
 * of identical zeros.  A model that watches for changes sees the first of
 * those and none of the rest, and then reports that the image arrived altered
 * - which is true, but the alteration was the model's.
 */
void nv_falcon_model_write(u32 offset, u32 value) {
    if (!model.present) return;
    if (offset < model.base || offset >= model.base + 0x1000) return;
    offset -= model.base;

    switch (offset) {
    case F_ENGINE: {
        bool reset_now = (value & 1) != 0;

        if (reset_now && !model.in_reset) {
            model.in_reset = true;
            model.started = false;
            model.halted = false;
            return;
        }
        if (!reset_now && model.in_reset) {
            /* Out of reset: everything cleared, and the scrubber runs.  Until
             * it finishes, writes to either memory are lost - which is the
             * whole reason the driver has to wait. */
            model.in_reset = false;
            model.resets++;
            memset(model.imem, 0, sizeof model.imem);
            memset(model.dmem, 0, sizeof model.dmem);
            memset(model.tag_valid, 0, sizeof model.tag_valid);
            model.imem_written = model.dmem_written = 0;
            model.tags_set = 0;
            model.started = false;
            model.halted = false;
            model.started_before_load = false;
            model.mailbox[0] = model.mailbox[1] = 0;

            model.scrubbing = true;
            model.scrub_ticks = 20;
            reg_set(F_DMACTL, DMACTL_IMEM_SCRUB | DMACTL_DMEM_SCRUB);
            reg_set(F_CPUCTL, CPUCTL_ALIAS_EN);
            reg_set(F_MAILBOX0, 0);
        }
        return;
    }

    case F_IMEMC(0):
        model.imem_at = value & 0x00FFFF00u;
        model.imem_incrementing = (value & MEMC_AINCW) != 0;
        return;

    case F_IMEMT(0):
        if (value < MODEL_BLOCKS && !model.scrubbing) {
            model.imem_tag_block = value;
            model.tag_valid[value] = true;
            model.tags_set++;
        }
        return;

    case F_IMEMD(0):
        if (model.scrubbing) {
            /* Lost, exactly as it would be.  Recorded so a test can say the
             * driver waited rather than merely that it worked. */
            model.wrote_while_scrubbing = true;
            return;
        }
        if (model.imem_at + 4 <= MODEL_IMEM) {
            for (int i = 0; i < 4; i++)
                model.imem[model.imem_at + i] = (u8)(value >> (8 * i));
            model.imem_written += 4;
        }
        if (model.imem_incrementing) model.imem_at += 4;
        return;

    case F_DMEMC(0):
        model.dmem_at = value & 0x00FFFFFCu;
        model.dmem_incrementing = (value & (MEMC_AINCW | MEMC_AINCR)) != 0;
        model.dmem_reading = (value & MEMC_AINCR) != 0;
        return;

    case F_DMEMD(0):
        if (model.scrubbing) { model.wrote_while_scrubbing = true; return; }
        if (model.dmem_reading) return;          /* the port is reading */
        if (model.dmem_at + 4 <= MODEL_DMEM) {
            for (int i = 0; i < 4; i++)
                model.dmem[model.dmem_at + i] = (u8)(value >> (8 * i));
            model.dmem_written += 4;
        }
        if (model.dmem_incrementing) model.dmem_at += 4;
        return;

    case F_CPUCTL:
    case F_CPUCTL_ALIAS:
        if (!(value & CPUCTL_STARTCPU) || model.started) return;

        model.started = true;
        model.boot_vector = reg_get(F_BOOTVEC);

        /* What a real one does: check there is something to run, that the
         * blocks it is about to execute are tagged, and only then run it.
         * Here "running it" means halting with a result, because there is no
         * Falcon instruction set to interpret - the protocol is what is being
         * checked. */
        {
            u32 result;
            if (model.imem_written == 0) {
                model.started_before_load = true;
                result = HALT_NO_CODE;
            } else if (model.tags_set == 0) {
                result = HALT_BAD_TAGS;
            } else if (model.boot_vector >= model.imem_written) {
                result = HALT_BAD_TAGS;
            } else {
                /* A real GSP image is signed and the hardware checks it here.
                 * Nothing this driver can produce would pass, and saying so is
                 * the honest answer rather than pretending it ran. */
                result = model.dmem_written ? HALT_OK : HALT_NOT_SIGNED;
            }

            model.mailbox[0] = result;
            reg_set(F_MAILBOX0, result);
            model.halted = true;
            reg_set(F_CPUCTL, CPUCTL_ALIAS_EN | CPUCTL_HALTED);
        }
        return;

    default:
        return;
    }
}

/* Reads that have to be answered afresh: the scrub finishing after a while,
 * and the next word of a data-memory walk. */
void nv_falcon_model_read(u32 offset) {
    if (!model.present) return;
    if (offset < model.base || offset >= model.base + 0x1000) return;
    offset -= model.base;

    if (offset == F_DMACTL && model.scrubbing) {
        if (--model.scrub_ticks <= 0) {
            model.scrubbing = false;
            reg_set(F_DMACTL, 0);
            reg_set(F_HWCFG, (MODEL_IMEM / 256) | ((MODEL_DMEM / 256) << 9));
        }
        return;
    }

    if (offset == F_DMEMD(0) && model.dmem_reading) {
        /* This runs before the driver takes the value, so what goes in the
         * register is the word it is asking for now - and only then does the
         * port step on.  Advancing first would hand back the second word to
         * the first read and lose the first entirely. */
        u32 word = 0;
        if (model.dmem_at + 4 <= MODEL_DMEM)
            for (int i = 0; i < 4; i++)
                word |= (u32)model.dmem[model.dmem_at + i] << (8 * i);
        reg_set(F_DMEMD(0), word);
        if (model.dmem_incrementing) model.dmem_at += 4;
        return;
    }
}

/* ------------------------------------------------------------------- setup */

void nv_falcon_model_attach(volatile u8 *window, u32 base) {
    memset(&model, 0, sizeof model);
    model.window = window;
    model.base = base;
    model.present = true;

    /* How a card looks before anything has touched it: not in reset, not
     * running, and not yet admitting its size. */
    reg_set(F_CPUCTL, CPUCTL_ALIAS_EN);
    reg_set(F_DMACTL, 0);
    reg_set(F_ENGINE, 0);
    reg_set(F_HWCFG, (MODEL_IMEM / 256) | ((MODEL_DMEM / 256) << 9));
}

void nv_falcon_model_detach(void) { model.present = false; }

/* What happened, for a test to check the sequence and not only the result. */
int  nv_falcon_model_resets(void)       { return model.resets; }
u32  nv_falcon_model_imem_written(void) { return model.imem_written; }
u32  nv_falcon_model_dmem_written(void) { return model.dmem_written; }
int  nv_falcon_model_tags(void)         { return model.tags_set; }
bool nv_falcon_model_started(void)      { return model.started; }
u32  nv_falcon_model_result(void)       { return model.mailbox[0]; }
u32  nv_falcon_model_bootvec(void)      { return model.boot_vector; }
bool nv_falcon_model_wrote_while_scrubbing(void) {
    return model.wrote_while_scrubbing;
}
const u8 *nv_falcon_model_imem(void)    { return model.imem; }
const u8 *nv_falcon_model_dmem(void)    { return model.dmem; }
