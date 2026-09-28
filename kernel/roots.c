/* roots.c - who this machine is willing to believe, and how a chain is checked.
 *
 * A server hands over a certificate saying it is the site you asked for.  That
 * claim is worth nothing on its own; what makes it worth something is that
 * some authority signed it, and that authority was signed by another, until
 * the chain reaches one this machine already had before the connection
 * started.  Those starting points are the only thing that cannot be checked -
 * they are the decision about who to trust, and it has to be made in advance.
 *
 * The list comes from the file the build puts at /etc/ssl/roots.bin, which is
 * Mozilla's, the same one most software uses.  It is held as the encoded bytes
 * and parsed one at a time when a chain actually needs a particular authority,
 * because parsing all eighty at once would cost a quarter of a megabyte to
 * answer a question about one of them.
 *
 * The check itself is the part worth being careful about.  Every one of these
 * has to hold, and a validator that skips any of them is not a validator:
 *
 *   - the leaf is for the host that was asked for;
 *   - every certificate in the chain is within its dates;
 *   - each one was really signed by the next;
 *   - everything above the leaf is marked as an authority;
 *   - and the top of the chain is one of the authorities in the file.
 *
 * The last one is the one that is easy to get subtly wrong.  A server can send
 * its own root along with the chain, and believing that root because it is
 * self-signed - it is, of course, it signed it itself - would mean believing
 * anybody.  The root a chain ends at has to be found in the file, not in the
 * connection.
 */
#include "kernel.h"
#include "klog.h"
#include "vfs.h"
#include "mm.h"
#include "time.h"
#include "x509.h"
#include "crypto.h"

#define ROOTS_PATH "/etc/ssl/roots.bin"

static const u8 roots_magic[8] = { 'K','R','O','O','T','S',0x00,0x01 };

/* While the self-test is deliberately presenting bad chains, the running
 * commentary about why each one was refused is the expected answer rather than
 * news - and printing it makes the log look as though something is wrong. */
static bool  roots_quiet;

static u8    *roots_data;
static size_t roots_len;
static int    roots_count;
static bool   roots_tried;

/* One scratch certificate, reused.  These are two and a half kilobytes each,
 * which is more than a kernel stack wants and far more than eighty of them
 * should occupy permanently. */
static x509_cert_t scratch;

static void load_roots(void) {
    if (roots_tried) return;
    roots_tried = true;

    vstat_t st;
    if (vfs_stat(ROOTS_PATH, &st) < 0 || st.size < 16) {
        kerr("roots", "there is no readable list of certificate authorities at "
                      "%s, so no secure connection can be checked", ROOTS_PATH);
        return;
    }

    roots_data = kmalloc((size_t)st.size);
    if (!roots_data) {
        kerr("roots", "there is not enough memory for the list of certificate "
                      "authorities");
        return;
    }

    s64 got = vfs_read_file(ROOTS_PATH, roots_data, (size_t)st.size);
    if (got != (s64)st.size) {
        kerr("roots", "the list of certificate authorities was cut short");
        kfree(roots_data);
        roots_data = NULL;
        return;
    }

    if (memcmp(roots_data, roots_magic, sizeof roots_magic)) {
        kerr("roots", "the list of certificate authorities is not in the "
                      "expected format");
        kfree(roots_data);
        roots_data = NULL;
        return;
    }

    u32 count, body;
    memcpy(&count, roots_data + 8, 4);
    memcpy(&body, roots_data + 12, 4);
    if ((size_t)body + 16 > (size_t)st.size) {
        kerr("roots", "the list of certificate authorities is truncated");
        kfree(roots_data);
        roots_data = NULL;
        return;
    }

    roots_count = (int)count;
    roots_len = (size_t)st.size;
    kinfo("roots", "%d certificate authorities are trusted", roots_count);
}

/* Walk the file, handing each authority's encoded bytes to `visit` until it
 * says stop. */
typedef bool (*root_visitor)(const u8 *der, size_t len, void *ctx);

