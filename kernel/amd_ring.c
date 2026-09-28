/* amd_ring.c - telling the card to do something.
 *
 * Nothing on a modern GPU is done by writing a register.  Work is described as
 * a stream of packets in memory, and the driver's whole job at this layer is to
 * build that stream correctly and then say how much of it there is.
 *
 * AMD's packet format has not changed since Southern Islands.  A type-3 packet
 * is a header carrying an opcode and a word count, followed by that many words.
 * The count is what the card's command processor uses to find the next packet -
 * so a packet whose count disagrees with the number of words actually written
 * does not fail on that packet.  It fails on the one after, which is read from
 * the wrong place, and the ring is desynchronised from then on with no
 * indication of where it happened.  That failure is why every packet built here
 * is opened with a declared length and closed with a check that the declaration
 * was true.  It is the single most valuable assertion in a ring driver.
 *
 * Two other things matter and both are ordering:
 *
 *   The packets must be in memory before the write pointer that reveals them.
 *   The other side of this is a processor, not a thread, and it is reading
 *   while the driver writes.
 *
 *   The ring is full when advancing the write pointer would make it equal the
 *   read pointer, not when it already is - so one word is always left unused,
 *   because otherwise full and empty are the same state.
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "amd.h"

bool amd_ring_init(amd_ring_t *r, u32 *memory, u64 gpu_address, u32 words,
                   volatile u32 *rptr_report, volatile u64 *fence) {
    memset(r, 0, sizeof *r);

    /* A power of two, because the wrap is a mask.  A ring whose length is not
     * one wraps to the wrong place and the failure looks like corruption. */
    if (!memory || !words || (words & (words - 1))) {
        kwarn("amd-ring", "a ring of %u words cannot be masked", words);
        return false;
    }

    r->base = memory;
    r->gpu_address = gpu_address;
    r->words = words;
    r->write = 0;
    r->rptr_report = rptr_report;
    r->fence = fence;
    r->fence_next = 1;
    r->ready = true;

    memset(memory, 0, (size_t)words * 4);
    if (rptr_report) *rptr_report = 0;
    if (fence) *fence = 0;
    return true;
}

u32 amd_ring_free(amd_ring_t *r) {
    u32 read = r->rptr_report ? (*r->rptr_report & (r->words - 1)) : 0;
    u32 used = (r->write + r->words - read) & (r->words - 1);
    return r->words - used - 1;
}

void amd_ring_begin(amd_ring_t *r, u32 words) {
    r->expected = words;
    r->emitted = 0;
}

void amd_ring_write(amd_ring_t *r, u32 value) {
    if (!r->ready) return;
    r->base[r->write & (r->words - 1)] = value;
    r->write = (r->write + 1) & (r->words - 1);
    r->emitted++;
}

bool amd_ring_commit(amd_card_t *c, amd_ring_t *r) {
    if (!r->ready) return false;

    if (r->emitted != r->expected) {
        kerr("amd-ring", "a packet declared %u words and wrote %u; the ring "
                         "would be desynchronised from here on",
             r->expected, r->emitted);
        /* Backed out rather than committed.  A wrong packet must never become
         * visible, because nothing downstream can recover from it. */
        r->write = (r->write + r->words - r->emitted) & (r->words - 1);
        r->emitted = r->expected = 0;
        return false;
    }

    r->emitted = r->expected = 0;

    /* Everything in memory before the pointer that reveals it. */
    __asm__ volatile("" ::: "memory");

    if (r->doorbell) amd_wr32(c, r->doorbell, r->write);
    if (r->wptr_report) *r->wptr_report = r->write;
    return true;
}

/* --------------------------------------------------------------- packets */

/* A fence: when everything ahead of this has finished, write a value to an
 * address the driver can see.  This is how completion is known - there is no
 * interrupt that says "that particular submission is done", only this. */
bool amd_ring_emit_fence(amd_card_t *c, amd_ring_t *r, u64 at, u64 value) {
    if (amd_ring_free(r) < 8) {
        kwarn("amd-ring", "no room for a fence");
        return false;
    }

    amd_ring_begin(r, 8);
    amd_ring_write(r, AMD_PACKET_TYPE3(AMD_PM4_RELEASE_MEM, 6));
    /* Which event, and that it is a timestamp event - meaning the write
     * happens after the pipeline has drained rather than when the packet is
     * read.  Getting this wrong gives a fence that fires early, and the bug it
     * causes is a drawing that is sometimes incomplete. */
    amd_ring_write(r, (5u << 8) | 0x04);      /* event index 5, cache flush   */
    amd_ring_write(r, (2u << 29));            /* write 64 bits of immediate   */
    amd_ring_write(r, (u32)at);
    amd_ring_write(r, (u32)(at >> 32));
    amd_ring_write(r, (u32)value);
    amd_ring_write(r, (u32)(value >> 32));
    amd_ring_write(r, 0);                     /* context id                   */
    return amd_ring_commit(c, r);
}

/* A call into a buffer of packets elsewhere.  Everything a driver actually
 * submits arrives this way: the ring holds the calls and the work is in the
 * buffers, because a buffer can be built once and called many times. */
bool amd_ring_emit_indirect(amd_card_t *c, amd_ring_t *r, u64 at, u32 words) {
    if (amd_ring_free(r) < 4) {
        kwarn("amd-ring", "no room for an indirect buffer");
        return false;
    }
    if (at & 0x1F) {
        kwarn("amd-ring", "an indirect buffer at %llx is not aligned; the "
                          "command processor requires 32 bytes",
              (unsigned long long)at);
        return false;
    }
    if (words == 0 || words > 0xFFFFF) {
        kwarn("amd-ring", "an indirect buffer of %u words cannot be described",
              words);
        return false;
    }

    amd_ring_begin(r, 4);
    amd_ring_write(r, AMD_PACKET_TYPE3(AMD_PM4_INDIRECT_BUFFER, 2));
    amd_ring_write(r, (u32)at);
    amd_ring_write(r, (u32)(at >> 32));
    amd_ring_write(r, words);
    return amd_ring_commit(c, r);
}

bool amd_ring_wait_fence(amd_ring_t *r, u64 value, int timeout_ms) {
    if (!r->fence) return false;

    for (int waited = 0; waited < timeout_ms * 100; waited++) {
        if (*r->fence >= value) return true;
        timer_udelay(10);
    }
    return false;
}
