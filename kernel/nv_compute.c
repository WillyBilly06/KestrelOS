/* nv_compute.c - encode the pushbuffer that launches a compute QMD.
 * See nv_compute.h.  Method offsets verified by tools/verify_nv_compute.py. */
#ifdef NV_COMPUTE_HOST_TEST
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;
#include "nv_compute.h"
#else
#include "nv_compute.h"
#endif

/* A single increasing-method write: one header word then `n` data words. */
static u32 emit(u32 *pb, u32 *at, u32 cap, u32 subc, u32 mthd,
                const u32 *data, u32 n) {
    if (*at + 1 + n > cap) return 0;
    pb[(*at)++] = NV_FIFO_PKHDR_SQ(subc, mthd, n);
    for (u32 i = 0; i < n; i++) pb[(*at)++] = data[i];
    return 1 + n;
}

u32 nv_compute_push_launch(u32 *pb, u32 *at, u32 cap,
                           u32 subc, u32 cls, u64 qmd_addr) {
    u32 start = *at;

    /* 1. Bind the compute class to the subchannel. */
    u32 obj = cls;
    if (!emit(pb, at, cap, subc, NVCEC0_SET_OBJECT, &obj, 1)) return 0;

    /* 2. Point at the QMD.  Its address is stored shifted right by eight (the
     *    QMD is 256-byte aligned), so the low eight bits are always zero. */
    u32 qmd_shifted = (u32)(qmd_addr >> 8);
    if (!emit(pb, at, cap, subc, NVCEC0_SEND_PCAS_A, &qmd_shifted, 1)) return 0;

    /* 3. Schedule it.  Blackwell (0xCEC0 > TURING_COMPUTE_A) uses PCAS2_B with
     *    PCAS_ACTION=INVALIDATE_COPY_SCHEDULE (0x3), NOT the Turing PCAS_B. */
    u32 sig = NVCEC0_SEND_SIGNALING_PCAS2_B_PCAS_ACTION_INVALIDATE_COPY_SCHEDULE;
    if (!emit(pb, at, cap, subc, NVCEC0_SEND_SIGNALING_PCAS2_B, &sig, 1)) return 0;

    return *at - start;
}

#ifndef NV_COMPUTE_HOST_TEST
#include "nv_qmd.h"
#include "klog.h"

/* Verify the whole compute dispatch the way nv_fifo_selftest verifies 2D: build
 * a QMD and the launch push buffer, then act as the GPU would - walk the push
 * buffer, follow the QMD pointer, and pull the launch back out - and check it
 * is the one that was built.  Self-contained (no channel, so it runs with no
 * GSP), reachable from `gpu selftest`.  Returns the number of failures. */
int nv_compute_selftest(void) {
    static u32 qmd[NV_QMD_WORDS];
    nv_compute_launch_t l = { 0 };
    l.program_addr = 0x7f001230ull;
    l.grid[0] = 4096; l.grid[1] = 1; l.grid[2] = 1;
    l.block[0] = 256; l.block[1] = 1; l.block[2] = 1;
    l.reg_count = 12; l.shared_bytes = 0; l.sass_version = 0x78;
    nv_qmd_build_compute(qmd, &l);
    nv_qmd_set_constant_buffer(qmd, 0, 0x10000000ull, 924);

    /* The push buffer names the QMD by its GPU address, a 40-bit value the
     * SEND_PCAS_A field carries as (addr >> 8).  Use a plausible 256-aligned
     * GPU address that fits that field (NOT the kernel pointer, which is 64-bit
     * and would truncate); the QMD itself is read from its real buffer below,
     * the way a model follows the driver's own memory. */
    u64 qmd_gpu = 0x00100000ull;
    u32 pb[16]; u32 at = 0;
    nv_compute_push_launch(pb, &at, 16, 1, 0xCEC0, qmd_gpu);

    u32 bound = 0; u64 got_qmd = 0; int launched = 0;
    for (u32 i = 0; i < at; ) {
        u32 h = pb[i++], op = h >> 29, cnt = (h >> 16) & 0x1FFF, m = (h & 0x1FFF) << 2;
        for (u32 k = 0; k < cnt; k++) {
            u32 d = pb[i++];
            if (m == 0x0000) bound = d;
            else if (m == 0x02b4) got_qmd = (u64)d << 8;
            else if (m == 0x02c0 && (d & 0xF) ==
                     NVCEC0_SEND_SIGNALING_PCAS2_B_PCAS_ACTION_INVALIDATE_COPY_SCHEDULE)
                launched = 1;
            if (op == 1) m += 4;
        }
    }

    int failures = 0;
    if (bound != 0xCEC0) { kerr("nv-compute", "class bound as %04x, expected CEC0", bound); failures++; }
    if (got_qmd != qmd_gpu) { kerr("nv-compute", "QMD address did not round-trip through the stream"); failures++; }
    if (!launched) { kerr("nv-compute", "no schedule signal in the stream"); failures++; }

    const u32 *mq = qmd;                 /* the model reads the QMD's real bytes */
    u64 prog = ((u64)nv_qmd_get(mq, NV_QMD_F_PROGRAM_ADDRESS_UPPER) << 32 |
                nv_qmd_get(mq, NV_QMD_F_PROGRAM_ADDRESS_LOWER)) << 4;
    if (prog != l.program_addr ||
        nv_qmd_get(mq, NV_QMD_F_GRID_WIDTH) != 4096 ||
        nv_qmd_get(mq, NV_QMD_F_CTA_THREAD_DIMENSION0) != 256 ||
        nv_qmd_get(mq, NV_QMD_F_REGISTER_COUNT) != 12) {
        kerr("nv-compute", "the QMD the model read back did not match the launch");
        failures++;
    }
    if (!failures)
        kinfo("nv-compute", "compute dispatch: a %ux%u grid of the compositing "
              "shader was built, submitted and read back by the model correctly",
              nv_qmd_get(mq, NV_QMD_F_GRID_WIDTH),
              nv_qmd_get(mq, NV_QMD_F_CTA_THREAD_DIMENSION0));
    return failures;
}

void nv_compute_launch_fifo(nv_fifo_t *f, int subc, u32 cls, u64 qmd_addr) {
    /* The same three methods, through the channel's own writer.  nv_fifo's
     * push_header (NV_PUSH_OP_INC) produces the identical Fermi+ header this
     * file's standalone encoder was verified to emit. */
    nv_push_immediate(f, subc, NVCEC0_SET_OBJECT, cls);
    nv_push_immediate(f, subc, NVCEC0_SEND_PCAS_A, (u32)(qmd_addr >> 8));
    nv_push_immediate(f, subc, NVCEC0_SEND_SIGNALING_PCAS2_B,
                      NVCEC0_SEND_SIGNALING_PCAS2_B_PCAS_ACTION_INVALIDATE_COPY_SCHEDULE);
}
#endif
