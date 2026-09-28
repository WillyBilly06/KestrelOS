/* nv_gsp_queue.c - the two rings a driver and the card's co-processor talk
 * through, laid out the way the co-processor's firmware actually lays them
 * out.
 *
 * From Turing onwards the card is not driven by writing registers.  A driver
 * asks the processor on the card to do things - allocate a channel, make an
 * object, set a mode - and those requests travel as messages through a pair
 * of ring buffers in memory the card can read.  Nothing else can be asked of
 * the card until this conversation works.
 *
 * ---------------------------------------------------------------------------
 * This file was rewritten against the published protocol, and the differences
 * from its first version are worth listing because every one of them was the
 * kind that produces silence rather than an error message:
 *
 *   - Each region is 256 KiB: a 4 KiB header page, then sixty-three 4 KiB
 *     ring entries.  The first version used 64-byte elements at a computed
 *     offset; the firmware speaks in pages.
 *
 *   - Every message begins with a 48-byte element header - two 16-byte
 *     buffers kept for encrypted sessions, a checksum, a sequence number and
 *     a page count - and then the 32-byte RPC header from nv_gsp_rpc.c.  The
 *     first version had no element header and a private RPC header that was
 *     36 bytes, four too long, so every field after the first would have been
 *     read from the wrong place.
 *
 *   - The read pointers are SWAPPED: the slot after the command ring's own
 *     header holds where this driver has read to in the STATUS ring, and the
 *     slot after the status ring's header holds where the processor has read
 *     to in the COMMAND ring.  The `flags = 1` in the header is that
 *     arrangement being declared.  The first version kept each ring's read
 *     pointer beside its own write pointer, which works perfectly when both
 *     ends are this file - and only then.
 *
 *   - The doorbell is the co-processor's Falcon register 0xc00, written
 *     zero.  The first version rang a register nothing listens to.
 *
 * Layout, header values, checksum and doorbell are from NVIDIA's published
 * message_queue_priv.h and the two open drivers built on it, which agree with
 * each other in every detail above.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "klog.h"
#include "mm.h"
#include "time.h"
#include "proc.h"     /* sched_yield - yield instead of busy-waiting an RPC */
#include "nv.h"

#define GSP_PAGE            4096u
#define QUEUE_ENTRIES       ((NV_GSP_QUEUE_REGION_BYTES / GSP_PAGE) - 1)

/* The most pages one message may span here.  The protocol allows more via
 * continuation records; nothing this driver sends needs them yet, and a
 * longer message arriving reports itself below rather than being misread. */
#define MOST_PAGES          16u

/* The doorbell: the co-processor's own Falcon register, not a queue one. */
#define GSP_DOORBELL        (NV_FALCON_GSP + 0xc00)

u32 nv_gsp_last_rpc_result = 0xFFFFFFFFu;
u32 nv_gsp_last_rpc_result_private = 0xFFFFFFFFu;

/* ------------------------------------------------------------- the headers */

/* The writer's description of its ring, at the start of its region. */
typedef struct {
    u32 version;
    u32 size;              /* bytes of the whole region                     */
    u32 msg_size;          /* bytes per element - a page                    */
    u32 msg_count;         /* how many elements                             */
    u32 write_ptr;         /* in elements                                   */
    u32 flags;             /* 1: the read pointers live in each other's
                            * regions - see the note at the top             */
    u32 rx_header_offset;  /* where the read pointer slot sits              */
    u32 entry_offset;      /* where element zero sits - one page in         */
} nv_msgq_tx_t;

typedef struct {
    u32 read_ptr;          /* in elements */
} nv_msgq_rx_t;

_Static_assert(sizeof(nv_msgq_tx_t) == 32, "the ring header is eight words");

/* The 48 bytes in front of every message.  The two buffers exist for
 * encrypted sessions and stay zero outside one; the rest is the framing the
 * firmware checks. */
typedef struct {
    u8  auth_tag[16];
    u8  aad[16];
    u32 checksum;          /* the XOR fold of the whole padded message      */
    u32 sequence;
    u32 elem_count;        /* pages this message spans                      */
    u32 pad;
} nv_gsp_msg_element_t;

_Static_assert(sizeof(nv_gsp_msg_element_t) == 48,
               "the element header is forty-eight bytes");
_Static_assert(sizeof(nv_gsp_rpc_header_t) == 32,
               "the RPC header is eight words");

static nv_msgq_tx_t *tx_of(u8 *region) { return (nv_msgq_tx_t *)region; }

/* The read pointer for a ring lives in the OTHER region - that is what the
 * swap means.  Given the region the reader acknowledges in, this is where. */
static nv_msgq_rx_t *rx_slot(u8 *region) {
    return (nv_msgq_rx_t *)(region + sizeof(nv_msgq_tx_t));
}

static u8 *entry(u8 *region, u32 index) {
    return region + GSP_PAGE + (size_t)index * GSP_PAGE;
}

/* ------------------------------------------------------------- setting up */

/* The command ring, which this driver owns and writes.  `peer` is the status
 * region, where the processor will keep its progress through our commands. */
void nv_gsp_cmdq_init(nv_gsp_queue_t *q, u8 *own, u8 *peer) {
    memset(q, 0, sizeof *q);
    q->memory = own;
    q->peer = peer;
    q->count = QUEUE_ENTRIES;

    nv_msgq_tx_t *tx = tx_of(own);
    memset(tx, 0, sizeof *tx);
    tx->version = 0;
    tx->size = NV_GSP_QUEUE_REGION_BYTES;
    tx->msg_size = GSP_PAGE;
    tx->entry_offset = GSP_PAGE;
    tx->msg_count = QUEUE_ENTRIES;
    tx->write_ptr = 0;
    tx->flags = 1;
    tx->rx_header_offset = sizeof(nv_msgq_tx_t);
    rx_slot(own)->read_ptr = 0;

    q->ready = true;
}

/* The status ring, which the PROCESSOR owns.  Its header is written by the
 * firmware when it boots; this only remembers where everything is, and must
 * write none of it. */
