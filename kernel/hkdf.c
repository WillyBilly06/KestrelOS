/* hkdf.c - turning one secret into all the others.
 *
 * A handshake produces a single shared secret.  What a connection needs is
 * several keys - one for each direction, plus the values that prove each side
 * knows the secret - and they must be independent: recovering one must say
 * nothing about the rest.
 *
 * The construction has two halves.  Extract concentrates whatever entropy the
 * input has into a fixed-size key, whether the input was a curve point, a
 * password or a previous secret.  Expand then produces as many independent
 * keys as are wanted, each labelled with what it is for - so a key derived for
 * one purpose can never collide with one derived for another, even from the
 * same secret.
 *
 * TLS 1.3 wraps the labels in a small structure of its own so that two
 * different protocols using the same construction can never derive the same
 * key either.  That wrapper is here too.
 */
#include "kernel.h"
#include "crypto.h"

/* Extract: one HMAC of the input under the salt. */
void hkdf_extract(const u8 *salt, size_t salt_len,
                  const u8 *material, size_t material_len,
                  u8 out[SHA256_SIZE]) {
    static const u8 zeros[SHA256_SIZE] = { 0 };
    if (!salt || !salt_len) { salt = zeros; salt_len = sizeof zeros; }
    hmac_sha256(salt, salt_len, material, material_len, out);
}

/* Expand: a chain of HMACs, each fed the previous block, the label and a
 * counter.  The counter is what makes each block different; the chaining is
 * what makes the whole output depend on all of it. */
void hkdf_expand(const u8 secret[SHA256_SIZE],
                 const u8 *info, size_t info_len,
                 u8 *out, size_t out_len) {
    u8 block[SHA256_SIZE];
    size_t have = 0;
    u8 counter = 1;
    bool first = true;

    while (have < out_len) {
        /* HMAC over the previous block, the label, and the counter. */
        const u8 *parts[3];
        size_t lengths[3];
        int count = 0;

        if (!first) { parts[count] = block; lengths[count] = SHA256_SIZE; count++; }
        if (info && info_len) { parts[count] = info; lengths[count] = info_len; count++; }
        parts[count] = &counter;
        lengths[count] = 1;
        count++;

        hmac_sha256_vector(secret, SHA256_SIZE, count, parts, lengths, block);
        first = false;

        size_t chunk = out_len - have < SHA256_SIZE ? out_len - have : SHA256_SIZE;
        memcpy(out + have, block, chunk);
        have += chunk;
        counter++;
    }
}

/* ------------------------------------------------------------ the TLS form
 *
 * The label a key is derived under is not the bare string: it is a structure
 * holding the length wanted, the string prefixed with "tls13 ", and a context
 * value - normally a hash of everything that has been said so far.  Including
 * the transcript is what binds every key to the exact handshake that produced
 * it, so a key from one connection cannot be replayed into another.
 */
void tls_hkdf_expand_label(const u8 secret[SHA256_SIZE],
                           const char *label,
                           const u8 *context, size_t context_len,
                           u8 *out, size_t out_len) {
    u8 info[2 + 1 + 6 + 32 + 1 + 32];
    size_t at = 0;

    info[at++] = (u8)(out_len >> 8);
    info[at++] = (u8)out_len;

    size_t label_len = strlen(label);
    if (label_len > 32) label_len = 32;
    info[at++] = (u8)(6 + label_len);
    memcpy(info + at, "tls13 ", 6);
    at += 6;
    memcpy(info + at, label, label_len);
    at += label_len;

    if (context_len > 32) context_len = 32;
    info[at++] = (u8)context_len;
    if (context && context_len) {
        memcpy(info + at, context, context_len);
        at += context_len;
    }

    hkdf_expand(secret, info, at, out, out_len);
}

/* Moving from one stage of the handshake to the next.  Each stage's secret is
 * derived from the previous one before the new material is mixed in, which is
 * what stops a compromise at one stage from unwinding the earlier ones. */
void tls_derive_secret(const u8 secret[SHA256_SIZE], const char *label,
                       const u8 *transcript_hash, u8 out[SHA256_SIZE]) {
    static const u8 empty_hash[SHA256_SIZE] = {
        /* SHA-256 of nothing at all, which is what an empty transcript is. */
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55,
    };
    const u8 *context = transcript_hash ? transcript_hash : empty_hash;
    tls_hkdf_expand_label(secret, label, context, SHA256_SIZE, out, SHA256_SIZE);
}

/* ------------------------------------------------------------------- test */

int hkdf_selftest(void) {
    int failures = 0;

    /* The first of the vectors RFC 5869 publishes. */
    static const u8 material[22] = {
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
    };
    static const u8 salt[13] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,
    };
    static const u8 info[10] = {
        0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,
    };
    static const u8 expected_prk[32] = {
        0x07,0x77,0x09,0x36,0x2c,0x2e,0x32,0xdf,0x0d,0xdc,0x3f,0x0d,0xc4,0x7b,0xba,0x63,
        0x90,0xb6,0xc7,0x3b,0xb5,0x0f,0x9c,0x31,0x22,0xec,0x84,0x4a,0xd7,0xc2,0xb3,0xe5,
    };
    static const u8 expected_okm[42] = {
        0x3c,0xb2,0x5f,0x25,0xfa,0xac,0xd5,0x7a,0x90,0x43,0x4f,0x64,0xd0,0x36,0x2f,0x2a,
        0x2d,0x2d,0x0a,0x90,0xcf,0x1a,0x5a,0x4c,0x5d,0xb0,0x2d,0x56,0xec,0xc4,0xc5,0xbf,
        0x34,0x00,0x72,0x08,0xd5,0xb8,0x87,0x18,0x58,0x65,
    };

    u8 prk[32];
    hkdf_extract(salt, sizeof salt, material, sizeof material, prk);
    if (memcmp(prk, expected_prk, sizeof prk)) {
        kerr("hkdf", "extract does not match the published vector");
        failures++;
    }

    u8 okm[42];
    hkdf_expand(prk, info, sizeof info, okm, sizeof okm);
    if (memcmp(okm, expected_okm, sizeof okm)) {
        kerr("hkdf", "expand does not match the published vector");
        failures++;
    }

    /* Two different labels over the same secret have to give unrelated keys.
     * If they did not, every key in a connection would be the same one. */
    u8 a[32], b[32];
    tls_hkdf_expand_label(prk, "key", NULL, 0, a, sizeof a);
    tls_hkdf_expand_label(prk, "iv", NULL, 0, b, sizeof b);
    if (!memcmp(a, b, sizeof a)) {
        kerr("hkdf", "two labels produced the same key");
        failures++;
    }

    if (!failures)
        kinfo("crypto", "HKDF matches the vectors RFC 5869 publishes");
    return failures;
}
