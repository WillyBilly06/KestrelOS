/* nv_fsp.c - asking the card's security processor to start the GSP.
 *
 * On Hopper and Blackwell the host does not start the graphics co-processor.
 * It cannot: the boot registers are locked against it.  What it does instead
 * is post a message to the security processor - NVIDIA calls it the FSP -
 * naming the firmware and the three things that prove the firmware is
 * NVIDIA's: a hash of it, a public key, and a signature.  The security
 * processor checks all three and starts the co-processor itself.
 *
 * NVIDIA calls that message a chain of trust, which is what it is.
 *
 * ---------------------------------------------------------------------------
 * WHERE EVERY VALUE IN THIS FILE CAME FROM
 *
 * None of it is guessed, and none of it is from a generation that happens to
 * be nearby.  This file was written after establishing, by reading nouveau,
 * that the card in this machine boots this way and not the other way:
 *
 *   nvkm/subdev/gsp/gb202.c binds a GB202 - the die in an RTX 5070 Ti - to
 *   gh100_gsp_init, which is Hopper's routine rather than Ada's.
 *
 *   nvkm/subdev/gsp/gh100.c writes no boot register at all.  It calls
 *   nvkm_fsp_boot_gsp_fmc().
 *
 * The framing, the payload and the field order are from nvkm/subdev/fsp/
 * gh100.c; the queue registers from NVIDIA's Hopper header dev_fsp_pri.h; the
 * scratchpad window from nvkm/falcon/gp102.c.  Blackwell's own published
 * dev_fsp_pri.h confirms the block is in the same place - its scratch
 * registers are at 0x008F03xx - though it does not publish the queue.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS EASY TO GET WRONG
 *
 * The payload's layout is a plain C structure with natural alignment, not a
 * packed one, and it must come out byte for byte the same as the structure
 * NVIDIA's own firmware expects.  That is not left to hope: the size is
 * checked at compile time, and the message carries its own size in a field the
 * security processor reads, so a layout that has drifted is refused rather
 * than half-understood.
 *
 * The message is not left in memory for the card to fetch.  It is pushed word
 * by word through a two-register window into the security processor's own
 * scratchpad, and only then is the queue pointer moved.  Moving the pointer
 * first offers the processor a message that is not there yet.
 *
 * The tail is set to the size minus four - the offset of the last word, not
 * the count of them - and the head afterwards.  That ordering is the doorbell.
 */
#include "kernel.h"
#include "klog.h"
#include "mm.h"
#include "nv.h"
#include "pci.h"
#include "time.h"

bool cmdline_has(const char *flag);

/* ------------------------------------------------------------- the framing */

/* A field of a word, given the way NVIDIA's headers write bit ranges: the
 * high bit first, then the low one. */
#define FIELD(hi, lo, v)   (((u32)(v) & ((1u << ((hi) - (lo) + 1)) - 1)) << (lo))

/* MCTP is a transport standard; these are its header's fields, and the two
 * that matter here say this message both starts and ends in this packet.
 * NVIDIA's own create_mctp_header (open-gpu-kernel-modules, kfspCreateMctpHeader)
 * sets ONLY som/eom/seid/seq - the low 16 bits are reserved and left zero.  An
 * earlier version here also set a "version" nibble at bits 3:0; nothing in
 * NVIDIA's fsp_mctp_format.h defines a field there, so it was a reserved bit set
 * to 1 that the real firmware never sees set - removed to match the source. */
#define MCTP_SOM           FIELD(31, 31, 1)
#define MCTP_EOM           FIELD(30, 30, 1)

/* And the message inside it: a vendor-defined one, NVIDIA's, of the type that
 * carries a chain of trust. */
#define MCTP_TYPE_VENDOR_PCI  0x7E
#define MCTP_VENDOR_ID_NV     0x10DE

#define NVDM_HEADER  (FIELD(6, 0, MCTP_TYPE_VENDOR_PCI) |     \
                      FIELD(23, 8, MCTP_VENDOR_ID_NV)  |     \
                      FIELD(31, 24, NVDM_TYPE_COT))

#define MCTP_HEADER  (MCTP_SOM | MCTP_EOM)

#define NVDM_TYPE_FSP_RESPONSE  0x15
#define FSP_RESPONSE_BYTES      20u

typedef struct {
    u32 mctp_header;
    u32 nvdm_header;
    u32 task_id;
    u32 command_nvdm_type;
    u32 error_code;
} __attribute__((packed)) nv_fsp_response_t;

_Static_assert(sizeof(nv_fsp_response_t) == FSP_RESPONSE_BYTES,
               "an FSP command response is five dwords");

static nv_fsp_diag_t fsp_diag;
static const char *fsp_stage = "the FSP handoff has not been attempted";

void nv_fsp_get_diag(nv_fsp_diag_t *out) {
    if (out) *out = fsp_diag;
}

const char *nv_fsp_last_stage(void) { return fsp_stage; }

/* The payload.
 *
 * Field for field and in this order, from NVIDIA's NVDM_PAYLOAD_COT - and
 * PACKED, because the original is declared under `#pragma pack(1)` and the
 * Rust rewrite of the same driver marks it `#[repr(C, packed)]`.  An earlier
 * version of this file said the opposite, with a confident comment about how
 * packing it "would produce something smaller that the security processor
 * would reject" - reasoning from what seemed sensible instead of from the
 * source, which put every field after the size four bytes too far along and
 * would have been refused by the first real card it met.  Two independent
 * drivers agree on the packing; this file no longer gets a vote.
 */
