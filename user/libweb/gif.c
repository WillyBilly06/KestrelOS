/* gif.c - a GIF decoder to RGB.
 *
 * The third image format the web is made of, after JPEG (jpeg.c) and PNG
 * (png.c): logos, icons, and the small animations GIF is still used for.  This
 * decodes the first frame to RGB - enough to show the picture - built from the
 * spec rather than a library.
 *
 * The heart of it is LZW decompression, GIF's own variable-width code scheme:
 * a dictionary that starts as the colour indices themselves and grows by one
 * string per code as it goes, the code width stepping up as the dictionary
 * fills, reset whenever a clear code appears.  Around it is the block structure
 * - screen descriptor, colour table, image descriptor, sub-blocked data - and
 * the de-interlace pass for the interlaced form.
 *
 * Verified on the host against PIL's decode of the same files (gif_host_test.c),
 * exactly: GIF is lossless, so a correct LZW reproduces the indices and the
 * colour table reproduces the pixels.
 */
#include "gif.h"

#ifdef GIF_HOST_TEST
#include <string.h>
#include <stdlib.h>
#else
#include "kestrel.h"
#endif

typedef unsigned char u8;
typedef unsigned int  u32;

/* ============================================================== LZW ======= */

#define GIF_MAX_CODES 4096

typedef struct {
    const u8 *in, *in_end;
    u32 bitbuf;
    int bitcnt;
    /* sub-block reader: GIF data is a chain of (length, bytes) sub-blocks. */
    int block_left;
} gif_bits_t;

/* Pull the next data byte, crossing sub-block boundaries. */
static int gif_next_byte(gif_bits_t *b) {
    if (b->block_left == 0) {
        if (b->in >= b->in_end) return -1;
        b->block_left = *b->in++;
        if (b->block_left == 0) return -1;      /* block terminator */
    }
    if (b->in >= b->in_end) return -1;
    b->block_left--;
    return *b->in++;
}

static int gif_get_code(gif_bits_t *b, int width) {
    while (b->bitcnt < width) {
        int by = gif_next_byte(b);
        if (by < 0) return -1;
        b->bitbuf |= (u32)by << b->bitcnt;
        b->bitcnt += 8;
    }
    int code = b->bitbuf & ((1u << width) - 1);
    b->bitbuf >>= width;
    b->bitcnt -= width;
    return code;
}

/* Decode the LZW stream into `out` (indices), at most out_cap.  Returns count
 * or -1. */
static int gif_lzw(const u8 *data, u32 len, int min_code_size,
                   u8 *out, u32 out_cap) {
    gif_bits_t b = { data, data + len, 0, 0, 0 };

    int clear = 1 << min_code_size;
    int end = clear + 1;
    int width = min_code_size + 1;
    int next = clear + 2;

    /* Dictionary: each entry is a prefix code plus one appended byte, so a
     * string is walked backwards from its code. */
    static short prefix[GIF_MAX_CODES];
    static u8    suffix[GIF_MAX_CODES];
    static u8    first[GIF_MAX_CODES];
    for (int i = 0; i < clear; i++) { prefix[i] = -1; suffix[i] = (u8)i; first[i] = (u8)i; }

    u32 at = 0;
    u8 stack[GIF_MAX_CODES];
    int prev = -1;

    for (;;) {
        int code = gif_get_code(&b, width);
        if (code < 0) break;
        if (code == clear) {
            width = min_code_size + 1;
            next = clear + 2;
            prev = -1;
            continue;
        }
        if (code == end) break;

        int sp = 0;
        int cur = code;
        if (cur >= next) {
            /* The one deferred case: a code not yet in the table decodes to the
             * previous string plus its own first byte. */
            if (prev < 0) return -1;
            stack[sp++] = (u8)first[prev];
            cur = prev;
        }
        while (cur >= clear) {
            if (sp >= GIF_MAX_CODES || cur < 0) return -1;
            stack[sp++] = suffix[cur];
            cur = prefix[cur];
        }
        stack[sp++] = (u8)cur;

        while (sp > 0) {
            if (at < out_cap) out[at] = stack[--sp]; else return -1;
            at++;
        }

        if (prev >= 0 && next < GIF_MAX_CODES) {
            prefix[next] = (short)prev;
            suffix[next] = (u8)cur;             /* cur is now the first byte */
            first[next] = first[prev];
            next++;
            if (next == (1 << width) && width < 12) width++;
        }
        prev = code;
    }
    return (int)at;
}

