/* Error-only, non-consuming NVENC diagnostic capture. Included by nv_chan.c
 * after channel/RM definitions; callers hold its existing render transaction.
 * NVIDIA 595.99.02 ctrl2080rc.h: RC_GET_ERROR_COUNT and RC_GET_ERROR_V2.
 * kernel_rc_ctrl.c forwards these to physical GSP and serializes the journal
 * without clearing it. journal.c appends records, so count-1 is newest.
 * These are GLOBAL records: proximity is not proof of NVENC attribution.
 * Never use a failed picture's unretired status memory as a firmware result. */
#ifndef KERNEL_NVENC_RC_JOURNAL_H
#define KERNEL_NVENC_RC_JOURNAL_H

typedef struct {
    u32 which_buffer;
    u32 output_record_size;
    u8 record_buffer[8192];
} nvenc_rc_record_t;
_Static_assert(sizeof(nvenc_rc_record_t) == 8200, "NV2080 RC_GET_ERROR_V2 ABI");
_Static_assert(sizeof(nvenc_rc_record_t) <= NV_RM_MAX_PARAMS, "RC journal RPC capacity");

static void nvenc_capture_rc_journal(nv_channel_t *ch) {
    static bool captured;
    static nvenc_rc_record_t record; /* Avoid an 8 KiB fault-path stack frame. */
    if (captured) return;
    captured = true; /* Bounded once, including refused/empty queries. */
    u32 count = 0, got = 0;
    bool ok = nv_rm_control(ch->card, ch->rm, RM_SUBDEVICE, 0x20802205u,
                            &count, sizeof count, &count, sizeof count, &got);
    kerr("nvenc-rc", "global journal count query: status=%#x reply=%u valid=%u count=%u (not a channel-specific firmware code)",
          nv_last_control_status, got, ok && got == sizeof count, count);
    if (!ok || got != sizeof count || !count) return;
    /* Bound the GSP linked-list walk too, not only our local dump loop. */
    if (count > 1024u) {
        kerr("nvenc-rc", "journal count exceeds bounded capture limit; no record queries issued");
        return;
    }
    const u32 first = count > 2u ? count - 2u : 0u;
    for (u32 index = first; index < count; index++) {
        memset(&record, 0, sizeof record);
        record.which_buffer = index;
        got = 0;
        ok = nv_rm_control(ch->card, ch->rm, RM_SUBDEVICE, 0x20802213u,
                            &record, sizeof record, &record, sizeof record, &got);
        kerr("nvenc-rc", "global journal record=%u status=%#x reply=%u bytes=%u (unattributed; history retained)",
              index, nv_last_control_status, got, record.output_record_size);
        if (!ok || got != sizeof record || record.which_buffer != index ||
            record.output_record_size > sizeof record.record_buffer) continue;
        const u32 bytes = record.output_record_size;
        if (!bytes) continue; /* Journal may change between the two controls. */
        /* rmcd.h RmProtoBuf_RECORD: NVCD_RECORD {u8 group,type;u16 size},
         * u32 protobuf bytes. Keep exact raw data even if the format differs;
         * no inferred firmware error or traversal of unvalidated protobuf. */
        if (bytes >= 8u) {
            u16 header_size;
            u32 payload_size;
            memcpy(&header_size, record.record_buffer + 2, sizeof header_size);
            memcpy(&payload_size, record.record_buffer + 4, sizeof payload_size);
            kerr("nvenc-rc", "record=%u group=%u type=%u header=%u payload=%u known-envelope=%u",
                  index, record.record_buffer[0], record.record_buffer[1],
                  header_size, payload_size,
                  record.record_buffer[0] == 1u && record.record_buffer[1] == 131u &&
                  header_size == 8u && payload_size == bytes - 8u);
        }
        for (u32 offset = 0; offset < bytes; offset += 32u) {
            static const char hex[] = "0123456789abcdef";
            char text[65];
            u32 length = bytes - offset;
            if (length > 32u) length = 32u;
            for (u32 i = 0; i < length; i++) {
                u8 value = record.record_buffer[offset + i];
                text[i * 2u] = hex[value >> 4];
                text[i * 2u + 1u] = hex[value & 15u];
            }
            text[length * 2u] = 0;
            kerr("nvenc-rc", "record=%u offset=%#x bytes=%u hex=%s", index, offset, length, text);
        }
    }
}
#endif
