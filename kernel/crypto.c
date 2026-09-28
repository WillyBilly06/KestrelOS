/* crypto.c - AES, SHA-1, SHA-256, HMAC, PBKDF2 and CCM.
 *
 * Written from FIPS 197, FIPS 180-4, RFC 2104, RFC 3610 and RFC 8018, and
 * checked at start-up against the vectors those documents publish.
 *
 * This is not constant-time.  AES here uses a substitution table, so the time a
 * block takes depends a little on the key - which matters when an attacker can
 * measure it, and does not when the thing being protected is a Wi-Fi link whose
 * frames are already in the air.  Saying so is better than implying otherwise.
 */
#include "kernel.h"
#include "klog.h"
#include "crypto.h"
#include "tls.h"
#include "ecc.h"
#include "roots.h"

/* --------------------------------------------------------------------- AES */

static const u8 sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

/* The round constants, which are 2^i in the field the cipher works over. */
static const u8 rcon[11] = { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10,
                             0x20, 0x40, 0x80, 0x1B, 0x36 };

/* Multiply by two in GF(2^8) with the AES polynomial: a shift, and a
 * conditional exclusive-or when the top bit was set. */
static inline u8 xtime(u8 x) {
    return (u8)((x << 1) ^ ((x & 0x80) ? 0x1B : 0x00));
}

static inline u8 gmul(u8 a, u8 b) {
    u8 result = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) result ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return result;
}

