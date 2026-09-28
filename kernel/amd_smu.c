/* amd_smu.c - the two processors that own the card before the driver does.
 *
 * An AMD graphics card has a security processor and a power processor, and
 * neither of them is optional.
 *
 *   The security processor (AMD calls it MP0, or the PSP) holds the root of
 *   trust.  Every piece of firmware that runs anywhere on the card is loaded
 *   through it and checked by it.  On a card from Vega onward there is no way
 *   to bring the graphics engine up that does not go through this processor
 *   first - and nothing anybody outside AMD can sign will pass it.
 *
 *   The power processor (MP1, the SMU) owns the clocks, the voltages, the fans
 *   and the temperature limits.  The driver does not set a clock; it asks.  A
 *   card whose power processor has not come up runs at its slowest possible
 *   clock and there is nothing the driver can do about it from outside.
 *
 * Both are spoken to through a bank of mailbox registers, and the protocol is
 * the same shape in both cases: clear the answer, leave the argument, leave the
 * message, wait for the answer to become non-zero.  Getting the order wrong is
 * the classic bug - if the message is written before the argument, the
 * processor reads the previous argument, and the failure is intermittent
 * because it depends on how fast the other side happened to be.
 *
 * Where these mailboxes are is not fixed.  It comes from the discovery table
 * (amd_core.c), which is why every access here is relative to a base the card
 * itself supplied.
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "amd.h"

/* The discovery table gives base addresses as dword offsets, and the mailbox
 * numbers in amd.h are dword offsets too, so the sum is scaled once here. */
static u32 mp1_reg(amd_card_t *c, u32 dword) {
    return (c->mp1_base + dword) * 4;
}

static u32 mp0_reg(amd_card_t *c, u32 dword) {
    return (c->mp0_base + dword) * 4;
}

/* --------------------------------------------------------- the power processor */

static const char *smu_result(u32 code) {
    switch (code) {
    case AMD_SMU_RESP_NONE:        return "no answer at all";
    case AMD_SMU_RESP_OK:          return "done";
    case AMD_SMU_RESP_BUSY:        return "busy with something else";
    case AMD_SMU_RESP_BAD_PREREQ:  return "asked for out of order";
    case AMD_SMU_RESP_UNKNOWN:     return "a message it does not know";
    case AMD_SMU_RESP_FAIL:        return "tried and failed";
    default:                       return "an answer that is not one of its own";
    }
}

bool amd_smu_send(amd_card_t *c, u32 message, u32 argument, u32 *reply) {
    if (!c->mp1_base) {
        kwarn("amd-smu", "the power processor's mailbox has no address; the "
                         "discovery table did not list it");
        return false;
    }

    /* Anything left over from last time has to go first, or the poll below
     * will see the previous answer and return immediately with it. */
    u32 previous = amd_rd32(c, mp1_reg(c, AMD_SMU_RESP));
    if (previous == AMD_SMU_RESP_NONE) {
        /* Nothing outstanding.  Good. */
    } else if (previous == AMD_SMU_RESP_BUSY) {
        kwarn("amd-smu", "the power processor is still busy with the last "
                         "message");
        return false;
    }

    amd_wr32(c, mp1_reg(c, AMD_SMU_RESP), 0);

    /* Argument first, then the message.  This order is the whole protocol. */
    amd_wr32(c, mp1_reg(c, AMD_SMU_ARG), argument);
    amd_wr32(c, mp1_reg(c, AMD_SMU_MSG), message);

    u32 answer = 0;
    for (int waited = 0; waited < 200000; waited++) {
        answer = amd_rd32(c, mp1_reg(c, AMD_SMU_RESP));
        if (answer != AMD_SMU_RESP_NONE) break;
        timer_udelay(1);
    }

    if (answer != AMD_SMU_RESP_OK) {
        kwarn("amd-smu", "message %u came back as %s (%02x)", message,
              smu_result(answer), answer);
        return false;
    }

    /* The answer's payload is in the same register the argument went into -
     * which is why it has to be read after the response and not before. */
    if (reply) *reply = amd_rd32(c, mp1_reg(c, AMD_SMU_ARG));
    return true;
}

bool amd_smu_bring_up(amd_card_t *c) {
    c->smu_ready = false;
    c->smu_version = 0;

    /* The first thing to ask is whether it is listening at all.  The test
     * message is defined to return its argument with a known value added, so
     * an answer that is merely non-zero is not enough - the round trip has to
     * carry data. */
    u32 echo = 0;
    if (!amd_smu_send(c, AMD_SMU_MSG_TEST, 0x12345678u, &echo)) {
        kwarn("amd-smu", "the power processor did not answer; clocks and fans "
                         "stay wherever firmware left them");
        return false;
    }

    if (echo != 0x12345678u + 1) {
        kwarn("amd-smu", "the test message came back as %08x, so the mailbox is "
                         "answering but not with this card's protocol", echo);
        return false;
    }

    if (!amd_smu_send(c, AMD_SMU_MSG_GET_SMU_VERSION, 0, &c->smu_version)) {
        kwarn("amd-smu", "the power processor would not say what it is running");
        return false;
    }

    c->smu_ready = true;
    kinfo("amd-smu", "the power processor answered: firmware %u.%u.%u",
          (c->smu_version >> 16) & 0xFF, (c->smu_version >> 8) & 0xFF,
          c->smu_version & 0xFF);
    return true;
}

