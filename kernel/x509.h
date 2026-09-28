/* x509.h - what a certificate says, once it has been read.
 *
 * See x509.c.  The three questions a certificate has to answer - is it for
 * this site, is it current, and does its signature trace back to something
 * already trusted - are separate functions here, because they are separate
 * questions and skipping any one of them makes the rest pointless.
 */
#ifndef KESTREL_X509_H
#define KESTREL_X509_H

#include "kernel.h"

#define X509_MAX_MODULUS 512        /* four thousand and ninety-six bits */
/* A large site's certificate can list dozens of names - every subdomain it
 * serves, every language variant, every alternate spelling.  Stopping at
 * sixteen means the one that would have matched is often not among them, and
 * the site is then rejected as "for a different site", which is both wrong and
 * one of the harder mistakes to see. */
#define X509_MAX_NAMES   96
#define X509_NAME_POOL   4096
#define X509_NAME_MAX    128

typedef enum {
    X509_KEY_UNKNOWN = 0,
    X509_KEY_RSA,
    X509_KEY_EC,
} x509_key_type;

typedef enum {
    X509_SIG_UNKNOWN = 0,
    X509_SIG_RSA_SHA256,
    X509_SIG_RSA_SHA384,
    X509_SIG_RSA_SHA512,
    X509_SIG_RSA_PSS,
    X509_SIG_ECDSA_SHA256,
    X509_SIG_ECDSA_SHA384,
    X509_SIG_ECDSA_SHA512,
} x509_sig_algorithm;

typedef struct {
    u16 year;
    u8  month, day, hour, minute, second;
} x509_time_t;

typedef struct {
    bool parsed;

    /* The bytes the signature is over.  Kept as they arrived: re-encoding
     * them would change them, and then no signature would ever check out. */
    const u8 *tbs;
    size_t    tbs_len;

    /* The issuer and subject names exactly as they were encoded.  Matching a
     * certificate to its issuer is done on these bytes and not on the decoded
     * text: two different authorities can print the same common name, and the
     * encoding carries more than the two fields read out below. */
    const u8 *issuer_raw;
    size_t    issuer_raw_len;
    const u8 *subject_raw;
    size_t    subject_raw_len;

    char subject_common_name[X509_NAME_MAX];
    char subject_organisation[X509_NAME_MAX];
    char issuer_common_name[X509_NAME_MAX];
    char issuer_organisation[X509_NAME_MAX];

    x509_time_t not_before, not_after;

    /* Every name this certificate is for.  A modern one lists them here and
     * the common name is decoration. */
    /* Packed end to end rather than as a rectangle: ninety-six slots of a
     * hundred and twenty-eight bytes each would be twelve kilobytes for a
     * handful of short names. */
    char alt_pool[X509_NAME_POOL];
    u16  alt_offset[X509_MAX_NAMES];
    int  alt_name_count;
    int  alt_pool_used;

    bool is_ca;

    x509_key_type key_type;
    bool          key_usable;      /* false for a key type we cannot check */
    u8            rsa_modulus[X509_MAX_MODULUS];
    size_t        rsa_modulus_len;
    u32           rsa_exponent;

    /* For an elliptic-curve key: which curve, and the point itself in its
     * uncompressed form - a marker byte and the two coordinates. */
    int  ec_curve;                 /* an ecc_curve; 0 when it is not one */
    u8   ec_point[1 + 2 * 48];
    size_t ec_point_len;

    x509_sig_algorithm signature_algorithm;
    const u8 *signature;
    size_t    signature_len;
} x509_cert_t;

bool x509_parse(const u8 *data, size_t len, x509_cert_t *cert);

/* Is this certificate for that site?  Wildcards match one label, never a dot. */
bool x509_matches_host(const x509_cert_t *cert, const char *host);

/* Is it within its dates? */
bool x509_is_current(const x509_cert_t *cert, const x509_time_t *now);

/* Does `signed_cert`'s signature check out against `signer`'s key? */
bool x509_verify_signature(const x509_cert_t *signed_cert, const x509_cert_t *signer);

/* Was this issued by that?  Compares the encoded names, not the printed ones. */
bool x509_issued_by(const x509_cert_t *cert, const x509_cert_t *possible_issuer);

/* A certificate whose issuer is itself.  A root, in other words. */
bool x509_is_self_issued(const x509_cert_t *cert);

const char *x509_signature_name(x509_sig_algorithm alg);

#endif