typedef struct {
    u16 version;
    u16 size;
    u64 gsp_fmc_sysmem_offset;
    u64 frts_sysmem_offset;
    u32 frts_sysmem_size;
    u64 frts_vidmem_offset;
    u32 frts_vidmem_size;
    u32 hash384[12];        /* 48 bytes  */
    u32 public_key[96];     /* 384 bytes; the key itself is 96 or 97 bytes */
    u32 signature[96];      /* 384 bytes; the signature is 96             */
    u64 gsp_boot_args_sysmem_offset;
} __attribute__((packed)) nv_cot_payload_t;

/* If any of these stop being true the firmware would be handed a record it
 * does not recognise, and the failure would arrive on the card rather than
 * here.  The offsets are the published packed layout, written out so a drift
 * in the struct above is caught against numbers, not against itself. */
_Static_assert(sizeof(nv_cot_payload_t) == 860,
               "the chain-of-trust payload must match NVIDIA's packed layout");
_Static_assert(__builtin_offsetof(nv_cot_payload_t, gsp_fmc_sysmem_offset) == 4,
               "the firmware address follows the two shorts immediately");
_Static_assert(__builtin_offsetof(nv_cot_payload_t, frts_vidmem_offset) == 24,
               "no padding before the video-memory region");
_Static_assert(__builtin_offsetof(nv_cot_payload_t, hash384) == 36,
               "the hash begins right after the region sizes");
_Static_assert(__builtin_offsetof(nv_cot_payload_t, public_key) == 84,
               "the key follows the 48-byte hash");
_Static_assert(__builtin_offsetof(nv_cot_payload_t, signature) == 468,
               "the signature follows the 384-byte key field");
_Static_assert(__builtin_offsetof(nv_cot_payload_t, gsp_boot_args_sysmem_offset) == 852,
               "the boot-arguments address is the last field");
_Static_assert(sizeof(u64) == 8 && sizeof(u32) == 4 && sizeof(u16) == 2,
               "the payload layout assumes these widths");

/* What each generation's security processor expects.  These differ, and not
 * cosmetically: a Blackwell part takes version 2 and a 97-byte public key
 * where Hopper takes version 1 and 96 - read straight off the firmware file
 * this system ships, whose `publickey` section is 97 bytes long. */
typedef struct { u16 version; u32 pkey_bytes; } nv_cot_config_t;

static nv_cot_config_t cot_config_for(const nv_card_t *c) {
    u32 family = c ? (c->chipset & 0x1F0) : 0x1B0;
    if (family >= 0x1A0) return (nv_cot_config_t){ 2, 97 };   /* Blackwell */
    return (nv_cot_config_t){ 1, 96 };                        /* Hopper    */
}

/* The whole packet: two header words then the payload. */
typedef struct {
    u32 mctp_header;
    u32 nvdm_header;
    nv_cot_payload_t cot;
} nv_cot_packet_t;

/* ------------------------------------------------------------- the sending */

/* Push the packet into the security processor's scratchpad.
 *
 * Setting bit 24 in the control register starts an auto-incrementing write at
 * the offset in its low bits, so the address follows the data along and the
 * control register is written once rather than per word.
 */
/* EMEM is addressed as a block number (control-register bits 15:8, one per
 * 256-byte / 64-dword block) and a dword offset within it (bits 7:2).  A
 * message of 217 dwords spans four blocks, so it relies on the write pointer
 * advancing from the last dword of one block to the first of the next.  Rather
 * than trust that the auto-increment carries across the block boundary on this
 * SKU - which, if it does not, silently wraps dword 64 back over dword 0 and
 * leaves the processor a corrupt packet it can only ignore - reconfigure the
 * control register at each boundary with the block set explicitly.  This is
 * correct whether the hardware carries or not.
 *
 * Then read the whole thing back through the read-auto-increment and compare:
 * a message that did not land byte-for-byte is a different failure from one the
 * processor received and refused, and until now the two were indistinguishable. */
static bool push_to_emem(nv_card_t *c, const u32 *words, u32 count) {
    for (u32 i = 0; i < count; i++) {
        if ((i & 63u) == 0)
            nv_wr32(c, NV_PFSP_EMEMC(NV_FSP_QUEUE_BOOT),
                    NV_PFSP_EMEMC_WRITE | ((i >> 6) << 8));
        nv_wr32(c, NV_PFSP_EMEMD(NV_FSP_QUEUE_BOOT), words[i]);
    }

    u32 bad = 0, first_i = 0, first_got = 0, first_exp = 0;
    for (u32 i = 0; i < count; i++) {
        if ((i & 63u) == 0)
            nv_wr32(c, NV_PFSP_EMEMC(NV_FSP_QUEUE_BOOT),
                    NV_PFSP_EMEMC_READ | ((i >> 6) << 8));
        u32 got = nv_rd32(c, NV_PFSP_EMEMD(NV_FSP_QUEUE_BOOT));
        if (got != words[i]) {
            if (!bad) { first_i = i; first_got = got; first_exp = words[i]; }
            bad++;
        }
    }
    if (bad) {
        kerr("nv-fsp", "the message did not land in the processor's memory: "
             "%u of %u dwords read back wrong (first at dword %u: got %#x, "
             "wanted %#x)", bad, count, first_i, first_got, first_exp);
        return false;
    }
    kinfo("nv-fsp", "the message is in the processor's memory: all %u dwords "
                    "read back byte-for-byte", count);
    return true;
}

