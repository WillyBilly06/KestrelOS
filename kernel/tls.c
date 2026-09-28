/* tls.c - TLS 1.3.
 *
 * Everything below this line has been building towards this file: the cipher,
 * the hash, the key derivation, the curve, the big-number arithmetic, the
 * certificate reader, the list of authorities.  None of them is useful alone.
 * What makes them useful is the protocol that ties them together into an
 * answer to one question - can I talk to this server without anyone in between
 * reading it or changing it, and am I sure it is really that server.
 *
 * Only version 1.3 is spoken here, and deliberately.  The older versions carry
 * two decades of accumulated options, most of which exist because something
 * turned out to be broken, and supporting them means supporting the ways they
 * are broken.  1.3 threw all of it out: one key exchange shape, one class of
 * cipher, no renegotiation, no compression, and the whole handshake encrypted
 * from the second message onwards.  A client that speaks only 1.3 is a
 * client with far less that can go wrong.
 *
 * The exchange, in order:
 *
 *   client                                          server
 *   ------                                          ------
 *   ClientHello        ------->
 *     key share, what I can do
 *                      <-------  ServerHello        key share, what we'll use
 *                          ...everything below here is encrypted...
 *                      <-------  EncryptedExtensions
 *                      <-------  Certificate        who I am
 *                      <-------  CertificateVerify  and here is the proof
 *                      <-------  Finished           and the handshake is intact
 *   Finished           ------->
 *   application data   <------>  application data
 *
 * One round trip.  After the two hellos both sides can compute the same
 * secret, and everything after that is encrypted with keys derived from it.
 *
 * What is NOT here, stated plainly so nobody has to find out the hard way:
 * session resumption and early data (a connection always does the full
 * handshake), client certificates (a server that demands one will be refused),
 * and elliptic-curve signatures - the client says so in its opening message,
 * so a server that has only an elliptic-curve certificate ends the connection
 * itself rather than failing halfway through.
 */
#include "kernel.h"
#include "klog.h"
#include "mm.h"
#include "net.h"
#include "time.h"
#include "crypto.h"
#include "x509.h"
#include "ecc.h"
#include "roots.h"
#include "tls.h"

/* ------------------------------------------------------------- constants */

#define REC_CHANGE_CIPHER   20
#define REC_ALERT           21
#define REC_HANDSHAKE       22
#define REC_APPLICATION     23

#define HS_CLIENT_HELLO      1
#define HS_SERVER_HELLO      2
#define HS_NEW_SESSION_TICKET 4
#define HS_ENCRYPTED_EXT     8
#define HS_CERTIFICATE      11
#define HS_CERT_REQUEST     13
#define HS_CERT_VERIFY      15
#define HS_FINISHED         20
#define HS_KEY_UPDATE       24

#define EXT_SERVER_NAME          0
#define EXT_SUPPORTED_GROUPS    10
#define EXT_SIGNATURE_ALGS      13
#define EXT_ALPN                16
#define EXT_SUPPORTED_VERSIONS  43
#define EXT_KEY_SHARE           51

#define GROUP_X25519        0x001D
#define SUITE_AES128_GCM    0x1301

#define SIG_RSA_PSS_SHA256  0x0804
#define SIG_RSA_PSS_SHA384  0x0805
#define SIG_RSA_PKCS1_SHA256 0x0401
#define SIG_RSA_PKCS1_SHA384 0x0501
#define SIG_RSA_PKCS1_SHA512 0x0601
#define SIG_ECDSA_SHA256    0x0403
#define SIG_ECDSA_SHA384    0x0503

/* A plaintext record's body is at most sixteen kilobytes; an encrypted one
 * adds the content type and the tag on top of that. */
#define MAX_PLAINTEXT       16384
#define MAX_RECORD          (MAX_PLAINTEXT + 256)

/* A certificate message carrying a long chain does not fit in one record, so
 * handshake messages are reassembled here. */
#define MAX_HANDSHAKE       32768

#define MAX_CHAIN           6

/* Room for a chain.  Three certificates of two kilobytes each is the usual
 * shape; twice that leaves room for the unusual ones. */
#define MAX_CERTIFICATES    16384

#define TLS_CONNECTIONS     8

/* The random value a server puts in its hello when it is not accepting the key
 * share that was offered, and wants the client to try again.  It is the hash
 * of a fixed string, chosen so that an old client sees it as an ordinary
 * random number and simply fails. */
static const u8 hello_retry_random[32] = {
    0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
    0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C,
};

/* -------------------------------------------------------------- the state */

typedef struct {
    bool in_use;
    int  tcp;
    bool established;
    bool closed;

    char host[256];

    /* The two directions have their own key, their own nonce base, and their
     * own counter.  Mixing them up produces a connection that appears to work
     * until the first record in the other direction. */
    aes_t read_aes, write_aes;
    u8    read_iv[12], write_iv[12];
    u64   read_seq, write_seq;
    bool  read_encrypted, write_encrypted;

    /* Only the very first record a client sends carries the older version
     * number in its header; every record after it must say 1.2, whatever it
     * contains.  The rule exists because equipment in the middle of the
     * internet looks at that byte, and a server is entitled to refuse a
     * connection that gets it wrong - which is exactly what happens, with a
     * fatal alert that arrives long after the record that caused it. */
    bool  sent_first_record;

    /* Kept so that a key update can derive the next one from it. */
    u8 read_secret[SHA256_SIZE];
    u8 write_secret[SHA256_SIZE];

    /* One record, as it arrives. */
    u8  record[MAX_RECORD + 8];
    int record_len;

    /* Decrypted application data waiting to be handed to the caller. */
    u8  plain[MAX_RECORD];
    int plain_len, plain_at;

    /* Handshake messages, reassembled across records. */
    u8  handshake[MAX_HANDSHAKE];
    int handshake_len, handshake_at;

    /* A parsed certificate does not copy the bytes it was read from - it
     * points into them, because a signature is over the bytes exactly as they
     * arrived and re-encoding them would change them.  The reassembly buffer
     * above is compacted as messages are consumed, which would move those
     * bytes out from under the pointers, so the chain is copied here first and
     * stays put for as long as the connection needs it. */
    u8  certificates[MAX_CERTIFICATES];
    int certificates_len;

    /* Everything sent and received during the handshake, hashed as it goes.
     * Both sides compute this over the same bytes in the same order, and the
     * Finished messages are what prove they agree - which is what stops
     * anybody in the middle from having quietly changed the opening messages
     * while they were still in the clear. */
    sha256_t transcript;

    /* Whether anything has come back yet.  A connection that ends without a
     * goodbye is a fault if nothing arrived and perfectly ordinary if the
     * whole page did - plenty of servers simply close rather than saying so,
     * and warning about that on every page would bury the real faults. */
    bool received_any;

    tls_peer_t peer;
    const char *error;
} tls_t;

