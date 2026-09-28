/* ecc.h - signatures made on an elliptic curve.
 *
 * See ecc.c.  Verification only: this system has no private keys and never
 * signs anything, so there is no signing here and no need for the
 * constant-time care that signing would demand.
 */
#ifndef KESTREL_ECC_H
#define KESTREL_ECC_H

#include "kernel.h"

typedef enum {
    ECC_NONE = 0,
    ECC_P256,        /* secp256r1, what almost everything uses */
    ECC_P384,        /* secp384r1, used by some authorities     */
} ecc_curve;

int ecc_field_bytes(ecc_curve which);

/* `public_key` is the uncompressed form: 0x04 followed by the two coordinates.
 * `r` and `s` are the two halves of the signature, as big-endian numbers with
 * any leading zeros already removed. */
bool ecc_verify(ecc_curve which, const u8 *public_key, size_t public_key_len,
                const u8 *hash, size_t hash_len,
                const u8 *r, size_t r_len,
                const u8 *s, size_t s_len);

/* Pull r and s out of the encoded form a certificate or a handshake stores. */
bool ecc_split_signature(const u8 *der, size_t len,
                         const u8 **r, size_t *r_len,
                         const u8 **s, size_t *s_len);

int ecc_selftest(void);

#endif
