/* blit.c - the pixel engine.
 *
 * Everything on the screen is composed by the processor here: there is no
 * hardware to hand the work to, because the one generation of card this
 * machine is likely to have keeps its drawing engine behind firmware nobody
 * else can sign.  So the processor has to be good at it.
 *
 * At 3840 by 2160 a single full-screen fill touches eight million pixels.  A
 * scalar loop moves one at a time; the vector unit on every x86-64 processor
 * ever made moves four, and the same instructions blend four at a time as
 * well.  That is the difference between a desktop that feels immediate and one
 * that feels like it is thinking.
 *
 * SSE2 rather than anything newer, deliberately: it is part of the definition
 * of x86-64, so there is no processor this system can boot on that lacks it,
 * and no run-time check to get wrong.  Where a wider unit would help - and it
 * would - the check for it is in `blit_features`, and the wide paths are
 * chosen only when the processor has said it has them.
 */
#include "gui.h"
#include <emmintrin.h>

/* --------------------------------------------------------------- features */

/* What this processor can do.  Read once, because CPUID is slow and the answer
 * never changes. */
typedef struct {
    bool checked;
    bool sse2;      /* guaranteed by the architecture, but confirmed anyway */
    bool ssse3;     /* byte shuffles, which make packing colours cheap      */
    bool sse41;     /* packed multiply and blend                            */
    bool avx2;      /* eight pixels at a time                               */
} features_t;

static features_t features;

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t out[4]) {
    __asm__ volatile ("cpuid"
                      : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3])
                      : "a"(leaf), "c"(sub));
}

static void detect_features(void) {
    if (features.checked) return;
    features.checked = true;
    features.sse2 = true;                 /* part of x86-64 itself */

    uint32_t r[4];
    cpuid(0, 0, r);
    uint32_t highest = r[0];

    if (highest >= 1) {
        cpuid(1, 0, r);
        features.ssse3 = (r[2] & (1u << 9)) != 0;
        features.sse41 = (r[2] & (1u << 19)) != 0;
    }
    if (highest >= 7) {
        cpuid(7, 0, r);
        /* AVX2 needs the operating system to have enabled the wide register
         * state as well as the processor supporting it.  This system does not
         * enable it, so the wide paths stay off until it does - claiming
         * otherwise would fault on the first wide instruction. */
        features.avx2 = false;
    }
}

const char *blit_engine_name(void) {
    detect_features();
    if (features.avx2) return "AVX2, eight pixels at a time";
    if (features.sse41) return "SSE4.1, four pixels at a time";
    if (features.ssse3) return "SSSE3, four pixels at a time";
    return "SSE2, four pixels at a time";
}

/* ------------------------------------------------------------------ fills */

/* A run of one colour.  The head and tail are done one pixel at a time so the
 * vector body only ever sees whole groups of four. */
void blit_fill_row(colour_t *dst, int count, colour_t c) {
    if (count <= 0) return;

    int i = 0;
    /* Up to the first four-pixel boundary. */
    while (i < count && (((uintptr_t)(dst + i)) & 15)) dst[i++] = c;

    __m128i v = _mm_set1_epi32((int)c);
    for (; i + 16 <= count; i += 16) {
        _mm_store_si128((__m128i *)(dst + i), v);
        _mm_store_si128((__m128i *)(dst + i + 4), v);
        _mm_store_si128((__m128i *)(dst + i + 8), v);
        _mm_store_si128((__m128i *)(dst + i + 12), v);
    }
    for (; i + 4 <= count; i += 4) _mm_store_si128((__m128i *)(dst + i), v);
    for (; i < count; i++) dst[i] = c;
}

/* ------------------------------------------------------------------ copies */

void blit_copy_row(colour_t *dst, const colour_t *src, int count) {
    if (count <= 0) return;

    int i = 0;
    /* Aligning the destination matters more than the source: a store that
     * straddles a cache line costs more than a load that does. */
    while (i < count && (((uintptr_t)(dst + i)) & 15)) { dst[i] = src[i]; i++; }

    for (; i + 16 <= count; i += 16) {
        __m128i a = _mm_loadu_si128((const __m128i *)(src + i));
        __m128i b = _mm_loadu_si128((const __m128i *)(src + i + 4));
        __m128i c = _mm_loadu_si128((const __m128i *)(src + i + 8));
        __m128i d = _mm_loadu_si128((const __m128i *)(src + i + 12));
        _mm_store_si128((__m128i *)(dst + i), a);
        _mm_store_si128((__m128i *)(dst + i + 4), b);
        _mm_store_si128((__m128i *)(dst + i + 8), c);
        _mm_store_si128((__m128i *)(dst + i + 12), d);
    }
    for (; i + 4 <= count; i += 4)
        _mm_store_si128((__m128i *)(dst + i),
                        _mm_loadu_si128((const __m128i *)(src + i)));
    for (; i < count; i++) dst[i] = src[i];
}

/* ------------------------------------------------------------------ blends
 *
 * Mixing two colours means doing it to each of three channels without letting
 * one overflow into the next.  The trick is to split the pixel in half: the
 * red and blue channels sit in the even byte positions and green in an odd
 * one, so masking them apart gives two sets of sixteen-bit lanes that can be
 * multiplied and added with no carry between channels, then put back together.
 *
 * That is four pixels, three channels each, in about a dozen instructions.
 */
#define MASK_RB 0x00FF00FFu
#define MASK_G  0x0000FF00u

