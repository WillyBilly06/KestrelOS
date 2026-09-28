/* gcm.c - AES in Galois/Counter mode.
 *
 * The mode every TLS 1.3 connection uses.  It does two things at once: the
 * counter half encrypts, by encrypting a running counter and adding the result
 * to the message; the Galois half authenticates, by running the ciphertext
 * through a multiplication in a finite field whose result nobody without the
 * key can predict.
 *
 * Doing both together matters.  Encryption alone leaves a message an attacker
 * can alter in ways the receiver cannot detect - flipping a bit of ciphertext
 * flips the same bit of plaintext - and every protocol that tried encryption
 * without authentication has eventually been broken through exactly that.
 *
 * The field is GF(2^128): a 128-bit number is a polynomial, multiplication is
 * polynomial multiplication, and the result is reduced by a fixed polynomial.
 * Bit order is the thing to be careful about - the specification numbers bits
 * from the left, which is the opposite of how a byte is usually read.
 */
#include "kernel.h"
#include "crypto.h"

/* --------------------------------------------------------- the field
 *
 * Multiplication in GF(2^128), done a bit at a time.  A table would be faster
 * and would also make the timing depend on the data, which is how several
 * implementations of this have leaked their keys.  This one is slower and
 * says nothing.
 */
static void gf_mul(u8 out[16], const u8 x[16], const u8 y[16]) {
    u8 z[16] = { 0 };
    u8 v[16];
    memcpy(v, y, 16);

    for (int i = 0; i < 128; i++) {
        /* Bit i counted from the left of x. */
        if ((x[i / 8] >> (7 - (i % 8))) & 1)
            for (int k = 0; k < 16; k++) z[k] ^= v[k];

        /* v moves right by one; if a one falls off the end, the reduction
         * polynomial comes back in at the top. */
        bool carry = v[15] & 1;
        for (int k = 15; k > 0; k--) v[k] = (u8)((v[k] >> 1) | ((v[k - 1] & 1) << 7));
        v[0] >>= 1;
        if (carry) v[0] ^= 0xE1;
    }
    memcpy(out, z, 16);
}

/* The authentication tag is built by folding each block into a running value
 * and multiplying by the hash key. */
static void ghash_block(u8 state[16], const u8 key[16], const u8 block[16], int len) {
    u8 padded[16] = { 0 };
    memcpy(padded, block, (size_t)(len < 16 ? len : 16));
    for (int i = 0; i < 16; i++) state[i] ^= padded[i];
    gf_mul(state, state, key);
}

static void ghash(u8 out[16], const u8 key[16],
                  const u8 *aad, size_t aad_len,
                  const u8 *data, size_t data_len) {
    u8 state[16] = { 0 };

    for (size_t at = 0; at < aad_len; at += 16)
        ghash_block(state, key, aad + at, (int)(aad_len - at));
    for (size_t at = 0; at < data_len; at += 16)
        ghash_block(state, key, data + at, (int)(data_len - at));

    /* And finally the two lengths, in bits, as one block. */
    u8 lengths[16] = { 0 };
    u64 aad_bits = (u64)aad_len * 8, data_bits = (u64)data_len * 8;
    for (int i = 0; i < 8; i++) {
        lengths[7 - i] = (u8)(aad_bits >> (8 * i));
        lengths[15 - i] = (u8)(data_bits >> (8 * i));
    }
    ghash_block(state, key, lengths, 16);

    memcpy(out, state, 16);
}

/* ------------------------------------------------------------- the counter */

static void increment(u8 counter[16]) {
    /* Only the last four bytes count, which is what the specification says and
     * also what limits a single key to about sixty-four gigabytes. */
    for (int i = 15; i >= 12; i--)
        if (++counter[i]) break;
}

static void counter_xor(const aes_t *aes, u8 counter[16],
                        const u8 *in, u8 *out, size_t len) {
    u8 stream[16];
    for (size_t at = 0; at < len; at += 16) {
        increment(counter);
        aes_encrypt_block(aes, counter, stream);

        size_t chunk = len - at < 16 ? len - at : 16;
        for (size_t i = 0; i < chunk; i++) out[at + i] = in[at + i] ^ stream[i];
    }
}