static tls_t *connections[TLS_CONNECTIONS];

/* --------------------------------------------------------- writing bytes
 *
 * A small append-only writer.  Every length in the protocol is written before
 * the thing it measures, so the common move is to leave a gap and fill it in
 * once the contents are known.
 */
typedef struct {
    u8    *data;
    size_t cap;
    size_t at;
    bool   overflow;
} writer_t;

static void w_init(writer_t *w, u8 *data, size_t cap) {
    w->data = data; w->cap = cap; w->at = 0; w->overflow = false;
}
static void w_bytes(writer_t *w, const void *data, size_t len) {
    if (w->at + len > w->cap) { w->overflow = true; return; }
    memcpy(w->data + w->at, data, len);
    w->at += len;
}
static void w_u8(writer_t *w, u8 v) { w_bytes(w, &v, 1); }
static void w_u16(writer_t *w, u16 v) { u8 b[2] = { (u8)(v >> 8), (u8)v }; w_bytes(w, b, 2); }
static void w_u24(writer_t *w, u32 v) {
    u8 b[3] = { (u8)(v >> 16), (u8)(v >> 8), (u8)v }; w_bytes(w, b, 3);
}

/* Leave room for a length and remember where it went. */
static size_t w_open16(writer_t *w) { w_u16(w, 0); return w->at; }
static void   w_close16(writer_t *w, size_t mark) {
    if (w->overflow || mark < 2) return;
    size_t len = w->at - mark;
    w->data[mark - 2] = (u8)(len >> 8);
    w->data[mark - 1] = (u8)len;
}
static size_t w_open8(writer_t *w) { w_u8(w, 0); return w->at; }
static void   w_close8(writer_t *w, size_t mark) {
    if (w->overflow || mark < 1) return;
    w->data[mark - 1] = (u8)(w->at - mark);
}
static size_t w_open24(writer_t *w) { w_u24(w, 0); return w->at; }
static void   w_close24(writer_t *w, size_t mark) {
    if (w->overflow || mark < 3) return;
    size_t len = w->at - mark;
    w->data[mark - 3] = (u8)(len >> 16);
    w->data[mark - 2] = (u8)(len >> 8);
    w->data[mark - 1] = (u8)len;
}

/* --------------------------------------------------------- reading bytes */

typedef struct {
    const u8 *data;
    size_t    len;
    size_t    at;
    bool      bad;
} reader_t;

static void r_init(reader_t *r, const u8 *data, size_t len) {
    r->data = data; r->len = len; r->at = 0; r->bad = false;
}
static size_t r_left(const reader_t *r) { return r->bad ? 0 : r->len - r->at; }
static u8 r_u8(reader_t *r) {
    if (r_left(r) < 1) { r->bad = true; return 0; }
    return r->data[r->at++];
}
static u16 r_u16(reader_t *r) {
    if (r_left(r) < 2) { r->bad = true; return 0; }
    u16 v = (u16)((r->data[r->at] << 8) | r->data[r->at + 1]);
    r->at += 2;
    return v;
}
static u32 r_u24(reader_t *r) {
    if (r_left(r) < 3) { r->bad = true; return 0; }
    u32 v = ((u32)r->data[r->at] << 16) | ((u32)r->data[r->at + 1] << 8) | r->data[r->at + 2];
    r->at += 3;
    return v;
}
static const u8 *r_take(reader_t *r, size_t len) {
    if (r_left(r) < len) { r->bad = true; return NULL; }
    const u8 *p = r->data + r->at;
    r->at += len;
    return p;
}

/* ------------------------------------------------------- the key schedule
 *
 * Every key in the connection descends from one chain of derivations, laid out
 * in the specification as a ladder.  The shape below follows it exactly, and
 * the order matters: each rung is computed from the one above it and from the
 * transcript at that precise moment, so deriving a secret one message too
 * early or too late produces keys that differ from the server's in a way
 * nothing detects until the first record fails to decrypt.
 */

static void derive_traffic_keys(const u8 secret[SHA256_SIZE],
                                aes_t *aes, u8 iv[12]) {
    u8 key[16];
    tls_hkdf_expand_label(secret, "key", NULL, 0, key, sizeof key);
    tls_hkdf_expand_label(secret, "iv", NULL, 0, iv, 12);
    aes_setkey(aes, key, 128);
}

/* The nonce for one record: the fixed part with the record's number counted
 * into its low end.  Reusing a nonce with the same key would give away the
 * plaintext, which is why the counter is never reset without also changing the
 * key. */
static void record_nonce(const u8 iv[12], u64 seq, u8 out[12]) {
    memcpy(out, iv, 12);
    for (int i = 0; i < 8; i++) out[11 - i] ^= (u8)(seq >> (8 * i));
}

static void transcript_hash(const tls_t *c, u8 out[SHA256_SIZE]) {
    sha256_t copy = c->transcript;         /* a snapshot; the real one goes on */
    sha256_final(&copy, out);
}

/* ------------------------------------------------------------ the records */

/* Send the whole thing.  A stream connection is allowed to accept part of what
 * it was given and ask to be called again, and a record that goes out half
 * written is not a record - the other side will read the first half as a
 * length and wait forever for the rest. */