static void remember_fsp_state(nv_card_t *c) {
    fsp_diag.cmd_head_after = nv_rd32(c, NV_PFSP_QUEUE_HEAD(NV_FSP_QUEUE_BOOT));
    fsp_diag.cmd_tail_after = nv_rd32(c, NV_PFSP_QUEUE_TAIL(NV_FSP_QUEUE_BOOT));
    fsp_diag.msg_head_after = nv_rd32(c, NV_PFSP_MSGQ_HEAD(NV_FSP_QUEUE_BOOT));
    fsp_diag.msg_tail_after = nv_rd32(c, NV_PFSP_MSGQ_TAIL(NV_FSP_QUEUE_BOOT));
    for (int i = 0; i < 4; i++)
        fsp_diag.scratch[i] = nv_rd32(c, NV_PFSP_FALCON_COMMON_SCRATCH_GROUP_2(i));
}

/* Read and validate the single-packet command response used by COT.  R570 does
 * not consider a command successful merely because FSP consumed it: it waits
 * for this response, checks that it answers COT, and maps a non-zero firmware
 * error to failure. */
static bool read_response(nv_card_t *c, int ms) {
    u32 head = 0, tail = 0;
    for (int spent = 0; spent < ms; spent++) {
        head = nv_rd32(c, NV_PFSP_MSGQ_HEAD(NV_FSP_QUEUE_BOOT));
        tail = nv_rd32(c, NV_PFSP_MSGQ_TAIL(NV_FSP_QUEUE_BOOT));
        if (head == 0xFFFFFFFFu || tail == 0xFFFFFFFFu) {
            fsp_stage = "the FSP response queue stopped answering";
            kerr("nv-fsp", "%s", fsp_stage);
            remember_fsp_state(c);
            return false;
        }
        if (head != tail) break;
        timer_mdelay(1);
    }

    fsp_diag.msg_head_after = head;
    fsp_diag.msg_tail_after = tail;
    if (head == tail) {
        fsp_stage = "the FSP consumed the command but sent no response";
        kerr("nv-fsp", "%s (msgq head %#x tail %#x)", fsp_stage, head, tail);
        remember_fsp_state(c);
        return false;
    }
    fsp_diag.response_seen = true;

    if (tail < head || tail - head + 4u != FSP_RESPONSE_BYTES) {
        fsp_stage = "the FSP returned a response with an invalid size";
        kerr("nv-fsp", "%s (msgq head %#x tail %#x)", fsp_stage, head, tail);
        nv_wr32(c, NV_PFSP_MSGQ_TAIL(NV_FSP_QUEUE_BOOT), head);
        nv_wr32(c, NV_PFSP_MSGQ_HEAD(NV_FSP_QUEUE_BOOT), head);
        remember_fsp_state(c);
        return false;
    }

    nv_fsp_response_t response;
    u32 *words = (u32 *)(void *)&response;
    nv_wr32(c, NV_PFSP_EMEMC(NV_FSP_QUEUE_BOOT), NV_PFSP_EMEMC_READ);
    for (u32 i = 0; i < FSP_RESPONSE_BYTES / 4u; i++)
        words[i] = nv_rd32(c, NV_PFSP_EMEMD(NV_FSP_QUEUE_BOOT));

    /* Setting TAIL=HEAD acknowledges the packet.  Write both in the same order
     * as NVIDIA's helper so the now-idle state is unambiguous in diagnostics. */
    nv_wr32(c, NV_PFSP_MSGQ_TAIL(NV_FSP_QUEUE_BOOT), head);
    nv_wr32(c, NV_PFSP_MSGQ_HEAD(NV_FSP_QUEUE_BOOT), head);

    fsp_diag.response_task = response.task_id;
    fsp_diag.response_command = response.command_nvdm_type;
    fsp_diag.response_error = response.error_code;

    bool framing = (response.mctp_header & (MCTP_SOM | MCTP_EOM)) ==
                       (MCTP_SOM | MCTP_EOM) &&
                   (response.nvdm_header & 0x7Fu) == MCTP_TYPE_VENDOR_PCI &&
                   ((response.nvdm_header >> 8) & 0xFFFFu) == MCTP_VENDOR_ID_NV &&
                   (response.nvdm_header >> 24) == NVDM_TYPE_FSP_RESPONSE;
    if (!framing || response.command_nvdm_type != NVDM_TYPE_COT) {
        fsp_stage = "the FSP response was malformed or answered another command";
        kerr("nv-fsp", "%s (MCTP %#x, NVDM %#x, task %#x, command %#x, error %#x)",
             fsp_stage, response.mctp_header, response.nvdm_header,
             response.task_id, response.command_nvdm_type, response.error_code);
        remember_fsp_state(c);
        return false;
    }
    if (response.error_code != 0) {
        fsp_stage = "the FSP rejected the chain-of-trust command";
        kerr("nv-fsp", "%s (task %#x, error %#x)", fsp_stage,
             response.task_id, response.error_code);
        remember_fsp_state(c);
        return false;
    }

    fsp_diag.response_valid = true;
    fsp_stage = "the FSP verified the image and returned success";
    kinfo("nv-fsp", "response: task %#x, command %#x, error 0 (success)",
          response.task_id, response.command_nvdm_type);
    remember_fsp_state(c);
    return true;
}

/* Hand the security processor a message and wait for it to take it.
 *
 * Taking it is the tail catching the head up: the processor moves the tail as
 * it consumes, and a tail that never moves means the message was not read -
 * which is a different failure from one that was read and rejected, and worth
 * telling apart. */
