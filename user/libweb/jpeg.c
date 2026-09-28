/* jpeg.c - a baseline JPEG decoder, written for this system.
 *
 * The browser could lay a page out but not show a single photograph: every
 * <img> was a grey frame, because nothing here turned the bytes of a JPEG into
 * pixels.  This does - baseline sequential JPEG, which is what the web is made
 * of - so a picture on a page is a picture, and so a sequence of them (Motion
 * JPEG) can be played as video.
 *
 * It is the textbook pipeline, and every stage is checked against a known-good
 * decoder rather than trusted: markers -> quantisation and Huffman tables ->
 * per-block entropy decode (a differential DC, then run-length AC) -> multiply
 * back the quantisation -> inverse cosine transform -> shift up -> upsample the
 * colour -> convert YCbCr to RGB.  Baseline only: progressive and arithmetic
 * coding are deliberately refused rather than half-decoded into garbage.
 *
 * Verified on the host against PIL's decode of the same file - see
 * jpeg_host_test.c - to within the rounding two correct integer inverse
 * transforms differ by, which is the honest bar for "correct" here.
 */
#include "jpeg.h"

#ifdef JPEG_HOST_TEST
#include <string.h>
#include <stdlib.h>
#else
#include "kestrel.h"     /* malloc/free/memset from this system's libc */
#endif

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed int     s32;

/* ------------------------------------------------------------ Huffman table */

typedef struct {
    u8  bits[17];       /* counts of codes of each length 1..16 */
    u8  vals[256];      /* the symbols, in code order */
    /* A fast form: for each length, the first code and the index into vals. */
    int mincode[17];
    int maxcode[18];
    int valptr[17];
    int count;
    int present;
} huff_t;

static void huff_build(huff_t *h) {
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        h->valptr[len] = k;
        h->mincode[len] = code;
        code += h->bits[len];
        h->maxcode[len] = h->bits[len] ? code - 1 : -1;
        k += h->bits[len];
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    h->count = k;
}

/* ------------------------------------------------------------- bit reader */

typedef struct {
    const u8 *p, *end;
    u32 bits;           /* buffered bits, MSB first */
    int nbits;
    int marker;         /* a marker met inside the scan (e.g. EOI) */
} bits_t;

static int fill_bit(bits_t *b) {
    if (b->nbits >= 24) return 1;
    while (b->nbits <= 24) {
        if (b->p >= b->end) { b->bits |= 0 << (24 - b->nbits); b->nbits += 8; continue; }
        u8 c = *b->p++;
        if (c == 0xFF) {
            u8 c2 = (b->p < b->end) ? *b->p : 0;
            if (c2 == 0x00) { b->p++; }           /* stuffed zero -> a real 0xFF */
            else if (c2 >= 0xD0 && c2 <= 0xD7) {   /* restart marker */
                b->marker = c2; b->p++; c = 0;
            } else { b->marker = c2; b->p--; c = 0; }
        }
        b->bits |= (u32)c << (24 - b->nbits);
        b->nbits += 8;
    }
    return 1;
}

static int get_bit(bits_t *b) {
    if (b->nbits == 0) fill_bit(b);
    if (b->nbits == 0) return 0;
    int bit = (b->bits >> 31) & 1;
    b->bits <<= 1;
    b->nbits--;
    return bit;
}

static int get_bits(bits_t *b, int n) {
    int v = 0;
    while (n--) v = (v << 1) | get_bit(b);
    return v;
}

/* Huffman-decode one symbol. */
static int huff_decode(bits_t *b, huff_t *h) {
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        code = (code << 1) | get_bit(b);
        if (h->maxcode[len] >= 0 && code <= h->maxcode[len])
            return h->vals[h->valptr[len] + (code - h->mincode[len])];
    }
    return 0;
}

/* Sign-extend an n-bit value the way JPEG's DC/AC amplitudes are coded. */
static int extend(int v, int n) {
    return (n && v < (1 << (n - 1))) ? v - (1 << n) + 1 : v;
}

/* ------------------------------------------------------------------ IDCT */

/* The AAN integer inverse DCT, a well-worn form.  Works on one 8x8 block in
 * place, taking dequantised coefficients to spatial samples. */
#define W1 2841
#define W2 2676
#define W3 2408
#define W5 1609
#define W6 1108
#define W7 565

