/* x509.c - reading a certificate and deciding whether to believe it.
 *
 * A certificate is a statement: "the holder of this public key is
 * example.com", signed by somebody.  On its own that is worth nothing - anyone
 * can write it.  It is worth something only when the signature traces back,
 * through however many intermediate signers, to a key that was already
 * trusted before the connection started.
 *
 * So there are three separate questions and all of them have to be answered:
 *
 *   1. Does the certificate actually say what we want?  The name has to match
 *      the site being visited, and it has to be within its validity dates.
 *   2. Does each signature in the chain check out against the next
 *      certificate's key?
 *   3. Does the chain end at a key we already trusted?
 *
 * Skipping any one of them makes the whole thing decorative.  A connection
 * that encrypts to whoever answered is not secure against the one attacker
 * that matters - the one who answered instead of the site.
 *
 * The encoding is DER: everything is a tag, a length and a value, nested.
 * It is simple, and its simplicity is load-bearing - a parser that guesses is
 * a parser an attacker can steer, so every length here is checked against what
 * actually remains.
 */
#include "kernel.h"
#include "klog.h"
#include "crypto.h"
#include "x509.h"
#include "ecc.h"

/* ------------------------------------------------------------------ DER */

#define DER_BOOLEAN      0x01
#define DER_INTEGER      0x02
#define DER_BIT_STRING   0x03
#define DER_OCTET_STRING 0x04
#define DER_NULL         0x05
#define DER_OID          0x06
#define DER_UTF8         0x0C
#define DER_SEQUENCE     0x30
#define DER_SET          0x31
#define DER_PRINTABLE    0x13
#define DER_IA5          0x16
#define DER_UTCTIME      0x17
#define DER_GENTIME      0x18

typedef struct {
    const u8 *data;
    size_t    len;
    size_t    at;
} der_t;

static void der_init(der_t *d, const u8 *data, size_t len) {
    d->data = data;
    d->len = len;
    d->at = 0;
}

static bool der_at_end(const der_t *d) { return d->at >= d->len; }

/* Read one tag-length-value.  The value is handed back as a reader of its own,
 * bounded to exactly its own length - which is what stops a nested structure
 * from reading past its parent. */
static bool der_read(der_t *d, u8 *tag_out, der_t *value_out) {
    if (d->at + 2 > d->len) return false;

    u8 tag = d->data[d->at++];
    u8 first = d->data[d->at++];
    size_t length;

    if (first < 0x80) {
        length = first;
    } else {
        int count = first & 0x7F;
        /* A length of a length longer than four bytes describes something
         * larger than any certificate; refusing it costs nothing. */
        if (count == 0 || count > 4 || d->at + (size_t)count > d->len) return false;
        length = 0;
        for (int i = 0; i < count; i++) length = (length << 8) | d->data[d->at++];
    }

    if (length > d->len - d->at) return false;

    if (tag_out) *tag_out = tag;
    if (value_out) der_init(value_out, d->data + d->at, length);
    d->at += length;
    return true;
}

/* Read one and require it to be what was expected. */
static bool der_expect(der_t *d, u8 want, der_t *value_out) {
    u8 tag;
    der_t value;
    if (!der_read(d, &tag, &value)) return false;
    if (tag != want) return false;
    if (value_out) *value_out = value;
    return true;
}

/* Step over one without looking at it. */
static bool der_skip(der_t *d) { return der_read(d, NULL, NULL); }

/* The bytes of the element about to be read, including its own header - which
 * is what a signature is computed over. */
static bool der_peek_raw(const der_t *d, const u8 **start, size_t *len) {
    der_t copy = *d;
    size_t from = copy.at;
    if (!der_skip(&copy)) return false;
    *start = d->data + from;
    *len = copy.at - from;
    return true;
}

/* ---------------------------------------------------------- the identifiers
 *
 * Object identifiers are compared as bytes rather than decoded: there is no
 * reason to turn one into numbers when the only question is which of a handful
 * it is.
 */
#define OID_MATCH(v, len, bytes) \
    ((len) == sizeof(bytes) && !memcmp((v), (bytes), sizeof(bytes)))