/* ============================================================== GIF ======= */

static u32 rd16(const u8 *p) { return p[0] | (p[1] << 8); }

int gif_decode_anim(const void *bytes, size_t len, gif_anim_t *out) {
    const u8 *p = bytes, *end = (const u8 *)bytes + len;
    memset(out, 0, sizeof *out);
    if (len < 13) return GIF_EBADDATA;
    if (memcmp(p, "GIF87a", 6) && memcmp(p, "GIF89a", 6)) return GIF_ENOTGIF;
    p += 6;

    int cw = (int)rd16(p), ch = (int)rd16(p + 2);
    u8 packed = p[4];
    p += 7;
    if (cw <= 0 || ch <= 0) return GIF_EBADDATA;

    u8 gct[256 * 3];
    int have_gct = (packed & 0x80) != 0;
    int gct_n = have_gct ? (1 << ((packed & 0x07) + 1)) : 0;
    if (have_gct) {
        if (p + gct_n * 3 > end) return GIF_EBADDATA;
        memcpy(gct, p, (size_t)gct_n * 3);
        p += gct_n * 3;
    }

    /* The canvas frames are composited onto, plus what disposal needs: the
     * background colour, and a saved copy for restore-to-previous. */
    u8 *canvas = malloc((size_t)cw * ch * 3);
    u8 *saved  = malloc((size_t)cw * ch * 3);
    if (!canvas || !saved) { free(canvas); free(saved); return GIF_ENOMEM; }
    memset(canvas, 0, (size_t)cw * ch * 3);

    out->width = cw; out->height = ch; out->loop_count = 0;

    int cap = 8;
    out->frames = malloc(sizeof(gif_frame_t) * cap);
    if (!out->frames) { free(canvas); free(saved); return GIF_ENOMEM; }

    /* Graphic-control state for the NEXT image block. */
    int gce_delay = 0, gce_transp = -1, gce_disposal = 0;
    int prev_disposal = 0, prev_x = 0, prev_y = 0, prev_w = 0, prev_h = 0;
    int rc = GIF_EBADDATA;

    while (p < end) {
        u8 sep = *p++;
        if (sep == 0x3B) { rc = out->frame_count ? GIF_OK : GIF_EBADDATA; break; }

        if (sep == 0x21) {                       /* extension */
            if (p >= end) break;
            u8 label = *p++;
            if (label == 0xF9 && p + 6 <= end && p[0] == 4) {
                u8 fp = p[1];
                gce_disposal = (fp >> 2) & 0x7;
                gce_transp = (fp & 1) ? p[4] : -1;
                gce_delay = (int)rd16(p + 2) * 10;   /* 1/100 s -> ms */
            } else if (label == 0xFF && p + 1 <= end && p[0] == 11 &&
                       p + 15 <= end && !memcmp(p + 1, "NETSCAPE2.0", 11)) {
                /* the loop-count application extension */
                const u8 *q = p + 12;            /* sub-block: 03 01 loop(2) */
                if (q + 4 <= end && q[0] >= 3) out->loop_count = (int)rd16(q + 2);
            }
            while (p < end) { u8 n = *p++; if (!n) break; p += n; }
            continue;
        }
        if (sep != 0x2C) continue;

        if (p + 9 > end) break;
        int ix = (int)rd16(p), iy = (int)rd16(p + 2);
        int iw = (int)rd16(p + 4), ih = (int)rd16(p + 6);
        u8 ipacked = p[8];
        p += 9;

        const u8 *table = have_gct ? gct : NULL; int table_n = gct_n;
        u8 lct[256 * 3];
        if (ipacked & 0x80) {
            int n = 1 << ((ipacked & 0x07) + 1);
            if (p + n * 3 > end) break;
            memcpy(lct, p, (size_t)n * 3);
            p += n * 3;
            table = lct; table_n = n;
        }
        int interlaced = (ipacked & 0x40) != 0;
        if (!table || iw <= 0 || ih <= 0 || ix < 0 || iy < 0 ||
            ix + iw > cw || iy + ih > ch) break;

        int min_code_size = (p < end) ? *p++ : 0;
        if (min_code_size < 2 || min_code_size > 8) break;

        u8 *idx = malloc((size_t)iw * ih);
        if (!idx) { rc = GIF_ENOMEM; break; }
        int got = gif_lzw(p, (u32)(end - p), min_code_size, idx, (u32)(iw * ih));
        /* advance p past this image's sub-blocks */
        while (p < end) { u8 n = *p++; if (!n) break; p += n; }
        if (got < iw * ih) { free(idx); break; }

        /* Apply the PREVIOUS frame's disposal before drawing this one. */
        if (prev_disposal == 2) {                /* restore to background */
            for (int y = 0; y < prev_h; y++) {
                u8 *o = canvas + ((size_t)(prev_y + y) * cw + prev_x) * 3;
                memset(o, 0, (size_t)prev_w * 3);
            }
        } else if (prev_disposal == 3) {         /* restore to previous */
            memcpy(canvas, saved, (size_t)cw * ch * 3);
        }
        if (gce_disposal == 3) memcpy(saved, canvas, (size_t)cw * ch * 3);

        /* Draw this image onto the canvas, honouring transparency. */
        static const int istart[4] = { 0, 4, 2, 1 };
        static const int istep[4]  = { 8, 8, 4, 2 };
        int src_row = 0;
        for (int pass = 0; pass < (interlaced ? 4 : 1); pass++) {
            int first_row = interlaced ? istart[pass] : 0;
            int step = interlaced ? istep[pass] : 1;
            for (int y = first_row; y < ih; y += step) {
                const u8 *si = idx + (size_t)src_row * iw;
                for (int x = 0; x < iw; x++) {
                    int ci = si[x];
                    if (ci == gce_transp) continue;   /* leave what's under */
                    if (ci >= table_n) ci = 0;
                    u8 *o = canvas + ((size_t)(iy + y) * cw + (ix + x)) * 3;
                    o[0] = table[ci * 3]; o[1] = table[ci * 3 + 1]; o[2] = table[ci * 3 + 2];
                }
                src_row++;
            }
        }
        free(idx);

        /* Snapshot the canvas as this frame. */
        if (out->frame_count >= cap) {
            cap *= 2;
            gif_frame_t *nf = malloc(sizeof(gif_frame_t) * cap);
            if (!nf) { rc = GIF_ENOMEM; break; }
            memcpy(nf, out->frames, sizeof(gif_frame_t) * out->frame_count);
            free(out->frames); out->frames = nf;
        }
        u8 *snap = malloc((size_t)cw * ch * 3);
        if (!snap) { rc = GIF_ENOMEM; break; }
        memcpy(snap, canvas, (size_t)cw * ch * 3);
        out->frames[out->frame_count].rgb = snap;
        out->frames[out->frame_count].delay_ms = gce_delay ? gce_delay : 100;
        out->frame_count++;

        prev_disposal = gce_disposal; prev_x = ix; prev_y = iy; prev_w = iw; prev_h = ih;
        gce_delay = 0; gce_transp = -1; gce_disposal = 0;   /* consumed */
        rc = GIF_OK;
    }

    free(canvas); free(saved);
    if (rc != GIF_OK || out->frame_count == 0) { gif_anim_free(out); return rc == GIF_OK ? GIF_EBADDATA : rc; }
    return GIF_OK;
}

void gif_anim_free(gif_anim_t *out) {
    if (!out || !out->frames) return;
    for (int i = 0; i < out->frame_count; i++) free(out->frames[i].rgb);
    free(out->frames);
    out->frames = 0; out->frame_count = 0;
}

/* First frame only, the common case for a still <img>. */
int gif_decode(const void *bytes, size_t len, gif_image_t *out) {
    memset(out, 0, sizeof *out);
    gif_anim_t a;
    int rc = gif_decode_anim(bytes, len, &a);
    if (rc != GIF_OK) return rc;
    out->width = a.width; out->height = a.height;
    out->rgb = a.frames[0].rgb;                  /* take frame 0's buffer */
    a.frames[0].rgb = 0;                          /* don't let free() take it */
    gif_anim_free(&a);
    return GIF_OK;
}

void gif_free(gif_image_t *img) {
    if (img && img->rgb) { free(img->rgb); img->rgb = 0; }
}