void nv_gsp_msgq_adopt(nv_gsp_queue_t *q, u8 *own, u8 *peer) {
    memset(q, 0, sizeof *q);
    q->memory = own;
    q->peer = peer;
    q->count = QUEUE_ENTRIES;
    q->ready = true;
}

/* -------------------------------------------------------------- sending */

static bool rpc_send(nv_card_t *c, nv_gsp_queue_t *q, u32 function,
                     const void *payload, u32 payload_len, u32 *sequence_out,
                     bool noseq, bool ring_doorbell) {
    if (!q->ready) return false;

    u32 total = (u32)sizeof(nv_gsp_msg_element_t) +
                (u32)sizeof(nv_gsp_rpc_header_t) + payload_len;
    u32 pages = (total + GSP_PAGE - 1) / GSP_PAGE;

    if (pages > MOST_PAGES || pages > q->count - 1) {
        kwarn("nv-gsp", "a %u byte message spans %u pages; continuation "
                        "records are not written yet and nothing this driver "
                        "asks needs them", total, pages);
        return false;
    }

    /* Staged whole, then copied page by page, because a message that wraps
     * the end of the ring is easier to get right from a straight copy than
     * from arithmetic done twice. */
    u8 *staged = kzalloc((size_t)pages * GSP_PAGE);
    if (!staged) return false;

    /* The queue-element sequence number MUST start at 0 and increment by one
     * per message, because the consumer (GSP-RM) checks
     * `element->seqNum == rxSeqNum` (rxSeqNum starts at 0) and, on a mismatch
     * where our number is AHEAD of its expectation, retries the same slot
     * forever WITHOUT consuming it - so an off-by-one here wedges the whole
     * command ring and the processor's read pointer never moves.  NVIDIA's own
     * sender post-increments (`pCQE->seqNum = txSeqNum; ... txSeqNum++`), i.e.
     * the first element carries 0.  We were pre-incrementing, so our first
     * element carried 1, GSP-RM expected 0, and every command was rejected -
     * "processor has read 0" for the entire ring.  Post-increment now. */
    u32 sequence = q->sequence++;

    nv_gsp_msg_element_t *elem = (nv_gsp_msg_element_t *)staged;
    nv_gsp_rpc_header_t *rpc =
        (nv_gsp_rpc_header_t *)(staged + sizeof *elem);

    /* The configuration RPCs (system_info, registry) are async: their INNER
     * rpc.sequence is zero (NVKM_GSP_RPC_REPLY_NOSEQ).  A synchronous RPC's
     * inner sequence is the transport counter.  The OUTER element sequence
     * above is separate and always increments - these are distinct fields in
     * NVIDIA's message_queue_priv.h. */
    u32 rpc_sequence = noseq ? 0 : sequence;
    nv_gsp_rpc_header(rpc, function,
                      (u32)sizeof *rpc + payload_len, rpc_sequence);
    /* On the wire both result words say "not answered yet". */
    rpc->result = 0xFFFFFFFFu;
    rpc->result_private = 0xFFFFFFFFu;

    if (payload_len)
        memcpy(staged + sizeof *elem + sizeof *rpc, payload, payload_len);

    elem->sequence = sequence;
    elem->elem_count = pages;
    elem->pad = 0;
    elem->checksum = 0;
    /* Over the whole padded length, checksum field zero - which is why it is
     * computed last. */
    elem->checksum = nv_gsp_checksum(staged, pages * GSP_PAGE);

    /* Room: the processor's progress through this ring is parked in the peer
     * region.  One element is always left empty so full and empty differ. */
    nv_msgq_tx_t *tx = tx_of(q->memory);
    volatile u32 *their_read = &rx_slot(q->peer)->read_ptr;

    u64 space_deadline = g_uptime_ms + 1000;   /* one second is plenty */
    for (;;) {
        u32 free = (*their_read + q->count - tx->write_ptr - 1) % q->count;
        if (free >= pages) break;
        if (g_uptime_ms >= space_deadline) {
            kwarn("nv-gsp", "the command ring stayed full; the processor is "
                            "not consuming");
            kfree(staged);
            return false;
        }
        sched_yield();   /* yield, don't busy-wait: keeps USB/log-flush alive */
    }

    u32 at = tx->write_ptr;
    for (u32 i = 0; i < pages; i++) {
        memcpy(entry(q->memory, at), staged + (size_t)i * GSP_PAGE, GSP_PAGE);
        at = (at + 1) % q->count;
    }
    kfree(staged);

    /* Everything is in memory before the pointer that makes it visible. */
    __asm__ volatile("" ::: "memory");
    tx->write_ptr = at;
    __asm__ volatile("" ::: "memory");

    /* Preboot entries are made visible through write_ptr but do not touch the
     * Falcon doorbell.  On this path the clean-state FLR happens after host
     * memory is staged and the Falcon is not alive to receive a bell anyway;
     * GSP-RM examines the already-populated ring as part of its own startup. */
    if (ring_doorbell) nv_wr32(c, GSP_DOORBELL, 0);

    if (sequence_out) *sequence_out = rpc_sequence;
    return true;
}

bool nv_gsp_rpc_send(nv_card_t *c, nv_gsp_queue_t *q, u32 function,
                     const void *payload, u32 payload_len, u32 *sequence_out) {
    if (!c) return false;
    return rpc_send(c, q, function, payload, payload_len, sequence_out,
                    false, true);
}

bool nv_gsp_rpc_enqueue_preboot(nv_gsp_queue_t *q, u32 function,
                                const void *payload, u32 payload_len) {
    return rpc_send(NULL, q, function, payload, payload_len, NULL,
                    true, false);
}

/* ------------------------------------------------------------- receiving */

/* Parse and log an RC_TRIGGERED (0x1004) rcJournalBuffer.  `r` points at the RPC
 * body (rpc_rc_triggered_v17_02: size@44, buffer@48); the buffer is an array of
 * RmRcDiagReport records (3272 bytes each = RmRCCommonJournal_RECORD(40) +
 * RmRcDiag_RECORD).  Per record: diagType@48 (1=GRSTATUS 2=GPCSTATUS 3=MMU_FAULT
 * 4=RC_ERROR), count@56, data[] of {offset,tag,value,attribute} 16B each @68.
 * Record 4 (RC_ERROR) carries the actual robust-channel reason code - the datum
 * that distinguishes a real GR exception from a watchdog timeout. */