static const u8 oid_rsa_encryption[]      = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01 };
static const u8 oid_sha256_with_rsa[]     = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B };
static const u8 oid_sha384_with_rsa[]     = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0C };
static const u8 oid_sha512_with_rsa[]     = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0D };
static const u8 oid_rsa_pss[]             = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0A };
static const u8 oid_ec_public_key[]       = { 0x2A,0x86,0x48,0xCE,0x3D,0x02,0x01 };
static const u8 oid_ecdsa_sha256[]        = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02 };
static const u8 oid_ecdsa_sha384[]        = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x03 };
static const u8 oid_ecdsa_sha512[]        = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x04 };
static const u8 oid_curve_p256[]          = { 0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07 };
static const u8 oid_curve_p384[]          = { 0x2B,0x81,0x04,0x00,0x22 };
static const u8 oid_curve_p521[]          = { 0x2B,0x81,0x04,0x00,0x23 };
static const u8 oid_common_name[]         = { 0x55,0x04,0x03 };
static const u8 oid_organisation[]        = { 0x55,0x04,0x0A };
static const u8 oid_basic_constraints[]   = { 0x55,0x1D,0x13 };
static const u8 oid_subject_alt_name[]    = { 0x55,0x1D,0x11 };

/* ------------------------------------------------------------------ names */

/* A distinguished name is a sequence of sets of pairs.  Only the common name
 * and the organisation are of any interest here; the rest is filed away. */
static void read_name(der_t *name, char *common, size_t common_cap,
                      char *organisation, size_t organisation_cap) {
    if (common_cap) common[0] = 0;
    if (organisation_cap) organisation[0] = 0;

    while (!der_at_end(name)) {
        der_t set;
        if (!der_expect(name, DER_SET, &set)) return;

        while (!der_at_end(&set)) {
            der_t pair;
            if (!der_expect(&set, DER_SEQUENCE, &pair)) break;

            der_t oid;
            if (!der_expect(&pair, DER_OID, &oid)) break;

            u8 tag;
            der_t value;
            if (!der_read(&pair, &tag, &value)) break;

            char *into = NULL;
            size_t cap = 0;
            if (OID_MATCH(oid.data, oid.len, oid_common_name)) {
                into = common; cap = common_cap;
            } else if (OID_MATCH(oid.data, oid.len, oid_organisation)) {
                into = organisation; cap = organisation_cap;
            }
            if (!into || !cap) continue;

            size_t n = value.len < cap - 1 ? value.len : cap - 1;
            memcpy(into, value.data, n);
            into[n] = 0;
        }
    }
}

/* ------------------------------------------------------------------ time */

/* Certificates state times as digits.  The two-digit-year form runs out in
 * 2049 by convention, which is why both forms exist. */
static bool read_time(const der_t *value, u8 tag, x509_time_t *out) {
    const u8 *p = value->data;
    size_t len = value->len;
    if (len < 10) return false;

    int at = 0;
    #define TWO_DIGITS() ({ int v = (p[at] - '0') * 10 + (p[at + 1] - '0'); at += 2; v; })

    if (tag == DER_GENTIME) {
        if (len < 12) return false;
        out->year = TWO_DIGITS() * 100;
        out->year += TWO_DIGITS();
    } else {
        int year = TWO_DIGITS();
        out->year = year < 50 ? 2000 + year : 1900 + year;
    }
    out->month = (u8)TWO_DIGITS();
    out->day = (u8)TWO_DIGITS();
    out->hour = (u8)TWO_DIGITS();
    out->minute = (u8)TWO_DIGITS();
    out->second = at + 1 < (int)len ? (u8)TWO_DIGITS() : 0;
    #undef TWO_DIGITS

    return out->month >= 1 && out->month <= 12 && out->day >= 1 && out->day <= 31;
}

/* Comparing two of them, which is all validity checking needs. */
static int compare_time(const x509_time_t *a, const x509_time_t *b) {
    if (a->year != b->year) return a->year < b->year ? -1 : 1;
    if (a->month != b->month) return a->month < b->month ? -1 : 1;
    if (a->day != b->day) return a->day < b->day ? -1 : 1;
    if (a->hour != b->hour) return a->hour < b->hour ? -1 : 1;
    if (a->minute != b->minute) return a->minute < b->minute ? -1 : 1;
    if (a->second != b->second) return a->second < b->second ? -1 : 1;
    return 0;
}

/* ------------------------------------------------------------ the key */

