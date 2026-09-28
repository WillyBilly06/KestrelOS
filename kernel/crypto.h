/* crypto.h - the cryptography Wi-Fi needs.
 *
 * No modern network is open, so joining one means implementing the primitives:
 * AES for the cipher, CCM for authenticated encryption, SHA-1 and SHA-256 for
 * the key derivation, HMAC over both, and PBKDF2 to turn a passphrase into a
 * key.
 *
 * All of it is written from the published specifications - FIPS 197, FIPS 180-4,
 * RFC 2104, RFC 3610, RFC 8018 - and every one is checked at start-up against
 * the test vectors those documents give.  Cryptography that has not been
 * checked against a known answer is cryptography that might be quietly wrong,
 * and quietly wrong is the worst thing it can be: everything appears to work
 * and nothing is protected.
 */
#ifndef KESTREL_CRYPTO_H
#define KESTREL_CRYPTO_H

#include "kernel.h"

/* --------------------------------------------------------------------- AES */

typedef struct {
    u32 round_key[60];       /* enough for 256-bit keys */
    int rounds;
} aes_t;

void aes_setkey(aes_t *ctx, const u8 *key, int key_bits);
void aes_encrypt_block(const aes_t *ctx, const u8 in[16], u8 out[16]);

/* ------------------------------------------------------------------ SHA-1 */

#define SHA1_SIZE 20

typedef struct {
    u32 state[5];
    u64 length;
    u8  buffer[64];
    int buffered;
} sha1_t;

void sha1_init(sha1_t *ctx);
void sha1_update(sha1_t *ctx, const void *data, size_t len);
void sha1_final(sha1_t *ctx, u8 out[SHA1_SIZE]);
void sha1(const void *data, size_t len, u8 out[SHA1_SIZE]);

/* ---------------------------------------------------------------- SHA-256 */

#define SHA256_SIZE 32

typedef struct {
    u32 state[8];
    u64 length;
    u8  buffer[64];
    int buffered;
} sha256_t;

void sha256_init(sha256_t *ctx);
void sha256_update(sha256_t *ctx, const void *data, size_t len);
void sha256_final(sha256_t *ctx, u8 out[SHA256_SIZE]);
void sha256(const void *data, size_t len, u8 out[SHA256_SIZE]);

/* ------------------------------------------------------------------- HMAC */

void hmac_sha1(const u8 *key, size_t key_len, const void *data, size_t len,
               u8 out[SHA1_SIZE]);
void hmac_sha256(const u8 *key, size_t key_len, const void *data, size_t len,
                 u8 out[SHA256_SIZE]);

/* Several messages hashed as one, which is what the key derivation wants. */
void hmac_sha1_vector(const u8 *key, size_t key_len, int count,
                      const u8 *const *data, const size_t *lens,
                      u8 out[SHA1_SIZE]);
void hmac_sha256_vector(const u8 *key, size_t key_len, int count,
                        const u8 *const *data, const size_t *lens,
                        u8 out[SHA256_SIZE]);

/* ----------------------------------------------------------------- PBKDF2 */

/* A passphrase and an SSID into a 256-bit key, as WPA specifies: 4096 rounds
 * of HMAC-SHA1, which is what makes a weak passphrase expensive to attack. */
void pbkdf2_sha1(const char *passphrase, const u8 *salt, size_t salt_len,
                 int iterations, u8 *out, size_t out_len);

/* -------------------------------------------------------------------- CCM */

/* AES-CCM, which is what CCMP is built on: encrypts and authenticates in one
 * pass over the data, with additional data that is authenticated but not
 * encrypted - the frame header, in Wi-Fi's case. */
bool aes_ccm_encrypt(const aes_t *aes, const u8 *nonce, int nonce_len,
                     const u8 *aad, int aad_len,
                     u8 *data, int data_len,
                     u8 *mic, int mic_len);

bool aes_ccm_decrypt(const aes_t *aes, const u8 *nonce, int nonce_len,
                     const u8 *aad, int aad_len,
                     u8 *data, int data_len,
                     const u8 *mic, int mic_len);

/* ------------------------------------------------------------------- tests */

/* Run every known-answer test.  Returns the number that failed, and logs each
 * one.  Called at start-up. */
