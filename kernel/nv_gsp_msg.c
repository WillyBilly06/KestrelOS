/* nv_gsp_msg.c - the envelope a message to the card's co-processor travels in.
 *
 * From Turing onwards the card is not driven by writing registers.  A driver
 * asks the processor on the card to do things - allocate a channel, make an
 * object, set a mode - and those requests travel as messages through a pair of
 * ring buffers in memory the card can read.  Nothing else can be asked of the
 * card until that conversation works, which is why this is the bottom of the
 * stack rather than a detail of it.
 *
 * This file is the envelope only: how a message is framed, checksummed and
 * numbered.  What goes inside it, and the rings it is placed in, are separate
 * pieces and are not here.
 *
 * ---------------------------------------------------------------------------
 * On where these numbers come from.
 *
 * Every field below is from NVIDIA's own published driver source - the bit
 * positions in mctp_format.h, the message type in nvdm_format.h, the element
 * layout and the checksum in message_queue_priv.h.  None of it is inferred
 * from behaviour or remembered from documentation, and that distinction is
 * load bearing: a header with a field one bit out of place is not rejected by
 * the card, it is ignored, and a driver waiting for an answer that will never
 * come looks exactly like a driver whose hardware is broken.
 *
 * What this file can be sure of and what it cannot: the framing is checked
 * below against values worked out by hand from those definitions, so the
 * encoding is right.  Whether the co-processor accepts it cannot be known
 * here - that needs a card, and the step before this one (starting the
 * processor at all) has never run on real silicon.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "klog.h"
#include "nv.h"

/* ------------------------------------------------------- the transport header
 *
 * Two 32-bit words.  The first says this is one whole message rather than a
 * fragment of a longer one; the second says who it is for.
 */
#define MCTP_VERSION_SHIFT   0      /* 3:0                                   */
#define MCTP_DEID_SHIFT      8      /* 15:8                                  */
#define MCTP_SEID_SHIFT     16      /* 23:16                                 */
#define MCTP_SEQ_SHIFT      28      /* 29:28                                 */
#define MCTP_EOM_SHIFT      30      /* 30:30  last fragment                  */
#define MCTP_SOM_SHIFT      31      /* 31:31  first fragment                 */

#define MCTP_MSG_TYPE_SHIFT       0   /* 6:0                                 */
#define MCTP_MSG_IC_SHIFT         7   /* 7:7                                 */
#define MCTP_MSG_VENDOR_SHIFT     8   /* 23:8                                */
#define MCTP_MSG_NVDM_TYPE_SHIFT 24   /* 31:24                               */

#define MCTP_TYPE_VENDOR_PCI  0x7E
#define MCTP_VENDOR_NVIDIA    0x10DE

/* The kind of message this carries.  0x25 is a request to the part of the
 * firmware that manages the card. */
#define NVDM_TYPE_RM_RPC      0x25

u32 nv_gsp_transport_header(bool first, bool last, u8 source, u8 destination,
                            u8 sequence) {
    return (1u << MCTP_VERSION_SHIFT) |
           ((u32)destination << MCTP_DEID_SHIFT) |
           ((u32)source << MCTP_SEID_SHIFT) |
           (((u32)sequence & 3u) << MCTP_SEQ_SHIFT) |
           ((u32)(last ? 1 : 0) << MCTP_EOM_SHIFT) |
           ((u32)(first ? 1 : 0) << MCTP_SOM_SHIFT);
}

u32 nv_gsp_message_header(u8 kind) {
    return ((u32)MCTP_TYPE_VENDOR_PCI << MCTP_MSG_TYPE_SHIFT) |
           (0u << MCTP_MSG_IC_SHIFT) |
           ((u32)MCTP_VENDOR_NVIDIA << MCTP_MSG_VENDOR_SHIFT) |
           ((u32)kind << MCTP_MSG_NVDM_TYPE_SHIFT);
}

/* ------------------------------------------------------------- the checksum
 *
 * Not a sum: every eight bytes of the payload are exclusive-ored together and
 * the two halves of the result folded into one word.  Cheap, and it catches
 * the failure that matters here - a message half written when the card reads
 * it - which an addition would also catch but which nothing else would.
 */