static bool read_public_key(der_t *spki, x509_cert_t *cert) {
    der_t algorithm;
    if (!der_expect(spki, DER_SEQUENCE, &algorithm)) return false;

    der_t oid;
    if (!der_expect(&algorithm, DER_OID, &oid)) return false;

    der_t bits;
    if (!der_expect(spki, DER_BIT_STRING, &bits)) return false;
    /* A bit string starts with a count of unused bits, which for a key is
     * always zero. */
    if (bits.len < 1 || bits.data[0] != 0) return false;
    der_t key;
    der_init(&key, bits.data + 1, bits.len - 1);

    if (OID_MATCH(oid.data, oid.len, oid_rsa_encryption)) {
        cert->key_type = X509_KEY_RSA;

        der_t rsa;
        if (!der_expect(&key, DER_SEQUENCE, &rsa)) return false;

        der_t modulus, exponent;
        if (!der_expect(&rsa, DER_INTEGER, &modulus)) return false;
        if (!der_expect(&rsa, DER_INTEGER, &exponent)) return false;

        /* An integer that would look negative is stored with a leading zero;
         * a key is never negative, so that byte is padding. */
        const u8 *m = modulus.data;
        size_t m_len = modulus.len;
        while (m_len > 1 && m[0] == 0) { m++; m_len--; }

        if (m_len > sizeof cert->rsa_modulus) return false;
        memcpy(cert->rsa_modulus, m, m_len);
        cert->rsa_modulus_len = m_len;

        cert->rsa_exponent = 0;
        for (size_t i = 0; i < exponent.len && i < 4; i++)
            cert->rsa_exponent = (cert->rsa_exponent << 8) | exponent.data[i];
        return cert->rsa_modulus_len >= 128 && cert->rsa_exponent >= 3;
    }

    if (OID_MATCH(oid.data, oid.len, oid_ec_public_key)) {
        cert->key_type = X509_KEY_EC;

        /* Which curve is a second identifier after the first, and it matters:
         * the same bytes mean different points on different curves, so a key
         * whose curve is unknown cannot be used even though it parses. */
        der_t curve;
        if (!der_expect(&algorithm, DER_OID, &curve)) return false;

        if (OID_MATCH(curve.data, curve.len, oid_curve_p256)) cert->ec_curve = ECC_P256;
        else if (OID_MATCH(curve.data, curve.len, oid_curve_p384)) cert->ec_curve = ECC_P384;
        else if (OID_MATCH(curve.data, curve.len, oid_curve_p521)) {
            /* Recognised so the message can say so, and not supported: the
             * largest curve is used by almost nothing and would need a third
             * set of constants. */
            cert->ec_curve = 0;
            return false;
        } else {
            cert->ec_curve = 0;
            return false;
        }

        /* The point, in the form with both coordinates written out.  The
         * compressed forms would need a square root to undo and no authority
         * issues them. */
        if (key.len < 1 || key.data[0] != 0x04) return false;
        if (key.len > sizeof cert->ec_point) return false;

        size_t expected = 1 + 2 * (size_t)ecc_field_bytes(cert->ec_curve);
        if (key.len != expected) return false;

        memcpy(cert->ec_point, key.data, key.len);
        cert->ec_point_len = key.len;
        return true;
    }

    cert->key_type = X509_KEY_UNKNOWN;
    return false;
}

/* --------------------------------------------------------- the extensions */

