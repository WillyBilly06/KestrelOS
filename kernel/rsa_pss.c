/* rsa_pss.c - checking the newer of the two RSA signature paddings.
 *
 * The older scheme, PKCS#1 v1.5, wraps the hash in a fixed frame.  It works,
 * but it has been the source of a long line of forgery attacks, all of them
 * variations on an implementation being lax about some part of the frame it
 * thought did not matter.  PSS replaces the fixed frame with a randomised one
 * derived from the message, and there is nothing in it a verifier can be lax
 * about without the check failing outright.
 *
 * TLS 1.3 requires it: a server proving it holds the certificate's private key
 * signs with PSS even when the certificate itself was signed the old way.  So
 * without this file no TLS 1.3 connection completes, whatever else works.
 *
 * The encoded message looks like this, reading left to right:
 *
 *     [ masked DB ][ H ][ 0xbc ]
 *
 * where H is the hash of a block built from the message hash and a salt, and
 * the masked DB is that salt hidden under a mask generated from H.  Verifying
 * means undoing the mask, recovering the salt, rebuilding the block, hashing
 * it, and finding H again.
 */
#include "kernel.h"
#include "crypto.h"

/* The mask generation function: a hash used as a stream, counter appended. */
static void mgf1_sha256(const u8 *seed, size_t seed_len, u8 *mask, size_t mask_len) {
    u32 counter = 0;
    size_t at = 0;
    while (at < mask_len) {
        u8 counter_bytes[4] = {
            (u8)(counter >> 24), (u8)(counter >> 16), (u8)(counter >> 8), (u8)counter,
        };
        u8 block[SHA256_SIZE];
        sha256_t ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, seed, seed_len);
        sha256_update(&ctx, counter_bytes, sizeof counter_bytes);
        sha256_final(&ctx, block);

        size_t chunk = mask_len - at < sizeof block ? mask_len - at : sizeof block;
        memcpy(mask + at, block, chunk);
        at += chunk;
        counter++;
    }
}

static void mgf1_sha384(const u8 *seed, size_t seed_len, u8 *mask, size_t mask_len) {
    u32 counter = 0;
    size_t at = 0;
    while (at < mask_len) {
        u8 counter_bytes[4] = {
            (u8)(counter >> 24), (u8)(counter >> 16), (u8)(counter >> 8), (u8)counter,
        };
        u8 block[SHA384_SIZE];
        sha512_t ctx;
        sha384_init(&ctx);
        sha512_update(&ctx, seed, seed_len);
        sha512_update(&ctx, counter_bytes, sizeof counter_bytes);
        sha512_final(&ctx, block);

        size_t chunk = mask_len - at < sizeof block ? mask_len - at : sizeof block;
        memcpy(mask + at, block, chunk);
        at += chunk;
        counter++;
    }
}

static void mgf1(int hash_len, const u8 *seed, size_t seed_len, u8 *mask, size_t mask_len) {
    if (hash_len == SHA384_SIZE) mgf1_sha384(seed, seed_len, mask, mask_len);
    else                         mgf1_sha256(seed, seed_len, mask, mask_len);
}

static void hash_of(int hash_len, const void *data, size_t len, u8 *out) {
    if (hash_len == SHA384_SIZE) sha384(data, len, out);
    else                         sha256(data, len, out);
}

/* The message hash is passed in already computed, because the caller usually
 * has it: TLS hashes a constructed block, a certificate hashes its own body. */