static bool tcp_write_all(tls_t *c, const u8 *data, int len, int timeout_ms) {
    int sent = 0;
    while (sent < len) {
        int n = tcp_send_data(c->tcp, data + sent, len - sent, timeout_ms);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static bool tcp_read_exact(tls_t *c, u8 *out, int len, int timeout_ms) {
    int got = 0;
    while (got < len) {
        int n = tcp_receive_data(c->tcp, out + got, len - got, timeout_ms);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

static const char *alert_meaning(u8 description) {
    switch (description) {
    case 0:   return "the server closed the connection";
    case 10:  return "the server did not understand a message";
    case 20:  return "a record failed its integrity check";
    case 40:  return "the server could not agree on a way to talk";
    case 42:  return "the server rejected the certificate offered";
    case 43:  return "the server rejected an unsupported certificate";
    case 45:  return "the server says its certificate has expired";
    case 46:  return "the server says a certificate was revoked";
    case 47:  return "the server rejected a value as illegal";
    case 48:  return "the server does not trust the certificate authority";
    case 49:  return "the server refused access";
    case 50:  return "the server could not decode a message";
    case 51:  return "the server could not verify a signature";
    case 70:  return "the server does not support this version of the protocol";
    case 71:  return "the server considers the security level insufficient";
    case 80:  return "the server hit an internal error";
    case 109: return "the server has no certificate for the name that was asked for";
    case 112: return "the server does not recognise that host name";
    case 116: return "the server wanted a certificate from this machine";
    default:  return "the server ended the connection";
    }
}

/* Read one record, decrypting it if the keys are up.  On return `type` is the
 * real content type and the body is in c->record. */
static bool read_record(tls_t *c, u8 *type, int timeout_ms) {
    u8 header[5];
    if (!tcp_read_exact(c, header, 5, timeout_ms)) {
        if (!c->error) c->error = "the connection ended before a reply arrived";
        return false;
    }

    int len = (header[3] << 8) | header[4];
    if (len < 0 || len > MAX_RECORD) {
        c->error = "the server sent a record larger than the protocol allows";
        return false;
    }

    if (!tcp_read_exact(c, c->record, len, timeout_ms)) {
        c->error = "the connection ended in the middle of a record";
        return false;
    }
    c->record_len = len;

    if (!c->read_encrypted || header[0] == REC_CHANGE_CIPHER) {
        *type = header[0];
        return true;
    }

    /* An encrypted record: the tag is the last sixteen bytes, and the header
     * itself is authenticated - so a middlebox that rewrites the length breaks
     * the check rather than going unnoticed. */
    if (len < 17) {
        c->error = "the server sent an encrypted record with nothing in it";
        return false;
    }

    u8 nonce[12];
    record_nonce(c->read_iv, c->read_seq, nonce);

    int body = len - 16;
    if (!aes_gcm_decrypt(&c->read_aes, nonce, header, 5,
                         c->record, body, c->record + body, c->record)) {
        c->error = "a record from the server failed its integrity check";
        return false;
    }
    c->read_seq++;

    /* The real content type is the last non-zero byte: padding of any length
     * may follow it, which is what hides how long a message really was. */
    while (body > 0 && c->record[body - 1] == 0) body--;
    if (body == 0) {
        c->error = "the server sent a record with no content type";
        return false;
    }
    *type = c->record[body - 1];
    c->record_len = body - 1;
    return true;
}

static bool write_record(tls_t *c, u8 type, const void *data, int len) {
    static u8 out[MAX_RECORD + 8];

    if (!c->write_encrypted) {
        out[0] = type;
        out[1] = 0x03;
        out[2] = c->sent_first_record ? 0x03 : 0x01;
        c->sent_first_record = true;
        out[3] = (u8)(len >> 8); out[4] = (u8)len;
        memcpy(out + 5, data, (size_t)len);
        return tcp_write_all(c, out, len + 5, 5000);
    }

    /* Encrypted records all claim to be application data on the outside,
     * whatever they really are.  That is what keeps the handshake's shape from
     * being visible. */
    int inner = len + 1;
    int total = inner + 16;
    if (total > MAX_RECORD) return false;

    u8 header[5] = { REC_APPLICATION, 0x03, 0x03, (u8)(total >> 8), (u8)total };

    static u8 body[MAX_RECORD];
    memcpy(body, data, (size_t)len);
    body[len] = type;

    u8 nonce[12];
    record_nonce(c->write_iv, c->write_seq, nonce);

    memcpy(out, header, 5);
    aes_gcm_encrypt(&c->write_aes, nonce, header, 5, body, (size_t)inner,
                    out + 5, out + 5 + inner);
    c->write_seq++;

    return tcp_write_all(c, out, total + 5, 5000);
}

static void send_alert(tls_t *c, u8 level, u8 description) {
    u8 alert[2] = { level, description };
    write_record(c, REC_ALERT, alert, 2);
}

/* ------------------------------------------------- one handshake message
 *
 * Handshake messages have their own four byte header and are not aligned to
 * records: one record can carry several, and one message can span several
 * records.  This hands back exactly one, reassembling as needed.
 */
static bool next_handshake(tls_t *c, u8 *type, const u8 **body, int *len,
                           int timeout_ms) {
    for (;;) {
        /* Is a whole message already buffered? */
        int available = c->handshake_len - c->handshake_at;
        if (available >= 4) {
            const u8 *p = c->handshake + c->handshake_at;
            int size = (p[1] << 16) | (p[2] << 8) | p[3];
            if (available >= 4 + size) {
                /* The hash covers the message with its header, exactly as it
                 * appeared. */
                sha256_update(&c->transcript, p, (size_t)(4 + size));
                *type = p[0];
                *body = p + 4;
                *len = size;
                c->handshake_at += 4 + size;
                return true;
            }
            if (size > MAX_HANDSHAKE - 4) {
                c->error = "the server sent a handshake message far too large";
                return false;
            }
        }

        /* Make room by dropping what has already been consumed. */
        if (c->handshake_at > 0) {
            memmove(c->handshake, c->handshake + c->handshake_at,
                    (size_t)(c->handshake_len - c->handshake_at));
            c->handshake_len -= c->handshake_at;
            c->handshake_at = 0;
        }

        u8 type_in;
        if (!read_record(c, &type_in, timeout_ms)) return false;

        if (type_in == REC_CHANGE_CIPHER) continue;   /* a relic; ignored */

        if (type_in == REC_ALERT) {
            if (c->record_len >= 2) c->error = alert_meaning(c->record[1]);
            else c->error = "the server ended the connection without saying why";
            return false;
        }

        if (type_in != REC_HANDSHAKE) {
            c->error = "the server sent data before the handshake finished";
            return false;
        }

        if (c->handshake_len + c->record_len > MAX_HANDSHAKE) {
            c->error = "the server's handshake is larger than this system will hold";
            return false;
        }
        memcpy(c->handshake + c->handshake_len, c->record, (size_t)c->record_len);
        c->handshake_len += c->record_len;
    }
}

/* ------------------------------------------------------------ ClientHello */

static int build_client_hello(tls_t *c, u8 *out, size_t cap,
                              const u8 client_random[32],
                              const u8 session_id[32],
                              const u8 public_key[32]) {
    writer_t w;
    w_init(&w, out, cap);

    w_u8(&w, HS_CLIENT_HELLO);
    size_t message = w_open24(&w);

    /* The version field says 1.2 and always will: too much equipment in the
     * middle of the internet drops anything else.  The real version is in an
     * extension, which is the compromise that let 1.3 be deployed at all. */
    w_u16(&w, 0x0303);
    w_bytes(&w, client_random, 32);

    /* A session identifier that means nothing, for the same reason: a hello
     * without one looks wrong to equipment that expects the old shape. */
    w_u8(&w, 32);
    w_bytes(&w, session_id, 32);

    /* One cipher suite.  Offering more would mean implementing more, and each
     * one is another set of code paths that has to be right. */
    w_u16(&w, 2);
    w_u16(&w, SUITE_AES128_GCM);

    /* Compression: none.  Compressing before encrypting leaks the plaintext
     * through the length, which is how several attacks worked. */
    w_u8(&w, 1);
    w_u8(&w, 0);

    size_t extensions = w_open16(&w);

    /* Which site is being asked for.  One address can serve thousands, and
     * without this the server cannot know which certificate to present. */
    if (c->host[0]) {
        size_t host_len = strlen(c->host);
        w_u16(&w, EXT_SERVER_NAME);
        size_t ext = w_open16(&w);
        size_t list = w_open16(&w);
        w_u8(&w, 0);                       /* a host name, as opposed to what */
        w_u16(&w, (u16)host_len);
        w_bytes(&w, c->host, host_len);
        w_close16(&w, list);
        w_close16(&w, ext);
    }

    w_u16(&w, EXT_SUPPORTED_GROUPS);
    {
        size_t ext = w_open16(&w);
        size_t list = w_open16(&w);
        w_u16(&w, GROUP_X25519);
        w_close16(&w, list);
        w_close16(&w, ext);
    }

    /* Which signatures this machine can check.  Saying only what is true here
     * is what turns "the connection broke somewhere in the middle" into "the
     * server has nothing this client can verify", decided by the server before
     * it commits to anything. */
    w_u16(&w, EXT_SIGNATURE_ALGS);
    {
        size_t ext = w_open16(&w);
        size_t list = w_open16(&w);
        w_u16(&w, SIG_ECDSA_SHA256);
        w_u16(&w, SIG_ECDSA_SHA384);
        w_u16(&w, SIG_RSA_PSS_SHA256);
        w_u16(&w, SIG_RSA_PSS_SHA384);
        w_u16(&w, SIG_RSA_PKCS1_SHA256);
        w_u16(&w, SIG_RSA_PKCS1_SHA384);
        w_u16(&w, SIG_RSA_PKCS1_SHA512);
        w_close16(&w, list);
        w_close16(&w, ext);
    }

    w_u16(&w, EXT_SUPPORTED_VERSIONS);
    {
        size_t ext = w_open16(&w);
        size_t list = w_open8(&w);
        w_u16(&w, 0x0304);                 /* 1.3, and nothing else */
        w_close8(&w, list);
        w_close16(&w, ext);
    }

    /* Which protocol will run inside.  Asking for HTTP/1.1 explicitly stops a
     * server from starting to speak HTTP/2, which is a different protocol
     * entirely and would arrive as unintelligible binary. */
    w_u16(&w, EXT_ALPN);
    {
        size_t ext = w_open16(&w);
        size_t list = w_open16(&w);
        w_u8(&w, 8);
        w_bytes(&w, "http/1.1", 8);
        w_close16(&w, list);
        w_close16(&w, ext);
    }

    /* The public half of a key this client just invented.  Sending it with the
     * first message is what makes the handshake one round trip instead of two. */
    w_u16(&w, EXT_KEY_SHARE);
    {
        size_t ext = w_open16(&w);
        size_t list = w_open16(&w);
        w_u16(&w, GROUP_X25519);
        w_u16(&w, 32);
        w_bytes(&w, public_key, 32);
        w_close16(&w, list);
        w_close16(&w, ext);
    }

    w_close16(&w, extensions);
    w_close24(&w, message);

    return w.overflow ? -1 : (int)w.at;
}

/* ----------------------------------------------------------- ServerHello */

static bool parse_server_hello(tls_t *c, const u8 *body, int len,
                               u8 server_public[32], bool *retry) {
    reader_t r;
    r_init(&r, body, (size_t)len);

    *retry = false;

    u16 version = r_u16(&r);
    (void)version;                         /* the real one is in an extension */

    const u8 *server_random = r_take(&r, 32);
    if (!server_random) { c->error = "the server's reply was truncated"; return false; }

    if (!memcmp(server_random, hello_retry_random, 32)) {
        *retry = true;
        return false;
    }

    u8 session_len = r_u8(&r);
    r_take(&r, session_len);

    u16 suite = r_u16(&r);
    if (suite != SUITE_AES128_GCM) {
        c->error = "the server chose a cipher this client did not offer";
        return false;
    }

    r_u8(&r);                              /* compression, must be none */

    u16 extensions_len = r_u16(&r);
    if (r.bad) { c->error = "the server's reply was truncated"; return false; }

    bool have_key = false;
    bool have_version = false;
    size_t end = r.at + extensions_len;
    if (end > r.len) { c->error = "the server's reply was truncated"; return false; }

    while (r.at + 4 <= end) {
        u16 kind = r_u16(&r);
        u16 size = r_u16(&r);
        const u8 *value = r_take(&r, size);
        if (!value) break;

        if (kind == EXT_SUPPORTED_VERSIONS) {
            if (size != 2 || value[0] != 0x03 || value[1] != 0x04) {
                c->error = "the server does not speak TLS 1.3";
                return false;
            }
            have_version = true;
        } else if (kind == EXT_KEY_SHARE) {
            if (size < 4) break;
            u16 group = (u16)((value[0] << 8) | value[1]);
            u16 key_len = (u16)((value[2] << 8) | value[3]);
            if (group != GROUP_X25519 || key_len != 32 || size < 4 + 32) {
                c->error = "the server chose a key exchange this client did not offer";
                return false;
            }
            memcpy(server_public, value + 4, 32);
            have_key = true;
        }
    }

    if (!have_version) {
        /* Without the extension the server is offering an older version, and
         * this client has none. */
        c->error = "the server only offers an older version of the protocol";
        return false;
    }
    if (!have_key) {
        c->error = "the server did not send its half of the key exchange";
        return false;
    }
    return true;
}

/* ------------------------------------------------------------ Certificate */

static bool parse_certificate(tls_t *c, const u8 *body, int len,
                              x509_cert_t *chain, int *chain_len,
                              const u8 **leaf_der, size_t *leaf_len) {
    reader_t r;
    r_init(&r, body, (size_t)len);

    u8 context_len = r_u8(&r);
    r_take(&r, context_len);               /* empty, unless this is a response */

    u32 list_len = r_u24(&r);
    if (r.bad) { c->error = "the certificate message was truncated"; return false; }

    size_t end = r.at + list_len;
    if (end > r.len) { c->error = "the certificate message was truncated"; return false; }

    *chain_len = 0;
    while (r.at + 3 <= end && *chain_len < MAX_CHAIN) {
        u32 size = r_u24(&r);
        const u8 *der = r_take(&r, size);
        if (!der) break;

        /* Into somewhere that will not move.  Everything the parsed form
         * knows - the body a signature covers, the encoded names a chain is
         * matched on - is a pointer into these bytes. */
        if (c->certificates_len + (int)size > MAX_CERTIFICATES) {
            c->error = "the server's certificate chain is larger than this "
                       "system will hold";
            return false;
        }
        u8 *stable = c->certificates + c->certificates_len;
        memcpy(stable, der, size);
        c->certificates_len += (int)size;

        if (!x509_parse(stable, size, &chain[*chain_len])) {
            c->error = "a certificate the server sent could not be read";
            return false;
        }
        if (*chain_len == 0) { *leaf_der = stable; *leaf_len = size; }
        (*chain_len)++;

        /* Each certificate carries its own extensions here, which nothing in
         * this client acts on. */
        u16 extensions = r_u16(&r);
        r_take(&r, extensions);
    }

    if (*chain_len == 0) {
        c->error = "the server sent no certificate";
        return false;
    }
    return true;
}

/* ------------------------------------------------------ CertificateVerify
 *
 * The certificate says who the server claims to be.  This is where it proves
 * it: a signature, made with the certificate's private key, over the whole
 * conversation so far.  Without this step anybody could replay a certificate
 * they copied from the real site.
 *
 * What gets signed is not the transcript directly but a block built around it,
 * beginning with sixty-four spaces.  That prefix exists so that a signature
 * made in one context can never be mistaken for one made in another - an old
 * attack that this padding closes off.
 */
static bool verify_certificate_signature(tls_t *c, const u8 *body, int len,
                                         const x509_cert_t *leaf,
                                         const u8 transcript[SHA256_SIZE]) {
    reader_t r;
    r_init(&r, body, (size_t)len);

    u16 algorithm = r_u16(&r);
    u16 signature_len = r_u16(&r);
    const u8 *signature = r_take(&r, signature_len);
    if (!signature) {
        c->error = "the server's proof of identity was truncated";
        return false;
    }

    if (!leaf->key_usable) {
        c->error = "the server's certificate uses a key type this system cannot check";
        return false;
    }

    static u8 block[64 + 34 + SHA256_SIZE];
    size_t at = 0;
    for (int i = 0; i < 64; i++) block[at++] = 0x20;
    memcpy(block + at, "TLS 1.3, server CertificateVerify", 33);
    at += 33;
    block[at++] = 0x00;
    memcpy(block + at, transcript, SHA256_SIZE);
    at += SHA256_SIZE;

    if (leaf->key_type == X509_KEY_RSA && signature_len != leaf->rsa_modulus_len) {
        c->error = "the server's proof of identity is the wrong size for its key";
        return false;
    }

    bool ok = false;
    switch (algorithm) {
    case SIG_ECDSA_SHA256:
    case SIG_ECDSA_SHA384: {
        if (leaf->key_type != X509_KEY_EC || leaf->ec_curve == 0) {
            c->error = "the server signed on a curve its certificate does not use";
            return false;
        }

        u8 digest[SHA384_SIZE];
        size_t digest_len;
        if (algorithm == SIG_ECDSA_SHA256) {
            sha256(block, at, digest);
            digest_len = SHA256_SIZE;
        } else {
            sha384(block, at, digest);
            digest_len = SHA384_SIZE;
        }

        const u8 *r = NULL, *sig_s = NULL;
        size_t r_len = 0, s_len = 0;
        if (!ecc_split_signature(signature, signature_len, &r, &r_len,
                                 &sig_s, &s_len)) {
            c->error = "the server's proof of identity is not a well-formed pair";
            return false;
        }
        ok = ecc_verify(leaf->ec_curve, leaf->ec_point, leaf->ec_point_len,
                        digest, digest_len, r, r_len, sig_s, s_len);
        break;
    }
    case SIG_RSA_PSS_SHA256: {
        if (leaf->key_type != X509_KEY_RSA) {
            c->error = "the server signed with RSA but its certificate holds "
                       "a different kind of key";
            return false;
        }
        u8 digest[SHA256_SIZE];
        sha256(block, at, digest);
        ok = rsa_pss_verify(signature, signature_len, leaf->rsa_modulus,
                            leaf->rsa_modulus_len, leaf->rsa_exponent,
                            digest, SHA256_SIZE);
        break;
    }
    case SIG_RSA_PSS_SHA384: {
        if (leaf->key_type != X509_KEY_RSA) {
            c->error = "the server signed with RSA but its certificate holds "
                       "a different kind of key";
            return false;
        }
        u8 digest[SHA384_SIZE];
        sha384(block, at, digest);
        ok = rsa_pss_verify(signature, signature_len, leaf->rsa_modulus,
                            leaf->rsa_modulus_len, leaf->rsa_exponent,
                            digest, SHA384_SIZE);
        break;
    }
    default:
        /* The client said which signatures it could check.  A server that
         * picks another one has not followed the protocol, and guessing at it
         * would be worse than saying so. */
        c->error = "the server signed with a method this client did not offer";
        return false;
    }

    if (!ok) c->error = "the server could not prove it owns its certificate";
    return ok;
}

/* --------------------------------------------------------------- Finished
 *
 * The last message each side sends during the handshake is a keyed hash of
 * everything that came before it.  Both sides have the same transcript and the
 * same key, so the values match - unless something in the earlier messages was
 * altered while it was still readable, which is exactly what this catches.
 */
static void finished_value(const u8 secret[SHA256_SIZE],
                           const u8 transcript[SHA256_SIZE],
                           u8 out[SHA256_SIZE]) {
    u8 key[SHA256_SIZE];
    tls_hkdf_expand_label(secret, "finished", NULL, 0, key, sizeof key);
    hmac_sha256(key, sizeof key, transcript, SHA256_SIZE, out);
}

/* ---------------------------------------------------------- the handshake */

static bool do_handshake(tls_t *c, int timeout_ms) {
    /* --- the client's opening message --- */

    u8 client_random[32], session_id[32], private_key[32], public_key[32];
    random_bytes(client_random, sizeof client_random);
    random_bytes(session_id, sizeof session_id);
    random_bytes(private_key, sizeof private_key);
    x25519_public(public_key, private_key);

    sha256_init(&c->transcript);

    static u8 hello[2048];
    int hello_len = build_client_hello(c, hello, sizeof hello,
                                       client_random, session_id, public_key);
    if (hello_len < 0) {
        c->error = "the host name is too long for an opening message";
        return false;
    }

    sha256_update(&c->transcript, hello, (size_t)hello_len);
    if (!write_record(c, REC_HANDSHAKE, hello, hello_len)) {
        c->error = "the opening message could not be sent";
        return false;
    }

    /* A record that exists only so that equipment in the middle sees the shape
     * it expects from an older connection.  It carries no meaning and both
     * sides ignore it. */
    { u8 one = 1; write_record(c, REC_CHANGE_CIPHER, &one, 1); }

    /* --- the server's reply --- */

    u8 type;
    const u8 *body;
    int len;
    if (!next_handshake(c, &type, &body, &len, timeout_ms)) return false;
    if (type != HS_SERVER_HELLO) {
        c->error = "the server replied with something other than a hello";
        return false;
    }

    u8 server_public[32];
    bool retry = false;
    if (!parse_server_hello(c, body, len, server_public, &retry)) {
        if (retry)
            c->error = "the server asked to start again with different settings, "
                       "which this client cannot do";
        return false;
    }

    /* --- the shared secret, and the keys that protect the rest --- */

    u8 shared[32];
    x25519(shared, private_key, server_public);

    /* A result of all zeros means the server sent a point with no shared
     * secret in it, deliberately or otherwise.  Continuing would produce keys
     * an attacker also knows. */
    {
        u8 combined = 0;
        for (int i = 0; i < 32; i++) combined |= shared[i];
        if (!combined) {
            c->error = "the server's key exchange value is not usable";
            return false;
        }
    }

    u8 zeros[SHA256_SIZE];
    memset(zeros, 0, sizeof zeros);

    u8 early_secret[SHA256_SIZE];
    hkdf_extract(NULL, 0, zeros, sizeof zeros, early_secret);

    u8 derived[SHA256_SIZE];
    tls_derive_secret(early_secret, "derived", NULL, derived);

    u8 handshake_secret[SHA256_SIZE];
    hkdf_extract(derived, sizeof derived, shared, sizeof shared, handshake_secret);

    u8 hello_hash[SHA256_SIZE];
    transcript_hash(c, hello_hash);

    u8 client_hs[SHA256_SIZE], server_hs[SHA256_SIZE];
    tls_derive_secret(handshake_secret, "c hs traffic", hello_hash, client_hs);
    tls_derive_secret(handshake_secret, "s hs traffic", hello_hash, server_hs);

    derive_traffic_keys(server_hs, &c->read_aes, c->read_iv);
    c->read_seq = 0;
    c->read_encrypted = true;
    memcpy(c->read_secret, server_hs, SHA256_SIZE);

    /* --- everything the server says from here is encrypted --- */

    static x509_cert_t chain[MAX_CHAIN];
    int chain_len = 0;
    const u8 *leaf_der = NULL;
    size_t leaf_len = 0;
    bool have_certificate = false;
    u8 transcript_before_verify[SHA256_SIZE];
    u8 transcript_before_finished[SHA256_SIZE];

    for (;;) {
        if (!next_handshake(c, &type, &body, &len, timeout_ms)) return false;

        if (type == HS_ENCRYPTED_EXT) {
            /* Almost nothing in here matters to this client, with one
             * exception: if the server picked a protocol to run inside the
             * connection that is not the one that was asked for, then what
             * gets sent next is not what it expects. */
            reader_t ext;
            r_init(&ext, body, (size_t)len);
            u16 total = r_u16(&ext);
            size_t stop = ext.at + total;
            if (stop > ext.len) stop = ext.len;
            while (ext.at + 4 <= stop) {
                u16 kind = r_u16(&ext);
                u16 size = r_u16(&ext);
                const u8 *value = r_take(&ext, size);
                if (!value) break;
                if (kind == EXT_ALPN && size >= 3) {
                    int name_len = value[2];
                    if (name_len > 0 && 3 + name_len <= size) {
                        char chosen[24];
                        int take = name_len < (int)sizeof chosen - 1
                                 ? name_len : (int)sizeof chosen - 1;
                        memcpy(chosen, value + 3, (size_t)take);
                        chosen[take] = 0;
                        if (strcmp(chosen, "http/1.1"))
                            kwarn("tls", "%s: the server chose \"%s\" to run "
                                         "inside the connection, not http/1.1",
                                  c->host, chosen);
                    }
                }
            }
            continue;
        }
        if (type == HS_CERT_REQUEST) {
            c->error = "the server wants a certificate from this machine, which "
                       "this system does not have";
            return false;
        }
        if (type == HS_CERTIFICATE) {
            /* The hash the signature covers is the one up to and including
             * this message, so it has to be taken now rather than later. */
            if (!parse_certificate(c, body, len, chain, &chain_len,
                                   &leaf_der, &leaf_len))
                return false;
            transcript_hash(c, transcript_before_verify);
            have_certificate = true;
            continue;
        }
        if (type == HS_CERT_VERIFY) {
            if (!have_certificate) {
                c->error = "the server offered proof of a certificate it never sent";
                return false;
            }
            if (!verify_certificate_signature(c, body, len, &chain[0],
                                              transcript_before_verify))
                return false;
            transcript_hash(c, transcript_before_finished);
            continue;
        }
        if (type == HS_FINISHED) {
            if (!have_certificate) {
                c->error = "the server finished the handshake without identifying itself";
                return false;
            }

            u8 expected[SHA256_SIZE];
            finished_value(server_hs, transcript_before_finished, expected);
            if (len != SHA256_SIZE || memcmp(body, expected, SHA256_SIZE)) {
                c->error = "the server's summary of the handshake does not match, "
                           "which means something altered it in transit";
                return false;
            }
            break;
        }

        c->error = "the server sent an unexpected message during the handshake";
        return false;
    }

    /* --- is the certificate any good --- */

    const char *problem = roots_check_chain(chain, chain_len, c->host);
    if (problem) {
        c->error = problem;
        send_alert(c, 2, 48);              /* fatal: unknown certificate authority */
        return false;
    }

    /* Worth keeping, so a caller can show who answered. */
    strlcpy(c->peer.subject, chain[0].subject_common_name, sizeof c->peer.subject);
    strlcpy(c->peer.organisation, chain[0].subject_organisation,
            sizeof c->peer.organisation);
    strlcpy(c->peer.issuer, chain[0].issuer_common_name, sizeof c->peer.issuer);
    c->peer.valid_until_year = chain[0].not_after.year;
    c->peer.valid_until_month = chain[0].not_after.month;
    c->peer.valid_until_day = chain[0].not_after.day;
    c->peer.chain_length = chain_len;
    strlcpy(c->peer.cipher, "TLS 1.3 AES-128-GCM x25519", sizeof c->peer.cipher);

    /* --- the client's own Finished --- */

    u8 after_server_finished[SHA256_SIZE];
    transcript_hash(c, after_server_finished);

    u8 client_verify[SHA256_SIZE];
    finished_value(client_hs, after_server_finished, client_verify);

    u8 finished_message[4 + SHA256_SIZE];
    finished_message[0] = HS_FINISHED;
    finished_message[1] = 0; finished_message[2] = 0;
    finished_message[3] = SHA256_SIZE;
    memcpy(finished_message + 4, client_verify, SHA256_SIZE);

    /* This one goes out under the handshake keys, and the application keys are
     * only installed afterwards - the order the specification requires. */
    derive_traffic_keys(client_hs, &c->write_aes, c->write_iv);
    c->write_seq = 0;
    c->write_encrypted = true;

    if (!write_record(c, REC_HANDSHAKE, finished_message, sizeof finished_message)) {
        /* Naming the state the connection is in turns this from "something
         * went wrong" into something that can be acted on: a connection the
         * other end reset is a different problem from one that timed out. */
        static char why[96];
        snprintf(why, sizeof why,
                 "the closing handshake message could not be sent (%s)",
                 tcp_state_name(c->tcp));
        c->error = why;
        return false;
    }

    /* --- the keys the rest of the connection uses --- */

    u8 master_derived[SHA256_SIZE], master_secret[SHA256_SIZE];
    tls_derive_secret(handshake_secret, "derived", NULL, master_derived);
    hkdf_extract(master_derived, sizeof master_derived, zeros, sizeof zeros,
                 master_secret);

    u8 client_app[SHA256_SIZE], server_app[SHA256_SIZE];
    tls_derive_secret(master_secret, "c ap traffic", after_server_finished, client_app);
    tls_derive_secret(master_secret, "s ap traffic", after_server_finished, server_app);

    derive_traffic_keys(client_app, &c->write_aes, c->write_iv);
    derive_traffic_keys(server_app, &c->read_aes, c->read_iv);
    c->write_seq = 0;
    c->read_seq = 0;
    memcpy(c->write_secret, client_app, SHA256_SIZE);
    memcpy(c->read_secret, server_app, SHA256_SIZE);

    c->established = true;
    return true;
}

/* --------------------------------------------------------- the public face */

static int allocate_slot(void) {
    for (int i = 0; i < TLS_CONNECTIONS; i++)
        if (!connections[i] || !connections[i]->in_use) return i;
    return -1;
}

int tls_connect(ipv4_t ip, u16 port, const char *host, int timeout_ms,
                const char **why) {
    if (why) *why = NULL;

    int slot = allocate_slot();
    if (slot < 0) {
        if (why) *why = "there are already as many secure connections as this "
                        "system will hold";
        return -1;
    }

    if (!connections[slot]) {
        connections[slot] = kmalloc(sizeof(tls_t));
        if (!connections[slot]) {
            if (why) *why = "there is not enough memory for another connection";
            return -1;
        }
    }

    tls_t *c = connections[slot];
    memset(c, 0, sizeof *c);
    c->in_use = true;
    if (host) strlcpy(c->host, host, sizeof c->host);

    c->tcp = tcp_connect(ip, port, timeout_ms);
    if (c->tcp < 0) {
        c->in_use = false;
        if (why) *why = "the server did not answer";
        return -1;
    }

    if (!do_handshake(c, timeout_ms)) {
        if (why) *why = c->error ? c->error : "the secure handshake failed";
        tcp_close(c->tcp);
        c->in_use = false;
        return -1;
    }

    kinfo("tls", "%s: %s, certified to %s by %s",
          c->host, c->peer.cipher,
          c->peer.subject[0] ? c->peer.subject : "(no name)",
          c->peer.issuer[0] ? c->peer.issuer : "(no issuer)");
    return slot;
}

static tls_t *lookup(int handle) {
    if (handle < 0 || handle >= TLS_CONNECTIONS) return NULL;
    tls_t *c = connections[handle];
    return (c && c->in_use) ? c : NULL;
}

int tls_send_data(int handle, const void *data, int len, int timeout_ms) {
    (void)timeout_ms;
    tls_t *c = lookup(handle);
    if (!c || !c->established || c->closed) return -1;

    const u8 *bytes = data;
    int sent = 0;
    while (sent < len) {
        int chunk = len - sent;
        if (chunk > MAX_PLAINTEXT) chunk = MAX_PLAINTEXT;
        if (!write_record(c, REC_APPLICATION, bytes + sent, chunk)) {
            kwarn("tls", "%s: could not send %d bytes (%s)", c->host, chunk,
                  tcp_state_name(c->tcp));
            return -1;
        }
        sent += chunk;
    }
    return sent;
}

/* A key update: the server has moved to the next key in its chain, and this
 * side has to move with it or nothing further will decrypt. */
static void advance_read_key(tls_t *c) {
    u8 next[SHA256_SIZE];
    tls_hkdf_expand_label(c->read_secret, "traffic upd", NULL, 0, next, sizeof next);
    memcpy(c->read_secret, next, SHA256_SIZE);
    derive_traffic_keys(c->read_secret, &c->read_aes, c->read_iv);
    c->read_seq = 0;
}

int tls_receive_data(int handle, void *buf, int len, int timeout_ms) {
    tls_t *c = lookup(handle);
    if (!c || !c->established) {
        kwarn("tls", "a read was asked for on handle %d, which is not an open "
                     "secure connection", handle);
        return -1;
    }

    /* Anything left over from the last record first. */
    if (c->plain_at < c->plain_len) {
        int available = c->plain_len - c->plain_at;
        int take = len < available ? len : available;
        memcpy(buf, c->plain + c->plain_at, (size_t)take);
        c->plain_at += take;
        return take;
    }

    if (c->closed) return 0;

    for (;;) {
        u8 type;
        if (!read_record(c, &type, timeout_ms)) {
            if (c->received_any) {
                /* The far end simply stopped, having already said everything
                 * it had to say.  Not a fault. */
                c->closed = true;
                return 0;
            }
            /* The reason a connection stopped producing data is the useful
             * part, and it is lost entirely if only a number comes back. */
            kwarn("tls", "%s: %s (%s)", c->host,
                  c->error ? c->error : "the connection stopped",
                  tcp_state_name(c->tcp));
            return -1;
        }

        if (type == REC_APPLICATION) {
            memcpy(c->plain, c->record, (size_t)c->record_len);
            c->plain_len = c->record_len;
            c->plain_at = 0;
            if (c->plain_len == 0) continue;   /* padding only */
            int take = len < c->plain_len ? len : c->plain_len;
            memcpy(buf, c->plain, (size_t)take);
            c->plain_at = take;
            c->received_any = true;
            return take;
        }

        if (type == REC_ALERT) {
            if (c->record_len >= 2 && c->record[1] == 0) {
                c->closed = true;           /* an orderly goodbye */
                return 0;
            }
            c->error = c->record_len >= 2 ? alert_meaning(c->record[1])
                                          : "the server ended the connection";
            kwarn("tls", "%s: %s (alert %u)", c->host, c->error,
                  c->record_len >= 2 ? c->record[1] : 0);
            c->closed = true;
            return -1;
        }

        if (type == REC_HANDSHAKE) {
            /* After the handshake a server may still send two things: session
             * tickets, which this client cannot use and discards, and a key
             * update, which it must act on. */
            reader_t r;
            r_init(&r, c->record, (size_t)c->record_len);
            while (r_left(&r) >= 4) {
                u8 kind = r_u8(&r);
                u32 size = r_u24(&r);
                if (!r_take(&r, size)) break;
                if (kind == HS_KEY_UPDATE) advance_read_key(c);
            }
            continue;
        }

        if (type == REC_CHANGE_CIPHER) continue;

        c->error = "the server sent something unexpected";
        return -1;
    }
}

void tls_close(int handle) {
    tls_t *c = lookup(handle);
    if (!c) return;

    /* Saying goodbye rather than dropping the connection lets the other side
     * tell an orderly end from a truncated one - which matters, because a
     * truncated response should not be treated as a complete one. */
    if (c->established && !c->closed) send_alert(c, 1, 0);

    tcp_close(c->tcp);
    c->in_use = false;
    c->established = false;
}

bool tls_is_open(int handle) {
    tls_t *c = lookup(handle);
    return c && c->established && !c->closed;
}

bool tls_peer_info(int handle, tls_peer_t *out) {
    tls_t *c = lookup(handle);
    if (!c || !c->established) return false;
    *out = c->peer;
    return true;
}

/* ------------------------------------------------------------------- test
 *
 * The handshake needs a server, so it cannot be tested here.  What can be
 * tested is the key schedule, and there is a published trace of a complete
 * TLS 1.3 handshake with every intermediate value written out, which is
 * exactly what is needed: it checks the whole ladder from the shared secret to
 * the traffic keys against numbers computed by someone else.
 */
int tls_selftest(void) {
    int failures = 0;

    /* From RFC 8448, "Example Handshake Traces for TLS 1.3", the simple
     * one round trip handshake.  Every value below is printed in that
     * document. */
    static const u8 shared[32] = {
        0x8b,0xd4,0x05,0x4f,0xb5,0x5b,0x9d,0x63,0xfd,0xfb,0xac,0xf9,0xf0,0x4b,0x9f,0x0d,
        0x35,0xe6,0xd6,0x3f,0x53,0x75,0x63,0xef,0xd4,0x62,0x72,0x90,0x0f,0x89,0x49,0x2d,
    };
    /* The transcript hash after the two hello messages. */
    static const u8 hello_hash[32] = {
        0x86,0x0c,0x06,0xed,0xc0,0x78,0x58,0xee,0x8e,0x78,0xf0,0xe7,0x42,0x8c,0x58,0xed,
        0xd6,0xb4,0x3f,0x2c,0xa3,0xe6,0xe9,0x5f,0x02,0xed,0x06,0x3c,0xf0,0xe1,0xca,0xd8,
    };
    static const u8 want_client_hs[32] = {
        0xb3,0xed,0xdb,0x12,0x6e,0x06,0x7f,0x35,0xa7,0x80,0xb3,0xab,0xf4,0x5e,0x2d,0x8f,
        0x3b,0x1a,0x95,0x07,0x38,0xf5,0x2e,0x96,0x00,0x74,0x6a,0x0e,0x27,0xa5,0x5a,0x21,
    };
    static const u8 want_server_hs[32] = {
        0xb6,0x7b,0x7d,0x69,0x0c,0xc1,0x6c,0x4e,0x75,0xe5,0x42,0x13,0xcb,0x2d,0x37,0xb4,
        0xe9,0xc9,0x12,0xbc,0xde,0xd9,0x10,0x5d,0x42,0xbe,0xfd,0x59,0xd3,0x91,0xad,0x38,
    };
    /* And the key and nonce derived from the server's handshake secret. */
    static const u8 want_server_key[16] = {
        0x3f,0xce,0x51,0x60,0x09,0xc2,0x17,0x27,0xd0,0xf2,0xe4,0xe8,0x6e,0xe4,0x03,0xbc,
    };
    static const u8 want_server_iv[12] = {
        0x5d,0x31,0x3e,0xb2,0x67,0x12,0x76,0xee,0x13,0x00,0x0b,0x30,
    };

    u8 zeros[SHA256_SIZE];
    memset(zeros, 0, sizeof zeros);

    u8 early[SHA256_SIZE];
    hkdf_extract(NULL, 0, zeros, sizeof zeros, early);

    u8 derived[SHA256_SIZE];
    tls_derive_secret(early, "derived", NULL, derived);

    u8 handshake[SHA256_SIZE];
    hkdf_extract(derived, sizeof derived, shared, sizeof shared, handshake);

    u8 client_hs[SHA256_SIZE], server_hs[SHA256_SIZE];
    tls_derive_secret(handshake, "c hs traffic", hello_hash, client_hs);
    tls_derive_secret(handshake, "s hs traffic", hello_hash, server_hs);

    if (memcmp(client_hs, want_client_hs, sizeof want_client_hs)) {
        kerr("tls", "the client's handshake secret does not match the published trace");
        failures++;
    }
    if (memcmp(server_hs, want_server_hs, sizeof want_server_hs)) {
        kerr("tls", "the server's handshake secret does not match the published trace");
        failures++;
    }

    u8 key[16], iv[12];
    tls_hkdf_expand_label(server_hs, "key", NULL, 0, key, sizeof key);
    tls_hkdf_expand_label(server_hs, "iv", NULL, 0, iv, sizeof iv);
    if (memcmp(key, want_server_key, sizeof want_server_key)) {
        kerr("tls", "the record key does not match the published trace");
        failures++;
    }
    if (memcmp(iv, want_server_iv, sizeof want_server_iv)) {
        kerr("tls", "the record nonce does not match the published trace");
        failures++;
    }

    /* The nonce for a given record number: the base with the counter counted
     * into its low end.  Record zero must leave it unchanged. */
    {
        u8 nonce[12];
        record_nonce(iv, 0, nonce);
        if (memcmp(nonce, iv, sizeof iv)) {
            kerr("tls", "the first record's nonce differs from the base");
            failures++;
        }
        record_nonce(iv, 1, nonce);
        if (nonce[11] != (iv[11] ^ 1) || memcmp(nonce, iv, 11)) {
            kerr("tls", "the second record's nonce is wrong");
            failures++;
        }
    }

    if (!failures)
        kinfo("crypto", "the TLS 1.3 key schedule matches the trace RFC 8448 "
                        "publishes, down to the record keys");
    return failures;
}