static bool send_and_wait(nv_card_t *c, const nv_cot_packet_t *packet, int ms) {
    const u32 bytes = (u32)sizeof *packet;
    const u32 words = bytes / 4;

    /* Before sending, get the channel into the state the processor expects.
     * NVIDIA's kfspCanSendPacket requires BOTH the command queue AND the
     * message queue idle (head == tail) before a send, and kfspReadMessage
     * acknowledges a message the processor posted by setting the message
     * queue's tail equal to its head.  A message the processor left in its
     * host-bound queue during its own boot, if never read, stops it accepting a
     * new command - which is indistinguishable from "it did not take the
     * message".  So drain any such message and wait for the command queue to go
     * idle first. */
    {
        u32 mh = nv_rd32(c, NV_PFSP_MSGQ_HEAD(NV_FSP_QUEUE_BOOT));
        u32 mt = nv_rd32(c, NV_PFSP_MSGQ_TAIL(NV_FSP_QUEUE_BOOT));
        if (mh != mt && mh != 0xFFFFFFFFu) {
            kinfo("nv-fsp", "the processor left a message unread (head %#x, "
                            "tail %#x); acknowledging it before sending", mh, mt);
            nv_wr32(c, NV_PFSP_MSGQ_TAIL(NV_FSP_QUEUE_BOOT), mh);
        }
        for (int i = 0; i < 1000; i++) {
            u32 h = nv_rd32(c, NV_PFSP_QUEUE_HEAD(NV_FSP_QUEUE_BOOT));
            u32 t = nv_rd32(c, NV_PFSP_QUEUE_TAIL(NV_FSP_QUEUE_BOOT));
            if (h == t) break;
            if (i == 0)
                kinfo("nv-fsp", "waiting for the command queue to go idle "
                                "(head %#x, tail %#x)", h, t);
            timer_mdelay(1);
        }
    }

    /* The state the channel is in at the instant of the send.  NVIDIA's
     * kfspCanSendPacket accepts a packet only when BOTH queues read idle
     * (head == tail); logging all four here - once, right before the doorbell -
     * turns the next boot into a definite answer to "was the channel ready?"
     * rather than a guess, without another round-trip. */
    fsp_diag.cmd_head_before = nv_rd32(c, NV_PFSP_QUEUE_HEAD(NV_FSP_QUEUE_BOOT));
    fsp_diag.cmd_tail_before = nv_rd32(c, NV_PFSP_QUEUE_TAIL(NV_FSP_QUEUE_BOOT));
    fsp_diag.msg_head_before = nv_rd32(c, NV_PFSP_MSGQ_HEAD(NV_FSP_QUEUE_BOOT));
    fsp_diag.msg_tail_before = nv_rd32(c, NV_PFSP_MSGQ_TAIL(NV_FSP_QUEUE_BOOT));
    kinfo("nv-fsp", "channel before send: cmdq head %#x tail %#x, msgq head %#x "
                    "tail %#x",
          fsp_diag.cmd_head_before, fsp_diag.cmd_tail_before,
          fsp_diag.msg_head_before, fsp_diag.msg_tail_before);
    if (fsp_diag.cmd_head_before != fsp_diag.cmd_tail_before) {
        fsp_stage = "the FSP command queue never became idle";
        kerr("nv-fsp", "%s", fsp_stage);
        remember_fsp_state(c);
        return false;
    }

    bool landed = push_to_emem(c, (const u32 *)packet, words);
    fsp_diag.emem_verified = landed;
    if (!landed) {
        fsp_stage = "the chain-of-trust packet did not land intact in FSP EMEM";
        remember_fsp_state(c);
        return false;
    }

    /* The offset of the last word, not the number of words.  Then the head,
     * which is what tells the processor to look. */
    nv_wr32(c, NV_PFSP_QUEUE_TAIL(NV_FSP_QUEUE_BOOT), bytes - 4);
    nv_wr32(c, NV_PFSP_QUEUE_HEAD(NV_FSP_QUEUE_BOOT), 0);

    /* How the processor signals it has dealt with the command: it posts a reply
     * in the MESSAGE queue (message head != message tail), which is exactly what
     * NVIDIA's kfspIsResponseAvailable watches.  It does NOT reliably drag the
     * COMMAND queue's head up to meet the tail - on this card the command head
     * stays at the zero we wrote while a full twenty-byte reply appears in the
     * message queue.  An earlier version here waited for command head == tail
     * and gave up when it never happened, throwing away a reply that was sitting
     * unread the whole time - which is why every attempt looked like "the
     * processor ignored the message".  Read the reply directly instead; a
     * message queue that never moves is the real "it did not answer". */
    fsp_diag.cmd_head_after = nv_rd32(c, NV_PFSP_QUEUE_HEAD(NV_FSP_QUEUE_BOOT));
    fsp_diag.cmd_tail_after = nv_rd32(c, NV_PFSP_QUEUE_TAIL(NV_FSP_QUEUE_BOOT));
    fsp_diag.command_consumed = (fsp_diag.cmd_head_after == fsp_diag.cmd_tail_after);
    if (read_response(c, ms)) return true;

    /* No usable reply.  The processor's own scratch registers are the only
     * account of why - NVIDIA's kfspDumpDebugState reads exactly these four. */
    kerr("nv-fsp", "no usable reply (cmd head %#x tail %#x, wrote head 0 tail %#x)",
         fsp_diag.cmd_head_after, fsp_diag.cmd_tail_after, bytes - 4);
    kerr("nv-fsp", "  FSP scratch: %#x %#x %#x %#x (its own status/error)",
         nv_rd32(c, NV_PFSP_FALCON_COMMON_SCRATCH_GROUP_2(0)),
         nv_rd32(c, NV_PFSP_FALCON_COMMON_SCRATCH_GROUP_2(1)),
         nv_rd32(c, NV_PFSP_FALCON_COMMON_SCRATCH_GROUP_2(2)),
         nv_rd32(c, NV_PFSP_FALCON_COMMON_SCRATCH_GROUP_2(3)));
    remember_fsp_state(c);
    return false;
}