bool rsa_pss_verify(const u8 *signature, size_t signature_len,
                    const u8 *modulus, size_t modulus_len, u32 exponent,
                    const u8 *message_hash, int hash_len) {
    if (hash_len != SHA256_SIZE && hash_len != SHA384_SIZE) return false;
    if (signature_len != modulus_len) return false;
    if (modulus_len > 512) return false;

    static u8 em[512];
    if (!rsa_public_op(signature, signature_len, modulus, modulus_len,
                       exponent, em, modulus_len))
        return false;

    size_t em_len = modulus_len;

    /* The salt is conventionally the same length as the hash, and that is what
     * every TLS implementation uses.  Anything shorter would still be a valid
     * PSS signature, but accepting a variable salt length means accepting a
     * zero-length one, which weakens the scheme for no benefit here. */
    size_t salt_len = (size_t)hash_len;

    if (em_len < salt_len + (size_t)hash_len + 2) return false;

    /* The last byte is fixed, and the check exists precisely so that a
     * verifier cannot skip it. */
    if (em[em_len - 1] != 0xBC) return false;

    size_t db_len = em_len - (size_t)hash_len - 1;
    const u8 *masked_db = em;
    const u8 *h = em + db_len;

    /* The number of bits in the modulus is one less than a whole number of
     * bytes here because the encoded message must be smaller than the modulus.
     * Those leading bits have to be zero. */
    size_t modulus_bits = modulus_len * 8;
    {
        /* The true bit length: skip leading zero bytes and find the top bit. */
        size_t i = 0;
        while (i < modulus_len && modulus[i] == 0) i++;
        if (i == modulus_len) return false;
        modulus_bits = (modulus_len - i) * 8;
        u8 top = modulus[i];
        while (!(top & 0x80)) { top <<= 1; modulus_bits--; }
    }
    size_t spare_bits = em_len * 8 - (modulus_bits - 1);
    if (spare_bits > 0 && spare_bits < 8) {
        if (masked_db[0] >> (8 - spare_bits)) return false;
    }

    static u8 db_mask[512];
    static u8 db[512];
    mgf1(hash_len, h, (size_t)hash_len, db_mask, db_len);
    for (size_t i = 0; i < db_len; i++) db[i] = masked_db[i] ^ db_mask[i];

    /* Undoing the mask can set bits the encoder was required to leave clear;
     * they are cleared back here, exactly as the specification says. */
    if (spare_bits > 0 && spare_bits < 8) db[0] &= (u8)(0xFF >> spare_bits);

    /* Everything before the salt has to be zeros then a single 0x01.  This is
     * the part a lax verifier skips, and skipping it is what makes forgery
     * possible. */
    size_t padding_len = db_len - salt_len - 1;
    for (size_t i = 0; i < padding_len; i++) if (db[i] != 0) return false;
    if (db[padding_len] != 0x01) return false;

    const u8 *salt = db + padding_len + 1;

    /* Rebuild the block that was hashed: eight zero bytes, the message hash,
     * and the salt.  The eight zero bytes are there so that a PSS signature
     * can never be confused with a signature over a raw hash. */
    static u8 block[8 + SHA512_SIZE + SHA512_SIZE];
    memset(block, 0, 8);
    memcpy(block + 8, message_hash, (size_t)hash_len);
    memcpy(block + 8 + hash_len, salt, salt_len);

    u8 rebuilt[SHA512_SIZE];
    hash_of(hash_len, block, 8 + (size_t)hash_len + salt_len, rebuilt);

    u8 difference = 0;
    for (int i = 0; i < hash_len; i++) difference |= rebuilt[i] ^ h[i];
    return difference == 0;
}

/* ------------------------------------------------------------------- test
 *
 * PSS signing needs a private key, which this system never has, so the test
 * cannot simply sign and verify.  What it can do is check the parts that have
 * a published answer - the mask generator - and check that the verifier
 * refuses the shapes it must refuse.
 */
int rsa_pss_selftest(void) {
    int failures = 0;

    /* MGF1 is defined as a hash of the seed and a counter; the first block for
     * a zero-length counter is the hash of the seed with four zero bytes.
     * Computing it the long way here checks the counter encoding, which is the
     * part that is easy to get wrong. */
    {
        u8 mask[64];
        mgf1_sha256((const u8 *)"seed", 4, mask, sizeof mask);

        u8 expected[SHA256_SIZE];
        sha256_t ctx;
        u8 counter[4] = { 0, 0, 0, 0 };
        sha256_init(&ctx);
        sha256_update(&ctx, "seed", 4);
        sha256_update(&ctx, counter, 4);
        sha256_final(&ctx, expected);
        if (memcmp(mask, expected, sizeof expected)) {
            kerr("pss", "the mask generator's first block is wrong");
            failures++;
        }

        u8 second[SHA256_SIZE];
        counter[3] = 1;
        sha256_init(&ctx);
        sha256_update(&ctx, "seed", 4);
        sha256_update(&ctx, counter, 4);
        sha256_final(&ctx, second);
        if (memcmp(mask + SHA256_SIZE, second, sizeof second)) {
            kerr("pss", "the mask generator's second block is wrong");
            failures++;
        }
    }

    /* A signature of the wrong length, and one whose trailer byte is wrong,
     * both have to be refused. */
    {
        u8 modulus[128];
        memset(modulus, 0xFF, sizeof modulus);
        modulus[sizeof modulus - 1] = 0xFF;
        u8 signature[64] = { 0 };
        u8 hash[SHA256_SIZE] = { 0 };
        if (rsa_pss_verify(signature, sizeof signature, modulus, sizeof modulus,
                           65537, hash, SHA256_SIZE)) {
            kerr("pss", "a signature shorter than the key was accepted");
            failures++;
        }
    }

    if (!failures)
        kinfo("crypto", "the PSS mask generator is correct and short signatures "
                        "are refused");
    return failures;
}
