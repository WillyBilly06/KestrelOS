/* nv_falcon.c - starting the small processors inside an NVIDIA card.
 *
 * A modern NVIDIA card is not one processor but a dozen. Scattered around it
 * are small microcontrollers - NVIDIA calls them Falcons - each responsible
 * for one thing: the memory controller's scrubber, the video decoder, the
 * security engine, and from Turing onward the GSP, which runs the whole of
 * what used to be the driver.
 *
 * They all boot the same way, and that sequence is what this file is. It is
 * the foundation everything else on a modern card is built on, because until a
 * Falcon is running there is nothing to talk to.
 *
 * The sequence, in order, and every step of it matters:
 *
 *   1. Hold the engine in reset, then release it. A Falcon that was left
 *      running by the firmware will otherwise be executing something else
 *      while its memory is rewritten underneath it.
 *   2. Wait for its memory scrubber to finish. On a cold card the instruction
 *      and data memories come up full of whatever was there, and the hardware
 *      clears them; writing during that loses the writes.
 *   3. Load the code, in two hundred and fifty-six byte blocks, through a port
 *      that auto-increments. Each block carries the address it belongs at,
 *      because instruction memory is tagged - the Falcon uses those tags to
 *      know which blocks are valid.
 *   4. Load the data the same way, through a different port, untagged.
 *   5. Point it at where to start, and let it go.
 *   6. Wait for it to halt, and read what it left behind.
 *
 * None of this needs a signature. The signature check happens inside what is
 * loaded, not in the loading - which is why this part can be written and
 * exercised, and why the part above it cannot be finished without NVIDIA's
 * own signed image.
 */
#include "kernel.h"
#include "klog.h"
#include "time.h"
#include "nv.h"

/* ------------------------------------------------------------- the registers
 *
 * Every Falcon has the same register block at a different base. The offsets
 * within it have been the same since the first one.
 */
#define FALCON_IRQSCLR        0x0004
#define FALCON_IRQSTAT        0x0008
#define FALCON_IRQMASK        0x0018
#define FALCON_IRQDEST        0x001C
#define FALCON_SCRATCH0       0x0040
#define FALCON_ITFEN          0x0048
#define FALCON_IDLESTATE      0x004C
#define FALCON_CURCTX         0x0050
#define FALCON_NXTCTX         0x0054
#define FALCON_MAILBOX0       0x0040
#define FALCON_MAILBOX1       0x0044
#define FALCON_CPUCTL         0x0100
#define FALCON_BOOTVEC        0x0104
#define FALCON_HWCFG          0x0108
#define FALCON_DMACTL         0x010C
#define FALCON_DMATRFBASE     0x0110
#define FALCON_DMATRFMOFFS    0x0114
#define FALCON_DMATRFCMD      0x0118
#define FALCON_DMATRFFBOFFS   0x011C
#define FALCON_HWCFG1         0x012C
#define FALCON_CPUCTL_ALIAS   0x0130
#define FALCON_IMEMC(i)       (0x0180 + (i) * 16)
#define FALCON_IMEMD(i)       (0x0184 + (i) * 16)
#define FALCON_IMEMT(i)       (0x0188 + (i) * 16)
#define FALCON_DMEMC(i)       (0x01C0 + (i) * 8)
#define FALCON_DMEMD(i)       (0x01C4 + (i) * 8)
#define FALCON_ENGINE         0x03C0

#define CPUCTL_STARTCPU       0x00000002u
#define CPUCTL_HALTED         0x00000010u
#define CPUCTL_ALIAS_EN       0x00000020u

#define DMACTL_IMEM_SCRUBBING 0x00000002u
#define DMACTL_DMEM_SCRUBBING 0x00000004u

#define ENGINE_RESET          0x00000001u

/* The port that loads memory auto-increments after each word, which is what
 * makes loading a block a single set-up and then a run of writes. */
#define MEMC_OFFS_SHIFT       2
#define MEMC_BLK_SHIFT        8
#define MEMC_AINCW            (1u << 24)
#define MEMC_AINCR            (1u << 25)
#define MEMC_SECURE           (1u << 28)

/* How big the two memories are, in two hundred and fifty-six byte blocks. */
#define HWCFG_IMEM_SIZE(v)    (((v) >> 0) & 0x1FF)
#define HWCFG_DMEM_SIZE(v)    (((v) >> 9) & 0x1FF)

static u32 falcon_rd(nv_card_t *c, const nv_falcon_t *f, u32 offset) {
    return nv_rd32(c, f->base + offset);
}