static void nv_rc_journal_dump(const u8 *r, u32 body_len) {
    if (body_len < 48 + 68) return;
    const u8 *jb = r + 48;
    u32 jsz = 0; memcpy(&jsz, r + 44, 4);
    u32 avail = body_len - 48;
    const u32 REC = 3272u;
    kwarn("nv-gsp", "RC journal: size %u avail %u (%u records of %u)",
          jsz, avail, jsz / REC, REC);
    for (u32 ri = 0; ri < 3; ri++) {
        u32 base = ri * REC;
        if (base + 68 > avail) break;
        u16 dtype = 0, count = 0;
        memcpy(&dtype, jb + base + 48, 2);
        memcpy(&count, jb + base + 56, 2);
        kwarn("nv-gsp", "RC rec%u: diagType %u (1=GR 2=GPC 3=MMU 4=RC) count %u",
              ri, dtype, count);
        for (u32 di = 0; di < count && di < 40; di++) {
            u32 eoff = base + 68 + di * 16;
            if (eoff + 16 > avail) break;
            u32 off = 0, tag = 0, val = 0, attr = 0;
            memcpy(&off, jb + eoff + 0, 4);  memcpy(&tag, jb + eoff + 4, 4);
            memcpy(&val, jb + eoff + 8, 4);  memcpy(&attr, jb + eoff + 12, 4);
            if (val || attr)
                kwarn("nv-gsp", "  rec%u[%u] reg %#x tag %#x val %#x attr %#x",
                      ri, di, off, tag, val, attr);
        }
    }
    void klog_persist_flush(void);
    klog_persist_flush();
}

bool nv_gsp_rpc_receive(nv_card_t *c, nv_gsp_queue_t *q, u32 *function,
                        u32 *sequence, void *payload, u32 payload_cap,
                        u32 *payload_len, int timeout_ms) {
    (void)c;
    if (!q->ready) return false;

    /* The processor's write pointer is in its own header; our progress is
     * parked in the peer (command) region. */
    nv_msgq_tx_t *their_tx = tx_of(q->memory);
    volatile u32 *our_read = &rx_slot(q->peer)->read_ptr;

    u64 recv_deadline = g_uptime_ms + (u64)(timeout_ms > 0 ? timeout_ms : 1);
    for (;;) {
        if (*our_read != their_tx->write_ptr) break;
        if (g_uptime_ms >= recv_deadline) return false;
        sched_yield();   /* yield, don't busy-wait: keeps USB/log-flush alive */
    }

    u32 at = *our_read;
    const nv_gsp_msg_element_t *elem =
        (const nv_gsp_msg_element_t *)entry(q->memory, at);

    u32 pages = elem->elem_count;
    if (!pages || pages > q->count - 1) {
        kwarn("nv-gsp", "a message claims to span %u pages; the ring holds %u"
                        " - skipping everything unread", pages, q->count - 1);
        *our_read = their_tx->write_ptr;
        return false;
    }

    const nv_gsp_rpc_header_t *rpc =
        (const nv_gsp_rpc_header_t *)((const u8 *)elem + sizeof *elem);

    if (rpc->signature != 0x43505256u) {
        kwarn("nv-gsp", "a message carried signature %08x, not a message at "
                        "all", rpc->signature);
        *our_read = (at + pages) % q->count;
        return false;
    }

    /* The length is the firmware's claim about itself, bounded by what the
     * page count can actually hold before it is believed. */
    u32 most = pages * GSP_PAGE - (u32)sizeof *elem;
    if (rpc->length < sizeof *rpc || rpc->length > most) {
        kwarn("nv-gsp", "a message claims %u bytes and its %u page(s) hold "
                        "at most %u", rpc->length, pages, most);
        *our_read = (at + pages) % q->count;
        return false;
    }

    u32 body = rpc->length - (u32)sizeof *rpc;
    u32 give = body < payload_cap ? body : payload_cap;
    if (body > payload_cap)
        kwarn("nv-gsp", "a %u byte reply was cut to fit %u", body, payload_cap);

    /* RC_TRIGGERED (0x1004) journals are ~9.8KB (3 diag records) but a control
     * reply_cap is only ~4KB, so the caller only ever sees record 0.  The WHOLE
     * message is still in the ring pages here (the read pointer advances below),
     * so capture the full body into a static buffer and parse every record - the
     * only place record 2 (RC_ERROR, the reason code) is reachable. */
    if (rpc->function == 0x1004u) {
        static u8 rc_full[10240];
        u32 want = body < sizeof rc_full ? body : (u32)sizeof rc_full;
        u32 t = 0, sk = (u32)sizeof *elem + (u32)sizeof *rpc;
        for (u32 i = 0; i < pages && t < want; i++) {
            const u8 *pg = entry(q->memory, (at + i) % q->count);
            u32 s = 0, av = GSP_PAGE;
            if (sk) { s = sk < GSP_PAGE ? sk : GSP_PAGE; av = GSP_PAGE - s; sk -= s; }
            u32 ck = want - t < av ? want - t : av;
            memcpy(rc_full + t, pg + s, ck); t += ck;
        }
        nv_rc_journal_dump(rc_full, t);
    }

    /* Copy out across the pages, minding the wrap: the message occupies
     * `pages` consecutive ring entries, and only the ring wraps - the bytes
     * inside the message do not. */
    u32 taken = 0;
    u32 skip = (u32)sizeof *elem + (u32)sizeof *rpc;
    for (u32 i = 0; i < pages && taken < give; i++) {
        const u8 *page = entry(q->memory, (at + i) % q->count);
        u32 start = 0, avail = GSP_PAGE;
        if (skip) {
            start = skip < GSP_PAGE ? skip : GSP_PAGE;
            avail = GSP_PAGE - start;
            skip -= start;
        }
        u32 chunk = give - taken < avail ? give - taken : avail;
        memcpy((u8 *)payload + taken, page + start, chunk);
        taken += chunk;
    }

    __asm__ volatile("" ::: "memory");
    *our_read = (at + pages) % q->count;

    if (function) *function = rpc->function;
    if (sequence) *sequence = rpc->sequence;
    nv_gsp_last_rpc_result = rpc->result;
    nv_gsp_last_rpc_result_private = rpc->result_private;
    if (payload_len) *payload_len = give;
    return true;
}

