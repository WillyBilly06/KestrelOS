/* nv_dp_aux.c - DisplayPort DPCD readback over the GSP-RM AUX channel.
 *
 * Re-lighting a DP output requires knowing the sink's link capabilities BEFORE
 * link training: the max link rate, the max lane count, whether enhanced framing
 * is supported, and whether TPS4 is available.  Those live in the first four
 * DPCD receiver-capability bytes (0x00000..0x00003), read from the sink over the
 * native AUX channel.
 *
 * With a booted GSP-RM we do NOT bit-bang the AUX HW directly; we ask GSP-RM to
 * run the AUX transaction for us via the NV0073 (NV04_DISPLAY_COMMON) control
 * NV0073_CTRL_CMD_DP_AUXCH_CTRL.  This mirrors nouveau's r535_dp_aux_xfer():
 *   linux-nouveau .../nvkm/subdev/gsp/rm/r535/disp.c:1069-1115 (r535_dp_aux_xfer)
 *
 * Every command id, struct field, and bitfield below carries a file:line
 * citation into the driver source.  NO GUESSING.
 *
 * NOTE: nv.h already declares nv_dpcd_t and nv_dp_read_dpcd() (shared types owned
 * by the main agent); we only implement the function here.
 */
#include "kernel.h"
#include "mm.h"
#include "klog.h"
#include "nv.h"
#include "time.h"

/* --- Command id -----------------------------------------------------------
 * ctrl0073dp.h:151
 *   #define NV0073_CTRL_CMD_DP_AUXCH_CTRL  (0x731341U)
 *     finn: (FINN_NV04_DISPLAY_COMMON_DP_INTERFACE_ID << 8)
 *           | NV0073_CTRL_DP_AUXCH_CTRL_PARAMS_MESSAGE_ID   (0x41, ctrl0073dp.h:154)
 * (ogkm src/common/sdk/nvidia/inc/ctrl/ctrl0073/ctrl0073dp.h)
 */
#define NV0073_CTRL_CMD_DP_AUXCH_CTRL   0x731341u

/* --- Parameter struct -----------------------------------------------------
 * Byte-for-byte the NV0073_CTRL_DP_AUXCH_CTRL_PARAMS struct.
 *   ctrl0073dp.h:156-166
 *     NvU32  subDeviceInstance;   (157)
 *     NvU32  displayId;           (158)
 *     NvBool bAddrOnly;           (159)   NvBool == u8 in the RM ABI
 *     NvU32  cmd;                 (160)
 *     NvU32  addr;                (161)
 *     NvU8   data[16];            (162)   NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE (153)
 *     NvU32  size;                (163)
 *     NvU32  replyType;           (164)
 *     NvU32  retryTimeMs;         (165)
 *
 * NvBool is a single byte (u8) and the following NvU32 is NATURALLY aligned -
 * this matches the NVIDIA struct compiled with normal C alignment, which is how
 * GSP-RM deserializes it and how this kernel's other NV0073 params are laid out
 * (see dp_get_caps_t / dfp_assign_sor_t in nv_dispca7d.c, both unpacked with
 * u8-then-u32 members and both accepted by GSP-RM).  So NO packed attribute:
 * bAddrOnly(u8) is followed by 3 pad bytes before cmd(u32), exactly like the
 * reference struct.
 */
#define NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE 16u   /* ctrl0073dp.h:153 */

typedef struct {
    u32 subDeviceInstance;                            /* ctrl0073dp.h:157 */
    u32 displayId;                                    /* ctrl0073dp.h:158 */
    u8  bAddrOnly;                                    /* ctrl0073dp.h:159 (NvBool) */
    u32 cmd;                                          /* ctrl0073dp.h:160 */
    u32 addr;                                         /* ctrl0073dp.h:161 */
    u8  data[NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE];     /* ctrl0073dp.h:162 */
    u32 size;                                         /* ctrl0073dp.h:163 */
    u32 replyType;                                    /* ctrl0073dp.h:164 */
    u32 retryTimeMs;                                  /* ctrl0073dp.h:165 */
} nv0073_dp_auxch_ctrl_params_t;