u32 nv_gsp_checksum(const void *data, u32 length) {
    const u8 *p = data;
    u64 sum = 0;

    /* Whole eight-byte words, read a byte at a time: the buffer is shared with
     * the card and nothing guarantees this driver's alignment of it. */
    u32 whole = length & ~7u;
    for (u32 i = 0; i < whole; i += 8) {
        u64 word = 0;
        for (int b = 0; b < 8; b++) word |= (u64)p[i + b] << (b * 8);
        sum ^= word;
    }

    /* And whatever is left over, which the published code reaches by always
     * being handed a length that is a multiple of eight - it rounds up to a
     * whole number of elements first.  Handling the remainder rather than
     * relying on that keeps this correct if it is ever called otherwise. */
    if (length & 7u) {
        u64 word = 0;
        for (u32 i = whole; i < length; i++)
            word |= (u64)p[i] << ((i - whole) * 8);
        sum ^= word;
    }

    return (u32)(sum >> 32) ^ (u32)sum;
}

/* --------------------------------------------------------------- an element
 *
 * The four words that precede every message in the ring.
 */
void nv_gsp_frame(nv_gsp_element_t *out, const void *payload, u32 length,
                  u32 sequence) {
    if (!out) return;

    out->transport = nv_gsp_transport_header(true, true, 0, 0, 0);
    out->message = nv_gsp_message_header(NVDM_TYPE_RM_RPC);
    out->sequence = sequence;
    out->checksum = payload ? nv_gsp_checksum(payload, length) : 0;
}

/* ------------------------------------------------------------------- tests
 *
 * The values below were worked out by hand from the field positions rather
 * than recorded from a run, so this checks the encoding against the
 * specification and not against itself.
 */
int nv_gsp_msg_selftest(void) {
    int failures = 0;

    /* A whole message from nobody to nobody: version 1, first and last set. */
    u32 t = nv_gsp_transport_header(true, true, 0, 0, 0);
    if (t != 0xC0000001u) {
        kwarn("nv-gsp", "selftest: a transport header came out %#x, expected "
                        "%#x - version 1 with both ends of the message set",
              t, 0xC0000001u);
        failures++;
    }

    /* A fragment in the middle: neither end, sequence two. */
    u32 mid = nv_gsp_transport_header(false, false, 0, 0, 2);
    if (mid != 0x20000001u) {
        kwarn("nv-gsp", "selftest: a middle fragment came out %#x, expected %#x",
              mid, 0x20000001u);
        failures++;
    }

    /* Addresses land in their own bytes and nowhere else. */
    u32 addressed = nv_gsp_transport_header(true, true, 0xAB, 0xCD, 0);
    if (addressed != 0xC0ABCD01u) {
        kwarn("nv-gsp", "selftest: source and destination came out %#x, "
                        "expected %#x", addressed, 0xC0ABCD01u);
        failures++;
    }

    /* Vendor-defined over PCI, NVIDIA, carrying a firmware request. */
    u32 m = nv_gsp_message_header(NVDM_TYPE_RM_RPC);
    if (m != 0x2510DE7Eu) {
        kwarn("nv-gsp", "selftest: a message header came out %#x, expected "
                        "%#x - type 0x7e, vendor 0x10de, kind 0x25",
              m, 0x2510DE7Eu);
        failures++;
    }

    /* The checksum of one word is that word's halves folded together. */
    {
        static const u8 one[8] = { 0x11, 0x22, 0x33, 0x44,
                                   0x55, 0x66, 0x77, 0x88 };
        u32 want = 0x88776655u ^ 0x44332211u;
        u32 got = nv_gsp_checksum(one, sizeof one);
        if (got != want) {
            kwarn("nv-gsp", "selftest: the checksum of one word came out %#x, "
                            "expected %#x", got, want);
            failures++;
        }
    }

    /* Two identical words cancel, which is what exclusive-or means and is the
     * property that catches a word written twice. */
    {
        static const u8 twice[16] = { 1, 2, 3, 4, 5, 6, 7, 8,
                                      1, 2, 3, 4, 5, 6, 7, 8 };
        if (nv_gsp_checksum(twice, sizeof twice) != 0) {
            kwarn("nv-gsp", "selftest: two identical words did not cancel");
            failures++;
        }
    }

    if (!failures)
        kinfo("nv-gsp", "messages to the card's co-processor are framed the "
                        "way its firmware expects");
    return failures;
}
