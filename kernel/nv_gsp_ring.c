/* nv_gsp_ring.c - the two rings a driver and the card's co-processor talk over.
 *
 * Each side owns one ring and writes only to it.  A header at the start of
 * each says how large it is, how large one entry is, and how far its owner has
 * written; a second, smaller header says how far the other side has read.
 * Neither side ever writes to the other's, which is what makes the whole thing
 * work without a lock across a bus.
 *
 * This file is the accounting only - how full a ring is, where the next entry
 * goes, and when a slot may be reused.  It touches no hardware and knows
 * nothing about what the entries contain.
 *
 * ---------------------------------------------------------------------------
 * Two details from NVIDIA's own implementation that are worth stating, because
 * both are the kind of thing a reimplementation gets subtly wrong.
 *
 * The count of free slots is one less than the distance between the pointers.
 * A ring where write catches up to read is indistinguishable from one where
 * read has caught up to write - both have the pointers equal - so one slot is
 * always left unused and "equal" always means empty.  Getting this wrong gives
 * a ring that reports itself empty when it is full, and a driver that
 * overwrites a message the card has not yet read.
 *
 * And the wrap is done by subtracting rather than by taking a remainder.  The
 * comment in the original says why: the processor on the card is a RISC-V core
 * where division is slow.  The result is identical; it is written this way
 * here to stay recognisably the same code.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "klog.h"
#include "nv.h"

/* Bumped by the firmware when the layout changes; a ring announcing anything
 * else is one this does not understand. */
#define MSGQ_VERSION 0

/* How many entries may still be written before the ring is full.
 *
 * `write` is this side's own pointer and `read` is the far side's, read out of
 * shared memory - so this is the one number here that can change underneath
 * the caller, and it is deliberately taken as an argument rather than read
 * again inside. */
u32 nv_gsp_ring_free(u32 write, u32 read, u32 count) {
    if (!count || read >= count || write >= count) return 0;

    u32 free = read + count - write - 1;
    if (free >= count) free -= count;
    return free;
}

/* How many entries have arrived and not yet been taken. */
u32 nv_gsp_ring_available(u32 write, u32 read, u32 count) {
    if (!count || read >= count || write >= count) return 0;

    u32 available = write + count - read;
    if (available >= count) available -= count;
    return available;
}

/* The next slot, and the pointer that follows it. */
u32 nv_gsp_ring_advance(u32 pointer, u32 by, u32 count) {
    if (!count) return 0;

    u32 next = pointer + by;
    while (next >= count) next -= count;
    return next;
}

/* Whether a ring's header describes something this driver can use. */
bool nv_gsp_ring_check(const nv_gsp_ring_header_t *h, const char *which) {
    if (!h) return false;

    if (h->version != MSGQ_VERSION) {
        kwarn("nv-gsp", "the %s ring announces version %u; this driver "
                        "understands %u", which, h->version, MSGQ_VERSION);
        return false;
    }
    if (!h->entry_size || (h->entry_size & (h->entry_size - 1))) {
        kwarn("nv-gsp", "the %s ring has %u-byte entries, which is not a power "
                        "of two", which, h->entry_size);
        return false;
    }
    if (h->entry_size < 16) {
        kwarn("nv-gsp", "the %s ring has %u-byte entries; sixteen is the "
                        "smallest the firmware uses", which, h->entry_size);
        return false;
    }
    if (!h->entry_count) {
        kwarn("nv-gsp", "the %s ring holds no entries", which);
        return false;
    }
    if (h->write >= h->entry_count) {
        kwarn("nv-gsp", "the %s ring's write pointer is %u of %u entries",
              which, h->write, h->entry_count);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------- tests */

int nv_gsp_ring_selftest(void) {
    int failures = 0;
    const u32 n = 8;

    /* Empty: the pointers are equal, and all but one slot may be written. */
    if (nv_gsp_ring_available(0, 0, n) != 0) {
        kwarn("nv-gsp", "selftest: an empty ring reported entries waiting");
        failures++;
    }
    if (nv_gsp_ring_free(0, 0, n) != n - 1) {
        kwarn("nv-gsp", "selftest: an empty ring offered %u slots, expected %u "
                        "- one is always held back so that equal pointers can "
                        "only mean empty",
              nv_gsp_ring_free(0, 0, n), n - 1);
        failures++;
    }

    /* Three written, none read. */
    if (nv_gsp_ring_available(3, 0, n) != 3) {
        kwarn("nv-gsp", "selftest: three written read back as %u",
              nv_gsp_ring_available(3, 0, n));
        failures++;
    }
    if (nv_gsp_ring_free(3, 0, n) != n - 4) {
        kwarn("nv-gsp", "selftest: with three written, %u slots were offered, "
                        "expected %u", nv_gsp_ring_free(3, 0, n), n - 4);
        failures++;
    }

    /* Wrapped: the writer is behind the reader in the buffer and ahead of it
     * in time, which is the case that plain subtraction gets wrong. */
    if (nv_gsp_ring_available(2, 6, n) != 4) {
        kwarn("nv-gsp", "selftest: a wrapped ring reported %u waiting, "
                        "expected 4", nv_gsp_ring_available(2, 6, n));
        failures++;
    }
    if (nv_gsp_ring_free(2, 6, n) != 3) {
        kwarn("nv-gsp", "selftest: a wrapped ring offered %u slots, expected 3",
              nv_gsp_ring_free(2, 6, n));
        failures++;
    }

    /* Full: one slot short of the reader, and nothing more may be written. */
    if (nv_gsp_ring_free(7, 0, n) != 0) {
        kwarn("nv-gsp", "selftest: a full ring offered %u slots",
              nv_gsp_ring_free(7, 0, n));
        failures++;
    }
    if (nv_gsp_ring_available(7, 0, n) != 7) {
        kwarn("nv-gsp", "selftest: a full ring reported %u waiting, expected 7",
              nv_gsp_ring_available(7, 0, n));
        failures++;
    }

    /* Free and available together never exceed the ring, at any position. */
    for (u32 w = 0; w < n; w++) {
        for (u32 r = 0; r < n; r++) {
            u32 total = nv_gsp_ring_free(w, r, n) + nv_gsp_ring_available(w, r, n);
            if (total == n - 1) continue;
            kwarn("nv-gsp", "selftest: with write %u and read %u, free plus "
                            "waiting came to %u rather than %u", w, r, total, n - 1);
            failures++;
            break;
        }
        if (failures) break;
    }

    /* Advancing wraps and never lands outside the ring. */
    if (nv_gsp_ring_advance(6, 5, n) != 3) {
        kwarn("nv-gsp", "selftest: advancing 6 by 5 in %u gave %u, expected 3",
              n, nv_gsp_ring_advance(6, 5, n));
        failures++;
    }

    if (!failures)
        kinfo("nv-gsp", "the rings to the card's co-processor account for "
                        "their entries correctly, including across the wrap");
    return failures;
}