/* --- cmd field bitfields (ctrl0073dp.h:168-177) ---------------------------
 *   NV0073_CTRL_DP_AUXCH_CMD_TYPE            3:3   (168)
 *     _TYPE_I2C  = 0                               (169)
 *     _TYPE_AUX  = 1                               (170)
 *   NV0073_CTRL_DP_AUXCH_CMD_I2C_MOT         2:2   (171)
 *     _I2C_MOT_FALSE = 0                           (172)
 *     _I2C_MOT_TRUE  = 1                           (173)
 *   NV0073_CTRL_DP_AUXCH_CMD_REQ_TYPE        1:0   (174)
 *     _REQ_TYPE_WRITE        = 0                   (175)
 *     _REQ_TYPE_READ         = 1                   (176)
 *     _REQ_TYPE_WRITE_STATUS = 2                   (177)
 *
 * For a native-AUX READ: TYPE=AUX(1)<<3 | I2C_MOT=FALSE(0)<<2 | REQ_TYPE=READ(1).
 *   = 0x8 | 0x0 | 0x1 = 0x9.
 * This is exactly the value r535_dp_aux_xfer receives as `type` from the DRM AUX
 * layer for DP_AUX_NATIVE_READ (disp.c:1086 "ctrl->cmd = type"), confirming the
 * NV0073 cmd encoding and the DRM request byte are identical.
 */
#define NV0073_DP_AUXCH_CMD_TYPE_AUX_SHIFT      3    /* field 3:3, ctrl0073dp.h:168/170 */
#define NV0073_DP_AUXCH_CMD_REQ_TYPE_READ       1    /* field 1:0, ctrl0073dp.h:176 */
#define NV0073_DP_AUXCH_CMD_NATIVE_AUX_READ \
    ((1u << NV0073_DP_AUXCH_CMD_TYPE_AUX_SHIFT) | NV0073_DP_AUXCH_CMD_REQ_TYPE_READ)  /* = 0x9 */
#define NV0073_DP_AUXCH_CMD_REQ_TYPE_WRITE      0    /* field 1:0, ctrl0073dp.h:175 */
#define NV0073_DP_AUXCH_CMD_NATIVE_AUX_WRITE \
    ((1u << NV0073_DP_AUXCH_CMD_TYPE_AUX_SHIFT) | NV0073_DP_AUXCH_CMD_REQ_TYPE_WRITE) /* = 0x8 */

/* --- addr field -----------------------------------------------------------
 *   NV0073_CTRL_DP_AUXCH_ADDR   20:0   (ctrl0073dp.h:179)
 * DPCD receiver-cap block starts at 0x00000 (nvkm/engine/disp/dp.h:12).
 */
#define DPCD_RECEIVER_CAP_BASE  0x00000u   /* DPCD_RC00_DPCD_REV, disp.h:12 */

/* --- DPCD receiver-capability field bits ----------------------------------
 * nvkm/engine/disp/dp.h:12-20
 *   DPCD_RC00_DPCD_REV            0x00000   (12)  raw[0] = DPCD revision (0x14 = 1.4)
 *   DPCD_RC01_MAX_LINK_RATE       0x00001   (13)  raw[1] = max link rate code
 *   DPCD_RC02                     0x00002   (14)  raw[2]:
 *     DPCD_RC02_ENHANCED_FRAME_CAP   0x80   (15)  bit7
 *     DPCD_RC02_TPS3_SUPPORTED       0x40   (16)  bit6
 *     DPCD_RC02_MAX_LANE_COUNT       0x1f   (17)  bits[4:0]
 *   DPCD_RC03                     0x00003   (18)  raw[3]:
 *     DPCD_RC03_TPS4_SUPPORTED       0x80   (19)  bit7
 *     DPCD_RC03_MAX_DOWNSPREAD       0x01   (20)  bit0
 * Confirmed in use at nvkm/engine/disp/dp.c:512 (ef = RC02 & ENHANCED_FRAME_CAP)
 * and dp.c:242-243 (RC03 & TPS4_SUPPORTED).
 */
#define DPCD_RC02_ENHANCED_FRAME_CAP  0x80u   /* dp.h:15 */
#define DPCD_RC02_MAX_LANE_COUNT      0x1fu   /* dp.h:17 */
#define DPCD_RC03_TPS4_SUPPORTED      0x80u   /* dp.h:19 */

