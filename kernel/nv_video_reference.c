/* A real compressed, nonuniform frame through the application NVDEC backend.
 * Only the GPU-test boot path calls this, before userland. The reference was
 * independently encoded/decoded by the installed Windows NVIDIA driver; it is
 * NOT native NVENC output and must never stand in for the encoder round trip. */
#include "kernel.h"
#include "klog.h"
#include "crypto.h"
#include "nv_video.h"
#include "../tools/fixtures/h264_nvenc_256.h"
#include "../tools/fixtures/h264_nvdec_256_oracle.h"

static void nv_video_digest_hex(const u8 digest[SHA256_SIZE], char text[65]) {
    static const char hex[]="0123456789abcdef";
    for (u32 i=0;i<SHA256_SIZE;i++) {
        text[i*2]=hex[digest[i]>>4];text[i*2+1]=hex[digest[i]&15u];
    }
    text[64]=0;
}

int nv_nvdec_application_selftest_hw(void) {
    enum { WIDTH=H264_NVDEC_REFERENCE_WIDTH, HEIGHT=H264_NVDEC_REFERENCE_HEIGHT,
           Y_BYTES=WIDTH*HEIGHT, UV_BYTES=Y_BYTES/2, BYTES=Y_BYTES+UV_BYTES };
    _Static_assert(WIDTH==256 && HEIGHT==256, "archived reference geometry");
    static u8 decoded[BYTES];
    memset(decoded,0xa5,sizeof decoded);
    kvideo_request_t info={.version=KVIDEO_ABI,.operation=KVIDEO_H264_DECODE_IDR};
    int rc=nv_nvdec_decode_idr(h264_nvenc_256,sizeof h264_nvenc_256,
                               decoded,sizeof decoded,&info);
    if (rc || info.phase!=KVIDEO_PHASE_COMPLETE || info.parse_status ||
        info.firmware_error || info.slice_error || info.error_mbs ||
        info.decoded_mbs!=Y_BYTES/256 || info.required_bytes!=BYTES ||
        info.written_bytes!=BYTES || info.coded_width!=WIDTH || info.coded_height!=HEIGHT ||
        info.display_width!=WIDTH || info.display_height!=HEIGHT || info.pitch!=WIDTH ||
        info.crop_left || info.crop_top || info.crop_right || info.crop_bottom) {
        kwarn("nv-video", "application NVDEC reference FAIL: rc=%d phase=%u parse=%u firmware=%#x slice=%#x MB=%u error-MB=%u bytes=%u/%u",
              rc,info.phase,info.parse_status,info.firmware_error,info.slice_error,
              info.decoded_mbs,info.error_mbs,info.written_bytes,info.required_bytes);
        return 1;
    }
    /* Hash ALL coded Y/UV bytes separately after successful completion and
     * resource release. The CPU hashes bytes, never decodes H.264 macroblocks. */
    u8 y_hash[SHA256_SIZE],uv_hash[SHA256_SIZE];
    char y_text[65],uv_text[65];
    sha256(decoded,Y_BYTES,y_hash);sha256(decoded+Y_BYTES,UV_BYTES,uv_hash);
    nv_video_digest_hex(y_hash,y_text);nv_video_digest_hex(uv_hash,uv_text);
    bool pass=!memcmp(y_hash,h264_nvdec_256_y_sha256,SHA256_SIZE) &&
              !memcmp(uv_hash,h264_nvdec_256_uv_sha256,SHA256_SIZE);
    /* One digest per entry: klog's bounded message must retain ALL 64 hex
     * digits. Combining both silently truncated the UV proof on the USB. */
    kinfo("nv-video", "application NVDEC reference %s: %ux%u MB=%u (independent Windows oracle; not native NVENC)",
          pass?"PASS":"FAIL",(u32)WIDTH,(u32)HEIGHT,info.decoded_mbs);
    kinfo("nv-video", "application NVDEC reference Y-sha256=%s",y_text);
    kinfo("nv-video", "application NVDEC reference UV-sha256=%s",uv_text);
    return pass?0:1;
}
