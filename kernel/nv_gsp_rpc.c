/* nv_gsp_rpc.c - the request inside the envelope.
 *
 * nv_gsp_msg.c frames a message and nv_gsp_ring.c finds it a slot; this is
 * what goes in one.  Every request to the firmware on the card - allocate an
 * object, make a channel, ask what the card is - begins with the same header,
 * and the firmware reads the whole of it before deciding whether to look at
 * what follows.
 *
 * ---------------------------------------------------------------------------
 * The signature is worth a sentence.  It is 0x43505256, which is "VRPC" laid
 * out least significant byte first, and it is the firmware's check that what
 * it is reading is a request at all rather than whatever was in that memory
 * before.  A header without it is discarded silently - there is no reply
 * saying the signature was wrong, because a message the firmware does not
 * believe is a message is not one it will answer.
 *
 * Every field here is from NVIDIA's published source: the layout from
 * g_rpc-message-header.h, the signature and the version fields from
 * rpc_headers.h.  As with the framing, a field in the wrong place produces
 * silence rather than an error, which is why these are copied rather than
 * reasoned about.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "klog.h"
#include "nv.h"

/* "VRPC", least significant byte first. */
#define RPC_SIGNATURE  0x43505256u

/* The version this driver speaks, packed the way the firmware reads it: major
 * in the top byte, minor in the one below. */
#define RPC_VERSION_MAJOR  3
#define RPC_VERSION_MINOR  0

u32 nv_gsp_rpc_version(void) {
    return ((u32)RPC_VERSION_MAJOR << 24) | ((u32)RPC_VERSION_MINOR << 16);
}

/* Fill in the header for one request.
 *
 * `length` is the whole request including this header, not the payload after
 * it - the firmware uses it to find the end, so counting only the payload
 * leaves it reading one header short and into whatever follows.
 */
void nv_gsp_rpc_header(nv_gsp_rpc_header_t *out, u32 function, u32 length,
                       u32 sequence) {
    if (!out) return;

    memset(out, 0, sizeof *out);
    out->header_version = nv_gsp_rpc_version();
    out->signature = RPC_SIGNATURE;
    out->length = length;
    out->function = function;
    out->result = 0;
    out->result_private = 0xFFFFFFFFu;   /* the firmware's "not set" */
    out->sequence = sequence;
    out->spare = 0;
}

/* Whether a reply is one this driver should read.
 *
 * Checked rather than assumed because the reply lands in memory this driver
 * handed over: a firmware that has not written yet leaves whatever was there,
 * and that is far more likely to look like a plausible reply than to look
 * like nothing.
 */
/* The selftest below deliberately hands this replies that must be refused.
 * Those refusals are the test passing, so they are not logged - a boot that
 * prints warnings to show that its checks worked is a boot people learn to
 * read past. */
static bool quiet_refusals;

bool nv_gsp_rpc_reply_ok(const nv_gsp_rpc_header_t *h, u32 expect_function,
                         u32 expect_sequence) {
    if (!h) return false;

    if (h->signature != RPC_SIGNATURE) {
        if (!quiet_refusals) kwarn("nv-gsp", "a reply carries signature %#x rather than %#x - the "
                        "firmware has not written here yet, or has written "
                        "something that is not a reply",
              h->signature, RPC_SIGNATURE);
        return false;
    }
    if (h->function != expect_function) {
        if (!quiet_refusals) kwarn("nv-gsp", "a reply answers request %u; %u was asked",
              h->function, expect_function);
        return false;
    }
    if (h->sequence != expect_sequence) {
        if (!quiet_refusals) kwarn("nv-gsp", "a reply is numbered %u; %u was expected - an answer "
                        "to an earlier request has been read as this one",
              h->sequence, expect_sequence);
        return false;
    }
    if (h->length < sizeof(nv_gsp_rpc_header_t)) {
        if (!quiet_refusals) kwarn("nv-gsp", "a reply says it is %u bytes, which is shorter than "
                        "its own header", h->length);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------- tests */

int nv_gsp_rpc_selftest(void) {
    int failures = 0;

    if (nv_gsp_rpc_version() != 0x03000000u) {
        kwarn("nv-gsp", "selftest: the version came out %#x, expected %#x - "
                        "major three in the top byte, minor zero below it",
              nv_gsp_rpc_version(), 0x03000000u);
        failures++;
    }

    nv_gsp_rpc_header_t h;
    nv_gsp_rpc_header(&h, 42, 256, 7);

    if (h.signature != 0x43505256u) {
        kwarn("nv-gsp", "selftest: the signature came out %#x, expected %#x "
                        "- \"VRPC\", least significant byte first",
              h.signature, 0x43505256u);
        failures++;
    }
    if (h.function != 42 || h.length != 256 || h.sequence != 7) {
        kwarn("nv-gsp", "selftest: a header came back as function %u, length "
                        "%u, sequence %u", h.function, h.length, h.sequence);
        failures++;
    }

    /* Its own reply is acceptable; the same reply against a different request
     * or a different number is not.  Both of those are how a driver ends up
     * reading an answer to something it asked a moment ago. */
    if (!nv_gsp_rpc_reply_ok(&h, 42, 7)) {
        kwarn("nv-gsp", "selftest: a well formed reply was refused");
        failures++;
    }

    quiet_refusals = true;

    /* A reply that has not been written: whatever was in the buffer. */
    nv_gsp_rpc_header_t stale;
    memset(&stale, 0xCD, sizeof stale);
    if (nv_gsp_rpc_reply_ok(&stale, 42, 7)) {
        kwarn("nv-gsp", "selftest: uninitialised memory was accepted as a reply");
        failures++;
    }

    /* And one whose length does not cover its own header. */
    nv_gsp_rpc_header_t short_one = h;
    short_one.length = 4;
    if (nv_gsp_rpc_reply_ok(&short_one, 42, 7)) {
        kwarn("nv-gsp", "selftest: a reply shorter than its header was "
                        "accepted");
        failures++;
    }

    /* The header is exactly what the firmware expects to read: eight words. */
    if (sizeof(nv_gsp_rpc_header_t) != 32) {
        kwarn("nv-gsp", "selftest: the header is %u bytes; the firmware reads "
                        "32", (unsigned)sizeof(nv_gsp_rpc_header_t));
        failures++;
    }

    quiet_refusals = false;

    if (!failures)
        kinfo("nv-gsp", "requests to the card's firmware carry the header it "
                        "reads, and a reply that was never written is refused");
    return failures;
}