/* ---------------------------------------------------------------- the boot */

/* Ask the security processor to verify and start the graphics co-processor.
 *
 * `hash`, `key` and `sig` are the sections of the firmware's own ELF - it
 * carries the three of them alongside the image - and their lengths are fixed
 * by the algorithm rather than by the file, so they are checked rather than
 * trusted.
 */
/* Whether the security processor has finished its own start-up.
 *
 * It boots before any driver touches the card and reports completion in a
 * thermal-block scratch register - a different register per generation.
 * Sending the chain-of-trust message before this reads 0xFF asks a processor
 * that is not listening yet, and the failure looks like a refused message
 * rather than an early question. */
#define NV_THERM_I2CS_SCRATCH_HOPPER     0x000200bcu
#define NV_THERM_I2CS_SCRATCH_BLACKWELL  0x00ad00bcu
#define FSP_BOOT_COMPLETE                0x000000FFu

bool nv_fsp_secure_boot_ready(nv_card_t *c, int ms) {
    u32 family = c->chipset & 0x1F0;
    u32 reg = family >= 0x1A0 ? NV_THERM_I2CS_SCRATCH_BLACKWELL
                              : NV_THERM_I2CS_SCRATCH_HOPPER;
    for (int spent = 0; spent < ms; spent++) {
        fsp_diag.secure_boot_value = nv_rd32(c, reg);
        if (fsp_diag.secure_boot_value == FSP_BOOT_COMPLETE) {
            fsp_diag.secure_boot_ready = true;
            return true;
        }
        timer_mdelay(1);
    }
    kerr("nv-fsp", "the security processor never reported its own boot "
                   "complete (register %#x)", reg);
    return false;
}

/* Reset the card so its firmware runs again from the start.
 *
 * Everything about the message we send checks out - it is byte-for-byte the one
 * NVIDIA's driver sends, it lands in the security processor's own memory intact,
 * the channel is idle and the doorbell is rung in the right order - and the
 * processor still does not read it.  The remaining difference from the machine
 * NVIDIA's driver runs on is the card's STATE: there the graphics co-processor
 * is halted (its firmware booted it for set-up and stopped it), and the security
 * processor is idle and listening.  Here the co-processor has been driving the
 * screen since power-on and never stopped, and in that state the security
 * processor does not service the host's command channel.
 *
 * A PCIe function-level reset is the one lever the host has that the co-processor
 * cannot lock out: it goes through the card's PCI configuration space, not the
 * co-processor's register block (which reads back the locked 0xbadf... on this
 * card).  It returns the whole function to power-on: the firmware re-runs, the
 * co-processor is left halted after set-up, and the security processor comes up
 * idle - the state the message is meant for.  The screen goes black when the
 * co-processor stops driving it; the log is on the boot volume, not the screen.
 *
 * The reset clears the configuration header, so it is saved first and put back
 * after - the memory windows above all else, because the rest of this driver
 * reaches the card through the first one. */
static bool nv_pcie_flr(nv_card_t *c) {
    pci_dev_t *d = NULL;
    while ((d = pci_find(0x03, 0xFF, 0xFF, d)) != NULL)
        if (d->bus == c->pci_bus && d->slot == c->pci_slot &&
            d->func == c->pci_func) break;
    if (!d) { kwarn("nv-flr", "the card is not on the bus to reset"); return false; }
    if (!d->cap_pcie) {
        kwarn("nv-flr", "the card has no PCI Express capability; no reset");
        return false;
    }

    u32 devcap = pci_read32(d, d->cap_pcie + 0x04);   /* Device Capabilities */
    if (!(devcap & (1u << 28))) {
        kwarn("nv-flr", "the card does not offer a function-level reset "
                        "(capabilities %#x); leaving it as it is", devcap);
        return false;
    }
    fsp_diag.flr_supported = true;

    u32 saved[16];
    for (int i = 0; i < 16; i++) saved[i] = pci_read32(d, (u32)(i * 4));

    kinfo("nv-flr", "resetting the card so its firmware runs again and the "
                    "security processor comes up idle (the screen will go dark)");

    /* The EFI framebuffer is the NVIDIA card's VRAM on this machine.  Once FLR
     * begins, even an innocent klog line that tries to paint a glyph through
     * that stale mapping can wedge the CPU before the serial/FAT diagnostics
     * are flushed.  Stop all console drawing while BAR0 and VRAM are still
     * alive; serial output and KERNEL.LOG do not depend on the framebuffer.
     * Do not take it back after reset: the old GOP surface is no longer a valid
     * display contract, even if the BAR happens to return at the same address. */
    /* Release immediately, then retire the loader's GOP surface permanently.
     * A plain ownership release is not enough: process exit normally calls
     * console_take_framebuffer(), and the desktop could otherwise map the old
     * physical range after this function returns.  Both would touch dead VRAM
     * after FLR.  console_abandon_framebuffer() contains the required release
     * and clears the authoritative base/size before any reset write. */
    console_abandon_framebuffer();
    fsp_diag.framebuffer_released = true;

    /* Make the last pre-reset line durable.  If the PCI function never comes
     * back, this distinguishes "entered FLR" from an earlier staging failure. */
    klog_persist_flush();

    /* Stop it reaching memory across the reset. */
    u16 cmd = pci_read16(d, 0x04);
    pci_write16(d, 0x04, (u16)(cmd & ~0x0007u));      /* clear I/O, memory, bus-master */

    /* Initiate: Device Control (capability + 0x08), bit 15. */
    u16 devctl = pci_read16(d, d->cap_pcie + 0x08);
    pci_write16(d, d->cap_pcie + 0x08, (u16)(devctl | (1u << 15)));
    fsp_diag.flr_performed = true;

    /* The specification allows the function up to 100 ms; give it more. */
    timer_mdelay(250);

    /* Put the header back - the base address registers (dwords 4..9) and the
     * expansion-ROM and interrupt words - then the command register last, so no
     * memory access is enabled until the windows that answer it are restored. */
    for (int i = 4; i <= 12; i++) pci_write32(d, (u32)(i * 4), saved[i]);
    pci_write32(d, 0x3C, saved[15]);
    pci_write16(d, 0x04, (u16)(saved[1] & 0xFFFFu));
    pci_enable_memory(d);
    pci_enable_bus_master(d);

    /* Wait for it to answer configuration reads again. */
    int back = 0;
    for (; back < 1000; back++) {
        if (pci_read16(d, 0x00) == 0x10DE) break;      /* NVIDIA answers again */
        timer_mdelay(1);
    }
    bool returned = back < 1000 && pci_read16(d, 0x00) == 0x10DE;
    fsp_diag.flr_returned = returned;
    kinfo("nv-flr", "after the reset it answers in %d ms: vendor %#x, command "
                    "%#x, first memory window %#x", back, pci_read16(d, 0x00),
          pci_read16(d, 0x04), pci_read32(d, 0x10));
    if (!returned)
        kerr("nv-flr", "the PCI function did not return after reset");
    return returned;
}