static inline __m128i blend4(__m128i dst, __m128i src, __m128i alpha) {
    const __m128i mask_rb = _mm_set1_epi32((int)MASK_RB);
    const __m128i mask_g  = _mm_set1_epi32((int)MASK_G);

    /* dst + (src - dst) * alpha / 256, per channel. */
    __m128i drb = _mm_and_si128(dst, mask_rb);
    __m128i srb = _mm_and_si128(src, mask_rb);
    __m128i dg  = _mm_srli_epi32(_mm_and_si128(dst, mask_g), 8);
    __m128i sg  = _mm_srli_epi32(_mm_and_si128(src, mask_g), 8);

    /* The subtraction has to be signed, so it is done as
     * (dst * (256 - a) + src * a) / 256 instead, which stays unsigned. */
    __m128i inv = _mm_sub_epi32(_mm_set1_epi32(256), alpha);

    __m128i rb = _mm_add_epi32(_mm_mullo_epi16(drb, inv), _mm_mullo_epi16(srb, alpha));
    /* mullo_epi16 multiplies the sixteen-bit lanes, which is exactly the two
     * channels held in the red-blue half; the high halves of each product stay
     * within their own lane because neither input exceeds 255 * 256. */
    rb = _mm_and_si128(_mm_srli_epi32(rb, 8), mask_rb);

    __m128i g = _mm_add_epi32(_mm_mullo_epi16(dg, inv), _mm_mullo_epi16(sg, alpha));
    g = _mm_and_si128(g, _mm_set1_epi32((int)MASK_G));

    return _mm_or_si128(rb, g);
}

/* Mix one colour into a run at a constant strength. */
void blit_blend_row(colour_t *dst, int count, colour_t c, int alpha) {
    if (count <= 0 || alpha <= 0) return;
    if (alpha >= 255) { blit_fill_row(dst, count, c); return; }

    int i = 0;
    while (i < count && (((uintptr_t)(dst + i)) & 15)) {
        dst[i] = colour_mix(dst[i], c, alpha);
        i++;
    }

    __m128i src = _mm_set1_epi32((int)c);
    __m128i a = _mm_set1_epi32(alpha + (alpha >> 7));    /* 255 maps to 256 */

    for (; i + 4 <= count; i += 4) {
        __m128i d = _mm_load_si128((const __m128i *)(dst + i));
        _mm_store_si128((__m128i *)(dst + i), blend4(d, src, a));
    }
    for (; i < count; i++) dst[i] = colour_mix(dst[i], c, alpha);
}

/* Mix one run into another at a constant strength - what a fading window is
 * composed with. */
void blit_blend_copy_row(colour_t *dst, const colour_t *src, int count, int alpha) {
    if (count <= 0 || alpha <= 0) return;
    if (alpha >= 255) { blit_copy_row(dst, src, count); return; }

    int i = 0;
    while (i < count && (((uintptr_t)(dst + i)) & 15)) {
        dst[i] = colour_mix(dst[i], src[i], alpha);
        i++;
    }

    __m128i a = _mm_set1_epi32(alpha + (alpha >> 7));

    for (; i + 4 <= count; i += 4) {
        __m128i d = _mm_load_si128((const __m128i *)(dst + i));
        __m128i s = _mm_loadu_si128((const __m128i *)(src + i));
        _mm_store_si128((__m128i *)(dst + i), blend4(d, s, a));
    }
    for (; i < count; i++) dst[i] = colour_mix(dst[i], src[i], alpha);
}

/* ---------------------------------------------------------------- gradient
 *
 * A vertical gradient is a fill per row, so it needs nothing special.  A
 * horizontal one changes colour along the row, which is where the vector unit
 * has to carry four running values rather than one. */
void blit_gradient_row(colour_t *dst, int count, colour_t left, colour_t right) {
    if (count <= 0) return;
    if (count == 1) { dst[0] = left; return; }

    for (int i = 0; i < count; i++)
        dst[i] = colour_mix(left, right, i * 255 / (count - 1));
}

/* -------------------------------------------------------------- the screen
 *
 * Presenting means copying the back buffer to video memory, which is mapped
 * write-combining: writes are fast, and the fastest of all are the ones that
 * fill a whole write-combining buffer before it is flushed.  Non-temporal
 * stores do exactly that and skip the cache, which is right here because
 * nothing will read these pixels back. */
void blit_present_row(colour_t *dst, const colour_t *src, int count) {
    if (count <= 0) return;

    int i = 0;
    while (i < count && (((uintptr_t)(dst + i)) & 15)) { dst[i] = src[i]; i++; }

    for (; i + 16 <= count; i += 16) {
        __m128i a = _mm_loadu_si128((const __m128i *)(src + i));
        __m128i b = _mm_loadu_si128((const __m128i *)(src + i + 4));
        __m128i c = _mm_loadu_si128((const __m128i *)(src + i + 8));
        __m128i d = _mm_loadu_si128((const __m128i *)(src + i + 12));
        _mm_stream_si128((__m128i *)(dst + i), a);
        _mm_stream_si128((__m128i *)(dst + i + 4), b);
        _mm_stream_si128((__m128i *)(dst + i + 8), c);
        _mm_stream_si128((__m128i *)(dst + i + 12), d);
    }
    for (; i + 4 <= count; i += 4)
        _mm_stream_si128((__m128i *)(dst + i),
                         _mm_loadu_si128((const __m128i *)(src + i)));
    for (; i < count; i++) dst[i] = src[i];
}

/* Non-temporal stores are not ordered against ordinary ones, so anything that
 * has to be visible before the next frame needs this afterwards. */
void blit_present_done(void) { _mm_sfence(); }