/* A window onto the four pointers that say whether the two ends are talking:
 * our write into the command ring and how far the processor has read it, its
 * write into the status ring and how far we have read that.  When a request
 * gets no answer this is the single most useful thing to see - it says whether
 * the processor even consumed the command (its command-read advancing) or
 * whether it consumed it and simply wrote nothing back (its status-write
 * staying put).  The read pointers live in each other's regions - the swap -
 * so this reads command's from the status region and vice versa. */
void nv_gsp_ring_state(const char *tag, nv_gsp_queue_t *command,
                       nv_gsp_queue_t *status) {
    if (!command || !status || !command->memory || !status->memory) return;
    u32 cmd_wptr      = tx_of(command->memory)->write_ptr;
    u32 gsp_cmd_read  = rx_slot(command->peer)->read_ptr;   /* their progress */
    u32 stat_wptr     = tx_of(status->memory)->write_ptr;   /* their write    */
    u32 our_stat_read = rx_slot(status->peer)->read_ptr;    /* our progress   */
    kinfo("nv-gsp", "%s rings: cmdq write %u / processor has read %u ; "
                    "statq write %u / we have read %u",
          tag, cmd_wptr, gsp_cmd_read, stat_wptr, our_stat_read);
}

/* Send one and wait for the answer.
 *
 * Replies are matched by FUNCTION, which is how the firmware pairs them;
 * anything else that arrives in the meantime is an unsolicited event -
 * a fault report, a display hot-plug - and is noted rather than dropped in
 * silence. */
bool nv_gsp_rpc_call(nv_card_t *c, nv_gsp_queue_t *command,
                     nv_gsp_queue_t *status, u32 function,
                     const void *payload, u32 payload_len,
                     void *reply, u32 reply_cap, u32 *reply_len,
                     int timeout_ms) {
    u32 sent = 0;
    if (!nv_gsp_rpc_send(c, command, function, payload, payload_len, &sent))
        return false;

    /* Wait bounded by a TIME budget, not by a count of interleaved messages.
     * During bring-up the processor emits bursts of unsolicited events (log,
     * init, fault) on the status ring; a fixed cap of N events could abandon a
     * reply that is legitimately just behind them.  Keep consuming events until
     * the reply arrives or the clock runs out. */
    u64 deadline = g_uptime_ms + (u64)(timeout_ms > 0 ? timeout_ms : 1);
    for (;;) {
        u64 now = g_uptime_ms;
        if (now >= deadline) {
            /* Timed out.  Show the ring pointers so the failure is legible:
             * did the processor consume the command (its read advanced) or
             * not?  Did it write anything back?  This is the difference
             * between "it never saw the doorbell" and "it saw it, processed
             * it, and answered nothing". */
            nv_gsp_ring_state("no answer -", command, status);
            return false;
        }
        int remaining = (int)(deadline - now);

        u32 got_function = 0, got_sequence = 0, got_len = 0;
        if (!nv_gsp_rpc_receive(c, status, &got_function, &got_sequence,
                                reply, reply_cap, &got_len, remaining)) {
            /* Nothing arrived before the receive deadline.  Dump the pointers:
             * if the processor's command-read advanced past our write, it
             * consumed the request and simply answered nothing; if it stayed
             * put, it never noticed the doorbell. */
            nv_gsp_ring_state("no answer (recv timed out) -", command, status);
            /* And the card's own queue + status registers.  The decisive read
             * is the queue HEAD (the doorbell): 0xBADFxxxx means our write was
             * PLM-masked and never reached the processor; the value we wrote
             * (0) reading back means the doorbell landed but the processor did
             * not act on it.  The mailboxes and HEAD/TAIL say whether GSP-RM is
             * alive and tracking the ring at all. */
            if (c) {
                u32 head = nv_rd32(c, NV_FALCON_GSP + 0xc00);
                u32 tail = nv_rd32(c, NV_FALCON_GSP + 0xc04);
                u32 mb0  = nv_rd32(c, NV_FALCON_GSP + 0x040);
                u32 mb1  = nv_rd32(c, NV_FALCON_GSP + 0x044);
                u32 os   = nv_rd32(c, NV_FALCON_GSP + 0x080);
                kwarn("nv-gsp", "GSP regs after no-answer: queue HEAD %#x TAIL "
                                "%#x, mailbox0 %#x mailbox1 %#x, os %#x",
                      head, tail, mb0, mb1, os);
            }
            return false;
        }

        /* Match on FUNCTION alone, the way NVIDIA's own driver does
         * (nouveau r535_gsp_msg_recv(gsp, fn, 0)).  RPCs are issued one at a
         * time (nv_rm_bring_up serialises them), and GSP-RM does NOT reliably
         * echo our sent sequence in the reply - requiring sequence==sent
         * rejected the real answer and left every alloc "unanswered", while
         * INIT_DONE (matched by function only) worked.  The sequence is logged
         * for a trace but no longer gates the match. */
        if (got_function == function) {
            if (got_sequence != sent)
                kinfo("nv-gsp", "reply to %#x carries sequence %u (sent %u); "
                                "accepting on the function match", function,
                      got_sequence, sent);
            if (reply_len) *reply_len = got_len;
            return true;
        }
        /* A NOCAT journal record (GSP_POST_NOCAT_RECORD) interleaved with the
         * reply we are waiting for means GSP-RM hit an error WHILE servicing
         * this very RPC - e.g. it journals an out-of-memory during a channel
         * alloc and then returns 81.  Decode it here (the fields we need,
         * recType@16 and errorCode@96, land inside even a truncated read) so the
         * reason is captured rather than discarded.  Offsets vs OGKM
         * ctrl2080nvd.h, same as nv_gsp_rpc_poll. */
        if (got_function == 0x1020u && got_len >= 104) {
            const u8 *r = (const u8 *)reply;
            u32 recType = r[16];
            u32 bugcheck = 0, subsystem = 0; u64 errorCode = 0;
            memcpy(&bugcheck,  r + 20, 4);
            memcpy(&subsystem, r + 92, 4);
            memcpy(&errorCode, r + 96, 8);
            kwarn("nv-gsp", "GSP journal (NOCAT) DURING the reply to %#x: recType "
                            "%u bugcheck %#x subsystem %#x errorCode %#llx - this "
                            "is why that call failed", function, recType, bugcheck,
                  subsystem, (unsigned long long)errorCode);
        } else if (got_function == 0x1004u && got_len >= 40) {
            /* RC_TRIGGERED (0x1004): the GPU's own report that a channel took a
             * fault and Robust-Channel recovery ran.  This is the decisive line
             * for a "card CONSUMED the ring but the engine faulted" - it names
             * the engine, the channel, the exception type and, for an MMU fault,
             * the faulting address.  Layout = rpc_rc_triggered_v17_02 (nouveau
             * r570 fifo.h): nv2080EngineType@0 chid@4 exceptLevel@12 exceptType@16
             * mmuFaultAddrLo@28 mmuFaultAddrHi@32 mmuFaultType@36. */
            const u8 *r = (const u8 *)reply;
            u32 engtype=0, chid=0, elevel=0, etype=0, flo=0, fhi=0, ftype=0;
            memcpy(&engtype, r + 0, 4);  memcpy(&chid,  r + 4,  4);
            memcpy(&elevel,  r + 12, 4); memcpy(&etype, r + 16, 4);
            memcpy(&flo,     r + 28, 4); memcpy(&fhi,   r + 32, 4);
            memcpy(&ftype,   r + 36, 4);
            kwarn("nv-gsp", "RC_TRIGGERED (channel fault) during reply to %#x: "
                            "nv2080Engine %#x chid %u exceptLevel %u exceptType %u "
                            "mmuFaultAddr %#x_%08x mmuFaultType %#x", function,
                  engtype, chid, elevel, etype, fhi, flo, ftype);
            /* The full rcJournalBuffer (all 3 records, incl. RC_ERROR) is dumped
             * by nv_gsp_rpc_receive from the ring pages before the read pointer
             * advances - the caller's `reply` here is truncated to ~4KB. */
        } else if (got_function == 0x1006u && got_len >= 12) {
            /* OS_ERROR_LOG - GSP-RM's OWN error string naming the fault.  This is
             * the path that actually fires at the GR fault ("sent event 0x1006
             * while a reply to 0x4c was outstanding").  nouveau prints it
             * ("Xid:%d %s", r535/gsp.c:938).  rpc_os_error_log_v17_00 (r535
             * nvrm/gsp.h:342): exceptType@0 runlistId@4 chid@8 errString[0x100]@12. */
            const u8 *r = (const u8 *)reply;
            u32 xtype = 0, rlid = 0, xchid = 0;
            memcpy(&xtype, r + 0, 4); memcpy(&rlid, r + 4, 4); memcpy(&xchid, r + 8, 4);
            char emsg[257];
            u32 slen = got_len - 12; if (slen > 256) slen = 256;
            memcpy(emsg, r + 12, slen); emsg[slen] = 0;
            for (u32 i = 0; i < slen; i++)
                if (emsg[i] == '\n' || emsg[i] == '\r') emsg[i] = ' ';
            kwarn("nv-gsp", "GSP OS_ERROR_LOG during reply to %#x (Xid %u runlist %u "
                            "chid %u): %s", function, xtype, rlid, xchid, emsg);
        } else {
            kinfo("nv-gsp", "the processor sent event %#x while a reply to %#x was "
                            "outstanding", got_function, function);
        }
    }
}