bool nv_dp_aux_read(nv_card_t *c, nv_rm_t *rm, u32 objcom, u32 display_id,
                    u32 addr, u8 *data, u32 len) {
    if (!data || !len) return false;
    while (len) {
        u32 chunk = len > NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE ?
                    NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE : len;
        bool complete = false;
        for (u32 attempt = 0; attempt < 3u; attempt++) {
            nv0073_dp_auxch_ctrl_params_t p;
            memset(&p, 0, sizeof p);
            p.displayId = display_id;
            p.cmd = NV0073_DP_AUXCH_CMD_NATIVE_AUX_READ;
            p.addr = addr;
            p.size = chunk - 1u;
            u32 got = 0;
            bool ok = nv_rm_control(c, rm, objcom,
                                    NV0073_CTRL_CMD_DP_AUXCH_CTRL,
                                    &p, sizeof p, &p, sizeof p, &got);
            if (!ok) {
                kwarn("nv-dp-aux", "DPCD read %#x+%u refused", addr, chunk);
                return false;
            }
            if (p.retryTimeMs) {
                u64 dl = g_uptime_ms + p.retryTimeMs;
                while (g_uptime_ms < dl) timer_udelay(500);
                continue;
            }
            /* RM reports the returned byte count in size.  Some revisions
             * preserve request's count-minus-one on full reads, so accept
             * either representation only when AUX ACKed (replyType zero). */
            u32 returned = p.size;
            if (returned == chunk - 1u && p.replyType == 0u) returned = chunk;
            if (returned < chunk || p.replyType != 0u) {
                kwarn("nv-dp-aux", "short/NACK DPCD read %#x: %u/%u reply %u",
                      addr, returned, chunk, p.replyType);
                return false;
            }
            memcpy(data, p.data, chunk);
            complete = true;
            break;
        }
        if (!complete) return false;
        addr += chunk;
        data += chunk;
        len -= chunk;
    }
    return true;
}

bool nv_dp_read_dpcd(nv_card_t *c, nv_rm_t *rm, u32 objcom, u32 display_id,
                     nv_dpcd_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);

    nv0073_dp_auxch_ctrl_params_t p;
    u32 got_bytes = 0;

    /* Retry loop mirrors r535_dp_aux_xfer (disp.c:1078): GSP may DEFER an AUX
     * transaction (eDP panel not yet ready) and hand back retryTimeMs; re-issue
     * after waiting.  Cap at 3 tries like nouveau. */
    for (int attempt = 0; attempt < 3; attempt++) {
        memset(&p, 0, sizeof p);

        /* disp.c:1083-1092 - fill the request. */
        p.subDeviceInstance = 0;                              /* disp.c:1083 */
        p.displayId         = display_id;   /* disp.c:1084 uses BIT(outp->index);
                                             * caller passes the same displayId mask */
        p.bAddrOnly         = 0;                              /* disp.c:1085 (!size, size=16) */
        p.cmd               = NV0073_DP_AUXCH_CMD_NATIVE_AUX_READ; /* disp.c:1086 ctrl->cmd = type */
        p.addr              = DPCD_RECEIVER_CAP_BASE;         /* disp.c:1091 ctrl->addr = addr */
        /* disp.c:1092: for a real (non addr-only) xfer, size field = requested-1. */
        p.size              = (u32)(NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE - 1); /* 16 -> 15 */
        /* disp.c:1093 memcpy(ctrl->data, data, size); for a READ the outbound
         * data is don't-care (already zeroed). */

        got_bytes = 0;
        bool ok = nv_rm_control(c, rm, objcom, NV0073_CTRL_CMD_DP_AUXCH_CTRL,
                                &p, sizeof p, &p, sizeof p, &got_bytes);
        /* Record the AUX-level reply (ACK/NACK/DEFER/TIMEOUT) on every path so the
         * caller can tell "RM refused the control" from "the sink did not ACK". */
        out->reply_type = p.replyType;
        if (!ok) {
            kwarn("nv-dp-aux",
                  "AUXCH_CTRL refused (displayId=0x%x addr=0x%x attempt=%d replyType=%u)",
                  display_id, DPCD_RECEIVER_CAP_BASE, attempt, p.replyType);
            out->valid = false;
            return false;
        }

        /* disp.c:1096 - GSP asked us to wait and retry (deferred). */
        if (p.retryTimeMs) {
            kinfo("nv-dp-aux", "GSP deferred AUX read, waiting %u ms (attempt %d)",
                  p.retryTimeMs, attempt);
            /* Same busy-wait the relight path in nv_dispca7d.c uses for GSP
             * retryTimeMs delays (timer_udelay/g_uptime_ms from time.h). */
            u64 dl = g_uptime_ms + p.retryTimeMs;
            while (g_uptime_ms < dl) timer_udelay(500);
            continue;
        }

        /* disp.c:1108 - the reply overwrites ctrl->size with the actual byte
         * count returned by the sink (may be short/deferred). replyType is at
         * ctrl->replyType (disp.c:1109). */
        got_bytes = p.size;
        break;
    }

    /* AUX reads can come back short.  Copy the whole 16-byte reply buffer (as
     * nouveau does, disp.c:1107), but only trust up to got_bytes; parse whatever
     * arrived.  Clamp defensively. */
    if (got_bytes > NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE)
        got_bytes = NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE;
    memcpy(out->raw, p.data, NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE);
    out->got_bytes = got_bytes;
    out->reply_type = p.replyType;

    if (got_bytes == 0) {
        kwarn("nv-dp-aux", "AUX read returned 0 bytes (displayId=0x%x replyType=%u)",
              display_id, p.replyType);
        out->valid = false;
        return false;
    }

    /* Parse the receiver-cap block (only the fields within the first 4 bytes). */
    out->rev               = out->raw[0];                    /* DPCD_RC00, dp.h:12 */
    out->max_link_rate_code = (got_bytes > 1) ? out->raw[1] : 0; /* DPCD_RC01, dp.h:13 */
    out->max_lanes         = (got_bytes > 2)
                             ? (out->raw[2] & DPCD_RC02_MAX_LANE_COUNT) : 0; /* dp.h:17 */
    out->enhanced_framing  = (got_bytes > 2)
                             && (out->raw[2] & DPCD_RC02_ENHANCED_FRAME_CAP); /* dp.h:15 */
    out->tps4_supported    = (got_bytes > 3)
                             && (out->raw[3] & DPCD_RC03_TPS4_SUPPORTED);     /* dp.h:19 */
    out->valid = true;

    kinfo("nv-dp-aux",
          "DPCD[0..3]=%02x %02x %02x %02x (%u bytes): rev=%u.%u rate_code=0x%02x "
          "lanes=%u ef=%d tps4=%d",
          out->raw[0], out->raw[1], out->raw[2], out->raw[3], got_bytes,
          out->rev >> 4, out->rev & 0xf, out->max_link_rate_code,
          out->max_lanes, (int)out->enhanced_framing, (int)out->tps4_supported);

    return true;
}