static void read_extensions(der_t *extensions, x509_cert_t *cert) {
    while (!der_at_end(extensions)) {
        der_t extension;
        if (!der_expect(extensions, DER_SEQUENCE, &extension)) return;

        der_t oid;
        if (!der_expect(&extension, DER_OID, &oid)) return;

        /* An optional "critical" flag may come before the value. */
        u8 tag;
        der_t value;
        if (!der_read(&extension, &tag, &value)) return;
        if (tag == DER_BOOLEAN) {
            if (!der_read(&extension, &tag, &value)) return;
        }
        if (tag != DER_OCTET_STRING) continue;

        der_t body;
        der_init(&body, value.data, value.len);

        if (OID_MATCH(oid.data, oid.len, oid_basic_constraints)) {
            der_t constraints;
            if (!der_expect(&body, DER_SEQUENCE, &constraints)) continue;
            if (der_at_end(&constraints)) continue;
            der_t flag;
            if (der_expect(&constraints, DER_BOOLEAN, &flag) && flag.len == 1)
                cert->is_ca = flag.data[0] != 0;
        } else if (OID_MATCH(oid.data, oid.len, oid_subject_alt_name)) {
            /* The names the certificate is actually for.  Modern browsers
             * ignore the common name entirely and use only these. */
            der_t names;
            if (!der_expect(&body, DER_SEQUENCE, &names)) continue;

            while (!der_at_end(&names) && cert->alt_name_count < X509_MAX_NAMES) {
                u8 name_tag;
                der_t name;
                if (!der_read(&names, &name_tag, &name)) break;
                /* Context tag 2 is a DNS name. */
                if (name_tag != 0x82) continue;

                size_t n = name.len < X509_NAME_MAX - 1 ? name.len : X509_NAME_MAX - 1;
                if (cert->alt_pool_used + (int)n + 1 > X509_NAME_POOL) break;
                cert->alt_offset[cert->alt_name_count] = (u16)cert->alt_pool_used;
                memcpy(cert->alt_pool + cert->alt_pool_used, name.data, n);
                cert->alt_pool_used += (int)n;
                cert->alt_pool[cert->alt_pool_used++] = 0;
                cert->alt_name_count++;
            }
        }
    }
}

/* ------------------------------------------------------------- the whole */

bool x509_parse(const u8 *data, size_t len, x509_cert_t *cert) {
    memset(cert, 0, sizeof *cert);

    der_t top;
    der_init(&top, data, len);

    der_t certificate;
    if (!der_expect(&top, DER_SEQUENCE, &certificate)) return false;

    /* The signed part, kept as raw bytes: a signature is over exactly these,
     * so they cannot be re-encoded or re-derived - they have to be the bytes
     * that arrived. */
    if (!der_peek_raw(&certificate, &cert->tbs, &cert->tbs_len)) return false;

    der_t tbs;
    if (!der_expect(&certificate, DER_SEQUENCE, &tbs)) return false;

    /* The version, if present, is wrapped in an explicit context tag. */
    if (!der_at_end(&tbs) && tbs.data[tbs.at] == 0xA0) {
        if (!der_skip(&tbs)) return false;
    }

    der_t serial;
    if (!der_expect(&tbs, DER_INTEGER, &serial)) return false;

    /* The algorithm named inside the signed part.  It has to match the one
     * outside it, or a signature could be checked under an algorithm the
     * signer never agreed to. */
    der_t inner_algorithm;
    if (!der_expect(&tbs, DER_SEQUENCE, &inner_algorithm)) return false;
    der_t inner_oid;
    if (!der_expect(&inner_algorithm, DER_OID, &inner_oid)) return false;

    der_t issuer;
    if (!der_peek_raw(&tbs, &cert->issuer_raw, &cert->issuer_raw_len)) return false;
    if (!der_expect(&tbs, DER_SEQUENCE, &issuer)) return false;
    read_name(&issuer, cert->issuer_common_name, sizeof cert->issuer_common_name,
              cert->issuer_organisation, sizeof cert->issuer_organisation);

    der_t validity;
    if (!der_expect(&tbs, DER_SEQUENCE, &validity)) return false;
    {
        u8 tag;
        der_t value;
        if (!der_read(&validity, &tag, &value)) return false;
        if (!read_time(&value, tag, &cert->not_before)) return false;
        if (!der_read(&validity, &tag, &value)) return false;
        if (!read_time(&value, tag, &cert->not_after)) return false;
    }

    der_t subject;
    if (!der_peek_raw(&tbs, &cert->subject_raw, &cert->subject_raw_len)) return false;
    if (!der_expect(&tbs, DER_SEQUENCE, &subject)) return false;
    read_name(&subject, cert->subject_common_name, sizeof cert->subject_common_name,
              cert->subject_organisation, sizeof cert->subject_organisation);

    der_t spki;
    if (!der_expect(&tbs, DER_SEQUENCE, &spki)) return false;
    if (!read_public_key(&spki, cert)) {
        /* An unusable key is still worth reporting properly rather than as a
         * parse failure: the difference matters to whoever is reading the
         * message. */
        cert->key_usable = false;
    } else {
        cert->key_usable = (cert->key_type == X509_KEY_RSA) ||
                           (cert->key_type == X509_KEY_EC && cert->ec_curve != 0);
    }

    /* Whatever is left of the signed part: issuer and subject unique
     * identifiers, then the extensions in a context tag. */
    while (!der_at_end(&tbs)) {
        u8 tag;
        der_t value;
        size_t before = tbs.at;
        if (!der_read(&tbs, &tag, &value)) break;
        if (tag == 0xA3) {
            der_t extensions;
            if (der_expect(&value, DER_SEQUENCE, &extensions))
                read_extensions(&extensions, cert);
        }
        if (tbs.at == before) break;
    }

    /* And the signature itself, outside the signed part. */
    der_t algorithm;
    if (!der_expect(&certificate, DER_SEQUENCE, &algorithm)) return false;
    der_t oid;
    if (!der_expect(&algorithm, DER_OID, &oid)) return false;

    if (OID_MATCH(oid.data, oid.len, oid_sha256_with_rsa))      cert->signature_algorithm = X509_SIG_RSA_SHA256;
    else if (OID_MATCH(oid.data, oid.len, oid_sha384_with_rsa)) cert->signature_algorithm = X509_SIG_RSA_SHA384;
    else if (OID_MATCH(oid.data, oid.len, oid_sha512_with_rsa)) cert->signature_algorithm = X509_SIG_RSA_SHA512;
    else if (OID_MATCH(oid.data, oid.len, oid_rsa_pss))         cert->signature_algorithm = X509_SIG_RSA_PSS;
    else if (OID_MATCH(oid.data, oid.len, oid_ecdsa_sha256))    cert->signature_algorithm = X509_SIG_ECDSA_SHA256;
    else if (OID_MATCH(oid.data, oid.len, oid_ecdsa_sha384))    cert->signature_algorithm = X509_SIG_ECDSA_SHA384;
    else if (OID_MATCH(oid.data, oid.len, oid_ecdsa_sha512))    cert->signature_algorithm = X509_SIG_ECDSA_SHA512;
    else cert->signature_algorithm = X509_SIG_UNKNOWN;

    /* The algorithm outside has to be the one inside. */
    if (inner_oid.len != oid.len || memcmp(inner_oid.data, oid.data, oid.len)) {
        kwarn("x509", "the certificate names two different signature algorithms");
        return false;
    }

    der_t signature;
    if (!der_expect(&certificate, DER_BIT_STRING, &signature)) return false;
    if (signature.len < 1 || signature.data[0] != 0) return false;

    cert->signature = signature.data + 1;
    cert->signature_len = signature.len - 1;

    cert->parsed = true;
    return true;
}