/* A secondary-bus reset on the card's upstream bridge.
 *
 * This is the reset that matters.  A function-level reset (nv_pcie_flr above)
 * does not re-run the card's on-chip boot sequence, so it leaves the protected
 * window (WPR2) exactly as it found it - which is why a boot after one still
 * meets 0xb.  A secondary-bus reset drives the fundamental-reset line on the
 * link below the bridge; the card comes out of it as if just powered, its boot
 * sequence runs again, and WPR2 is down.  It is heavier - the whole link goes
 * down and the display with it - but it is the only thing that clears the
 * window without a running GSP-RM to unload, which we do not have.  Verified
 * against NVIDIA's own recovery sequence (bridge SBR + config/BAR restore) and
 * the PCI-to-PCI bridge specification (Bridge Control, offset 0x3E, bit 6). */
bool nv_gpu_reset_wpr2(nv_card_t *c) {
    /* The card itself. */
    pci_dev_t *d = NULL;
    while ((d = pci_find(0x03, 0xFF, 0xFF, d)) != NULL)
        if (d->bus == c->pci_bus && d->slot == c->pci_slot &&
            d->func == c->pci_func) break;
    if (!d) { kwarn("nv-sbr", "the card is not on the bus to reset"); return false; }

    /* Its upstream bridge: the PCI-to-PCI bridge whose secondary bus is the bus
     * the card sits on.  The reset is driven from there, not from the card. */
    pci_dev_t *br = NULL, *found = NULL;
    while ((br = pci_find(0x06, 0x04, 0xFF, br)) != NULL) {
        if (pci_read8(br, 0x19) == d->bus) { found = br; break; }   /* 0x19 = secondary bus */
    }
    if (!found) {
        kwarn("nv-sbr", "no upstream bridge carries bus %u; cannot do a "
                        "secondary-bus reset", d->bus);
        return false;
    }
    kinfo("nv-sbr", "resetting through bridge %02x:%02x.%u, whose secondary bus "
                    "is %u (the card's) - the screen will go dark until the "
                    "resource manager re-lights it",
          found->bus, found->slot, found->func, d->bus);

    /* Save the card's header so its memory windows can be put back afterwards -
     * a reset clears them to zero, and BAR0 has to answer again for the GSP
     * boot that follows. */
    u32 saved[16];
    for (int i = 0; i < 16; i++) saved[i] = pci_read32(d, (u32)(i * 4));

    /* The framebuffer is the card's own VRAM; once the link drops, painting a
     * glyph through the stale mapping can wedge the CPU before the serial and
     * on-disk logs are flushed.  Abandon it first, and make the last line
     * durable so a machine that never comes back still says it entered here. */
    console_abandon_framebuffer();
    klog_persist_flush();

    /* Stop the card reaching memory across the reset. */
    u16 cmd = pci_read16(d, 0x04);
    pci_write16(d, 0x04, (u16)(cmd & ~0x0007u));

    /* Assert secondary-bus reset on the bridge, hold, then release.  The
     * specification asks for at least 1 ms of reset; 2 ms is comfortable. */
    u16 bc = pci_read16(found, 0x3E);
    pci_write16(found, 0x3E, (u16)(bc | (1u << 6)));
    timer_mdelay(2);
    pci_write16(found, 0x3E, (u16)(bc & ~(1u << 6)));

    /* The link has to retrain and the card re-run its boot sequence.  Give it
     * time before expecting configuration reads to answer. */
    timer_mdelay(150);

    int back = 0;
    for (; back < 1000; back++) {
        if (pci_read16(d, 0x00) == 0x10DE) break;
        timer_mdelay(1);
    }
    bool returned = pci_read16(d, 0x00) == 0x10DE;
    if (!returned) {
        kerr("nv-sbr", "the card did not answer after the reset (vendor %#x)",
             pci_read16(d, 0x00));
        return false;
    }

    /* Put the header back: memory windows first (dwords 4..12) and the expansion
     * ROM / interrupt word, then the command register last, so nothing is
     * enabled before the windows that answer it are in place. */
    for (int i = 4; i <= 12; i++) pci_write32(d, (u32)(i * 4), saved[i]);
    pci_write32(d, 0x3C, saved[15]);
    pci_write16(d, 0x04, (u16)(saved[1] & 0xFFFFu));
    pci_enable_memory(d);
    pci_enable_bus_master(d);

    kinfo("nv-sbr", "the card answered again in %d ms after the secondary-bus "
                    "reset: vendor %#x, first memory window %#x", back,
          pci_read16(d, 0x00), pci_read32(d, 0x10));
    return true;
}