static void falcon_wr(nv_card_t *c, const nv_falcon_t *f, u32 offset, u32 value) {
    nv_wr32(c, f->base + offset, value);
}

/* ------------------------------------------------------------------ waiting */

static bool wait_for(nv_card_t *c, const nv_falcon_t *f, u32 offset, u32 mask,
                     u32 want, int timeout_ms, const char *what) {
    for (int waited = 0; waited < timeout_ms * 100; waited++) {
        if ((falcon_rd(c, f, offset) & mask) == want) return true;
        timer_udelay(10);
    }
    kwarn("nvidia", "%s: %s did not happen within %d ms", f->name, what,
          timeout_ms);
    return false;
}

/* ------------------------------------------------------------------- reset */

bool nv_falcon_reset(nv_card_t *c, nv_falcon_t *f) {
    /* Nothing running, nothing pending. */
    falcon_wr(c, f, FALCON_IRQMASK, 0);
    falcon_wr(c, f, FALCON_IRQDEST, 0);
    falcon_wr(c, f, FALCON_IRQSCLR, 0xFFFFFFFFu);

    /* Hold it in reset, then let it go.  The pause between is not politeness:
     * the reset has to be seen on the engine's own clock, which is slower than
     * the one these writes are issued on. */
    u32 engine = falcon_rd(c, f, FALCON_ENGINE);
    falcon_wr(c, f, FALCON_ENGINE, engine | ENGINE_RESET);
    timer_udelay(10);
    falcon_wr(c, f, FALCON_ENGINE, engine & ~ENGINE_RESET);
    timer_udelay(10);

    /* On a cold card the hardware clears both memories before anything may be
     * written to them, and it takes a while at the size these have grown to. */
    if (!wait_for(c, f, FALCON_DMACTL,
                  DMACTL_IMEM_SCRUBBING | DMACTL_DMEM_SCRUBBING, 0, 200,
                  "the memory scrub"))
        return false;

    /* How much memory this one actually has.  It differs between engines and
     * between chips, and loading past the end of it silently wraps. */
    u32 hwcfg = falcon_rd(c, f, FALCON_HWCFG);
    f->imem_size = HWCFG_IMEM_SIZE(hwcfg) * 256;
    f->dmem_size = HWCFG_DMEM_SIZE(hwcfg) * 256;

    if (!f->imem_size || !f->dmem_size) {
        kwarn("nvidia", "%s: reports no memory (hwcfg %08x), so it is not "
                        "there or not reachable", f->name, hwcfg);
        return false;
    }

    f->ready = true;
    return true;
}

/* --------------------------------------------------------- loading the code
 *
 * Instruction memory is tagged: each two hundred and fifty-six byte block
 * carries the address it belongs at, and the Falcon uses the tag to decide
 * whether a block holds what it is about to execute. Writing code without
 * setting the tags produces a processor that starts and immediately faults on
 * memory it believes is not present.
 */
bool nv_falcon_load_imem(nv_card_t *c, nv_falcon_t *f, const u8 *code,
                         size_t len, u32 at, bool secure) {
    if (!f->ready) return false;
    if (at + len > f->imem_size) {
        kwarn("nvidia", "%s: %zu bytes at %u will not fit in %u of instruction "
                        "memory", f->name, len, at, f->imem_size);
        return false;
    }

    u32 port = 0;
    u32 block = at >> MEMC_BLK_SHIFT;

    /* Where to start, and that the address should step after every word. */
    falcon_wr(c, f, FALCON_IMEMC(port),
              (at & 0x00FFFF00u) | MEMC_AINCW | (secure ? MEMC_SECURE : 0));

    for (size_t off = 0; off < len; off += 4) {
        /* The tag changes at every block boundary, and has to be written
         * before the first word of that block. */
        if ((off % 256) == 0) {
            falcon_wr(c, f, FALCON_IMEMT(port), block++);
        }

        u32 word = 0;
        for (int i = 0; i < 4 && off + (size_t)i < len; i++)
            word |= (u32)code[off + i] << (8 * i);
        falcon_wr(c, f, FALCON_IMEMD(port), word);
    }

    /* Whatever is left of the last block is still part of it as far as the tag
     * is concerned, so it is padded rather than left with a stale tag. */
    size_t tail = len % 256;
    if (tail) {
        for (size_t pad = tail; pad < 256; pad += 4)
            falcon_wr(c, f, FALCON_IMEMD(port), 0);
    }
    return true;
}