/* ---------------------------------------------------------- SHA-512, SHA-384
 *
 * Certificate authorities sign with SHA-384 often enough that a chain cannot
 * be checked without it.  See sha512.c. */
#define SHA512_SIZE 64
#define SHA384_SIZE 48

typedef struct {
    u64    state[8];
    u64    length;
    u8     buffer[128];
    size_t buffered;
    size_t out_len;
} sha512_t;

void sha512_init(sha512_t *ctx);
void sha384_init(sha512_t *ctx);
void sha512_update(sha512_t *ctx, const void *data, size_t len);
void sha512_final(sha512_t *ctx, u8 *out);
void sha512(const void *data, size_t len, u8 out[SHA512_SIZE]);
void sha384(const void *data, size_t len, u8 out[SHA384_SIZE]);
int  sha512_selftest(void);

/* ---------------------------------------------------------------- random
 *
 * Unpredictable numbers, from the processor where it has a generator and from
 * a pool of the machine's own timing where it does not.  See random.c. */
void random_init(void);
void random_bytes(void *out, size_t len);
void random_stir(const void *data, size_t len);
void random_event(void);          /* called from the interrupt path */
bool random_is_strong(void);      /* false when there is no hardware source */
int  random_selftest(void);

/* -------------------------------------------------------------------- RSA
 *
 * Raising a signature to the public exponent, which is what checking a
 * certificate comes down to.  `out_len` should be the modulus's length.  See
 * bignum.c. */
bool rsa_public_op(const u8 *signature, size_t signature_len,
                   const u8 *modulus, size_t modulus_len,
                   u32 exponent, u8 *out, size_t out_len);
int  bignum_selftest(void);

/* ------------------------------------------------------------------- HKDF
 *
 * Turning one shared secret into all the independent keys a connection needs.
 * See hkdf.c. */
void hkdf_extract(const u8 *salt, size_t salt_len,
                  const u8 *material, size_t material_len, u8 out[SHA256_SIZE]);
void hkdf_expand(const u8 secret[SHA256_SIZE], const u8 *info, size_t info_len,
                 u8 *out, size_t out_len);
/* The form TLS 1.3 uses, which wraps the label so no two protocols can derive
 * the same key from the same secret. */
void tls_hkdf_expand_label(const u8 secret[SHA256_SIZE], const char *label,
                           const u8 *context, size_t context_len,
                           u8 *out, size_t out_len);
void tls_derive_secret(const u8 secret[SHA256_SIZE], const char *label,
                       const u8 *transcript_hash, u8 out[SHA256_SIZE]);
int  hkdf_selftest(void);

/* --------------------------------------------------------------- AES-GCM
 *
 * The mode TLS 1.3 encrypts with: it encrypts and authenticates in one pass,
 * so an altered message is refused rather than decrypted into rubbish.  The
 * nonce is always twelve bytes and the tag sixteen.  See gcm.c. */
void aes_gcm_encrypt(const aes_t *aes, const u8 nonce[12],
                     const u8 *aad, size_t aad_len,
                     const u8 *plain, size_t len,
                     u8 *cipher, u8 tag[16]);
/* False when the tag does not match, in which case nothing is written to
 * `plain`: the check happens before any decryption. */
bool aes_gcm_decrypt(const aes_t *aes, const u8 nonce[12],
                     const u8 *aad, size_t aad_len,
                     const u8 *cipher, size_t len,
                     const u8 tag[16], u8 *plain);
int  gcm_selftest(void);

/* ------------------------------------------------------------------ x25519
 *
 * The key agreement TLS 1.3 uses by default.  Both sides multiply the same
 * base point by a private number, exchange the results, and multiply again to
 * arrive at the same secret.  See x25519.c. */
void x25519(u8 out[32], const u8 scalar[32], const u8 point[32]);
void x25519_public(u8 out[32], const u8 secret[32]);
int  x25519_selftest(void);

/* --------------------------------------------------------------- RSA-PSS
 *
 * The padding TLS 1.3 requires for a server's proof of key possession.  See
 * rsa_pss.c. */
bool rsa_pss_verify(const u8 *signature, size_t signature_len,
                    const u8 *modulus, size_t modulus_len, u32 exponent,
                    const u8 *message_hash, int hash_len);
int  rsa_pss_selftest(void);

int crypto_selftest(void);

#endif