/* Wait for one specific message the processor sends on its own account -
 * the way its "I have finished booting" arrives.
 *
 * Bounded by a WALL-CLOCK deadline, not a fixed count of interleaved messages:
 * a healthy GSP-RM emits a burst of log prints (UCODE_LIBOS_PRINT 0x100c) and
 * telemetry records (GSP_POST_NOCAT_RECORD 0x1020, ~1.2 KiB each) around boot,
 * and a fixed 64-event cap gave up in the middle of that burst before the
 * event we wanted arrived.  The scratch buffer is large enough to hold a whole
 * NOCAT record so it is drained in one read rather than truncated.  Only the
 * first few interleaved events are logged, so a log burst does not itself
 * bury the boot log. */
bool nv_gsp_rpc_poll(nv_card_t *c, nv_gsp_queue_t *status, u32 function,
                     int timeout_ms) {
    static u8 scratch[2048];
    u64 deadline = g_uptime_ms + (u64)(timeout_ms > 0 ? timeout_ms : 1);
    int noted = 0;
    for (;;) {
        u64 now = g_uptime_ms;
        if (now >= deadline) return false;
        u32 got = 0, len = 0;
        if (!nv_gsp_rpc_receive(c, status, &got, NULL, scratch,
                                sizeof scratch, &len, (int)(deadline - now)))
            continue;   /* nothing yet; the deadline check above bounds the wait */
        if (got == function) return true;
        if (got == 0x1020) {
            /* GSP_POST_NOCAT_RECORD - the resource manager's own boot journal.
             * These are NORMAL telemetry, NOT errors: the real driver DROPS them.
             * Confirmed in nouveau r570 (our GSP era) - r570_gsp_drop_post_nocat_record
             * (rm/r570/gsp.c:27-33) registers event 0x1020 with NULL/NULL callbacks
             * (drain + discard) whenever debug < NV_DBG_DEBUG.  We were PRINTING
             * them, which made routine boot telemetry look like faults on screen.
             * Match the driver: DRAIN silently by default; only decode+print under
             * the `gspdebug` cmdline flag.  (Field offsets from
             * NV2080CtrlNocatJournalInsertRecord: recType@16, bugcheck@20,
             * subsystem@92, errorCode@96.) */
            if (cmdline_has("gspdebug") && noted < 12) {
                u8 recType = len >= 17 ? scratch[16] : 0;
                u32 bugcheck = 0, subsystem = 0;
                u64 errorCode = 0;
                if (len >= 24) memcpy(&bugcheck, scratch + 20, 4);
                if (len >= 96) memcpy(&subsystem, scratch + 92, 4);
                if (len >= 104) memcpy(&errorCode, scratch + 96, 8);
                kinfo("nv-gsp", "GSP boot journal (NOCAT): recType %u bugcheck "
                                "%#x subsystem %#x errorCode %#llx (%u-byte "
                                "record) while waiting for %#x", recType,
                      bugcheck, subsystem, (unsigned long long)errorCode, len,
                      function);
                noted++;
            }
            /* else: drained and discarded, exactly like drop_post_nocat_record */
        } else if (got == 0x1004 && len >= 40) {
            /* RC_TRIGGERED - a channel faulted; name engine/chid/type/addr (same
             * rpc_rc_triggered_v17_02 layout as nv_gsp_rpc_receive). */
            u32 engtype=0, chid=0, elevel=0, etype=0, flo=0, fhi=0, ftype=0;
            memcpy(&engtype, scratch + 0, 4);  memcpy(&chid,  scratch + 4,  4);
            memcpy(&elevel,  scratch + 12, 4); memcpy(&etype, scratch + 16, 4);
            memcpy(&flo,     scratch + 28, 4); memcpy(&fhi,   scratch + 32, 4);
            memcpy(&ftype,   scratch + 36, 4);
            kwarn("nv-gsp", "RC_TRIGGERED (channel fault) while waiting for %#x: "
                            "nv2080Engine %#x chid %u exceptLevel %u exceptType %u "
                            "mmuFaultAddr %#x_%08x mmuFaultType %#x", function,
                  engtype, chid, elevel, etype, fhi, flo, ftype);
            if (noted < 12) noted++;
        } else if (got == 0x1006 && len >= 12) {
            /* OS_ERROR_LOG - GSP-RM's OWN error string naming the fault it hit.
             * nouveau prints it ("Xid:%d %s", r535_gsp_msg_os_error_log,
             * r535/gsp.c:938).  We were dropping it into the generic logger and
             * losing GSP's own diagnosis of WHY the GR channel faults.
             * rpc_os_error_log_v17_00 (r535 nvrm/gsp.h:342): exceptType@0,
             * runlistId@4, chid@8, char errString[0x100]@12. */
            u32 xtype = 0, rlid = 0, xchid = 0;
            memcpy(&xtype, scratch + 0, 4);
            memcpy(&rlid,  scratch + 4, 4);
            memcpy(&xchid, scratch + 8, 4);
            char emsg[257];
            u32 slen = len - 12; if (slen > 256) slen = 256;
            memcpy(emsg, scratch + 12, slen); emsg[slen] = 0;
            for (u32 i = 0; i < slen; i++)
                if (emsg[i] == '\n' || emsg[i] == '\r') emsg[i] = ' ';
            kwarn("nv-gsp", "GSP OS_ERROR_LOG (Xid %u runlist %u chid %u): %s "
                            "(while waiting for %#x)", xtype, rlid, xchid, emsg,
                  function);
            if (noted < 12) noted++;
        } else if (noted < 12) {
            kinfo("nv-gsp", "event %#x arrived while waiting for %#x", got, function);
            noted++;
        }
    }
}