/* --------------------------------------------------------------- the whole */

/* The nonce TLS uses is always twelve bytes, which is the case the
 * specification makes simplest: the counter block is the nonce followed by a
 * one. */
static void build_counter(u8 counter[16], const u8 nonce[12]) {
    memcpy(counter, nonce, 12);
    counter[12] = 0;
    counter[13] = 0;
    counter[14] = 0;
    counter[15] = 1;
}

void aes_gcm_encrypt(const aes_t *aes, const u8 nonce[12],
                     const u8 *aad, size_t aad_len,
                     const u8 *plain, size_t len,
                     u8 *cipher, u8 tag[16]) {
    /* The hash key is what the block cipher makes of a block of zeros.  In and
     * out are separate buffers deliberately: a block cipher is entitled to
     * write its output as it goes, and handing it the same buffer for both
     * would have it reading bytes it had already replaced. */
    static const u8 zeros[16] = { 0 };
    u8 hash_key[16];
    aes_encrypt_block(aes, zeros, hash_key);

    u8 counter[16];
    build_counter(counter, nonce);

    /* The first counter block encrypts the tag rather than the message, so it
     * is taken before the message uses the counter. */
    u8 tag_mask[16];
    aes_encrypt_block(aes, counter, tag_mask);

    counter_xor(aes, counter, plain, cipher, len);

    ghash(tag, hash_key, aad, aad_len, cipher, len);
    for (int i = 0; i < 16; i++) tag[i] ^= tag_mask[i];
}

bool aes_gcm_decrypt(const aes_t *aes, const u8 nonce[12],
                     const u8 *aad, size_t aad_len,
                     const u8 *cipher, size_t len,
                     const u8 tag[16], u8 *plain) {
    static const u8 zeros[16] = { 0 };
    u8 hash_key[16];
    aes_encrypt_block(aes, zeros, hash_key);

    u8 counter[16];
    build_counter(counter, nonce);

    u8 tag_mask[16];
    aes_encrypt_block(aes, counter, tag_mask);

    /* The tag is checked before a single byte is decrypted.  Decrypting first
     * and checking afterwards means acting on data that has not been
     * authenticated, which is the mistake that has broken more protocols than
     * any weakness in a cipher. */
    u8 expected[16];
    ghash(expected, hash_key, aad, aad_len, cipher, len);
    for (int i = 0; i < 16; i++) expected[i] ^= tag_mask[i];

    /* Compared in constant time: a comparison that stops at the first
     * difference tells an attacker how much of a forged tag was right, and
     * that is enough to find the rest one byte at a time. */
    u8 difference = 0;
    for (int i = 0; i < 16; i++) difference |= expected[i] ^ tag[i];
    if (difference) return false;

    counter_xor(aes, counter, cipher, plain, len);
    return true;
}

/* ------------------------------------------------------------------- test */