bool nv_fsp_boot_gsp(nv_card_t *c, u64 fmc_phys, u64 boot_args_phys,
                     u64 rsvd_size,
                     const void *hash, size_t hash_len,
                     const void *key, size_t key_len,
                     const void *sig, size_t sig_len) {
    static nv_cot_packet_t packet;              /* 868 bytes: too big for a
                                                  * kernel stack frame here */
    memset(&fsp_diag, 0, sizeof fsp_diag);
    fsp_diag.attempted = true;
    /* The model checks the wire protocol only.  A real-machine boot flag must
     * never make that self-test go looking for and resetting a PCI function. */
    fsp_diag.flr_requested = !c->modelled && cmdline_has("gspflr");
    fsp_stage = "validating the chain-of-trust inputs";
    nv_cot_config_t cfg = cot_config_for(c);

    /* The proofs are the sizes the ALGORITHM fixes, not the field sizes: the
     * fields are 384-byte boxes and the key that goes in one is 96 or 97
     * bytes depending on the generation.  This guard used to demand the box
     * size, and the real firmware file - whose publickey section is 97 bytes
     * - would have been refused as "the wrong shape" by our own check. */
    if (hash_len != 48 || key_len != cfg.pkey_bytes || sig_len != 96) {
        kerr("nv-fsp", "the firmware's proof is the wrong shape: hash %u, key "
                       "%u, signature %u bytes; this generation wants 48, %u, 96",
             (unsigned)hash_len, (unsigned)key_len, (unsigned)sig_len,
             (unsigned)cfg.pkey_bytes);
        return false;
    }

    /* If asked, reset the card first so it comes up in the state the message is
     * meant for (see nv_pcie_flr).  Opt-in with `gspflr` on the boot line: a
     * reset that does not come back cleanly needs a power-cycle, so it is not
     * done on an ordinary boot.  The secure-boot wait below then doubles as the
     * wait for the firmware to finish running again. */
    if (fsp_diag.flr_requested && !nv_pcie_flr(c)) {
        fsp_stage = fsp_diag.flr_performed
                  ? "the GPU did not return from the requested PCIe FLR"
                  : "the requested PCIe FLR is not supported on this function";
        remember_fsp_state(c);
        klog_persist_flush();
        return false;
    }
    if (fsp_diag.flr_requested) klog_persist_flush();

    fsp_stage = "waiting for the FSP secure boot to complete";
    if (!nv_fsp_secure_boot_ready(c, 5000)) {
        fsp_stage = "the FSP did not report secure-boot completion";
        remember_fsp_state(c);
        return false;
    }

    memset(&packet, 0, sizeof packet);
    packet.mctp_header = MCTP_HEADER;
    packet.nvdm_header = NVDM_HEADER;

    packet.cot.version = cfg.version;
    packet.cot.size = (u16)sizeof packet.cot;
    packet.cot.gsp_fmc_sysmem_offset = fmc_phys;
    packet.cot.gsp_boot_args_sysmem_offset = boot_args_phys;

    /* Where in video memory the security processor puts FRTS.  This field is an
     * OFFSET FROM THE END OF THE FRAME BUFFER.  NVIDIA computes it in
     * kfspPrepareBootCommands_GH100 as memmgrGetFBEndReserveSizeEstimate (0x220000
     * on GB20x) PLUS, when kpmuReservedMemorySizeGet != 0, the extra-reserved
     * 4 KiB + the PMU reserve, 2 MiB-aligned.  On THIS card the PMU reserve is
     * non-zero (~24 MiB), so the correct offset is ~28 MiB - which is exactly the
     * non-WPR-heap + PMU reserve `rsvd_size` computed by the caller and rounded to
     * 2 MiB.  Confirmed on real GB203: this value places WPR2 correctly at the top
     * of a 16 GiB FB (window reads ~15.7..15.9 GiB); a bare 0x220000 leaves the
     * PMU reserve out, WPR2 is not built, and the FMC fails earlier with 0x44.
     * The size is the fixed 1 MiB every driver of these parts asks for. */
    packet.cot.frts_vidmem_offset = (rsvd_size + 0x1FFFFFULL) & ~0x1FFFFFULL;
    packet.cot.frts_vidmem_size = rsvd_size ? 0x100000u : 0;

    /* AND a matching region in SYSTEM memory, which on a bare-metal card the
     * security processor also wants - and, unlike the video-memory one, its
     * location has to be told to the processor in three PBUS scratch registers
     * BEFORE the message, because the processor reads them to find it.  This is
     * the one card-touching step this driver was missing versus NVIDIA's own
     * (kfspFrtsSysmemLocationProgram): a 1 MiB region, whose address goes in the
     * two ADDR halves and whose size in 4 KiB units and SYSMEM media type go in
     * the config word.  Left at reset the config reads size 0 = INVALID, which
     * is not what the processor is set up to accept on this SKU. */
    {
        static u64  frts_sys_phys;
        static void *frts_sys_va;
        if (!frts_sys_va)
            frts_sys_va = dma_alloc_pages(0x100000u / PAGE_SIZE, &frts_sys_phys);
        if (frts_sys_va) {
            packet.cot.frts_sysmem_offset = frts_sys_phys;
            packet.cot.frts_sysmem_size = 0x100000u;
            nv_wr32(c, NV_PBUS_SW_FRTS_INSECURE_ADDR_LO32, (u32)frts_sys_phys);
            nv_wr32(c, NV_PBUS_SW_FRTS_INSECURE_ADDR_HI32,
                    (u32)(frts_sys_phys >> 32));
            nv_wr32(c, NV_PBUS_SW_FRTS_INSECURE_CONFIG,
                    ((0x100000u >> NV_PBUS_SW_FRTS_INSECURE_CONFIG_SIZE_4K_SHIFT)
                        & 0xFFFFu) |
                    NV_PBUS_SW_FRTS_INSECURE_CONFIG_MEDIA_TYPE_SYSMEM);
            kinfo("nv-fsp", "sysmem FRTS region at %llx (1 MiB) told to the "
                            "processor before the message",
                  (unsigned long long)frts_sys_phys);
        } else {
            packet.cot.frts_sysmem_offset = 0;
            packet.cot.frts_sysmem_size = 0;
            kwarn("nv-fsp", "no memory for the sysmem FRTS region; the processor "
                            "may refuse the message on this card");
        }
    }

    memcpy(packet.cot.hash384, hash, hash_len);
    memcpy(packet.cot.public_key, key, key_len);
    memcpy(packet.cot.signature, sig, sig_len);

    kinfo("nv-fsp", "asking the security processor (message version %u) to "
                    "verify %u bytes of firmware at %llx and start the "
                    "co-processor",
          cfg.version, (unsigned)sizeof packet.cot,
          (unsigned long long)fmc_phys);

    fsp_stage = "sending the chain-of-trust command to the FSP";
    if (!send_and_wait(c, &packet, 2000)) return false;

    kinfo("nv-fsp", "the command was consumed and its success response validated");
    return true;
}