static inline u32 word(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static inline u32 sub_word(u32 w) {
    return ((u32)sbox[(w >> 24) & 0xFF] << 24) |
           ((u32)sbox[(w >> 16) & 0xFF] << 16) |
           ((u32)sbox[(w >> 8) & 0xFF] << 8) |
           (u32)sbox[w & 0xFF];
}

static inline u32 rot_word(u32 w) { return (w << 8) | (w >> 24); }

void aes_setkey(aes_t *ctx, const u8 *key, int key_bits) {
    int nk = key_bits / 32;                 /* words in the key      */
    ctx->rounds = nk + 6;
    int total = 4 * (ctx->rounds + 1);      /* words in the schedule */

    for (int i = 0; i < nk; i++) ctx->round_key[i] = word(key + i * 4);

    for (int i = nk; i < total; i++) {
        u32 temp = ctx->round_key[i - 1];
        if (i % nk == 0)
            temp = sub_word(rot_word(temp)) ^ ((u32)rcon[i / nk] << 24);
        else if (nk > 6 && i % nk == 4)
            temp = sub_word(temp);
        ctx->round_key[i] = ctx->round_key[i - nk] ^ temp;
    }
}

static void add_round_key(u8 state[16], const u32 *round_key) {
    for (int c = 0; c < 4; c++) {
        u32 k = round_key[c];
        state[c * 4 + 0] ^= (u8)(k >> 24);
        state[c * 4 + 1] ^= (u8)(k >> 16);
        state[c * 4 + 2] ^= (u8)(k >> 8);
        state[c * 4 + 3] ^= (u8)k;
    }
}

void aes_encrypt_block(const aes_t *ctx, const u8 in[16], u8 out[16]) {
    u8 state[16];
    memcpy(state, in, 16);

    add_round_key(state, ctx->round_key);

    for (int round = 1; round <= ctx->rounds; round++) {
        for (int i = 0; i < 16; i++) state[i] = sbox[state[i]];

        /* ShiftRows.  The state is stored column by column, so row r is the
         * bytes at 4c+r, and shifting a row left by r means moving those. */
        u8 t;
        t = state[1];  state[1] = state[5];  state[5] = state[9];
        state[9] = state[13]; state[13] = t;

        t = state[2];  state[2] = state[10]; state[10] = t;
        t = state[6];  state[6] = state[14]; state[14] = t;

        t = state[15]; state[15] = state[11]; state[11] = state[7];
        state[7] = state[3]; state[3] = t;

        /* MixColumns, except on the last round. */
        if (round != ctx->rounds) {
            for (int c = 0; c < 4; c++) {
                u8 *col = state + c * 4;
                u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = (u8)(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
                col[1] = (u8)(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
                col[2] = (u8)(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
                col[3] = (u8)(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
            }
        }

        add_round_key(state, ctx->round_key + round * 4);
    }

    memcpy(out, state, 16);
}

/* ------------------------------------------------------------------ SHA-1 */

static inline u32 rotl32(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(sha1_t *ctx, const u8 *block) {
    u32 w[80];
    for (int i = 0; i < 16; i++) w[i] = word(block + i * 4);
    for (int i = 16; i < 80; i++)
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    u32 a = ctx->state[0], b = ctx->state[1], c = ctx->state[2];
    u32 d = ctx->state[3], e = ctx->state[4];

    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }

        u32 temp = rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = temp;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e;
}

void sha1_init(sha1_t *ctx) {
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE; ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->length = 0;
    ctx->buffered = 0;
}

void sha1_update(sha1_t *ctx, const void *data, size_t len) {
    const u8 *p = data;
    ctx->length += len;

    while (len) {
        int room = 64 - ctx->buffered;
        int take = (int)(len < (size_t)room ? len : (size_t)room);
        memcpy(ctx->buffer + ctx->buffered, p, (size_t)take);
        ctx->buffered += take;
        p += take;
        len -= (size_t)take;

        if (ctx->buffered == 64) {
            sha1_block(ctx, ctx->buffer);
            ctx->buffered = 0;
        }
    }
}

void sha1_final(sha1_t *ctx, u8 out[SHA1_SIZE]) {
    u64 bits = ctx->length * 8;

    /* A one bit, then zeroes, then the length - with a second block when the
     * length will not fit after the padding. */
    u8 pad = 0x80;
    sha1_update(ctx, &pad, 1);
    u8 zero = 0;
    while (ctx->buffered != 56) sha1_update(ctx, &zero, 1);

    u8 length[8];
    for (int i = 0; i < 8; i++) length[i] = (u8)(bits >> (56 - i * 8));
    sha1_update(ctx, length, 8);

    for (int i = 0; i < 5; i++) {
        out[i * 4 + 0] = (u8)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (u8)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (u8)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (u8)ctx->state[i];
    }
}

void sha1(const void *data, size_t len, u8 out[SHA1_SIZE]) {
    sha1_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, out);
}

/* ---------------------------------------------------------------- SHA-256 */

static const u32 sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static inline u32 rotr32(u32 v, int n) { return (v >> n) | (v << (32 - n)); }

static void sha256_block(sha256_t *ctx, const u8 *block) {
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = word(block + i * 4);
    for (int i = 16; i < 64; i++) {
        u32 s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        u32 s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    u32 a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    u32 e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        u32 s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 temp1 = h + s1 + ch + sha256_k[i] + w[i];
        u32 s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 temp2 = s0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(sha256_t *ctx) {
    static const u32 initial[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(ctx->state, initial, sizeof initial);
    ctx->length = 0;
    ctx->buffered = 0;
}

void sha256_update(sha256_t *ctx, const void *data, size_t len) {
    const u8 *p = data;
    ctx->length += len;

    while (len) {
        int room = 64 - ctx->buffered;
        int take = (int)(len < (size_t)room ? len : (size_t)room);
        memcpy(ctx->buffer + ctx->buffered, p, (size_t)take);
        ctx->buffered += take;
        p += take;
        len -= (size_t)take;

        if (ctx->buffered == 64) {
            sha256_block(ctx, ctx->buffer);
            ctx->buffered = 0;
        }
    }
}

void sha256_final(sha256_t *ctx, u8 out[SHA256_SIZE]) {
    u64 bits = ctx->length * 8;

    u8 pad = 0x80;
    sha256_update(ctx, &pad, 1);
    u8 zero = 0;
    while (ctx->buffered != 56) sha256_update(ctx, &zero, 1);

    u8 length[8];
    for (int i = 0; i < 8; i++) length[i] = (u8)(bits >> (56 - i * 8));
    sha256_update(ctx, length, 8);

    for (int i = 0; i < 8; i++) {
        out[i * 4 + 0] = (u8)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (u8)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (u8)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (u8)ctx->state[i];
    }
}

void sha256(const void *data, size_t len, u8 out[SHA256_SIZE]) {
    sha256_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

/* ------------------------------------------------------------------- HMAC */

/* The construction is the same for both hashes: the key exclusive-ored with
 * one constant and prepended to the message, hashed, then the same with a
 * second constant and prepended to that result. */

void hmac_sha1_vector(const u8 *key, size_t key_len, int count,
                      const u8 *const *data, const size_t *lens,
                      u8 out[SHA1_SIZE]) {
    u8 padded[64], inner_pad[64], outer_pad[64];

    /* A key longer than a block is replaced by its own hash. */
    if (key_len > 64) {
        sha1(key, key_len, padded);
        key_len = SHA1_SIZE;
        key = padded;
    }
    memset(inner_pad, 0x36, sizeof inner_pad);
    memset(outer_pad, 0x5C, sizeof outer_pad);
    for (size_t i = 0; i < key_len; i++) {
        inner_pad[i] ^= key[i];
        outer_pad[i] ^= key[i];
    }

    sha1_t ctx;
    u8 inner[SHA1_SIZE];
    sha1_init(&ctx);
    sha1_update(&ctx, inner_pad, sizeof inner_pad);
    for (int i = 0; i < count; i++) sha1_update(&ctx, data[i], lens[i]);
    sha1_final(&ctx, inner);

    sha1_init(&ctx);
    sha1_update(&ctx, outer_pad, sizeof outer_pad);
    sha1_update(&ctx, inner, sizeof inner);
    sha1_final(&ctx, out);
}

void hmac_sha1(const u8 *key, size_t key_len, const void *data, size_t len,
               u8 out[SHA1_SIZE]) {
    const u8 *p = data;
    hmac_sha1_vector(key, key_len, 1, &p, &len, out);
}

void hmac_sha256_vector(const u8 *key, size_t key_len, int count,
                        const u8 *const *data, const size_t *lens,
                        u8 out[SHA256_SIZE]) {
    u8 padded[64], inner_pad[64], outer_pad[64];

    if (key_len > 64) {
        sha256(key, key_len, padded);
        key_len = SHA256_SIZE;
        key = padded;
    }
    memset(inner_pad, 0x36, sizeof inner_pad);
    memset(outer_pad, 0x5C, sizeof outer_pad);
    for (size_t i = 0; i < key_len; i++) {
        inner_pad[i] ^= key[i];
        outer_pad[i] ^= key[i];
    }

    sha256_t ctx;
    u8 inner[SHA256_SIZE];
    sha256_init(&ctx);
    sha256_update(&ctx, inner_pad, sizeof inner_pad);
    for (int i = 0; i < count; i++) sha256_update(&ctx, data[i], lens[i]);
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, outer_pad, sizeof outer_pad);
    sha256_update(&ctx, inner, sizeof inner);
    sha256_final(&ctx, out);
}

void hmac_sha256(const u8 *key, size_t key_len, const void *data, size_t len,
                 u8 out[SHA256_SIZE]) {
    const u8 *p = data;
    hmac_sha256_vector(key, key_len, 1, &p, &len, out);
}

/* ----------------------------------------------------------------- PBKDF2 */

void pbkdf2_sha1(const char *passphrase, const u8 *salt, size_t salt_len,
                 int iterations, u8 *out, size_t out_len) {
    size_t pass_len = strlen(passphrase);
    u32 block = 1;

    while (out_len) {
        /* The first round hashes the salt with the block number appended. */
        u8 counter[4] = { (u8)(block >> 24), (u8)(block >> 16),
                          (u8)(block >> 8), (u8)block };
        const u8 *parts[2] = { salt, counter };
        size_t lens[2] = { salt_len, sizeof counter };

        u8 u[SHA1_SIZE], result[SHA1_SIZE];
        hmac_sha1_vector((const u8 *)passphrase, pass_len, 2, parts, lens, u);
        memcpy(result, u, SHA1_SIZE);

        /* Every round after that hashes the previous one, and the results are
         * combined - which is what makes the whole thing cost what it does. */
        for (int i = 1; i < iterations; i++) {
            hmac_sha1((const u8 *)passphrase, pass_len, u, SHA1_SIZE, u);
            for (int k = 0; k < SHA1_SIZE; k++) result[k] ^= u[k];
        }

        size_t take = out_len < SHA1_SIZE ? out_len : SHA1_SIZE;
        memcpy(out, result, take);
        out += take;
        out_len -= take;
        block++;
    }
}

/* -------------------------------------------------------------------- CCM */

/* CCM is counter mode for the encryption and CBC-MAC for the authentication,
 * over the same key.  The awkward part is the formatting: the first block
 * encodes the flags, the nonce and the length, and the additional data is
 * length-prefixed before being folded in. */

static void ccm_xor_block(u8 *dst, const u8 *src, int len) {
    for (int i = 0; i < len; i++) dst[i] ^= src[i];
}

static void ccm_start(const aes_t *aes, const u8 *nonce, int nonce_len,
                      const u8 *aad, int aad_len, int data_len, int mic_len,
                      u8 mac[16]) {
    int length_field = 15 - nonce_len;

    u8 block[16];
    memset(block, 0, sizeof block);
    block[0] = (u8)(((aad_len > 0) ? 0x40 : 0x00) |
                    (((mic_len - 2) / 2) << 3) |
                    (length_field - 1));
    memcpy(block + 1, nonce, (size_t)nonce_len);
    for (int i = 0; i < length_field; i++)
        block[15 - i] = (u8)(data_len >> (i * 8));

    aes_encrypt_block(aes, block, mac);

    if (aad_len > 0) {
        u8 buffer[16];
        memset(buffer, 0, sizeof buffer);
        int offset;

        /* Lengths below 2^16 - 2^8 use a two-byte prefix, which is every case
         * a Wi-Fi frame produces. */
        if (aad_len < 65280) {
            buffer[0] = (u8)(aad_len >> 8);
            buffer[1] = (u8)aad_len;
            offset = 2;
        } else {
            buffer[0] = 0xFF; buffer[1] = 0xFE;
            for (int i = 0; i < 4; i++) buffer[2 + i] = (u8)(aad_len >> (24 - i * 8));
            offset = 6;
        }

        int taken = 0;
        while (taken < aad_len) {
            int room = 16 - offset;
            int take = aad_len - taken;
            if (take > room) take = room;
            memcpy(buffer + offset, aad + taken, (size_t)take);
            taken += take;

            ccm_xor_block(mac, buffer, 16);
            aes_encrypt_block(aes, mac, mac);

            memset(buffer, 0, sizeof buffer);
            offset = 0;
        }
    }
}

/* The counter block for position i, which is also how the tag is masked. */
static void ccm_counter(const u8 *nonce, int nonce_len, u32 index, u8 out[16]) {
    int length_field = 15 - nonce_len;
    memset(out, 0, 16);
    out[0] = (u8)(length_field - 1);
    memcpy(out + 1, nonce, (size_t)nonce_len);
    for (int i = 0; i < length_field; i++)
        out[15 - i] = (u8)(index >> (i * 8));
}

static void ccm_run(const aes_t *aes, const u8 *nonce, int nonce_len,
                    const u8 *aad, int aad_len, u8 *data, int data_len,
                    int mic_len, u8 *mic_out, bool encrypting) {
    u8 mac[16];
    ccm_start(aes, nonce, nonce_len, aad, aad_len, data_len, mic_len, mac);

    u8 counter[16], keystream[16];

    /* When decrypting, the plaintext has to exist before it can be
     * authenticated, so the order of the two operations swaps. */
    for (int offset = 0; offset < data_len; offset += 16) {
        int len = data_len - offset;
        if (len > 16) len = 16;

        if (!encrypting) {
            ccm_counter(nonce, nonce_len, (u32)(offset / 16 + 1), counter);
            aes_encrypt_block(aes, counter, keystream);
            ccm_xor_block(data + offset, keystream, len);
        }

        u8 block[16];
        memset(block, 0, sizeof block);
        memcpy(block, data + offset, (size_t)len);
        ccm_xor_block(mac, block, 16);
        aes_encrypt_block(aes, mac, mac);

        if (encrypting) {
            ccm_counter(nonce, nonce_len, (u32)(offset / 16 + 1), counter);
            aes_encrypt_block(aes, counter, keystream);
            ccm_xor_block(data + offset, keystream, len);
        }
    }

    /* Counter zero masks the authentication tag. */
    ccm_counter(nonce, nonce_len, 0, counter);
    aes_encrypt_block(aes, counter, keystream);
    for (int i = 0; i < mic_len; i++) mic_out[i] = (u8)(mac[i] ^ keystream[i]);
}

bool aes_ccm_encrypt(const aes_t *aes, const u8 *nonce, int nonce_len,
                     const u8 *aad, int aad_len, u8 *data, int data_len,
                     u8 *mic, int mic_len) {
    if (nonce_len < 7 || nonce_len > 13) return false;
    if (mic_len < 4 || mic_len > 16 || (mic_len & 1)) return false;

    ccm_run(aes, nonce, nonce_len, aad, aad_len, data, data_len, mic_len,
            mic, true);
    return true;
}

bool aes_ccm_decrypt(const aes_t *aes, const u8 *nonce, int nonce_len,
                     const u8 *aad, int aad_len, u8 *data, int data_len,
                     const u8 *mic, int mic_len) {
    if (nonce_len < 7 || nonce_len > 13) return false;
    if (mic_len < 4 || mic_len > 16 || (mic_len & 1)) return false;

    u8 computed[16];
    ccm_run(aes, nonce, nonce_len, aad, aad_len, data, data_len, mic_len,
            computed, false);

    /* Compare without an early exit: how long the comparison takes should not
     * say how much of the tag was right. */
    u8 difference = 0;
    for (int i = 0; i < mic_len; i++) difference |= (u8)(computed[i] ^ mic[i]);
    return difference == 0;
}

/* ------------------------------------------------------------------- tests */

static bool same(const u8 *a, const u8 *b, int len) {
    return memcmp(a, b, (size_t)len) == 0;
}

static int check(const char *what, bool ok) {
    if (!ok) kerr("crypto", "%s does not match its published test vector", what);
    return ok ? 0 : 1;
}

int crypto_selftest(void) {
    int failures = 0;

    /* AES-128, from FIPS 197 appendix B. */
    {
        static const u8 key[16] = {
            0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
            0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c };
        static const u8 plain[16] = {
            0x32,0x43,0xf6,0xa8,0x88,0x5a,0x30,0x8d,
            0x31,0x31,0x98,0xa2,0xe0,0x37,0x07,0x34 };
        static const u8 expected[16] = {
            0x39,0x25,0x84,0x1d,0x02,0xdc,0x09,0xfb,
            0xdc,0x11,0x85,0x97,0x19,0x6a,0x0b,0x32 };

        aes_t ctx;
        u8 out[16];
        aes_setkey(&ctx, key, 128);
        aes_encrypt_block(&ctx, plain, out);
        failures += check("AES-128", same(out, expected, 16));
    }

    /* AES-256, from FIPS 197 appendix C.3. */
    {
        static const u8 key[32] = {
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
            0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
            0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
            0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f };
        static const u8 plain[16] = {
            0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
            0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
        static const u8 expected[16] = {
            0x8e,0xa2,0xb7,0xca,0x51,0x67,0x45,0xbf,
            0xea,0xfc,0x49,0x90,0x4b,0x49,0x60,0x89 };

        aes_t ctx;
        u8 out[16];
        aes_setkey(&ctx, key, 256);
        aes_encrypt_block(&ctx, plain, out);
        failures += check("AES-256", same(out, expected, 16));
    }

    /* SHA-1 of "abc", from FIPS 180-4. */
    {
        static const u8 expected[SHA1_SIZE] = {
            0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
            0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d };
        u8 out[SHA1_SIZE];
        sha1("abc", 3, out);
        failures += check("SHA-1", same(out, expected, SHA1_SIZE));
    }

    /* SHA-256 of "abc", from FIPS 180-4. */
    {
        static const u8 expected[SHA256_SIZE] = {
            0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
            0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
            0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
            0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
        u8 out[SHA256_SIZE];
        sha256("abc", 3, out);
        failures += check("SHA-256", same(out, expected, SHA256_SIZE));
    }

    /* HMAC-SHA1, RFC 2202 test case 1. */
    {
        static const u8 key[20] = {
            0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
            0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b };
        static const u8 expected[SHA1_SIZE] = {
            0xb6,0x17,0x31,0x86,0x55,0x05,0x72,0x64,0xe2,0x8b,
            0xc0,0xb6,0xfb,0x37,0x8c,0x8e,0xf1,0x46,0xbe,0x00 };
        u8 out[SHA1_SIZE];
        hmac_sha1(key, sizeof key, "Hi There", 8, out);
        failures += check("HMAC-SHA1", same(out, expected, SHA1_SIZE));
    }

    /* HMAC-SHA256, RFC 4231 test case 1. */
    {
        static const u8 key[20] = {
            0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
            0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b };
        static const u8 expected[SHA256_SIZE] = {
            0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,
            0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
            0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,
            0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7 };
        u8 out[SHA256_SIZE];
        hmac_sha256(key, sizeof key, "Hi There", 8, out);
        failures += check("HMAC-SHA256", same(out, expected, SHA256_SIZE));
    }

    /* PBKDF2-HMAC-SHA1, RFC 6070 test case 2 - the same construction WPA uses
     * to turn a passphrase into a key, at a smaller iteration count. */
    {
        static const u8 expected[20] = {
            0xea,0x6c,0x01,0x4d,0xc7,0x2d,0x6f,0x8c,0xcd,0x1e,
            0xd9,0x2a,0xce,0x1d,0x41,0xf0,0xd8,0xde,0x89,0x57 };
        u8 out[20];
        pbkdf2_sha1("password", (const u8 *)"salt", 4, 2, out, sizeof out);
        failures += check("PBKDF2-SHA1", same(out, expected, sizeof expected));
    }

    /* AES-CCM, RFC 3610 packet vector 1. */
    {
        static const u8 key[16] = {
            0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,
            0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf };
        static const u8 nonce[13] = {
            0x00,0x00,0x00,0x03,0x02,0x01,0x00,0xa0,
            0xa1,0xa2,0xa3,0xa4,0xa5 };
        static const u8 aad[8] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07 };
        static const u8 plain[23] = {
            0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
            0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
            0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e };
        static const u8 expected[23] = {
            0x58,0x8c,0x97,0x9a,0x61,0xc6,0x63,0xd2,
            0xf0,0x66,0xd0,0xc2,0xc0,0xf9,0x89,0x80,
            0x6d,0x5f,0x6b,0x61,0xda,0xc3,0x84 };
        static const u8 expected_mic[8] = {
            0x17,0xe8,0xd1,0x2c,0xfd,0xf9,0x26,0xe0 };

        aes_t ctx;
        aes_setkey(&ctx, key, 128);

        u8 data[23];
        u8 mic[8];
        memcpy(data, plain, sizeof plain);

        aes_ccm_encrypt(&ctx, nonce, sizeof nonce, aad, sizeof aad,
                        data, sizeof data, mic, sizeof mic);

        bool ok = same(data, expected, sizeof expected) &&
                  same(mic, expected_mic, sizeof expected_mic);
        failures += check("AES-CCM encrypt", ok);

        /* And the other way, which must give back exactly what went in. */
        bool authentic = aes_ccm_decrypt(&ctx, nonce, sizeof nonce, aad,
                                         sizeof aad, data, sizeof data,
                                         mic, sizeof mic);
        failures += check("AES-CCM decrypt",
                          authentic && same(data, plain, sizeof plain));

        /* A tampered tag must be rejected. */
        u8 bad_mic[8];
        memcpy(bad_mic, mic, sizeof bad_mic);
        bad_mic[0] ^= 0x01;
        aes_ccm_encrypt(&ctx, nonce, sizeof nonce, aad, sizeof aad,
                        data, sizeof data, mic, sizeof mic);
        failures += check("AES-CCM rejects a bad tag",
                          !aes_ccm_decrypt(&ctx, nonce, sizeof nonce, aad,
                                           sizeof aad, data, sizeof data,
                                           bad_mic, sizeof bad_mic));
    }

    /* The pieces a secure connection to a web site needs, each against its
     * own specification's vectors.  They live in their own files because they
     * are large, but they are checked here with everything else: a system that
     * started up with a broken curve implementation would make connections
     * that looked encrypted and were not. */
    failures += x25519_selftest();
    failures += gcm_selftest();
    failures += hkdf_selftest();
    failures += bignum_selftest();
    failures += random_selftest();
    failures += sha512_selftest();
    failures += rsa_pss_selftest();
    failures += tls_selftest();
    failures += ecc_selftest();
    failures += roots_selftest();
    failures += roots_check_selftest();

    if (failures)
        kerr("crypto", "%d self-test(s) failed; nothing that depends on "
                       "cryptography can be trusted", failures);
    else
        kinfo("crypto", "AES, SHA-1, SHA-256, HMAC, PBKDF2, CCM, GCM, x25519, "
                        "HKDF and RSA all match their published vectors");
    return failures;
}