static bool for_each_root(root_visitor visit, void *ctx) {
    load_roots();
    if (!roots_data) return false;

    size_t at = 16;
    for (int i = 0; i < roots_count; i++) {
        if (at + 4 > roots_len) break;
        u32 len;
        memcpy(&len, roots_data + at, 4);
        at += 4;
        if (len > roots_len - at) break;
        if (visit(roots_data + at, len, ctx)) return true;
        at += len;
        while (at % 4) at++;
    }
    return false;
}

/* ------------------------------------------------------- finding an issuer */

typedef struct {
    const x509_cert_t *child;      /* whose issuer we are looking for */
    x509_cert_t       *found;      /* where to leave it */
    int  name_matches;             /* how many carried the right name */
    bool signature_failed;         /* a name matched but the signature did not */
} issuer_search_t;

static bool try_as_issuer(const u8 *der, size_t len, void *ctx) {
    issuer_search_t *search = ctx;

    if (!x509_parse(der, len, &scratch)) return false;
    if (!x509_issued_by(search->child, &scratch)) return false;
    search->name_matches++;

    /* The names matching is only a hint about which one to try; the signature
     * is what decides.  Two authorities can carry the same name, and one of
     * them being the wrong one must not end the search. */
    if (!x509_verify_signature(search->child, &scratch)) {
        search->signature_failed = true;
        return false;
    }

    *search->found = scratch;
    return true;
}

bool roots_find_issuer(const x509_cert_t *child, x509_cert_t *out) {
    issuer_search_t search = { child, out, 0, false };
    if (for_each_root(try_as_issuer, &search)) return true;

    /* Saying which of the two things went wrong is the difference between a
     * message that can be acted on and one that cannot: an authority nobody
     * here has heard of is a different problem from one whose signature does
     * not check out. */
    if (roots_quiet) return false;

    if (search.signature_failed)
        kwarn("roots", "\"%s\" names \"%s%s%s\" as its issuer, and that "
                       "authority is here, but the signature does not check out",
              child->subject_common_name,
              child->issuer_common_name,
              child->issuer_organisation[0] ? " of " : "",
              child->issuer_organisation);
    else
        kwarn("roots", "\"%s\" names \"%s%s%s\" as its issuer, and no such "
                       "authority is in this system's list",
              child->subject_common_name,
              child->issuer_common_name,
              child->issuer_organisation[0] ? " of " : "",
              child->issuer_organisation);
    return false;
}

int roots_trusted_count(void) {
    load_roots();
    return roots_count;
}

/* ------------------------------------------------------------- the check */

static void now_as_x509_time(x509_time_t *out) {
    datetime_t now;
    rtc_read(&now);
    out->year = now.year;
    out->month = now.month;
    out->day = now.day;
    out->hour = now.hour;
    out->minute = now.minute;
    out->second = now.second;
}

/* Is this certificate itself one of the authorities in the file?
 *
 * The comparison is on the name and the key, not on the whole encoding.  A
 * widely trusted authority is often sent in a second form as well, signed by
 * an older one - the same name and the same key inside a different wrapper,
 * so that machines whose lists predate it can still reach something they know.
 * Comparing encodings would miss that; comparing what actually matters does
 * not. */
typedef struct {
    const x509_cert_t *wanted;
    x509_cert_t       *found;
} contains_t;

static bool same_authority(const u8 *der, size_t len, void *ctx) {
    contains_t *search = ctx;
    const x509_cert_t *wanted = search->wanted;

    if (!x509_parse(der, len, &scratch)) return false;

    if (scratch.subject_raw_len != wanted->subject_raw_len) return false;
    if (memcmp(scratch.subject_raw, wanted->subject_raw, scratch.subject_raw_len))
        return false;

    if (scratch.key_type != wanted->key_type) return false;

    if (wanted->key_type == X509_KEY_RSA) {
        if (scratch.rsa_modulus_len != wanted->rsa_modulus_len) return false;
        if (scratch.rsa_exponent != wanted->rsa_exponent) return false;
        if (memcmp(scratch.rsa_modulus, wanted->rsa_modulus, scratch.rsa_modulus_len))
            return false;
    } else if (wanted->key_type == X509_KEY_EC) {
        if (scratch.ec_curve != wanted->ec_curve) return false;
        if (scratch.ec_point_len != wanted->ec_point_len) return false;
        if (memcmp(scratch.ec_point, wanted->ec_point, scratch.ec_point_len))
            return false;
    } else {
        return false;
    }

    if (search->found) *search->found = scratch;
    return true;
}