/* Native-AUX WRITE of a single DPCD byte (e.g. DP_SET_POWER 0x600 = D0 to wake the
 * sink receiver before video - nouveau_dp.c:423-428).  Mirrors the read path but
 * REQ_TYPE=WRITE, size = nbytes-1 = 0 for one byte, data[0] = val. */
bool nv_dp_write_dpcd(nv_card_t *c, nv_rm_t *rm, u32 objcom, u32 display_id,
                      u32 addr, u8 val) {
    nv0073_dp_auxch_ctrl_params_t p;
    for (int attempt = 0; attempt < 3; attempt++) {
        memset(&p, 0, sizeof p);
        p.subDeviceInstance = 0;
        p.displayId         = display_id;
        p.bAddrOnly         = 0;
        p.cmd               = NV0073_DP_AUXCH_CMD_NATIVE_AUX_WRITE;
        p.addr              = addr;
        p.size              = 0;              /* 1 byte -> size-1 = 0 */
        p.data[0]           = val;
        u32 got = 0;
        bool ok = nv_rm_control(c, rm, objcom, NV0073_CTRL_CMD_DP_AUXCH_CTRL,
                                &p, sizeof p, &p, sizeof p, &got);
        if (!ok) {
            kwarn("nv-dp-aux", "DPCD write %#x=%#x refused (attempt %d)", addr, val, attempt);
            return false;
        }
        if (p.retryTimeMs) {                  /* DEFER: wait and retry */
            u64 dl = g_uptime_ms + p.retryTimeMs;
            while (g_uptime_ms < dl) timer_udelay(500);
            continue;
        }
        kinfo("nv-dp-aux", "DPCD write %#x=%#x ok (replyType %u)", addr, val, p.replyType);
        return true;
    }
    return false;
}