int gcm_selftest(void) {
    int failures = 0;

    /* The vectors from the specification's own test appendix.  Cases 3 and 4
     * share a key and a nonce and differ in exactly two ways - case 4 is four
     * bytes shorter and adds twenty bytes that are authenticated but not
     * encrypted - which makes them a good pair: the first checks the ordinary
     * path and the second checks that both the length and the additional data
     * really do reach the tag.
     *
     * They are also easy to mix up, and mixing them up produces a test that
     * fails while the code is right.  The lengths are stated here rather than
     * left implicit for that reason. */
    static const u8 key[16] = {
        0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08,
    };
    static const u8 nonce[12] = {
        0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88,
    };

    /* Case 3: sixty-four bytes, no additional data. */
    static const u8 plain3[64] = {
        0xd9,0x31,0x32,0x25,0xf8,0x84,0x06,0xe5,0xa5,0x59,0x09,0xc5,0xaf,0xf5,0x26,0x9a,
        0x86,0xa7,0xa9,0x53,0x15,0x34,0xf7,0xda,0x2e,0x4c,0x30,0x3d,0x8a,0x31,0x8a,0x72,
        0x1c,0x3c,0x0c,0x95,0x95,0x68,0x09,0x53,0x2f,0xcf,0x0e,0x24,0x49,0xa6,0xb5,0x25,
        0xb1,0x6a,0xed,0xf5,0xaa,0x0d,0xe6,0x57,0xba,0x63,0x7b,0x39,0x1a,0xaf,0xd2,0x55,
    };
    static const u8 cipher3[64] = {
        0x42,0x83,0x1e,0xc2,0x21,0x77,0x74,0x24,0x4b,0x72,0x21,0xb7,0x84,0xd0,0xd4,0x9c,
        0xe3,0xaa,0x21,0x2f,0x2c,0x02,0xa4,0xe0,0x35,0xc1,0x7e,0x23,0x29,0xac,0xa1,0x2e,
        0x21,0xd5,0x14,0xb2,0x54,0x66,0x93,0x1c,0x7d,0x8f,0x6a,0x5a,0xac,0x84,0xaa,0x05,
        0x1b,0xa3,0x0b,0x39,0x6a,0x0a,0xac,0x97,0x3d,0x58,0xe0,0x91,0x47,0x3f,0x59,0x85,
    };
    static const u8 tag3[16] = {
        0x4d,0x5c,0x2a,0xf3,0x27,0xcd,0x64,0xa6,0x2c,0xf3,0x5a,0xbd,0x2b,0xa6,0xfa,0xb4,
    };

    aes_t aes;
    aes_setkey(&aes, key, 128);

    u8 cipher[64], tag[16], recovered[64];

    aes_gcm_encrypt(&aes, nonce, NULL, 0, plain3, sizeof plain3, cipher, tag);
    if (memcmp(cipher, cipher3, sizeof cipher3)) {
        kerr("gcm", "the ciphertext does not match the published vector");
        failures++;
    }
    if (memcmp(tag, tag3, sizeof tag3)) {
        kerr("gcm", "the authentication tag does not match the published vector");
        failures++;
    }

    if (!aes_gcm_decrypt(&aes, nonce, NULL, 0, cipher, sizeof cipher3, tag, recovered)) {
        kerr("gcm", "a message this code encrypted failed its own check");
        failures++;
    } else if (memcmp(recovered, plain3, sizeof plain3)) {
        kerr("gcm", "the message did not survive the round trip");
        failures++;
    }

    /* A single altered bit has to be caught.  That is the entire point of the
     * mode, and an implementation that silently accepts it is worse than none:
     * it looks like it works. */
    u8 tampered[64];
    memcpy(tampered, cipher, sizeof tampered);
    tampered[7] ^= 0x01;
    if (aes_gcm_decrypt(&aes, nonce, NULL, 0, tampered, sizeof tampered, tag, recovered)) {
        kerr("gcm", "an altered message was accepted");
        failures++;
    }

    /* Case 4: sixty bytes - the same message four bytes shorter - with twenty
     * bytes of additional data.  Both differences change the tag, which is
     * what this checks. */
    static const u8 aad[20] = {
        0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,
        0xab,0xad,0xda,0xd2,
    };
    static const u8 tag4[16] = {
        0x5b,0xc9,0x4f,0xbc,0x32,0x21,0xa5,0xdb,0x94,0xfa,0xe9,0x5a,0xe7,0x12,0x1a,0x47,
    };

    aes_gcm_encrypt(&aes, nonce, aad, sizeof aad, plain3, 60, cipher, tag);
    if (memcmp(cipher, cipher3, 60)) {
        kerr("gcm", "the shorter message encrypted differently");
        failures++;
    }
    if (memcmp(tag, tag4, sizeof tag4)) {
        kerr("gcm", "the tag over additional data does not match the vector");
        failures++;
    }

    /* Additional data that has been altered has to be caught too, even though
     * it is not encrypted - that is what protects a record's header. */
    u8 bad_aad[20];
    memcpy(bad_aad, aad, sizeof bad_aad);
    bad_aad[0] ^= 0x01;
    if (aes_gcm_decrypt(&aes, nonce, bad_aad, sizeof bad_aad, cipher, 60, tag, recovered)) {
        kerr("gcm", "an altered header was accepted");
        failures++;
    }

    if (!failures)
        kinfo("crypto", "AES-GCM matches its published vectors, and refuses an "
                        "altered message and an altered header");
    return failures;
}