bool roots_contains(const x509_cert_t *cert, x509_cert_t *out) {
    if (!cert->parsed || !cert->subject_raw) return false;
    if (cert->rsa_modulus_len == 0 && cert->ec_point_len == 0) return false;
    contains_t search = { cert, out };
    return for_each_root(same_authority, &search);
}

const char *roots_check_chain(const x509_cert_t *chain, int count,
                              const char *host) {
    if (count < 1) return "the server sent no certificate";
    if (!chain[0].parsed) return "the server's certificate could not be read";

    x509_time_t now;
    now_as_x509_time(&now);

    /* Is it for the site that was asked for?  A certificate for another site
     * is a real certificate, correctly signed, and completely useless here -
     * this is the check that separates a connection to the right server from a
     * connection to whoever answered. */
    if (host && !x509_matches_host(&chain[0], host))
        return "the certificate is for a different site";

    /* Walk up from the leaf, checking each link as it is crossed, and stop at
     * the first certificate this machine already trusts.
     *
     * Stopping there is the part that is easy to get wrong.  A server usually
     * sends one certificate more than is needed: above the authority that
     * actually anchors the chain sits another copy of it signed by an older
     * authority, kept there for machines whose lists are out of date.  A
     * validator that insists on reaching the top of what was sent will reject
     * a perfectly good chain because it has never heard of the ancient
     * authority at the end of it.  What matters is reaching something trusted,
     * not reaching the end. */
    for (int i = 0; i < count; i++) {
        if (!chain[i].parsed) return "part of the certificate chain could not be read";

        if (!x509_is_current(&chain[i], &now))
            return i == 0 ? "the certificate has expired or is not yet valid"
                          : "a certificate authority in the chain has expired";

        if (i > 0) {
            if (!chain[i].is_ca)
                return "a certificate in the chain is not allowed to sign others";
            if (!x509_issued_by(&chain[i - 1], &chain[i]))
                return "the certificate chain does not join up";
            if (!x509_verify_signature(&chain[i - 1], &chain[i]))
                return "a signature in the certificate chain is wrong";
        }

        if (i > 0 && roots_contains(&chain[i], NULL))
            return NULL;                /* anchored, and every link checked */
    }

    /* Nothing in the chain was itself trusted, so the last one has to have
     * been signed by something that is. */
    const x509_cert_t *top = &chain[count - 1];

    static x509_cert_t root;
    if (roots_find_issuer(top, &root)) {
        if (!root.is_ca)
            return "the authority at the top of the chain is not marked as one";
        if (!x509_is_current(&root, &now))
            return "the certificate authority this chain rests on has expired";
        return NULL;                    /* good */
    }

    /* A self-signed top that is not in the file is the case worth naming
     * precisely, because it is what a machine-in-the-middle looks like. */
    if (x509_is_self_issued(top))
        return "the chain ends at an authority this system does not trust";

    return "the certificate chain does not reach a trusted authority";
}

/* ------------------------------------------------------------------- test
 *
 * The chain check cannot be tested against a server without a network, but it
 * can be tested against the authorities already on this machine - and those
 * are real certificates, signed with real keys, which is far better material
 * than anything that could be made up here.
 *
 * Nearly every root is self-signed: the signature on it was made with the very
 * key it contains.  So each one is a complete, genuine test of the whole
 * signature path - the big-number arithmetic, the padding, the hash, the
 * certificate reader - with a known answer.  And a root checked against a
 * different root must fail, which tests the other direction.
 */

typedef struct {
    int checked;
    int verified;
    int self_signed;
    int unreadable;
    int not_checkable;
    int reported;
    bool have_pair;
    int  pair_count;
} audit_t;

/* Two unrelated authorities, kept for the negative half of the test.  Both are
 * RSA so that the check exercises the signature comparison rather than being
 * turned away at the key type. */
static x509_cert_t audit_first;
static x509_cert_t audit_second;