/* The sensors live behind the power processor rather than in a register, which
 * is the change from earlier generations: the driver no longer reads a
 * temperature, it asks for one. */
void amd_read_sensors(amd_card_t *c) {
    c->temperature_c = -1000;
    c->gfx_clock_mhz = 0;
    c->mem_clock_mhz = 0;

    if (!c->smu_ready) return;

    u32 metrics = 0;
    if (!amd_smu_send(c, AMD_SMU_MSG_GET_METRICS_TABLE, 0, &metrics)) return;

    /* The metrics word packs what a driver reads most often: the hotspot
     * temperature in the low byte and the two clocks above it, each in units
     * of ten megahertz so they fit. */
    c->temperature_c = (int)(metrics & 0xFF);
    c->gfx_clock_mhz = ((metrics >> 8) & 0xFFF) * 10;
    c->mem_clock_mhz = ((metrics >> 20) & 0xFFF) * 10;
}

/* ------------------------------------------------------ the security processor
 *
 * Its bootloader answers on one register and reports on the same one: bit 31
 * set means it has finished with whatever was asked last, and a command is
 * issued by writing a bit rather than a number.  The address of the firmware
 * to load goes in a second register, shifted right by twenty - the bootloader
 * only ever loads from a megabyte boundary, so the low bits would be wasted.
 */
bool amd_psp_wait_bootloader(amd_card_t *c, int timeout_ms) {
    if (!c->mp0_base) {
        kwarn("amd-psp", "the security processor's mailbox has no address");
        return false;
    }

    for (int waited = 0; waited < timeout_ms * 100; waited++) {
        u32 status = amd_rd32(c, mp0_reg(c, AMD_PSP_BL_CMD));
        if (status & AMD_PSP_BL_READY) return true;
        timer_udelay(10);
    }

    kwarn("amd-psp", "the security processor's bootloader never became ready");
    return false;
}

bool amd_psp_bootloader_load(amd_card_t *c, u32 command, u64 firmware_at,
                             int timeout_ms) {
    if (!amd_psp_wait_bootloader(c, timeout_ms)) return false;

    /* Clearing it is what lets the wait below distinguish "finished this one"
     * from "was already finished with the last one". */
    amd_wr32(c, mp0_reg(c, AMD_PSP_BL_CMD), 0);

    amd_wr32(c, mp0_reg(c, AMD_PSP_BL_ARG), (u32)(firmware_at >> 20));
    amd_wr32(c, mp0_reg(c, AMD_PSP_BL_CMD), command);

    for (int waited = 0; waited < timeout_ms * 100; waited++) {
        u32 status = amd_rd32(c, mp0_reg(c, AMD_PSP_BL_CMD));
        if (status & AMD_PSP_BL_READY) {
            /* The low bits carry the verdict.  Anything other than zero means
             * the image was rejected, and on a real card that is what happens
             * to anything not signed by AMD. */
            u32 verdict = status & 0xFFFF;
            if (verdict) {
                kwarn("amd-psp", "the security processor rejected the image "
                                 "(%04x)", verdict);
                return false;
            }
            return true;
        }
        timer_udelay(10);
    }

    kwarn("amd-psp", "the security processor did not finish loading");
    return false;
}

/* Once its firmware is up, the security processor is driven by a ring of
 * commands in memory rather than by the mailbox - the mailbox is only used to
 * tell it where the ring is and to nudge it. */
bool amd_psp_ring_create(amd_card_t *c, u64 ring_at, u32 ring_bytes) {
    if (!c->mp0_base) return false;

    /* Wait for the secure OS to say it is alive.  Creating a ring before that
     * writes into registers the bootloader is still using. */
    bool alive = false;
    for (int waited = 0; waited < 20000; waited++) {
        if (amd_rd32(c, mp0_reg(c, AMD_PSP_SOS_ALIVE)) != 0) { alive = true; break; }
        timer_udelay(10);
    }
    if (!alive) {
        kwarn("amd-psp", "the secure OS never signalled that it was running");
        return false;
    }

    amd_wr32(c, mp0_reg(c, AMD_PSP_RING_LO), (u32)ring_at);
    amd_wr32(c, mp0_reg(c, AMD_PSP_RING_HI), (u32)(ring_at >> 32));
    amd_wr32(c, mp0_reg(c, AMD_PSP_RING_SIZE), ring_bytes);
    amd_wr32(c, mp0_reg(c, AMD_PSP_RING_WPTR), 0);

    /* Ring type one is the one a driver uses; it goes in the high half of the
     * command word. */
    amd_wr32(c, mp0_reg(c, AMD_PSP_RING_CMD), 1u << 16);

    for (int waited = 0; waited < 20000; waited++) {
        u32 status = amd_rd32(c, mp0_reg(c, AMD_PSP_RING_CMD));
        if (status & AMD_PSP_RESP_FLAG)
            return (status & 0xFFFF) == 0;
        timer_udelay(10);
    }

    kwarn("amd-psp", "the security processor never acknowledged the ring");
    return false;
}
