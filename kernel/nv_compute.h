/* nv_compute.h - the pushbuffer commands that launch a compute QMD.
 *
 * A channel runs a "pushbuffer": a stream of (header, data...) method writes the
 * GPU's FIFO reads.  To launch a compute grid the driver writes three methods
 * of the BLACKWELL_COMPUTE_B class (0xCEC0) into it: bind the class to a
 * subchannel, point at the QMD (built by nv_qmd.c), and signal it to run.  This
 * encodes exactly those, into a caller's word buffer.
 *
 * The method OFFSETS are from NVIDIA's classes/compute/clcec0.h and are checked
 * by tools/verify_nv_compute.py.  The pushbuffer HEADER format is the Fermi+
 * "NVC0" format (0x2<<28 = increasing methods), unchanged from Fermi through
 * Blackwell, as used by nouveau and Mesa.
 *
 * What this does NOT do is own a real channel or ring the doorbell that makes
 * the GPU read the pushbuffer - that needs the GSP up on real silicon
 * (kestrelos-gpu-two-walls).  This is the command ENCODING, unit-tested on the
 * host; its submission is the hardware-gated caller.
 */
#ifndef KESTREL_NV_COMPUTE_H
#define KESTREL_NV_COMPUTE_H

#ifndef NV_COMPUTE_HOST_TEST
#include "kernel.h"
#endif

/* Method offsets in the compute class (bytes), from clcec0.h. */
#define NVCEC0_SET_OBJECT              0x0000u
#define NVCEC0_SEND_PCAS_A             0x02b4u   /* data = QMD address >> 8   */
#define NVCEC0_SEND_SIGNALING_PCAS_B   0x02bcu   /* Turing-era: INVALIDATE[0] SCHEDULE[1] */

#define NVCEC0_SEND_SIGNALING_PCAS_B_INVALIDATE_TRUE  0x1u
#define NVCEC0_SEND_SIGNALING_PCAS_B_SCHEDULE_TRUE    0x2u

/* Blackwell (post-Turing) launch trigger.  Mesa nvk_cmd_dispatch.c branches:
 * `if (compute_cls <= TURING_COMPUTE_A) SEND_SIGNALING_PCAS_B else
 *  SEND_SIGNALING_PCAS2_B(PCAS_ACTION_INVALIDATE_COPY_SCHEDULE)`.  0xCEC0 is far
 * past Turing, so the correct kick is PCAS2_B @ 0x2c0 with action 0x3.  The old
 * PCAS_B@0x2bc still exists for back-compat but no Blackwell driver uses it. */
#define NVCEC0_SEND_SIGNALING_PCAS2_B  0x02c0u   /* PCAS_ACTION[3:0] */
#define NVCEC0_SEND_SIGNALING_PCAS2_B_PCAS_ACTION_INVALIDATE_COPY_SCHEDULE  0x3u

/* Fermi+ pushbuffer header: increasing methods.  count words follow. */
#define NV_FIFO_PKHDR_SQ(subc, mthd, count) \
    (0x20000000u | ((u32)(count) << 16) | ((u32)(subc) << 13) | ((u32)(mthd) >> 2))

/* Append the launch of `qmd_addr` (the QMD's GPU address) on subchannel `subc`
 * bound to compute class `cls` (0xCEC0), into `pb` at *at (in words).  Advances
 * *at.  `cap` guards the buffer.  Returns the number of words written, or 0 if
 * it would overflow.  Standalone form, used to unit-test the method sequence on
 * the host; the kernel uses the nv_fifo form below. */
u32 nv_compute_push_launch(u32 *pb, u32 *at, u32 cap,
                           u32 subc, u32 cls, u64 qmd_addr);

#ifndef NV_COMPUTE_HOST_TEST
#include "nv.h"   /* nv_fifo_t and nv_push_immediate */
/* The real path: emit the same launch into a live channel's push buffer through
 * nv_fifo's own method writer (the one the 2D and 3D paths already use and the
 * FIFO model already checks), rather than re-encoding the headers here.  Called
 * once a compute channel exists - which is after the GSP is up on real silicon
 * (see kestrelos-gpu-two-walls); until then it has no caller and that is
 * honest, the channel is the hardware-gated part. */
void nv_compute_launch_fifo(nv_fifo_t *f, int subc, u32 cls, u64 qmd_addr);

/* Verify the compute dispatch (QMD + launch push buffer) against a model, the
 * way nv_fifo_selftest verifies 2D.  Runs with no GSP; called by gpu selftest.
 * Returns the number of failures. */
int nv_compute_selftest(void);
#endif

#endif