static bool audit_one(const u8 *der, size_t len, void *ctx) {
    audit_t *a = ctx;

    if (!x509_parse(der, len, &scratch)) {
        a->unreadable++;
        return false;
    }
    a->checked++;

    if (scratch.key_type == X509_KEY_RSA && a->pair_count < 2) {
        if (a->pair_count == 0) audit_first = scratch;
        else                    audit_second = scratch;
        a->pair_count++;
        if (a->pair_count == 2) a->have_pair = true;
    }

    if (!x509_is_self_issued(&scratch)) return false;

    /* Two kinds of authority cannot be checked here, and neither is a fault.
     * Some of the older ones signed themselves with SHA-1, which has been
     * broken for years and is deliberately not accepted.  A few use the
     * largest curve, which nothing else does and which this system does not
     * carry the constants for.  Either way it costs nothing in practice: a
     * root is trusted because it is in this file, not because of its own
     * signature, and whatever sits below it in a real chain is signed with
     * something current. */
    if (!scratch.key_usable ||
        scratch.signature_algorithm == X509_SIG_UNKNOWN) {
        a->not_checkable++;
        return false;
    }

    a->self_signed++;

    /* Signed by itself, so the check has a known answer: yes. */
    if (x509_verify_signature(&scratch, &scratch)) a->verified++;
    else if (a->reported < 8) {
        a->reported++;
        kwarn("roots", "  \"%s\": %s did not verify against its own key",
              scratch.subject_common_name[0] ? scratch.subject_common_name
                                             : scratch.subject_organisation,
              x509_signature_name(scratch.signature_algorithm));
    }
    return false;                       /* keep going through the whole list */
}

int roots_selftest(void) {
    int failures = 0;

    load_roots();
    if (!roots_data) {
        kerr("roots", "there is no list of certificate authorities to check");
        return 1;
    }

    audit_t audit;
    memset(&audit, 0, sizeof audit);
    for_each_root(audit_one, &audit);

    if (audit.unreadable) {
        kerr("roots", "%d of the certificate authorities could not be read",
             audit.unreadable);
        failures++;
    }

    /* Every one of them is self-signed and every signature has to check out.
     * A single failure here means the arithmetic is wrong somewhere, and it
     * would mean every real chain fails too. */
    if (audit.verified != audit.self_signed) {
        kerr("roots", "%d of %d self-signed authorities did not verify against "
                      "their own keys", audit.self_signed - audit.verified,
             audit.self_signed);
        failures++;
    }

    /* And the other direction: one authority checked against a different one's
     * key has to fail.  A validator that says yes to everything passes the
     * test above and is worthless. */
    if (audit.have_pair) {
        if (x509_verify_signature(&audit_first, &audit_second)) {
            kerr("roots", "an authority verified against an unrelated key");
            failures++;
        }
    }

    /* A single altered byte in the signed part has to be caught.  This is the
     * property the whole scheme rests on. */
    if (audit.have_pair && audit_first.tbs_len > 100) {
        static u8 altered[8192];
        /* Only the copy is touched; the list itself is left alone. */
        size_t offset = (size_t)(audit_first.tbs - roots_data);
        if (offset + audit_first.tbs_len < roots_len &&
            audit_first.tbs_len < sizeof altered) {
            x509_cert_t tampered = audit_first;
            memcpy(altered, audit_first.tbs, audit_first.tbs_len);
            altered[audit_first.tbs_len / 2] ^= 0x01;
            tampered.tbs = altered;
            if (x509_verify_signature(&tampered, &audit_first)) {
                kerr("roots", "a certificate with an altered body was accepted");
                failures++;
            }
        }
    }

    if (!failures)
        kinfo("crypto", "all %d checkable certificate authorities verify against "
                        "their own keys and none against anyone else's; %d more "
                        "sign themselves in a way this system will not accept",
              audit.self_signed, audit.not_checkable);
    return failures;
}

/* ------------------------------------------------- checking the checks
 *
 * The self-test above proves the signature arithmetic is right.  It says
 * nothing about the chain check itself, which is the part with the most ways
 * to be quietly wrong - and the most consequential, because a validator that
 * accepts too much looks exactly like one that works.
 *
 * The obvious way to test it is against a server that deliberately presents a
 * bad certificate, and there are public sites for exactly that.  They cannot
 * be used here: none of them completes a TLS 1.3 handshake with this client,
 * so the connection ends before there is a certificate to judge.  A test that
 * depends on somebody else's server is also a test that fails when their
 * server changes.
 *
 * So the cases are built here instead, out of the real certificates already on
 * this machine.  Each one alters exactly one thing and checks that the right
 * complaint comes back - and one case alters nothing, so that a check which
 * has started refusing everything is caught too.
 */