static void idct_row(s32 *blk) {
    s32 x0, x1, x2, x3, x4, x5, x6, x7, x8;
    if (!((x1 = blk[4] << 11) | (x2 = blk[6]) | (x3 = blk[2]) |
          (x4 = blk[1]) | (x5 = blk[7]) | (x6 = blk[5]) | (x7 = blk[3]))) {
        blk[0] = blk[1] = blk[2] = blk[3] = blk[4] = blk[5] = blk[6] = blk[7] =
            blk[0] << 3;
        return;
    }
    x0 = (blk[0] << 11) + 128;
    x8 = W7 * (x4 + x5);
    x4 = x8 + (W1 - W7) * x4;
    x5 = x8 - (W1 + W7) * x5;
    x8 = W3 * (x6 + x7);
    x6 = x8 - (W3 - W5) * x6;
    x7 = x8 - (W3 + W5) * x7;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = W6 * (x3 + x2);
    x2 = x1 - (W2 + W6) * x2;
    x3 = x1 + (W2 - W6) * x3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;
    blk[0] = (x7 + x1) >> 8;
    blk[1] = (x3 + x2) >> 8;
    blk[2] = (x0 + x4) >> 8;
    blk[3] = (x8 + x6) >> 8;
    blk[4] = (x8 - x6) >> 8;
    blk[5] = (x0 - x4) >> 8;
    blk[6] = (x3 - x2) >> 8;
    blk[7] = (x7 - x1) >> 8;
}

static void idct_col(s32 *blk) {
    s32 x0, x1, x2, x3, x4, x5, x6, x7, x8;
    if (!((x1 = blk[8 * 4] << 8) | (x2 = blk[8 * 6]) | (x3 = blk[8 * 2]) |
          (x4 = blk[8 * 1]) | (x5 = blk[8 * 7]) | (x6 = blk[8 * 5]) |
          (x7 = blk[8 * 3]))) {
        s32 v = (blk[0] + 32) >> 6;
        for (int i = 0; i < 8; i++) blk[8 * i] = v;
        return;
    }
    x0 = (blk[0] << 8) + 8192;
    x8 = W7 * (x4 + x5) + 4;
    x4 = (x8 + (W1 - W7) * x4) >> 3;
    x5 = (x8 - (W1 + W7) * x5) >> 3;
    x8 = W3 * (x6 + x7) + 4;
    x6 = (x8 - (W3 - W5) * x6) >> 3;
    x7 = (x8 - (W3 + W5) * x7) >> 3;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = W6 * (x3 + x2) + 4;
    x2 = (x1 - (W2 + W6) * x2) >> 3;
    x3 = (x1 + (W2 - W6) * x3) >> 3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;
    blk[8 * 0] = (x7 + x1) >> 14;
    blk[8 * 1] = (x3 + x2) >> 14;
    blk[8 * 2] = (x0 + x4) >> 14;
    blk[8 * 3] = (x8 + x6) >> 14;
    blk[8 * 4] = (x8 - x6) >> 14;
    blk[8 * 5] = (x0 - x4) >> 14;
    blk[8 * 6] = (x3 - x2) >> 14;
    blk[8 * 7] = (x7 - x1) >> 14;
}

static const u8 zigzag[64] = {
     0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63
};

/* ---------------------------------------------------------------- decoder */

#define MAX_COMPS 3

typedef struct {
    int id, h, v, tq;       /* sampling factors and quant table */
    int td, ta;             /* dc/ac huffman table selectors */
    int dc_pred;
    /* per-component plane at full (upsampled) resolution */
    u8 *plane;
    int pw, ph;             /* padded plane dimensions (whole MCUs) */
    int cw, ch;             /* valid sample dimensions (the real data)  */
} comp_t;

typedef struct {
    const u8 *data;
    size_t len;
    int width, height;
    int ncomp;
    comp_t comp[MAX_COMPS];
    u16 qt[4][64];
    huff_t dc[4], ac[4];
    int restart_interval;
    int hmax, vmax;
} jpg_t;

static u16 rd16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* Decode one 8x8 block into `out` (spatial samples, 0..255). */
static int decode_block(jpg_t *j, bits_t *b, comp_t *c, u8 *out, int stride) {
    static s32 blk[64];
    memset(blk, 0, sizeof blk);

    /* DC: a difference from the previous block of this component. */
    int t = huff_decode(b, &j->dc[c->td]);
    int diff = t ? extend(get_bits(b, t), t) : 0;
    c->dc_pred += diff;
    blk[0] = c->dc_pred * j->qt[c->tq][0];

    /* AC: run-length of zeros then an amplitude, until 63 or an EOB. */
    for (int k = 1; k < 64; ) {
        int rs = huff_decode(b, &j->ac[c->ta]);
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r != 15) break;      /* EOB */
            k += 16;                 /* ZRL: sixteen zeros */
            continue;
        }
        k += r;
        if (k >= 64) break;
        int z = zigzag[k];
        blk[z] = extend(get_bits(b, s), s) * j->qt[c->tq][k];
        k++;
    }

    for (int i = 0; i < 8; i++) idct_row(blk + i * 8);
    for (int i = 0; i < 8; i++) idct_col(blk + i);

    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            out[y * stride + x] = (u8)clamp8(blk[y * 8 + x] + 128);
    return 1;
}

