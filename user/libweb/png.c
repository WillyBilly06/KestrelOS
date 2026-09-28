/* png.c - a PNG decoder, written for this system, with the DEFLATE decoder it
 * needs alongside it.
 *
 * Most pictures on the web are PNG, not JPEG - logos, icons, screenshots, line
 * art - so a browser that could show a JPEG (see jpeg.c) still showed a grey
 * frame for the majority of real images.  This reads them.
 *
 * Two pieces, both from their specifications rather than from behaviour:
 *
 *   - INFLATE (RFC 1951), because a PNG's pixels are a DEFLATE stream inside a
 *     zlib wrapper.  Fixed and dynamic Huffman blocks, and the length/distance
 *     back-references that are the whole point of the format.  Written once
 *     here; it is the same decompressor gzip and zlib are built on, so it is
 *     worth having beyond pictures.
 *
 *   - PNG itself (RFC 2083): the chunk stream (IHDR, PLTE, tRNS, IDAT, IEND),
 *     the five row filters undone in order, and the colour types unpacked to
 *     RGB - greyscale, truecolour, palette, and both with alpha, at eight bits.
 *
 * Verified on the host against PIL's decode of the same files - see
 * png_host_test.c - exactly, because unlike the JPEG path nothing here is
 * lossy: a correct inflate and a correct unfilter reproduce the bytes.
 */
#include "png.h"

#ifdef PNG_HOST_TEST
#include <string.h>
#include <stdlib.h>
#else
#include "kestrel.h"
#endif

typedef unsigned char  u8;
typedef unsigned int   u32;

/* =========================================================== INFLATE ====== */

typedef struct {
    const u8 *in, *in_end;
    u32 bitbuf;
    int bitcnt;
    u8 *out;
    u32 out_at, out_cap;
    int error;
} inflate_t;

static int get_bit(inflate_t *z) {
    if (z->bitcnt == 0) {
        if (z->in >= z->in_end) { z->error = 1; return 0; }
        z->bitbuf = *z->in++;
        z->bitcnt = 8;
    }
    int b = z->bitbuf & 1;
    z->bitbuf >>= 1;
    z->bitcnt--;
    return b;
}

static u32 get_bits(inflate_t *z, int n) {
    u32 v = 0;
    for (int i = 0; i < n; i++) v |= (u32)get_bit(z) << i;
    return v;
}

/* A canonical-Huffman table built from a list of code lengths, decoded the
 * way DEFLATE reads them: bit by bit, MSB first within a code. */
typedef struct {
    unsigned short counts[16];   /* how many codes of each length */
    unsigned short symbols[288]; /* symbols sorted by code        */
    int n;
} htree_t;

static void tree_build(htree_t *t, const u8 *lengths, int n) {
    for (int i = 0; i < 16; i++) t->counts[i] = 0;
    for (int i = 0; i < n; i++) t->counts[lengths[i]]++;
    t->counts[0] = 0;
    int offs[16]; offs[0] = 0;
    for (int i = 1; i < 16; i++) offs[i] = offs[i - 1] + t->counts[i - 1];
    for (int i = 0; i < n; i++)
        if (lengths[i]) t->symbols[offs[lengths[i]]++] = (unsigned short)i;
    t->n = n;
}

static int tree_decode(inflate_t *z, htree_t *t) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= get_bit(z);
        int count = t->counts[len];
        if (code - first < count) return t->symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    z->error = 1;
    return -1;
}

static void out_byte(inflate_t *z, u8 b) {
    if (z->out_at < z->out_cap) z->out[z->out_at] = b;
    z->out_at++;
}