/* ---------------------------------------------------- the other side of it
 *
 * A processor that is not there, answering the way the protocol says it
 * must: it writes its own status-ring header the way the firmware does at
 * boot, keeps its command-ring progress in the status region the way the
 * swap requires, and CHECKS what arrives - the checksum, the page count, the
 * signature - because a model that accepts anything teaches a driver
 * nothing.
 */
#define GSP_MODEL_MAX_PAYLOAD 512

static struct {
    bool present;
    u8  *cmdq;                   /* the driver writes, this reads  */
    u8  *msgq;                   /* this writes, the driver reads  */
    u32  sequence;
    int  handled;
    int  events_sent;
    int  bad_checksums;
    u32  last_function;
    u32  last_payload_len;
    nv_gsp_model_handler_t handler;
} gsp_model;

/* Writing into the status ring, which the model owns. */
static bool model_put(u32 function, const u8 *payload, u32 len, u32 sequence) {
    nv_msgq_tx_t *tx = tx_of(gsp_model.msgq);
    volatile u32 *their_read = &rx_slot(gsp_model.cmdq)->read_ptr;

    u32 total = (u32)sizeof(nv_gsp_msg_element_t) +
                (u32)sizeof(nv_gsp_rpc_header_t) + len;
    u32 pages = (total + GSP_PAGE - 1) / GSP_PAGE;

    u32 free = (*their_read + QUEUE_ENTRIES - tx->write_ptr - 1) % QUEUE_ENTRIES;
    if (free < pages) return false;

    static u8 staged[MOST_PAGES * GSP_PAGE];
    if (pages > MOST_PAGES) return false;
    memset(staged, 0, (size_t)pages * GSP_PAGE);

    nv_gsp_msg_element_t *elem = (nv_gsp_msg_element_t *)staged;
    nv_gsp_rpc_header_t *rpc = (nv_gsp_rpc_header_t *)(staged + sizeof *elem);
    nv_gsp_rpc_header(rpc, function, (u32)sizeof *rpc + len, sequence);
    if (len) memcpy(staged + sizeof *elem + sizeof *rpc, payload, len);

    elem->sequence = sequence;
    elem->elem_count = pages;
    elem->checksum = 0;
    elem->checksum = nv_gsp_checksum(staged, pages * GSP_PAGE);

    u32 at = tx->write_ptr;
    for (u32 i = 0; i < pages; i++) {
        memcpy(entry(gsp_model.msgq, at), staged + (size_t)i * GSP_PAGE,
               GSP_PAGE);
        at = (at + 1) % QUEUE_ENTRIES;
    }
    __asm__ volatile("" ::: "memory");
    tx->write_ptr = at;
    return true;
}