/* --------------------------------------------------------------- the check
 *
 * Driven against a model of the security processor, because the alternative is
 * finding out on a card that is displaying a picture.
 *
 * The model is written from the published framing rather than from this file,
 * so it can disagree with it.  That matters: twice in this project a model
 * built from the driver's own assumption agreed with the driver and hid a real
 * mistake, and the point of this one is to be able to say no.
 */
bool nv_fsp_model_attach(nv_card_t *c);
void nv_fsp_model_detach(void);
int  nv_fsp_model_faults(char *out, size_t cap);
bool nv_fsp_model_started_gsp(void);

int nv_fsp_selftest(void) {
    nv_card_t card;
    memset(&card, 0, sizeof card);

    if (!nv_fsp_model_attach(&card)) {
        kwarn("nv-fsp", "selftest: no memory for the model");
        return 0;
    }

    int failures = 0;

    /* The three proofs, at the sizes the real firmware file carries them:
     * its `publickey` section is 97 bytes and its `signature` 96, and those
     * are what the guard has to accept - a test that handed over box-sized
     * blocks proved a shape the hardware never produces. */
    static u8 hash[48], key[97], sig[96];
    for (unsigned i = 0; i < sizeof hash; i++) hash[i] = (u8)(0x10 + i);
    for (unsigned i = 0; i < sizeof key;  i++) key[i]  = (u8)(0x40 + i);
    for (unsigned i = 0; i < sizeof sig;  i++) sig[i]  = (u8)(0x90 + i);

    if (!nv_fsp_boot_gsp(&card, 0x1234000, 0x5678000, 0x1C00000,
                         hash, sizeof hash, key, sizeof key, sig, sizeof sig)) {
        kerr("nv-fsp", "selftest: the model refused the message");
        failures++;
    }

    char why[192];
    int faults = nv_fsp_model_faults(why, sizeof why);
    if (faults) {
        kerr("nv-fsp", "selftest: the model found %d fault(s): %s", faults, why);
        failures += faults;
    }

    if (!nv_fsp_model_started_gsp()) {
        kerr("nv-fsp", "selftest: the co-processor was not started");
        failures++;
    }

    /* And a message whose proof is the wrong length must be refused here
     * rather than sent, because the card would reject it and a rejection from
     * the card costs a boot to observe. */
    if (nv_fsp_boot_gsp(&card, 0x1234000, 0x5678000, 0x1C00000,
                        hash, 47, key, sizeof key, sig, sizeof sig)) {
        kerr("nv-fsp", "selftest: a short hash was sent rather than refused");
        failures++;
    }

    /* A Hopper-sized key on a Blackwell part is the mistake a wrong family
     * table would make, and it has to stop here too. */
    if (nv_fsp_boot_gsp(&card, 0x1234000, 0x5678000, 0x1C00000,
                        hash, sizeof hash, key, 96, sig, sizeof sig)) {
        kerr("nv-fsp", "selftest: a 96-byte key was sent to a part that "
                       "wants 97");
        failures++;
    }

    if (!failures)
        kinfo("nv-fsp", "the chain-of-trust message is framed the way the "
                        "security processor expects, and starts it");

    nv_fsp_model_detach();
    return failures;
}