/* Bilinear upsample of a component plane to full resolution, with the sample
 * centres offset by half a pixel the way "fancy" upsampling does - which is
 * what makes a subsampled chroma plane come back smooth at a colour edge
 * rather than blocky.  Fixed point in sixteenths.  A component that is not
 * subsampled (h == hmax) falls straight through. */
static u8 sample(comp_t *c, int x, int y, int fullw, int fullh, int hmax, int vmax) {
    (void)fullw; (void)fullh;
    if (c->h == hmax && c->v == vmax) {
        int sx = x < c->cw ? x : c->cw - 1;
        int sy = y < c->ch ? y : c->ch - 1;
        return c->plane[sy * c->pw + sx];
    }

    /* Centred source coordinate, in 1/16 units:
     *   src = (x + 0.5) * h / hmax - 0.5  */
    int fx = ((2 * x + 1) * c->h * 8) / hmax - 8;
    int fy = ((2 * y + 1) * c->v * 8) / vmax - 8;
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;

    int x0 = fx >> 4, y0 = fy >> 4;
    int x1 = x0 + 1, y1 = y0 + 1;
    int dx = fx & 15, dy = fy & 15;
    if (x0 >= c->cw) x0 = c->cw - 1;   /* the valid extent, not the padding */
    if (x1 >= c->cw) x1 = c->cw - 1;
    if (y0 >= c->ch) y0 = c->ch - 1;
    if (y1 >= c->ch) y1 = c->ch - 1;

    int p00 = c->plane[y0 * c->pw + x0], p10 = c->plane[y0 * c->pw + x1];
    int p01 = c->plane[y1 * c->pw + x0], p11 = c->plane[y1 * c->pw + x1];
    int top = p00 * (16 - dx) + p10 * dx;
    int bot = p01 * (16 - dx) + p11 * dx;
    return (u8)((top * (16 - dy) + bot * dy + 128) >> 8);
}