/* Reading from the command ring, checking everything the firmware checks. */
static bool model_take(u32 *function, u32 *sequence, u8 *payload, u32 cap,
                       u32 *len) {
    nv_msgq_tx_t *their_tx = tx_of(gsp_model.cmdq);
    volatile u32 *my_read = &rx_slot(gsp_model.msgq)->read_ptr;

    if (*my_read == their_tx->write_ptr) return false;

    u32 at = *my_read;
    u8 *first = entry(gsp_model.cmdq, at);
    nv_gsp_msg_element_t *elem = (nv_gsp_msg_element_t *)first;

    u32 pages = elem->elem_count;
    if (!pages || pages > QUEUE_ENTRIES - 1) {
        *my_read = their_tx->write_ptr;
        return false;
    }

    /* The checksum, exactly as the firmware computes it: gathered across the
     * ring pages with the checksum field itself zeroed. */
    static u8 whole[MOST_PAGES * GSP_PAGE];
    if (pages <= MOST_PAGES) {
        for (u32 i = 0; i < pages; i++)
            memcpy(whole + (size_t)i * GSP_PAGE,
                   entry(gsp_model.cmdq, (at + i) % QUEUE_ENTRIES), GSP_PAGE);
        u32 claimed = ((nv_gsp_msg_element_t *)whole)->checksum;
        ((nv_gsp_msg_element_t *)whole)->checksum = 0;
        if (nv_gsp_checksum(whole, pages * GSP_PAGE) != claimed) {
            gsp_model.bad_checksums++;
            *my_read = (at + pages) % QUEUE_ENTRIES;
            return false;
        }
    }

    nv_gsp_rpc_header_t *rpc = (nv_gsp_rpc_header_t *)(whole + sizeof *elem);
    if (rpc->signature != 0x43505256u) {
        *my_read = (at + pages) % QUEUE_ENTRIES;
        return false;
    }

    u32 body = rpc->length - (u32)sizeof *rpc;
    if (body > cap) body = cap;
    memcpy(payload, whole + sizeof *elem + sizeof *rpc, body);

    *my_read = (at + pages) % QUEUE_ENTRIES;
    *function = rpc->function;
    *sequence = rpc->sequence;
    *len = body;
    return true;
}

/* The bell being rung: everything the driver has left is consumed and
 * answered, in the order it was left. */
static void gsp_model_doorbell(void) {
    if (!gsp_model.present) return;

    for (;;) {
        u32 function = 0, sequence = 0, len = 0;
        static u8 payload[GSP_MODEL_MAX_PAYLOAD];
        if (!model_take(&function, &sequence, payload, sizeof payload, &len))
            return;

        gsp_model.handled++;
        gsp_model.last_function = function;
        gsp_model.last_payload_len = len;

        /* Before the third answer, something the driver never asked for.  A
         * real processor reports faults and hot-plug this way, in the middle
         * of whatever else is going on. */
        if (gsp_model.handled == 3 && gsp_model.events_sent == 0) {
            u8 event[4] = { 0xEE, 0xEE, 0xEE, 0xEE };
            if (model_put(0x1005, event, sizeof event, ++gsp_model.sequence))
                gsp_model.events_sent++;
        }

        static u8 reply[GSP_MODEL_MAX_PAYLOAD];
        u32 reply_len = 0;

        if (gsp_model.handler) {
            /* Something that understands what the messages mean, rather than
             * this file's default of echoing them back.  The resource
             * manager in nv_gsp_rm.c supplies one. */
            if (!gsp_model.handler(function, payload, len, reply,
                                   sizeof reply, &reply_len))
                reply_len = 0;
        } else {
            /* Every byte inverted, so a test can tell a real reply from a
             * buffer nobody wrote. */
            for (u32 i = 0; i < len; i++) reply[i] = (u8)~payload[i];
            reply_len = len;
        }

        /* A response carries the request's sequence.  An earlier model used a
         * private monotonically increasing counter, so the unsolicited event
         * above shifted every later reply by one; the now-correct driver quite
         * properly discarded all of them as stale. */
        model_put(function, reply, reply_len, sequence);
    }
}

/* Called for every register write to the card, so that ringing the bell has
 * the effect ringing a bell has. */
void nv_gsp_model_wrote(u32 offset, u32 value) {
    (void)value;
    if (!gsp_model.present) return;
    if (offset == GSP_DOORBELL) gsp_model_doorbell();
}

void nv_gsp_model_attach(u8 *cmdq_region, u8 *msgq_region) {
    memset(&gsp_model, 0, sizeof gsp_model);
    gsp_model.cmdq = cmdq_region;
    gsp_model.msgq = msgq_region;

    /* The firmware writes its own ring's header when it boots, and so does
     * its stand-in - a driver that only works when it wrote both headers
     * itself would be being tested against nothing. */
    nv_msgq_tx_t *tx = tx_of(msgq_region);
    memset(tx, 0, sizeof *tx);
    tx->version = 0;
    tx->size = NV_GSP_QUEUE_REGION_BYTES;
    tx->msg_size = GSP_PAGE;
    tx->entry_offset = GSP_PAGE;
    tx->msg_count = QUEUE_ENTRIES;
    tx->flags = 1;
    tx->rx_header_offset = sizeof(nv_msgq_tx_t);
    rx_slot(msgq_region)->read_ptr = 0;

    gsp_model.present = true;
}

void nv_gsp_model_detach(void) { gsp_model.present = false; gsp_model.handler = NULL; }

/* Somewhere for a layer that understands the messages to hook itself in.  Set
 * AFTER attaching, because attaching clears everything. */
void nv_gsp_model_set_handler(nv_gsp_model_handler_t handler) {
    gsp_model.handler = handler;
}
int  nv_gsp_model_handled(void) { return gsp_model.handled; }
int  nv_gsp_model_events(void) { return gsp_model.events_sent; }
int  nv_gsp_model_bad_checksums(void) { return gsp_model.bad_checksums; }

/* ------------------------------------------------------------------- test */

