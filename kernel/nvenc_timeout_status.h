/* Diagnostic-only reads of boot-lifetime storage after a failed submission.
 * Never a completion barrier, successful firmware result, bitstream length,
 * reason to unquarantine/replay a channel, or permission to release resources.
 * Included by nv_chan.c after the native RM/encoder allocation definitions. */
#ifndef KERNEL_NVENC_TIMEOUT_STATUS_H
#define KERNEL_NVENC_TIMEOUT_STATUS_H
static void nvenc_capture_unretired_status(nv_channel_t *ch, u64 offset)
{
    static bool captured;
    if (captured || !ch || !ch->rm || !ch->rm->host_api || !ch->submit_failed)
        return;
    captured = true;
    if (offset > NVENC_VRAM_BYTES || sizeof(nvenc_pic_stat_s) > NVENC_VRAM_BYTES - offset ||
        (offset & 3u)) {
        kerr("nvenc-status", "unretired status range invalid; no read issued");
        return;
    }
    nvenc_pic_stat_s samples[2];
    u32 status[2];
    /* Distinct destination poison detects a transfer falsely reporting success
     * without copying. The GPU allocation was initialized to 0xa5 before launch.
     * Use RM's independent coherent read path, never the failed encoder channel. */
    memset(&samples[0], 0x5a, sizeof samples[0]);
    memset(&samples[1], 0xc3, sizeof samples[1]);
    for (u32 i = 0; i < 2; i++)
        status[i] = nvrm_transfer_rm_memory(ch->rm->client, H_NVENC_VRAM, offset,
                                           &samples[i], sizeof samples[i], true);
    bool matching = !status[0] && !status[1] &&
                    !memcmp(&samples[0], &samples[1], sizeof samples[0]);
    kerr("nvenc-status", "UNRETIRED diagnostic only: read statuses %#x/%#x matching=%u; not a completed firmware result",
          status[0], status[1], matching);
    for (u32 i = 0; i < 2; i++) {
        if (status[i]) continue;
        const u8 *raw = (const u8 *)&samples[i];
        u32 poison = 0;
        for (u32 j = 0; j < sizeof samples[i]; j++) poison += raw[j] == 0xa5u;
        kerr("nvenc-status", "sample=%u initial-poison=%u/%u raw picture=%#x state=%#x ucode=%#x bits=%#x (unretired fields)",
              i, poison, (u32)sizeof samples[i], samples[i].picture_index,
              (u32)samples[i].error_status, (u32)samples[i].ucode_error_status,
              samples[i].total_bit_count);
        for (u32 off = 0; off < sizeof samples[i]; off += 32) {
            static const char hex[] = "0123456789abcdef";
            char text[65];
            u32 bytes = sizeof samples[i] - off;
            if (bytes > 32) bytes = 32;
            for (u32 j = 0; j < bytes; j++) {
                text[2*j] = hex[raw[off+j] >> 4];
                text[2*j+1] = hex[raw[off+j] & 15u];
            }
            text[2*bytes] = 0;
            kerr("nvenc-status", "sample=%u offset=%#x bytes=%u hex=%s", i, off, bytes, text);
        }
    }
}
#endif