bool nv_falcon_load_dmem(nv_card_t *c, nv_falcon_t *f, const u8 *data,
                         size_t len, u32 at) {
    if (!f->ready) return false;
    if (at + len > f->dmem_size) {
        kwarn("nvidia", "%s: %zu bytes at %u will not fit in %u of data memory",
              f->name, len, at, f->dmem_size);
        return false;
    }

    u32 port = 0;
    falcon_wr(c, f, FALCON_DMEMC(port), (at & 0x00FFFFFCu) | MEMC_AINCW);

    for (size_t off = 0; off < len; off += 4) {
        u32 word = 0;
        for (int i = 0; i < 4 && off + (size_t)i < len; i++)
            word |= (u32)data[off + i] << (8 * i);
        falcon_wr(c, f, FALCON_DMEMD(port), word);
    }
    return true;
}

/* Reading data memory back, which is how a Falcon returns anything larger than
 * the two mailbox registers. */
bool nv_falcon_read_dmem(nv_card_t *c, nv_falcon_t *f, u8 *out, size_t len,
                         u32 at) {
    if (!f->ready) return false;
    if (at + len > f->dmem_size) return false;

    u32 port = 0;
    falcon_wr(c, f, FALCON_DMEMC(port), (at & 0x00FFFFFCu) | MEMC_AINCR);

    for (size_t off = 0; off < len; off += 4) {
        u32 word = falcon_rd(c, f, FALCON_DMEMD(port));
        for (int i = 0; i < 4 && off + (size_t)i < len; i++)
            out[off + i] = (u8)(word >> (8 * i));
    }
    return true;
}

/* ------------------------------------------------------------------ running */

bool nv_falcon_start(nv_card_t *c, nv_falcon_t *f, u32 boot_vector) {
    if (!f->ready) return false;

    falcon_wr(c, f, FALCON_BOOTVEC, boot_vector);

    /* Let it reach memory outside itself; without this it can only see its own
     * instruction and data memory, which is enough for a bootloader and not
     * for anything else. */
    falcon_wr(c, f, FALCON_DMACTL, 0);
    falcon_wr(c, f, FALCON_ITFEN, 0x00000003u);

    u32 cpuctl = falcon_rd(c, f, FALCON_CPUCTL);
    if (cpuctl & CPUCTL_ALIAS_EN)
        falcon_wr(c, f, FALCON_CPUCTL_ALIAS, CPUCTL_STARTCPU);
    else
        falcon_wr(c, f, FALCON_CPUCTL, CPUCTL_STARTCPU);

    f->running = true;
    return true;
}

bool nv_falcon_wait_halt(nv_card_t *c, nv_falcon_t *f, int timeout_ms) {
    if (!wait_for(c, f, FALCON_CPUCTL, CPUCTL_HALTED, CPUCTL_HALTED,
                  timeout_ms, "the processor halting"))
        return false;
    f->running = false;
    return true;
}

u32 nv_falcon_mailbox(nv_card_t *c, nv_falcon_t *f, int which) {
    return falcon_rd(c, f, which ? FALCON_MAILBOX1 : FALCON_MAILBOX0);
}

void nv_falcon_set_mailbox(nv_card_t *c, nv_falcon_t *f, int which, u32 value) {
    falcon_wr(c, f, which ? FALCON_MAILBOX1 : FALCON_MAILBOX0, value);
}

bool nv_falcon_halted(nv_card_t *c, nv_falcon_t *f) {
    return (falcon_rd(c, f, FALCON_CPUCTL) & CPUCTL_HALTED) != 0;
}

/* ------------------------------------------------------------------- test
 *
 * Against the model next door, because no machine this has run on has a card
 * to try it on. What is proved here is the sequence and the error handling -
 * that the driver waits for the scrub, tags the blocks, loads before starting,
 * and reads back what it wrote. What is not proved, and cannot be by any
 * model, is that these register offsets are the ones the silicon uses.
 */