int nv_gsp_queue_selftest(void) {
    int failures = 0;

    nv_card_t *c = nv_model_card();
    if (!c) {
        kinfo("nv-gsp", "no card to try this on");
        return 0;
    }

    /* Two real-sized regions.  512 KiB is more than a test used to take, and
     * exactly what the protocol takes: a smaller stand-in region was how the
     * first version of this file agreed with itself about a layout the
     * firmware does not use. */
    u64 phys = 0;
    u8 *shared = dma_alloc_pages(2 * NV_GSP_QUEUE_REGION_BYTES / 4096, &phys);
    if (!shared) {
        kwarn("nv-gsp", "no memory for the ring test");
        return 0;
    }
    memset(shared, 0, 2 * NV_GSP_QUEUE_REGION_BYTES);

    u8 *cmdq_region = shared;
    u8 *msgq_region = shared + NV_GSP_QUEUE_REGION_BYTES;

    static nv_gsp_queue_t command, status;
    nv_gsp_model_attach(cmdq_region, msgq_region);
    nv_gsp_cmdq_init(&command, cmdq_region, msgq_region);
    nv_gsp_msgq_adopt(&status, msgq_region, cmdq_region);

    /* Early configuration is queued before the Falcon exists.  It must be
     * visible in the command ring, must carry RPC sequence zero, and must not
     * ring the doorbell until bootstrap.  Ring it explicitly here only so the
     * model can inspect and answer the staged record. */
    {
        const u8 payload[4] = { 0x52, 0x35, 0x37, 0x30 };
        int handled = nv_gsp_model_handled();
        if (!nv_gsp_rpc_enqueue_preboot(&command, 72, payload,
                                        sizeof payload)) {
            kerr("nv-gsp", "a preboot NOSEQ message could not be queued");
            failures++;
        } else if (nv_gsp_model_handled() != handled) {
            kerr("nv-gsp", "queueing a preboot message rang the dormant "
                            "processor's doorbell");
            failures++;
        } else {
            nv_wr32(c, GSP_DOORBELL, 0);
            u32 fn = 0, seq = ~0u, len = 0;
            u8 reply[8] = {0};
            if (!nv_gsp_rpc_receive(c, &status, &fn, &seq, reply,
                                    sizeof reply, &len, 50)) {
                kerr("nv-gsp", "the model could not read the staged preboot "
                                "message after bootstrap");
                failures++;
            } else if (fn != 72 || seq != 0 || len != sizeof payload) {
                kerr("nv-gsp", "preboot framing came back as function %u, "
                                "sequence %u, length %u", fn, seq, len);
                failures++;
            } else {
                for (u32 i = 0; i < len; i++) {
                    if (reply[i] != (u8)~payload[i]) {
                        kerr("nv-gsp", "preboot payload byte %u was damaged", i);
                        failures++;
                        break;
                    }
                }
            }
        }
    }

    /* One that fits in a single page. */
    {
        u8 payload[16];
        for (int i = 0; i < 16; i++) payload[i] = (u8)(i * 3 + 1);

        u8 reply[64];
        u32 reply_len = 0;
        if (!nv_gsp_rpc_call(c, &command, &status, 0x0001, payload,
                             sizeof payload, reply, sizeof reply, &reply_len,
                             50)) {
            kerr("nv-gsp", "a short message got no answer");
            failures++;
        } else if (reply_len != sizeof payload) {
            kerr("nv-gsp", "the answer was %u bytes, expected %u", reply_len,
                 (unsigned)sizeof payload);
            failures++;
        } else {
            for (int i = 0; i < 16; i++) {
                if (reply[i] != (u8)~payload[i]) {
                    kerr("nv-gsp", "byte %d of the answer is wrong", i);
                    failures++;
                    break;
                }
            }
        }
    }

    /* One that spans pages, so the framing is used rather than dodged. */
    {
        static u8 payload[GSP_PAGE + 512];
        for (u32 i = 0; i < sizeof payload; i++) payload[i] = (u8)(i * 7 + 3);

        /* The model caps what it echoes; what matters here is that a
         * multi-page message arrives intact at all. */
        u8 reply[GSP_MODEL_MAX_PAYLOAD];
        u32 reply_len = 0;
        if (!nv_gsp_rpc_call(c, &command, &status, 0x0002, payload,
                             sizeof payload, reply, sizeof reply, &reply_len,
                             50)) {
            kerr("nv-gsp", "a page-spanning message got no answer");
            failures++;
        } else {
            for (u32 i = 0; i < reply_len; i++) {
                if (reply[i] != (u8)~payload[i]) {
                    kerr("nv-gsp", "byte %u of the long answer is wrong", i);
                    failures++;
                    break;
                }
            }
        }
    }

    /* Enough traffic to wrap the ring, with the unsolicited event the model
     * sends along the way handled rather than fatal. */
    {
        bool wrapped_ok = true;
        for (int i = 0; i < 2 * (int)QUEUE_ENTRIES; i++) {
            u8 payload[8] = { (u8)i, 2, 3, 4, 5, 6, 7, 8 };
            u8 reply[32];
            u32 reply_len = 0;
            if (!nv_gsp_rpc_call(c, &command, &status, 0x0010 + (u32)(i & 7),
                                 payload, sizeof payload, reply, sizeof reply,
                                 &reply_len, 50)) {
                kerr("nv-gsp", "message %d failed after the ring wrapped", i);
                failures++;
                wrapped_ok = false;
                break;
            }
        }
        if (wrapped_ok && nv_gsp_model_events() == 0) {
            kerr("nv-gsp", "the model's unsolicited event never arrived");
            failures++;
        }
    }

    /* A message too long for this driver must be refused here, not sent
     * broken. */
    {
        static u8 huge[MOST_PAGES * GSP_PAGE];
        if (nv_gsp_rpc_send(c, &command, 0x0099, huge, sizeof huge, NULL)) {
            kerr("nv-gsp", "an oversized message was sent rather than refused");
            failures++;
        }
    }

    /* And the model must have believed every checksum: a single bad one means
     * this side computes it differently from the firmware's own recipe. */
    if (nv_gsp_model_bad_checksums()) {
        kerr("nv-gsp", "%d message(s) failed the firmware's checksum",
             nv_gsp_model_bad_checksums());
        failures++;
    }

    nv_gsp_model_detach();

    if (!failures)
        kinfo("nv-gsp", "the message rings carry requests and replies the "
                        "way the firmware lays them out: page elements, the "
                        "48-byte header, the folded checksum, and read "
                        "pointers parked in each other's regions");
    return failures;
}
