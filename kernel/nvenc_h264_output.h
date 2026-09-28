/* Bounded transport of the encoder smoke test's actual Annex-B output.
 * This locates one IDR NAL; it does NOT validate SPS/PPS, slice syntax, encoded
 * pixels or general H.264 conformance. Decoder round-trip validation is separate.
 */
#ifndef KESTREL_NVENC_H264_OUTPUT_H
#define KESTREL_NVENC_H264_OUTPUT_H
#define NVENC_H264_TEST_OUTPUT_CAPACITY 0x40000u
#define NVENC_H264_TEST_WIDTH 256u
#define NVENC_H264_TEST_HEIGHT 256u

typedef struct {
    unsigned char data[NVENC_H264_TEST_OUTPUT_CAPACITY] __attribute__((aligned(4)));
    unsigned bytes, slice_start, slice_end;
} nvenc_h264_test_output_s;

/* Slice offsets include the three/four-byte start code, as NVDEC expects.
 * Leading/trailing zero bytes are allowed. Reject a truncated NAL, forbidden
 * bit, non-reference IDR, extra VCL picture/slice, or unsupported NAL type.
 * Emulation-prevention bytes (00 00 03 xx) cannot masquerade as a start code.
 * Only the locally configured, single-IDR test is accepted, not arbitrary video.
 */
static inline int nvenc_h264_single_idr(const unsigned char *data, unsigned bytes,
                               unsigned *slice_start, unsigned *slice_end) {
    if (!slice_start || !slice_end) return 0;
    *slice_start = *slice_end = 0;
    if (!data || bytes > NVENC_H264_TEST_OUTPUT_CAPACITY || bytes < 5) return 0;
    unsigned header = 0, previous_type = 0, found = 0, start = 0, end = 0;
    for (unsigned i = 0; i + 2 < bytes; i++) {
        if (data[i] || data[i+1]) continue;
        unsigned prefix = data[i+2] == 1 ? 3u :
            (i+3 < bytes && !data[i+2] && data[i+3] == 1 ? 4u : 0u);
        if (!prefix) continue;
        if (previous_type) {
            if (i <= header+1) return 0; /* no NAL payload */
            if (previous_type == 5) end = i;
        } else for (unsigned k = 0; k < i; k++) if (data[k]) return 0;
        header = i + prefix;
        if (header >= bytes || (data[header] & 0x80u)) return 0;
        unsigned type = data[header] & 31u;
        if (type == 5) {
            if (found || !(data[header] & 0x60u)) return 0;
            found = 1; start = i;
        } else if (type < 6 || type > 12) return 0;
        previous_type = type;
        i = header; /* scan payload next; never rescan the prefix */
    }
    if (!found || !previous_type || bytes <= header+1) return 0;
    if (previous_type == 5) end = bytes;
    if (end <= start) return 0;
    *slice_start = start; *slice_end = end; return 1;
}
#endif
