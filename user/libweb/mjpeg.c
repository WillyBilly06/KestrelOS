/* mjpeg.c - Motion JPEG: software video from the JPEG decoder.
 *
 * The user asked for the OS to decode video.  Real web video (H.264/HEVC) needs
 * a codec far larger than anything here, but Motion JPEG - a video that is just
 * a sequence of JPEG frames - is decodable with the baseline JPEG decoder
 * already written (jpeg.c), and it is a real format (webcams, some AVI/MOV).
 * This is that: find each frame in the stream and hand it to jpeg_decode.
 *
 * The stream form handled is the simplest: JPEG frames one after another, each
 * starting with the SOI marker FF D8.  A frame runs from its SOI to the next
 * one (or the end).  Timing is not carried in this raw form, so a caller plays
 * at a chosen rate; a container that carries a frame rate (AVI) would set it.
 *
 * Verified on the host (mjpeg_host_test.c): each frame decodes to within the
 * JPEG decoder's own rounding of PIL's decode of the same frame.
 */
#include "mjpeg.h"

#ifdef MJPEG_HOST_TEST
#include <string.h>
#include <stdlib.h>
#else
#include "kestrel.h"
#endif

#include "jpeg.h"

typedef unsigned char u8;

/* The length of ONE JPEG starting at d[0], by walking its markers to the EOING
 * marker.  Crucially this skips APPn segments by their declared length, so an
 * EXIF thumbnail (a whole JPEG embedded in APP1) is NOT mistaken for a separate
 * frame - which a naive scan for FF D8 would do, and which would corrupt normal
 * JPEG display in the browser.  Returns 0 if it does not start with SOI. */
static size_t jpeg_one_length(const u8 *d, size_t len) {
    if (len < 2 || d[0] != 0xFF || d[1] != 0xD8) return 0;   /* SOI */
    size_t i = 2;
    while (i + 1 < len) {
        if (d[i] != 0xFF) { i++; continue; }
        u8 m = d[i + 1];
        if (m == 0xFF) { i++; continue; }                    /* fill byte */
        if (m == 0xD9) return i + 2;                         /* EOI -> end */
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            i += 2; continue;                                /* standalone markers */
        }
        if (i + 4 > len) return len;
        size_t seg = ((size_t)d[i + 2] << 8) | d[i + 3];     /* length-bearing */
        i += 2 + seg;
        if (m == 0xDA) {                                     /* SOS: entropy data */
            while (i + 1 < len) {
                if (d[i] == 0xFF) {
                    u8 mm = d[i + 1];
                    if (mm == 0x00) { i += 2; continue; }    /* stuffed FF 00 */
                    if (mm >= 0xD0 && mm <= 0xD7) { i += 2; continue; } /* RST */
                    if (mm == 0xFF) { i++; continue; }
                    break;                                   /* a real marker next */
                }
                i++;
            }
        }
    }
    return len;
}

/* Byte range of frame `n`, splitting the stream one JPEG at a time. */
static int mjpeg_frame_range(const u8 *d, size_t len, int n,
                             size_t *off, size_t *flen) {
    size_t at = 0; int idx = 0;
    while (at + 1 < len) {
        if (!(d[at] == 0xFF && d[at + 1] == 0xD8)) { at++; continue; }  /* seek SOI */
        size_t l = jpeg_one_length(d + at, len - at);
        if (!l) return 0;
        if (idx == n) { *off = at; *flen = l; return 1; }
        idx++;
        at += l;
    }
    return 0;
}

int mjpeg_frame_count(const void *bytes, size_t len) {
    const u8 *d = bytes;
    size_t at = 0; int count = 0;
    while (at + 1 < len) {
        if (!(d[at] == 0xFF && d[at + 1] == 0xD8)) { at++; continue; }
        size_t l = jpeg_one_length(d + at, len - at);
        if (!l) break;
        count++;
        at += l;
    }
    return count;
}

int mjpeg_decode_frame(const void *bytes, size_t len, int n, mjpeg_frame_t *out) {
    const u8 *d = bytes;
    memset(out, 0, sizeof *out);
    size_t off, flen;
    if (!mjpeg_frame_range(d, len, n, &off, &flen)) return MJPEG_ENOFRAME;

    jpeg_image_t j;
    if (jpeg_decode(d + off, flen, &j) != JPEG_OK) return MJPEG_EBADFRAME;
    out->width = j.width;
    out->height = j.height;
    out->rgb = j.rgb;                                    /* jpeg_decode malloc'd it */
    return MJPEG_OK;
}

void mjpeg_frame_free(mjpeg_frame_t *f) {
    if (f && f->rgb) { free(f->rgb); f->rgb = 0; }
}
