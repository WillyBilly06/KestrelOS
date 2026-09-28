#ifndef KESTREL_NVRM_DP_LINK_H
#define KESTREL_NVRM_DP_LINK_H
/* Host-only readback; callers serialize with device teardown/modesetting.
 * These are snapshots, not proof of visible pixels. No direct GSP transport,
 * AUX writes, training requests, SOR assignment or ownership changes. */
typedef struct {
    u32 tx_status, tx_after_status, aux_status;
    u32 aux_reply, aux_bytes;
    u32 lanes, rate_code, rate_10mbps;
    bool fec, tx_valid, rx_valid, tx_config_stable;
    bool legacy_lock_known, legacy_locked;
    u8 receiver[6]; /* DPCD 0x200..0x205: count/IRQ/lane01/lane23/align/sink */
    u32 msa_status, mvid, nvid;
    u32 h_total, v_total, h_start, v_start, width, height, h_sync, v_sync;
    u8 misc0, misc1;
    bool h_positive, v_positive;
} nvrm_dp_link_t;
void nvrm_host_read_dp_link(u32 client, u32 display_object, u32 display_id,
                           nvrm_dp_link_t *out);
#endif