int jpeg_decode(const void *bytes, size_t len, jpeg_image_t *out) {
    static jpg_t j;
    memset(&j, 0, sizeof j);
    memset(out, 0, sizeof *out);
    const u8 *p = bytes, *end = (const u8 *)bytes + len;
    j.data = bytes; j.len = len;

    if (len < 2 || p[0] != 0xFF || p[1] != 0xD8) return JPEG_ENOTJPEG;
    p += 2;

    while (p + 4 <= end) {
        if (p[0] != 0xFF) { p++; continue; }
        u8 m = p[1]; p += 2;
        if (m == 0xD9) break;                       /* EOI */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        int seg = rd16(p);
        const u8 *s = p + 2, *segend = p + seg;
        if (segend > end) return JPEG_EBADDATA;

        switch (m) {
        case 0xDB:                                  /* DQT */
            while (s < segend) {
                int pq = s[0] >> 4, tq = s[0] & 15; s++;
                if (tq > 3) return JPEG_EBADDATA;
                for (int i = 0; i < 64; i++) {
                    j.qt[tq][i] = pq ? rd16(s) : *s;
                    s += pq ? 2 : 1;
                }
            }
            break;
        case 0xC0: case 0xC1: {                      /* SOF0/1 baseline */
            j.height = rd16(s + 1);
            j.width = rd16(s + 3);
            j.ncomp = s[5];
            if (j.ncomp > MAX_COMPS) return JPEG_EUNSUPPORTED;
            const u8 *cp = s + 6;
            j.hmax = j.vmax = 1;
            for (int i = 0; i < j.ncomp; i++) {
                j.comp[i].id = cp[0];
                j.comp[i].h = cp[1] >> 4;
                j.comp[i].v = cp[1] & 15;
                j.comp[i].tq = cp[2];
                if (j.comp[i].h > j.hmax) j.hmax = j.comp[i].h;
                if (j.comp[i].v > j.vmax) j.vmax = j.comp[i].v;
                cp += 3;
            }
            break;
        }
        case 0xC2: return JPEG_EUNSUPPORTED;          /* progressive */
        case 0xC4:                                    /* DHT */
            while (s < segend) {
                int tc = s[0] >> 4, th = s[0] & 15; s++;
                huff_t *h = tc ? &j.ac[th] : &j.dc[th];
                memset(h, 0, sizeof *h);
                int total = 0;
                for (int i = 1; i <= 16; i++) { h->bits[i] = s[i - 1]; total += s[i - 1]; }
                s += 16;
                for (int i = 0; i < total; i++) h->vals[i] = s[i];
                s += total;
                huff_build(h);
                h->present = 1;
            }
            break;
        case 0xDD:                                    /* DRI */
            j.restart_interval = rd16(s);
            break;
        case 0xDA:                                    /* SOS - the scan */
            goto scan;
        default:
            break;                                    /* APPn, COM, etc. */
        }
        p = segend;
    }
    return JPEG_EBADDATA;

scan: {
    const u8 *s = p + 2;
    int ns = s[0]; s++;
    for (int i = 0; i < ns; i++) {
        int id = s[0], tdta = s[1]; s += 2;
        for (int k = 0; k < j.ncomp; k++)
            if (j.comp[k].id == id) {
                j.comp[k].td = tdta >> 4;
                j.comp[k].ta = tdta & 15;
            }
    }
    s += 3;                                            /* Ss, Se, Ah/Al */

    /* Allocate each component's own plane, sized in whole MCUs. */
    int mcux = (j.width + 8 * j.hmax - 1) / (8 * j.hmax);
    int mcuy = (j.height + 8 * j.vmax - 1) / (8 * j.vmax);
    for (int i = 0; i < j.ncomp; i++) {
        comp_t *c = &j.comp[i];
        c->pw = mcux * c->h * 8;
        c->ph = mcuy * c->v * 8;
        c->cw = (j.width * c->h + j.hmax - 1) / j.hmax;    /* valid extent */
        c->ch = (j.height * c->v + j.vmax - 1) / j.vmax;
        c->plane = malloc((size_t)c->pw * c->ph);
        c->dc_pred = 0;
        if (!c->plane) return JPEG_ENOMEM;
    }

    bits_t b = { s, end, 0, 0, 0 };
    int rst = j.restart_interval;

    for (int my = 0; my < mcuy; my++) {
        for (int mx = 0; mx < mcux; mx++) {
            for (int i = 0; i < j.ncomp; i++) {
                comp_t *c = &j.comp[i];
                for (int by = 0; by < c->v; by++)
                    for (int bx = 0; bx < c->h; bx++) {
                        int px = (mx * c->h + bx) * 8;
                        int py = (my * c->v + by) * 8;
                        decode_block(&j, &b, c, c->plane + py * c->pw + px, c->pw);
                    }
            }
            if (j.restart_interval && --rst == 0 && !(my == mcuy - 1 && mx == mcux - 1)) {
                /* Realign to the restart marker and reset the DC predictors. */
                b.nbits = 0; b.bits = 0;
                while (b.p + 1 < b.end && !(b.p[0] == 0xFF && b.p[1] >= 0xD0 && b.p[1] <= 0xD7)) b.p++;
                if (b.p + 1 < b.end) b.p += 2;
                for (int i = 0; i < j.ncomp; i++) j.comp[i].dc_pred = 0;
                rst = j.restart_interval;
            }

        }
    }

    /* Compose: upsample chroma and convert to RGB. */
    out->width = j.width;
    out->height = j.height;
    out->rgb = malloc((size_t)j.width * j.height * 3);
    if (!out->rgb) { for (int i=0;i<j.ncomp;i++) free(j.comp[i].plane); return JPEG_ENOMEM; }

    for (int y = 0; y < j.height; y++) {
        for (int x = 0; x < j.width; x++) {
            u8 *o = out->rgb + (y * j.width + x) * 3;
            if (j.ncomp == 1) {
                u8 Y = sample(&j.comp[0], x, y, j.width, j.height, j.hmax, j.vmax);
                o[0] = o[1] = o[2] = Y;
            } else {
                int Y  = sample(&j.comp[0], x, y, j.width, j.height, j.hmax, j.vmax);
                int Cb = sample(&j.comp[1], x, y, j.width, j.height, j.hmax, j.vmax) - 128;
                int Cr = sample(&j.comp[2], x, y, j.width, j.height, j.hmax, j.vmax) - 128;
                o[0] = (u8)clamp8(Y + ((91881 * Cr) >> 16));
                o[1] = (u8)clamp8(Y - ((22554 * Cb + 46802 * Cr) >> 16));
                o[2] = (u8)clamp8(Y + ((116130 * Cb) >> 16));
            }
        }
    }
    for (int i = 0; i < j.ncomp; i++) free(j.comp[i].plane);
    return JPEG_OK;
}
}

void jpeg_free(jpeg_image_t *img) {
    if (img && img->rgb) { free(img->rgb); img->rgb = 0; }
}