/* -------------------------------------------------------- checking a name
 *
 * A certificate lists the names it is for, and one of them has to be the site
 * being visited.  A leading "*." matches one label and only one: a wildcard
 * that matched dots would let a certificate for "*.example.com" stand in for
 * "anything.at.all.example.com", which is not what it says.
 */
static bool name_matches(const char *pattern, const char *host) {
    if (!pattern[0] || !host[0]) return false;

    if (pattern[0] == '*' && pattern[1] == '.') {
        /* The rest has to match exactly, and the part the star stands for must
         * be a single label. */
        const char *dot = strchr(host, '.');
        if (!dot) return false;
        return strcasecmp(pattern + 2, dot + 1) == 0;
    }
    return strcasecmp(pattern, host) == 0;
}

bool x509_matches_host(const x509_cert_t *cert, const char *host) {
    /* The alternative names are authoritative when they are present, which on
     * anything issued this decade they always are. */
    for (int i = 0; i < cert->alt_name_count; i++)
        if (name_matches(cert->alt_pool + cert->alt_offset[i], host)) return true;

    if (cert->alt_name_count) return false;
    return name_matches(cert->subject_common_name, host);
}

bool x509_is_current(const x509_cert_t *cert, const x509_time_t *now) {
    if (compare_time(now, &cert->not_before) < 0) return false;
    if (compare_time(now, &cert->not_after) > 0) return false;
    return true;
}

/* -------------------------------------------------- checking a signature
 *
 * The old padding scheme, and still the one certificates are signed with: the
 * signed value is a fixed prefix, a run of ones, a zero, a short header naming
 * the hash, and then the hash itself.  Everything about it is fixed, so
 * checking means rebuilding what it should be and comparing - never taking
 * anything the signature says at face value.
 */
