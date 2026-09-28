/* nv_fsp_model.c - the card's security processor, in software.
 *
 * It receives a chain-of-trust message the way the real one does: words pushed
 * through a scratchpad window, then a queue pointer moved. It takes the
 * message apart, checks it against the published framing, and either starts
 * the co-processor it is standing in for or records why it would not have.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS WRITTEN FROM THE SPECIFICATION AND NOT FROM THE DRIVER
 *
 * Twice in this project a model was built by reading the driver it was meant
 * to check, and both times it encoded the driver's own mistake and agreed with
 * it: a thermal sensor read from the wrong register, and video memory counted
 * from the wrong one of two partition registers. Both tests passed. Both
 * would have failed on real silicon.
 *
 * So the numbers here were taken from the framing NVIDIA and nouveau publish -
 * the bit ranges, the field order, the constants - and written out
 * independently. Where this file and nv_fsp.c agree, that is two readings of
 * the same source agreeing. Where they disagree, this one is entitled to say
 * so, and the test is what makes it able to.
 *
 * The checks are on the WIRE FORMAT, deliberately: what a real processor can
 * see is the bytes that arrive and the order the registers were touched in,
 * and nothing about how the driver arranged to produce them.
 */
#include "kernel.h"
#include "klog.h"
#include "nv.h"
#include "mm.h"

/* The window this model presents, big enough for the register block the
 * driver reaches into. */
/* Big enough to hold the highest register the driver touches on this path -
 * the Blackwell thermal scratch at 0x00ad00bc where the security processor
 * reports its own boot complete sits above the old 9 MiB window. */
#define WINDOW_BYTES  0x00B00000u

/* What a correct message looks like, spelled out here rather than shared. */
/* NVIDIA's kfspCreateMctpHeader_GH100 leaves the VERSION field at reset (0).
 * It is defined by the format header but deliberately is not populated by the
 * R570 sender. */
#define EXPECT_MCTP_VERSION   0
#define EXPECT_MCTP_SOM       1
#define EXPECT_MCTP_EOM       1
#define EXPECT_MSG_TYPE       0x7E      /* vendor-defined, over PCI          */
#define EXPECT_VENDOR_ID      0x10DE    /* NVIDIA                            */
#define EXPECT_NVDM_TYPE      0x14      /* a chain of trust                  */
#define EXPECT_COT_VERSION    2   /* Blackwell; Hopper would say 1 */
#define EXPECT_COT_SIZE       860

#define MAX_MESSAGE_WORDS     512

static u8 *window;
static bool model_present;
static u32 emem[MAX_MESSAGE_WORDS];
static u32 emem_next;                  /* where the next pushed word lands  */
static bool emem_writing;
static bool started_gsp;
static u32 tail_seen;
static bool tail_before_head;

static char faults[256];
static int  fault_count;

static void fault(const char *what) {
    fault_count++;
    size_t used = strlen(faults);
    if (used + strlen(what) + 3 < sizeof faults)
        snprintf(faults + used, sizeof faults - used, "%s%s",
                 used ? "; " : "", what);
}

/* Pull a field out the way the headers name it: high bit, then low. */
static u32 field(u32 word, int hi, int lo) {
    u32 width = (u32)(hi - lo + 1);
    return (word >> lo) & ((1u << width) - 1u);
}

/* Read a value out of the received bytes at a byte offset, so this model
 * inspects the message as it arrived rather than through a structure that
 * would impose its own idea of the layout. */