/* The length and distance side tables DEFLATE fixes for codes 257..285. */
static const unsigned short len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const u8 len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const unsigned short dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const u8 dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static void inflate_block(inflate_t *z, htree_t *lit, htree_t *dist) {
    for (;;) {
        int sym = tree_decode(z, lit);
        if (z->error) return;
        if (sym == 256) return;                 /* end of block */
        if (sym < 256) { out_byte(z, (u8)sym); continue; }
        sym -= 257;
        if (sym >= 29) { z->error = 1; return; }
        int length = len_base[sym] + (int)get_bits(z, len_extra[sym]);
        int dsym = tree_decode(z, dist);
        if (z->error || dsym >= 30) { z->error = 1; return; }
        int distance = dist_base[dsym] + (int)get_bits(z, dist_extra[dsym]);
        if ((u32)distance > z->out_at) { z->error = 1; return; }
        u32 from = z->out_at - distance;
        for (int i = 0; i < length; i++)
            out_byte(z, (z->out ? z->out[(from + i) < z->out_cap ? from + i : 0] : 0));
    }
}

/* Inflate a raw DEFLATE stream into `out` (capacity out_cap).  Returns the
 * number of bytes produced, or -1 on error. */
static int inflate(const u8 *in, u32 in_len, u8 *out, u32 out_cap) {
    inflate_t z = { in, in + in_len, 0, 0, out, 0, out_cap, 0 };

    int final;
    do {
        final = get_bit(&z);
        int type = (int)get_bits(&z, 2);
        if (type == 0) {                        /* stored */
            z.bitbuf = 0; z.bitcnt = 0;         /* to a byte boundary */
            if (z.in + 4 > z.in_end) { z.error = 1; break; }
            u32 len = z.in[0] | (z.in[1] << 8);
            z.in += 4;
            for (u32 i = 0; i < len; i++) {
                if (z.in >= z.in_end) { z.error = 1; break; }
                out_byte(&z, *z.in++);
            }
        } else if (type == 1) {                 /* fixed Huffman */
            static u8 litlen[288], distlen[30];
            static int built = 0;
            static htree_t lit, dist;
            if (!built) {
                int i = 0;
                for (; i < 144; i++) litlen[i] = 8;
                for (; i < 256; i++) litlen[i] = 9;
                for (; i < 280; i++) litlen[i] = 7;
                for (; i < 288; i++) litlen[i] = 8;
                for (i = 0; i < 30; i++) distlen[i] = 5;
                tree_build(&lit, litlen, 288);
                tree_build(&dist, distlen, 30);
                built = 1;
            }
            inflate_block(&z, &lit, &dist);
        } else if (type == 2) {                 /* dynamic Huffman */
            int hlit = (int)get_bits(&z, 5) + 257;
            int hdist = (int)get_bits(&z, 5) + 1;
            int hclen = (int)get_bits(&z, 4) + 4;
            static const u8 ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            u8 cl_lengths[19];
            for (int i = 0; i < 19; i++) cl_lengths[i] = 0;
            for (int i = 0; i < hclen; i++) cl_lengths[ord[i]] = (u8)get_bits(&z, 3);
            htree_t cl; tree_build(&cl, cl_lengths, 19);

            u8 lengths[288 + 30];
            int n = 0, total = hlit + hdist;
            while (n < total) {
                int sym = tree_decode(&z, &cl);
                if (z.error) break;
                if (sym < 16) lengths[n++] = (u8)sym;
                else if (sym == 16) { int r = 3 + (int)get_bits(&z, 2); u8 p = n ? lengths[n-1] : 0; while (r-- && n < total) lengths[n++] = p; }
                else if (sym == 17) { int r = 3 + (int)get_bits(&z, 3); while (r-- && n < total) lengths[n++] = 0; }
                else               { int r = 11 + (int)get_bits(&z, 7); while (r-- && n < total) lengths[n++] = 0; }
            }
            htree_t lit, dist;
            tree_build(&lit, lengths, hlit);
            tree_build(&dist, lengths + hlit, hdist);
            inflate_block(&z, &lit, &dist);
        } else { z.error = 1; }
    } while (!final && !z.error);

    return z.error ? -1 : (int)z.out_at;
}

/* =============================================================== PNG ====== */

static u32 rd32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

/* Read channel `s` of pixel `x` from an unfiltered row: the raw sample value
 * at `depth` bits (0..2^depth-1 below 8 bits, the byte at 8, the high byte at
 * 16).  Sub-byte depths only occur with one channel (grey or palette). */
