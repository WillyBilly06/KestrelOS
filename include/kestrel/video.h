/* Synchronous hardware video interface. CPU parses headers and copies pixels;
 * compressed slice data is decoded by NVDEC, never a software fallback.
 * ABI 1 deliberately exposes independently decodable baseline H.264 IDRs,
 * not arbitrary H.264 playback, other codecs, or a working encoder claim. */
#ifndef KESTREL_VIDEO_H
#define KESTREL_VIDEO_H
#define KVIDEO_ABI 1u
#define KVIDEO_H264_INSPECT 0u /* metadata only: does not require/prove a GPU */
#define KVIDEO_H264_DECODE_IDR 1u
#define KVIDEO_H264_ENCODE_PLAN 2u /* geometry/capacity only, no hardware work */
#define KVIDEO_H264_ENCODE_IDR 3u  /* native NVENC; currently 256x256 NV12 only */
#define KVIDEO_ENCODE_WIDTH 256u
#define KVIDEO_ENCODE_HEIGHT 256u
#define KVIDEO_ENCODE_INPUT_BYTES (KVIDEO_ENCODE_WIDTH*KVIDEO_ENCODE_HEIGHT*3u/2u)
#define KVIDEO_ENCODE_OUTPUT_MAX (0x40000u+128u)
#define KVIDEO_INPUT_MAX (16u * 1024u * 1024u)
#define KVIDEO_OUTPUT_MAX (4096u * 4096u * 3u / 2u)
#define KVIDEO_PHASE_HEADERS 1u
#define KVIDEO_PHASE_LAYOUT 2u
#define KVIDEO_PHASE_ALLOCATE 3u
#define KVIDEO_PHASE_UPLOAD 4u
#define KVIDEO_PHASE_SUBMIT 5u
#define KVIDEO_PHASE_STATUS 6u
#define KVIDEO_PHASE_READBACK 7u
#define KVIDEO_PHASE_UNTILE 8u
#define KVIDEO_PHASE_RELEASE 9u
#define KVIDEO_PHASE_COMPLETE 10u
#define KVIDEO_PHASE_TILE 11u
#define KVIDEO_PHASE_PACKAGE 12u
/* Input/output are USER addresses, snapshotted/validated by SYS_GPU. No GPU
 * address or kernel pointer crosses this interface. INSPECT returns required
 * bytes without decoding. DECODE success returns tightly packed coded NV12
 * (Y, then interleaved UV), pitch=coded_width. Cropping describes the display
 * rectangle; it does not change allocation size. written_bytes is zero unless
 * the entire retired output, firmware status, conversion and cleanup succeed.
 * Decode failure leaves the user's pixel buffer unchanged; a later user-memory
 * copy fault may leave a prefix. Consume output only after syscall success.
 * Metadata includes
 * the failing phase and (only after a completion fence) firmware error codes.
 * reserved must be zero. Calls may return E_BUSY; retry without CPU fallback.
 * ENCODE_PLAN takes coded_width/height and returns capacity in required_bytes;
 * no input pointer is needed. ENCODE_IDR additionally takes exactly one tightly
 * packed NV12 image. It returns a standalone baseline H.264 IDR with SPS/PPS,
 * not a container or interframe session. required_bytes is a capacity bound;
 * written_bytes is the actual stream size. firmware_error/slice_error report
 * encoder status after retirement: firmware_error is zero for picture state 2,
 * otherwise raw two-bit state + 1; slice_error is raw ucode_error_status.
 * These fields do not replace syscall success or output validation. decoded_mbs and
 * error_mbs stay zero for encode. Encoding requests normal intra compression
 * at constant QP 26; it is lossy, not a byte-preserving image conversion. CPU
 * only moves pixels and packages parameter headers. Native encode remains
 * unverified; normal compression is not a claim of working video sessions. */
typedef struct {
    unsigned int version, operation;
    unsigned long long input, input_bytes, output, output_capacity;
    unsigned int coded_width, coded_height, display_width, display_height;
    unsigned int crop_left, crop_top, crop_right, crop_bottom;
    unsigned int pitch, required_bytes, written_bytes, phase;
    unsigned int parse_status, firmware_error, slice_error, decoded_mbs;
    unsigned int error_mbs, reserved;
} kvideo_request_t;
#if defined(__cplusplus)
static_assert(sizeof(kvideo_request_t)==112,"video ABI");
#else
_Static_assert(sizeof(kvideo_request_t)==112,"video ABI");
#endif
#endif