/* Is `needle` anywhere in `haystack`?  The kernel's string library has no
 * such function, and this is the only place that wants one. */
static bool contains(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return true;
    for (const char *at = haystack; *at; at++)
        if (!strncmp(at, needle, n)) return true;
    return false;
}

static x509_cert_t check_subject;

static bool first_usable_root(const u8 *der, size_t len, void *ctx) {
    x509_cert_t *out = ctx;
    if (!x509_parse(der, len, &scratch)) return false;
    if (!scratch.key_usable) return false;
    if (!x509_is_self_issued(&scratch)) return false;
    if (!scratch.subject_common_name[0]) return false;
    *out = scratch;
    return true;
}

int roots_check_selftest(void) {
    int failures = 0;

    load_roots();
    roots_quiet = true;
    if (!roots_data) return 0;      /* already reported */

    if (!for_each_root(first_usable_root, &check_subject)) {
        kerr("roots", "no authority could be found to check the chain rules with");
        return 1;
    }

    x509_time_t now;
    now_as_x509_time(&now);

    /* Nothing altered: a certificate that is in the list, asked for by the
     * name it carries, has to be accepted.  Without this the three cases below
     * would all pass on a check that says no to everything. */
    {
        const char *problem = roots_check_chain(&check_subject, 1,
                                                check_subject.subject_common_name);
        if (problem) {
            kerr("roots", "a trusted authority asked for by its own name was "
                          "refused: %s", problem);
            failures++;
        }
    }

    /* The name.  Same certificate, different site. */
    {
        const char *problem = roots_check_chain(&check_subject, 1,
                                                "not-this-authority.invalid");
        if (!problem) {
            kerr("roots", "a certificate was accepted for a site it is not for");
            failures++;
        } else if (!contains(problem, "different site")) {
            kerr("roots", "a certificate for another site was refused for the "
                          "wrong reason: %s", problem);
            failures++;
        }
    }

    /* The dates.  Same certificate, moved into the past. */
    {
        static x509_cert_t expired;
        expired = check_subject;
        expired.not_after = expired.not_before;
        if (expired.not_after.year > 1) expired.not_after.year--;

        const char *problem = roots_check_chain(&expired, 1,
                                                expired.subject_common_name);
        if (!problem) {
            kerr("roots", "an expired certificate was accepted");
            failures++;
        } else if (!contains(problem, "expired")) {
            kerr("roots", "an expired certificate was refused for the wrong "
                          "reason: %s", problem);
            failures++;
        }
    }

    /* The anchor.  A certificate whose issuer is nobody this machine has heard
     * of - which is what a machine-in-the-middle presents. */
    {
        static x509_cert_t unknown;
        static u8 invented_issuer[256];

        unknown = check_subject;
        size_t len = unknown.issuer_raw_len;
        if (len > sizeof invented_issuer) len = sizeof invented_issuer;
        memcpy(invented_issuer, unknown.issuer_raw, len);
        /* One byte deep in the name, so it is still a well-formed name and
         * simply belongs to somebody else. */
        invented_issuer[len / 2] ^= 0x55;
        unknown.issuer_raw = invented_issuer;
        unknown.issuer_raw_len = len;

        const char *problem = roots_check_chain(&unknown, 1,
                                                unknown.subject_common_name);
        if (!problem) {
            kerr("roots", "a certificate from an unknown authority was accepted");
            failures++;
        } else if (!contains(problem, "trusted")) {
            kerr("roots", "a certificate from an unknown authority was refused "
                          "for the wrong reason: %s", problem);
            failures++;
        }
    }

    roots_quiet = false;

    if (!failures)
        kinfo("crypto", "the certificate chain check accepts a good chain and "
                        "refuses the wrong site, an expired certificate and an "
                        "unknown authority");
    return failures;
}