static int sample_raw(const u8 *row, int x, int chan, int s, int depth) {
    if (depth == 8)  return row[x * chan + s];
    if (depth == 16) return row[(x * chan + s) * 2];    /* high byte */
    int per = 8 / depth;
    int shift = (per - 1 - (x % per)) * depth;
    return (row[x / per] >> shift) & ((1 << depth) - 1);
}

/* Scale a `depth`-bit grey level to 0..255 (16-bit is already reduced to its
 * high byte by sample_raw, so it passes through). */
static int grey8(int v, int depth) {
    return (depth == 8 || depth == 16) ? v : v * 255 / ((1 << depth) - 1);
}

int png_decode(const void *bytes, size_t len, png_image_t *out) {
    const u8 *p = bytes, *end = (const u8 *)bytes + len;
    memset(out, 0, sizeof *out);
    static const u8 sig[8] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
    if (len < 8 || memcmp(p, sig, 8)) return PNG_ENOTPNG;
    p += 8;

    int width = 0, height = 0, bit_depth = 0, color = 0, interlace = 0;
    u8 palette[256 * 3];
    u8 palette_alpha[256];
    int have_pal = 0, pal_n = 0, have_trns = 0;
    for (int i = 0; i < 256; i++) palette_alpha[i] = 255;

    /* IDAT chunks are concatenated into one DEFLATE stream. */
    u8 *idat = NULL; u32 idat_len = 0, idat_cap = 0;

    while (p + 8 <= end) {
        u32 clen = rd32(p);
        const u8 *ctype = p + 4;
        const u8 *cdata = p + 8;
        if (cdata + clen + 4 > end) break;

        if (!memcmp(ctype, "IHDR", 4)) {
            width = (int)rd32(cdata);
            height = (int)rd32(cdata + 4);
            bit_depth = cdata[8];
            color = cdata[9];
            interlace = cdata[12];
            if (interlace) { free(idat); return PNG_EUNSUPPORTED; }  /* Adam7 not written */
            if (bit_depth != 1 && bit_depth != 2 && bit_depth != 4 &&
                bit_depth != 8 && bit_depth != 16) { free(idat); return PNG_EUNSUPPORTED; }
        } else if (!memcmp(ctype, "PLTE", 4)) {
            pal_n = clen / 3;
            if (pal_n > 256) pal_n = 256;
            memcpy(palette, cdata, (size_t)pal_n * 3);
            have_pal = 1;
        } else if (!memcmp(ctype, "tRNS", 4)) {
            if (color == 3) {
                for (u32 i = 0; i < clen && i < 256; i++) palette_alpha[i] = cdata[i];
                have_trns = 1;
            }
        } else if (!memcmp(ctype, "IDAT", 4)) {
            if (idat_len + clen > idat_cap) {
                idat_cap = (idat_len + clen) * 2 + 4096;
                u8 *ni = malloc(idat_cap);
                if (!ni) { free(idat); return PNG_ENOMEM; }
                if (idat) { memcpy(ni, idat, idat_len); free(idat); }
                idat = ni;
            }
            memcpy(idat + idat_len, cdata, clen);
            idat_len += clen;
        } else if (!memcmp(ctype, "IEND", 4)) {
            break;
        }
        p = cdata + clen + 4;      /* skip data + CRC */
    }

    if (!width || !height || !idat_len) { free(idat); return PNG_EBADDATA; }

    /* Channels per pixel by colour type. */
    int chan;
    switch (color) {
        case 0: chan = 1; break;   /* grey       */
        case 2: chan = 3; break;   /* truecolour */
        case 3: chan = 1; break;   /* palette    */
        case 4: chan = 2; break;   /* grey+alpha */
        case 6: chan = 4; break;   /* RGBA       */
        default: free(idat); return PNG_EUNSUPPORTED;
    }

    /* Raw scanlines: one filter byte then the packed samples of a row.  The
     * filter works on bytes, so its pixel stride is the pixel size rounded up
     * to a byte (one, for sub-byte depths); the row is the packed bits. */
    int bits_per_pixel = chan * bit_depth;
    int bpp = (bits_per_pixel + 7) / 8;
    int rowbytes = (width * bits_per_pixel + 7) / 8;
    u32 raw_cap = (u32)(rowbytes + 1) * height + 64;
    u8 *raw = malloc(raw_cap);
    if (!raw) { free(idat); return PNG_ENOMEM; }

    /* Skip the 2-byte zlib header, inflate the DEFLATE stream. */
    int got = (idat_len > 2) ? inflate(idat + 2, idat_len - 2, raw, raw_cap) : -1;
    free(idat);
    if (got < 0) { free(raw); return PNG_EBADDATA; }

    /* Undo the row filters, in place, producing unfiltered rows. */
    u8 *img = malloc((size_t)rowbytes * height);
    if (!img) { free(raw); return PNG_ENOMEM; }
    for (int y = 0; y < height; y++) {
        const u8 *src = raw + (size_t)y * (rowbytes + 1);
        int filter = src[0];
        u8 *row = img + (size_t)y * rowbytes;
        const u8 *prev = y ? img + (size_t)(y - 1) * rowbytes : NULL;
        for (int i = 0; i < rowbytes; i++) {
            int a = (i >= bpp) ? row[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int v = src[1 + i];
            switch (filter) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += paeth(a, b, c); break;
                default: free(raw); free(img); return PNG_EBADDATA;
            }
            row[i] = (u8)v;
        }
    }
    free(raw);

    /* Compose to RGB, and keep an alpha plane when the image carries one so a
     * caller (the icon renderer) can blend it per pixel.  Fully-opaque images
     * leave out->alpha NULL, which is what the browser expects. */
    out->width = width;
    out->height = height;
    out->rgb = malloc((size_t)width * height * 3);
    if (!out->rgb) { free(img); return PNG_ENOMEM; }
    out->alpha = NULL;
    if (color == 6 || color == 4 || (color == 3 && have_trns)) {
        out->alpha = malloc((size_t)width * height);
        if (!out->alpha) { free(out->rgb); out->rgb = NULL; free(img); return PNG_ENOMEM; }
    }

    for (int y = 0; y < height; y++) {
        const u8 *row = img + (size_t)y * rowbytes;
        u8 *o = out->rgb + (size_t)y * width * 3;
        u8 *a = out->alpha ? out->alpha + (size_t)y * width : NULL;
        for (int x = 0; x < width; x++, o += 3) {
            if (color == 2) {                   /* truecolour */
                o[0] = sample_raw(row, x, 3, 0, bit_depth);
                o[1] = sample_raw(row, x, 3, 1, bit_depth);
                o[2] = sample_raw(row, x, 3, 2, bit_depth);
            } else if (color == 6) {            /* RGBA */
                o[0] = sample_raw(row, x, 4, 0, bit_depth);
                o[1] = sample_raw(row, x, 4, 1, bit_depth);
                o[2] = sample_raw(row, x, 4, 2, bit_depth);
                if (a) a[x] = sample_raw(row, x, 4, 3, bit_depth);
            } else if (color == 0) {            /* grey */
                o[0] = o[1] = o[2] = grey8(sample_raw(row, x, 1, 0, bit_depth), bit_depth);
            } else if (color == 4) {            /* grey+alpha */
                o[0] = o[1] = o[2] = grey8(sample_raw(row, x, 2, 0, bit_depth), bit_depth);
                if (a) a[x] = sample_raw(row, x, 2, 1, bit_depth);
            } else if (color == 3) {            /* palette */
                int idx = sample_raw(row, x, 1, 0, bit_depth);
                if (!have_pal || idx >= 256) { o[0]=o[1]=o[2]=0; if (a) a[x]=255; }
                else {
                    o[0]=palette[idx*3]; o[1]=palette[idx*3+1]; o[2]=palette[idx*3+2];
                    if (a) a[x] = palette_alpha[idx];
                }
            }
        }
    }
    free(img);
    return PNG_OK;
}

void png_free(png_image_t *img) {
    if (img && img->rgb) { free(img->rgb); img->rgb = 0; }
    if (img && img->alpha) { free(img->alpha); img->alpha = 0; }
}