int nv_falcon_selftest(void) {
    int failures = 0;

    nv_card_t *c = nv_model_card();
    if (!c) {
        kinfo("nv-falcon", "no card to try this on");
        return 0;
    }

    nv_falcon_model_attach(c->regs, NV_FALCON_GSP);

    nv_falcon_t gsp = { "gsp", NV_FALCON_GSP, 0, 0, false, false };

    /* Starting one that was never reset must be refused, not attempted. */
    if (nv_falcon_start(c, &gsp, 0)) {
        kerr("nv-falcon", "a processor that was never brought up was started");
        failures++;
    }

    if (!nv_falcon_reset(c, &gsp)) {
        kerr("nv-falcon", "the processor did not come out of reset");
        nv_falcon_model_detach();
        return failures + 1;
    }

    if (nv_falcon_model_resets() != 1) {
        kerr("nv-falcon", "the reset was not seen by the hardware");
        failures++;
    }

    /* The card's own answer about its size, which the driver must take rather
     * than assume. */
    if (gsp.imem_size != 32 * 1024 || gsp.dmem_size != 16 * 1024) {
        kerr("nv-falcon", "read back %u and %u bytes of memory, expected "
                          "32768 and 16384", gsp.imem_size, gsp.dmem_size);
        failures++;
    }

    /* Anything written before the scrub finished would have been lost.  The
     * model records that it happened; the driver must not have let it. */
    if (nv_falcon_model_wrote_while_scrubbing()) {
        kerr("nv-falcon", "code was written while the memory was still being "
                          "cleared, and was lost");
        failures++;
    }

    /* More than the memory holds has to be refused rather than wrapped. */
    static u8 too_much[64 * 1024];
    if (nv_falcon_load_imem(c, &gsp, too_much, sizeof too_much, 0, false)) {
        kerr("nv-falcon", "an image larger than the memory was accepted");
        failures++;
    }

    /* Something that spans several blocks, so the tags are exercised. */
    static u8 code[1024];
    for (size_t i = 0; i < sizeof code; i++) code[i] = (u8)(i * 7 + 3);

    static u8 data[512];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (u8)(i ^ 0xA5);

    if (!nv_falcon_load_imem(c, &gsp, code, sizeof code, 0, false)) {
        kerr("nv-falcon", "the code would not load");
        failures++;
    }
    if (!nv_falcon_load_dmem(c, &gsp, data, sizeof data, 0)) {
        kerr("nv-falcon", "the data would not load");
        failures++;
    }

    /* Four blocks of code means four tags. */
    if (nv_falcon_model_tags() != 4) {
        kerr("nv-falcon", "%d block tags were set, expected 4",
             nv_falcon_model_tags());
        failures++;
    }

    /* And the bytes have to have arrived as they were sent. */
    if (memcmp(nv_falcon_model_imem(), code, sizeof code)) {
        kerr("nv-falcon", "the code arrived altered");
        failures++;
    }
    if (memcmp(nv_falcon_model_dmem(), data, sizeof data)) {
        kerr("nv-falcon", "the data arrived altered");
        failures++;
    }

    /* Reading it back out, which is how a Falcon returns anything larger than
     * its two mailbox registers. */
    static u8 back[512];
    if (!nv_falcon_read_dmem(c, &gsp, back, sizeof back, 0)) {
        kerr("nv-falcon", "the data could not be read back");
        failures++;
    } else if (memcmp(back, data, sizeof back)) {
        kerr("nv-falcon", "the data read back differently from how it went in");
        failures++;
    }

    /* Now run it. */
    if (!nv_falcon_start(c, &gsp, 0)) {
        kerr("nv-falcon", "the processor would not start");
        failures++;
    } else if (!nv_falcon_wait_halt(c, &gsp, 100)) {
        kerr("nv-falcon", "the processor never halted");
        failures++;
    } else {
        u32 result = nv_falcon_mailbox(c, &gsp, 0);
        if (result != 0) {
            kerr("nv-falcon", "the processor halted with %08x", result);
            failures++;
        }
    }

    if (nv_falcon_model_bootvec() != 0) {
        kerr("nv-falcon", "it started somewhere other than where it was told");
        failures++;
    }

    /* And the other direction: a processor started with nothing loaded must
     * fail, and the driver must be able to tell that it failed. */
    if (nv_falcon_reset(c, &gsp)) {
        nv_falcon_start(c, &gsp, 0);
        nv_falcon_wait_halt(c, &gsp, 100);
        if (nv_falcon_mailbox(c, &gsp, 0) == 0) {
            kerr("nv-falcon", "a processor with no code loaded reported success");
            failures++;
        }
    }

    nv_falcon_model_detach();

    if (!failures)
        kinfo("nv-falcon", "the processor boot sequence is correct: reset, "
                           "wait for the scrub, load %u bytes tagged by block, "
                           "start, halt, and read the result back",
              (unsigned)sizeof code);
    return failures;
}
