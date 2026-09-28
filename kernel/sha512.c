/* sha512.c - SHA-512 and SHA-384.
 *
 * SHA-256 covers most of what a certificate chain uses, but not all of it: a
 * good number of intermediate and root authorities sign with SHA-384, and a
 * chain is only as checkable as its least common hash.  Without this, those
 * chains would have to be either rejected - closing off a large part of the
 * web - or waved through, which is worse.
 *
 * It is the same shape as SHA-256 with everything twice as wide: sixty-four
 * bit words, eighty rounds instead of sixty-four, a hundred and twenty-eight
 * byte block, and different rotation amounts.  SHA-384 is the identical
 * function with a different starting state, truncated to forty-eight bytes -
 * the different start is what stops one being a prefix of the other.
 */
#include "kernel.h"
#include "crypto.h"

static const u64 K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static inline u64 ror64(u64 x, int n) { return (x >> n) | (x << (64 - n)); }

static void sha512_block(sha512_t *ctx, const u8 *block) {
    u64 w[80];

    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int b = 0; b < 8; b++) w[i] = (w[i] << 8) | block[i * 8 + b];
    }
    for (int i = 16; i < 80; i++) {
        u64 s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        u64 s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    u64 a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    u64 e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 80; i++) {
        u64 s1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
        u64 choose = (e & f) ^ (~e & g);
        u64 t1 = h + s1 + choose + K[i] + w[i];
        u64 s0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
        u64 majority = (a & b) ^ (a & c) ^ (b & c);
        u64 t2 = s0 + majority;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha512_init(sha512_t *ctx) {
    static const u64 start[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
    };
    memcpy(ctx->state, start, sizeof start);
    ctx->length = 0;
    ctx->buffered = 0;
    ctx->out_len = SHA512_SIZE;
}

void sha384_init(sha512_t *ctx) {
    /* A different starting state, so that a SHA-384 digest is not simply the
     * first forty-eight bytes of the SHA-512 one. */
    static const u64 start[8] = {
        0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
        0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL,
    };
    memcpy(ctx->state, start, sizeof start);
    ctx->length = 0;
    ctx->buffered = 0;
    ctx->out_len = SHA384_SIZE;
}

void sha512_update(sha512_t *ctx, const void *data, size_t len) {
    const u8 *bytes = data;
    ctx->length += len;

    while (len) {
        size_t room = 128 - ctx->buffered;
        size_t take = len < room ? len : room;
        memcpy(ctx->buffer + ctx->buffered, bytes, take);
        ctx->buffered += take;
        bytes += take;
        len -= take;
        if (ctx->buffered == 128) {
            sha512_block(ctx, ctx->buffer);
            ctx->buffered = 0;
        }
    }
}

void sha512_final(sha512_t *ctx, u8 *out) {
    /* The length goes in as a hundred and twenty-eight bit big-endian count of
     * bits.  Nothing here will ever hash more than a few gigabytes, so the top
     * sixty-four bits are always zero - but they still have to be there. */
    u64 bits = ctx->length * 8;

    u8 pad = 0x80;
    sha512_update(ctx, &pad, 1);
    pad = 0;
    while (ctx->buffered != 112) sha512_update(ctx, &pad, 1);

    u8 tail[16];
    memset(tail, 0, 8);
    for (int i = 0; i < 8; i++) tail[8 + i] = (u8)(bits >> (56 - 8 * i));
    sha512_update(ctx, tail, sizeof tail);

    for (int i = 0; i < 8; i++)
        for (int b = 0; b < 8; b++)
            if ((size_t)(i * 8 + b) < ctx->out_len)
                out[i * 8 + b] = (u8)(ctx->state[i] >> (56 - 8 * b));
}

void sha512(const void *data, size_t len, u8 out[SHA512_SIZE]) {
    sha512_t ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, data, len);
    sha512_final(&ctx, out);
}

void sha384(const void *data, size_t len, u8 out[SHA384_SIZE]) {
    sha512_t ctx;
    sha384_init(&ctx);
    sha512_update(&ctx, data, len);
    sha512_final(&ctx, out);
}

/* ------------------------------------------------------------------- test */

int sha512_selftest(void) {
    int failures = 0;

    /* The published digest of "abc", the standard first case. */
    {
        static const u8 want[SHA512_SIZE] = {
            0xdd,0xaf,0x35,0xa1,0x93,0x61,0x7a,0xba,0xcc,0x41,0x73,0x49,0xae,0x20,0x41,0x31,
            0x12,0xe6,0xfa,0x4e,0x89,0xa9,0x7e,0xa2,0x0a,0x9e,0xee,0xe6,0x4b,0x55,0xd3,0x9a,
            0x21,0x92,0x99,0x2a,0x27,0x4f,0xc1,0xa8,0x36,0xba,0x3c,0x23,0xa3,0xfe,0xeb,0xbd,
            0x45,0x4d,0x44,0x23,0x64,0x3c,0xe8,0x0e,0x2a,0x9a,0xc9,0x4f,0xa5,0x4c,0xa4,0x9f,
        };
        u8 got[SHA512_SIZE];
        sha512("abc", 3, got);
        if (memcmp(got, want, sizeof want)) {
            kerr("sha512", "the digest of \"abc\" does not match");
            failures++;
        }
    }

    /* And the SHA-384 one, which checks the different starting state as much
     * as it checks the compression function. */
    {
        static const u8 want[SHA384_SIZE] = {
            0xcb,0x00,0x75,0x3f,0x45,0xa3,0x5e,0x8b,0xb5,0xa0,0x3d,0x69,0x9a,0xc6,0x50,0x07,
            0x27,0x2c,0x32,0xab,0x0e,0xde,0xd1,0x63,0x1a,0x8b,0x60,0x5a,0x43,0xff,0x5b,0xed,
            0x80,0x86,0x07,0x2b,0xa1,0xe7,0xcc,0x23,0x58,0xba,0xec,0xa1,0x34,0xc8,0x25,0xa7,
        };
        u8 got[SHA384_SIZE];
        sha384("abc", 3, got);
        if (memcmp(got, want, sizeof want)) {
            kerr("sha384", "the digest of \"abc\" does not match");
            failures++;
        }
    }

    /* A message longer than one block, which exercises the padding path where
     * the length does not fit in the last block. */
    {
        static const u8 want[SHA512_SIZE] = {
            0x8e,0x95,0x9b,0x75,0xda,0xe3,0x13,0xda,0x8c,0xf4,0xf7,0x28,0x14,0xfc,0x14,0x3f,
            0x8f,0x77,0x79,0xc6,0xeb,0x9f,0x7f,0xa1,0x72,0x99,0xae,0xad,0xb6,0x88,0x90,0x18,
            0x50,0x1d,0x28,0x9e,0x49,0x00,0xf7,0xe4,0x33,0x1b,0x99,0xde,0xc4,0xb5,0x43,0x3a,
            0xc7,0xd3,0x29,0xee,0xb6,0xdd,0x26,0x54,0x5e,0x96,0xe5,0x5b,0x87,0x4b,0xe9,0x09,
        };
        const char *message = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                              "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
        u8 got[SHA512_SIZE];
        sha512(message, strlen(message), got);
        if (memcmp(got, want, sizeof want)) {
            kerr("sha512", "the digest of a two-block message does not match");
            failures++;
        }
    }

    if (!failures)
        kinfo("crypto", "SHA-512 and SHA-384 match their published vectors");
    return failures;
}