static const u8 sha256_der_prefix[] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,
    0x05,0x00,0x04,0x20,
};

/* The frame PKCS#1 v1.5 puts round a hash is the DER encoding of an algorithm
 * identifier and an octet string.  It is constant for a given hash, so it is
 * written out rather than built. */
static const u8 sha384_der_prefix[] = {
    0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,
    0x05,0x00,0x04,0x30,
};
static const u8 sha512_der_prefix[] = {
    0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,
    0x05,0x00,0x04,0x40,
};

bool x509_verify_signature(const x509_cert_t *signed_cert,
                           const x509_cert_t *signer) {
    if (!signed_cert->parsed || !signer->parsed) return false;

    if (!signer->key_usable) {
        kwarn("x509", "\"%s\" signed with a key type this system cannot check",
              signer->subject_common_name);
        return false;
    }

    /* Which hash, and which of the two paddings. */
    const u8 *prefix = NULL;
    size_t prefix_len = 0;
    size_t hash_len = 0;
    bool pss = false;
    u8 digest[SHA512_SIZE];

    switch (signed_cert->signature_algorithm) {
    case X509_SIG_RSA_SHA256:
        prefix = sha256_der_prefix; prefix_len = sizeof sha256_der_prefix;
        hash_len = SHA256_SIZE;
        sha256(signed_cert->tbs, signed_cert->tbs_len, digest);
        break;
    case X509_SIG_RSA_SHA384:
        prefix = sha384_der_prefix; prefix_len = sizeof sha384_der_prefix;
        hash_len = SHA384_SIZE;
        sha384(signed_cert->tbs, signed_cert->tbs_len, digest);
        break;
    case X509_SIG_RSA_SHA512:
        prefix = sha512_der_prefix; prefix_len = sizeof sha512_der_prefix;
        hash_len = SHA512_SIZE;
        sha512(signed_cert->tbs, signed_cert->tbs_len, digest);
        break;
    case X509_SIG_RSA_PSS:
        /* The parameters name the hash, but almost every PSS certificate in
         * use is SHA-256 and the parser does not read them yet.  Both are
         * tried rather than guessing: a wrong guess would reject a valid
         * chain, and trying the other costs one modular exponentiation. */
        pss = true;
        hash_len = SHA256_SIZE;
        break;

    case X509_SIG_ECDSA_SHA256:
    case X509_SIG_ECDSA_SHA384:
    case X509_SIG_ECDSA_SHA512: {
        if (signer->key_type != X509_KEY_EC || signer->ec_curve == 0) {
            kwarn("x509", "\"%s\" is signed on a curve, but its issuer's key "
                          "is not a curve key", signed_cert->subject_common_name);
            return false;
        }

        u8 digest[SHA512_SIZE];
        size_t digest_len;
        if (signed_cert->signature_algorithm == X509_SIG_ECDSA_SHA256) {
            sha256(signed_cert->tbs, signed_cert->tbs_len, digest);
            digest_len = SHA256_SIZE;
        } else if (signed_cert->signature_algorithm == X509_SIG_ECDSA_SHA384) {
            sha384(signed_cert->tbs, signed_cert->tbs_len, digest);
            digest_len = SHA384_SIZE;
        } else {
            sha512(signed_cert->tbs, signed_cert->tbs_len, digest);
            digest_len = SHA512_SIZE;
        }

        /* The signature here is a pair of numbers wrapped in the usual
         * encoding, not one long value the way an RSA signature is. */
        const u8 *r = NULL, *sig_s = NULL;
        size_t r_len = 0, s_len = 0;
        if (!ecc_split_signature(signed_cert->signature, signed_cert->signature_len,
                                 &r, &r_len, &sig_s, &s_len)) {
            kwarn("x509", "the signature on \"%s\" is not a well-formed pair",
                  signed_cert->subject_common_name);
            return false;
        }

        return ecc_verify(signer->ec_curve, signer->ec_point, signer->ec_point_len,
                          digest, digest_len, r, r_len, sig_s, s_len);
    }
    default:
        kwarn("x509", "\"%s\" is signed with %s, which this system cannot check",
              signed_cert->subject_common_name,
              x509_signature_name(signed_cert->signature_algorithm));
        return false;
    }

    /* Everything from here is RSA, so the key has to be one. */
    if (signer->key_type != X509_KEY_RSA) {
        kwarn("x509", "\"%s\" is signed with RSA, but its issuer's key is not "
                      "an RSA key", signed_cert->subject_common_name);
        return false;
    }

    if (signed_cert->signature_len != signer->rsa_modulus_len) {
        kwarn("x509", "the signature is %zu bytes and the key is %zu",
              signed_cert->signature_len, signer->rsa_modulus_len);
        return false;
    }

    if (pss) {
        u8 sha256_digest[SHA256_SIZE], sha384_digest[SHA384_SIZE];
        sha256(signed_cert->tbs, signed_cert->tbs_len, sha256_digest);
        if (rsa_pss_verify(signed_cert->signature, signed_cert->signature_len,
                           signer->rsa_modulus, signer->rsa_modulus_len,
                           signer->rsa_exponent, sha256_digest, SHA256_SIZE))
            return true;
        sha384(signed_cert->tbs, signed_cert->tbs_len, sha384_digest);
        return rsa_pss_verify(signed_cert->signature, signed_cert->signature_len,
                              signer->rsa_modulus, signer->rsa_modulus_len,
                              signer->rsa_exponent, sha384_digest, SHA384_SIZE);
    }

    static u8 recovered[X509_MAX_MODULUS];
    if (!rsa_public_op(signed_cert->signature, signed_cert->signature_len,
                       signer->rsa_modulus, signer->rsa_modulus_len,
                       signer->rsa_exponent, recovered, signed_cert->signature_len))
        return false;

    /* Rebuild what the padded value has to be, and compare the whole thing.
     * Checking only the hash and trusting the padding is how signature forgery
     * attacks on this scheme have worked. */
    size_t total = signed_cert->signature_len;
    if (total < prefix_len + hash_len + 11) return false;

    static u8 expected[X509_MAX_MODULUS];
    size_t at = 0;
    expected[at++] = 0x00;
    expected[at++] = 0x01;
    size_t padding = total - prefix_len - hash_len - 3;
    for (size_t i = 0; i < padding; i++) expected[at++] = 0xFF;
    expected[at++] = 0x00;
    memcpy(expected + at, prefix, prefix_len);
    at += prefix_len;
    memcpy(expected + at, digest, hash_len);
    at += hash_len;

    if (at != total) return false;

    u8 difference = 0;
    for (size_t i = 0; i < total; i++) difference |= expected[i] ^ recovered[i];
    return difference == 0;
}