static u64 rd64_at(u32 off) {
    const u8 *p = (const u8 *)emem + off;
    u64 v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static u32 rd32_at(u32 off) {
    const u8 *p = (const u8 *)emem + off;
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u16 rd16_at(u32 off) {
    const u8 *p = (const u8 *)emem + off;
    return (u16)((u32)p[0] | ((u32)p[1] << 8));
}

/* Everything the message has to be right about, checked when the doorbell
 * rings.
 *
 * The field offsets below are computed from the published field order and the
 * ordinary alignment rules, written out one line at a time rather than taken
 * from a structure - so that a structure in the driver which has drifted is
 * caught here instead of matching itself. */
/* The PACKED layout, as `#pragma pack(1)` in the original and `#[repr(C,
 * packed)]` in its Rust rewrite both declare.  An earlier version of this
 * model applied ordinary alignment to these offsets - the same wrong
 * assumption the driver had made, arrived at independently but from the same
 * habit of mind, which is exactly the way a model fails to catch anything.
 * The offsets are hand sums of the field sizes with no padding anywhere. */
#define OFF_MCTP        0
#define OFF_NVDM        4
#define OFF_VERSION     8
#define OFF_SIZE        10
#define OFF_FMC         12
#define OFF_FRTS_SYS    20
#define OFF_FRTS_SYSSZ  28
#define OFF_FRTS_VID    32
#define OFF_FRTS_VIDSZ  40
#define OFF_HASH        44
#define OFF_PUBKEY      92
#define OFF_SIGNATURE   476
#define OFF_BOOTARGS    860
#define EXPECT_TOTAL    868

static void examine_message(void) {
    u32 mctp = rd32_at(OFF_MCTP);
    u32 nvdm = rd32_at(OFF_NVDM);

    if (field(mctp, 3, 0) != EXPECT_MCTP_VERSION) fault("MCTP version wrong");
    if (field(mctp, 31, 31) != EXPECT_MCTP_SOM)   fault("start-of-message not set");
    if (field(mctp, 30, 30) != EXPECT_MCTP_EOM)   fault("end-of-message not set");

    if (field(nvdm, 6, 0) != EXPECT_MSG_TYPE)      fault("not a vendor message");
    if (field(nvdm, 23, 8) != EXPECT_VENDOR_ID)    fault("vendor is not NVIDIA");
    if (field(nvdm, 31, 24) != EXPECT_NVDM_TYPE)   fault("not a chain of trust");

    if (rd16_at(OFF_VERSION) != EXPECT_COT_VERSION) fault("payload version wrong");

    /* The size the message declares of itself.  A driver whose structure has
     * drifted from the firmware's declares a different number here, and this
     * is the one field that catches that before the card does. */
    u16 size = rd16_at(OFF_SIZE);
    if (size != EXPECT_COT_SIZE) {
        static char said[64];
        snprintf(said, sizeof said, "payload says it is %u bytes, expected %u",
                 size, EXPECT_COT_SIZE);
        fault(said);
    }

    if (!rd64_at(OFF_FMC))      fault("no firmware address");
    if (!rd64_at(OFF_BOOTARGS)) fault("no boot arguments address");

    /* The three proofs have to have actually arrived.  An all-zero hash is
     * what a driver that allocated the record and forgot to fill it in
     * produces, and it is indistinguishable from a real one by length. */
    bool any;
    any = false;
    for (u32 i = 0; i < 48; i++) if (((const u8 *)emem)[OFF_HASH + i]) any = true;
    if (!any) fault("the hash is empty");

    any = false;
    for (u32 i = 0; i < 97; i++) if (((const u8 *)emem)[OFF_PUBKEY + i]) any = true;
    if (!any) fault("the public key is empty");

    /* And the rest of the 384-byte box past the 97-byte key must be zero: a
     * driver that copied a whole box rather than a key would leave whatever
     * followed the key in memory here. */
    for (u32 i = 97; i < 384; i++)
        if (((const u8 *)emem)[OFF_PUBKEY + i]) { fault("data past the key"); break; }

    any = false;
    for (u32 i = 0; i < 96; i++) if (((const u8 *)emem)[OFF_SIGNATURE + i]) any = true;
    if (!any) fault("the signature is empty");

    /* The doorbell's own rule: the tail names the last word's offset, which is
     * four less than the size, and it is set before the head. */
    if (tail_seen != EXPECT_TOTAL - 4) {
        static char said[64];
        snprintf(said, sizeof said, "tail is %u, expected %u",
                 tail_seen, EXPECT_TOTAL - 4);
        fault(said);
    }
    if (!tail_before_head) fault("the head was rung before the tail was set");

    if (!fault_count) started_gsp = true;

    /* Answer the way the real processor answers.  It consumes a command by
     * moving QUEUE_HEAD to QUEUE_TAIL, then places a 20-byte single-packet
     * FSP_RESPONSE in the same EMEM channel and advertises it through MSGQ.
     *
     * This is written into the register window rather than returned from a
     * read hook, because the driver reads these registers straight out of the
     * window - which is what it will do on a real card too.  A model that
     * answered through a side channel would be testing a read path the driver
     * does not take. */
    if (window) {
        *(volatile u32 *)(window + NV_PFSP_QUEUE_HEAD(NV_FSP_QUEUE_BOOT)) = tail_seen;

        emem[0] = 0xC0000000u; /* SOM | EOM; the version/reserved bits are zero */
        emem[1] = (0x15u << 24) | (EXPECT_VENDOR_ID << 8) | EXPECT_MSG_TYPE;
        emem[2] = 1;                 /* task ID (opaque to the host) */
        emem[3] = EXPECT_NVDM_TYPE;  /* command being answered: COT */
        emem[4] = 0;                 /* success */
        *(volatile u32 *)(window + NV_PFSP_MSGQ_HEAD(NV_FSP_QUEUE_BOOT)) = 0;
        *(volatile u32 *)(window + NV_PFSP_MSGQ_TAIL(NV_FSP_QUEUE_BOOT)) = 16;
    }
}

/* ------------------------------------------------------------ the registers */

/* Called by the card's register write path when this model is attached. */
void nv_fsp_model_write(u32 offset, u32 value) {
    if (!model_present) return;
    if (offset == NV_PFSP_EMEMC(NV_FSP_QUEUE_BOOT)) {
        emem_writing = (value & NV_PFSP_EMEMC_WRITE) != 0;
        emem_next = (value & 0xFFFFFFu) / 4;
        return;
    }

    if (offset == NV_PFSP_EMEMD(NV_FSP_QUEUE_BOOT)) {
        if (!emem_writing) { fault("data pushed without opening the window"); return; }
        if (emem_next < MAX_MESSAGE_WORDS) emem[emem_next++] = value;
        return;
    }

    if (offset == NV_PFSP_QUEUE_TAIL(NV_FSP_QUEUE_BOOT)) {
        tail_seen = value;
        tail_before_head = true;
        return;
    }

    if (offset == NV_PFSP_QUEUE_HEAD(NV_FSP_QUEUE_BOOT)) {
        examine_message();
        return;
    }
}

/* Put the next EMEM word in the memory-mapped data register immediately before
 * nv_rd32 takes it.  This is the same arrangement the Falcon model uses for
 * its auto-incrementing DMEM port, and lets both the outbound read-back and the
 * inbound response exercise the real register-read path. */
void nv_fsp_model_read(u32 offset) {
    if (!model_present || !window ||
        offset != NV_PFSP_EMEMD(NV_FSP_QUEUE_BOOT) || emem_writing)
        return;
    u32 word = emem_next < MAX_MESSAGE_WORDS ? emem[emem_next] : 0;
    *(volatile u32 *)(window + offset) = word;
    emem_next++;
}

/* ------------------------------------------------------------- the harness */

bool nv_fsp_model_attach(nv_card_t *c) {
    if (!window) {
        window = kzalloc(WINDOW_BYTES);
        if (!window) return false;
    }
    memset(window, 0, WINDOW_BYTES);
    memset(emem, 0, sizeof emem);
    faults[0] = '\0';
    fault_count = 0;
    emem_next = 0;
    emem_writing = false;
    started_gsp = false;
    tail_seen = 0;
    tail_before_head = false;

    c->regs = (volatile u8 *)window;
    c->regs_size = WINDOW_BYTES;
    c->modelled = true;
    model_present = true;

    /* The processor this models finished its own secure boot long before any
     * driver arrived, and says so where the driver must look first. */
    c->chipset = 0x1B3;                                  /* a GB203 */
    *(volatile u32 *)(window + 0x00ad00bcu) = 0x000000FFu;
    return true;
}

void nv_fsp_model_detach(void) { model_present = false; }

int nv_fsp_model_faults(char *out, size_t cap) {
    if (out && cap) strlcpy(out, faults[0] ? faults : "none", cap);
    return fault_count;
}

bool nv_fsp_model_started_gsp(void) { return started_gsp; }