/* ------------------------------------------------------------------ chains */

bool x509_issued_by(const x509_cert_t *cert, const x509_cert_t *possible_issuer) {
    if (!cert->parsed || !possible_issuer->parsed) return false;
    if (!cert->issuer_raw || !possible_issuer->subject_raw) return false;
    if (cert->issuer_raw_len != possible_issuer->subject_raw_len) return false;
    return memcmp(cert->issuer_raw, possible_issuer->subject_raw,
                  cert->issuer_raw_len) == 0;
}

bool x509_is_self_issued(const x509_cert_t *cert) {
    if (!cert->parsed || !cert->issuer_raw || !cert->subject_raw) return false;
    if (cert->issuer_raw_len != cert->subject_raw_len) return false;
    return memcmp(cert->issuer_raw, cert->subject_raw, cert->issuer_raw_len) == 0;
}

/* ------------------------------------------------------------ description */

const char *x509_signature_name(x509_sig_algorithm alg) {
    switch (alg) {
    case X509_SIG_RSA_SHA256:   return "RSA with SHA-256";
    case X509_SIG_RSA_SHA384:   return "RSA with SHA-384";
    case X509_SIG_RSA_SHA512:   return "RSA with SHA-512";
    case X509_SIG_RSA_PSS:      return "RSA-PSS";
    case X509_SIG_ECDSA_SHA256: return "ECDSA with SHA-256";
    case X509_SIG_ECDSA_SHA384: return "ECDSA with SHA-384";
    case X509_SIG_ECDSA_SHA512: return "ECDSA with SHA-512";
    default:                    return "an algorithm this system does not know";
    }
}
